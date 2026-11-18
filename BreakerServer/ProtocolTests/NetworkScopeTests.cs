using System.Net;
using System.Net.Sockets;
using BreakerServer.Configuration;
using BreakerServer.Network;
using BreakerServer.Services;
using Microsoft.Extensions.Logging.Abstractions;
using Xunit;

namespace BreakerServer.ProtocolTests;

/// <summary>
/// The UDP services listen only on Breaker:BindAddress (loopback by default), not on every interface.
/// </summary>
public sealed class NetworkScopeTests
{
    [Fact]
    public void BindAddressDefaultsToLoopback()
    {
        var options = new BreakerOptions();
        Assert.Equal("127.0.0.1", options.BindAddress);
        Assert.Equal(IPAddress.Loopback, options.BindIPAddress);
    }

    [Fact]
    public void UnparseableBindAddressFallsBackToLoopbackNotAnyInterface()
    {
        var options = new BreakerOptions { BindAddress = "not-an-ip" };
        Assert.Equal(IPAddress.Loopback, options.BindIPAddress);
    }

    [Fact]
    public async Task DiarkisListenerBindsTheConfiguredAddressOnly()
    {
        var port = FreeUdpPort();
        var options = new BreakerOptions { BindAddress = "127.0.0.1", UdpPort = port, UdpSessionPort = FreeUdpPort() };
        var service = new DiarkisUdpService(
            Microsoft.Extensions.Options.Options.Create(options),
            NullLogger<DiarkisUdpService>.Instance,
            sessionListener: false,
            sessionKeys: new SessionKeyRegistry());
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        await service.StartAsync(timeout.Token);
        try
        {
            // Wait until the listener owns 127.0.0.1:port.
            var deadline = DateTime.UtcNow.AddSeconds(5);
            while (CanBind(IPAddress.Loopback, port) && DateTime.UtcNow < deadline)
                await Task.Delay(25, timeout.Token);
            Assert.False(CanBind(IPAddress.Loopback, port), "listener did not bind 127.0.0.1");

            // A listener on 0.0.0.0 would also occupy the port on every other local address.
            var other = OtherLocalIPv4();
            if (other is not null)
                Assert.True(CanBind(other, port), $"port {port} is taken on {other}: listener is not loopback-only");
        }
        finally
        {
            await service.StopAsync(CancellationToken.None);
        }
    }

    private static bool CanBind(IPAddress address, int port)
    {
        try
        {
            using var probe = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
            probe.Bind(new IPEndPoint(address, port));
            return true;
        }
        catch (SocketException)
        {
            return false;
        }
    }

    private static IPAddress? OtherLocalIPv4() =>
        System.Net.NetworkInformation.NetworkInterface.GetAllNetworkInterfaces()
            .Where(n => n.OperationalStatus == System.Net.NetworkInformation.OperationalStatus.Up)
            .SelectMany(n => n.GetIPProperties().UnicastAddresses)
            .Select(a => a.Address)
            .FirstOrDefault(a => a.AddressFamily == AddressFamily.InterNetwork && !IPAddress.IsLoopback(a));

    private static int FreeUdpPort()
    {
        using var udp = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        return ((IPEndPoint)udp.Client.LocalEndPoint!).Port;
    }
}
