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

    private static object[] Body(string userId) =>
        [new Dictionary<string, object?> { ["userId"] = userId }, new List<object?>()];
}
