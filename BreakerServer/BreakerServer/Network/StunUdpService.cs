using BreakerServer.Configuration;
using Microsoft.Extensions.Options;
using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;

namespace BreakerServer.Network;

public sealed class StunUdpService(
    IOptions<BreakerOptions> options,
    ILogger<StunUdpService> logger) : BackgroundService
{
    private const uint MagicCookie = 0x2112A442;
    private readonly BreakerOptions _options = options.Value;

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        var workers = _options.StunPorts
            .Distinct()
            .Select(port => RunPortAsync(port, stoppingToken))
            .ToArray();

        await Task.WhenAll(workers);
    }

    private async Task RunPortAsync(int port, CancellationToken ct)
    {
        using var udp = new UdpClient(new IPEndPoint(IPAddress.Any, port));
        logger.LogInformation("STUN emulator listening on UDP {Port}", port);

        while (!ct.IsCancellationRequested)
        {
            UdpReceiveResult received;
            try { received = await udp.ReceiveAsync(ct); }
            catch (OperationCanceledException) { break; }
            catch (Exception ex)
            {
                logger.LogWarning(ex, "STUN receive failed on UDP {Port}", port);
                continue;
            }

            var response = BuildBindingResponse(received.Buffer, received.RemoteEndPoint);
            if (response is null)
            {
                logger.LogDebug(
                    "STUN unknown packet port={Port} from={Remote} bytes={Length} preview={Preview}",
                    port,
                    received.RemoteEndPoint,
                    received.Buffer.Length,
                    Convert.ToHexString(received.Buffer.AsSpan(0, Math.Min(received.Buffer.Length, 64))));
                continue;
            }

            await udp.SendAsync(response, received.RemoteEndPoint, ct);
        }
    }

    private static byte[]? BuildBindingResponse(byte[] request, IPEndPoint remote)
    {
        if (request.Length < 20)
            return null;

        var type = BinaryPrimitives.ReadUInt16BigEndian(request.AsSpan(0, 2));
        var cookie = BinaryPrimitives.ReadUInt32BigEndian(request.AsSpan(4, 4));
        if (type != 0x0001 || cookie != MagicCookie)
            return null;

        var response = new byte[32];
        BinaryPrimitives.WriteUInt16BigEndian(response.AsSpan(0, 2), 0x0101);
        BinaryPrimitives.WriteUInt16BigEndian(response.AsSpan(2, 2), 12);
        BinaryPrimitives.WriteUInt32BigEndian(response.AsSpan(4, 4), MagicCookie);
        request.AsSpan(8, 12).CopyTo(response.AsSpan(8, 12));

        // XOR-MAPPED-ADDRESS, IPv4.
        BinaryPrimitives.WriteUInt16BigEndian(response.AsSpan(20, 2), 0x0020);
        BinaryPrimitives.WriteUInt16BigEndian(response.AsSpan(22, 2), 8);
        response[24] = 0;
        response[25] = 0x01;
        BinaryPrimitives.WriteUInt16BigEndian(response.AsSpan(26, 2), (ushort)(remote.Port ^ (MagicCookie >> 16)));

        var address = remote.Address.MapToIPv4().GetAddressBytes();
        Span<byte> cookieBytes = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(cookieBytes, MagicCookie);
        for (var i = 0; i < 4; i++)
            response[28 + i] = (byte)(address[i] ^ cookieBytes[i]);

        return response;
    }
}
