using BreakerServer.Configuration;
using BreakerServer.Services;
using Microsoft.Extensions.Options;

namespace BreakerServer.Endpoints;

public static class NewsEndpoints
{
    public static IEndpointRouteBuilder MapNewsEndpoints(this IEndpointRouteBuilder app)
    {
        var configured = app.ServiceProvider
            .GetRequiredService<IOptions<BreakerOptions>>()
            .Value;

        if (!Uri.TryCreate(configured.NewsImageUrl, UriKind.Absolute, out var imageUri))
            throw new InvalidOperationException("Breaker:NewsImageUrl must be an absolute URL.");

        var imageRoute = imageUri.AbsolutePath;

        app.MapMethods(imageRoute, [HttpMethods.Get, HttpMethods.Head], (
            IWebHostEnvironment environment,
            IOptions<BreakerOptions> options) =>
        {
            var file = FindNewsImage(environment, options.Value.NewsImageFile);

            if (file is not null)
                return Results.File(file, "image/png");

            throw new InvalidOperationException("The bundled news image could not be found.");
        });

        MapApi(app, "/dbtb-prd/{title}/api");
        MapApi(app, "/dbtb-prd/{prefix}/{title}/api");

        return app;
    }

    private static void MapApi(IEndpointRouteBuilder app, string prefix)
    {
        var api = app.MapGroup(prefix);

        api.MapPost("/news/get_3d_model_news", (
            ApiResponseService responses,
            IOptions<BreakerOptions> options) =>
        {
            var (availableAt, expireAt) = ActiveWindow();
            return responses.Respond(new object?[]
            {
                0,
                new object?[]
                {
                    new object?[]
                    {
                        options.Value.NewsImageUrl,
                        availableAt,
                        expireAt
                    }
                }
            });
        });

        api.MapPost("/news/get_2d_ui_news_list", (ApiResponseService responses) =>
            responses.Respond(new object?[]
            {
                0,
                Array.Empty<object?>()
            }));
    }

    private static (string AvailableAt, string ExpireAt) ActiveWindow()
    {
        var now = DateTimeOffset.Now;
        return (
            now.AddDays(-1).ToString("yyyy-MM-dd HH:mm:ss"),
            now.AddYears(10).ToString("yyyy-MM-dd HH:mm:ss"));
    }

    private static string? FindNewsImage(IWebHostEnvironment environment, string configuredPath)
    {
        if (string.IsNullOrWhiteSpace(configuredPath))
            return null;

        if (Path.IsPathRooted(configuredPath))
            return File.Exists(configuredPath) ? configuredPath : null;

        var candidates = new[]
        {
            Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, configuredPath)),
            Path.GetFullPath(Path.Combine(environment.ContentRootPath, configuredPath)),
            Path.GetFullPath(Path.Combine(Directory.GetCurrentDirectory(), configuredPath))
        };

        return candidates.FirstOrDefault(File.Exists);
    }
}
