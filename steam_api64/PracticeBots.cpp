#include "StdInc.h"
#include "PracticeBots.h"
#include "HookManager.h"
#include "Logger.h"
#include "SteamConfig.h"

// Experimental: add survivor bots to practice matches (2026-11-11).
//
// Findings in the client (DRAGON BALL THE BREAKERS.exe, sha256 04879652..., base 0x140000000):
// - The practice lobby (GamePhaseLobby sub_14067B260) adds the Raider bot when no player is the Raider:
//   sub_1408BF360(slots = *(0x14177E470), slot 7, ...), which calls sub_1408C0B70(slots, slot, kind 1, 100).
// - sub_1408C0B70 kind 0 = an unused "avatar" (survivor) bot slot: present, role 0, +0x48 = 2, default
//   AvatarVisual. Slot fields (relative to slots, n = slot index, stride 0xCD0):
//     +0x38 present, +0x3C role (0 survivor, 1 raider), +0x40 control (AI attached when 0 or 1),
//     +0x44 AI mode (-> character+0x200), +0x48 kind (1 rival, 2 avatar), +0x50 name (std::string),
//     +0xD04 (non-zero skips the AI hookup in the battle spawn sub_1406622F0).
// - AI mode 6/7 picks SimpleAIOrientation (brain chooser sub_1401FC5E0): the tutorial's full autonomous
//   AI; for role 0 it runs the survivor states (sub_1402C12D0) and planner (sub_1402BE880).
// - That AI calls sub_1408924D0(*(slots + 0x6828)) every frame without a null check; the object at +0x6828
//   is the tutorial manager. Guarded below so a missing manager returns "none" instead of crashing.

namespace
{
    constexpr std::uintptr_t kGameGlobalPtrRva = 0x177E470;   // XG::Game global holding the 8 player slots
    constexpr std::uintptr_t kAddRaiderBotRva = 0x8BF360;     // sub_1408BF360
    constexpr std::uintptr_t kMakeBotSlotRva = 0x8C0B70;      // sub_1408C0B70
    constexpr std::uintptr_t kTutorialGetRva = 0x8924D0;      // sub_1408924D0
    constexpr std::uintptr_t kStringAssignRva = 0x558F0;      // sub_1400558F0 (std::string assign)

    constexpr std::uintptr_t kSlotStride = 0xCD0;
    constexpr std::uintptr_t kSlotPresent = 0x38;
    constexpr std::uintptr_t kSlotRole = 0x3C;
    constexpr std::uintptr_t kSlotControl = 0x40;
    constexpr std::uintptr_t kSlotAiMode = 0x44;
    constexpr std::uintptr_t kSlotKind = 0x48;
    constexpr std::uintptr_t kSlotName = 0x50;
    constexpr std::uintptr_t kSlotSkipAi = 0xD04;
    constexpr std::uintptr_t kTutorialManager = 0x6828;

    constexpr std::uint32_t kSurvivorSlot = 6;
    constexpr std::uint32_t kAiModeOrientation = 6;

    using AddRaiderBotFn = void(__fastcall*)(std::uintptr_t, std::uint32_t, std::uint32_t, std::uint8_t, std::uint8_t);
    using MakeBotSlotFn = void(__fastcall*)(std::uintptr_t, std::uint32_t, std::uint32_t, std::uint32_t);
    using TutorialGetFn = std::uintptr_t(__fastcall*)(std::uintptr_t);
    using StringAssignFn = void*(__fastcall*)(void*, const char*, std::size_t);

    std::uintptr_t g_Base = 0;
    void* g_AddRaiderBotTarget = nullptr;
    void* g_TutorialGetTarget = nullptr;
    AddRaiderBotFn g_OriginalAddRaiderBot = nullptr;
    TutorialGetFn g_OriginalTutorialGet = nullptr;

    template <typename T>
    T& At(std::uintptr_t address) { return *reinterpret_cast<T*>(address); }

    std::string Hex(std::uintptr_t value)
    {
        std::ostringstream text;
        text << "0x" << std::hex << std::uppercase << value;
        return text.str();
    }

    std::uintptr_t __fastcall GuardedTutorialGet(std::uintptr_t manager)
    {
        if (manager == 0)
            return 0;
        return g_OriginalTutorialGet(manager);
    }

    void AddSurvivorBot(std::uintptr_t slots)
    {
        const std::uintptr_t slot = slots + kSurvivorSlot * kSlotStride;
        if (At<std::uint8_t>(slot + kSlotPresent) != 0)
        {
            Logger::Info("PracticeBots: slot " + std::to_string(kSurvivorSlot) + " is taken, no survivor bot added");
            return;
        }

        auto makeBotSlot = reinterpret_cast<MakeBotSlotFn>(g_Base + kMakeBotSlotRva);
        makeBotSlot(slots, kSurvivorSlot, 0 /* avatar */, 100);

        At<std::uint32_t>(slot + kSlotRole) = 0;
        At<std::uint32_t>(slot + kSlotControl) = 1;
        At<std::uint32_t>(slot + kSlotAiMode) = kAiModeOrientation;
        At<std::uint8_t>(slot + kSlotSkipAi) = 0;

        auto assign = reinterpret_cast<StringAssignFn>(g_Base + kStringAssignRva);
        static const char kName[] = "SURVIVOR BOT";
        assign(reinterpret_cast<void*>(slot + kSlotName), kName, sizeof(kName) - 1);

        Logger::Info(
            "PracticeBots: survivor bot in slot " + std::to_string(kSurvivorSlot) +
            " (present=" + std::to_string(At<std::uint8_t>(slot + kSlotPresent)) +
            " role=" + std::to_string(At<std::uint32_t>(slot + kSlotRole)) +
            " control=" + std::to_string(At<std::uint32_t>(slot + kSlotControl)) +
            " aiMode=" + std::to_string(At<std::uint32_t>(slot + kSlotAiMode)) +
            " kind=" + std::to_string(At<std::uint32_t>(slot + kSlotKind)) + ")");
    }

    void __fastcall AddRaiderBotDetour(
        std::uintptr_t slots, std::uint32_t slotIndex, std::uint32_t a3, std::uint8_t a4, std::uint8_t a5)
    {
        g_OriginalAddRaiderBot(slots, slotIndex, a3, a4, a5);

        const std::uintptr_t raider = slots + slotIndex * kSlotStride;
        Logger::Info(
            "PracticeBots: raider bot slot " + std::to_string(slotIndex) +
            " control=" + std::to_string(At<std::uint32_t>(raider + kSlotControl)) +
            " aiMode=" + std::to_string(At<std::uint32_t>(raider + kSlotAiMode)) +
            " skipAi=" + std::to_string(At<std::uint8_t>(raider + kSlotSkipAi)) +
            " tutorialManager=" + Hex(At<std::uintptr_t>(slots + kTutorialManager)));

        try
        {
            AddSurvivorBot(slots);
        }
        catch (...)
        {
            Logger::Error("PracticeBots: adding the survivor bot threw");
        }
        Logger::Flush();
    }

    bool Hook(std::uintptr_t rva, void* detour, void** original, void** target, const char* name)
    {
        *target = reinterpret_cast<void*>(g_Base + rva);
        if (!HookManager::Create(*target, detour, original) || !HookManager::Enable(*target))
        {
            Logger::Error(std::string("PracticeBots: failed to hook ") + name);
            *target = nullptr;
            *original = nullptr;
            return false;
        }
        Logger::Info(std::string("PracticeBots: hooked ") + name);
        return true;
    }
}

bool PracticeBots::Init()
{
    const int enabled = GetPrivateProfileIntA("steam", "survivor_bots", 0, SteamConfig::GetPath().c_str());
    if (enabled == 0)
    {
        Logger::Info("PracticeBots: off (set survivor_bots=1 in steam_config.ini to try survivor bots)");
        return true;
    }

    g_Base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_Base == 0)
        return false;

    const bool guard = Hook(kTutorialGetRva, reinterpret_cast<void*>(&GuardedTutorialGet),
        reinterpret_cast<void**>(&g_OriginalTutorialGet), &g_TutorialGetTarget, "sub_1408924D0 (null guard)");
    const bool add = Hook(kAddRaiderBotRva, reinterpret_cast<void*>(&AddRaiderBotDetour),
        reinterpret_cast<void**>(&g_OriginalAddRaiderBot), &g_AddRaiderBotTarget, "sub_1408BF360 (add raider bot)");
    return guard && add;
}

void PracticeBots::Shutdown()
{
    if (g_AddRaiderBotTarget)
        HookManager::Disable(g_AddRaiderBotTarget);
    if (g_TutorialGetTarget)
        HookManager::Disable(g_TutorialGetTarget);
    g_AddRaiderBotTarget = nullptr;
    g_TutorialGetTarget = nullptr;
}
