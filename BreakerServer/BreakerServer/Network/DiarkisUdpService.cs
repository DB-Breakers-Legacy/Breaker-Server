using BreakerServer.Configuration;
using BreakerServer.Protocol.Diarkis;
using Microsoft.Extensions.Options;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace BreakerServer.Network;

public sealed class DiarkisUdpService(
    IOptions<BreakerOptions> options,
    ILogger<DiarkisUdpService> logger) : BackgroundService
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
        public int DnsCounter;
        public int PacketCount;
        public bool ConnectionActive;
        public bool AwaitingConnectionAck;
        public bool ConnectionEstablished;
        public bool RedirectSent;
        public DateTimeOffset LastSeen = DateTimeOffset.UtcNow;

        public void ResetForHandshake()
        {
            SequenceCounter = 0;
            DnsCounter = 0;
            ConnectionActive = true;
            AwaitingConnectionAck = false;
            ConnectionEstablished = false;
            RedirectSent = false;
            LastSeen = DateTimeOffset.UtcNow;
        }
    }

    private readonly BreakerOptions _options = options.Value;
    private readonly Dictionary<string, ClientState> _clients = new(StringComparer.Ordinal);
    private DiarkisKeySet? _keys;
    private int _packetsSinceCleanup;

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
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

        using var udp = new UdpClient(new IPEndPoint(IPAddress.Any, _options.UdpPort));
        logger.LogInformation(
            "Diarkis UDP service listening on port {Port} with AES-CBC/HMAC-SHA256 framing",
            _options.UdpPort);

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
            logger.LogInformation(
                "Diarkis client discovered {Remote} firstPacketBytes={Length} preview={Preview}",
                remote,
                data.Length,
                Convert.ToHexString(data.AsSpan(0, Math.Min(data.Length, 64))));
        }

        state.LastSeen = DateTimeOffset.UtcNow;
        state.PacketCount++;
        CleanupStaleClients();

        // The client can coalesce multiple 20-byte control frames into one UDP
        // datagram. Parse those frames individually instead of classifying the
        // entire datagram only by its byte count.
        if (TrySplitControlFrames(data, out var frames))
        {
            foreach (var frame in frames)
                await HandleControlFrameAsync(udp, frame, remote, state, ct);
            return;
        }

        if (data.Length < DatagramHeader.Length)
        {
            LogUnclassified(remote, data, "short");
            return;
        }

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
                await HandleEchoAsync(udp, header.Sequence, header.Payload, remote, state, ct);
                return;

            case NetworkSendState.SendAck:
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

        // Keep the older observed size-specific paths available for packets
        // whose packet state is not yet decoded, but never let them reset the
        // transport state just because their length happens to match a control
        // frame.
        if (data.Length == 130)
        {
            logger.LogInformation(
                "Diarkis transport packet from={Remote} state={State} seq={Sequence} bytes=130",
                remote,
                StateName(header.PacketType),
                header.Sequence);
            return;
        }

        if (data.Length == 26)
        {
            state.DnsCounter++;
            var target = $"{_options.UdpHost}:{_options.UdpPort}";
            await SendRawEncryptedAsync(udp, Convert.FromHexString("0F0F0C0C00000001002B"), target, remote, ct);

            if (state.DnsCounter == 5)
            {
                var finalPrefix = Convert.FromHexString("00000C0C00000001002B");
                var response = BuildRawEncrypted(finalPrefix, target);
                for (var i = 0; i < 5; i++)
                    await SendAsync(udp, response, remote, ct);
            }

            return;
        }

        LogUnclassified(remote, data, $"state={StateName(header.PacketType)} seq={header.Sequence}");
    }

    private async Task HandleControlFrameAsync(
        UdpClient udp,
        byte[] frame,
        IPEndPoint remote,
        ClientState state,
        CancellationToken ct)
    {
        var header = DatagramHeader.Parse(frame);
        LogPacket(remote, frame.Length, header.Sequence, header.PacketType, header.Payload.Length, state);

        switch ((NetworkSendState)header.PacketType)
        {
            case NetworkSendState.SendSyn:
                state.ResetForHandshake();
                await SendAsync(udp, DiarkisPacketFactory.Ack(header.Sequence), remote, ct);
                logger.LogInformation("Diarkis SYN accepted from {Remote} seq={Sequence}", remote, header.Sequence);
                break;

            case NetworkSendState.SendClientKey:
                await HandleClientKeyAsync(udp, frame, header.Sequence, remote, state, ct);
                break;

            case NetworkSendState.SendEcho:
                await HandleEchoAsync(udp, header.Sequence, header.Payload, remote, state, ct);
                break;

            case NetworkSendState.SendAck:
                await HandleAckAsync(udp, header.Sequence, remote, state, ct);
                break;

            case NetworkSendState.SendEack:
                logger.LogInformation("Diarkis EACK from={Remote} seq={Sequence}", remote, header.Sequence);
                break;

            case NetworkSendState.SendFin:
                logger.LogInformation("Diarkis FIN from={Remote} seq={Sequence}", remote, header.Sequence);
                _clients.Remove(remote.ToString());
                break;

            default:
                logger.LogInformation(
                    "Diarkis control frame from={Remote} state={State} seq={Sequence} payloadBytes={PayloadBytes}",
                    remote,
                    StateName(header.PacketType),
                    header.Sequence,
                    header.Payload.Length);
                break;
        }
    }

    private async Task HandleClientKeyAsync(
        UdpClient udp,
        byte[] data,
        int incomingSequence,
        IPEndPoint remote,
        ClientState state,
        CancellationToken ct)
    {
        if (!state.ConnectionActive)
            state.ResetForHandshake();

        state.SequenceCounter++;
        await SendAsync(udp, DiarkisPacketFactory.Ack(incomingSequence), remote, ct);

        logger.LogInformation(
            "Diarkis ClientKey from={Remote} seq={Sequence} bytes={Length} preview={Preview}",
            remote,
            incomingSequence,
            data.Length,
            Convert.ToHexString(data.AsSpan(0, Math.Min(data.Length, 48))));

        if (!state.ConnectionEstablished)
        {
            await SendCommandAsync(udp, 0, 3, 0x0001, 0, 0x01, 0x1B,
                $"{AdvertisedPrefix()}:{remote.Port}", remote, ct);

            state.ConnectionEstablished = true;
            state.AwaitingConnectionAck = true;
            logger.LogInformation("Diarkis matching transport established for {Remote}", remote);
            return;
        }

        // Retain the later observed command sequence while the exact command
        // meanings are still being mapped. These are no longer driven by raw
        // packet length or Echo traffic.
        if (state.SequenceCounter == 3)
        {
            await SendCommandAsync(udp, 3, 3, 0x012F, 1, 0xFF, 0x2D,
                $"{_options.UdpHost}:{_options.UdpPort}", remote, ct);
        }
        else if (state.SequenceCounter == 4)
        {
            await SendCommandAsync(udp, 4, 3, 0x0131, 1, 0x01, 0x01, string.Empty, remote, ct);
        }
        else if (state.SequenceCounter > 4)
        {
            var seq = unchecked((byte)(state.SequenceCounter & 0xFF));
            await SendCommandAsync(udp, seq, 3, 0x0001, 0, 0x01, 0x1B,
                $"{AdvertisedPrefix()}:{remote.Port}", remote, ct);
        }
    }

    private async Task HandleEchoAsync(
        UdpClient udp,
        int sequence,
        byte[] payload,
        IPEndPoint remote,
        ClientState state,
        CancellationToken ct)
    {
        // Diarkis Echo carries a payload that the client later uses to measure
        // the echo round trip. Return the same payload with the same Echo state;
        // do not reset the ClientKey/connection state.
        await SendAsync(udp, DiarkisPacketFactory.Echo(sequence, payload), remote, ct);
        logger.LogInformation(
            "Diarkis Echo reply to={Remote} seq={Sequence} payloadBytes={PayloadBytes} established={Established}",
            remote,
            sequence,
            payload.Length,
            state.ConnectionEstablished);
    }

    private async Task HandleAckAsync(
        UdpClient udp,
        int sequence,
        IPEndPoint remote,
        ClientState state,
        CancellationToken ct)
    {
        logger.LogInformation(
            "Diarkis ACK from={Remote} seq={Sequence} waitingConnectionAck={Waiting} redirectSent={RedirectSent}",
            remote,
            sequence,
            state.AwaitingConnectionAck,
            state.RedirectSent);

        // The first ACK after the initial matching transport reply acknowledges
        // sequence 0. Only that ACK advances the old redirect stage. ACKs for
        // the redirect packets themselves (seq 1/2) must not restart it.
        if (!state.AwaitingConnectionAck || state.RedirectSent || sequence != 0)
            return;

        state.AwaitingConnectionAck = false;
        state.RedirectSent = true;

        await SendCommandAsync(udp, 1, 3, 0x012F, 1, 0xFF, 0x2F,
            $"6.{_options.UdpHost}:{_options.UdpSessionPort}", remote, ct);
        await SendCommandAsync(udp, 2, 3, 0x012F, 1, 0xFF, 0x2D,
            $"{_options.UdpHost}:{_options.UdpSessionPort}", remote, ct);

        logger.LogInformation(
            "Diarkis matching redirect sent {Remote} -> {Host}:{Port}",
            remote,
            _options.UdpHost,
            _options.UdpSessionPort);
    }

    private bool TrySplitControlFrames(byte[] data, out List<byte[]> frames)
    {
        frames = [];
        if (data.Length < 20 || data.Length % 20 != 0)
            return false;

        for (var offset = 0; offset < data.Length; offset += 20)
        {
            var frame = data.AsSpan(offset, 20);
            var packetType = frame[3];
            if (packetType > (byte)NetworkSendState.SendFin)
            {
                frames.Clear();
                return false;
            }

            // Current client control frames are a 4-byte datagram header plus
            // the configured 16-byte SID. Requiring that shape prevents an
            // unrelated 40/60-byte payload from being split accidentally.
            if (_keys!.Sid.Length != 16 || !frame[4..20].SequenceEqual(_keys.Sid))
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

    private Task SendCommandAsync(
        UdpClient udp,
        byte sequence,
        byte packetType,
        ushort command,
        byte version,
        byte status,
        uint clearPayloadSize,
        string text,
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
            _keys!);
        return SendAsync(udp, packet, remote, ct);
    }

    private Task SendRawEncryptedAsync(
        UdpClient udp,
        byte[] prefix,
        string text,
        IPEndPoint remote,
        CancellationToken ct) =>
        SendAsync(udp, BuildRawEncrypted(prefix, text), remote, ct);

    private byte[] BuildRawEncrypted(byte[] prefix, string text) =>
        DiarkisPacketFactory.Combine(prefix, SecureEnvelope.Encrypt(Encoding.UTF8.GetBytes(text), _keys!));

    private string AdvertisedPrefix()
    {
        var pieces = _options.AdvertisedHost.Split('.');
        return pieces.Length >= 2 ? $"{pieces[0]}.{pieces[1]}" : _options.AdvertisedHost;
    }

    private static Task SendAsync(UdpClient udp, byte[] data, IPEndPoint remote, CancellationToken ct) =>
        udp.SendAsync(data, remote, ct).AsTask();
}
