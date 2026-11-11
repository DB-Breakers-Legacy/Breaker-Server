using BreakerServer.Configuration;
using BreakerServer.Protocol.Diarkis;
using Microsoft.Extensions.Options;
using System.Net;
using System.Net.Sockets;
using System.Text;

namespace BreakerServer.Network;

public sealed class DiarkisUdpService(
    IOptions<BreakerOptions> options,
    ILogger<DiarkisUdpService> logger,
    bool sessionListener = false,
    Services.SessionKeyRegistry? sessionKeys = null) : BackgroundService
{
    private enum NetworkSendState : byte
    {
        SendSyn = 0,
        SendClientKey = 1,
        SendEcho = 2,
        SendUnreliableBuffer = 3,
        SendAck = 4,
        SendEack = 5,
        SendRetry = 6,
        SendReliableBuffer = 7,
        SendFin = 8
    }

    private sealed class ClientState
    {
        public int SequenceCounter;
        public int PacketCount;
        public bool ConnectionActive;
        public bool AwaitingConnectionAck;
        public bool ConnectionEstablished;
        public bool RedirectSent;
        public DiarkisKeySet? Keys;
        public DateTimeOffset LastSeen = DateTimeOffset.UtcNow;

        public void ResetForHandshake()
        {
            SequenceCounter = 0;
            ConnectionActive = true;
            AwaitingConnectionAck = false;
            ConnectionEstablished = false;
            RedirectSent = false;
            LastSeen = DateTimeOffset.UtcNow;
        }
    }

    private static readonly byte[] DirectoryCommand = [0x0C, 0x0C, 0, 0, 0, 1];
    private readonly BreakerOptions _options = options.Value;
    private readonly Dictionary<string, ClientState> _clients = new(StringComparer.Ordinal);
    private readonly int _port = sessionListener ? options.Value.UdpSessionPort : options.Value.UdpPort;
    private DiarkisKeySet? _keys;
    private int _packetsSinceCleanup;

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        if (!sessionListener)
        {
            try
            {
                _keys = SecureEnvelope.FromHex(
                    _options.DiarkisSidKey,
                    _options.DiarkisAesKey,
                    _options.DiarkisIvKey,
                    _options.DiarkisHashKey);
            }
            catch (Exception ex)
            {
                logger.LogCritical(ex, "Invalid Diarkis crypto configuration");
                return;
            }
        }

        using var udp = new UdpClient(new IPEndPoint(IPAddress.Any, _port));
        logger.LogInformation(
            "Diarkis {Listener} UDP service listening on port {Port}",
            sessionListener ? "session" : "matching",
            _port);

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
    }

    private async Task HandleAsync(UdpClient udp, byte[] data, IPEndPoint remote, CancellationToken ct)
    {
        var key = remote.ToString();
        if (!_clients.TryGetValue(key, out var state))
        {
            state = new ClientState();
            _clients[key] = state;
            logger.LogDebug(
                "Diarkis client discovered {Remote} firstPacketBytes={Length} preview={Preview}",
                remote,
                data.Length,
                Convert.ToHexString(data.AsSpan(0, Math.Min(data.Length, 64))));
        }

        state.LastSeen = DateTimeOffset.UtcNow;
        state.PacketCount++;
        CleanupStaleClients();

        if (data.Length >= 10 && data.AsSpan(2, 6).SequenceEqual(DirectoryCommand))
        {
            await HandleDirectoryAsync(udp, data, remote, ct);
            return;
        }

        if (sessionListener && state.Keys is null && data.Length >= 20 &&
            sessionKeys is not null && sessionKeys.TryResolve(data.AsSpan(4, 16), out var resolvedKeys))
            state.Keys = resolvedKeys;

        var keys = sessionListener ? state.Keys : _keys;
        if (keys is null)
        {
            LogUnclassified(remote, data, "unknown or expired session");
            return;
        }

        if (TrySplitControlFrames(data, keys, out var frames))
        {
            foreach (var frame in frames)
                await HandleDatagramAsync(udp, frame, remote, state, ct);
            return;
        }

        if (data.Length < DatagramHeader.Length)
        {
            LogUnclassified(remote, data, "short");
            return;
        }

        await HandleDatagramAsync(udp, data, remote, state, ct);
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

    private async Task HandleDatagramAsync(
        UdpClient udp,
        byte[] data,
        IPEndPoint remote,
        ClientState state,
        CancellationToken ct)
    {
        var header = DatagramHeader.Parse(data);
        LogPacket(remote, data.Length, header.Sequence, header.PacketType, header.Payload.Length, state);

        switch ((NetworkSendState)header.PacketType)
        {
            case NetworkSendState.SendSyn:
                state.ResetForHandshake();
                await SendAsync(udp, DiarkisPacketFactory.Ack(header.Sequence), remote, ct);
                logger.LogInformation("Diarkis SYN accepted from {Remote} seq={Sequence}", remote, header.Sequence);
                return;

            case NetworkSendState.SendClientKey:
                await HandleClientKeyAsync(udp, data, header.Sequence, remote, state, ct);
                return;

            case NetworkSendState.SendEcho:
                await SendAsync(udp, DiarkisPacketFactory.Echo(header.Sequence, header.Payload), remote, ct);
                return;

            case NetworkSendState.SendAck:
                if (!sessionListener)
                    await HandleAckAsync(udp, header.Sequence, remote, state, ct);
                return;

            case NetworkSendState.SendEack:
                logger.LogInformation("Diarkis EACK from={Remote} seq={Sequence} bytes={Length}", remote, header.Sequence, data.Length);
                return;

            case NetworkSendState.SendFin:
                logger.LogInformation("Diarkis FIN from={Remote} seq={Sequence}", remote, header.Sequence);
                _clients.Remove(remote.ToString());
                return;
        }

        LogUnclassified(remote, data, $"state={StateName(header.PacketType)} seq={header.Sequence}");
    }

    private async Task HandleClientKeyAsync(
        UdpClient udp,
        byte[] data,
        int incomingSequence,
        IPEndPoint remote,
        ClientState state,
        CancellationToken ct)
    {
        if (!sessionListener && !state.ConnectionActive)
            state.ResetForHandshake();

        state.SequenceCounter++;
        await SendAsync(udp, DiarkisPacketFactory.Ack(incomingSequence), remote, ct);

        logger.LogInformation(
            "Diarkis ClientKey from={Remote} seq={Sequence} bytes={Length}",
            remote,
            incomingSequence,
            data.Length);

        var text = $"{AdvertisedPrefix()}:{remote.Port}";
        var keys = sessionListener ? state.Keys! : _keys!;
        if (sessionListener)
        {
            if (state.ConnectionEstablished)
                return;

            await SendCommandAsync(udp, 0, 3, 0x0001, 0, 0x01, 0x1B, text, keys, remote, ct);
            state.ConnectionEstablished = true;
            return;
        }

        if (!state.ConnectionEstablished)
        {
            await SendCommandAsync(udp, 0, 3, 0x0001, 0, 0x01, 0x1B, text, keys, remote, ct);
            state.ConnectionEstablished = true;
            state.AwaitingConnectionAck = true;
            return;
        }

        if (state.SequenceCounter == 3)
        {
            await SendCommandAsync(udp, 3, 3, 0x012F, 1, 0xFF, 0x2D,
                $"{_options.UdpHost}:{_options.UdpPort}", keys, remote, ct);
        }
        else if (state.SequenceCounter == 4)
        {
            await SendCommandAsync(udp, 4, 3, 0x0131, 1, 0x01, 0x01, string.Empty, keys, remote, ct);
        }
        else if (state.SequenceCounter > 4)
        {
            var seq = unchecked((byte)(state.SequenceCounter & 0xFF));
            await SendCommandAsync(udp, seq, 3, 0x0001, 0, 0x01, 0x1B, text, keys, remote, ct);
        }
    }

    private async Task HandleAckAsync(
        UdpClient udp,
        int sequence,
        IPEndPoint remote,
        ClientState state,
        CancellationToken ct)
    {
        if (!state.AwaitingConnectionAck || state.RedirectSent || sequence != 0)
            return;

        state.AwaitingConnectionAck = false;
        state.RedirectSent = true;
        var keys = _keys!;
        await SendCommandAsync(udp, 1, 3, 0x012F, 1, 0xFF, 0x2F,
            $"6.{_options.UdpHost}:{_options.UdpSessionPort}", keys, remote, ct);
        await SendCommandAsync(udp, 2, 3, 0x012F, 1, 0xFF, 0x2D,
            $"{_options.UdpHost}:{_options.UdpSessionPort}", keys, remote, ct);
    }

    private static bool TrySplitControlFrames(
        byte[] data,
        DiarkisKeySet keys,
        out List<byte[]> frames)
    {
        frames = [];
        if (data.Length < 20 || data.Length % 20 != 0)
            return false;

        for (var offset = 0; offset < data.Length; offset += 20)
        {
            var frame = data.AsSpan(offset, 20);
            if (frame[3] > (byte)NetworkSendState.SendFin ||
                keys.Sid.Length != 16 ||
                !frame[4..20].SequenceEqual(keys.Sid))
            {
                frames.Clear();
                return false;
            }

            frames.Add(frame.ToArray());
        }

        return frames.Count != 0;
    }

    private void LogPacket(
        IPEndPoint remote,
        int length,
        int sequence,
        byte packetType,
        int payloadBytes,
        ClientState state)
    {
        if (state.PacketCount <= 40 || packetType is (byte)NetworkSendState.SendClientKey or (byte)NetworkSendState.SendFin)
        {
            logger.LogInformation(
                "Diarkis RX from={Remote} state={State}({Type}) seq={Sequence} bytes={Length} payloadBytes={PayloadBytes}",
                remote,
                StateName(packetType),
                packetType,
                sequence,
                length,
                payloadBytes);
        }
    }

    private void LogUnclassified(IPEndPoint remote, byte[] data, string reason) =>
        logger.LogInformation(
            "Diarkis unclassified packet from={Remote} bytes={Length} reason={Reason} preview={Preview}",
            remote,
            data.Length,
            reason,
            Convert.ToHexString(data.AsSpan(0, Math.Min(data.Length, 96))));

    private static string StateName(byte value) =>
        Enum.IsDefined(typeof(NetworkSendState), value)
            ? ((NetworkSendState)value).ToString()
            : $"Unknown{value}";

    private void CleanupStaleClients()
    {
        if (++_packetsSinceCleanup < 128)
            return;

        _packetsSinceCleanup = 0;
        var cutoff = DateTimeOffset.UtcNow - TimeSpan.FromMinutes(2);
        foreach (var pair in _clients.Where(x => x.Value.LastSeen < cutoff).ToArray())
            _clients.Remove(pair.Key);
    }

    private static Task SendCommandAsync(
        UdpClient udp,
        byte sequence,
        byte packetType,
        ushort command,
        byte version,
        byte status,
        uint clearPayloadSize,
        string text,
        DiarkisKeySet keys,
        IPEndPoint remote,
        CancellationToken ct)
    {
        var packet = DiarkisPacketFactory.BuildCommand(
            sequence,
            packetType,
            command,
            version,
            status,
            clearPayloadSize,
            Encoding.UTF8.GetBytes(text),
            keys);
        return SendAsync(udp, packet, remote, ct);
    }

    private string AdvertisedPrefix()
    {
        var pieces = _options.AdvertisedHost.Split('.');
        return pieces.Length >= 2 ? $"{pieces[0]}.{pieces[1]}" : _options.AdvertisedHost;
    }

    private static Task SendAsync(UdpClient udp, byte[] data, IPEndPoint remote, CancellationToken ct) =>
        udp.SendAsync(data, remote, ct).AsTask();
}
