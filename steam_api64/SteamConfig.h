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
}
