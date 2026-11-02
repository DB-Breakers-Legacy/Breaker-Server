using System.Collections.Concurrent;
using BreakerServer.Protocol.Diarkis;
using System.Security.Cryptography;

namespace BreakerServer.Services;

public sealed record SessionKeys(string Sid, string Aes, string Iv, string Mac);

public sealed class SessionKeyRegistry(TimeProvider? clock = null)
{
    private const int MaxSessions = 4096;
    private static readonly TimeSpan Lifetime = TimeSpan.FromMinutes(15);

    private sealed record Entry(SessionKeys Keys, DateTimeOffset CreatedAt, DateTimeOffset ExpiresAt);

    private readonly ConcurrentDictionary<string, Entry> _keys = new(StringComparer.Ordinal);
    private readonly TimeProvider _clock = clock ?? TimeProvider.System;

    public SessionKeys Create()
    {
        var now = _clock.GetUtcNow();
        while (_keys.Count >= MaxSessions)
        {
            var oldest = _keys.OrderBy(x => x.Value.CreatedAt).Select(x => x.Key).FirstOrDefault();
            if (oldest is null || _keys.TryRemove(oldest, out _)) break;
        }

        static string Hex() => Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant();
        var keys = new SessionKeys(Hex(), Hex(), Hex(), Hex());
        _keys[keys.Sid] = new Entry(keys, now, now.Add(Lifetime));
        return keys;
    }

    public bool TryGet(string sid, out SessionKeys? keys)
    {
        keys = null;
        if (!_keys.TryGetValue(sid, out var entry))
            return false;

        if (entry.ExpiresAt <= _clock.GetUtcNow())
        {
            _keys.TryRemove(sid, out _);
            return false;
        }

        keys = entry.Keys;
        return true;
    }

    public bool TryResolve(ReadOnlySpan<byte> sid, out DiarkisKeySet? keys)
    {
        keys = null;
        if (sid.Length != 16 || !TryGet(Convert.ToHexString(sid).ToLowerInvariant(), out var entry) || entry is null)
            return false;
        keys = SecureEnvelope.FromHex(entry.Sid, entry.Aes, entry.Iv, entry.Mac);
        return true;
    }

}
