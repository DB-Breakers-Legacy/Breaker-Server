namespace BreakerServer.Configuration;

public sealed class BreakerOptions
{
    public const string SectionName = "Breaker";

    // Public URL returned to the game during environment/bootstrap discovery.
    public string BackendBaseUrl { get; set; } = "http://127.0.0.1:5000";

    // Addresses advertised by the emulated game/network services.
    public string EnvHost { get; set; } = "127.0.0.1";
    public string PrdHost { get; set; } = "127.0.0.1";
    public string AdvertisedHost { get; set; } = "127.0.0.1";
    public string StunHost { get; set; } = "127.0.0.1";
    public int[] StunPorts { get; set; } = [3478, 3479];
    public string UdpHost { get; set; } = "127.0.0.1";
    public int UdpPort { get; set; } = 7100;
    public int UdpSessionPort { get; set; } = 7099;

    public string NewsImageUrl { get; set; } =
        "https://d1bwv3qlbl0wno.cloudfront.net/tss/025348/0/2d986ba27f06455090b72be37c8b7d48.png";
    public string NewsImageFile { get; set; } =
        "Assets/News/2d986ba27f06455090b72be37c8b7d48.png";

    public string AdjustmentDataPath { get; set; } = string.Empty;
    public string DiarkisIvKey { get; set; } = new('0', 32);
    public string DiarkisAesKey { get; set; } = new('0', 32);
    public string DiarkisSidKey { get; set; } = new('0', 32);
    public string DiarkisHashKey { get; set; } = new('0', 32);
}
