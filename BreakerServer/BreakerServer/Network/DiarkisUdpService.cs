using BreakerServer.Configuration;
using BreakerServer.Protocol.Diarkis;
using BreakerServer.Services;
using Microsoft.Extensions.Options;
using System.Net;
using System.Net.Sockets;
using System.Text;

namespace BreakerServer.Network;

/// <summary>
/// Diarkis RUDP peer (server side). Port of the Python reference peer (Breaker-Server src/diarkis_peer.py),
/// whose behaviour comes from decrypted captures of the official servers.
///
/// Two listeners run: the matching listener (UdpPort, handed out by battle/get_diarkis_matching_server_info)
/// and the session listener (UdpSessionPort, handed out by battle/get_connection_server_info). The session
/// listener is the Python "probe" listener: it does not push server addresses after bootstrap and answers
/// command 0x55F0 with a 0x0002 redirect.
///
/// Keys: every session's key set is issued over HTTP and stored in <see cref="SessionKeyRegistry"/>; the
/// client names its session by the 16-byte SID in clear text, and this service looks the keys up by it.
/// </summary>
public sealed class DiarkisUdpService(
    IOptions<BreakerOptions> options,
    ILogger<DiarkisUdpService> logger,
    bool sessionListener = false,
    SessionKeyRegistry? sessionKeys = null) : BackgroundService
{
    private const int SidLength = 16;
    private const int MaxSeenSequences = 4096;
    private static readonly TimeSpan RetransmitInterval = TimeSpan.FromMilliseconds(500);
    private static readonly TimeSpan IdleTimeout = TimeSpan.FromMinutes(2);
    private static readonly byte[] DirectoryCommand = [0x0C, 0x0C, 0, 0, 0, 1];
    private static readonly byte[] InitMarker = [0xA0, 0x01, 0x00, 0x00];
    private static readonly byte[] RedirectMarker = [0xBE, 0xEF, 0xFE, 0xED];

    // Command names from the client's own name table (sub_140DA1470 in the game exe).
    private const ushort CmdInitOrHeartbeat = 0x0001;           // 1   "TcpHeartbeat / UdpEcho"
    private const ushort CmdRedirect = 0x0002;                  // 2   "Reconnect"
    private const ushort CmdUdpRequestHolePunch = 0x012F;       // 303 server -> client: punch toward this address
    private const ushort CmdUdpNotifyHolePunchDone = 0x0130;    // 304 client -> server: punch done
    private const ushort CmdUdpCheckNatType = 0x0131;           // 305 client -> server (16 zero bytes)
    private const ushort CmdConnectionRoomProbe = 0x55F0;
    // Captures show exactly two hole-punch requests per matching session (UdpPort, then UdpSessionPort).
    // Answering every "done" with a new request loops forever (seen live on 2026-11-10).
    private const int HolePunchRequestsPerSession = 2;

    private sealed class Session(string sid, DiarkisKeySet keys, IPEndPoint remote)
    {
        public string Sid { get; } = sid;
        public DiarkisKeySet Keys { get; } = keys;
        public IPEndPoint Remote { get; set; } = remote;
        public DateTimeOffset LastSeen { get; set; } = DateTimeOffset.UtcNow;
        public HashSet<int> SeenClientSequences { get; } = [];
        public int NextSequence;
        public SortedDictionary<int, byte[]> Unacked { get; } = [];  // our DAT seq -> command envelope
        public FragmentReassembler Fragments { get; } = new();
        public bool Bootstrapped;
        public int HolePunchRequests;
        public string? ClientPublicAddress;
        public string? ClientLanAddress;
    }

    private readonly BreakerOptions _options = options.Value;
    private readonly int _port = sessionListener ? options.Value.UdpSessionPort : options.Value.UdpPort;
    private readonly string _name = sessionListener ? "session" : "matching";
    private readonly Dictionary<string, Session> _sessions = new(StringComparer.Ordinal);
    private readonly object _gate = new();
    private UdpClient? _udp;

    private string MatchingAddress => $"{_options.UdpHost}:{_options.UdpPort}";
    private string SessionAddress => $"{_options.UdpHost}:{_options.UdpSessionPort}";

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        if (sessionKeys is null)
        {
            logger.LogCritical("Diarkis {Listener} UDP service has no session key registry", _name);
            return;
        }

        using var udp = new UdpClient(new IPEndPoint(IPAddress.Any, _port));
        IgnoreConnectionResets(udp);
        _udp = udp;
        logger.LogInformation("Diarkis {Listener} UDP service listening on port {Port}", _name, _port);

        var retransmit = RetransmitLoopAsync(udp, stoppingToken);
        while (!stoppingToken.IsCancellationRequested)
        {
            UdpReceiveResult received;
            try
            {
                received = await udp.ReceiveAsync(stoppingToken);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (Exception ex)
            {
                logger.LogError(ex, "UDP receive failed");
                continue;
            }

            try
            {
                await HandleAsync(udp, received.Buffer, received.RemoteEndPoint, stoppingToken);
            }
            catch (Exception ex)
            {
                logger.LogWarning(ex, "Failed to process UDP packet from {Remote}", received.RemoteEndPoint);
            }
        }

        try { await retransmit; } catch (OperationCanceledException) { }
    }

    private async Task HandleAsync(UdpClient udp, byte[] data, IPEndPoint remote, CancellationToken ct)
    {
        // Clear-text 0x0C0C directory query: no RUDP header, no crypto.
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

        var (sequence, type, payload) = DatagramHeader.Parse(data);
        switch ((DiarkisPacketType)type)
        {
            case DiarkisPacketType.Syn:
                await HandleSynAsync(udp, payload, remote, ct);
                return;
            case DiarkisPacketType.Fin:
                await HandleFinAsync(udp, payload, remote, ct);
                return;
            case DiarkisPacketType.Ack:
            case DiarkisPacketType.Eack:
                HandleAck(sequence, payload);
                return;
            case DiarkisPacketType.Udp:
            case DiarkisPacketType.Dat:
            case DiarkisPacketType.Resend:  // a client DAT we did not acknowledge in time
                await HandleDatAsync(udp, sequence, (DiarkisPacketType)type, payload, remote, ct);
                return;
            default:
                LogUnclassified(remote, data, $"unknown packet type {type}");
                return;
        }
    }

    // --- connection lifecycle ---

    private async Task HandleSynAsync(UdpClient udp, byte[] payload, IPEndPoint remote, CancellationToken ct)
    {
        if (payload.Length < SidLength)
        {
            LogUnclassified(remote, payload, "SYN shorter than a SID");
            return;
        }

        var sid = Convert.ToHexString(payload, 0, SidLength).ToLowerInvariant();
        lock (_gate)
        {
            if (!_sessions.TryGetValue(sid, out var session))
            {
                if (!sessionKeys!.TryResolve(payload.AsSpan(0, SidLength), out var keys) || keys is null)
                {
                    logger.LogWarning("Diarkis {Listener} SYN with unknown or expired SID {Sid} from {Remote}: dropped",
                        _name, sid, remote);
                    return;
                }

                _sessions[sid] = new Session(sid, keys, remote);
                logger.LogInformation("Diarkis {Listener} session open sid={Sid} from={Remote}", _name, sid, remote);
            }
            else
            {
                session.Remote = remote;
                session.LastSeen = DateTimeOffset.UtcNow;
            }
        }

        await SendAsync(udp, DiarkisPacketFactory.Ack(0), remote, ct);  // bare ACK, sequence 0
    }

    private async Task HandleFinAsync(UdpClient udp, byte[] payload, IPEndPoint remote, CancellationToken ct)
    {
        if (payload.Length < SidLength)
            return;

        var sid = Convert.ToHexString(payload, 0, SidLength).ToLowerInvariant();
        Session? session;
        lock (_gate)
        {
            if (!_sessions.Remove(sid, out session))
                return;
        }

        // Bare ACK carrying our next DAT sequence (captures: teardown).
        await SendAsync(udp, DiarkisPacketFactory.Ack(session.NextSequence), remote, ct);
        logger.LogInformation("Diarkis {Listener} session closed sid={Sid}", _name, sid);
    }

    private void HandleAck(int sequence, byte[] payload)
    {
        // Header sequence = first acknowledged server DAT; payload = SID, then 20-byte records
        // [3-byte LE sequence][type][SID] for any further acknowledged sequences.
        if (payload.Length < SidLength)
            return;

        var sid = Convert.ToHexString(payload, 0, SidLength).ToLowerInvariant();
        lock (_gate)
        {
            if (!_sessions.TryGetValue(sid, out var session))
                return;

            session.LastSeen = DateTimeOffset.UtcNow;
            session.Unacked.Remove(sequence);
            var sidBytes = payload.AsSpan(0, SidLength);
            for (var offset = SidLength; payload.Length - offset >= 20; offset += 20)
            {
                if (!payload.AsSpan(offset + 4, SidLength).SequenceEqual(sidBytes))
                    break;
                session.Unacked.Remove(payload[offset] | (payload[offset + 1] << 8) | (payload[offset + 2] << 16));
            }
        }
    }

    private async Task HandleDatAsync(
        UdpClient udp,
        int sequence,
        DiarkisPacketType type,
        byte[] payload,
        IPEndPoint remote,
        CancellationToken ct)
    {
        var headerLength = CommandEnvelopeCodec.ClientHeaderLength;
        if (payload.Length < headerLength + SidLength || !payload.AsSpan(0, 4).SequenceEqual(CommandEnvelopeCodec.Magic))
        {
            LogUnclassified(remote, payload, $"{type} without a command envelope");
            return;
        }

        var sid = Convert.ToHexString(payload, headerLength, SidLength).ToLowerInvariant();
        Session? session;
        bool duplicate;
        lock (_gate)
        {
            if (!_sessions.TryGetValue(sid, out session))
            {
                logger.LogWarning("Diarkis {Listener} {Type} for unknown session sid={Sid} from {Remote}", _name, type, sid, remote);
                return;
            }

            session.Remote = remote;
            session.LastSeen = DateTimeOffset.UtcNow;
            if (session.SeenClientSequences.Count >= MaxSeenSequences)
                session.SeenClientSequences.Clear();
            duplicate = !session.SeenClientSequences.Add(sequence);
        }

        // Every client DAT gets a bare ACK echoing its sequence; a repeat is only re-acknowledged.
        await SendAsync(udp, DiarkisPacketFactory.Ack(sequence), remote, ct);
        if (duplicate)
            return;

        // Fragmented message: envelope header, SID, then FF FE FD FC fragment header and a chunk of the blob.
        if (payload.Length >= headerLength + SidLength + 4 &&
            payload.AsSpan(headerLength + SidLength, 4).SequenceEqual(Fragmentation.Magic))
        {
            var unwrapped = Fragmentation.UnwrapClientFragment(payload, payload.AsSpan(headerLength, SidLength));
            if (unwrapped is null)
                return;

            var (fragmentCommand, fragment) = unwrapped.Value;
            byte[]? blob;
            lock (_gate)
                blob = session.Fragments.Add(fragment);
            if (blob is not null)
                await HandleSecureCommandAsync(udp, session, fragmentCommand, payload[4], blob, remote, ct);
            return;
        }

        foreach (var envelope in CommandEnvelopeCodec.ParseAll(payload, fromClient: true))
        {
            if (envelope.Body.Length < SidLength)
                continue;
            await HandleSecureCommandAsync(udp, session, envelope.Command, envelope.Version, envelope.Body[SidLength..], remote, ct);
        }
    }

    private async Task HandleSecureCommandAsync(
        UdpClient udp,
        Session session,
        ushort command,
        byte version,
        byte[] blob,
        IPEndPoint remote,
        CancellationToken ct)
    {
        var result = SecureEnvelope.Decrypt(blob, session.Keys);
        if (result.Plaintext is null)
        {
            logger.LogWarning("Diarkis {Listener} decrypt failed sid={Sid} cmd=0x{Command:X4}: {Note}",
                _name, session.Sid, command, result.Note);
            return;
        }

        var plaintext = result.Plaintext;
        logger.LogInformation("Diarkis {Listener} cmd=0x{Command:X4} ver={Version} bytes={Length} sid={Sid}{Note}",
            _name, command, version, plaintext.Length, session.Sid,
            result.Note.Length == 0 ? string.Empty : $" note={result.Note}");

        switch (command)
        {
            case CmdInitOrHeartbeat:
                await HandleInitOrHeartbeatAsync(udp, session, version, plaintext, remote, ct);
                return;
            case CmdUdpNotifyHolePunchDone when !sessionListener:
                // After the first punch (toward UdpPort) ask for the second (toward UdpSessionPort); then stop.
                if (session.HolePunchRequests < HolePunchRequestsPerSession)
                    await RequestHolePunchAsync(udp, session, SessionAddress, ct);
                return;
            case CmdUdpNotifyHolePunchDone:
                return;
            case CmdUdpCheckNatType:
                return;  // 16 zero bytes; the ACK was already sent
            case CmdConnectionRoomProbe when sessionListener:
                await SendDatAsync(udp, session, CmdRedirect, 0, 0xFF,
                    [.. RedirectMarker, .. Encoding.ASCII.GetBytes(MatchingAddress)], ct);
                return;
            default:
                logger.LogInformation(
                    "Diarkis {Listener} unhandled cmd=0x{Command:X4} ver={Version} bytes={Length} preview={Preview}",
                    _name, command, version, plaintext.Length,
                    Convert.ToHexString(plaintext.AsSpan(0, Math.Min(plaintext.Length, 96))));
                return;
        }
    }

    private async Task HandleInitOrHeartbeatAsync(
        UdpClient udp,
        Session session,
        byte version,
        byte[] plaintext,
        IPEndPoint remote,
        CancellationToken ct)
    {
        // Init:      [4-byte ms counter][A0 01 00 00][BE32 len][public address][BE32 len][LAN address]
        // Heartbeat: [4-byte ms counter][A0 01 00 00]
        if (plaintext.Length > 8 && TryParseInitAddresses(plaintext, out var publicAddress, out var lanAddress))
        {
            session.ClientPublicAddress = publicAddress;
            session.ClientLanAddress = lanAddress;
            logger.LogInformation("Diarkis {Listener} client addresses public={Public} lan={Lan}", _name, publicAddress, lanAddress);
        }

        // Reply: [01][counter echo][A0 01 00 00][the address the server sees]
        var counter = plaintext.AsSpan(0, Math.Min(4, plaintext.Length)).ToArray();
        byte[] reply = [0x01, .. counter, .. InitMarker, .. Encoding.ASCII.GetBytes($"{remote.Address}:{remote.Port}")];
        await SendDatAsync(udp, session, CmdInitOrHeartbeat, version, 0x01, reply, ct);

        if (!sessionListener && !session.Bootstrapped)
        {
            session.Bootstrapped = true;
            await RequestHolePunchAsync(udp, session, MatchingAddress, ct);
        }
    }

    private Task RequestHolePunchAsync(UdpClient udp, Session session, string address, CancellationToken ct)
    {
        session.HolePunchRequests++;
        var text = Encoding.ASCII.GetBytes(address);
        byte[] request = [(byte)(text.Length >> 8), (byte)text.Length, .. text];
        return SendDatAsync(udp, session, CmdUdpRequestHolePunch, 1, 0xFF, request, ct);
    }

    private static bool TryParseInitAddresses(byte[] plaintext, out string? publicAddress, out string? lanAddress)
    {
        publicAddress = lanAddress = null;
        try
        {
            if (!plaintext.AsSpan(4, 4).SequenceEqual(InitMarker))
                return false;
            var publicLength = (int)System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(plaintext.AsSpan(8, 4));
            publicAddress = Encoding.ASCII.GetString(plaintext, 12, publicLength);
            var offset = 12 + publicLength;
            var lanLength = (int)System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(plaintext.AsSpan(offset, 4));
            lanAddress = Encoding.ASCII.GetString(plaintext, offset + 4, lanLength);
            return true;
        }
        catch (ArgumentException)
        {
            return false;
        }
    }

    // --- clear-text 0x0C0C directory ---

    private async Task HandleDirectoryAsync(UdpClient udp, byte[] data, IPEndPoint remote, CancellationToken ct)
    {
        var declaredLength = (data[8] << 8) | data[9];
        if (declaredLength != data.Length - 10)
        {
            LogUnclassified(remote, data, "invalid directory length");
            return;
        }

        // Answer with the session host; the flag flips between query (00 00) and answer (0F 0F).
        var payload = Encoding.ASCII.GetBytes(SessionAddress);
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

    // --- sending ---

    private Task SendDatAsync(
        UdpClient udp,
        Session session,
        ushort command,
        byte version,
        byte status,
        byte[] plaintext,
        CancellationToken ct)
    {
        var envelope = DiarkisPacketFactory.BuildEnvelope(command, version, status, plaintext, session.Keys);
        int sequence;
        IPEndPoint remote;
        lock (_gate)
        {
            sequence = session.NextSequence;
            session.NextSequence = (session.NextSequence + 1) & 0xFFFFFF;
            session.Unacked[sequence] = envelope;
            remote = session.Remote;
        }

        logger.LogInformation("Diarkis {Listener} TX cmd=0x{Command:X4} seq={Sequence} bytes={Length} sid={Sid}",
            _name, command, sequence, plaintext.Length, session.Sid);
        return SendAsync(udp, DiarkisPacketFactory.Combine(DatagramHeader.Pack(sequence, (byte)DiarkisPacketType.Dat), envelope), remote, ct);
    }

    private async Task RetransmitLoopAsync(UdpClient udp, CancellationToken ct)
    {
        // Unacknowledged DATs are resent as type 5 with the same sequence and body every 0.5 s,
        // no backoff (captures: teardown). Idle sessions are dropped.
        using var timer = new PeriodicTimer(RetransmitInterval);
        while (await timer.WaitForNextTickAsync(ct))
        {
            List<(byte[] Packet, IPEndPoint Remote)> resend = [];
            lock (_gate)
            {
                var now = DateTimeOffset.UtcNow;
                foreach (var (sid, session) in _sessions.ToArray())
                {
                    if (now - session.LastSeen > IdleTimeout)
                    {
                        _sessions.Remove(sid);
                        logger.LogInformation("Diarkis {Listener} session timed out sid={Sid}", _name, sid);
                        continue;
                    }

                    foreach (var (sequence, envelope) in session.Unacked)
                        resend.Add((DiarkisPacketFactory.Combine(DatagramHeader.Pack(sequence, (byte)DiarkisPacketType.Resend), envelope), session.Remote));
                }
            }

            foreach (var (packet, remote) in resend)
            {
                try { await SendAsync(udp, packet, remote, ct); }
                catch (SocketException ex) { logger.LogDebug(ex, "Resend to {Remote} failed", remote); }
            }
        }
    }

    private static Task SendAsync(UdpClient udp, byte[] data, IPEndPoint remote, CancellationToken ct) =>
        udp.SendAsync(data, remote, ct).AsTask();

    private static void IgnoreConnectionResets(UdpClient udp)
    {
        // On Windows an ICMP "port unreachable" from a closed client makes the next ReceiveAsync throw;
        // SIO_UDP_CONNRESET off keeps one gone client from disturbing the others.
        if (!OperatingSystem.IsWindows())
            return;
        const int SioUdpConnReset = unchecked((int)0x9800000C);
        try { udp.Client.IOControl(SioUdpConnReset, [0, 0, 0, 0], null); }
        catch (SocketException) { }
    }

    private void LogUnclassified(IPEndPoint remote, byte[] data, string reason) =>
        logger.LogInformation(
            "Diarkis {Listener} unclassified packet from={Remote} bytes={Length} reason={Reason} preview={Preview}",
            _name,
            remote,
            data.Length,
            reason,
            Convert.ToHexString(data.AsSpan(0, Math.Min(data.Length, 96))));
}
