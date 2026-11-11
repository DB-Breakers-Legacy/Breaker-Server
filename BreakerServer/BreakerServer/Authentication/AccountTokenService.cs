using System.Security.Cryptography;
using System.Text;

namespace BreakerServer.Authentication;

/// <summary>
/// The userId the client receives from user/auth and sends back in every request header.
///
/// It must stay the same for an account across logins: the client ties its local save (play.dat) to it,
/// and a different value at each login made the client discard the save and restart character creation
/// and the tutorial every time (live test 2026-11-10; ProjectTemporal's notes say the id "is checked by
/// save data"). It must also not be guessable from the public Steam ID, or anyone could act as anyone.
///
/// So it is HMAC-SHA256(server secret, Steam ID), first 16 bytes as 32 uppercase hex characters: stable per
/// account, secret per server. The secret is created once and kept next to the database.
/// </summary>
public sealed class AccountTokenService
{
    public const string SecretFileName = "account-token-secret.key";
    private readonly byte[] _secret;

    public AccountTokenService(IHostEnvironment environment)
        : this(LoadOrCreateSecret(Path.Combine(environment.ContentRootPath, SecretFileName)))
    {
    }

    public AccountTokenService(byte[] secret)
    {
        if (secret.Length < 32)
            throw new ArgumentException("The account token secret must be at least 32 bytes.", nameof(secret));
        _secret = secret;
    }

    public string TokenFor(long userId) =>
        Convert.ToHexString(HMACSHA256.HashData(_secret, Encoding.ASCII.GetBytes(userId.ToString()))[..16]);

    private static byte[] LoadOrCreateSecret(string path)
    {
        if (File.Exists(path))
        {
            var existing = File.ReadAllBytes(path);
            if (existing.Length >= 32)
                return existing;
        }

        var secret = RandomNumberGenerator.GetBytes(32);
        File.WriteAllBytes(path, secret);
        return secret;
    }
}
