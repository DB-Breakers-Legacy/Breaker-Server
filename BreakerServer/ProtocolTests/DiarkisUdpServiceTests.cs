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

/// <summary>
/// Exercises the UDP services the way the live client did on 2026-11-10: SYN (type 2) with the SID,
/// then commands 0x0001 / 0x0131 as DAT (type 3), resent as type 5 until acknowledged.
/// </summary>
public sealed class DiarkisUdpServiceTests
{
    private static readonly byte[] InitPlaintext =
        [0x00, 0x00, 0x30, 0x39, 0xA0, 0x01, 0x00, 0x00,
         0, 0, 0, 13, .. "203.0.113.5:1"u8, 0, 0, 0, 13, .. "192.168.0.2:1"u8];

    [Fact]
    public async Task SessionListenerAcksSynAndAnswersInitWithKeysIssuedForTheSid()
    {
        var registry = new SessionKeyRegistry();
        var issued = registry.Create();
        await using var harness = await Harness.StartAsync(registry, sessionListener: true);
        var sid = Convert.FromHexString(issued.Sid);
        registry.TryResolve(sid, out var keys);

        var ack = await harness.SendAndReceiveAsync(DiarkisPacketFactory.Combine(DatagramHeader.Pack(0, 2), sid));
        Assert.Equal(DiarkisPacketFactory.Ack(0), ack);

        await harness.SendAsync(ClientDat(7, 0x0001, sid, InitPlaintext, keys!));
        Assert.Equal(DiarkisPacketFactory.Ack(7), await harness.ReceiveAsync());

        var (command, status, plaintext) = Open(await harness.ReceiveAsync(), keys!);
        Assert.Equal(0x0001, command);
        Assert.Equal(1, status);
        Assert.Equal([0x01, 0x00, 0x00, 0x30, 0x39, 0xA0, 0x01, 0x00, 0x00, .. Encoding.ASCII.GetBytes($"127.0.0.1:{harness.ClientPort}")],
            plaintext);
    }

    [Fact]
    public async Task MatchingListenerPushesItsAddressAfterInitAndResendsUntilAcked()
    {
        var registry = new SessionKeyRegistry();
        var issued = registry.Create();
        await using var harness = await Harness.StartAsync(registry, sessionListener: false);
        var sid = Convert.FromHexString(issued.Sid);
        registry.TryResolve(sid, out var keys);

        await harness.SendAndReceiveAsync(DiarkisPacketFactory.Combine(DatagramHeader.Pack(0, 2), sid));
        await harness.SendAsync(ClientDat(0, 0x0001, sid, InitPlaintext, keys!));
        Assert.Equal(DiarkisPacketFactory.Ack(0), await harness.ReceiveAsync());

        var reply = await harness.ReceiveAsync();
        Assert.Equal(0x0001, Open(reply, keys!).Command);
        Assert.Equal(0, DatagramHeader.Parse(reply).Sequence);

        var push = await harness.ReceiveAsync();
        var (command, status, plaintext) = Open(push, keys!);
        var address = $"127.0.0.1:{harness.Options.UdpPort}";
        Assert.Equal(0x012F, command);
        Assert.Equal(0xFF, status);
        Assert.Equal([0, (byte)address.Length, .. Encoding.ASCII.GetBytes(address)], plaintext);
        Assert.Equal(1, DatagramHeader.Parse(push).Sequence);

        // Acknowledge only the reply (seq 0): the push (seq 1) must come back as a type-5 resend.
        await harness.SendAsync(DiarkisPacketFactory.Combine(DatagramHeader.Pack(0, 4), sid));
        var resend = await harness.ReceiveAsync(TimeSpan.FromSeconds(3));
        var resendHeader = DatagramHeader.Parse(resend);
        Assert.Equal(5, resendHeader.PacketType);
        Assert.Equal(1, resendHeader.Sequence);
    }

    [Fact]
    public async Task HolePunchRequestsStopAfterTheSecondDone()
    {
        // 0x012F = UdpRequestHolePunch, 0x0130 = UdpNotifyHolePunchDone (client name table).
        // Live client 2026-11-10: answering every "done" with a new request looped ~800,000 times.
        var registry = new SessionKeyRegistry();
        var issued = registry.Create();
        await using var harness = await Harness.StartAsync(registry, sessionListener: false);
        var sid = Convert.FromHexString(issued.Sid);
        registry.TryResolve(sid, out var keys);

        await harness.SendAndReceiveAsync(DiarkisPacketFactory.Combine(DatagramHeader.Pack(0, 2), sid));
        await harness.SendAsync(ClientDat(0, 0x0001, sid, InitPlaintext, keys!));
        await harness.ReceiveAsync();                                          // ACK 0
        await harness.ReceiveAsync();                                          // 0x0001 reply (seq 0)
        var first = Open(await harness.ReceiveAsync(), keys!);                 // request 1 (seq 1)
        Assert.Equal(0x012F, first.Command);
        await harness.SendAsync(DiarkisPacketFactory.Combine(DatagramHeader.Pack(0, 4), sid));
        await harness.SendAsync(DiarkisPacketFactory.Combine(DatagramHeader.Pack(1, 4), sid));

        await harness.SendAsync(ClientDat(1, 0x0130, sid, [], keys!));
        Assert.Equal(DiarkisPacketFactory.Ack(1), await harness.ReceiveAsync());
        var second = await harness.ReceiveAsync();                             // request 2 (seq 2)
        var (command, _, plaintext) = Open(second, keys!);
        var sessionAddress = $"127.0.0.1:{harness.Options.UdpSessionPort}";
        Assert.Equal(0x012F, command);
        Assert.Equal([0, (byte)sessionAddress.Length, .. Encoding.ASCII.GetBytes(sessionAddress)], plaintext);
        await harness.SendAsync(DiarkisPacketFactory.Combine(DatagramHeader.Pack(2, 4), sid));

        await harness.SendAsync(ClientDat(2, 0x0130, sid, [], keys!));
        Assert.Equal(DiarkisPacketFactory.Ack(2), await harness.ReceiveAsync());
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => harness.ReceiveAsync(TimeSpan.FromMilliseconds(800)));
    }

    [Fact]
    public async Task ClientResendIsAcknowledgedButNotProcessedTwice()
    {
        var registry = new SessionKeyRegistry();
        var issued = registry.Create();
        await using var harness = await Harness.StartAsync(registry, sessionListener: true);
        var sid = Convert.FromHexString(issued.Sid);
        registry.TryResolve(sid, out var keys);

        await harness.SendAndReceiveAsync(DiarkisPacketFactory.Combine(DatagramHeader.Pack(0, 2), sid));
        var dat = ClientDat(3, 0x0131, sid, new byte[16], keys!);
        await harness.SendAsync(dat);
        Assert.Equal(DiarkisPacketFactory.Ack(3), await harness.ReceiveAsync());

        dat[3] = 5;  // the client resends with type 5 when it missed our ACK
        await harness.SendAsync(dat);
        Assert.Equal(DiarkisPacketFactory.Ack(3), await harness.ReceiveAsync());
    }

    [Fact]
    public async Task SynWithUnknownSidIsDropped()
    {
        await using var harness = await Harness.StartAsync(new SessionKeyRegistry(), sessionListener: false);
        // Wait until the listener is bound (a directory query always gets an answer), then send the SYN.
        await harness.SendAndReceiveAsync(DiarkisPacketFactory.Combine([0, 0, 0x0C, 0x0C, 0, 0, 0, 1, 0, 16], new byte[16]));
        await harness.SendAsync(DiarkisPacketFactory.Combine(DatagramHeader.Pack(0, 2), new byte[16]));
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => harness.ReceiveAsync(TimeSpan.FromMilliseconds(300)));
    }

    [Fact]
    public async Task DirectoryReplyIsClearTextNamesTheSessionHostAndFlipsTheFlag()
    {
        await using var harness = await Harness.StartAsync(new SessionKeyRegistry(), sessionListener: false);
        var query = DiarkisPacketFactory.Combine([0, 0, 0x0C, 0x0C, 0, 0, 0, 1, 0, 16], new byte[16]);

        Assert.Equal(DirectoryResponse(0x0F, harness.Options.UdpSessionPort), await harness.SendAndReceiveAsync(query));

        query[0] = 0x0F;
        query[1] = 0x0F;
        Assert.Equal(DirectoryResponse(0, harness.Options.UdpSessionPort), await harness.SendAndReceiveAsync(query));
    }

    private static byte[] ClientDat(int sequence, ushort command, byte[] sid, byte[] plaintext, DiarkisKeySet keys) =>
        DiarkisPacketFactory.Combine(
            DatagramHeader.Pack(sequence, 3),
            CommandEnvelopeCodec.Build(command, DiarkisPacketFactory.Combine(sid, SecureEnvelope.Encrypt(plaintext, keys)), version: 0));

    private static (ushort Command, byte Status, byte[] Plaintext) Open(byte[] datagram, DiarkisKeySet keys)
    {
        var envelope = CommandEnvelopeCodec.Parse(DatagramHeader.Parse(datagram).Payload, fromClient: false);
        var result = SecureEnvelope.Decrypt(envelope.Body, keys);
        Assert.True(result.HmacOk, result.Note);
        return (envelope.Command, envelope.Status!.Value, result.Plaintext!);
    }

    private static byte[] DirectoryResponse(byte flag, int port)
    {
        var payload = Encoding.ASCII.GetBytes($"127.0.0.1:{port}");
        return DiarkisPacketFactory.Combine(
            [flag, flag, 0x0C, 0x0C, 0, 0, 0, 1, (byte)(payload.Length >> 8), (byte)payload.Length],
            payload);
    }

    private sealed class Harness : IAsyncDisposable
    {
        private readonly DiarkisUdpService _service;
        private readonly UdpClient _client;
        private readonly CancellationTokenSource _timeout = new(TimeSpan.FromSeconds(10));

        private Harness(DiarkisUdpService service, UdpClient client, BreakerOptions options)
        {
            _service = service;
            _client = client;
            Options = options;
        }

        public BreakerOptions Options { get; }
        public int ClientPort => ((IPEndPoint)_client.Client.LocalEndPoint!).Port;

        public static async Task<Harness> StartAsync(SessionKeyRegistry registry, bool sessionListener)
        {
            var options = new BreakerOptions
            {
                UdpHost = "127.0.0.1",
                UdpPort = FreeUdpPort(),
                UdpSessionPort = FreeUdpPort()
            };
            var service = new DiarkisUdpService(
                Microsoft.Extensions.Options.Options.Create(options),
                NullLogger<DiarkisUdpService>.Instance,
                sessionListener,
                registry);
            var client = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
            client.Connect(new IPEndPoint(IPAddress.Loopback, sessionListener ? options.UdpSessionPort : options.UdpPort));
            var harness = new Harness(service, client, options);
            await service.StartAsync(harness._timeout.Token);
            return harness;
        }

        public async Task SendAsync(byte[] packet) => await _client.SendAsync(packet, _timeout.Token);

        public async Task<byte[]> ReceiveAsync(TimeSpan? within = null)
        {
            using var attempt = CancellationTokenSource.CreateLinkedTokenSource(_timeout.Token);
            attempt.CancelAfter(within ?? TimeSpan.FromSeconds(2));
            return (await _client.ReceiveAsync(attempt.Token)).Buffer;
        }

        /// <summary>Retries until the listener is up (the first packets can arrive before it binds).</summary>
        public async Task<byte[]> SendAndReceiveAsync(byte[] packet)
        {
            while (true)
            {
                try
                {
                    await SendAsync(packet);
                    return await ReceiveAsync(TimeSpan.FromMilliseconds(150));
                }
                catch (OperationCanceledException) when (!_timeout.IsCancellationRequested)
                {
                }
                catch (SocketException ex) when (ex.SocketErrorCode is SocketError.ConnectionReset or SocketError.ConnectionRefused)
                {
                }
            }
        }

        public async ValueTask DisposeAsync()
        {
            await _service.StopAsync(CancellationToken.None);
            _client.Dispose();
            _timeout.Dispose();
        }
    }

    private static int FreeUdpPort()
    {
        using var udp = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        return ((IPEndPoint)udp.Client.LocalEndPoint!).Port;
    }
}
