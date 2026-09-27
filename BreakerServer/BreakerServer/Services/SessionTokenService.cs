using System.Security.Cryptography;

namespace BreakerServer.Services;

public sealed class SessionTokenService
{
    public string Next() => Convert.ToHexString(RandomNumberGenerator.GetBytes(16));
}
