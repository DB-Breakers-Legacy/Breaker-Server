using MessagePack;

namespace BreakerServer.Infrastructure;

public sealed class BadRequestMiddleware(
    RequestDelegate next,
    ILogger<BadRequestMiddleware> logger)
{
    public async Task InvokeAsync(HttpContext context)
    {
        try
        {
            await next(context);
        }
        catch (Exception ex) when (IsBadRequest(ex))
        {
            if (context.Response.HasStarted) throw;

            logger.LogDebug(ex, "Rejected malformed request");
            context.Response.Clear();
            context.Response.StatusCode = ex is BadHttpRequestException badRequest
                ? badRequest.StatusCode
                : StatusCodes.Status400BadRequest;
            await context.Response.WriteAsJsonAsync(
                new { error = "The request body is invalid." },
                context.RequestAborted);
        }
    }

    private static bool IsBadRequest(Exception ex) => ex is
        BadHttpRequestException or
        InvalidDataException or
        MessagePackSerializationException or
        FormatException or
        OverflowException or
        InvalidCastException;
}
