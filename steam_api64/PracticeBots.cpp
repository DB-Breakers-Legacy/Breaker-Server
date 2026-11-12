#include "StdInc.h"
#include "PracticeBots.h"
#include "HookManager.h"
#include "Logger.h"
#include "SteamConfig.h"
#include <intrin.h>

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
    constexpr std::uintptr_t kAddRaiderBotRva = 0x8BF360;     // sub_1408BF360 (lobby adds the Raider bot to slot 7)
    constexpr std::uintptr_t kRivalIdRva = 0xC60F70;          // sub_140C60F70() -> the value the lobby passes as a3
    constexpr std::uintptr_t kRivalOptionRva = 0xC60FD0;      // sub_140C60FD0(1, 0) -> the value the lobby passes as a4
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

    // Looks. A slot's look container is at +0x148; the game copies an AvatarVisual into it with
    // sub_140707830(slot + 0x148, avatarVisual), which takes the AvatarVisual fields +0x08..+0x440 (plain data)
    // into container +0x28.. - i.e. the slot's own AvatarVisual data sits at slot + 0x168.
    constexpr std::uintptr_t kCopyLookRva = 0x707830;          // sub_140707830
    constexpr std::uintptr_t kSlotLookContainer = 0x148;
    constexpr std::uintptr_t kSlotLookData = 0x168;            // = container + 0x20, same layout as an AvatarVisual
    constexpr std::size_t kLookSize = 0x440;                   // AvatarVisual bytes used by the copy
    constexpr std::uintptr_t kLocalPlayerSlot = 0x66B8;        // slots + 0x66B8 = the local player's slot index

    // Follower. The tutorial brain (SimpleAIOrientation) has a "follow a character" state 30000 (0x7530,
    // sub_1402C08C0 case 0: target handle at AI+0x184C) that ends after ~2 s. The follower bot is sent back
    // into it whenever it is more than kFollowDistance from the player.
    constexpr std::uintptr_t kAiUpdateRva = 0x2BE6E0;          // SimpleAIOrientation update (vtable slot 1)
    constexpr std::uintptr_t kCharacterTableRva = 0x33B9600;   // handle table: 16-byte entries, character* at +0
    constexpr std::uint32_t kCharacterTableSize = 0x2000;
    constexpr std::uintptr_t kAiCharacterHandle = 0x14;
    constexpr std::uintptr_t kAiState = 0x1784;
    constexpr std::uintptr_t kAiNextState = 0x178C;
    constexpr std::uintptr_t kAiFollowTarget = 0x184C;
    constexpr std::uint32_t kStateFollow = 0x7530;
    // Survivor state 10001 (sub_1402C12D0 case 1): approach the character at AI+0x184C - sets the behaviour
    // mask 0x2FF, walks there with a MoveTarget action (sub_1402BC870 at a point from sub_1402CCBE0), then
    // switches to 30000 when in range. 30000 alone does not walk (its approach action ends at once).
    constexpr std::uint32_t kStateApproach = 0x2711;
    constexpr std::uint32_t kStateDowned = 0x2738;
    constexpr std::uintptr_t kCharSlot = 0x1F4;                 // player slot (UnitCharacter ctor arg)
    constexpr std::uintptr_t kCharRole = 0x204;                 // 0 survivor, 1 raider
    // Real characters only: the handle table also holds items, pods, vendors... whose fields at +0x1F4 can
    // look like a slot number (2026-11-11: an item was taken for the player and the game crashed).
    constexpr std::uintptr_t kMobVftableRva = 0x11787A0;        // XG::Game::Mob (survivors, player and bots)
    constexpr std::uintptr_t kRivalVftableRva = 0x1177F40;      // XG::Game::BattleRival (the Raider)
    constexpr std::uintptr_t kCharPosition = 0x60;              // x, y, z floats
    constexpr std::uintptr_t kCharFlags = 0xA18;                // 0x20000000 = downed
    constexpr float kFollowDistance = 5.0f;

    constexpr std::uint32_t kSlotCount = 8;
    constexpr std::uint32_t kAiModeOrientation = 6;

    using LobbyStartFn = void(__fastcall*)(std::uintptr_t);
    using AddRaiderBotFn = void(__fastcall*)(std::uintptr_t, std::uint32_t, std::uint32_t, std::uint8_t, std::uint8_t);
    using MakeBotSlotFn = void(__fastcall*)(std::uintptr_t, std::uint32_t, std::uint32_t, std::uint32_t);
    using TutorialGetFn = std::uintptr_t(__fastcall*)(std::uintptr_t);
    using StringAssignFn = void*(__fastcall*)(void*, const char*, std::size_t);
    using CopyLookFn = std::uint8_t(__fastcall*)(std::uintptr_t, const void*);
    using AiUpdateFn = std::uint64_t(__fastcall*)(std::uintptr_t, float);

    std::uintptr_t g_Base = 0;
    int g_BotCount = 0;
    bool g_RaiderBot = true;
    // Brain mode. The chooser (sub_1401FC5E0) gives AI mode 6 the tutorial mode 0 (+0x1788 = 0, the guide
    // handler sub_1402C4810 that leads the player to objectives - bots ran ahead and "followed" crates with an
    // approach-a-character action that never finishes) and AI mode 7 mode 3. Our bots get -1 (no tutorial
    // mode): only the free survivor planner runs.
    std::uint8_t g_BotSlotMask = 0;
    bool g_FreeMode = false;
    bool g_CasualBots = false;   // casual_bots=1: also fill normal-matchmaking matches (server autofill)
    int g_FollowerBot = 0;                      // survivor_bot_follow: which bot (1-7) follows the player, 0 = none
    std::uint32_t g_FollowerSlot = 0xFFFFFFFF;   // slot of that bot in the current match
    std::uint32_t g_PlayerHandle = 0;
    void* g_AiUpdateTarget = nullptr;
    AiUpdateFn g_OriginalAiUpdate = nullptr;
    std::array<std::string, 7> g_BotLooks;   // survivor_bot_look1..7 = name of a .look file in BreakersRevived\looks
    void* g_AddRaiderBotTarget = nullptr;
    AddRaiderBotFn g_OriginalAddRaiderBot = nullptr;
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

    std::filesystem::path LooksFolder()
    {
        return std::filesystem::path(SteamConfig::GetPath()).parent_path() / "looks";
    }

    // Saves the local player's current look so it can be given to a bot (e.g. wear the Bulma skin, start a
    // practice match, rename the capture to bulma.look).
    void CapturePlayerLook(std::uintptr_t slots)
    {
        const std::uint32_t player = At<std::uint32_t>(slots + kLocalPlayerSlot);
        if (player >= 8)
            return;
        const auto* look = reinterpret_cast<const char*>(slots + player * kSlotStride + kSlotLookData);
        try
        {
            std::filesystem::create_directories(LooksFolder());
            const auto path = LooksFolder() / "last_player_look.look";
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(look, kLookSize);
            Logger::Info("PracticeBots: saved your current look (slot " + std::to_string(player) + ") to " + path.string());
        }
        catch (...)
        {
            Logger::Error("PracticeBots: could not save the player look");
        }
    }

    // Gives a bot a saved look: start from the bot's own look data (valid header), overwrite the AvatarVisual
    // fields +0x08.. with the saved bytes, and let the game copy it into the slot.
    void ApplyLook(std::uintptr_t slots, std::uint32_t index, const std::string& name)
    {
        if (name.empty())
            return;
        const auto path = LooksFolder() / (name + ".look");
        std::ifstream in(path, std::ios::binary);
        std::vector<char> saved(kLookSize);
        if (!in || !in.read(saved.data(), static_cast<std::streamsize>(kLookSize)))
        {
            Logger::Error("PracticeBots: look '" + name + "' not found or too short: " + path.string());
            return;
        }

        const std::uintptr_t slot = slots + index * kSlotStride;
        alignas(16) std::array<char, kLookSize> buffer{};
        std::memcpy(buffer.data(), reinterpret_cast<const void*>(slot + kSlotLookData), kLookSize);
        std::memcpy(buffer.data() + 8, saved.data() + 8, kLookSize - 8);

        auto copyLook = reinterpret_cast<CopyLookFn>(g_Base + kCopyLookRva);
        copyLook(slot + kSlotLookContainer, buffer.data());
        Logger::Info("PracticeBots: bot in slot " + std::to_string(index) + " wears look '" + name + "'");
    }

    std::uintptr_t CharacterAt(std::uint32_t handle)
    {
        if (handle == 0 || handle >= kCharacterTableSize)
            return 0;
        return At<std::uintptr_t>(g_Base + kCharacterTableRva + static_cast<std::uintptr_t>(handle) * 16);
    }

    std::string ClassRva(std::uintptr_t object)
    {
        if (object == 0)
            return "none";
        return Hex(At<std::uintptr_t>(object) - g_Base + 0x140000000);
    }

    bool IsCharacter(std::uintptr_t c)
    {
        if (c == 0)
            return false;
        const std::uintptr_t vtable = At<std::uintptr_t>(c);
        return vtable == g_Base + kMobVftableRva || vtable == g_Base + kRivalVftableRva;
    }

    // The local player's character, from the game's own slot lookup sub_140751860(slots+0x6830, slot), and its
    // handle in the object table (found by its exact address; cached). Characters keep -1 at +0x48, so the
    // handle cannot be read from the character itself.
    constexpr std::uintptr_t kCharacterBySlotRva = 0x751860;   // sub_140751860(manager, slot) -> UnitCharacter*
    constexpr std::uintptr_t kCharacterManager = 0x6830;       // slots + 0x6830
    constexpr std::uintptr_t kHandleMoveRva = 0x7E830;         // sub_14007E830(dst, &src): counted handle move
    using CharacterBySlotFn = std::uintptr_t(__fastcall*)(std::uintptr_t, std::uint64_t);
    using HandleMoveFn = std::uintptr_t(__fastcall*)(std::uint32_t*, std::uint32_t*);

    std::uintptr_t FindPlayer()
    {
        const std::uintptr_t slots = At<std::uintptr_t>(g_Base + kGameGlobalPtrRva);
        if (slots == 0 || At<std::uintptr_t>(slots + kCharacterManager) == 0)
            return 0;
        const std::uint32_t playerSlot = At<std::uint32_t>(slots + kLocalPlayerSlot);
        const auto bySlot = reinterpret_cast<CharacterBySlotFn>(g_Base + kCharacterBySlotRva);
        const std::uintptr_t player = bySlot(At<std::uintptr_t>(slots + kCharacterManager), playerSlot);
        if (player == 0)
            return 0;
        if (CharacterAt(g_PlayerHandle) == player)
            return player;

        for (std::uint32_t h = 1; h < kCharacterTableSize; ++h)
        {
            if (CharacterAt(h) == player)
            {
                g_PlayerHandle = h;
                Logger::Info("PracticeBots: player character handle " + std::to_string(h) +
                    " (slot " + std::to_string(playerSlot) + ", " + ClassRva(player) + ")");
                Logger::Flush();
                return player;
            }
        }
        g_PlayerHandle = 0;
        return 0;
    }

    // Store a handle in an AI field the way the game does: take one reference on the table entry
    // (entry +8 is the count, cf. sub_140682C20), then move it in with sub_14007E830, which releases the old one.
    void AssignHandle(std::uintptr_t field, std::uint32_t handle)
    {
        if (At<std::uint32_t>(field) == handle)
            return;
        auto* count = reinterpret_cast<volatile long*>(g_Base + kCharacterTableRva + static_cast<std::uintptr_t>(handle) * 16 + 8);
        InterlockedIncrement(count);
        std::uint32_t temp = handle;
        reinterpret_cast<HandleMoveFn>(g_Base + kHandleMoveRva)(reinterpret_cast<std::uint32_t*>(field), &temp);
    }

    // Bot status report (survivor_bot_trace=1): every 2 s per bot, what its brain and current action are doing.
    // SimpleAI: +0x18 current action. Interact action (SimpleAIActionOrientationInteract): +0x1700 step,
    // +0x1704 result (1 running, 2 failed, 3 done), +0x170C seconds in the action, +0x1790 target kind,
    // +0x1794 target object handle (object +0x178 = object type, read by sub_1402BA1E0).
    bool g_Trace = false;
    std::unordered_map<std::uintptr_t, std::uint64_t> g_LastTrace;


    void TraceBot(std::uintptr_t ai, std::uintptr_t bot)
    {
        const std::uint64_t now = GetTickCount64();
        auto& last = g_LastTrace[ai];
        if (now - last < 2000)
            return;
        last = now;

        std::ostringstream line;
        line << "PracticeBots trace: slot " << At<std::uint32_t>(bot + kCharSlot)
             << " state=" << Hex(At<std::uint32_t>(ai + kAiState))
             << " next=" << Hex(At<std::uint32_t>(ai + kAiNextState))
             << " mode=" << At<std::int32_t>(ai + 0x1780)
             << " mask=" << Hex(At<std::uint32_t>(ai + 0x1790))
             << " follow=" << At<std::int32_t>(ai + kAiFollowTarget)
             << " downed=" << ((At<std::uint32_t>(bot + kCharFlags) & 0x20000000) != 0);

        const std::uintptr_t action = At<std::uintptr_t>(ai + 0x18);
        line << " action=" << ClassRva(action);
        if (action != 0 && At<std::uintptr_t>(action) == g_Base + 0x11AD298)   // SimpleAIActionOrientationInteract
        {
            const std::uint32_t target = At<std::uint32_t>(action + 0x1794);
            const std::uintptr_t object = CharacterAt(target);
            line << " interact{step=" << At<std::uint32_t>(action + 0x1700)
                 << " result=" << At<std::uint32_t>(action + 0x1704)
                 << " t=" << At<float>(action + 0x170C)
                 << " kind=" << At<std::uint32_t>(action + 0x1790)
                 << " target=" << target << " class=" << ClassRva(object);
            if (object != 0)
                line << " type=" << At<std::uint32_t>(object + 0x178);
            line << "}";
        }
        Logger::Info(line.str());
        Logger::Flush();
    }

    // Raider bot absorbs civilians (raider_absorb, on by default with casual_bots=1; normal matches only).
    // The practice Raider AI (SimpleAIPractice, brain at character +0x2480 with type +0x10 == 1) only patrols and
    // chases. Its Interact action (SimpleAIActionPracticeInteract, started by sub_1402C8060(brain, target)) asks the
    // target for its interact position, warps there and presses interact, and drops back to patrol when the target
    // is gone; it switches to fighting when a survivor is in sight (0x1402C8F00). So whenever the Raider bot is
    // just patrolling, it is sent to the nearest civilian (CivilianUnit) to absorb it. Checked once a second, from
    // this per-frame bot update (game thread).
    constexpr std::uintptr_t kCivilianVftableRva = 0x1247B90;         // XG::Game::CivilianUnit
    constexpr std::uintptr_t kPracticePatrolVftableRva = 0x11A7EB8;   // SimpleAIActionPracticePatrol
    constexpr std::uintptr_t kStartInteractRva = 0x2C8060;            // sub_1402C8060(brain, target unit)
    constexpr std::uintptr_t kCharBrain = 0x2480;
    constexpr float kAbsorbRange = 80.0f;
    using StartInteractFn = std::uint64_t(__fastcall*)(std::uintptr_t, std::uintptr_t);
    bool g_RaiderAbsorb = false;
    volatile LONG64 g_LastAbsorbCheck = 0;
    int g_AbsorbLogged = 0;

    // Test 1 (2026-11-11): starting Interact from far away did nothing - the Interact update first asks
    // sub_140121600(raider) (the character's interaction slot +0x1A90: kind +0x1A94, sub-kind +0x1A98, target
    // handle +0x1AC4) whether something is in reach and drops back to patrol at once when not. The game's own patrol
    // walks close first (patrol +0x1790 = destination, +0x1704 = 1 "moving", sub_1402B52B0 stops within 5) and only
    // then starts Interact. Same here: walk the Raider to the target with its patrol action, start Interact once it
    // is within kInteractRange and the game shows something in reach. Downed survivors come first, then civilians.
    constexpr std::uintptr_t kInteractionSlotRva = 0x121600;         // sub_140121600(character) -> +0x1A90 or null
    constexpr std::uintptr_t kRivalLevelRva = 0xCD9E0;               // sub_1400CD9E0(character +0x2E0 object)
    constexpr float kInteractRange = 6.0f;
    constexpr float kDownedRange = 120.0f;
    using CharacterPtrFn = std::uintptr_t(__fastcall*)(std::uintptr_t);
    using RivalLevelFn = std::uint32_t(__fastcall*)(std::uintptr_t);

    std::uintptr_t Nearest(const float* from, bool downedSurvivors, float range, float& distance)
    {
        std::uintptr_t best = 0;
        float bestDistance = range * range;
        for (std::uint32_t h = 1; h < kCharacterTableSize; ++h)
        {
            const std::uintptr_t unit = CharacterAt(h);
            if (unit == 0)
                continue;
            const std::uintptr_t vtable = At<std::uintptr_t>(unit);
            if (downedSurvivors)
            {
                if (vtable != g_Base + kMobVftableRva || At<std::uint32_t>(unit + kCharRole) != 0 ||
                    (At<std::uint32_t>(unit + kCharFlags) & 0x20000000) == 0)
                    continue;
            }
            else if (vtable != g_Base + kCivilianVftableRva)
                continue;
            const float* p = reinterpret_cast<const float*>(unit + kCharPosition);
            const float dx = p[0] - from[0], dy = p[1] - from[1], dz = p[2] - from[2];
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < bestDistance)
            {
                bestDistance = d;
                best = unit;
            }
        }
        distance = std::sqrt(bestDistance);
        return best;
    }

    void RaiderAbsorbTick()
    {
        // Bot updates run on several threads at once: only one of them may take each 250 ms tick.
        const LONG64 now = static_cast<LONG64>(GetTickCount64());
        const LONG64 last = g_LastAbsorbCheck;
        if (now - last < 250 || InterlockedCompareExchange64(&g_LastAbsorbCheck, now, last) != last)
            return;

        const std::uintptr_t slots = At<std::uintptr_t>(g_Base + kGameGlobalPtrRva);
        if (slots == 0 || At<std::uint8_t>(slots + kPracticeFlag) != 0 || At<std::uintptr_t>(slots + kCharacterManager) == 0)
            return;
        const auto bySlot = reinterpret_cast<CharacterBySlotFn>(g_Base + kCharacterBySlotRva);
        const std::uintptr_t raider = bySlot(At<std::uintptr_t>(slots + kCharacterManager), 7);
        if (raider == 0 || At<std::uintptr_t>(raider) != g_Base + kRivalVftableRva)
            return;
        const std::uintptr_t brain = At<std::uintptr_t>(raider + kCharBrain);
        if (brain == 0 || At<std::uint32_t>(brain + 0x10) != 1)   // not the practice Raider AI (e.g. a player Raider)
            return;
        const std::uintptr_t action = At<std::uintptr_t>(brain + 0x18);
        const std::uintptr_t inReach = reinterpret_cast<CharacterPtrFn>(g_Base + kInteractionSlotRva)(raider);
        const float* r = reinterpret_cast<const float*>(raider + kCharPosition);

        float distance = 0.0f;
        bool downed = true;
        std::uintptr_t target = Nearest(r, true, kDownedRange, distance);
        if (target == 0)
        {
            downed = false;
            target = Nearest(r, false, kAbsorbRange, distance);
        }

        const bool patrolling = action != 0 && At<std::uintptr_t>(action) == g_Base + kPracticePatrolVftableRva;
        const char* step = "busy";
        if (patrolling && target != 0)
        {
            if (distance <= kInteractRange && inReach != 0)
            {
                reinterpret_cast<StartInteractFn>(g_Base + kStartInteractRva)(brain, target);
                step = "interact";
            }
            else
            {
                const float* p = reinterpret_cast<const float*>(target + kCharPosition);
                auto* destination = reinterpret_cast<float*>(action + 0x1790);
                destination[0] = p[0];
                destination[1] = p[1];
                destination[2] = p[2];
                destination[3] = 1.0f;
                At<std::uint32_t>(action + 0x1704) = 1;   // patrol: move to the destination
                step = "walk";
            }
        }

        // Trace once a second: what the Raider bot is doing and what it sees in reach.
        static std::uint64_t lastTrace = 0;
        if (static_cast<std::uint64_t>(now) - lastTrace >= 1000 && g_AbsorbLogged < 400)
        {
            lastTrace = static_cast<std::uint64_t>(now);
            ++g_AbsorbLogged;
            const std::uintptr_t rivalState = At<std::uintptr_t>(raider + 0x2E0);
            const std::uint32_t level = rivalState != 0 ? reinterpret_cast<RivalLevelFn>(g_Base + kRivalLevelRva)(rivalState) : 0;
            std::ostringstream line;
            line << "RaiderBot: action=" << ClassRva(action) << " step=" << step << " level=" << level
                 << " reach=" << (inReach != 0 ? std::to_string(At<std::uint32_t>(raider + 0x1A94)) + "/" +
                                                   std::to_string(At<std::uint32_t>(raider + 0x1A98)) : std::string("none"))
                 << " target=" << (target == 0 ? "none" : downed ? "downed survivor" : "civilian")
                 << " dist=" << static_cast<int>(distance);
            Logger::Info(line.str());
            Logger::Flush();
        }
    }

    std::uint64_t __fastcall AiUpdateDetour(std::uintptr_t ai, float dt)
    {
        if (g_RaiderAbsorb)
            RaiderAbsorbTick();
        const std::uintptr_t self = CharacterAt(At<std::uint32_t>(ai + kAiCharacterHandle));
        if (IsCharacter(self))
        {
            const std::uint32_t slot = At<std::uint32_t>(self + kCharSlot);
            // Only once the brain's movement controller exists (AI+0xA0): the tutorial mode makes it on the
            // first movement order, and the character update reads AI+0xA0 -> +0x28 without a null check
            // (crash 2026-11-11 at 0x1402184D2 when the mode was cleared on the very first frame).
            // Off for now (2026-11-11): free mode leaves the behaviour mask empty and the bots idle; tutorial
            // mode 0 is what runs the planner and picks area objectives. Kept behind survivor_bot_free_mode=1.
            if (g_FreeMode && slot < kSlotCount && (g_BotSlotMask & (1u << slot)) != 0 && At<std::uintptr_t>(ai + 0xA0) != 0 &&
                (At<std::int32_t>(ai + 0x1780) != -1 || At<std::int32_t>(ai + 0x1788) != -1))
            {
                At<std::int32_t>(ai + 0x1780) = -1;
                At<std::int32_t>(ai + 0x1788) = -1;
                Logger::Info("PracticeBots: slot " + std::to_string(slot) + " brain set to free mode (-1)");
            }
            if (g_Trace)
                TraceBot(ai, self);
        }
        if (g_FollowerSlot < kSlotCount)
        {
            const std::uintptr_t bot = CharacterAt(At<std::uint32_t>(ai + kAiCharacterHandle));
            if (IsCharacter(bot) && At<std::uint32_t>(bot + kCharSlot) == g_FollowerSlot &&
                (At<std::uint32_t>(bot + kCharFlags) & 0x20000000) == 0)
            {
                const std::uintptr_t player = FindPlayer();
                if (player != 0)
                {
                    const float* a = reinterpret_cast<const float*>(bot + kCharPosition);
                    const float* b = reinterpret_cast<const float*>(player + kCharPosition);
                    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
                    const std::uint32_t state = At<std::uint32_t>(ai + kAiState);
                    const std::int32_t next = At<std::int32_t>(ai + kAiNextState);
                    if (dx * dx + dy * dy + dz * dz > kFollowDistance * kFollowDistance &&
                        state != kStateApproach && state != kStateDowned && next < 0)
                    {
                        AssignHandle(ai + kAiFollowTarget, g_PlayerHandle);
                        At<std::uint32_t>(ai + kAiNextState) = kStateApproach;
                    }
                }
            }
        }
        return g_OriginalAiUpdate(ai, dt);
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

    // practice_raider_bot=0: the Raider bot is still added, but switched off - the same state the game uses
    // when the practice "rival bot" rule is off (sub_1408BF360 a5 = 1 -> slot +0xD04 = 1, AI mode 0), so it
    // stands at its spawn. A match with no Raider at all crashes while loading (2026-11-11: divide by zero at
    // 0x14028668E in sub_1402863B0, which spreads items over the Raider list).
    void __fastcall AddRaiderBotDetour(
        std::uintptr_t slots, std::uint32_t slotIndex, std::uint32_t a3, std::uint8_t a4, std::uint8_t a5)
    {
        NSR_UNUSED(a5);
        g_OriginalAddRaiderBot(slots, slotIndex, a3, a4, 1);
        Logger::Info("PracticeBots: Raider bot added switched off (practice_raider_bot=0)");
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
        if (slots == 0)
            return;
        const bool practice = At<std::uint8_t>(slots + kPracticeFlag) != 0;
        if (!practice && !g_CasualBots)
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

            // Normal matchmaking autofill (casual_bots=1): the server starts the match with only the player;
            // the empty slots are filled here. The practice lobby adds the Raider bot itself; a normal match
            // does not, so when no player is the Raider it is added the way the practice lobby does it
            // (sub_14067B260: sub_1408BF360(slots, 7, sub_140C60F70(), sub_140C60FD0(1, 0), aiOff)) with its AI on.
            if (!practice)
            {
                bool hasRaider = false;
                for (std::uint32_t i = 0; i < kSlotCount; ++i)
                {
                    const std::uintptr_t slot = slots + i * kSlotStride;
                    if (At<std::uint8_t>(slot + kSlotPresent) != 0 && At<std::uint32_t>(slot + kSlotRole) == 1)
                        hasRaider = true;
                }
                if (!hasRaider && At<std::uint8_t>(slots + 7 * kSlotStride + kSlotPresent) == 0)
                {
                    const auto rivalId = reinterpret_cast<std::uint32_t(__fastcall*)()>(g_Base + kRivalIdRva)();
                    const auto option = reinterpret_cast<std::uint8_t(__fastcall*)(std::uint32_t, std::uint32_t)>(g_Base + kRivalOptionRva)(1, 0);
                    // The original, not the hooked entry: practice_raider_bot=0 hooks this function to switch the
                    // practice Raider off, which must not apply here.
                    const auto addRaider = g_OriginalAddRaiderBot != nullptr
                        ? g_OriginalAddRaiderBot : reinterpret_cast<AddRaiderBotFn>(g_Base + kAddRaiderBotRva);
                    addRaider(slots, 7, rivalId, option, 0);
                    // sub_1408BF360 gives the Raider AI mode 4: SimpleAIPractice with the practice patrol action
                    // (brain chooser sub_1401FC5E0), which walks the practice Raider patrol points
                    // (practiceRaiderPatrolPosition.prpp). Without them the bot went under area X; with the plain
                    // SimpleAI (mode 2) it stood still (live 2026-11-11). Mode 4 is kept, and the patrol points are
                    // now loaded in normal matches too (PatchPracticePatrolLoad).
                    Logger::Info("PracticeBots: normal match - Raider bot added in slot 7 (AI on, practice AI mode 4)");
                }
            }

            CapturePlayerLook(slots);
            g_FollowerSlot = 0xFFFFFFFF;
            g_PlayerHandle = 0;
            g_BotSlotMask = 0;

            int added = 0;
            for (std::uint32_t i = 1; i < kSlotCount && added < g_BotCount; ++i)
            {
                if (At<std::uint8_t>(slots + i * kSlotStride + kSlotPresent) != 0)
                    continue;
                AddSurvivorBot(slots, i, added + 1);
                ApplyLook(slots, i, g_BotLooks[static_cast<std::size_t>(added)]);
                g_BotSlotMask = static_cast<std::uint8_t>(g_BotSlotMask | (1u << i));
                ++added;
                if (added == g_FollowerBot)
                {
                    g_FollowerSlot = i;
                    Logger::Info("PracticeBots: SURVIVOR BOT " + std::to_string(added) + " (slot " +
                        std::to_string(i) + ") follows the player");
                }
            }
            Logger::Info("PracticeBots: added " + std::to_string(added) + " survivor bot(s)");
        }
        catch (...)
        {
            Logger::Error("PracticeBots: adding survivor bots threw");
        }
        Logger::Flush();
    }

    // Normal matchmaking host (casual_bots=1). In a normal match the client treats the Raider in the match-found
    // role list as the host: sub_140806780 returns the role entry whose category (+0x28, sub_140806820: roles
    // 0/10/20) is 1, and MatchingSubModuleMatching (0x14081AB50) fails with error 1603 when there is none
    // (live 2026-11-11, "startup information was not genuine", 320001603). In an autofilled match a survivor
    // player is alone in the role list and the Raider is a bot added in the match, so the player is made the
    // host: the find returns the player's own entry (+0x70 = this is me), and sub_14081B8F0 (am I host) says yes.
    // Only for calls from the matching sub-module (0x1408194F0..0x14081D000), not the tutorial matching module.
    constexpr std::uintptr_t kFindRaiderEntryRva = 0x806780;   // sub_140806780(roleListHolder) -> entry or 0
    constexpr std::uintptr_t kIsMatchHostRva = 0x81B8F0;       // sub_14081B8F0(MatchingSubModuleMatching*) -> bool
    constexpr std::uintptr_t kMatchingCallersBegin = 0x8194F0;
    constexpr std::uintptr_t kMatchingCallersEnd = 0x81D000;
    constexpr std::uintptr_t kRoleListHolder = 0x148;          // in MatchingSubModuleMatching
    constexpr std::uintptr_t kRoleEntryStride = 216;

    using FindRaiderEntryFn = std::uintptr_t(__fastcall*)(std::uintptr_t);
    using IsMatchHostFn = std::uint8_t(__fastcall*)(std::uintptr_t);
    FindRaiderEntryFn g_OriginalFindRaiderEntry = nullptr;
    IsMatchHostFn g_OriginalIsMatchHost = nullptr;
    void* g_FindRaiderEntryTarget = nullptr;
    void* g_IsMatchHostTarget = nullptr;
    bool g_LoggedSoloHost = false;

    std::uintptr_t FindOwnRoleEntry(std::uintptr_t holder)
    {
        const auto begin = At<std::uintptr_t>(holder + 0x48);
        const auto end = At<std::uintptr_t>(holder + 0x50);
        for (auto e = begin; e != 0 && e < end; e += kRoleEntryStride)
            if (At<std::uint8_t>(e + 0x70) != 0)
                return e;
        return 0;
    }

    std::uintptr_t __fastcall FindRaiderEntryDetour(std::uintptr_t holder)
    {
        const auto found = g_OriginalFindRaiderEntry(holder);
        if (found != 0)
            return found;
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_Base;
        if (caller < kMatchingCallersBegin || caller >= kMatchingCallersEnd)
            return 0;
        const auto own = FindOwnRoleEntry(holder);
        if (own != 0 && !g_LoggedSoloHost)
        {
            g_LoggedSoloHost = true;
            Logger::Info("PracticeBots: no Raider in the match-found role list - the player hosts the match");
            Logger::Flush();
        }
        return own;
    }

    std::uint8_t __fastcall IsMatchHostDetour(std::uintptr_t matching)
    {
        const auto host = g_OriginalIsMatchHost(matching);
        if (host != 0)
            return host;
        const auto holder = matching + kRoleListHolder;
        return g_OriginalFindRaiderEntry(holder) == 0 && FindOwnRoleEntry(holder) != 0 ? 1 : 0;
    }

    // Practice Raider patrol points in normal matches. The stage loader (sub_14070D370, at 0x14070D8C3) loads
    // practiceRaiderPatrolPosition.prpp only when game +0x28 (practice), +0x2A and +0x29 are all set - the same
    // flags the lobby uses to turn the practice Raider bot's AI on. The three "je" that skip it are replaced
    // with NOPs, so the patrol points exist and the practice Raider AI (mode 4) has somewhere to go.
    // Only applied when the bytes are exactly the expected ones.
    constexpr std::uintptr_t kPatrolGateRva = 0x70D8C3;
    void PatchPracticePatrolLoad()
    {
        static const std::uint8_t expected[18] = {
            0x80, 0x78, 0x28, 0x00, 0x74, 0x6E,    // cmp byte [rax+28h],0 ; je
            0x80, 0x78, 0x2A, 0x00, 0x74, 0x68,    // cmp byte [rax+2Ah],0 ; je
            0x80, 0x78, 0x29, 0x00, 0x74, 0x62 };  // cmp byte [rax+29h],0 ; je
        auto* code = reinterpret_cast<std::uint8_t*>(g_Base + kPatrolGateRva);
        if (std::memcmp(code, expected, sizeof(expected)) != 0)
        {
            Logger::Error("PracticeBots: patrol-point gate bytes differ from the expected ones - not patched");
            return;
        }
        DWORD old = 0;
        if (!VirtualProtect(code, sizeof(expected), PAGE_EXECUTE_READWRITE, &old))
        {
            Logger::Error("PracticeBots: VirtualProtect failed - patrol-point gate not patched");
            return;
        }
        for (const std::size_t at : { 4u, 10u, 16u })
        {
            code[at] = 0x90;
            code[at + 1] = 0x90;
        }
        VirtualProtect(code, sizeof(expected), old, &old);
        FlushInstructionCache(GetCurrentProcess(), code, sizeof(expected));
        Logger::Info("PracticeBots: practice Raider patrol points are loaded in every match (gate 0x14070D8C3 patched)");
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
    g_FollowerBot = GetPrivateProfileIntA("steam", "survivor_bot_follow", 0, SteamConfig::GetPath().c_str());
    if (g_FollowerBot > 0)
        Logger::Info("PracticeBots: survivor bot " + std::to_string(g_FollowerBot) + " will follow the player");
    for (std::size_t i = 0; i < g_BotLooks.size(); ++i)
    {
        char value[128] = {};
        const std::string key = "survivor_bot_look" + std::to_string(i + 1);
        GetPrivateProfileStringA("steam", key.c_str(), "", value, sizeof(value), SteamConfig::GetPath().c_str());
        g_BotLooks[i] = value;
        if (!g_BotLooks[i].empty())
            Logger::Info("PracticeBots: survivor bot " + std::to_string(i + 1) + " look = " + g_BotLooks[i]);
    }

    g_Base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_Base == 0)
        return false;

    const bool guard = Hook(kTutorialGetRva, reinterpret_cast<void*>(&GuardedTutorialGet),
        reinterpret_cast<void**>(&g_OriginalTutorialGet), &g_TutorialGetTarget, "sub_1408924D0 (null guard)");
    const bool add = Hook(kLobbyStartRva, reinterpret_cast<void*>(&LobbyStartDetour),
        reinterpret_cast<void**>(&g_OriginalLobbyStart), &g_LobbyStartTarget, "sub_14067B260 (lobby match start)");

    g_CasualBots = GetPrivateProfileIntA("steam", "casual_bots", 0, SteamConfig::GetPath().c_str()) != 0;
    if (g_CasualBots)
    {
        Logger::Info("PracticeBots: normal matches are filled with bots too (casual_bots=1)");
        PatchPracticePatrolLoad();
        g_RaiderAbsorb = GetPrivateProfileIntA("steam", "raider_absorb", 1, SteamConfig::GetPath().c_str()) != 0;
        if (g_RaiderAbsorb)
            Logger::Info("PracticeBots: the normal-match Raider bot absorbs civilians when it is only patrolling (raider_absorb)");
        Hook(kFindRaiderEntryRva, reinterpret_cast<void*>(&FindRaiderEntryDetour),
            reinterpret_cast<void**>(&g_OriginalFindRaiderEntry), &g_FindRaiderEntryTarget, "sub_140806780 (match host = Raider)");
        if (g_OriginalFindRaiderEntry != nullptr)
            Hook(kIsMatchHostRva, reinterpret_cast<void*>(&IsMatchHostDetour),
                reinterpret_cast<void**>(&g_OriginalIsMatchHost), &g_IsMatchHostTarget, "sub_14081B8F0 (am I the match host)");
    }
    g_FreeMode = GetPrivateProfileIntA("steam", "survivor_bot_free_mode", 0, SteamConfig::GetPath().c_str()) != 0;
    g_Trace = GetPrivateProfileIntA("steam", "survivor_bot_trace", 0, SteamConfig::GetPath().c_str()) != 0;
    if (g_Trace)
        Logger::Info("PracticeBots: bot status trace on (every 2 s per bot)");

    // Always on with bots: the update hook also takes the bots out of the tutorial mode.
    const bool follow = Hook(kAiUpdateRva, reinterpret_cast<void*>(&AiUpdateDetour),
            reinterpret_cast<void**>(&g_OriginalAiUpdate), &g_AiUpdateTarget, "0x1402BE6E0 (tutorial AI update, follower)");

    g_RaiderBot = GetPrivateProfileIntA("steam", "practice_raider_bot", 1, SteamConfig::GetPath().c_str()) != 0;
    bool raider = true;
    if (!g_RaiderBot)
    {
        raider = Hook(kAddRaiderBotRva, reinterpret_cast<void*>(&AddRaiderBotDetour),
            reinterpret_cast<void**>(&g_OriginalAddRaiderBot), &g_AddRaiderBotTarget, "sub_1408BF360 (Raider bot off)");
        Logger::Info("PracticeBots: practice Raider bot switched off (practice_raider_bot=0)");
    }
    return guard && add && raider && follow;
}

void PracticeBots::Shutdown()
{
    if (g_LobbyStartTarget)
        HookManager::Disable(g_LobbyStartTarget);
    if (g_AddRaiderBotTarget)
        HookManager::Disable(g_AddRaiderBotTarget);
    g_AddRaiderBotTarget = nullptr;
    if (g_AiUpdateTarget)
        HookManager::Disable(g_AiUpdateTarget);
    g_AiUpdateTarget = nullptr;
    if (g_TutorialGetTarget)
        HookManager::Disable(g_TutorialGetTarget);
    if (g_IsMatchHostTarget)
        HookManager::Disable(g_IsMatchHostTarget);
    if (g_FindRaiderEntryTarget)
        HookManager::Disable(g_FindRaiderEntryTarget);
    g_IsMatchHostTarget = nullptr;
    g_FindRaiderEntryTarget = nullptr;
    g_LobbyStartTarget = nullptr;
    g_TutorialGetTarget = nullptr;
}
