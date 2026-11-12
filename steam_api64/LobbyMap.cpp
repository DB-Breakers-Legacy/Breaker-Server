#include "StdInc.h"
#include "LobbyMap.h"
#include "HookManager.h"
#include "Logger.h"
#include "SteamConfig.h"
#include <intrin.h>
#include <map>
#include <vector>

// Experimental: show another map as the lobby (steam_config.ini lobby_map=<map number>).
//
// Map numbers: 9000 = lobby (m9000_lobby), 1000 Grass Rock (Highland River), 1200 Kai Resorts, 1300 Canyon Namek,
// 4000 Inside Buu, 8000 Time Nest. The game keeps the current number at game global (0x14177E470) +0x20.
//
// What we learned (2026-11-11):
//  - Test 1/2: switching the number only at the map package step (sub_14065C6D0 step 2) changed the lobby SKY.
//  - Test 3: switching the game's number at the world constructor (sub_14031EA20, run by the lobby phase
//    sub_14067C270 before the package step) built the world as the new map but the game then never left the
//    loading screen: other lobby-only data was then looked up for the new map.
//
// So now the game's own number stays 9000, and only the map-name lookups used by the map's own files are
// answered with the new map: sub_140848D70 (folder) and sub_140848DB0 (name) on table 133 (MapListData), when
// called from the map file loaders (ground, object instances, layouts: 0x140424810..0x14042C900), the map
// package path (sub_14042C280) and the smoke volume (sub_1403B7890). The world/sky object keeps 9000. The lobby
// object layout (lobbyobject.lopt, sub_1404273B0) keeps the lobby's.
namespace
{
    constexpr std::uint32_t kLobbyMap = 9000;
    constexpr std::uintptr_t kMapDirRva = 0x848D70;   // sub_140848D70(table, map) -> folder
    constexpr std::uintptr_t kMapNameRva = 0x848DB0;  // sub_140848DB0(table, map) -> name

    struct Range { std::uintptr_t begin, end; };
    // Callers (RVA ranges) that get the new map.
    constexpr Range kSwitched[] = {
        // Test 5 (2026-11-11): with every point-data loader switched (ground smoke, event layouts, ground, instances,
        // warp points, start positions, event points/units, effects) the lobby still hung before the package step.
        // Now only the ground, the map object instances and the package path are switched.
        { 0x4253A0, 0x425650 },  // MapGroundData.mgd (sub_1404253A0)
        { 0x425650, 0x425D50 },  // instance.objptd (sub_140425650)
        { 0x42C280, 0x42C380 },  // map package path (sub_14042C280) - test 1/2: changed the sky
        { 0x3B7890, 0x3B7A00 },  // smoke volume path (sub_1403B7890)
    };
    // Callers that keep the lobby's map even inside a switched range.
    constexpr Range kKept[] = {
        { 0x4273B0, 0x427690 },  // lobbyobject.lopt (sub_1404273B0)
        // Test 4 (2026-11-11): with the world/sky object (sub_14031F360, outside the ranges above) also switched,
        // the lobby never left the loading screen - it likely waits for the new map's area data. It stays 9000.
    };

    using MapLookupFn = const char*(__fastcall*)(std::uintptr_t, std::uint32_t);
    MapLookupFn g_OriginalMapDir = nullptr;
    MapLookupFn g_OriginalMapName = nullptr;
    void* g_MapDirTarget = nullptr;
    void* g_MapNameTarget = nullptr;
    std::uintptr_t g_Base = 0;
    std::uint32_t g_TargetMap = 0;
    volatile long g_Logged = 0;

    bool In(const Range* ranges, std::size_t count, std::uintptr_t rva)
    {
        for (std::size_t i = 0; i < count; ++i)
            if (rva >= ranges[i].begin && rva < ranges[i].end)
                return true;
        return false;
    }

    std::uint32_t Pick(std::uint32_t map, std::uintptr_t returnAddress, const char* which)
    {
        if (map != kLobbyMap)
            return map;
        const auto rva = returnAddress - g_Base;
        const bool switched = In(kSwitched, std::size(kSwitched), rva) && !In(kKept, std::size(kKept), rva);
        if (InterlockedIncrement(&g_Logged) <= 200)
        {
            char line[160];
            sprintf_s(line, "LobbyMap: %s lookup for 9000 from 0x%llX -> %u", which,
                static_cast<unsigned long long>(0x140000000ull + rva), switched ? g_TargetMap : kLobbyMap);
            Logger::Info(line);
            Logger::Flush();
        }
        return switched ? g_TargetMap : map;
    }

    const char* __fastcall MapDirDetour(std::uintptr_t table, std::uint32_t map)
    {
        return g_OriginalMapDir(table, Pick(map, reinterpret_cast<std::uintptr_t>(_ReturnAddress()), "folder"));
    }

    const char* __fastcall MapNameDetour(std::uintptr_t table, std::uint32_t map)
    {
        return g_OriginalMapName(table, Pick(map, reinterpret_cast<std::uintptr_t>(_ReturnAddress()), "name"));
    }

    // F8 position dump (steam_config.ini position_dump=1): writes every unit in the game's handle table
    // (0x1433B9600, 16-byte entries, object* at +0) with its vtable and position (+0x60 x, y, z floats) to the log.
    // Used to find the lobby's spawn point and spots on other maps (2026-11-11: lobby on the Cell Games platform).
    constexpr std::uintptr_t kHandleTableRva = 0x33B9600;
    constexpr std::uint32_t kHandleTableSize = 0x2000;
    constexpr std::uintptr_t kUnitPosition = 0x60;

    struct UnitInfo { std::uint32_t handle; std::uintptr_t vtableRva; float x, y, z; };

    bool ReadUnit(std::uint32_t handle, UnitInfo& out)
    {
        __try
        {
            const auto unit = *reinterpret_cast<std::uintptr_t*>(g_Base + kHandleTableRva + static_cast<std::uintptr_t>(handle) * 16);
            if (unit == 0)
                return false;
            const auto vtable = *reinterpret_cast<std::uintptr_t*>(unit);
            if (vtable < g_Base + 0x1000000 || vtable > g_Base + 0x1800000)
                return false;
            const float* pos = reinterpret_cast<const float*>(unit + kUnitPosition);
            out = { handle, vtable - g_Base, pos[0], pos[1], pos[2] };
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void DumpPositions()
    {
        const auto game = *reinterpret_cast<std::uintptr_t*>(g_Base + 0x177E470);
        std::uint32_t map = 0;
        if (game != 0)
            map = *reinterpret_cast<std::uint32_t*>(game + 0x20);
        Logger::Info("PositionDump: --- F8 pressed, map number " + std::to_string(map) + " ---");
        // Bulk objects (hundreds of the same class) are skipped: test 1 (2026-11-11) filled the first 400 lines with
        // two classes and never reached the characters.
        std::vector<UnitInfo> units;
        std::map<std::uintptr_t, int> perClass;
        for (std::uint32_t h = 0; h < kHandleTableSize; ++h)
        {
            UnitInfo u{};
            if (ReadUnit(h, u))
            {
                units.push_back(u);
                ++perClass[u.vtableRva];
            }
        }
        int count = 0, skipped = 0;
        for (const auto& u : units)
        {
            if (perClass[u.vtableRva] > 16 || count >= 600)
            {
                ++skipped;
                continue;
            }
            char line[160];
            sprintf_s(line, "PositionDump: handle %u vtable 0x%llX pos %.2f %.2f %.2f", u.handle,
                static_cast<unsigned long long>(0x140000000ull + u.vtableRva), u.x, u.y, u.z);
            Logger::Info(line);
            ++count;
        }
        Logger::Info("PositionDump: " + std::to_string(count) + " units logged, " + std::to_string(skipped) + " bulk skipped");
        Logger::Flush();
    }

    DWORD WINAPI DumpKeyThread(LPVOID)
    {
        bool wasDown = false;
        for (;;)
        {
            const bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
            if (down && !wasDown)
                DumpPositions();
            wasDown = down;
            Sleep(50);
        }
    }

    // Lobby lift (lobby_lift_y=<units>, needs lobby_map): F8 dumps (2026-11-11) put the lobby island at height 0
    // around the map origin (pieces from -14 to +16) and the Cell Games platform on Highland River at height 32.34
    // around the same origin (player in its middle: 1.32, 32.34, 2.21). So the lobby pieces and the player are raised
    // once each by lobby_lift_y after they spawn. Moved: the lobby piece classes (vtables 0x141250000 and up) and
    // the player/NPC characters (Mob 0x1411787A0); bulk classes (more than 16 objects) are left alone.
    float g_LiftY = 0.0f;
    constexpr std::uintptr_t kMobVftableRva = 0x11787A0;

    // Test 7 (2026-11-11): writing +0x60 from a side thread changed the stored value but nothing moved on screen;
    // the player's physics body keeps its own position. Characters now go through the game's warp
    // (sub_140213410(character, &position, 0, 1): used by SimpleAIActionPracticeWarp; sets +0x60 via the unit
    // setter 0x140C208E0, copies it to +0x70, moves the physics body at +0xA50, sets the moved flags), and all of
    // it runs on the game thread from the lobby update.
    constexpr std::uintptr_t kWarpRva = 0x213410;
    using WarpFn = std::uint64_t(__fastcall*)(std::uintptr_t, const float*, std::uint64_t, std::uint64_t);

    bool LiftUnit(std::uint32_t handle, float dy, std::uintptr_t& vtableRva)
    {
        __try
        {
            const auto unit = *reinterpret_cast<std::uintptr_t*>(g_Base + kHandleTableRva + static_cast<std::uintptr_t>(handle) * 16);
            if (unit == 0)
                return false;
            vtableRva = *reinterpret_cast<std::uintptr_t*>(unit) - g_Base;
            if (vtableRva != kMobVftableRva && (vtableRva < 0x1250000 || vtableRva > 0x1800000))
                return false;
            auto* pos = reinterpret_cast<float*>(unit + kUnitPosition);
            if (vtableRva == kMobVftableRva)
            {
                alignas(16) float target[4] = { pos[0], pos[1] + dy, pos[2], pos[3] };
                reinterpret_cast<WarpFn>(g_Base + kWarpRva)(unit, target, 0, 1);
            }
            else
            {
                pos[1] += dy;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::uint32_t CurrentMap()
    {
        __try
        {
            const auto game = *reinterpret_cast<std::uintptr_t*>(g_Base + 0x177E470);
            return game != 0 ? *reinterpret_cast<std::uint32_t*>(game + 0x20) : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    std::uintptr_t ObjectAt(std::uint32_t handle)
    {
        __try { return *reinterpret_cast<std::uintptr_t*>(g_Base + kHandleTableRva + static_cast<std::uintptr_t>(handle) * 16); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    std::map<std::uint32_t, std::uintptr_t> g_Lifted;   // handle -> object, so a reused handle is lifted again
    int g_LiftFrame = 0;

    void LiftTick()
    {
        auto& lifted = g_Lifted;
        {
            if (++g_LiftFrame % 15 != 0)
                return;
            if (CurrentMap() != kLobbyMap)
            {
                lifted.clear();
                return;
            }
            std::vector<UnitInfo> units;
            std::map<std::uintptr_t, int> perClass;
            for (std::uint32_t h = 0; h < kHandleTableSize; ++h)
            {
                UnitInfo u{};
                if (ReadUnit(h, u))
                {
                    units.push_back(u);
                    ++perClass[u.vtableRva];
                }
            }
            int moved = 0;
            for (const auto& u : units)
            {
                if (perClass[u.vtableRva] > 16)
                    continue;
                const auto object = ObjectAt(u.handle);
                const auto it = lifted.find(u.handle);
                if (it != lifted.end() && it->second == object)
                    continue;
                std::uintptr_t vtableRva = 0;
                if (LiftUnit(u.handle, g_LiftY, vtableRva))
                {
                    lifted[u.handle] = object;
                    ++moved;
                    char line[160];
                    sprintf_s(line, "LobbyLift: raised handle %u (vtable 0x%llX) from %.2f to %.2f", u.handle,
                        static_cast<unsigned long long>(0x140000000ull + vtableRva), u.y, u.y + g_LiftY);
                    Logger::Info(line);
                }
            }
            if (moved > 0)
                Logger::Flush();
        }
    }

    constexpr std::uintptr_t kLobbyUpdateRva = 0x677520;   // GamePhaseLobby update (vtable slot 9), every frame
    using LobbyUpdateFn = void(__fastcall*)(std::uintptr_t);
    LobbyUpdateFn g_OriginalLobbyUpdate = nullptr;
    void* g_LobbyUpdateTarget = nullptr;

    void __fastcall LobbyUpdateDetour(std::uintptr_t lobby)
    {
        g_OriginalLobbyUpdate(lobby);
        LiftTick();
    }

    // Lobby collision (lobby_collision=<file>). Test 8 (2026-11-11): warped onto the Cell Games platform, the
    // player fell through - the lobby has no Highland River collision. The lobby world (sub_14031F9B0) loads ONE
    // stage file "<folder><name>.map" (m9000_lobby: the island collision); Highland River's collision is per area,
    // "data/Map/m1000_areaNN_col.map", loaded by a loader the lobby never runs. Both are opened with
    // sub_140447770(path, 1, 128) as XG::Game::Battle::StageFile. So the lobby's .map is swapped for one area file.
    constexpr std::uintptr_t kOpenStageFileRva = 0x447770;
    using OpenStageFileFn = std::uint64_t(__fastcall*)(const char*, std::uint64_t, std::uint64_t);
    OpenStageFileFn g_OriginalOpenStageFile = nullptr;
    void* g_OpenStageFileTarget = nullptr;
    char g_LobbyCollision[200] = {};

    std::uint64_t __fastcall OpenStageFileDetour(const char* path, std::uint64_t a2, std::uint64_t a3)
    {
        const std::size_t length = path != nullptr ? strlen(path) : 0;
        if (g_LobbyCollision[0] != 0 && length > 4 && strcmp(path + length - 4, ".map") == 0 &&
            strstr(path, "m9000_lobby") != nullptr)
        {
            Logger::Info(std::string("LobbyMap: stage file ") + path + " -> " + g_LobbyCollision);
            Logger::Flush();
            return g_OriginalOpenStageFile(g_LobbyCollision, a2, a3);
        }
        return g_OriginalOpenStageFile(path, a2, a3);
    }

    bool HookOne(std::uintptr_t rva, void* detour, void** original, void** target, const char* name)
    {
        *target = reinterpret_cast<void*>(g_Base + rva);
        if (!HookManager::Create(*target, detour, original) || !HookManager::Enable(*target))
        {
            Logger::Error(std::string("LobbyMap: failed to hook ") + name);
            *target = nullptr;
            return false;
        }
        return true;
    }
}

bool LobbyMap::Init()
{
    if (GetPrivateProfileIntA("steam", "position_dump", 0, SteamConfig::GetPath().c_str()) != 0)
    {
        g_Base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        CreateThread(nullptr, 0, &DumpKeyThread, nullptr, 0, nullptr);
        Logger::Info("PositionDump: press F8 to log every unit's position (position_dump=1)");
    }
    g_TargetMap = static_cast<std::uint32_t>(GetPrivateProfileIntA("steam", "lobby_map", 0, SteamConfig::GetPath().c_str()));
    if (g_TargetMap == 0 || g_TargetMap == kLobbyMap)
        return true;
    g_Base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_Base == 0)
        return false;
    const bool dir = HookOne(kMapDirRva, reinterpret_cast<void*>(&MapDirDetour),
        reinterpret_cast<void**>(&g_OriginalMapDir), &g_MapDirTarget, "sub_140848D70");
    const bool name = HookOne(kMapNameRva, reinterpret_cast<void*>(&MapNameDetour),
        reinterpret_cast<void**>(&g_OriginalMapName), &g_MapNameTarget, "sub_140848DB0");
    Logger::Info("LobbyMap: the lobby's map files will come from map " + std::to_string(g_TargetMap) + " (lobby_map)");
    GetPrivateProfileStringA("steam", "lobby_collision", "", g_LobbyCollision, sizeof(g_LobbyCollision), SteamConfig::GetPath().c_str());
    if (g_LobbyCollision[0] != 0)
    {
        HookOne(kOpenStageFileRva, reinterpret_cast<void*>(&OpenStageFileDetour),
            reinterpret_cast<void**>(&g_OriginalOpenStageFile), &g_OpenStageFileTarget, "sub_140447770 (stage file)");
        Logger::Info(std::string("LobbyMap: the lobby's collision file will be ") + g_LobbyCollision + " (lobby_collision)");
    }
    char lift[32] = {};
    GetPrivateProfileStringA("steam", "lobby_lift_y", "", lift, sizeof(lift), SteamConfig::GetPath().c_str());
    g_LiftY = static_cast<float>(atof(lift));
    if (g_LiftY != 0.0f)
    {
        HookOne(kLobbyUpdateRva, reinterpret_cast<void*>(&LobbyUpdateDetour),
            reinterpret_cast<void**>(&g_OriginalLobbyUpdate), &g_LobbyUpdateTarget, "0x140677520 (lobby update)");
        Logger::Info(std::string("LobbyLift: lobby pieces and the player are raised by ") + lift + " (lobby_lift_y)");
    }
    return dir && name;
}

void LobbyMap::Shutdown()
{
    for (void** target : { &g_MapDirTarget, &g_MapNameTarget, &g_LobbyUpdateTarget, &g_OpenStageFileTarget })
    {
        if (*target)
            HookManager::Disable(*target);
        *target = nullptr;
    }
}
