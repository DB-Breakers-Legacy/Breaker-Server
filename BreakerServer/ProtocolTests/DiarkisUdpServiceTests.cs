using System.Net;
using System.Net.Sockets;
using System.Text;
using BreakerServer.Configuration;
using BreakerServer.Network;
using BreakerServer.Protocol.Diarkis;
using BreakerServer.Services;
using Microsoft.Extensions.Logging.Abstractions;
using Microsoft.Extensions.Options;
using Xunit;

namespace BreakerServer.ProtocolTests;

public sealed class DiarkisUdpServiceTests
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
        var service = new DiarkisUdpService(
            options,
            NullLogger<DiarkisUdpService>.Instance,
            sessionListener: true,
            sessionKeys: registry);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        using var client = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        client.Connect(new IPEndPoint(IPAddress.Loopback, port));

        await service.StartAsync(timeout.Token);
        try
        {
            var sid = Convert.FromHexString(issued.Sid);
            var syn = DiarkisPacketFactory.Combine(DatagramHeader.Pack(0, 0), sid);
            await SendAndReceiveAsync(client, syn, timeout.Token);

            await client.SendAsync(DiarkisPacketFactory.Combine(DatagramHeader.Pack(1, 1), sid), timeout.Token);
            await client.ReceiveAsync(timeout.Token);
            var reply = await client.ReceiveAsync(timeout.Token);

            var header = DatagramHeader.Parse(reply.Buffer);
            var command = CommandEnvelopeCodec.Parse(header.Payload, false);
            registry.TryResolve(sid, out var keys);
            var decrypted = SecureEnvelope.Decrypt(command.Body.AsSpan(4), keys!);

            var clientPort = ((IPEndPoint)client.Client.LocalEndPoint!).Port;
            Assert.Equal($"127.0:{clientPort}", Encoding.UTF8.GetString(decrypted.Plaintext!));
        }
        finally
        {
            await service.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task DirectoryReplyIsClearTextAndFlipsTheFlag()
    {
        var port = FreeUdpPort();
        var options = Options.Create(new BreakerOptions
        {
            UdpPort = port,
            UdpHost = "127.0.0.1",
            DiarkisIvKey = string.Concat(Enumerable.Repeat("01", 16)),
            DiarkisAesKey = string.Concat(Enumerable.Repeat("02", 16)),
            DiarkisSidKey = string.Concat(Enumerable.Repeat("03", 16)),
            DiarkisHashKey = string.Concat(Enumerable.Repeat("04", 16))
        });
        var service = new DiarkisUdpService(options, NullLogger<DiarkisUdpService>.Instance);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        using var client = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        client.Connect(new IPEndPoint(IPAddress.Loopback, port));

        await service.StartAsync(timeout.Token);
        try
        {
            var query = DiarkisPacketFactory.Combine(
                [0, 0, 0x0C, 0x0C, 0, 0, 0, 1, 0, 16],
                new byte[16]);
            var response = await SendAndReceiveAsync(client, query, timeout.Token);
            Assert.Equal(DirectoryResponse(0x0F, port), response.Buffer);

            query[0] = 0x0F;
            query[1] = 0x0F;
            response = await SendAndReceiveAsync(client, query, timeout.Token);
            Assert.Equal(DirectoryResponse(0, port), response.Buffer);
        }
        finally
        {
            await service.StopAsync(CancellationToken.None);
        }
    }

    private static byte[] DirectoryResponse(byte flag, int port)
    {
        var payload = Encoding.ASCII.GetBytes($"127.0.0.1:{port}");
        return DiarkisPacketFactory.Combine(
            [flag, flag, 0x0C, 0x0C, 0, 0, 0, 1, (byte)(payload.Length >> 8), (byte)payload.Length],
            payload);
    }

    private static async Task<UdpReceiveResult> SendAndReceiveAsync(
        UdpClient client,
        byte[] packet,
        CancellationToken timeout)
    {
        while (true)
        {
            using var attempt = CancellationTokenSource.CreateLinkedTokenSource(timeout);
            attempt.CancelAfter(TimeSpan.FromMilliseconds(100));
            try
            {
                await client.SendAsync(packet, attempt.Token);
                return await client.ReceiveAsync(attempt.Token);
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
