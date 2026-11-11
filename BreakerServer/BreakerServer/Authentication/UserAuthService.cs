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

        if (!LocalSteamTicket.TryParse(token.Bytes, out var localTicket) || localTicket is null)
            return Failure("NetworkToken is not a valid Breakers Revived local ticket.");

        if (localTicket.SteamId != (ulong)request.NumericId)
            return Failure("Local ticket does not match the requested user.");

        await accounts.GetOrCreateAsync(request.NumericId, ct);
        var session = sessions.Create(request.NumericId, localTicket.Serial);

        logger.LogInformation(
            "user/auth accepted numericId={NumericId} arg3={Argument3} ticketBytes={TicketBytes} serial={Serial}",
            request.NumericId,
            request.Argument3,
            token.Bytes.Length,
            localTicket.Serial);

        return new UserAuthResult(true, session.Session, "breakers-revived");
    }

    private UserAuthResult Failure(string reason)
    {
        logger.LogWarning("user/auth rejected: {Reason}", reason);
        return new UserAuthResult(false, string.Empty, "invalid", reason);
    }
}
