using System.Diagnostics;
using System.Text;

namespace BreakerServer.Infrastructure;

public sealed class RequestTraceMiddleware(
    RequestDelegate next,
    ILogger<RequestTraceMiddleware> logger)
{
    private const int PreviewLimit = 256;

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

        var stopwatch = Stopwatch.StartNew();
        try
        {
            await next(context);
        }
        finally
        {
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
