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
    constexpr std::uintptr_t kLobbyStartRva = 0x67B260;       // sub_14067B260 (GamePhaseLobby: start the match)
    constexpr std::uintptr_t kLobbyPhaseState = 0x308;        // GamePhaseLobby+0x308, set to 16 when the match starts
    constexpr std::uintptr_t kPracticeFlag = 0x28;            // slots+0x28: practice room
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

    constexpr std::uint32_t kSlotCount = 8;
    constexpr std::uint32_t kAiModeOrientation = 6;

    using LobbyStartFn = void(__fastcall*)(std::uintptr_t);
    using MakeBotSlotFn = void(__fastcall*)(std::uintptr_t, std::uint32_t, std::uint32_t, std::uint32_t);
    using TutorialGetFn = std::uintptr_t(__fastcall*)(std::uintptr_t);
    using StringAssignFn = void*(__fastcall*)(void*, const char*, std::size_t);

    std::uintptr_t g_Base = 0;
    int g_BotCount = 0;
    void* g_LobbyStartTarget = nullptr;
    void* g_TutorialGetTarget = nullptr;
    LobbyStartFn g_OriginalLobbyStart = nullptr;
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

    void AddSurvivorBot(std::uintptr_t slots, std::uint32_t index, int number)
    {
        const std::uintptr_t slot = slots + index * kSlotStride;

        auto makeBotSlot = reinterpret_cast<MakeBotSlotFn>(g_Base + kMakeBotSlotRva);
        makeBotSlot(slots, index, 0 /* avatar */, 100);

        At<std::uint32_t>(slot + kSlotRole) = 0;
        At<std::uint32_t>(slot + kSlotControl) = 1;
        At<std::uint32_t>(slot + kSlotAiMode) = kAiModeOrientation;
        At<std::uint8_t>(slot + kSlotSkipAi) = 0;

        auto assign = reinterpret_cast<StringAssignFn>(g_Base + kStringAssignRva);
        const std::string name = "SURVIVOR BOT " + std::to_string(number);
        assign(reinterpret_cast<void*>(slot + kSlotName), name.c_str(), name.size());

        Logger::Info(
            "PracticeBots: survivor bot in slot " + std::to_string(index) +
            " (present=" + std::to_string(At<std::uint8_t>(slot + kSlotPresent)) +
            " role=" + std::to_string(At<std::uint32_t>(slot + kSlotRole)) +
            " control=" + std::to_string(At<std::uint32_t>(slot + kSlotControl)) +
            " aiMode=" + std::to_string(At<std::uint32_t>(slot + kSlotAiMode)) +
            " kind=" + std::to_string(At<std::uint32_t>(slot + kSlotKind)) + ")");
    }

    // The lobby phase function runs every frame; the match starts in the call that sets +0x308 to 16.
    // By then the game has added the Raider bot (slot 7) if no player is the Raider. Survivor bots go into
    // the free slots after that, so this works whether the player is a survivor or the Raider.
    void __fastcall LobbyStartDetour(std::uintptr_t lobby)
    {
        const std::uint32_t before = At<std::uint32_t>(lobby + kLobbyPhaseState);
        g_OriginalLobbyStart(lobby);
        const std::uint32_t after = At<std::uint32_t>(lobby + kLobbyPhaseState);
        if (before == 16 || after != 16)
            return;

        const std::uintptr_t slots = At<std::uintptr_t>(g_Base + kGameGlobalPtrRva);
        if (slots == 0 || At<std::uint8_t>(slots + kPracticeFlag) == 0)
            return;

        try
        {
            std::string taken;
            for (std::uint32_t i = 0; i < kSlotCount; ++i)
            {
                const std::uintptr_t slot = slots + i * kSlotStride;
                if (At<std::uint8_t>(slot + kSlotPresent) != 0)
                    taken += " " + std::to_string(i) + "(role " + std::to_string(At<std::uint32_t>(slot + kSlotRole)) +
                        ", control " + std::to_string(At<std::uint32_t>(slot + kSlotControl)) +
                        ", aiMode " + std::to_string(At<std::uint32_t>(slot + kSlotAiMode)) + ")";
            }
            Logger::Info("PracticeBots: practice match starting; taken slots:" + taken +
                "; tutorialManager=" + Hex(At<std::uintptr_t>(slots + kTutorialManager)));

            int added = 0;
            for (std::uint32_t i = 1; i < kSlotCount && added < g_BotCount; ++i)
            {
                if (At<std::uint8_t>(slots + i * kSlotStride + kSlotPresent) != 0)
                    continue;
                AddSurvivorBot(slots, i, added + 1);
                ++added;
            }
            Logger::Info("PracticeBots: added " + std::to_string(added) + " survivor bot(s)");
        }
        catch (...)
        {
            Logger::Error("PracticeBots: adding survivor bots threw");
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
    // survivor_bots = how many survivor bots to add to a practice match (0 = off, at most 7).
    g_BotCount = GetPrivateProfileIntA("steam", "survivor_bots", 0, SteamConfig::GetPath().c_str());
    if (g_BotCount <= 0)
    {
        Logger::Info("PracticeBots: off (set survivor_bots=1..7 in BreakersRevived\\steam_config.ini)");
        return true;
    }
    if (g_BotCount > 7)
        g_BotCount = 7;
    Logger::Info("PracticeBots: up to " + std::to_string(g_BotCount) + " survivor bot(s) per practice match");

    g_Base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_Base == 0)
        return false;

    const bool guard = Hook(kTutorialGetRva, reinterpret_cast<void*>(&GuardedTutorialGet),
        reinterpret_cast<void**>(&g_OriginalTutorialGet), &g_TutorialGetTarget, "sub_1408924D0 (null guard)");
    const bool add = Hook(kLobbyStartRva, reinterpret_cast<void*>(&LobbyStartDetour),
        reinterpret_cast<void**>(&g_OriginalLobbyStart), &g_LobbyStartTarget, "sub_14067B260 (lobby match start)");
    return guard && add;
}

void PracticeBots::Shutdown()
{
    if (g_LobbyStartTarget)
        HookManager::Disable(g_LobbyStartTarget);
    if (g_TutorialGetTarget)
        HookManager::Disable(g_TutorialGetTarget);
    g_LobbyStartTarget = nullptr;
    g_TutorialGetTarget = nullptr;
}
