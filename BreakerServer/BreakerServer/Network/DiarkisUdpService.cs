using BreakerServer.Configuration;
using Microsoft.Extensions.Options;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;

namespace BreakerServer.Network;

public sealed class DiarkisUdpService(
    IOptions<BreakerOptions> options,
    ILogger<DiarkisUdpService> logger) : BackgroundService
{
    private sealed class ClientState
    {
        public int SequenceCounter;
        public int DnsCounter;
        public bool ConnectionActive;
        public bool AwaitingConnectionAck;
        public bool ConnectionEstablished;
        public DateTimeOffset LastSeen = DateTimeOffset.UtcNow;

        public void ResetForHandshake()
        {
            SequenceCounter = 0;
            DnsCounter = 0;
            ConnectionActive = true;
            AwaitingConnectionAck = false;
            ConnectionEstablished = false;
            LastSeen = DateTimeOffset.UtcNow;
        }
    }

    private readonly BreakerOptions _options = options.Value;
    private readonly Dictionary<string, ClientState> _clients = new(StringComparer.Ordinal);
    private int _packetsSinceCleanup;

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        using var udp = new UdpClient(new IPEndPoint(IPAddress.Any, _options.UdpPort));
        logger.LogInformation("Diarkis UDP service listening on port {Port}", _options.UdpPort);

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
        }

        state.LastSeen = DateTimeOffset.UtcNow;
        CleanupStaleClients();

        logger.LogDebug(
            "Diarkis packet from={Remote} bytes={Length} seq={Sequence} established={Established} waitingAck={WaitingAck}",
            remote,
            data.Length,
            state.SequenceCounter,
            state.ConnectionEstablished,
            state.AwaitingConnectionAck);

        // Fresh 20-byte packets reopen the client handshake.
        if (data.Length == 20)
        {
            if (state.AwaitingConnectionAck)
            {
                state.AwaitingConnectionAck = false;
                var h1 = Convert.FromHexString("01000003FEBEDEEF01000054012FFF0000002F");
                var h2 = Convert.FromHexString("02000003FEBEDEEF01000054012FFF0000002D");
                await SendAsync(udp, Combine(h1, Encrypt($"6.{_options.UdpHost}:{_options.UdpSessionPort}")), remote, ct);
                await SendAsync(udp, Combine(h2, Encrypt($"{_options.UdpHost}:{_options.UdpSessionPort}")), remote, ct);
                logger.LogDebug("Diarkis connection ACK completed for {Remote}", remote);
                return;
            }

            state.ResetForHandshake();
            await SendAsync(udp, [0, 0, 0, 4], remote, ct);
            logger.LogDebug("Diarkis handshake reset/opened for {Remote}", remote);
            return;
        }

        if (data.Length == 82)
        {
            if (!state.ConnectionActive)
                state.ResetForHandshake();

            state.SequenceCounter++;
            var seq = unchecked((byte)(state.SequenceCounter & 0xFF));
            await SendAsync(udp, [seq, 0, 0, 4], remote, ct);

            if (!state.ConnectionEstablished)
            {
                await SendAsync(udp, [0, 0, 0, 4], remote, ct);
                var header = Convert.FromHexString("00000003FEBEDEEF000000440001010000001B");
                var response = Combine(header, Encrypt($"{AdvertisedPrefix()}:{remote.Port}"));
                await SendAsync(udp, response, remote, ct);
                state.ConnectionEstablished = true;
                state.AwaitingConnectionAck = true;
            }

            if (state.SequenceCounter == 3)
            {
                var header = Convert.FromHexString("03000003FEBEDEEF01000054012FFF0000002D");
                await SendAsync(udp, Combine(header, Encrypt($"{_options.UdpHost}:{_options.UdpPort}")), remote, ct);
            }
            else if (state.SequenceCounter == 4)
            {
                var header = Convert.FromHexString("04000003FEBEDEEF0100003401310100000001");
                await SendAsync(udp, Combine(header, Encrypt(string.Empty, true)), remote, ct);
            }
            else if (state.SequenceCounter > 4)
            {
                var header = Convert.FromHexString("000003FEBEDEEF000000440001010000001B");
                await SendAsync(udp, Combine([seq], header, Encrypt($"{AdvertisedPrefix()}:{remote.Port}")), remote, ct);
            }

            return;
        }

        if (data.Length == 130)
            return;

        if (data.Length == 26)
        {
            state.DnsCounter++;
            var target = $"{_options.UdpHost}:{_options.UdpPort}";
            var header = Convert.FromHexString("0F0F0C0C00000001002B");
            await SendAsync(udp, Combine(header, Encrypt(target)), remote, ct);

            if (state.DnsCounter == 5)
            {
                var finalHeader = Convert.FromHexString("00000C0C00000001002B");
                var response = Combine(finalHeader, Encrypt(target));
                for (var i = 0; i < 5; i++)
                    await SendAsync(udp, response, remote, ct);
            }
        }
    }

    private void CleanupStaleClients()
    {
        if (++_packetsSinceCleanup < 128)
            return;

        _packetsSinceCleanup = 0;
        var cutoff = DateTimeOffset.UtcNow - TimeSpan.FromMinutes(2);
        foreach (var pair in _clients.Where(x => x.Value.LastSeen < cutoff).ToArray())
            _clients.Remove(pair.Key);
    }

    private byte[] Encrypt(string text, bool noPadding = false)
    {
        var raw = Encoding.UTF8.GetBytes(text);
        byte[] padded;
        if (noPadding)
        {
            padded = raw;
        }
        else
        {
            var pad = 16 - raw.Length % 16;
            padded = raw.Concat(new byte[pad]).ToArray();
        }

        if (padded.Length == 0)
            return Array.Empty<byte>();

        var iv = RandomNumberGenerator.GetBytes(16);
        using var aes = Aes.Create();
        aes.Key = Convert.FromHexString(_options.DiarkisAesKey);
        aes.IV = iv;
        aes.Mode = CipherMode.CBC;
        aes.Padding = PaddingMode.None;

        using var enc = aes.CreateEncryptor();
        var cipher = enc.TransformFinalBlock(padded, 0, padded.Length);

        using var hmac = new HMACSHA256(Convert.FromHexString(_options.DiarkisHashKey));
        var mac = hmac.ComputeHash(Combine(iv, cipher));
        return Combine(mac, iv, cipher);
    }

    private string AdvertisedPrefix()
    {
        var pieces = _options.AdvertisedHost.Split('.');
        return pieces.Length >= 2 ? $"{pieces[0]}.{pieces[1]}" : _options.AdvertisedHost;
    }

    private static Task SendAsync(UdpClient udp, byte[] data, IPEndPoint remote, CancellationToken ct) =>
        udp.SendAsync(data, remote, ct).AsTask();

    private static byte[] Combine(params byte[][] parts)
    {
        var length = parts.Sum(x => x.Length);
        var result = new byte[length];
        var offset = 0;
        foreach (var part in parts)
        {
            part.CopyTo(result, offset);
            offset += part.Length;
        }

        return result;
    }
}
