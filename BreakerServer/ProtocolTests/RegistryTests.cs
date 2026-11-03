using BreakerServer.Authentication;
using BreakerServer.Services;
using Xunit;

namespace BreakerServer.ProtocolTests;

public sealed class RegistryTests
{
    [Fact]
    public void IssuedSessionKeysResolveToOriginalMaterial()
    {
        var sessions = new SessionKeyRegistry();
        var issued = sessions.Create();

        Assert.True(sessions.TryResolve(Convert.FromHexString(issued.Sid), out var keys));
        Assert.Equal(Convert.FromHexString(issued.Aes), keys!.Key);
        Assert.False(sessions.TryResolve(new byte[15], out _));
        Assert.False(sessions.TryResolve(new byte[16], out _));
    }

    [Fact]
    public void SessionKeysExpireAndUseCaseSensitiveIdentifiers()
    {
        var clock = new ManualTimeProvider();
        var sessions = new SessionKeyRegistry(clock);
        var issued = sessions.Create();

        Assert.False(sessions.TryGet(issued.Sid.ToUpperInvariant(), out _));
        clock.Advance(TimeSpan.FromMinutes(16));
        Assert.False(sessions.TryGet(issued.Sid, out _));
        Assert.False(sessions.TryResolve(Convert.FromHexString(issued.Sid), out _));
    }

    [Fact]
    public void AuthSessionsExpireAndUseCaseSensitiveTokens()
    {
        var clock = new ManualTimeProvider();
        var sessions = new AuthSessionRegistry(clock);
        var issued = sessions.Create(42, 1);

        Assert.False(sessions.TryGet(issued.Session.ToLowerInvariant(), out _));
        clock.Advance(TimeSpan.FromHours(9));
        Assert.False(sessions.TryGet(issued.Session, out _));
    }

    [Fact]
    public void RegistriesEvictOldestEntriesAtCapacity()
    {
        var clock = new ManualTimeProvider();
        var auth = new AuthSessionRegistry(clock);
        var firstAuth = auth.Create(1, 0);
        clock.Advance(TimeSpan.FromSeconds(1));
        for (var i = 0; i < 4096; i++) auth.Create(i + 2, 0);
        Assert.False(auth.TryGet(firstAuth.Session, out _));

        var keys = new SessionKeyRegistry(clock);
        var firstKeys = keys.Create();
        clock.Advance(TimeSpan.FromSeconds(1));
        for (var i = 0; i < 4096; i++) keys.Create();
        Assert.False(keys.TryGet(firstKeys.Sid, out _));
    }

    private sealed class ManualTimeProvider : TimeProvider
    {
        private DateTimeOffset _now = DateTimeOffset.UnixEpoch;
        public override DateTimeOffset GetUtcNow() => _now;
        public void Advance(TimeSpan duration) => _now += duration;
    }
}
