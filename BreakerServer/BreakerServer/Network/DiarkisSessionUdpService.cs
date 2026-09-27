using BreakerServer.Configuration;
using Microsoft.Extensions.Options;
using System.Net;
using System.Net.Sockets;

namespace BreakerServer.Network;

public sealed class DiarkisSessionUdpService(
    IOptions<BreakerOptions> options,
    ILogger<DiarkisSessionUdpService> logger) : BackgroundService
{
    private readonly BreakerOptions _options = options.Value;

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        using var udp = new UdpClient(new IPEndPoint(IPAddress.Any, _options.UdpSessionPort));
        logger.LogInformation("Diarkis session capture listening on UDP {Port}", _options.UdpSessionPort);

        while (!stoppingToken.IsCancellationRequested)
        {
            UdpReceiveResult received;
            try { received = await udp.ReceiveAsync(stoppingToken); }
            catch (OperationCanceledException) { break; }
            catch (Exception ex)
            {
                logger.LogWarning(ex, "Diarkis session receive failed");
                continue;
            }

            var previewLength = Math.Min(received.Buffer.Length, 96);
            logger.LogDebug(
                "Diarkis session packet from={Remote} bytes={Length} preview={Preview}",
                received.RemoteEndPoint,
                received.Buffer.Length,
                Convert.ToHexString(received.Buffer.AsSpan(0, previewLength)));
        }
    }
}
