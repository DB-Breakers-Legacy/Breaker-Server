namespace BreakerServer.Infrastructure;

public sealed class CdnHeadersMiddleware(RequestDelegate next)
{
    public async Task InvokeAsync(HttpContext context)
    {
        context.Response.OnStarting(() =>
        {
            context.Response.Headers["server"] = "nginx";
            context.Response.Headers["via"] = "1.1 google";
            return Task.CompletedTask;
        });
        await next(context);
    }
}
