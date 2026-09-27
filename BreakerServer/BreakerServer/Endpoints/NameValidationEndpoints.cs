using BreakerServer.Infrastructure;
using BreakerServer.Services;

namespace BreakerServer.Endpoints;

public static class NameValidationEndpoints
{
    public static IEndpointRouteBuilder MapNameValidationEndpoints(this IEndpointRouteBuilder app)
    {
        var api = app.MapGroup("/dbtb-prd/{title}/api");

        api.MapPost("/sys/check_ngname", async (
            HttpRequest request,
            ApiResponseService responses,
            ILoggerFactory loggerFactory,
            CancellationToken ct) =>
        {
            var logger = loggerFactory.CreateLogger("NameValidation");
            var body = ObjectTree.List(await MessagePackHttp.ReadAsync(request, ct));

            string name = string.Empty;
            string language = string.Empty;

            if (body.Count > 1)
            {
                var args = ObjectTree.List(body[1]);
                if (args.Count > 0)
                    name = ObjectTree.Text(args[0]);
                if (args.Count > 1)
                    language = ObjectTree.Text(args[1]);
            }

            logger.LogDebug(
                "Accepted offline name validation name={Name} language={Language}",
                name,
                language);

            // Offline names are accepted locally.
            return responses.Respond(new object?[] { 0 });
        });

        return app;
    }
}
