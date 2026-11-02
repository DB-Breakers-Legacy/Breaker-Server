using System.Net;
using System.Net.Sockets;
using BreakerServer.Configuration;
using BreakerServer.Network;
using BreakerServer.Protocol.Diarkis;
using BreakerServer.Services;
using Microsoft.Extensions.Logging.Abstractions;
using Microsoft.Extensions.Options;
using Xunit;

namespace BreakerServer.ProtocolTests;

public sealed class DiarkisSessionUdpServiceTests
{
    [Fact]
    public async Task SessionReplyUsesKeysIssuedForTheClientSid()
    {
        var registry = new SessionKeyRegistry();
        var issued = registry.Create();
        var port = FreeUdpPort();
        var options = Options.Create(new BreakerOptions
        {
            UdpSessionPort = port,
            AdvertisedHost = "127.0.0.1"
        });
        var service = new DiarkisSessionUdpService(
            options,
            registry,
            NullLogger<DiarkisSessionUdpService>.Instance);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        using var client = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        client.Connect(new IPEndPoint(IPAddress.Loopback, port));

        await service.StartAsync(timeout.Token);
        try
        {
            var sid = Convert.FromHexString(issued.Sid);
            var syn = DiarkisPacketFactory.Combine(DatagramHeader.Pack(0, 0), sid);
            await ReceiveHandshakeAck(client, syn, timeout.Token);

            await client.SendAsync(DiarkisPacketFactory.Combine(DatagramHeader.Pack(1, 1), sid), timeout.Token);
            await client.ReceiveAsync(timeout.Token);
            var reply = await client.ReceiveAsync(timeout.Token);

            var header = DatagramHeader.Parse(reply.Buffer);
            var command = CommandEnvelopeCodec.Parse(header.Payload, false);
            registry.TryResolve(sid, out var keys);
            var decrypted = SecureEnvelope.Decrypt(command.Body.AsSpan(4), keys!);

            var clientPort = ((IPEndPoint)client.Client.LocalEndPoint!).Port;
            Assert.Equal($"127.0:{clientPort}", System.Text.Encoding.UTF8.GetString(decrypted.Plaintext!));
        }
        finally
        {
            await service.StopAsync(CancellationToken.None);
        }
    }

    private static async Task ReceiveHandshakeAck(
        UdpClient client,
        byte[] syn,
        CancellationToken timeout)
    {
        while (true)
        {
            using var attempt = CancellationTokenSource.CreateLinkedTokenSource(timeout);
            attempt.CancelAfter(TimeSpan.FromMilliseconds(100));
            try
            {
                await client.SendAsync(syn, attempt.Token);
                await client.ReceiveAsync(attempt.Token);
                return;
            }
            catch (OperationCanceledException) when (!timeout.IsCancellationRequested)
            {
            }
            catch (SocketException ex) when (ex.SocketErrorCode == SocketError.ConnectionReset)
            {
            }

            await Task.Delay(20, timeout);
        }
    }

    private static int FreeUdpPort()
    {
        using var udp = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        return ((IPEndPoint)udp.Client.LocalEndPoint!).Port;
    }
}
