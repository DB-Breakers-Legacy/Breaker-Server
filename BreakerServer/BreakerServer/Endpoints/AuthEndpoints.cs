using BreakerServer.Authentication;
using BreakerServer.Infrastructure;
using BreakerServer.Services;

namespace BreakerServer.Endpoints;

public static class AuthEndpoints
{
    public static IEndpointRouteBuilder MapAuthEndpoints(this IEndpointRouteBuilder app)
    {
        var root = app.MapGroup("/{title}/api");

        root.MapPost("/user/auth", async (
            HttpRequest request,
            ApiResponseService responses,
            UserAuthService auth,
            CancellationToken ct) =>
        {
            try
            {
                var body = await MessagePackHttp.ReadAsync(request, ct);
                var parsed = UserAuthRequest.Parse(body);
                var result = await auth.AuthenticateAsync(parsed, ct);

                if (!result.Success)
                    return responses.Respond(new object?[] { 1, new object?[] { string.Empty, 0 } }, string.Empty, false);

                return responses.Respond(
                    new object?[] { 0, new object?[] { result.AuthToken, 0 } },
                    result.AuthToken,
                    false);
            }
            catch (Exception ex) when (ex is InvalidDataException or FormatException or OverflowException)
            {
                return responses.Respond(new object?[] { 1, new object?[] { string.Empty, 0 } }, string.Empty, false);
            }
        });

        return app;
    }
}
