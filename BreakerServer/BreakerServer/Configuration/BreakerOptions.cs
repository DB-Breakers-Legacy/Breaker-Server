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
    // Empty default: configuration binding appends to a non-empty default array (it produced
    // [3478, 3479, 3478, 3479]). Program.cs falls back to 3478/3479 when nothing is configured.
    public int[] StunPorts { get; set; } = [];
    public string UdpHost { get; set; } = "127.0.0.1";

    // Interface the UDP services (Diarkis matching/session, STUN) listen on. Loopback by default: only this
    // PC. For a Tailscale session set it to this PC's Tailscale IP (and UdpHost/StunHost/... to the same).
    // "0.0.0.0" listens on every interface, the whole LAN included; the container image uses it because
    // Docker publishes the ports on the host side.
    public string BindAddress { get; set; } = "127.0.0.1";

    public System.Net.IPAddress BindIPAddress =>
        System.Net.IPAddress.TryParse(BindAddress, out var address) ? address : System.Net.IPAddress.Loopback;

    // Casual matchmaking: "bots" (default) fills the room with bot members after a short search;
    // "players" groups real players with MatchmakingCoordinator. Practice always uses bots.
    public string CasualMatchmaking { get; set; } = "bots";

    public bool RealPlayerMatchmaking =>
        string.Equals(CasualMatchmaking, "players", StringComparison.OrdinalIgnoreCase);
    public int UdpPort { get; set; } = 7100;
    public int UdpSessionPort { get; set; } = 7099;

    public string NewsImageUrl { get; set; } =
        "https://d1bwv3qlbl0wno.cloudfront.net/tss/025348/0/2d986ba27f06455090b72be37c8b7d48.png";
    public string NewsImageFile { get; set; } =
        "Assets/News/2d986ba27f06455090b72be37c8b7d48.png";

    public string AdjustmentDataPath { get; set; } = string.Empty;

    // Folder of official saves waiting to be imported (<steamId>.json, see OfficialSaveImporter). Empty = off.
    public string ImportDir { get; set; } = string.Empty;

    public string DiarkisIvKey { get; set; } = string.Empty;
    public string DiarkisAesKey { get; set; } = string.Empty;
    public string DiarkisSidKey { get; set; } = string.Empty;
    public string DiarkisHashKey { get; set; } = string.Empty;

    // Optional development override: when all four keys are set, every session gets this fixed set.
    // When none are set, each session gets fresh random keys.
    public bool HasFixedDiarkisKeys =>
        DiarkisSidKey.Length > 0 && DiarkisAesKey.Length > 0 && DiarkisIvKey.Length > 0 && DiarkisHashKey.Length > 0;

    public bool HasAnyDiarkisKey =>
        DiarkisSidKey.Length > 0 || DiarkisAesKey.Length > 0 || DiarkisIvKey.Length > 0 || DiarkisHashKey.Length > 0;
}
