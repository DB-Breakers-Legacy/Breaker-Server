using System.Collections.Concurrent;
using System.Security.Cryptography;

namespace BreakerServer.Authentication;

public sealed record AuthSession(long UserId, string Session, DateTimeOffset CreatedAt, uint TicketSerial);

public sealed class AuthSessionRegistry(TimeProvider? clock = null)
{
    private const int MaxSessions = 4096;
    private static readonly TimeSpan Lifetime = TimeSpan.FromHours(8);

    private sealed record Entry(AuthSession Session, DateTimeOffset ExpiresAt);

    private readonly ConcurrentDictionary<string, Entry> _sessions = new(StringComparer.Ordinal);
    private readonly TimeProvider _clock = clock ?? TimeProvider.System;

    public AuthSession Create(long userId, uint ticketSerial)
    {
        var now = _clock.GetUtcNow();
        while (_sessions.Count >= MaxSessions)
        {
            var oldest = _sessions.OrderBy(x => x.Value.Session.CreatedAt).Select(x => x.Key).FirstOrDefault();
            if (oldest is null || _sessions.TryRemove(oldest, out _)) break;
        }

        var session = Convert.ToHexString(RandomNumberGenerator.GetBytes(16));
        var value = new AuthSession(userId, session, now, ticketSerial);
        _sessions[session] = new Entry(value, now.Add(Lifetime));
        return value;
    }

    public bool TryGet(string session, out AuthSession? value)
    {
        value = null;
        if (string.IsNullOrWhiteSpace(session) ||
            !_sessions.TryGetValue(session, out var entry))
            return false;

        if (entry.ExpiresAt <= _clock.GetUtcNow())
        {
            _sessions.TryRemove(session, out _);
            return false;
        }

        value = entry.Session;
        return true;
    }

}
