using BreakerServer.Authentication;
using Microsoft.Extensions.Logging.Abstractions;
using Xunit;

namespace BreakerServer.ProtocolTests;

public sealed class RequestIdentityResolverTests
{
    [Fact]
    public void NumericUserIdIsNotAcceptedAsIdentity()
    {
        var resolver = new RequestIdentityResolver(
            new AuthSessionRegistry(),
            NullLogger<RequestIdentityResolver>.Instance);

        Assert.Throws<InvalidDataException>(() => resolver.ResolveUserId(Body("12345")));
    }

    [Fact]
    public void ActiveSessionResolvesToItsOwner()
    {
        var sessions = new AuthSessionRegistry();
        var session = sessions.Create(12345, 7);
        var resolver = new RequestIdentityResolver(sessions, NullLogger<RequestIdentityResolver>.Instance);

        Assert.Equal(12345, resolver.ResolveUserId(Body(session.Session)));
    }

    [Fact]
    public void AccountTokenIsStablePerAccountAndSecretPerServer()
    {
        // The client ties play.dat to this id, so it must not change between logins.
        var secret = Enumerable.Range(0, 32).Select(i => (byte)i).ToArray();
        var tokens = new AccountTokenService(secret);
        var again = new AccountTokenService(secret.ToArray());
        var otherServer = new AccountTokenService(Enumerable.Repeat((byte)9, 32).ToArray());

        Assert.Equal(tokens.TokenFor(76561199006065890), again.TokenFor(76561199006065890));
        Assert.NotEqual(tokens.TokenFor(76561199006065890), tokens.TokenFor(76561199006065891));
        Assert.NotEqual(tokens.TokenFor(76561199006065890), otherServer.TokenFor(76561199006065890));
        Assert.Matches("^[0-9A-F]{32}$", tokens.TokenFor(76561199006065890));
        Assert.DoesNotContain("76561199006065890", tokens.TokenFor(76561199006065890));
    }

    [Fact]
    public void StableTokenResolvesToItsOwnerAfterEachLogin()
    {
        var sessions = new AuthSessionRegistry();
        var tokens = new AccountTokenService(Enumerable.Repeat((byte)1, 32).ToArray());
        var first = sessions.Create(12345, 1, tokens.TokenFor(12345));
        var second = sessions.Create(12345, 2, tokens.TokenFor(12345));
        var resolver = new RequestIdentityResolver(sessions, NullLogger<RequestIdentityResolver>.Instance);

        Assert.Equal(first.Session, second.Session);
        Assert.Equal(12345, resolver.ResolveUserId(Body(second.Session)));
    }

    private static object[] Body(string userId) =>
        [new Dictionary<string, object?> { ["userId"] = userId }, new List<object?>()];
}
