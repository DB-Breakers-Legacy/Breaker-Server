using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Text;
using BreakerServer.Configuration;
using BreakerServer.Protocol.Diarkis;
using Microsoft.Extensions.Options;

namespace BreakerServer.Network;

public sealed class DiarkisUdpService(
    IOptions<BreakerOptions> options,
    ILogger<DiarkisUdpService> logger,
    bool sessionListener = false,
    Services.SessionKeyRegistry? sessionKeys = null,
    TimeSpan? retransmitInterval = null,
    Services.MatchmakingCoordinator? coordinator = null,
    Services.ConnectionRoomCoordinator? connectionRooms = null) : BackgroundService
{
    private const int MaxFragmentChunkBytes = 1200;
    private const int MaxRememberedDatagrams = 256;

    private sealed class PendingDatagram(byte[] body, DateTimeOffset lastSent)
    {
        public byte[] Body { get; } = body;
        public DateTimeOffset LastSent { get; set; } = lastSent;
    }

    private sealed class ClientState(DiarkisKeySet keys, IPEndPoint remote)
    {
        private readonly Dictionary<int, byte[]> _recentDatagrams = [];
        private readonly Queue<int> _recentOrder = new();

        public DiarkisKeySet Keys { get; } = keys;
        public IPEndPoint Remote { get; set; } = remote;
        public int NextSequence { get; set; }
        public ushort FragmentSequence { get; set; }
        public bool Bootstrapped { get; set; }
        public string? ClientPublicAddress { get; set; }
        public string? ClientLanAddress { get; set; }
        public string? UserId { get; set; }
        public ushort Platform { get; set; }
        public DateTimeOffset NextQueueKeepalive { get; set; } = DateTimeOffset.UtcNow;
        public DateTimeOffset LastSeen { get; set; } = DateTimeOffset.UtcNow;
        public Dictionary<int, PendingDatagram> Unacked { get; } = [];
        public FragmentReassembler Fragments { get; } = new();

        public bool IsDuplicate(int sequence, ReadOnlySpan<byte> data)
        {
            if (_recentDatagrams.TryGetValue(sequence, out var previous))
            {
                if (!previous.AsSpan().SequenceEqual(data))
                    throw new InvalidDataException("Conflicting reliable datagram sequence reuse.");
                return true;
            }

            if (_recentOrder.Count >= MaxRememberedDatagrams)
                _recentDatagrams.Remove(_recentOrder.Dequeue());

            _recentDatagrams[sequence] = data.ToArray();
            _recentOrder.Enqueue(sequence);
            return false;
        }
    }

    private static readonly byte[] DirectoryCommand = [0x0C, 0x0C, 0, 0, 0, 1];
    private static readonly byte[] InitMarker = [0xA0, 0x01, 0, 0];
    private readonly BreakerOptions _options = options.Value;
    private readonly Dictionary<string, ClientState> _sessions = new(StringComparer.Ordinal);
    private readonly int _port = sessionListener ? options.Value.UdpSessionPort : options.Value.UdpPort;
    private readonly TimeSpan _retransmitInterval = retransmitInterval ?? TimeSpan.FromMilliseconds(500);
    private int _packetsSinceCleanup;

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        using var udp = new UdpClient(new IPEndPoint(IPAddress.Any, _port));
        logger.LogInformation(
            "Diarkis {Listener} UDP service listening on port {Port}",
            sessionListener ? "session" : "matching",
            _port);

        var receiveTask = udp.ReceiveAsync(stoppingToken).AsTask();
        var nextRetransmit = DateTimeOffset.UtcNow + _retransmitInterval;

        while (!stoppingToken.IsCancellationRequested)
        {
            try
            {
                var remaining = nextRetransmit - DateTimeOffset.UtcNow;
                if (remaining < TimeSpan.Zero)
                    remaining = TimeSpan.Zero;

                var delayTask = Task.Delay(remaining, stoppingToken);
                var completed = await Task.WhenAny(receiveTask, delayTask);
                if (completed != receiveTask)
                {
                    await delayTask;
                    await RetransmitAsync(udp, stoppingToken);
                    nextRetransmit = DateTimeOffset.UtcNow + _retransmitInterval;
                    continue;
                }

                var received = await receiveTask;
                receiveTask = udp.ReceiveAsync(stoppingToken).AsTask();
                await HandleAsync(udp, received.Buffer, received.RemoteEndPoint, stoppingToken);
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
                break;
            }
            catch (Exception ex)
            {
                logger.LogWarning(ex, "Failed to process Diarkis UDP traffic");
            }
        }
    }

    private async Task HandleAsync(UdpClient udp, byte[] data, IPEndPoint remote, CancellationToken ct)
    {
        if (data.Length >= 10 && data.AsSpan(2, 6).SequenceEqual(DirectoryCommand))
        {
            await HandleDirectoryAsync(udp, data, remote, ct);
            return;
        }

        if (data.Length < DatagramHeader.Length)
        {
            LogUnclassified(remote, data, "short");
            return;
        }

        CleanupStaleClients();
        var header = DatagramHeader.Parse(data);
        switch ((DiarkisDatagramType)header.PacketType)
        {
            case DiarkisDatagramType.Syn:
                await HandleSynAsync(udp, header.Payload, remote, ct);
                return;
            case DiarkisDatagramType.Ack:
                HandleAck(header.Sequence, header.Payload);
                return;
            case DiarkisDatagramType.Fin:
                await HandleFinAsync(udp, header.Payload, remote, ct);
                return;
            case DiarkisDatagramType.Reliable:
                await HandleDataAsync(udp, data, header, remote, reliable: true, ct);
                return;
            case DiarkisDatagramType.Unreliable:
                await HandleDataAsync(udp, data, header, remote, reliable: false, ct);
                return;
            default:
                LogUnclassified(remote, data, $"type={header.PacketType} seq={header.Sequence}");
                return;
        }
    }

    private async Task HandleSynAsync(
        UdpClient udp,
        ReadOnlyMemory<byte> payload,
        IPEndPoint remote,
        CancellationToken ct)
    {
        if (payload.Length != 16 || sessionKeys is null ||
            !sessionKeys.TryResolve(payload.Span, out var keys) || keys is null)
        {
            LogUnclassified(remote, payload.ToArray(), "unknown or invalid SYN session");
            return;
        }

        var sid = SidKey(payload.Span);
        if (!_sessions.TryGetValue(sid, out var state))
        {
            state = new ClientState(keys, remote);
            _sessions[sid] = state;
            logger.LogInformation("Diarkis session opened for {Remote} on {Listener} listener", remote,
                sessionListener ? "session" : "matching");
        }
        else
        {
            state.Remote = remote;
            state.LastSeen = DateTimeOffset.UtcNow;
        }

        await SendAsync(udp, DiarkisPacketFactory.Ack(), remote, ct);
    }

    private async Task HandleFinAsync(
        UdpClient udp,
        ReadOnlyMemory<byte> payload,
        IPEndPoint remote,
        CancellationToken ct)
    {
        if (payload.Length != 16 || !_sessions.TryGetValue(SidKey(payload.Span), out var state))
            return;

        await SendAsync(udp, DiarkisPacketFactory.Ack(state.NextSequence), remote, ct);
        coordinator?.RemoveSession(state.UserId);
        _sessions.Remove(SidKey(payload.Span));
        logger.LogInformation("Diarkis session closed for {Remote}", remote);
    }

    private void HandleAck(int sequence, ReadOnlyMemory<byte> payload)
    {
        if (payload.Length < 16 || !_sessions.TryGetValue(SidKey(payload.Span[..16]), out var state))
            return;

        state.LastSeen = DateTimeOffset.UtcNow;
        state.Unacked.Remove(sequence);
        var rest = payload.Span[16..];
        while (rest.Length >= 20)
        {
            if (rest[3] != (byte)DiarkisDatagramType.Ack ||
                !rest[4..20].SequenceEqual(state.Keys.Sid))
            {
                logger.LogWarning("Invalid batched ACK record for {Remote}", state.Remote);
                return;
            }

            var ackedSequence = rest[0] | (rest[1] << 8) | (rest[2] << 16);
            state.Unacked.Remove(ackedSequence);
            rest = rest[20..];
        }
    }

    private async Task HandleDataAsync(
        UdpClient udp,
        byte[] data,
        (int Sequence, byte PacketType, byte[] Payload) header,
        IPEndPoint remote,
        bool reliable,
        CancellationToken ct)
    {
        var payload = header.Payload;
        if (payload.Length < CommandEnvelopeCodec.ClientHeaderLength + 16)
            return;

        var sid = payload.AsSpan(CommandEnvelopeCodec.ClientHeaderLength, 16);
        if (!_sessions.TryGetValue(SidKey(sid), out var state))
            return;

        state.Remote = remote;
        state.LastSeen = DateTimeOffset.UtcNow;

        if (reliable)
            await SendAsync(udp, DiarkisPacketFactory.Ack(header.Sequence), remote, ct);

        if (reliable && state.IsDuplicate(header.Sequence, data))
            return;

        var fragment = Fragmentation.UnwrapClientFragment(payload, state.Keys.Sid);
        if (fragment is not null)
        {
            var complete = state.Fragments.Add(fragment.Value.Fragment);
            if (complete is null)
                return;
            payload = complete;
        }

        foreach (var envelope in CommandEnvelopeCodec.ParseAll(payload, fromClient: true))
        {
            var decrypted = SecureEnvelope.Decrypt(envelope.Body, state.Keys, fromClient: true);
            if (decrypted.Plaintext is null || decrypted.SidOk is false)
            {
                logger.LogWarning(
                    "Diarkis command decrypt failed from {Remote} cmd={Command} reason={Reason}",
                    remote,
                    envelope.Command,
                    decrypted.Note);
                continue;
            }

            await HandleCommandAsync(udp, state, envelope, decrypted.Plaintext, ct);
        }
    }

    private async Task HandleCommandAsync(
        UdpClient udp,
        ClientState state,
        CommandEnvelope envelope,
        byte[] plaintext,
        CancellationToken ct)
    {
        switch (envelope.Command)
        {
            case 0x0001:
                await HandleInitOrHeartbeatAsync(udp, state, envelope, plaintext, ct);
                return;
            case 0x0130 when !sessionListener:
                await PushServerAddressAsync(udp, state, _options.UdpSessionPort, ct);
                return;
            case 0x0131:
                return;
            case 0x2EE0:
            case 0x55F0:
                if (!await ProcessCsmsAsync(udp, state, envelope.Command, plaintext, ct))
                    return;
                if (envelope.Command == 0x55F0 && sessionListener)
                {
                    var redirect = DiarkisPacketFactory.Combine(
                        [0xBE, 0xEF, 0xFE, 0xED],
                        Encoding.ASCII.GetBytes($"{_options.UdpHost}:{_options.UdpPort}"));
                    await SendCommandAsync(udp, state, 0x0002, 0, 0xFF, redirect, ct);
                }
                return;
            default:
                if (sessionListener && connectionRooms is not null)
                    await HandleSessionRoomAsync(udp, state, envelope, plaintext, ct);
                else
                    logger.LogInformation("Unhandled Diarkis command {Command} from {Remote}", envelope.Command, state.Remote);
                return;
        }
    }

    private async Task HandleSessionRoomAsync(
        UdpClient udp,
        ClientState state,
        CommandEnvelope envelope,
        byte[] plaintext,
        CancellationToken ct)
    {
        try
        {
            var messages = connectionRooms!.HandleRoomCommand(state.UserId ?? string.Empty, envelope);
            foreach (var message in messages)
            {
                foreach (var target in _sessions.Values.Where(x => x.UserId == message.TargetUserId))
                {
                    var body = CommandEnvelopeCodec.Build(
                        message.Command,
                        SecureEnvelope.Encrypt(message.Plaintext, target.Keys),
                        message.Version,
                        message.Status);
                    if (message.Reliable)
                        await SendBodyAsync(udp, target, body, ct);
                    else
                        await SendUnreliableBodyAsync(udp, target, body, ct);
                }
            }
        }
        catch (InvalidDataException ex)
        {
            logger.LogWarning(ex, "Invalid session-room command {Command} from {Remote}", envelope.Command, state.Remote);
        }
    }

    private async Task<bool> ProcessCsmsAsync(
        UdpClient udp,
        ClientState state,
        ushort command,
        byte[] plaintext,
        CancellationToken ct)
    {
        try
        {
            var packet = CsmsCodec.Parse(plaintext, fromClient: true);
            state.UserId = packet.UserId;
            state.Platform = packet.Platform;
            IReadOnlyList<Services.OutgoingCsms>? messages = command switch
            {
                0x2EE0 when coordinator is not null => coordinator.Handle(packet),
                0x55F0 when connectionRooms is not null => connectionRooms.Handle(packet),
                _ => null
            };
            if (command == 0x55F0 && connectionRooms is not null)
                connectionRooms.SetEndpoints(packet.UserId, state.ClientPublicAddress, state.ClientLanAddress);
            if (messages is not null)
            {
                foreach (var message in messages)
                {
                    var response = CsmsCodec.BuildResponse(
                        message.TargetUserId,
                        message.TargetPlatform,
                        message.SubId,
                        message.Data,
                        push: message.Push);
                    foreach (var target in _sessions.Values.Where(x => x.UserId == message.TargetUserId))
                    {
                        await SendCommandAsync(
                            udp,
                            target,
                            command,
                            1,
                            message.Push ? (byte)0xFF : (byte)1,
                            response,
                            ct);
                    }
                }
            }
            return true;
        }
        catch (InvalidDataException ex)
        {
            logger.LogWarning(ex, "Invalid CSMS command {Command} from {Remote}", command, state.Remote);
            return false;
        }
    }

    private async Task HandleInitOrHeartbeatAsync(
        UdpClient udp,
        ClientState state,
        CommandEnvelope envelope,
        byte[] plaintext,
        CancellationToken ct)
    {
        if (plaintext.Length < InitMarker.Length + 4 || !plaintext.AsSpan(4, 4).SequenceEqual(InitMarker))
        {
            logger.LogWarning("Invalid init/heartbeat payload from {Remote}", state.Remote);
            return;
        }

        if (plaintext.Length > 8 && TryParseInitAddresses(plaintext, out var publicAddress, out var lanAddress))
        {
            state.ClientPublicAddress = publicAddress;
            state.ClientLanAddress = lanAddress;
        }

        var reply = DiarkisPacketFactory.Combine(
            [1],
            plaintext.AsSpan(0, 4).ToArray(),
            InitMarker,
            Encoding.ASCII.GetBytes($"{state.Remote.Address}:{state.Remote.Port}"));
        await SendCommandAsync(udp, state, envelope.Command, envelope.Version, 1, reply, ct);

        if (!sessionListener && !state.Bootstrapped)
        {
            state.Bootstrapped = true;
            await PushServerAddressAsync(udp, state, _options.UdpPort, ct);
        }
    }

    private Task PushServerAddressAsync(UdpClient udp, ClientState state, int port, CancellationToken ct)
    {
        var address = Encoding.ASCII.GetBytes($"{_options.UdpHost}:{port}");
        var length = new byte[2];
        BinaryPrimitives.WriteUInt16BigEndian(length, checked((ushort)address.Length));
        return SendCommandAsync(udp, state, 0x012F, 1, 0xFF,
            DiarkisPacketFactory.Combine(length, address), ct);
    }

    private async Task SendCommandAsync(
        UdpClient udp,
        ClientState state,
        ushort command,
        byte version,
        byte status,
        ReadOnlyMemory<byte> plaintext,
        CancellationToken ct)
    {
        var body = CommandEnvelopeCodec.Build(
            command,
            SecureEnvelope.Encrypt(plaintext.Span, state.Keys),
            version,
            status);
        await SendBodyAsync(udp, state, body, ct);
    }

    private async Task SendBodyAsync(UdpClient udp, ClientState state, byte[] body, CancellationToken ct)
    {
        if (body.Length > MaxFragmentChunkBytes)
        {
            var fragments = Fragmentation.Split(state.FragmentSequence++, body, MaxFragmentChunkBytes);
            foreach (var fragment in fragments)
                await SendReliableBodyAsync(udp, state, fragment, ct);
            return;
        }

        await SendReliableBodyAsync(udp, state, body, ct);
    }

    private async Task SendReliableBodyAsync(UdpClient udp, ClientState state, byte[] body, CancellationToken ct)
    {
        var sequence = state.NextSequence;
        state.NextSequence = (state.NextSequence + 1) & 0xFFFFFF;
        state.Unacked[sequence] = new PendingDatagram(body, DateTimeOffset.UtcNow);
        await SendAsync(udp,
            DiarkisPacketFactory.Combine(DatagramHeader.Pack(sequence, (byte)DiarkisDatagramType.Reliable), body),
            state.Remote,
            ct);
    }

    private async Task SendUnreliableBodyAsync(UdpClient udp, ClientState state, byte[] body, CancellationToken ct)
    {
        if (body.Length > MaxFragmentChunkBytes)
            throw new InvalidDataException("Unreliable Diarkis payload exceeds the datagram limit.");

        var sequence = state.NextSequence;
        state.NextSequence = (state.NextSequence + 1) & 0xFFFFFF;
        await SendAsync(udp,
            DiarkisPacketFactory.Combine(DatagramHeader.Pack(sequence, (byte)DiarkisDatagramType.Unreliable), body),
            state.Remote,
            ct);
    }

    private async Task RetransmitAsync(UdpClient udp, CancellationToken ct)
    {
        var now = DateTimeOffset.UtcNow;
        foreach (var state in _sessions.Values)
        {
            foreach (var pair in state.Unacked)
            {
                if (now - pair.Value.LastSent < _retransmitInterval)
                    continue;

                await SendAsync(udp,
                    DiarkisPacketFactory.Combine(
                        DatagramHeader.Pack(pair.Key, (byte)DiarkisDatagramType.Retransmit),
                        pair.Value.Body),
                    state.Remote,
                    ct);
                pair.Value.LastSent = now;
            }

            if (!sessionListener && coordinator?.IsQueued(state.UserId) == true &&
                now >= state.NextQueueKeepalive)
            {
                await SendCommandAsync(udp, state, 0x00DB, 1, 0xFF,
                    Encoding.ASCII.GetBytes("Timeout"), ct);
                state.NextQueueKeepalive = now + TimeSpan.FromSeconds(5);
            }
        }
    }

    private async Task HandleDirectoryAsync(UdpClient udp, byte[] data, IPEndPoint remote, CancellationToken ct)
    {
        var declaredLength = (data[8] << 8) | data[9];
        if (declaredLength != data.Length - 10)
        {
            LogUnclassified(remote, data, "invalid directory length");
            return;
        }

        var payload = Encoding.ASCII.GetBytes($"{_options.UdpHost}:{_options.UdpPort}");
        var response = new byte[10 + payload.Length];
        if (data[0] == 0 && data[1] == 0)
        {
            response[0] = 0x0F;
            response[1] = 0x0F;
        }
        DirectoryCommand.CopyTo(response, 2);
        response[8] = (byte)(payload.Length >> 8);
        response[9] = (byte)payload.Length;
        payload.CopyTo(response, 10);
        await SendAsync(udp, response, remote, ct);
    }

    private void CleanupStaleClients()
    {
        if (++_packetsSinceCleanup < 128)
            return;

        _packetsSinceCleanup = 0;
        var now = DateTimeOffset.UtcNow;
        coordinator?.Expire(now);
        var cutoff = now - TimeSpan.FromMinutes(2);
        foreach (var pair in _sessions.Where(x => x.Value.LastSeen < cutoff).ToArray())
        {
            coordinator?.RemoveSession(pair.Value.UserId);
            _sessions.Remove(pair.Key);
        }
    }

    private static bool TryParseInitAddresses(
        ReadOnlySpan<byte> plaintext,
        out string? publicAddress,
        out string? lanAddress)
    {
        publicAddress = null;
        lanAddress = null;
        if (plaintext.Length < 12 || !plaintext.Slice(4, 4).SequenceEqual(InitMarker))
            return false;

        var publicLength = BinaryPrimitives.ReadUInt32BigEndian(plaintext[8..12]);
        if (publicLength > int.MaxValue)
            return false;

        var lanLengthOffset = 12 + (int)publicLength;
        if (lanLengthOffset < 12 || plaintext.Length - lanLengthOffset < 4)
            return false;

        var lanLength = BinaryPrimitives.ReadUInt32BigEndian(plaintext.Slice(lanLengthOffset, 4));
        var lanOffset = lanLengthOffset + 4;
        if (lanLength > int.MaxValue || lanLength != plaintext.Length - lanOffset)
            return false;

        publicAddress = Encoding.ASCII.GetString(plaintext.Slice(12, (int)publicLength));
        lanAddress = Encoding.ASCII.GetString(plaintext.Slice(lanOffset, (int)lanLength));
        return true;
    }

    private static string SidKey(ReadOnlySpan<byte> sid) => Convert.ToHexString(sid);

    private void LogUnclassified(IPEndPoint remote, byte[] data, string reason) =>
        logger.LogInformation(
            "Diarkis unclassified packet from={Remote} bytes={Length} reason={Reason} preview={Preview}",
            remote,
            data.Length,
            reason,
            Convert.ToHexString(data.AsSpan(0, Math.Min(data.Length, 96))));

    private static Task SendAsync(UdpClient udp, byte[] data, IPEndPoint remote, CancellationToken ct) =>
        udp.SendAsync(data, remote, ct).AsTask();
}
