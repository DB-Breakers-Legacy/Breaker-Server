using BreakerServer.Configuration;
using BreakerServer.Protocol.Diarkis;
using Microsoft.Extensions.Options;
using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace BreakerServer.Network;

public sealed class DiarkisSessionUdpService(
    IOptions<BreakerOptions> options,
    ILogger<DiarkisSessionUdpService> logger) : BackgroundService
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
        public int Sequence;
        public int PacketCount;
        public bool Established;
        public DateTimeOffset LastSeen = DateTimeOffset.UtcNow;
    }

    private readonly BreakerOptions _options = options.Value;
    private readonly Dictionary<string, ClientState> _clients = new(StringComparer.Ordinal);
    private DiarkisKeySet? _keys;

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
            logger.LogCritical(ex, "Invalid Diarkis session crypto configuration");
            return;
        }

        using var udp = new UdpClient(new IPEndPoint(IPAddress.Any, _options.UdpSessionPort));
        logger.LogInformation("Diarkis session service listening on UDP {Port}", _options.UdpSessionPort);

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
                logger.LogWarning(ex, "Diarkis session receive failed");
                continue;
            }

            try
            {
                await HandleAsync(udp, received.Buffer, received.RemoteEndPoint, stoppingToken);
            }
            catch (Exception ex)
            {
                logger.LogWarning(ex, "Diarkis session packet failed from {Remote}", received.RemoteEndPoint);
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
                "Diarkis session client discovered {Remote} firstPacketBytes={Length} preview={Preview}",
                remote,
                data.Length,
                Convert.ToHexString(data.AsSpan(0, Math.Min(data.Length, 64))));
        }

        state.LastSeen = DateTimeOffset.UtcNow;
        state.PacketCount++;

        if (TrySplitControlFrames(data, out var frames))
        {
            foreach (var frame in frames)
                await HandleControlFrameAsync(udp, frame, remote, state, ct);
            return;
        }

        if (data.Length < DatagramHeader.Length)
        {
            LogPacket(remote, data, "short");
            return;
        }

        var header = DatagramHeader.Parse(data);
        logger.LogInformation(
            "Diarkis session RX from={Remote} state={State}({Type}) seq={Sequence} bytes={Length} payloadBytes={PayloadBytes}",
            remote,
            StateName(header.PacketType),
            header.PacketType,
            header.Sequence,
            data.Length,
            header.Payload.Length);

        switch ((NetworkSendState)header.PacketType)
        {
            case NetworkSendState.SendSyn:
                state.Established = false;
                state.Sequence = 0;
                await SendAsync(udp, DiarkisPacketFactory.Ack(header.Sequence), remote, ct);
                return;

            case NetworkSendState.SendClientKey:
                await HandleClientKeyAsync(udp, data, header.Sequence, remote, state, ct);
                return;

            case NetworkSendState.SendEcho:
                await SendAsync(udp, DiarkisPacketFactory.Echo(header.Sequence, header.Payload), remote, ct);
                logger.LogInformation("Diarkis session Echo reply to={Remote} seq={Sequence}", remote, header.Sequence);
                return;

            case NetworkSendState.SendAck:
                logger.LogInformation("Diarkis session ACK from={Remote} seq={Sequence}", remote, header.Sequence);
                return;

            case NetworkSendState.SendEack:
                logger.LogInformation("Diarkis session EACK from={Remote} seq={Sequence}", remote, header.Sequence);
                return;

            case NetworkSendState.SendFin:
                logger.LogInformation("Diarkis session FIN from={Remote} seq={Sequence}", remote, header.Sequence);
                _clients.Remove(key);
                return;
        }

        LogPacket(remote, data, $"state={StateName(header.PacketType)} seq={header.Sequence}");
    }

    private async Task HandleControlFrameAsync(
        UdpClient udp,
        byte[] frame,
        IPEndPoint remote,
        ClientState state,
        CancellationToken ct)
    {
        var header = DatagramHeader.Parse(frame);
        logger.LogInformation(
            "Diarkis session control from={Remote} state={State}({Type}) seq={Sequence} payloadBytes={PayloadBytes}",
            remote,
            StateName(header.PacketType),
            header.PacketType,
            header.Sequence,
            header.Payload.Length);

        switch ((NetworkSendState)header.PacketType)
        {
            case NetworkSendState.SendSyn:
                state.Established = false;
                state.Sequence = 0;
                await SendAsync(udp, DiarkisPacketFactory.Ack(header.Sequence), remote, ct);
                break;

            case NetworkSendState.SendClientKey:
                await HandleClientKeyAsync(udp, frame, header.Sequence, remote, state, ct);
                break;

            case NetworkSendState.SendEcho:
                await SendAsync(udp, DiarkisPacketFactory.Echo(header.Sequence, header.Payload), remote, ct);
                break;

            case NetworkSendState.SendAck:
            case NetworkSendState.SendEack:
                break;

            case NetworkSendState.SendFin:
                _clients.Remove(remote.ToString());
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
        state.Sequence++;
        await SendAsync(udp, DiarkisPacketFactory.Ack(incomingSequence), remote, ct);

        logger.LogInformation(
            "Diarkis session ClientKey from={Remote} seq={Sequence} bytes={Length} preview={Preview}",
            remote,
            incomingSequence,
            data.Length,
            Convert.ToHexString(data.AsSpan(0, Math.Min(data.Length, 48))));

        if (state.Established)
            return;

        var text = $"{AdvertisedPrefix()}:{remote.Port}";
        var packet = DiarkisPacketFactory.BuildCommand(
            0, 3, 0x0001, 0, 0x01, 0x1B, Encoding.UTF8.GetBytes(text), _keys!);
        await SendAsync(udp, packet, remote, ct);
        state.Established = true;
        logger.LogInformation("Diarkis session transport established for {Remote}", remote);
    }

    private bool TrySplitControlFrames(byte[] data, out List<byte[]> frames)
    {
        frames = [];
        if (data.Length < 20 || data.Length % 20 != 0)
            return false;

        for (var offset = 0; offset < data.Length; offset += 20)
        {
            var frame = data.AsSpan(offset, 20);
            if (frame[3] > (byte)NetworkSendState.SendFin)
            {
                frames.Clear();
                return false;
            }

            if (_keys!.Sid.Length != 16 || !frame[4..20].SequenceEqual(_keys.Sid))
            {
                frames.Clear();
                return false;
            }

            frames.Add(frame.ToArray());
        }

        return frames.Count != 0;
    }

    private static string StateName(byte value) =>
        Enum.IsDefined(typeof(NetworkSendState), value)
            ? ((NetworkSendState)value).ToString()
            : $"Unknown{value}";

    private void LogPacket(IPEndPoint remote, byte[] data, string reason) =>
        logger.LogInformation(
            "Diarkis session packet from={Remote} bytes={Length} reason={Reason} preview={Preview}",
            remote,
            data.Length,
            reason,
            Convert.ToHexString(data.AsSpan(0, Math.Min(data.Length, 96))));

    private string AdvertisedPrefix()
    {
        var pieces = _options.AdvertisedHost.Split('.');
        return pieces.Length >= 2 ? $"{pieces[0]}.{pieces[1]}" : _options.AdvertisedHost;
    }

    private static Task SendAsync(UdpClient udp, byte[] data, IPEndPoint remote, CancellationToken ct) =>
        udp.SendAsync(data, remote, ct).AsTask();
}
