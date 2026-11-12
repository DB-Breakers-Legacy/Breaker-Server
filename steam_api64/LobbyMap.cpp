#include "StdInc.h"
#include "LobbyMap.h"
#include "HookManager.h"
#include "Logger.h"
#include "SteamConfig.h"

// The game keeps the current map number at +0x20 of its game global (0x14177E470): 9000 = the lobby
// (data/Map/m9000_lobby), 1000 Grass Rock, 1200 Kai Resorts, 1300 Canyon Namek, 4000 Inside Buu, 8000 Time Nest.
// GamePhaseBattleCore's load step sub_14065C6D0(phase, step) builds the map package path from that number
// (step 2, and step 3 unless the stage info names its own map at +0x18), and the collision loader
// sub_14027D7D0 reads it again. Switching 9000 to another number right before step 2/3 makes the lobby
// load that map instead. Experiment (2026-11-11): what the lobby's own objects, NPCs and spawn points do on
// another map is unknown.
namespace
{
    constexpr std::uintptr_t kGameGlobalPtrRva = 0x177E470;
    constexpr std::uintptr_t kMapNumber = 0x20;
    constexpr std::uintptr_t kStageInfoHolder = 0x6828;   // game + 0x6828 -> +0x10 = current stage info
    constexpr std::uintptr_t kBattleCoreLoadRva = 0x65C6D0; // sub_14065C6D0(GamePhaseBattleCore*, step)
    constexpr std::uint32_t kLobbyMap = 9000;

    using LoadStepFn = std::uint64_t(__fastcall*)(std::uintptr_t, std::uint64_t);
    LoadStepFn g_OriginalLoadStep = nullptr;
    void* g_LoadStepTarget = nullptr;
    std::uintptr_t g_Base = 0;
    std::uint32_t g_TargetMap = 0;

    // Reads (and, for the lobby, switches) the map number; no C++ objects here so __try is allowed.
    bool SwitchMap(std::uint32_t& before, std::uint32_t& stageMap)
    {
        __try
        {
            const auto game = *reinterpret_cast<std::uintptr_t*>(g_Base + kGameGlobalPtrRva);
            if (game == 0)
                return false;
            auto* map = reinterpret_cast<std::uint32_t*>(game + kMapNumber);
            before = *map;
            stageMap = 0xFFFFFFFF;
            const auto holder = *reinterpret_cast<std::uintptr_t*>(game + kStageInfoHolder);
            if (holder != 0)
            {
                const auto info = *reinterpret_cast<std::uintptr_t*>(holder + 0x10);
                if (info != 0)
                    stageMap = *reinterpret_cast<std::uint32_t*>(info + 0x18);
            }
            if (before == kLobbyMap)
                *map = g_TargetMap;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Stage area set-up (sub_14027E170, its step 0 creates one entry per area) and the per-area collision
    // path (sub_14042C380(out, map, area) -> "data/Map/m%u_area%02d_col.map"): switched / logged too, to find
    // which load brings in the lobby island (2026-11-11: switching only the map package changed the skybox,
    // the island stayed).
    constexpr std::uintptr_t kAreaSetupRva = 0x27E170;
    constexpr std::uintptr_t kAreaCollisionPathRva = 0x42C380;
    using AreaSetupFn = std::uint32_t(__fastcall*)(std::uintptr_t);
    using AreaCollisionPathFn = void(__fastcall*)(std::uintptr_t, std::uint32_t, std::uint32_t);
    AreaSetupFn g_OriginalAreaSetup = nullptr;
    AreaCollisionPathFn g_OriginalAreaCollisionPath = nullptr;
    void* g_AreaSetupTarget = nullptr;
    void* g_AreaCollisionPathTarget = nullptr;

    std::uint32_t ReadStep(std::uintptr_t setup)
    {
        __try { return *reinterpret_cast<std::uint32_t*>(setup + 0x50); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFF; }
    }

    std::uint32_t __fastcall AreaSetupDetour(std::uintptr_t setup)
    {
        if (ReadStep(setup) == 0)
        {
            std::uint32_t before = 0, stageMap = 0;
            if (SwitchMap(before, stageMap))
            {
                Logger::Info("LobbyMap: area set-up map=" + std::to_string(before) +
                    (before == kLobbyMap ? " -> switched to " + std::to_string(g_TargetMap) : std::string()));
                Logger::Flush();
            }
        }
        return g_OriginalAreaSetup(setup);
    }

    void __fastcall AreaCollisionPathDetour(std::uintptr_t out, std::uint32_t map, std::uint32_t area)
    {
        Logger::Info("LobbyMap: area collision m" + std::to_string(map) + "_area" + std::to_string(area));
        g_OriginalAreaCollisionPath(out, map, area);
    }

    std::uint64_t __fastcall LoadStepDetour(std::uintptr_t phase, std::uint64_t step)
    {
        if (step == 2 || step == 3)
        {
            std::uint32_t before = 0, stageMap = 0;
            if (SwitchMap(before, stageMap))
            {
                Logger::Info("LobbyMap: load step " + std::to_string(step) + " map=" + std::to_string(before) +
                    " stageInfoMap=" + std::to_string(static_cast<int>(stageMap)) +
                    (before == kLobbyMap ? " -> switched to " + std::to_string(g_TargetMap) : std::string()));
                Logger::Flush();
            }
        }
        return g_OriginalLoadStep(phase, step);
    }
}

bool LobbyMap::Init()
{
    g_TargetMap = static_cast<std::uint32_t>(GetPrivateProfileIntA("steam", "lobby_map", 0, SteamConfig::GetPath().c_str()));
    if (g_TargetMap == 0 || g_TargetMap == kLobbyMap)
        return true;
    g_Base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_Base == 0)
        return false;
    g_LoadStepTarget = reinterpret_cast<void*>(g_Base + kBattleCoreLoadRva);
    if (!HookManager::Create(g_LoadStepTarget, reinterpret_cast<void*>(&LoadStepDetour), reinterpret_cast<void**>(&g_OriginalLoadStep)) ||
        !HookManager::Enable(g_LoadStepTarget))
    {
        Logger::Error("LobbyMap: failed to hook sub_14065C6D0");
        g_LoadStepTarget = nullptr;
        return false;
    }
    g_AreaSetupTarget = reinterpret_cast<void*>(g_Base + kAreaSetupRva);
    if (!HookManager::Create(g_AreaSetupTarget, reinterpret_cast<void*>(&AreaSetupDetour), reinterpret_cast<void**>(&g_OriginalAreaSetup)) ||
        !HookManager::Enable(g_AreaSetupTarget))
    {
        Logger::Error("LobbyMap: failed to hook sub_14027E170");
        g_AreaSetupTarget = nullptr;
    }
    g_AreaCollisionPathTarget = reinterpret_cast<void*>(g_Base + kAreaCollisionPathRva);
    if (!HookManager::Create(g_AreaCollisionPathTarget, reinterpret_cast<void*>(&AreaCollisionPathDetour), reinterpret_cast<void**>(&g_OriginalAreaCollisionPath)) ||
        !HookManager::Enable(g_AreaCollisionPathTarget))
    {
        Logger::Error("LobbyMap: failed to hook sub_14042C380");
        g_AreaCollisionPathTarget = nullptr;
    }
    Logger::Info("LobbyMap: the lobby will load map " + std::to_string(g_TargetMap) + " (lobby_map)");
    return true;
}

void LobbyMap::Shutdown()
{
    for (void** target : { &g_LoadStepTarget, &g_AreaSetupTarget, &g_AreaCollisionPathTarget })
    {
        if (*target)
            HookManager::Disable(*target);
        *target = nullptr;
    }
}
