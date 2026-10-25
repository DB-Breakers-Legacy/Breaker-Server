using System.Collections.Concurrent;
using System.Security.Cryptography;

namespace BreakerServer.Authentication;

public sealed record AuthSession(long UserId, string Session, DateTimeOffset CreatedAt, uint TicketSerial);

public sealed class AuthSessionRegistry
{
    private readonly ConcurrentDictionary<string, AuthSession> _sessions =
        new(StringComparer.OrdinalIgnoreCase);

    public AuthSession Create(long userId, uint ticketSerial)
    {
        var session = Convert.ToHexString(RandomNumberGenerator.GetBytes(16));
        var value = new AuthSession(userId, session, DateTimeOffset.UtcNow, ticketSerial);
        _sessions[session] = value;
        return value;
    }

    public bool TryGet(string session, out AuthSession? value)
    {
        if (string.IsNullOrWhiteSpace(session))
        {
            value = null;
            return false;
        }

        return _sessions.TryGetValue(session, out value);
    }
}
