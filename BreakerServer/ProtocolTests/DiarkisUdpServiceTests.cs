using System.Buffers.Binary;
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
    private static readonly byte[] InitMarker = [0xA0, 0x01, 0, 0];

    [Fact]
    public async Task BootstrapNatAndHeartbeatFollowCapturedSequence()
    {
        var registry = new SessionKeyRegistry();
        var port = FreeUdpPort();
        var service = CreateService(port, registry);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(8));
        using var client = new FakeClient(port, registry.Create(), timeout.Token);
        await service.StartAsync(timeout.Token);
        try
        {
            await client.SynAsync();
            var counter = new byte[] { 1, 2, 3, 4 };
            await client.InitAsync(counter);
            await client.ReceiveAckAsync(0);

            var reply = await client.ReceiveCommandAsync(0x0001);
            Assert.Equal(0, reply.Sequence);
            Assert.Equal(0, reply.Envelope.Version);
            Assert.Equal((byte)1, reply.Envelope.Status);
            Assert.Equal(
                DiarkisPacketFactory.Combine(
                    [1], counter, InitMarker,
                    Encoding.ASCII.GetBytes($"127.0.0.1:{client.Port}")),
                reply.Plaintext);

            var firstPush = await client.ReceiveCommandAsync(0x012F);
            Assert.Equal(1, firstPush.Sequence);
            Assert.Equal((byte)0xFF, firstPush.Envelope.Status);
            Assert.Equal(AddressPayload("127.0.0.1", port), firstPush.Plaintext);
            await client.AckServerAsync(0, 1);

            await client.SendCommandAsync(0x0131, new byte[16], version: 1);
            await client.ReceiveAckAsync(1);

            var cookie = SecureEnvelope.Encrypt(Encoding.ASCII.GetBytes("cookie-data"), client.Keys);
            WithDeclaredLength(cookie, 0);
            await client.SendCommandAsync(0x0130, [], version: 1, cookie);
            await client.ReceiveAckAsync(2);
            var secondPush = await client.ReceiveCommandAsync(0x012F);
            Assert.Equal(2, secondPush.Sequence);
            Assert.Equal(AddressPayload("127.0.0.1", port + 1), secondPush.Plaintext);
            await client.AckServerAsync(2);

            var heartbeat = new byte[] { 10, 11, 12, 13 };
            await client.SendCommandAsync(0x0001, DiarkisPacketFactory.Combine(heartbeat, InitMarker));
            await client.ReceiveAckAsync(3);
            var heartbeatReply = await client.ReceiveCommandAsync(0x0001);
            Assert.Equal(3, heartbeatReply.Sequence);
            Assert.Equal(heartbeat, heartbeatReply.Plaintext.AsSpan(1, 4).ToArray());
            await client.AckServerAsync(3);
        }
        finally
        {
            await service.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task TwoClientsUseIndependentKeysAndSequenceSpaces()
    {
        var registry = new SessionKeyRegistry();
        var port = FreeUdpPort();
        var service = CreateService(port, registry);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(8));
        using var first = new FakeClient(port, registry.Create(), timeout.Token);
        using var second = new FakeClient(port, registry.Create(), timeout.Token);
        await service.StartAsync(timeout.Token);
        try
        {
            await first.SynAsync();
            await second.SynAsync();
            await first.InitAsync([0xAA, 0xAA, 0xAA, 0xAA]);
            await second.InitAsync([0xBB, 0xBB, 0xBB, 0xBB]);
            await first.ReceiveAckAsync(0);
            await second.ReceiveAckAsync(0);

            var firstReply = await first.ReceiveCommandAsync(0x0001);
            var secondReply = await second.ReceiveCommandAsync(0x0001);
            Assert.Equal(0, firstReply.Sequence);
            Assert.Equal(0, secondReply.Sequence);
            Assert.NotEqual(firstReply.Plaintext, secondReply.Plaintext);
            Assert.NotEqual(first.Keys.Sid, second.Keys.Sid);
            await first.AckServerAsync(0, 1);
            await second.AckServerAsync(0, 1);

            await first.SendCommandAsync(0x0001, DiarkisPacketFactory.Combine([0xAA, 1, 1, 1], InitMarker));
            await second.SendCommandAsync(0x0001, DiarkisPacketFactory.Combine([0xBB, 1, 1, 1], InitMarker));
            await first.ReceiveAckAsync(1);
            await second.ReceiveAckAsync(1);
            var firstHeartbeat = await first.ReceiveCommandAsync(0x0001);
            var secondHeartbeat = await second.ReceiveCommandAsync(0x0001);
            Assert.Equal(2, firstHeartbeat.Sequence);
            Assert.Equal(2, secondHeartbeat.Sequence);
            Assert.Equal([0xAA, 1, 1, 1], firstHeartbeat.Plaintext.AsSpan(1, 4).ToArray());
            Assert.Equal([0xBB, 1, 1, 1], secondHeartbeat.Plaintext.AsSpan(1, 4).ToArray());
            await first.AckServerAsync(2);
            await second.AckServerAsync(2);
        }
        finally
        {
            await service.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task UnackedDatagramsRetransmitAndBatchedAckStopsThem()
    {
        var registry = new SessionKeyRegistry();
        var port = FreeUdpPort();
        var service = CreateService(port, registry, TimeSpan.FromMilliseconds(100));
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(8));
        using var client = new FakeClient(port, registry.Create(), timeout.Token);
        await service.StartAsync(timeout.Token);
        try
        {
            await client.SynAsync();
            await client.InitAsync();
            await client.ReceiveAckAsync(0);
            await client.ReceiveCommandAsync(0x0001);
            await client.ReceiveCommandAsync(0x012F);

            var retransmitted = new HashSet<int>();
            while (retransmitted.Count < 2)
            {
                var command = await client.ReceiveCommandAsync();
                if (command.PacketType == (byte)DiarkisDatagramType.Retransmit)
                    retransmitted.Add(command.Sequence);
            }
            Assert.Equal([0, 1], retransmitted.Order().ToArray());

            await client.AckServerAsync(0, 1);
            await Task.Delay(250, timeout.Token);
            await client.DrainAsync();
            await client.ExpectSilenceAsync();
        }
        finally
        {
            await service.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task FinReceivesAckWithServerNextSequence()
    {
        var registry = new SessionKeyRegistry();
        var port = FreeUdpPort();
        var service = CreateService(port, registry);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(8));
        using var client = new FakeClient(port, registry.Create(), timeout.Token);
        await service.StartAsync(timeout.Token);
        try
        {
            await client.SynAsync();
            await client.InitAsync();
            await client.ReceiveAckAsync(0);
            await client.ReceiveCommandAsync(0x0001);
            await client.ReceiveCommandAsync(0x012F);
            await client.AckServerAsync(0, 1);

            await client.FinAsync();
            await client.ReceiveAckAsync(2);
        }
        finally
        {
            await service.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task FragmentedClientInitIsReassembledBeforeDispatch()
    {
        var registry = new SessionKeyRegistry();
        var port = FreeUdpPort();
        var service = CreateService(port, registry);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(8));
        using var client = new FakeClient(port, registry.Create(), timeout.Token);
        await service.StartAsync(timeout.Token);
        try
        {
            await client.SynAsync();
            await client.SendFragmentedInitAsync();
            await client.ReceiveAckAsync(0);
            await client.ReceiveAckAsync(1);
            var reply = await client.ReceiveCommandAsync(0x0001);
            Assert.Equal([1, 2, 3, 4], reply.Plaintext.AsSpan(1, 4).ToArray());
            await client.ReceiveCommandAsync(0x012F);
            await client.AckServerAsync(0, 1);
        }
        finally
        {
            await service.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task SessionProbeRedirectsAndKeySetCarriesToRealListener()
    {
        var registry = new SessionKeyRegistry();
        var port = FreeUdpPort();
        var service = CreateService(port, registry, sessionListener: true);
        var issued = registry.Create();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(8));
        using var client = new FakeClient(port, issued, timeout.Token);
        await service.StartAsync(timeout.Token);
        try
        {
            await client.SynAsync();
            await client.InitAsync();
            await client.ReceiveAckAsync(0);
            var reply = await client.ReceiveCommandAsync(0x0001);
            Assert.Equal(0, reply.Sequence);
            await client.AckServerAsync(0);

            var request = CsmsCodec.BuildRequest(
                "100000000000000001", 3, 22001, "{\"connectionRoomId\":\"ROOM\"}");
            await client.SendCommandAsync(0x55F0, request, version: 1);
            await client.ReceiveAckAsync(1);
            var redirect = await client.ReceiveCommandAsync(0x0002);
            Assert.Equal(1, redirect.Sequence);
            Assert.Equal((byte)0xFF, redirect.Envelope.Status);
            Assert.Equal(
                DiarkisPacketFactory.Combine(
                    [0xBE, 0xEF, 0xFE, 0xED],
                    Encoding.ASCII.GetBytes("127.0.0.1:7100")),
                redirect.Plaintext);
            await client.AckServerAsync(1);
            await client.FinAsync();
            await client.ReceiveAckAsync(2);

            var realPort = FreeUdpPort();
            var realService = CreateService(realPort, registry);
            using var realClient = new FakeClient(realPort, issued, timeout.Token);
            await realService.StartAsync(timeout.Token);
            try
            {
                await realClient.SynAsync();
                await realClient.InitAsync();
                await realClient.ReceiveAckAsync(0);
                var realReply = await realClient.ReceiveCommandAsync(0x0001);
                Assert.Equal(0, realReply.Sequence);
                await realClient.ReceiveCommandAsync(0x012F);
                await realClient.AckServerAsync(0, 1);
            }
            finally
            {
                await realService.StopAsync(CancellationToken.None);
            }
        }
        finally
        {
            await service.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task FragmentedConnectionRoomCsmsTriggersRedirect()
    {
        var registry = new SessionKeyRegistry();
        var port = FreeUdpPort();
        var service = CreateService(port, registry, sessionListener: true);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(8));
        using var client = new FakeClient(port, registry.Create(), timeout.Token);
        await service.StartAsync(timeout.Token);
        try
        {
            await client.SynAsync();
            await client.InitAsync();
            await client.ReceiveAckAsync(0);
            await client.ReceiveCommandAsync(0x0001);
            await client.AckServerAsync(0);

            var request = CsmsCodec.BuildRequest(
                "100000000000000001",
                3,
                22001,
                "{\"connectionRoomId\":\"ROOM\"}");
            await client.SendFragmentedCsmsAsync(request);
            await client.ReceiveAckAsync(1);
            await client.ReceiveAckAsync(2);
            var redirect = await client.ReceiveCommandAsync(0x0002);
            Assert.Equal(1, redirect.Sequence);
            await client.AckServerAsync(1);
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

    private static DiarkisUdpService CreateService(
        int port,
        SessionKeyRegistry registry,
        TimeSpan? retransmitInterval = null,
        bool sessionListener = false) =>
        new(
            Options.Create(new BreakerOptions
            {
                UdpPort = sessionListener ? 7100 : port,
                UdpSessionPort = sessionListener ? port : port + 1,
                UdpHost = "127.0.0.1",
                AdvertisedHost = "127.0.0.1"
            }),
            NullLogger<DiarkisUdpService>.Instance,
            sessionListener,
            registry,
            retransmitInterval);

    private static byte[] AddressPayload(string host, int port)
    {
        var address = Encoding.ASCII.GetBytes($"{host}:{port}");
        return DiarkisPacketFactory.Combine(
            [(byte)(address.Length >> 8), (byte)address.Length],
            address);
    }

    private static void WithDeclaredLength(byte[] envelope, uint length) =>
        BinaryPrimitives.WriteUInt32BigEndian(envelope, length);

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

    private sealed record ServerCommand(
        int Sequence,
        byte PacketType,
        CommandEnvelope Envelope,
        byte[] Plaintext);

    private sealed class FakeClient : IDisposable
    {
        private readonly UdpClient _client;
        private readonly CancellationToken _ct;

        public FakeClient(int serverPort, SessionKeys issued, CancellationToken ct)
        {
            _ct = ct;
            Keys = SecureEnvelope.FromHex(issued.Sid, issued.Aes, issued.Iv, issued.Mac);
            _client = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
            _client.Connect(new IPEndPoint(IPAddress.Loopback, serverPort));
        }

        public DiarkisKeySet Keys { get; }
        public int Port => ((IPEndPoint)_client.Client.LocalEndPoint!).Port;
        private int Sequence { get; set; }

        public async Task SynAsync()
        {
            var syn = DiarkisPacketFactory.Combine(
                DatagramHeader.Pack(0, (byte)DiarkisDatagramType.Syn), Keys.Sid);
            while (true)
            {
                await SendRawAsync(syn);
                using var attempt = CancellationTokenSource.CreateLinkedTokenSource(_ct);
                attempt.CancelAfter(TimeSpan.FromMilliseconds(100));
                try
                {
                    var packet = await ReceiveUntilAsync(data =>
                        data.Length >= 4 && data[3] == (byte)DiarkisDatagramType.Ack,
                        attempt.Token);
                    Assert.Equal(DiarkisPacketFactory.Ack(), packet);
                    return;
                }
                catch (OperationCanceledException) when (!_ct.IsCancellationRequested)
                {
                }
                catch (SocketException ex) when (ex.SocketErrorCode == SocketError.ConnectionReset)
                {
                }
            }
        }

        public Task InitAsync(byte[]? counter = null)
        {
            counter ??= [1, 2, 3, 4];
            var publicAddress = Encoding.ASCII.GetBytes($"192.0.2.100:{Port}");
            var lanAddress = Encoding.ASCII.GetBytes($"192.168.0.100:{Port}");
            var publicLength = new byte[4];
            var lanLength = new byte[4];
            BinaryPrimitives.WriteUInt32BigEndian(publicLength, (uint)publicAddress.Length);
            BinaryPrimitives.WriteUInt32BigEndian(lanLength, (uint)lanAddress.Length);
            return SendCommandAsync(0x0001, DiarkisPacketFactory.Combine(
                counter, InitMarker, publicLength, publicAddress, lanLength, lanAddress));
        }

        public Task SendFragmentedInitAsync()
        {
            var plaintext = DiarkisPacketFactory.Combine([1, 2, 3, 4], InitMarker);
            var inner = CommandEnvelopeCodec.Build(
                0x0001,
                DiarkisPacketFactory.Combine(Keys.Sid, SecureEnvelope.Encrypt(plaintext, Keys)));
            var fragments = Fragmentation.Split(7, inner, 16);
            return SendFragmentsAsync(fragments);
        }

        public Task SendFragmentedCsmsAsync(byte[] request)
        {
            var inner = CommandEnvelopeCodec.Build(
                0x55F0,
                DiarkisPacketFactory.Combine(Keys.Sid, SecureEnvelope.Encrypt(request, Keys)),
                version: 1);
            return SendFragmentsAsync(Fragmentation.Split(8, inner, 100), 0x55F0);
        }

        public async Task SendCommandAsync(
            ushort command,
            byte[] plaintext,
            byte version = 0,
            byte[]? encrypted = null)
        {
            encrypted ??= SecureEnvelope.Encrypt(plaintext, Keys);
            var envelope = CommandEnvelopeCodec.Build(
                command,
                DiarkisPacketFactory.Combine(Keys.Sid, encrypted),
                version);
            await SendReliableAsync(envelope);
        }

        public async Task AckServerAsync(params int[] sequences)
        {
            var packet = DiarkisPacketFactory.Combine(
                DatagramHeader.Pack(sequences[0], (byte)DiarkisDatagramType.Ack), Keys.Sid);
            foreach (var sequence in sequences.Skip(1))
            {
                packet = DiarkisPacketFactory.Combine(packet,
                    DatagramHeader.Pack(sequence, (byte)DiarkisDatagramType.Ack), Keys.Sid);
            }
            await SendRawAsync(packet);
        }

        public Task FinAsync() => SendRawAsync(DiarkisPacketFactory.Combine(
            DatagramHeader.Pack(Sequence, (byte)DiarkisDatagramType.Fin), Keys.Sid));

        public async Task ReceiveAckAsync(int sequence)
        {
            var packet = await ReceiveUntilAsync(data =>
                data.Length >= 4 && data[3] == (byte)DiarkisDatagramType.Ack);
            Assert.Equal(DiarkisPacketFactory.Ack(sequence), packet);
        }

        public async Task<ServerCommand> ReceiveCommandAsync(ushort? expectedCommand = null)
        {
            while (true)
            {
                var packet = await ReceiveUntilAsync(data =>
                    data.Length >= 15 &&
                    data[3] is (byte)DiarkisDatagramType.Reliable or (byte)DiarkisDatagramType.Retransmit &&
                    data.AsSpan(4, 4).SequenceEqual(CommandEnvelopeCodec.Magic));
                var header = DatagramHeader.Parse(packet);
                var envelope = CommandEnvelopeCodec.Parse(header.Payload, fromClient: false);
                var decrypted = SecureEnvelope.Decrypt(envelope.Body, Keys);
                Assert.True(decrypted.HmacOk);
                Assert.NotNull(decrypted.Plaintext);
                if (expectedCommand is null || envelope.Command == expectedCommand)
                    return new(header.Sequence, header.PacketType, envelope, decrypted.Plaintext);
            }
        }

        public async Task DrainAsync()
        {
            using var attempt = CancellationTokenSource.CreateLinkedTokenSource(_ct);
            attempt.CancelAfter(TimeSpan.FromMilliseconds(100));
            try
            {
                while (true)
                    await _client.ReceiveAsync(attempt.Token);
            }
            catch (OperationCanceledException) when (!_ct.IsCancellationRequested)
            {
            }
        }

        public async Task ExpectSilenceAsync()
        {
            using var attempt = CancellationTokenSource.CreateLinkedTokenSource(_ct);
            attempt.CancelAfter(TimeSpan.FromMilliseconds(250));
            await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
                await _client.ReceiveAsync(attempt.Token));
        }

        private async Task SendFragmentsAsync(IReadOnlyList<byte[]> fragments, ushort command = 0x0001)
        {
            foreach (var fragment in fragments)
                await SendReliableAsync(Fragmentation.WrapClientFragment(command, Keys.Sid, fragment));
        }

        private Task SendReliableAsync(byte[] payload)
        {
            var packet = DiarkisPacketFactory.Combine(
                DatagramHeader.Pack(Sequence++, (byte)DiarkisDatagramType.Reliable), payload);
            return SendRawAsync(packet);
        }

        private Task SendRawAsync(byte[] packet) => _client.SendAsync(packet, _ct).AsTask();

        private async Task<byte[]> ReceiveUntilAsync(
            Func<byte[], bool> predicate,
            CancellationToken? cancellationToken = null)
        {
            while (true)
            {
                var packet = (await _client.ReceiveAsync(cancellationToken ?? _ct)).Buffer;
                if (predicate(packet))
                    return packet;
            }
        }

        public void Dispose() => _client.Dispose();
    }
}
