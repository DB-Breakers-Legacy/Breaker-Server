using System.Diagnostics;
using System.Text;

namespace BreakerServer.Infrastructure;

public sealed class RequestTraceMiddleware(
    RequestDelegate next,
    ILogger<RequestTraceMiddleware> logger)
{
    private const int PreviewLimit = 256;
    private const int CaptureLimit = 16 * 1024 * 1024;

    // Opt-in research aid: when BREAKER_CAPTURE_DIR is set, every request body is also saved whole to
    // <dir>/<time>_<path>.bin, so client payloads (avatar data, tutorial steps...) can be studied offline.
    private static readonly string? CaptureDirectory = Environment.GetEnvironmentVariable("BREAKER_CAPTURE_DIR");
    private static int _captureCounter;

    public async Task InvokeAsync(HttpContext context)
    {
        context.Request.EnableBuffering();

        var body = await ReadPreviewAsync(context.Request, context.RequestAborted);
        if (logger.IsEnabled(LogLevel.Debug))
        {
            logger.LogDebug(
                "HTTP request {Method} {Scheme}://{Host}{Path}{Query} type={ContentType} bytes={Length} preview={Preview}",
                context.Request.Method,
                context.Request.Scheme,
                context.Request.Host.Value,
                context.Request.Path.Value,
                context.Request.QueryString.Value,
                context.Request.ContentType ?? "none",
                context.Request.ContentLength ?? body.LongLength,
                FormatPreview(body));
        }

        // With capture on, the reply is saved too (2026-11-13: "log everything from the beginning"): the response
        // body goes to a buffer, is copied on to the game unchanged, and is written next to the request as
        // <time>_<number>_<path>.reply.bin (same number as the request file).
        var captureId = 0;
        Stream? originalBody = null;
        MemoryStream? replyBuffer = null;
        if (!string.IsNullOrWhiteSpace(CaptureDirectory))
        {
            captureId = Interlocked.Increment(ref _captureCounter);
            await CaptureAsync(context.Request, captureId, context.RequestAborted);
            originalBody = context.Response.Body;
            replyBuffer = new MemoryStream();
            context.Response.Body = replyBuffer;
        }

        var stopwatch = Stopwatch.StartNew();
        try
        {
            await next(context);
        }
        finally
        {
            if (replyBuffer is not null && originalBody is not null)
            {
                context.Response.Body = originalBody;
                replyBuffer.Position = 0;
                await replyBuffer.CopyToAsync(originalBody, context.RequestAborted);
                await SaveReplyAsync(context.Request, captureId, replyBuffer);
                await replyBuffer.DisposeAsync();
            }

            stopwatch.Stop();
            var path = context.Request.Path.Value ?? string.Empty;

            if (context.Response.StatusCode >= 400)
            {
                logger.LogWarning(
                    "HTTP {Method} {Path} -> {StatusCode} ({ElapsedMs}ms)",
                    context.Request.Method,
                    path,
                    context.Response.StatusCode,
                    stopwatch.ElapsedMilliseconds);
            }
            else
            {
                logger.LogInformation(
                    "HTTP {Method} {Path} -> {StatusCode} ({ElapsedMs}ms)",
                    context.Request.Method,
                    path,
                    context.Response.StatusCode,
                    stopwatch.ElapsedMilliseconds);
            }
        }
    }

    private async Task SaveReplyAsync(HttpRequest request, int captureId, MemoryStream reply)
    {
        try
        {
            if (reply.Length == 0 || reply.Length > CaptureLimit)
                return;
            Directory.CreateDirectory(CaptureDirectory!);
            var name = (request.Path.Value ?? "root").Trim('/').Replace('/', '_');
            var file = Path.Combine(CaptureDirectory!, $"{DateTime.Now:HHmmss}_{captureId:D4}_{name}.reply.bin");
            await File.WriteAllBytesAsync(file, reply.ToArray());
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            logger.LogWarning(ex, "Reply capture failed");
        }
    }

    private async Task CaptureAsync(HttpRequest request, int captureId, CancellationToken ct)
    {
        try
        {
            if (request.ContentLength is not > 0)
                return;
            request.Body.Position = 0;
            using var buffer = new MemoryStream();
            await request.Body.CopyToAsync(buffer, ct);
            request.Body.Position = 0;
            if (buffer.Length > CaptureLimit)
                return;

            Directory.CreateDirectory(CaptureDirectory!);
            var name = (request.Path.Value ?? "root").Trim('/').Replace('/', '_');
            var file = Path.Combine(CaptureDirectory!,
                $"{DateTime.Now:HHmmss}_{captureId:D4}_{name}.bin");
            await File.WriteAllBytesAsync(file, buffer.ToArray(), ct);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            logger.LogWarning(ex, "Request capture failed");
        }
    }

    private static async Task<byte[]> ReadPreviewAsync(HttpRequest request, CancellationToken ct)
    {
        if (request.ContentLength is not > 0)
            return Array.Empty<byte>();

        var readLength = (int)Math.Min(request.ContentLength.Value, 64 * 1024);
        var body = new byte[readLength];
        var total = 0;

        while (total < body.Length)
        {
            var read = await request.Body.ReadAsync(body.AsMemory(total, body.Length - total), ct);
            if (read == 0)
                break;
            total += read;
        }

        if (total != body.Length)
            Array.Resize(ref body, total);

        request.Body.Position = 0;
        return body;
    }

    private static string FormatPreview(byte[] body)
    {
        if (body.Length == 0)
            return "<empty>";

        var take = Math.Min(body.Length, PreviewLimit);
        var sb = new StringBuilder(take * 2);

        for (var i = 0; i < take; i++)
        {
            var b = body[i];
            switch (b)
            {
                case (byte)'\r':
                    sb.Append("\\r");
                    break;
                case (byte)'\n':
                    sb.Append("\\n");
                    break;
                case (byte)'\t':
                    sb.Append("\\t");
                    break;
                case >= 0x20 and <= 0x7E:
                    sb.Append((char)b);
                    break;
                default:
                    sb.Append("\\x");
                    sb.Append(b.ToString("X2"));
                    break;
            }
        }

        if (body.Length > take)
            sb.Append("...");

        return sb.ToString();
    }
}
