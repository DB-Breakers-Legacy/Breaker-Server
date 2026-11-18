#include "StdInc.h"

#include "AreaDestroyStack.h"

#include "HookManager.h"
#include "Logger.h"
#include "SteamConfig.h"

#include <intrin.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")
#pragma intrinsic(_ReturnAddress)

// Area destruction stacking (experimental, 2026-11-17).
//
// Project lead, 2026-11-16: "another experiment id like to do is for raiders to be able to stack up their area
// destroying attacks should they evolve without using it, such as if cell evolves from level 2 to 3 without using an
// area destroying attack, he can use it twice at level 3, if he evolves from level 3 to 4 without using any area
// destroying move, he can use it 3 times, if he uses 1 area destroying move at level 3 but not 2, he can use it
// twice at level 4, id also like to introduce destroying up to 4 areas under specific circumstances".
// 2026-11-17: "for now lets test the stacking area destroying rules, for the purpose of testing, if i do not destroy
// any areas from levels 1 all the way to 4, then the level 4 raider can destroy 4 areas rather than 3" (the real
// circumstances for a 4th area are still to be decided - "i was thinking during a 'last resort' form").
//
// What the game does (read from the exe; the game's data calls area destruction "MapBreak"):
// - Every unit has a skill table at unit + 0x2F0: +0x00 the unit's index in the unit table (0x1433B9600, 16-byte
//   entries, pointer first), then from +0x04 five level blocks (level 0 - 4) of eleven 0x24-byte records:
//     record + 0x00 u8   ready (the skill can be used)        + 0x04 f32  cooldown length
//     record + 0x0C f32  cooldown left                         + 0x10 f32  cooldown scale
//     record + 0x18 u32  largest stock (multi-charge skills)   + 0x1C i32  stock = uses left
//     record + 0x20 u8   cooldown paused (while in use)        + 0x23 u8   multi-charge mode
//   Slots 2 and 3 are the two supers, 5 - 8 the four active skills.
// - sub_14031da90(table, slot) uses a skill: stock - 1 if it was above 0, the cooldown starts, and "ready" is
//   cleared when no stock is left. The per-frame tick (sub_14031dd60) sets "ready" again once the cooldown is over
//   AND stock is not 0. sub_14031d990(table, slot) un-pauses the cooldown when the skill's action is over.
// - Area destruction is skill-state kind 20 (XG::Game::StateSkill setup sub_1401a6760). The callback that runs when
//   its last action ends (class_141197ad8::vfunc_2) calls sub_14031d990 with the skill's slot; that call returns to
//   game + 0x1C1F59. Every level block has its own record, so each level brings its own single use and a use left
//   over at a lower level is simply never looked at again. That is the whole "once per evolution" rule.
//   Possible_ (not checked in memory yet, the log line below shows it): the area destruction record starts with
//   stock 1 and no cooldown.
//
// What this module does: a detour on sub_14031d990. When the call is the one that ends an area destruction of a
// Raider:
//   1. test rule (raider_destroy_bonus=1): at level 4, if every lower level that grants a destruction still has
//      its use (= nothing was destroyed before level 4, and this was the first one at level 4), one extra use;
//   2. one unused use is taken from the highest lower level that still has one and given to the current level.
// The game's own tick then makes the skill ready again. Nothing else is touched; with nothing left over the game
// behaves as before. Levels that grant a destruction = every level above the Raider's first one (Broly starts at
// level 2, everyone else at level 1).
//
// Limits: only the machine that runs the Raider does this - local practice matches only.

namespace
{
    constexpr std::uintptr_t kSkillReleaseRva = 0x31D990;       // sub_14031d990(skill table, slot)
    constexpr std::uintptr_t kMapBreakEndReturnRva = 0x1C1F59;  // after the call in class_141197ad8::vfunc_2
    constexpr std::uintptr_t kUnitTableRva = 0x33B9600;         // 16-byte entries, unit pointer first
    constexpr std::uintptr_t kGameGlobalPtrRva = 0x177E470;     // the game object (same global PracticeBots uses)
    constexpr std::uintptr_t kLevelGetterRva = 0xCD9E0;         // sub_1400cd9e0(object at unit + 0x2E0) -> level
    constexpr std::uintptr_t kUnitIsRaider = 0x204;             // u32, 1 = Raider
    constexpr std::uintptr_t kUnitFamily = 0x20C;               // u32, 5 = Broly
    constexpr std::uintptr_t kUnitLevelHolder = 0x2E0;
    constexpr std::uintptr_t kTableFirstRecord = 0x04;
    constexpr std::uintptr_t kRecordSize = 0x24;
    constexpr std::uintptr_t kRecReady = 0x00;
    constexpr std::uintptr_t kRecCooldownLength = 0x04;
    constexpr std::uintptr_t kRecCooldownLeft = 0x0C;
    constexpr std::uintptr_t kRecLargestStock = 0x18;
    constexpr std::uintptr_t kRecStock = 0x1C;
    constexpr std::uintptr_t kRecPaused = 0x20;
    constexpr std::uintptr_t kRecMode = 0x23;
    constexpr int kSlotsPerLevel = 11;
    constexpr int kLevelCount = 5;
    constexpr std::uint32_t kFamilyBroly = 5;

    using ReleaseFn = std::uint64_t(__fastcall*)(std::uint8_t* table, std::int32_t slot);
    using LevelFn = std::uint32_t(__fastcall*)(void* holder);

    std::uintptr_t g_Base = 0;
    bool g_Bonus = false;
    ReleaseFn g_OriginalRelease = nullptr;

    struct Seen
    {
        std::uint32_t family = 0xFFFFFFFF;
        std::uint32_t level = 0;
        std::int32_t slot = -1;
        std::int32_t firstLevel = 0;
        std::int32_t before[kLevelCount] = {};
        std::int32_t after[kLevelCount] = {};
        std::uint32_t largest[kLevelCount] = {};
        float cooldownLength[kLevelCount] = {};
        float cooldownLeft[kLevelCount] = {};
        std::uint8_t ready[kLevelCount] = {};
        std::uint8_t paused[kLevelCount] = {};
        std::uint8_t mode[kLevelCount] = {};
        std::int32_t takenFrom = 0;                             // level a left-over use was taken from (0 = none)
        std::int32_t destroyed = -1;                            // areas marked destroyed right now (-1 = not readable)
        std::int32_t cut = 0;                                   // uses taken back by the safety limit
        bool bonus = false;
        bool odd = false;                                       // values are not what the reading expects: nothing done
    };

    std::uint8_t* Record(std::uint8_t* table, int level, int slot)
    {
        return table + kTableFirstRecord + (static_cast<std::uintptr_t>(level) * kSlotsPerLevel + slot) * kRecordSize;
    }

    std::int32_t& Stock(std::uint8_t* table, int level, int slot)
    {
        return *reinterpret_cast<std::int32_t*>(Record(table, level, slot) + kRecStock);
    }

    // Plain data and one call of the game's own level getter, so the fault guard can wrap it.
    // Returns 1 = a Raider's area destruction ended (seen is filled), 0 = not ours, -1 = fault.
    int StackRaw(std::uint8_t* table, std::int32_t slot, Seen* seen)
    {
        __try
        {
            if (slot < 0 || slot >= kSlotsPerLevel)
                return 0;
            const std::int32_t index = *reinterpret_cast<std::int32_t*>(table);
            if (index < 0)
                return 0;
            std::uint8_t* unit = *reinterpret_cast<std::uint8_t**>(g_Base + kUnitTableRva +
                static_cast<std::uintptr_t>(index) * 0x10);
            if (unit == nullptr || *reinterpret_cast<std::uint32_t*>(unit + kUnitIsRaider) != 1)
                return 0;
            void* holder = *reinterpret_cast<void**>(unit + kUnitLevelHolder);
            if (holder == nullptr)
                return 0;
            const std::uint32_t level = reinterpret_cast<LevelFn>(g_Base + kLevelGetterRva)(holder);
            if (level < 1 || level >= static_cast<std::uint32_t>(kLevelCount))
                return 0;
            seen->family = *reinterpret_cast<std::uint32_t*>(unit + kUnitFamily);
            seen->level = level;
            seen->slot = slot;
            seen->firstLevel = seen->family == kFamilyBroly ? 2 : 1;
            for (int l = 0; l < kLevelCount; ++l)
            {
                const std::uint8_t* rec = Record(table, l, slot);
                seen->before[l] = Stock(table, l, slot);
                seen->largest[l] = *reinterpret_cast<const std::uint32_t*>(rec + kRecLargestStock);
                seen->cooldownLength[l] = *reinterpret_cast<const float*>(rec + kRecCooldownLength);
                seen->cooldownLeft[l] = *reinterpret_cast<const float*>(rec + kRecCooldownLeft);
                seen->ready[l] = rec[kRecReady];
                seen->paused[l] = rec[kRecPaused];
                seen->mode[l] = rec[kRecMode];
            }
            const int now = static_cast<int>(level);
            // Only act on what the reading expects: this level holding 0 - 4, lower levels holding 0 or 1.
            // (This level can hold more than 0 here: the first destruction at level 4 may bring two at once.)
            seen->odd = seen->before[now] < 0 || seen->before[now] > 4;
            for (int l = seen->firstLevel + 1; l < now; ++l)
                seen->odd = seen->odd || seen->before[l] < 0 || seen->before[l] > 1;
            if (!seen->odd)
            {
                if (g_Bonus && now == 4)
                {
                    bool untouched = seen->firstLevel + 1 < now;
                    for (int l = seen->firstLevel + 1; l < now; ++l)
                        untouched = untouched && Stock(table, l, slot) == 1;
                    if (untouched)
                    {
                        Stock(table, now, slot) += 1;
                        seen->bonus = true;
                    }
                }
                for (int l = now - 1; l > seen->firstLevel; --l)
                {
                    if (Stock(table, l, slot) == 1)
                    {
                        Stock(table, l, slot) = 0;
                        Stock(table, now, slot) += 1;
                        seen->takenFrom = l;
                        break;
                    }
                }
            }
            // Safety limit: destroyed areas + uses left must never be more than the levels gained (+ the test
            // bonus). The areas: *(*(game + 0x6828) + 0x18), 24-byte entries from +0x18 to +0x20, entry + 8 = area
            // number (1 = A), entry + 0x12 = destroyed (the same list the Raider bot's area picker reads).
            const std::uintptr_t game = *reinterpret_cast<std::uintptr_t*>(g_Base + kGameGlobalPtrRva);
            const std::uintptr_t areaHolder = game != 0 ? *reinterpret_cast<std::uintptr_t*>(game + 0x6828) : 0;
            const std::uintptr_t manager = areaHolder != 0 ? *reinterpret_cast<std::uintptr_t*>(areaHolder + 0x18) : 0;
            if (manager != 0)
            {
                const std::uintptr_t begin = *reinterpret_cast<std::uintptr_t*>(manager + 0x18);
                const std::uintptr_t end = *reinterpret_cast<std::uintptr_t*>(manager + 0x20);
                if (end > begin && end - begin < 24 * 64)
                {
                    seen->destroyed = 0;
                    for (std::uintptr_t e = begin; e < end; e += 24)
                        if (*reinterpret_cast<std::uint32_t*>(e + 8) >= 1 && *reinterpret_cast<std::uint8_t*>(e + 0x12) != 0)
                            ++seen->destroyed;
                }
            }
            if (!seen->odd && seen->destroyed >= 0)
            {
                const int most = (now - seen->firstLevel) + (g_Bonus && now == 4 ? 1 : 0) - seen->destroyed;
                const int allowed = most > 0 ? most : 0;
                if (Stock(table, now, slot) > allowed)
                {
                    seen->cut = Stock(table, now, slot) - allowed;
                    Stock(table, now, slot) = allowed;
                }
            }
            for (int l = 0; l < kLevelCount; ++l)
                seen->after[l] = Stock(table, l, slot);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    std::string Levels(const std::int32_t* values)
    {
        std::string text;
        for (int l = 1; l < kLevelCount; ++l)
            text += (l > 1 ? " / " : "") + std::to_string(values[l]);
        return text;
    }

    std::uint64_t __fastcall ReleaseDetour(std::uint8_t* table, std::int32_t slot)
    {
        const std::uintptr_t fromRva = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_Base;
        const std::uint64_t result = g_OriginalRelease(table, slot);
        if (fromRva != kMapBreakEndReturnRva || table == nullptr)
            return result;
        Seen seen;
        const int done = StackRaw(table, slot, &seen);
        if (done < 0)
        {
            Logger::Warn("AreaDestroyStack: fault while reading the skill table - nothing changed");
            return result;
        }
        if (done == 0)
            return result;
        const int now = static_cast<int>(seen.level);
        std::string line = "AreaDestroyStack: area destruction ended (family " + std::to_string(seen.family) + ", level " +
            std::to_string(seen.level) + ", skill slot " + std::to_string(seen.slot) + "). Uses left by level 1 / 2 / 3 / 4: " +
            Levels(seen.before) + " -> " + Levels(seen.after) + ". ";
        if (seen.odd)
            line += "The numbers are not what was expected - NOTHING changed.";
        else if (seen.bonus || seen.takenFrom != 0)
            line += std::string(seen.takenFrom != 0 ? "Carried over 1 unused destruction from level " +
                std::to_string(seen.takenFrom) + ". " : "") +
                (seen.bonus ? "Test bonus: nothing was destroyed before level 4, +1. " : "");
        else
            line += "Nothing carried over this time. ";
        {
            // What is really left: this level's stock plus what lower levels still hold (each comes over after a use).
            int left = seen.after[now];
            for (int l = seen.firstLevel + 1; l < now; ++l)
                left += seen.after[l] > 0 ? seen.after[l] : 0;
            line += "The Raider can destroy " + std::to_string(left) + " more at this level.";
        }
        line += " Areas destroyed so far: " + (seen.destroyed >= 0 ? std::to_string(seen.destroyed) : std::string("?")) + ".";
        if (seen.cut > 0)
            line += " SAFETY LIMIT: " + std::to_string(seen.cut) + " use(s) taken back (more than the levels gained allow).";
        Logger::Info(line);
        static std::atomic<int> detailed{0};
        if (detailed.fetch_add(1) < 6)
        {
            for (int l = 1; l < kLevelCount; ++l)
            {
                char text[200];
                sprintf_s(text, "AreaDestroyStack:   level %d record: ready %u, cooldown %.2f of %.2f, stock %d, "
                    "largest stock %u, paused %u, mode %u", l, seen.ready[l], seen.cooldownLeft[l],
                    seen.cooldownLength[l], seen.before[l], seen.largest[l], seen.paused[l], seen.mode[l]);
                Logger::Info(text);
            }
        }
        return result;
    }
}

// ----------------------------------------------------------------------------------------- the void sky
// Project lead, 2026-11-17: "for the time being, when 4 areas are destroy, the sky box would be completely gone and
// there is no skybox, like an empty void".
// Read from the exe: the renderer has 60 draw layers ("sub scenes"), set up from a table at 0x1415EE910 (16 bytes
// each: name, flags): 0 First ... 19 Main Begin, 20 BG 2D, 21 VFX ClearBG, 22 Pre Main, 24 Chara0, 25 Main,
// 26 SkyBox, 27 Far Bg, 28 Vfx Bg ... Everything that draws first tells the engine its layer with
// XG::Eva::EmGeDeviceContext::SetPrioritySubScene(layer) (xgcore.dll, the game's import slot at game + 0x11629A0).
// EXPERIMENT (Possible_, not seen yet): while the void is on, a request for layer 26 is turned into a request for
// raider_destroy_void_layer (default 0 = "First", which runs before the frame is cleared), so the sky is drawn
// where it is wiped again. If the picture shows something else, try another layer number in the settings.
// The void is on when raider_destroy_void_areas (default 4) areas are destroyed; raider_destroy_void_key (a
// virtual key, 0 = none) flips it by hand for looking at it.
namespace
{
    constexpr std::uintptr_t kSubSceneImportRva = 0x11629A0;    // IAT slot of EmGeDeviceContext::SetPrioritySubScene
    constexpr std::uint32_t kSubSceneSkyBox = 26;

    using SubSceneFn = void(__fastcall*)(void* context, std::uint32_t layer);
    SubSceneFn g_OriginalSubScene = nullptr;
    std::atomic<bool> g_VoidNow{false};
    std::atomic<std::uint32_t> g_SkyCalls{0};                   // layer 26 requests seen (to know the sky uses it)
    std::atomic<std::uint32_t> g_SkyMoved{0};
    std::uint32_t g_VoidLayer = 0;
    int g_VoidAreas = 4;
    int g_VoidKey = 0;

    void __fastcall SubSceneDetour(void* context, std::uint32_t layer)
    {
        if (layer == kSubSceneSkyBox)
        {
            g_SkyCalls.fetch_add(1, std::memory_order_relaxed);
            if (g_VoidNow.load(std::memory_order_relaxed))
            {
                g_SkyMoved.fetch_add(1, std::memory_order_relaxed);
                layer = g_VoidLayer;
            }
        }
        g_OriginalSubScene(context, layer);
    }

    // First test (2026-11-17 00:20 - 00:25, 4 areas destroyed, void flag ON): the game never asked for layer 26
    // through SetPrioritySubScene, so nothing changed on screen. Second try: the same for the engine's other way of
    // naming a layer, EmGeDeviceContext::ApplyPrioritySet(scene, sub scene, pass, user) (import slot game + 0x1162458).
    constexpr std::uintptr_t kPrioritySetImportRva = 0x1162458;
    using PrioritySetFn = void(__fastcall*)(void* context, std::uint32_t scene, std::uint32_t layer, std::uint32_t pass,
        std::uint32_t user);
    PrioritySetFn g_OriginalPrioritySet = nullptr;
    std::atomic<std::uint32_t> g_SkySetCalls{0};

    void __fastcall PrioritySetDetour(void* context, std::uint32_t scene, std::uint32_t layer, std::uint32_t pass,
        std::uint32_t user)
    {
        if (layer == kSubSceneSkyBox)
        {
            g_SkySetCalls.fetch_add(1, std::memory_order_relaxed);
            if (g_VoidNow.load(std::memory_order_relaxed))
            {
                g_SkyMoved.fetch_add(1, std::memory_order_relaxed);
                layer = g_VoidLayer;
            }
        }
        g_OriginalPrioritySet(context, scene, layer, pass, user);
    }

    // Plain data, so the fault guard can wrap it: areas marked destroyed, -1 = no match / not readable.
    int DestroyedAreasRaw()
    {
        __try
        {
            const std::uintptr_t game = *reinterpret_cast<std::uintptr_t*>(g_Base + kGameGlobalPtrRva);
            const std::uintptr_t areaHolder = game != 0 ? *reinterpret_cast<std::uintptr_t*>(game + 0x6828) : 0;
            const std::uintptr_t manager = areaHolder != 0 ? *reinterpret_cast<std::uintptr_t*>(areaHolder + 0x18) : 0;
            if (manager == 0)
                return -1;
            const std::uintptr_t begin = *reinterpret_cast<std::uintptr_t*>(manager + 0x18);
            const std::uintptr_t end = *reinterpret_cast<std::uintptr_t*>(manager + 0x20);
            if (!(end > begin && end - begin < 24 * 64))
                return -1;
            int destroyed = 0;
            for (std::uintptr_t e = begin; e < end; e += 24)
                if (*reinterpret_cast<std::uint32_t*>(e + 8) >= 1 && *reinterpret_cast<std::uint8_t*>(e + 0x12) != 0)
                    ++destroyed;
            return destroyed;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    DWORD WINAPI VoidThread(LPVOID)
    {
        bool flipped = false, keyWasDown = false, byAreas = false, said = false;
        int lastDestroyed = -2;
        for (int tick = 0;; ++tick)
        {
            Sleep(100);
            if (g_VoidKey != 0)
            {
                const bool down = (GetAsyncKeyState(g_VoidKey) & 0x8000) != 0;
                DWORD pid = 0;
                HWND front = GetForegroundWindow();
                if (down && !keyWasDown && front != nullptr && GetWindowThreadProcessId(front, &pid) != 0 &&
                    pid == GetCurrentProcessId())
                {
                    flipped = !flipped;
                    Logger::Info(std::string("AreaDestroyStack: void sky test key pressed - the void is now ") +
                        (flipped != byAreas ? "ON" : "off") + " (sky layer requests seen so far: " +
                        std::to_string(g_SkyCalls.load()) + ", moved: " + std::to_string(g_SkyMoved.load()) + ")");
                }
                keyWasDown = down;
            }
            if (tick % 5 == 0)
            {
                const int destroyed = DestroyedAreasRaw();
                if (destroyed != lastDestroyed)
                {
                    lastDestroyed = destroyed;
                    const bool now = destroyed >= g_VoidAreas;
                    if (now != byAreas)
                        Logger::Info("AreaDestroyStack: " + std::to_string(destroyed) + " areas destroyed - void sky " +
                            (now ? "ON" : "off"));
                    byAreas = now;
                    if (destroyed <= 0)
                        flipped = false;                        // a new match starts with the game's own sky
                }
            }
            g_VoidNow.store(flipped != byAreas);
            if (!said && (g_SkyCalls.load() != 0 || g_SkySetCalls.load() != 0))
            {
                said = true;
                Logger::Info("AreaDestroyStack: the game draws into the SkyBox layer (26) - the void sky can act on it "
                    "(SetPrioritySubScene: " + std::to_string(g_SkyCalls.load()) + ", ApplyPrioritySet: " +
                    std::to_string(g_SkySetCalls.load()) + ")");
            }
            if (tick % 300 == 299 && !said)
                Logger::Info("AreaDestroyStack: void sky - still no request for the SkyBox layer (26) from the game");
        }
    }

    bool InitVoid(const std::string& config)
    {
        if (GetPrivateProfileIntA("steam", "raider_destroy_void", 0, config.c_str()) == 0)
            return true;
        g_VoidAreas = GetPrivateProfileIntA("steam", "raider_destroy_void_areas", 4, config.c_str());
        g_VoidKey = GetPrivateProfileIntA("steam", "raider_destroy_void_key", 0, config.c_str());
        if (g_VoidKey < 0 || g_VoidKey > 254)
            g_VoidKey = 0;
        g_VoidLayer = GetPrivateProfileIntA("steam", "raider_destroy_void_layer", 0, config.c_str());
        if (g_VoidLayer >= 60)
            g_VoidLayer = 0;
        // The engine function behind the game's import slot; it has to be inside xgcore.dll.
        void* target = *reinterpret_cast<void**>(g_Base + kSubSceneImportRva);
        HMODULE owner = nullptr;
        char name[MAX_PATH] = {};
        if (target == nullptr || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(target), &owner) ||
            GetModuleFileNameA(owner, name, MAX_PATH) == 0 || StrStrIA(name, "xgcore.dll") == nullptr)
        {
            Logger::Error(std::string("AreaDestroyStack: void sky - the import slot does not point into xgcore.dll (") +
                name + ") - not hooked");
            return false;
        }
        if (!HookManager::Create(target, reinterpret_cast<void*>(&SubSceneDetour),
                reinterpret_cast<void**>(&g_OriginalSubScene)) || !HookManager::Enable(target))
        {
            Logger::Error("AreaDestroyStack: void sky - hook on EmGeDeviceContext::SetPrioritySubScene failed");
            return false;
        }
        void* setTarget = *reinterpret_cast<void**>(g_Base + kPrioritySetImportRva);
        HMODULE setOwner = nullptr;
        char setName[MAX_PATH] = {};
        if (setTarget != nullptr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(setTarget), &setOwner) &&
            GetModuleFileNameA(setOwner, setName, MAX_PATH) != 0 && StrStrIA(setName, "xgcore.dll") != nullptr &&
            HookManager::Create(setTarget, reinterpret_cast<void*>(&PrioritySetDetour),
                reinterpret_cast<void**>(&g_OriginalPrioritySet)) && HookManager::Enable(setTarget))
            Logger::Info("AreaDestroyStack: void sky - also watching EmGeDeviceContext::ApplyPrioritySet");
        else
            Logger::Warn("AreaDestroyStack: void sky - EmGeDeviceContext::ApplyPrioritySet not hooked");
        HANDLE thread = CreateThread(nullptr, 0, VoidThread, nullptr, 0, nullptr);
        if (thread != nullptr)
            CloseHandle(thread);
        Logger::Info("AreaDestroyStack: void sky ready (raider_destroy_void=1): on at " + std::to_string(g_VoidAreas) +
            " destroyed areas, sky layer 26 moved to layer " + std::to_string(g_VoidLayer) + ", test key " +
            std::to_string(g_VoidKey));
        return thread != nullptr;
    }
}

bool AreaDestroyStack::Init()
{
    const std::string& config = SteamConfig::GetPath();
    g_Base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_Base == 0)
        return false;
    const bool voidOk = InitVoid(config);
    if (GetPrivateProfileIntA("steam", "raider_destroy_stack", 0, config.c_str()) == 0)
        return voidOk;
    g_Bonus = GetPrivateProfileIntA("steam", "raider_destroy_bonus", 0, config.c_str()) != 0;
    void* target = reinterpret_cast<void*>(g_Base + kSkillReleaseRva);
    if (!HookManager::Create(target, reinterpret_cast<void*>(&ReleaseDetour),
            reinterpret_cast<void**>(&g_OriginalRelease)) || !HookManager::Enable(target))
    {
        Logger::Error("AreaDestroyStack: hook on sub_14031d990 (skill released) failed");
        return false;
    }
    Logger::Info(std::string("AreaDestroyStack: hook on sub_14031d990 installed - unused area destructions carry over "
        "to the next level (raider_destroy_stack=1); test bonus of a 4th at level 4 ") +
        (g_Bonus ? "ON (raider_destroy_bonus=1)" : "off (raider_destroy_bonus=0)"));
    return voidOk;
}
