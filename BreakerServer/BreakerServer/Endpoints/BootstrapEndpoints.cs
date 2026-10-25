using BreakerServer.Configuration;
using BreakerServer.Services;
using Microsoft.Extensions.Options;

namespace BreakerServer.Endpoints;

public static class BootstrapEndpoints
{
    public static IEndpointRouteBuilder MapBootstrapEndpoints(this IEndpointRouteBuilder app)
    {
        var root = app.MapGroup("/{title}/api");

        root.MapPost("/sys/get_env_v3", (
            ApiResponseService responses,
            IOptions<BreakerOptions> options,
            ILoggerFactory loggerFactory) =>
        {
            var backendBaseUrl = options.Value.BackendBaseUrl.TrimEnd('/');
            var commonBaseUrl = $"{backendBaseUrl}/";
            var gameBaseUrl = $"{backendBaseUrl}/dbtb-prd/";

            loggerFactory.CreateLogger("EnvironmentBootstrap").LogInformation(
                "Environment bootstrap common={CommonBaseUrl} game={GameBaseUrl}",
                commonBaseUrl,
                gameBaseUrl);

            return responses.Respond(
                new object?[] { 0, commonBaseUrl, gameBaseUrl },
                string.Empty,
                false);
        });

        root.MapPost("/sys/agree_kpi", (ApiResponseService responses, ResponseCatalog catalog) =>
            responses.Respond(catalog.Get("agreeKPI"), string.Empty, false));

        root.MapPost("/user/get_country", (ApiResponseService responses, ResponseCatalog catalog) =>
            responses.Respond(catalog.Get("getUserCountry"), versioned: false));

        root.MapPost("/user/get_tracking_num", (ApiResponseService responses, ResponseCatalog catalog) =>
            responses.Respond(catalog.Get("getTrackingNum"), versioned: false));

        return app;
    }
}
