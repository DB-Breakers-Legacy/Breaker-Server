using MessagePack;
using BreakerServer.Configuration;
using BreakerServer.Protocol.Diarkis;
using BreakerServer.Protocol.Game;
using System.Text.Json;
using System.Text.Json.Nodes;
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
    private const ushort CmdPracticeRoom = 32000;                // 0x7D00, game messages 32xxx -> 33xxx
    private const ushort CmdRoomRegister = 115;                  // 0x0073, Diarkis built-in room command (ver 1)
    private const ushort CmdPracticeConnection = 42000;         // 0xA410, connection room 42xxx -> 43xxx (session listener)
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
        public Dictionary<int, int> ResendCounts { get; } = [];
        public string? PracticeRoomId;
        public JsonObject? PracticeCreate;   // the player's 32000 request body (name, ranks, sdpData, ...)
        public JsonObject? PracticeDesire;   // the player's 32012 request body (desiredRole, desiredStageId)
        public string? PracticeSdp;          // latest sdpData from 32005 (member data set)
        public bool PracticeTrial;           // room set to Trial Match (32026 rivalBot 0): fill it with bot members
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
            if (session.Unacked.Remove(sequence, out var acked))
                LogAcked(session, sequence, acked);
            var sidBytes = payload.AsSpan(0, SidLength);
            for (var offset = SidLength; payload.Length - offset >= 20; offset += 20)
            {
                if (!payload.AsSpan(offset + 4, SidLength).SequenceEqual(sidBytes))
                    break;
                var extra = payload[offset] | (payload[offset + 1] << 8) | (payload[offset + 2] << 16);
                if (session.Unacked.Remove(extra, out var extraAcked))
                    LogAcked(session, extra, extraAcked);
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
            case CmdPracticeRoom:
                await HandlePracticeRoomAsync(udp, session, version, plaintext, ct);
                return;
            case CmdPracticeConnection:
                await HandlePracticeConnectionAsync(udp, session, version, plaintext, ct);
                return;
            case CmdRoomRegister when version == 1:
                // Diarkis built-in RoomRegister (ver 1, cmd 115): u32 roomType, then length-prefixed
                // roomID, roomName and metadata JSON (live: type 0, "<room>-conn", the player name,
                // {"rule","ver","created_at","uid","platform"}). The client's Room response switch
                // (sub_140D8DEF0, case 105 = cmd 115) raises the register event (sub_140D97200) with
                // success = status 1 and any payload. Unanswered, the client stayed on "establishing
                // the connection" (live 2026-11-10).
                CaptureCommand(command, version, plaintext);
                logger.LogInformation("Diarkis {Listener} RoomRegister -> success", _name);
                await SendDatAsync(udp, session, CmdRoomRegister, 1, 0x01, [], ct);
                return;
            default:
                logger.LogInformation(
                    "Diarkis {Listener} unhandled cmd=0x{Command:X4} ver={Version} bytes={Length} preview={Preview}",
                    _name, command, version, plaintext.Length,
                    Convert.ToHexString(plaintext.AsSpan(0, Math.Min(plaintext.Length, 96))));
                CaptureCommand(command, version, plaintext);
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

    // Research aid: with BREAKER_CAPTURE_DIR set, unhandled commands are saved whole (decrypted).
    private void CaptureCommand(ushort command, byte version, byte[] plaintext)
    {
        var directory = Environment.GetEnvironmentVariable("BREAKER_CAPTURE_DIR");
        if (string.IsNullOrWhiteSpace(directory))
            return;
        try
        {
            Directory.CreateDirectory(directory);
            File.WriteAllBytes(Path.Combine(directory,
                $"{DateTime.Now:HHmmss}_udp_{_name}_cmd{command:X4}_v{version}_{Guid.NewGuid():N}.bin"), plaintext);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            logger.LogWarning(ex, "Command capture failed");
        }
    }

    // --- practice room (first cut, 2026-11-10) ---
    // Request ids 32xxx, replies 33xxx (= request + 1000), fields from docs/protocol MATCHMAKING_PROTOCOL.md
    // section 5/6. Replies go back as the same Diarkis command, status 1, no 4-byte prefix (to be confirmed live).
    private int _roomCounter;

    private async Task HandlePracticeRoomAsync(UdpClient udp, Session session, byte version, byte[] plaintext, CancellationToken ct)
    {
        CaptureCommand(CmdPracticeRoom, version, plaintext);
        GameMessage request;
        try
        {
            request = GameMessage.Parse(plaintext);
        }
        catch (Exception ex) when (ex is InvalidDataException or MessagePackSerializationException)
        {
            logger.LogWarning(ex, "Diarkis {Listener} practice: unreadable game message", _name);
            return;
        }

        logger.LogInformation("Practice request id={Id} serial={Serial} headerId={HeaderId} data={Data}",
            request.Id, request.Serial, request.HeaderMessageId, request.Data);

        JsonObject body;
        try { body = JsonNode.Parse(request.Data) as JsonObject ?? []; }
        catch (JsonException) { body = []; }

        var reply = new JsonObject { ["status"] = 0 };
        switch (request.Id)
        {
            case 32000: // PracticeRoomCreate -> 33000
            case 32001: // PracticeRoomJoin -> 33001
                session.PracticeRoomId ??= $"practice{Interlocked.Increment(ref _roomCounter):D6}";
                session.PracticeCreate = body;
                reply["roomId"] = session.PracticeRoomId;
                reply["roomKey"] = "0000";
                reply["roomData"] = new JsonObject { ["gameRule"] = body["gameRule"]?.GetValue<int>() ?? 0 };
                reply["rivalBot"] = 1;
                reply["normalRule"] = 1;
                reply["coolDown"] = 1;
                break;
            case 32006: // PracticeRoomDataGet -> 33006
                reply["roomData"] = new JsonObject { ["gameRule"] = 0 };
                break;
            case 32008: // PracticeRoomOwnerGet -> 33008
                reply["roomId"] = session.PracticeRoomId ?? string.Empty;
                break;
            case 32017: // PracticeRoomOpen -> 33017
                reply["battleRoomId"] = $"{session.PracticeRoomId ?? "practice"}-battle";
                break;
        }

        var replyId = request.Id + 1000;
        var json = reply.ToJsonString();
        logger.LogInformation("Practice reply id={Id} data={Data}", replyId, json);
        await SendDatAsync(udp, session, CmdPracticeRoom, version, 0x01, request.Reply(replyId, json), ct);

        // Notifies go through the same response path as the replies: the client's decoder queues both by
        // message id.
        //
        // 32101 member list. After the role request, Join state 22 (0x140815DA0 case 21) waits until the
        // room member list (MatchingModule+0x208, filled from this notify) holds the player's own userId
        // (sub_140806E50 compares it with SysCosmos+0xF8, the login userId). Reader: room id variant of
        // sub_140780D40 -> roomId, roomOwnerUserId, roomMemberList, total, serial (all required);
        // member sub_1407A0420 -> userId, userData, sdpData; userData sub_1407A07C0 -> userName, userRank,
        // userRivalRank, desiredRole, desiredStageId, platform (all required). Live test 2026-11-10.
        if (request.Id == 32012)
        {
            session.PracticeDesire = body;
            await SendPracticeMembersAsync(udp, session, version, request, ct);
        }

        // 32005 member data set: the client sends its current sdpData (e.g. "able_rematch":true, which the
        // room creation sdpData had as false). The lobby menu (sub_1409C0070) shows start only when every
        // member has sdp able_rematch (entry +0x1A0 = sdp +0x110, parser sub_140800990) and the can-open
        // flag (+0x1BA, set by 32120); otherwise MenuMatchCasual+0x88 = 3 and start is hidden (live
        // 2026-11-10). So store it, re-send the member list, then 32120 again.
        if (request.Id == 32005)
        {
            if (body["sdpData"]?.GetValue<string>() is { Length: > 0 } sdp)
                session.PracticeSdp = sdp;
            await SendPracticeMembersAsync(udp, session, version, request, ct);
            await SendPracticeCanOpenAsync(udp, session, version, request, ct);
        }

        // After the owner sets the practice rules (32026): echo them to the room (32126 ChangePracticeRule)
        // and tell the owner the room may be opened (32120 ProtocolNotifyPracticeCanOpen {roomId};
        // PracticeMatchingModule slot 74 sub_140802F30 sets +0x1BA on every member entry).
        if (request.Id == 32026)
        {
            var rule = new JsonObject
            {
                ["roomId"] = session.PracticeRoomId ?? string.Empty,
                ["rivalBot"] = body["rivalBot"]?.GetValue<int>() ?? 1,
                ["normalRule"] = body["normalRule"]?.GetValue<int>() ?? 0,
                ["coolDown"] = body["coolDown"]?.GetValue<int>() ?? 0,
            };
            var ruleJson = rule.ToJsonString();
            logger.LogInformation("Practice push id=32126 data={Data}", ruleJson);
            await SendDatAsync(udp, session, CmdPracticeRoom, version, 0x01, request.Reply(32126, ruleJson), ct);

            // Trial Match = the practice room with rivalBot 0 / normalRule 1 (live 2026-11-11). The room menu
            // (MenuMatchCasual, sub_1409C0070 / vfunc_4_2) only allows start when the member list holds 8
            // ("You need eight players for a trial match."), so the room is filled with 7 bot members.
            session.PracticeTrial = (body["rivalBot"]?.GetValue<int>() ?? 1) == 0;
            await SendPracticeMembersAsync(udp, session, version, request, ct);
            await SendPracticeCanOpenAsync(udp, session, version, request, ct);
        }

        // Practice against the bot has no other players to wait for: once the room is opened (32017),
        // tell the client the match is found (32105 ProtocolNotifyPracticeRoomOnMatchingComplete).
        // Rules (MATCHMAKING_PROTOCOL.md section 8): all five keys present; roleList entries {userId, role}
        // for members the client knows (here only the player; Possible_ the bot is added by the client from
        // rivalBot); stageId must be a stage the client knows (we echo desiredStageId, e.g. "1000").
        // The client accepts it only in matching states 0/14/15, so it goes out after a short delay.
        if (request.Id == 32017)
        {
            // 32118 ProtocolNotifyPracticeOpenRoom {roomId}: the Open module (0x14081E030, state 4) waits
            // for both the 33017 reply (+0x2C) and this notify (+0x2D); after 30 s without both it fails
            // with error 1402 (client showed 330001402, "the room was not made public"; live 2026-11-10).
            var openJson = new JsonObject { ["roomId"] = session.PracticeRoomId ?? string.Empty }.ToJsonString();
            logger.LogInformation("Practice push id=32118 data={Data}", openJson);
            await SendDatAsync(udp, session, CmdPracticeRoom, version, 0x01, request.Reply(32118, openJson), ct);

            var desire = session.PracticeDesire ?? [];
            var role = desire["desiredRole"]?.GetValue<int>() ?? 1;
            var stageId = desire["desiredStageId"]?.GetValue<string>() ?? "1000";
            if (stageId == "unselected")
                stageId = "1000";
            var matched = new JsonObject
            {
                ["roleList"] = TrialRoleList(session, request.UserId, role),
                ["battleRoomId"] = $"{session.PracticeRoomId ?? "practice"}-battle",
                ["stageId"] = stageId,
                ["rivalPriorityPoint"] = 0,
                ["patrollerPriorityPoint"] = 0,
            };
            var matchedJson = matched.ToJsonString();
            _ = Task.Run(async () =>
            {
                await Task.Delay(TimeSpan.FromSeconds(1.5), ct);
                logger.LogInformation("Practice push id=32105 data={Data}", matchedJson);
                await SendDatAsync(udp, session, CmdPracticeRoom, version, 0x01, request.Reply(32105, matchedJson), ct);
            }, ct);
        }
    }

    // --- practice connection room (first cut, 2026-11-10) ---
    // After match found the client asks battle/get_connection_server_info, connects to the session listener
    // and sends Diarkis command 42000 (0xA410) with CSMS game messages: 42000 CreateConnectionRoom
    // {connectionRoomId} (live: the practice roomId) -> 43000 {status, diarkisRoomId}; 42001 Join -> 43001;
    // 42004 UpdateMatchedSdp {connectionRoomId, sdpData} -> 43004; 42005 CheckBattleFinish -> 43005
    // {status, userList}. Unanswered 42000 ended in error 330001402 ("room was not made public").
    // After the create / join reply, 42100 BattleRoomAllJoined {roomMemberList: [{userId}]} (the bot is not
    // a member).
    private async Task HandlePracticeConnectionAsync(UdpClient udp, Session session, byte version, byte[] plaintext, CancellationToken ct)
    {
        CaptureCommand(CmdPracticeConnection, version, plaintext);
        GameMessage request;
        try
        {
            request = GameMessage.Parse(plaintext);
        }
        catch (Exception ex) when (ex is InvalidDataException or MessagePackSerializationException)
        {
            logger.LogWarning(ex, "Diarkis {Listener} connection room: unreadable game message", _name);
            return;
        }

        logger.LogInformation("Connection request id={Id} serial={Serial} data={Data}", request.Id, request.Serial, request.Data);

        JsonObject body;
        try { body = JsonNode.Parse(request.Data) as JsonObject ?? []; }
        catch (JsonException) { body = []; }

        var reply = new JsonObject { ["status"] = 0 };
        switch (request.Id)
        {
            case 42000:
            case 42001:
                reply["diarkisRoomId"] = $"{body["connectionRoomId"]?.GetValue<string>() ?? "practice"}-conn";
                break;
            case 42005:
                reply["userList"] = new JsonArray(new JsonObject { ["userId"] = request.UserId, ["isBattleFinished"] = false });
                break;
        }

        var replyId = request.Id + 1000;
        var json = reply.ToJsonString();
        logger.LogInformation("Connection reply id={Id} data={Data}", replyId, json);
        await SendDatAsync(udp, session, CmdPracticeConnection, version, 0x01, request.Reply(replyId, json), ct);

        // 42100 sets Matching+0x120 (optional, flag +0x140; slot 81 0x140819DB0). Matching state 4 waits for
        // +0x1D0 (43000 reply) and +0x140, and only then moves on to the SDP update (42004, state 6), so the
        // notify goes right after the create / join reply (live 2026-11-10: stuck in state 4).
        if (request.Id is 42000 or 42001)
        {
            var joined = new JsonObject
            {
                ["roomMemberList"] = new JsonArray(new JsonObject { ["userId"] = request.UserId }),
            }.ToJsonString();
            logger.LogInformation("Connection push id=42100 data={Data}", joined);
            await SendDatAsync(udp, session, CmdPracticeConnection, version, 0x01, request.Reply(42100, joined), ct);
        }
    }

    private async Task SendPracticeMembersAsync(
        UdpClient udp, Session session, byte version, GameMessage request, CancellationToken ct)
    {
        var create = session.PracticeCreate ?? [];
        var desire = session.PracticeDesire ?? [];
        var member = new JsonObject
        {
            ["userId"] = request.UserId,
            ["userData"] = new JsonObject
            {
                ["userName"] = create["userName"]?.GetValue<string>() ?? "Player",
                ["userRank"] = create["userRank"]?.GetValue<int>() ?? 1,
                ["userRivalRank"] = create["userRivalRank"]?.GetValue<int>() ?? 1,
                ["desiredRole"] = desire["desiredRole"]?.GetValue<int>() ?? 1,
                ["desiredStageId"] = desire["desiredStageId"]?.GetValue<string>() ?? "unselected",
                ["platform"] = 3,
            },
            ["sdpData"] = session.PracticeSdp ?? create["sdpData"]?.GetValue<string>() ?? string.Empty,
        };
        var list = new JsonArray(member);
        if (session.PracticeTrial)
        {
            var playerRole = desire["desiredRole"]?.GetValue<int>() ?? 1;
            foreach (var bot in TrialBots(playerRole))
                list.Add(bot.Member);
        }

        var members = new JsonObject
        {
            ["roomId"] = session.PracticeRoomId ?? string.Empty,
            ["roomOwnerUserId"] = request.UserId,
            ["roomMemberList"] = list,
            // The list arrives in pages: total = number of pages, serial = this page (1-based). The client
            // (sub_140806A90) keeps a slot per page and applies the list once all pages are in; a serial
            // above total is dropped. Earlier we counted serial up (2, 3, ...) and every list after the
            // first was ignored (live 2026-11-10). One page: always total 1, serial 1.
            ["total"] = 1,
            ["serial"] = 1,
        };
        var json = members.ToJsonString();
        logger.LogInformation("Practice push id=32101 data={Data}", json);
        await SendDatAsync(udp, session, CmdPracticeRoom, version, 0x01, request.Reply(32101, json), ct);
    }

    // The 7 bot members of a Trial Match room. One wants to be the Raider unless the player does (desiredRole:
    // 0 Raider / 1 Survivor, MATCHMAKING_PROTOCOL.md section 3); the rest are survivors. Member layout as read
    // by the client: userId, userData {userName, userRank, userRivalRank, desiredRole, desiredStageId,
    // platform}, sdpData (JSON text; able_rematch true or the menu blocks start).
    private static IEnumerable<(string UserId, int Role, JsonObject Member)> TrialBots(int playerRole)
    {
        for (var i = 1; i <= 7; i++)
        {
            var userId = $"BOT{i:D29}";
            var name = $"BOT {i}";
            var role = i == 1 && playerRole != 0 ? 0 : 1;
            var sdp = new JsonObject
            {
                ["player_level"] = 1,
                ["p2p_room_id"] = string.Empty,
                ["p2p_uid"] = userId,
                ["platform"] = 3,
                ["pf_uid"] = "0",
                ["patroller_name"] = role == 0 ? "RAIDER" : "SURVIVOR",
                ["account_name"] = name,
                ["able_rematch"] = true,
                ["prac_update"] = false,
            };
            var member = new JsonObject
            {
                ["userId"] = userId,
                ["userData"] = new JsonObject
                {
                    ["userName"] = name,
                    ["userRank"] = 1,
                    ["userRivalRank"] = 1,
                    ["desiredRole"] = role,
                    ["desiredStageId"] = "unselected",
                    ["platform"] = 3,
                },
                ["sdpData"] = sdp.ToJsonString(),
            };
            yield return (userId, role, member);
        }
    }

    // Match-found role list: the player, plus in a Trial Match the 7 bot members with their roles.
    private static JsonArray TrialRoleList(Session session, string playerUserId, int playerRole)
    {
        var roles = new JsonArray(new JsonObject { ["userId"] = playerUserId, ["role"] = playerRole });
        if (session.PracticeTrial)
            foreach (var bot in TrialBots(playerRole))
                roles.Add(new JsonObject { ["userId"] = bot.UserId, ["role"] = bot.Role });
        return roles;
    }

    private async Task SendPracticeCanOpenAsync(
        UdpClient udp, Session session, byte version, GameMessage request, CancellationToken ct)
    {
        var json = new JsonObject { ["roomId"] = session.PracticeRoomId ?? string.Empty }.ToJsonString();
        logger.LogInformation("Practice push id=32120 data={Data}", json);
        await SendDatAsync(udp, session, CmdPracticeRoom, version, 0x01, request.Reply(32120, json), ct);
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
                    {
                        resend.Add((DiarkisPacketFactory.Combine(DatagramHeader.Pack(sequence, (byte)DiarkisPacketType.Resend), envelope), session.Remote));
                        var count = session.ResendCounts.TryGetValue(sequence, out var c) ? c + 1 : 1;
                        session.ResendCounts[sequence] = count;
                        if (count <= 3 || count % 20 == 0)
                            logger.LogInformation("Diarkis {Listener} RESEND seq={Sequence} cmd=0x{Command:X4} attempt={Count} sid={Sid}",
                                _name, sequence, EnvelopeCommand(envelope), count, sid);
                    }
                }
            }

            foreach (var (packet, remote) in resend)
            {
                try { await SendAsync(udp, packet, remote, ct); }
                catch (SocketException ex) { logger.LogDebug(ex, "Resend to {Remote} failed", remote); }
            }
        }
    }

    private static int EnvelopeCommand(byte[] envelope) =>
        envelope.Length >= 10 ? (envelope[8] << 8) | envelope[9] : -1;

    private void LogAcked(Session session, int sequence, byte[] envelope)
    {
        var command = EnvelopeCommand(envelope);
        if (command != CmdInitOrHeartbeat)
            logger.LogInformation("Diarkis {Listener} ACKED by client seq={Sequence} cmd=0x{Command:X4} sid={Sid}",
                _name, sequence, command, session.Sid);
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
