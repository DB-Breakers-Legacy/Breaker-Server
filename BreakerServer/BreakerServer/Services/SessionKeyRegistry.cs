using System.Collections.Concurrent;
using BreakerServer.Protocol.Diarkis;
using System.Security.Cryptography;

namespace BreakerServer.Services;

public sealed record SessionKeys(string Sid, string Aes, string Iv, string Mac);

public sealed class SessionKeyRegistry
{
    private readonly ConcurrentDictionary<string, SessionKeys> _keys = new(StringComparer.OrdinalIgnoreCase);
    public SessionKeys Create()
    {
        static string Hex() => Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant();
        var keys = new SessionKeys(Hex(), Hex(), Hex(), Hex());
        _keys[keys.Sid] = keys;
        return keys;
    }
    public bool TryGet(string sid, out SessionKeys? keys) => _keys.TryGetValue(sid, out keys);

    public bool TryResolve(ReadOnlySpan<byte> sid, out DiarkisKeySet? keys)
    {
        keys = null;
        if (sid.Length != 16 || !TryGet(Convert.ToHexString(sid), out var entry) || entry is null)
            return false;
        keys = SecureEnvelope.FromHex(entry.Sid, entry.Aes, entry.Iv, entry.Mac);
        return true;
    }
}
