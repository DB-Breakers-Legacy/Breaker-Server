#include "StdInc.h"
#include "LobbyMap.h"
#include "HookManager.h"
#include "Logger.h"
#include "SteamConfig.h"
#include <intrin.h>

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
        int count = 0;
        for (std::uint32_t h = 0; h < kHandleTableSize && count < 400; ++h)
        {
            UnitInfo u{};
            if (!ReadUnit(h, u))
                continue;
            char line[160];
            sprintf_s(line, "PositionDump: handle %u vtable 0x%llX pos %.2f %.2f %.2f", u.handle,
                static_cast<unsigned long long>(0x140000000ull + u.vtableRva), u.x, u.y, u.z);
            Logger::Info(line);
            ++count;
        }
        Logger::Info("PositionDump: " + std::to_string(count) + " units");
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
    return dir && name;
}

void LobbyMap::Shutdown()
{
    for (void** target : { &g_MapDirTarget, &g_MapNameTarget })
    {
        if (*target)
            HookManager::Disable(*target);
        *target = nullptr;
    }
}
