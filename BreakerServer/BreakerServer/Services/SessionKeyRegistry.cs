using System.Collections.Concurrent;
using System.Security.Cryptography;

namespace BreakerServer.Services;

public sealed record SessionKeys(string Sid, string Aes, string Iv, string Mac);

public sealed class SessionKeyRegistry
{
    private readonly ConcurrentDictionary<string, SessionKeys> _keys = new();
    public SessionKeys Create()
    {
        static string Hex() => Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant();
        var keys = new SessionKeys(Hex(), Hex(), Hex(), Hex());
        _keys[keys.Sid] = keys;
        return keys;
    }
    public bool TryGet(string sid, out SessionKeys? keys) => _keys.TryGetValue(sid, out keys);
}
