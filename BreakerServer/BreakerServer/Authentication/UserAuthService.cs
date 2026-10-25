using BreakerServer.Services;

namespace BreakerServer.Authentication;

public sealed record UserAuthResult(bool Success, string AuthToken, string TicketKind, string? FailureReason = null);

public sealed class UserAuthService(
    AccountService accounts,
    AuthSessionRegistry sessions,
    ILogger<UserAuthService> logger)
{
    public async Task<UserAuthResult> AuthenticateAsync(UserAuthRequest request, CancellationToken ct)
    {
        if (request.NumericId <= 0)
            return Failure("Numeric user identifier is invalid.");

        if (!NetworkToken.TryParse(request.NetworkToken, out var token) || token is null)
            return Failure("NetworkToken is not uppercase hexadecimal.");

        uint serial = 0;
        var ticketKind = "opaque-platform";
        ulong? embeddedSteamId = null;

        if (LocalSteamTicket.TryParse(token.Bytes, out var localTicket) && localTicket is not null)
        {
            ticketKind = "breakers-revived";
            serial = localTicket.Serial;
            embeddedSteamId = localTicket.SteamId;
        }

        await accounts.GetOrCreateAsync(request.NumericId, ct);
        var session = sessions.Create(request.NumericId, serial);

        logger.LogInformation(
            "user/auth accepted numericId={NumericId} arg3={Argument3} ticket={TicketKind} ticketBytes={TicketBytes} embeddedSteamId={EmbeddedSteamId} serial={Serial} authToken={AuthToken}",
            request.NumericId,
            request.Argument3,
            ticketKind,
            token.Bytes.Length,
            embeddedSteamId,
            serial,
            session.Session);

        return new UserAuthResult(true, session.Session, ticketKind);
    }

    private UserAuthResult Failure(string reason)
    {
        logger.LogWarning("user/auth rejected: {Reason}", reason);
        return new UserAuthResult(false, string.Empty, "invalid", reason);
    }
}
