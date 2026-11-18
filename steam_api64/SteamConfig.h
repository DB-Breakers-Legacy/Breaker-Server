#pragma once
#include "StdInc.h"

namespace SteamConfig
{
    enum class Mode
    {
        Offline,
        CustomServer,
        RevivedServer,
        SteamPassthrough
    };

    struct Config
    {
        Mode CurrentMode = Mode::Offline;
        std::string PersonaName = "BreakersRevived";
        uint64_t SteamID = 76561199006065889ull;
        std::string CustomServerHost = "127.0.0.1";
        int CustomServerPort = 47584;

        // Local HTTP backend used by the game-service emulator.
        std::string BackendHost = "127.0.0.1";
        int BackendPort = 5000;
        bool RedirectPublicHosts = true;
        bool BlockOfficialTraffic = true;
        bool LanOnly = true;
        bool LogPackets = true;
        bool EnableLocalStorage = true;
        bool EnableLocalStats = true;
        bool EnableLocalMatchmaking = true;
        bool AutoInstanceSteamID = true;
        bool EnableEmbeddedMasterServer = true;

        // ---- Peer networking scope (lobby discovery + emulated Steam P2P) ----
        // Address the DLL's UDP sockets bind to. 127.0.0.1 = this PC only (default, two instances on one PC).
        // Set to this PC's Tailscale IP (100.x.y.z) to play over a tailnet. 0.0.0.0 = every interface (LAN too).
        std::string BindAddress = "127.0.0.1";
        // Address put in lobby adverts as "host". Empty = derived from BindAddress.
        std::string AdvertiseHost;
        // Hosts that receive lobby adverts / discovery / first-contact P2P packets (comma-separated IPv4).
        // Over Tailscale: the other players' Tailscale IPs. Replaces the old hard-coded 26.255.255.255.
        std::string DiscoveryTargets = "127.0.0.1";
        // Also send to 255.255.255.255 (whole LAN segment). Off by default.
        bool LanBroadcast = false;
        // Networks peers may be in: outbound packets to other addresses are blocked and inbound lobby/P2P
        // packets from other addresses are dropped. Loopback, custom_server and backend_host are always allowed.
        std::string AllowedNetworks = "127.0.0.0/8,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,169.254.0.0/16,100.64.0.0/10";
    };

    bool Init();
    bool Load();
    bool Save();

    const Config& Get();
    Config& Mutable();

    bool IsOffline();
    bool IsCustomServer();
    bool IsRevivedServer();
    bool ShouldBlockOfficialTraffic();
    bool IsLanOnly();

    const std::string& GetPath();
    const char* ModeName(Mode mode);

    // Peer networking scope helpers (all IPv4 values in host byte order).
    uint32_t BindIPv4();                                   // INADDR_ANY only when bind_address is 0.0.0.0
    bool IsBindAny();
    bool IsAllowedPeerIPv4(uint32_t hostOrderIp);          // loopback, allowed_networks, custom_server, backend_host
    const std::vector<std::string>& GetDiscoveryTargets(); // parsed discovery_targets (valid IPv4 only)
}
