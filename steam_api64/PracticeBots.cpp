#include "StdInc.h"
#include "PracticeBots.h"
#include "HookManager.h"
#include "Logger.h"
#include "SteamConfig.h"
#include <intrin.h>
#include <random>

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
    constexpr std::uintptr_t kPressInputRva = 0x27B1B0;               // sub_14027B1B0(input queue, 2) = interact
    using PressInputFn = std::uint64_t(__fastcall*)(std::uintptr_t, std::uint64_t);
    using CharacterPtrFn = std::uintptr_t(__fastcall*)(std::uintptr_t);
    using RivalLevelFn = std::uint32_t(__fastcall*)(std::uintptr_t);
    constexpr std::uintptr_t kAddEnergyRva = 0xCCC40;                // sub_1400CCC40(form, amount): add evolution energy
    using AddEnergyFn = void(__fastcall*)(std::uintptr_t, float);
    bool g_EnergyTest = false;
    float g_EnergyRate = 1.6f;
    int g_EnergyFills = 0;

    // Raider character whose interact button is being held (0 = none); set by RaiderAbsorbTick every 250 ms.
    volatile std::uintptr_t g_HoldRaider = 0;
    volatile std::uint32_t g_HoldBits = 2;   // input bits held for g_HoldRaider: 0x2 interact, 0x800000 destroy area

    // Area destruction (raider_bot_destroy=1, 2026-11-12). Game rule (user): each evolution gives the Raider one
    // area destruction, 3 per match. Input trace of the user playing Cell: the only rare input, once after each
    // evolution (01:16:49 level 2, 01:18:27 level 3, 01:19:23 level 4), was bits 0x800000, pressed for 1-2 s.
    // So: 8 s after the bot's level goes up (evolution animation), hold 0x800000 for 2.5 s, once per evolution.
    bool g_RaiderDestroy = false;
    constexpr std::uint32_t kDestroyAreaBits = 0x800000;

    // Aim (2026-11-12, user: "the beam goes whereever the raider aims at"; test: the level 3 Kamehameha fired the
    // opposite way; the area should be random each time). Before firing, a random living civilian at least 100 away
    // is picked (civilians are spread over every area and die with a destroyed area, so destroyed areas and Cell's
    // own area drop out) and Cell is turned toward it on every bot update while 0x800000 is held.
    // sub_140212FB0(character, &direction (x, y, z, w), 1) = Possible_SetFacing: yaw from the vector, snaps
    // (+0x1924 / +0x1928 yaw, +0x1950 direction). raider_bot_aim_flip=1 turns the other way, in case the game's
    // forward is the opposite of this vector.
    // Random numbers for the bot (2026-11-12): std::rand keeps its state per thread and srand ran only on the
    // loading thread, so the bot threads always drew the same sequence (areas B then C in two matches). One shared
    // generator, seeded from the performance counter and the tick count on first use, guarded by a lock.
    std::uint32_t BotRandom(std::uint32_t below)
    {
        static std::mutex lock;
        static std::mt19937 generator = [] {
            LARGE_INTEGER counter = {};
            QueryPerformanceCounter(&counter);
            std::seed_seq seed{ static_cast<std::uint32_t>(counter.QuadPart), static_cast<std::uint32_t>(counter.QuadPart >> 32),
                                static_cast<std::uint32_t>(GetTickCount64()), static_cast<std::uint32_t>(GetCurrentProcessId()) };
            return std::mt19937(seed);
        }();
        if (below <= 1)
            return 0;
        std::lock_guard<std::mutex> guard(lock);
        return std::uniform_int_distribution<std::uint32_t>(0, below - 1)(generator);
    }

    constexpr std::uintptr_t kSetFacingRva = 0x212FB0;
    using SetFacingFn = float(__fastcall*)(std::uintptr_t, const float*, std::uint32_t);
    volatile bool g_AimActive = false;
    float g_AimPos[3] = {};
    bool g_AimFlip = false;
    bool g_FaceTarget = false;   // raider_bot_face_target=1: also turn Cell toward a far civilian (old test)

    // Lock-on (test 4, 2026-11-12: Cell faced the chosen area exactly, yet the level 2 / 3 beam flew toward E and A).
    // The beam aims at character +0x1F60, which sub_140231640 copies every frame from the lock-on (MobLockOn at
    // character +0x310: +0xC target handle = character +0x31C, +0x84 mode = character +0x394; modes 0/1 use the
    // target character's lock point via its vtable +0x3A0, otherwise +0x1F60 is zero). The practice AI locks on to
    // survivors, so the beam went wherever that survivor was. While aiming, the lock-on target is set to a civilian
    // standing in the chosen area (g_AimLockHandle), or cleared (handle 0) when there is none.
    volatile std::uint32_t g_AimLockHandle = 0;

    void FaceAim(std::uintptr_t raider)
    {
        if (!g_AimActive || raider == 0)
            return;
        At<std::uint32_t>(raider + 0x31C) = g_AimLockHandle;
        const float* r = reinterpret_cast<const float*>(raider + kCharPosition);
        float dx = g_AimPos[0] - r[0], dz = g_AimPos[2] - r[2];
        const float flat = std::sqrt(dx * dx + dz * dz);
        if (flat < 0.01f)
            return;
        if (g_AimFlip)
        {
            dx = -dx;
            dz = -dz;
        }
        alignas(16) float direction[4] = { dx / flat, 0.0f, dz / flat, 0.0f };
        reinterpret_cast<SetFacingFn>(g_Base + kSetFacingRva)(raider, direction, 1);
    }

    // A unit that belongs to area `areaId`, for its position (2026-11-12). The area-warning code sub_14027EBF0 looks
    // these up by area number: sub_14074FC40(*(game +0x6850), id) walks the list at +0xCE0 for a unit whose +0x518 is
    // the area number, sub_14074D960(*(game +0x6860), id) the list at +0xAE0 for +0x6D0. List nodes: next at +0,
    // unit handle at +0x14 (same 16-byte unit table as characters). Returns the unit or 0.
    std::uintptr_t AreaUnit(std::uint32_t areaId, int& whichList)
    {
        const std::uintptr_t game = At<std::uintptr_t>(g_Base + kGameGlobalPtrRva);
        if (game == 0)
            return 0;
        const struct { std::uintptr_t managerOffset, listOffset, idOffset; } lists[2] = {
            { 0x6850, 0xCE0, 0x518 }, { 0x6860, 0xAE0, 0x6D0 } };
        for (int i = 0; i < 2; ++i)
        {
            const std::uintptr_t manager = At<std::uintptr_t>(game + lists[i].managerOffset);
            if (manager == 0)
                continue;
            const std::uintptr_t head = At<std::uintptr_t>(manager + lists[i].listOffset);
            if (head == 0)
                continue;
            std::uintptr_t node = At<std::uintptr_t>(head);
            for (int guard = 0; node != 0 && node != head && guard < 4096; ++guard, node = At<std::uintptr_t>(node))
            {
                const std::uintptr_t unit = CharacterAt(At<std::uint32_t>(node + 0x14));
                if (unit != 0 && At<std::uint32_t>(unit + lists[i].idOffset) == areaId)
                {
                    whichList = i;
                    return unit;
                }
            }
        }
        return 0;
    }

    // Random living civilian at least minDistance from `from` (0 = none). Uses rand() seeded once.
    std::uintptr_t RandomFarCivilian(const float* from, float minDistance)
    {
        std::uintptr_t found[256];
        int count = 0;
        for (std::uint32_t h = 1; h < kCharacterTableSize && count < 256; ++h)
        {
            const std::uintptr_t unit = CharacterAt(h);
            if (unit == 0 || At<std::uintptr_t>(unit) != g_Base + kCivilianVftableRva)
                continue;
            const float* p = reinterpret_cast<const float*>(unit + kCharPosition);
            const float dx = p[0] - from[0], dz = p[2] - from[2];
            if (dx * dx + dz * dz >= minDistance * minDistance)
                found[count++] = unit;
        }
        return count == 0 ? 0 : found[BotRandom(count)];
    }
    volatile LONG64 g_LastHoldPress = 0;

    // Presses interact for the held Raider about once a frame (bot updates run on several threads; at most one
    // press per 10 ms). The tick re-checks every 250 ms that the interaction is still in reach.
    void RaiderHoldInteract()
    {
        const std::uintptr_t raider = g_HoldRaider;
        if (raider == 0)
            return;
        const LONG64 now = static_cast<LONG64>(GetTickCount64());
        const LONG64 last = g_LastHoldPress;
        if (now - last < 10 || InterlockedCompareExchange64(&g_LastHoldPress, now, last) != last)
            return;
        if (g_HoldBits == kDestroyAreaBits)
            FaceAim(raider);
        const std::uintptr_t input = At<std::uintptr_t>(raider + 0x5B8);
        if (input != 0)
            reinterpret_cast<PressInputFn>(g_Base + kPressInputRva)(input, g_HoldBits);
    }

    // Finished bodies (2026-11-12): after a finisher the survivor keeps the downed flag (0x20000000), so the bot kept
    // picking the same body and stood on it. A downed survivor the Raider stands on (within 3) without the game
    // offering a finisher for 2 s is done: it is skipped for 60 s.
    struct SkippedUnit { std::uintptr_t unit; LONG64 until; };
    SkippedUnit g_Skipped[16] = {};

    bool IsSkipped(std::uintptr_t unit, LONG64 now)
    {
        for (const auto& s : g_Skipped)
            if (s.unit == unit && now < s.until)
                return true;
        return false;
    }

    void Skip(std::uintptr_t unit, LONG64 now)
    {
        SkippedUnit* slot = &g_Skipped[0];
        for (auto& s : g_Skipped)
            if (s.unit == unit || now >= s.until) { slot = &s; break; }
        slot->unit = unit;
        slot->until = now + 60000;
    }

    std::uintptr_t Nearest(const float* from, bool downedSurvivors, float range, float& distance)
    {
        const LONG64 now = static_cast<LONG64>(GetTickCount64());
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
                    (At<std::uint32_t>(unit + kCharFlags) & 0x20000000) == 0 || IsSkipped(unit, now))
                    continue;
            }
            else if (vtable != g_Base + kCivilianVftableRva || IsSkipped(unit, now))
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
        g_HoldRaider = 0;   // re-decided below; never keep holding for a Raider that may be gone

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

        // Test 2 (2026-11-11): the patrol walk stops within 5 of its destination (sub_1402B52B0(.., 5, 5)) so the
        // Raider hovered 5-7 away; next to a downed survivor the game offered interaction 0/3, but the practice
        // Interact action only presses for kind 5 (civilian absorb) and kept repositioning. Now: aim 4.5 past the
        // target so "arrived" is on top of it, and press interact directly (input queue sub_14027B1B0(char+0x5B8, 2),
        // what sub_1402B1B20 does for bots) when the matching interaction is in reach: 0/3 at a downed survivor
        // (finisher), kind 5 at a civilian (absorb).
        const bool patrolling = action != 0 && At<std::uintptr_t>(action) == g_Base + kPracticePatrolVftableRva;
        const std::uint32_t reachKind = inReach != 0 ? At<std::uint32_t>(raider + 0x1A94) : 0xFFFFFFFF;
        const std::uint32_t reachSub = inReach != 0 ? At<std::uint32_t>(raider + 0x1A98) : 0xFFFFFFFF;
        // Test 3 (2026-11-11): at a civilian, 5/17 came into reach and one press started the tail animation, but
        // the absorb needs interact HELD - the game's Interact action presses it every frame while the interaction
        // runs (0x1402C8F00 case 2). Next to a downed survivor the game offered 1/16 (0/3 in test 2). So while the
        // right interaction is in reach, g_HoldInteract makes RaiderHoldInteract() press it on every bot update.
        const bool finisher = downed && ((reachKind == 1 && reachSub == 16) || (reachKind == 0 && reachSub == 3));
        const bool absorb = !downed && reachKind == 5;
        // Level 4 (2026-11-12): next to a downed survivor the game showed nothing in reach (reach=none at distance
        // 1-3 for 20 s), yet a level 4 Cell executes with a point-blank ki blast instead of the tail. Test: at level
        // 4 hold interact anyway when standing on a downed survivor; give up on that body after 6 s.
        const std::uintptr_t formNow = At<std::uintptr_t>(raider + 0x2E0);
        const std::uint32_t levelNow = formNow != 0 ? reinterpret_cast<RivalLevelFn>(g_Base + kRivalLevelRva)(formNow) : 0;
        const bool blast = downed && !finisher && levelNow >= 4 && target != 0 && distance <= 3.0f;
        const char* step = "busy";
        static std::uintptr_t noFinisherUnit = 0;
        static LONG64 noFinisherSince = 0;
        // Same for civilians (user 2026-11-12: "cell also attempted to chase after a civilian that has already been
        // absorbed"): any target the Raider stands on with no finisher / absorb offered for 2 s is skipped.
        if (target != 0 && distance <= 3.0f && !finisher && !absorb)
        {
            if (noFinisherUnit != target)
            {
                noFinisherUnit = target;
                noFinisherSince = now;
            }
            else if (now - noFinisherSince >= (blast ? 6000 : 2000))
            {
                Skip(target, now);
                noFinisherUnit = 0;
                Logger::Info(std::string("RaiderBot: ") + (downed ? "downed survivor" : "civilian") +
                             " here offers no finisher/absorb for 2 s (already done) - skipped for 60 s");
                target = 0;
            }
        }
        else
            noFinisherUnit = 0;
        // Area destruction: once per evolution, 8 s after the level went up, hold 0x800000 for 2.5 s.
        static std::uintptr_t destroyRaider = 0;
        static std::uint32_t destroyLevel = 0;      // highest level a destruction was already used (or planned) for
        static LONG64 destroyAt = 0, destroyUntil = 0;
        if (destroyRaider != raider)
        {
            destroyRaider = raider;
            destroyLevel = levelNow;                // no destruction for the level the bot spawned with
            destroyAt = destroyUntil = 0;
        }
        if (g_RaiderDestroy && levelNow > destroyLevel && levelNow >= 2)
        {
            destroyLevel = levelNow;
            destroyAt = now + 8000;
            destroyUntil = destroyAt + 2500;
            Logger::Info("RaiderBot: evolved to level " + std::to_string(levelNow) + " - destroying an area in 8 s");
        }
        const bool destroying = destroyAt != 0 && now >= destroyAt && now < destroyUntil;
        static float usedAims[3][3] = {};
        static int usedAimCount = 0;
        if (destroyRaider == raider && levelNow <= 1)
            usedAimCount = 0;                       // new match: Larva again
        // The real target (2026-11-12): the Raider's destroy routine sub_1401A1740 reads the aimed area number at
        // raider +0x2664; if it is a valid, not yet destroyed area it destroys that one, otherwise it takes the
        // first undestroyed area from 1 upward (why the bot destroyed A, B, C in order). Areas: manager =
        // *(*(game +0x6828) +0x18), entries [+0x18, +0x20) of 24 bytes, entry +8 = area number, +0x12 = destroyed
        // (sub_1402802A0 = Possible_FindArea). The fallback loop tries numbers 1 .. count-1; so does this.
        static std::uint32_t destroyTarget = 0;
        if (destroying && destroyTarget == 0)
        {
            std::uint32_t candidates[16];
            int candidateCount = 0;
            const std::uintptr_t game = At<std::uintptr_t>(g_Base + kGameGlobalPtrRva);
            const std::uintptr_t holder = game != 0 ? At<std::uintptr_t>(game + 0x6828) : 0;
            const std::uintptr_t manager = holder != 0 ? At<std::uintptr_t>(holder + 0x18) : 0;
            std::ostringstream seen;
            if (manager != 0)
            {
                const std::uintptr_t begin = At<std::uintptr_t>(manager + 0x18), end = At<std::uintptr_t>(manager + 0x20);
                const std::uint32_t count = (end > begin && end - begin < 24 * 64) ? static_cast<std::uint32_t>((end - begin) / 24) : 0;
                for (std::uint32_t id = 1; id < count && candidateCount < 16; ++id)
                    for (std::uintptr_t e = begin; e < end; e += 24)
                        if (At<std::uint32_t>(e + 8) == id)
                        {
                            const bool destroyed = At<std::uint8_t>(e + 0x12) != 0;
                            seen << " " << static_cast<char>('A' + id - 1) << (destroyed ? "(destroyed)" : "");
                            if (!destroyed)
                                candidates[candidateCount++] = id;
                            break;
                        }
            }
            destroyTarget = candidateCount > 0 ? candidates[BotRandom(candidateCount)] : 0xFFFFFFFF;
            Logger::Info("RaiderBot: areas" + seen.str() + " -> destroying " +
                         (destroyTarget != 0xFFFFFFFF ? std::string(1, static_cast<char>('A' + destroyTarget - 1)) +
                                                        " (area " + std::to_string(destroyTarget) + ")"
                                                      : std::string("? (no area list - the game picks)")));
            // Face the chosen area (test 3, 2026-11-12: random areas worked, the beam still went the wrong way at
            // levels 2 and 3): aim at a unit that belongs to that area, held for 8 s with the patrol stopped.
            int whichList = -1;
            const std::uintptr_t areaUnit = destroyTarget != 0xFFFFFFFF ? AreaUnit(destroyTarget, whichList) : 0;
            if (areaUnit != 0)
            {
                const float* p = reinterpret_cast<const float*>(areaUnit + kCharPosition);
                g_AimPos[0] = p[0];
                g_AimPos[1] = p[1];
                g_AimPos[2] = p[2];
                // Lock-on target: the living civilian nearest the area unit, within 120 of it.
                std::uint32_t lockHandle = 0;
                float lockBest = 120.0f * 120.0f;
                for (std::uint32_t h = 1; h < kCharacterTableSize; ++h)
                {
                    const std::uintptr_t unit = CharacterAt(h);
                    if (unit == 0 || At<std::uintptr_t>(unit) != g_Base + kCivilianVftableRva || IsSkipped(unit, now))
                        continue;
                    const float* c = reinterpret_cast<const float*>(unit + kCharPosition);
                    const float dx = c[0] - p[0], dz = c[2] - p[2], d = dx * dx + dz * dz;
                    if (d < lockBest)
                    {
                        lockBest = d;
                        lockHandle = h;
                    }
                }
                g_AimLockHandle = lockHandle;
                Logger::Info("RaiderBot: lock-on was handle " + std::to_string(At<std::uint32_t>(raider + 0x31C)) +
                             " (mode " + std::to_string(At<std::uint32_t>(raider + 0x394)) + ") -> " +
                             (lockHandle != 0 ? "civilian handle " + std::to_string(lockHandle) + ", " +
                                                std::to_string(static_cast<int>(std::sqrt(lockBest))) + " from the area unit"
                                              : std::string("cleared (no civilian in that area)")));
                g_AimActive = true;
                FaceAim(raider);
                std::ostringstream line;
                line << "RaiderBot: facing area " << static_cast<char>('A' + destroyTarget - 1) << " via "
                     << ClassRva(areaUnit) << " (list " << whichList << ") at (" << static_cast<int>(p[0]) << ", "
                     << static_cast<int>(p[1]) << ", " << static_cast<int>(p[2]) << "); Cell at ("
                     << static_cast<int>(r[0]) << ", " << static_cast<int>(r[2]) << ")";
                Logger::Info(line.str());
            }
            else
                Logger::Info("RaiderBot: no unit found for the chosen area - not turning Cell");
        }
        if (destroying && destroyTarget != 0 && destroyTarget != 0xFFFFFFFF)
            At<std::uint32_t>(raider + 0x2664) = destroyTarget;   // the aimed area, every tick while holding
        if (!destroying && destroyAt == 0)
            destroyTarget = 0;

        if (destroying && !g_AimActive && g_FaceTarget)
        {
            // Pick the area: a random living civilian 100+ away, not near an area this bot already destroyed.
            std::uintptr_t civ = 0;
            for (int tries = 0; tries < 20 && civ == 0; ++tries)
            {
                civ = RandomFarCivilian(r, 100.0f);
                if (civ == 0)
                    break;
                const float* p = reinterpret_cast<const float*>(civ + kCharPosition);
                for (int i = 0; i < usedAimCount && civ != 0; ++i)
                {
                    const float dx = p[0] - usedAims[i][0], dz = p[2] - usedAims[i][2];
                    if (dx * dx + dz * dz < 120.0f * 120.0f)
                        civ = 0;
                }
            }
            if (civ != 0)
            {
                const float* p = reinterpret_cast<const float*>(civ + kCharPosition);
                g_AimPos[0] = p[0];
                g_AimPos[1] = p[1];
                g_AimPos[2] = p[2];
                if (usedAimCount < 3)
                {
                    usedAims[usedAimCount][0] = p[0];
                    usedAims[usedAimCount][1] = p[1];
                    usedAims[usedAimCount][2] = p[2];
                    ++usedAimCount;
                }
                g_AimActive = true;
                FaceAim(raider);
                std::ostringstream line;
                line << "RaiderBot: aiming the area destroy at a civilian at (" << static_cast<int>(p[0]) << ", "
                     << static_cast<int>(p[1]) << ", " << static_cast<int>(p[2]) << "), "
                     << static_cast<int>(std::sqrt((p[0] - r[0]) * (p[0] - r[0]) + (p[2] - r[2]) * (p[2] - r[2])))
                     << " away; Cell at (" << static_cast<int>(r[0]) << ", " << static_cast<int>(r[2]) << ")"
                     << (g_AimFlip ? " [flipped]" : "");
                Logger::Info(line.str());
            }
            else
                Logger::Info("RaiderBot: no civilian 100+ away to aim the area destroy at - firing where Cell faces");
        }
        // Test 2 (2026-11-12): levels 2 and 3 fired the wrong way, level 4 right. Facing was only set while the
        // button was held (2.5 s); the beam comes after a charge animation, while the patrol keeps walking and
        // turning Cell. Now the aim is held for 8 s from the press, the patrol walk is stopped for that time, and
        // Cell's facing (+0x1924 yaw, +0x1950 direction) is logged every 0.5 s next to the wanted direction.
        static LONG64 aimUntil = 0, lastAimLog = 0;
        if (destroyAt != 0 && now >= destroyUntil)
        {
            destroyAt = destroyUntil = 0;
            aimUntil = g_AimActive ? now + 5500 : 0;     // keep aiming 5.5 s more (8 s from the first press)
            Logger::Info("RaiderBot: area destroy input (0x800000) held for 2.5 s - keeping the aim for 5.5 s");
        }
        if (aimUntil != 0 && now >= aimUntil)
        {
            aimUntil = 0;
            g_AimActive = false;
            Logger::Info("RaiderBot: area destroy aim released");
        }
        const bool aiming = g_AimActive && (destroying || aimUntil != 0);
        if (aiming)
        {
            FaceAim(raider);
            if (patrolling)
                At<std::uint32_t>(action + 0x1704) = 0;   // patrol: stop moving
            if (now - lastAimLog >= 500)
            {
                lastAimLog = now;
                const float* d = reinterpret_cast<const float*>(raider + 0x1950);
                const float wx = g_AimPos[0] - r[0], wz = g_AimPos[2] - r[2];
                const float wl = std::sqrt(wx * wx + wz * wz);
                std::ostringstream line;
                line.precision(2);
                line << std::fixed << "RaiderBot: aim check yaw=" << At<float>(raider + 0x1924)
                     << " facing=(" << d[0] << ", " << d[2] << ") wanted=(" << (wl > 0 ? wx / wl : 0) << ", "
                     << (wl > 0 ? wz / wl : 0) << ") lock=" << At<std::uint32_t>(raider + 0x31C) << "/mode "
                     << At<std::uint32_t>(raider + 0x394) << " aimpoint=(" << At<float>(raider + 0x1F60) << ", "
                     << At<float>(raider + 0x1F68) << ") action=" << ClassRva(action);
                Logger::Info(line.str());
            }
        }

        const bool hold = target != 0 && distance <= 3.0f && (finisher || absorb || blast);
        g_HoldBits = destroying ? kDestroyAreaBits : 2u;
        g_HoldRaider = (hold || destroying) ? raider : 0;
        if (destroying)
            step = "destroy-area";
        else if (aiming)
            step = "destroy-aim";
        else if (hold)
            step = finisher ? "hold-finisher" : absorb ? "hold-absorb" : "hold-blast";
        else if (patrolling && target != 0)
        {
            const float* p = reinterpret_cast<const float*>(target + kCharPosition);
            float dx = p[0] - r[0], dz = p[2] - r[2];
            const float flat = std::sqrt(dx * dx + dz * dz);
            if (flat > 0.01f)
            {
                dx /= flat;
                dz /= flat;
            }
            auto* destination = reinterpret_cast<float*>(action + 0x1790);
            destination[0] = p[0] + dx * 4.5f;
            destination[1] = p[1];
            destination[2] = p[2] + dz * 4.5f;
            destination[3] = 1.0f;
            At<std::uint32_t>(action + 0x1704) = 1;   // patrol: move to the destination
            step = "walk";
        }

        // Energy test (raider_bot_energy_test=1, 2026-11-11): when the bot's evolution energy (form +0xA0) has not
        // moved for 10 s, fill it once to the amount needed (+0xA8) through the game's own add-energy function
        // sub_1400CCC40(form, amount). The trace then shows whether energy grows by itself (before any fill) and
        // whether a full bar makes the bot evolve by itself.
        const std::uintptr_t form = At<std::uintptr_t>(raider + 0x2E0);
        // Test result (2026-11-11 23:37): the bot's energy stayed 0/650 with lock 0 - it gets none of the steady gain
        // a player Raider gets (recording: ~1.6 per second from 0) - and a full bar evolves it by itself within ~5 s.
        // The game keeps the energy after an evolution (650 stays, needed becomes 950). So the bot gets the player's
        // measured steady gain here (raider_bot_energy_rate, per second, 0 = off) through the same add function.
        static LONG64 lastGain = 0;
        if (g_EnergyRate > 0.0f && form != 0)
        {
            const LONG64 elapsed = lastGain == 0 ? 0 : now - lastGain;
            if (elapsed > 0 && elapsed <= 1000)
                reinterpret_cast<AddEnergyFn>(g_Base + kAddEnergyRva)(form, g_EnergyRate * static_cast<float>(elapsed) / 1000.0f);
        }
        lastGain = now;
        if (g_EnergyTest && form != 0)
        {
            static float lastEnergy = -1.0f;
            static LONG64 sameSince = 0;
            const float energy = At<float>(form + 0xA0);
            const float needed = At<float>(form + 0xA8);
            if (energy != lastEnergy)
            {
                lastEnergy = energy;
                sameSince = now;
            }
            else if (now - sameSince >= 10000 && needed > energy && g_EnergyFills < 8)
            {
                ++g_EnergyFills;
                sameSince = now;
                reinterpret_cast<AddEnergyFn>(g_Base + kAddEnergyRva)(form, needed - energy);
                Logger::Info("RaiderBot: energy stuck at " + std::to_string(energy) + " for 10 s - test fill to " +
                             std::to_string(At<float>(form + 0xA0)) + "/" + std::to_string(needed) +
                             " lock=" + std::to_string(At<std::uint8_t>(form + 0x14)));
                Logger::Flush();
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
            // Evolution energy (2026-11-11): the HUD Raider gauge (sub_1408DA9C0) reads it from this same form
            // object: +0xA0 = current energy (float), +0xA8 = energy needed for the next level (float). Energy is
            // added only by sub_1400CCC40(form, amount), which does nothing while the byte +0x14 is set.
            if (rivalState != 0)
                line << " energy=" << At<float>(rivalState + 0xA0) << "/" << At<float>(rivalState + 0xA8)
                     << " lock=" << static_cast<int>(At<std::uint8_t>(rivalState + 0x14));
            Logger::Info(line.str());
            Logger::Flush();
        }
    }

    // Input trace (input_trace=1, 2026-11-12): what the PLAYER's character presses, to copy the Raider's
    // area-destroying move for the bot. Every press goes into the character's input ring (character +0x5B8):
    // sub_14027B1B0(input, bits) writes entry i at input + (i + 1) * 48 (bits = first dword) and the next write
    // position at input +0xC34 (64 entries). Read from the bot update (about every 15 ms); once a second the
    // pressed bit values are logged with counts, plus what is in reach (+0x1A94/+0x1A98) when that changes.
    bool g_InputTrace = false;
    volatile LONG64 g_LastInputPoll = 0;

    void InputTraceTick()
    {
        const LONG64 now = static_cast<LONG64>(GetTickCount64());
        const LONG64 last = g_LastInputPoll;
        if (now - last < 15 || InterlockedCompareExchange64(&g_LastInputPoll, now, last) != last)
            return;
        const std::uintptr_t player = FindPlayer();
        if (player == 0)
            return;
        const std::uintptr_t input = At<std::uintptr_t>(player + 0x5B8);
        if (input == 0)
            return;
        static std::uintptr_t lastInput = 0;
        static std::uint32_t readPos = 0;
        static std::map<std::uint32_t, int> counts;
        static LONG64 lastLog = 0;
        static std::string lastReach;
        const std::uint32_t writePos = At<std::uint32_t>(input + 0xC34) & 0x3F;
        if (input != lastInput)
        {
            lastInput = input;
            readPos = writePos;
        }
        while (readPos != writePos)
        {
            const std::uint32_t bits = At<std::uint32_t>(input + (readPos + 1) * 48);
            ++counts[bits];
            readPos = (readPos + 1) & 0x3F;
        }
        const std::uintptr_t inReach = reinterpret_cast<CharacterPtrFn>(g_Base + kInteractionSlotRva)(player);
        const std::string reach = inReach != 0 ? std::to_string(At<std::uint32_t>(player + 0x1A94)) + "/" +
                                                 std::to_string(At<std::uint32_t>(player + 0x1A98)) : std::string("none");
        if (reach != lastReach)
        {
            lastReach = reach;
            Logger::Info("InputTrace: in reach " + reach);
        }
        if (now - lastLog >= 1000 && !counts.empty())
        {
            lastLog = now;
            std::ostringstream line;
            line << "InputTrace: pressed";
            for (const auto& [bits, n] : counts)
                line << " 0x" << std::hex << bits << std::dec << "x" << n;
            const std::uintptr_t form = At<std::uintptr_t>(player + 0x2E0);
            if (form != 0)
                line << " (level " << reinterpret_cast<RivalLevelFn>(g_Base + kRivalLevelRva)(form) << ")";
            Logger::Info(line.str());
            Logger::Flush();
            counts.clear();
        }
    }

    // Counter-attack test key (counter_test_key, off by default; 2026-11-15). User: "lets do a test key for the
    // counter attack". The Raider's strike back at a weaker survivor's Dragon Change melee hit is action entry 43
    // of the Raider's own action file (state 0x43D29502). The game starts it when the character's requested
    // reaction number, (character +0x590) -> +0x14, is 0x31: the state machine set-up sub_1401287F0 registers the
    // condition sub_1401371F0 ("is the requested number 0x31?", then it sets the number back to 0). Survivors
    // cannot transform and attack yet, so this key writes that number for the LOCAL PLAYER's Raider - nothing
    // else is changed. counter_test_key = a Windows virtual-key number (119 = F8); 0 = off. Read from the bot
    // update like the input trace, so it needs at least one bot in the match; only while the game is the window
    // in front. Private testing in local matches only.
    int g_CounterTestKey = 0;
    volatile LONG64 g_LastCounterPoll = 0;
    constexpr std::uintptr_t kCharReaction = 0x590;             // character -> reaction request object
    constexpr std::uintptr_t kReactionRequest = 0x14;           // requested reaction number (0 = none)
    constexpr std::uint32_t kReactionCounterAttack = 0x31;

    void CounterTestTick()
    {
        const LONG64 now = static_cast<LONG64>(GetTickCount64());
        const LONG64 last = g_LastCounterPoll;
        if (now - last < 15 || InterlockedCompareExchange64(&g_LastCounterPoll, now, last) != last)
            return;
        static bool wasDown = false;
        const bool down = (GetAsyncKeyState(g_CounterTestKey) & 0x8000) != 0;
        if (!down || wasDown)
        {
            wasDown = down;
            return;
        }
        wasDown = true;
        DWORD frontProcess = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &frontProcess);
        if (frontProcess != GetCurrentProcessId())
            return;                                            // the key was pressed in another program
        const std::uintptr_t player = FindPlayer();
        if (player == 0 || At<std::uintptr_t>(player) != g_Base + kRivalVftableRva)
        {
            Logger::Info("CounterTest: key pressed, but the player is not a Raider in a match - nothing done");
            Logger::Flush();
            return;
        }
        const std::uintptr_t reaction = At<std::uintptr_t>(player + kCharReaction);
        if (reaction == 0)
        {
            Logger::Info("CounterTest: key pressed, but the Raider has no reaction object (+0x590) - nothing done");
            Logger::Flush();
            return;
        }
        const std::uint32_t before = At<std::uint32_t>(reaction + kReactionRequest);
        At<std::uint32_t>(reaction + kReactionRequest) = kReactionCounterAttack;
        Logger::Info("CounterTest: key pressed - requested reaction 0x31 (counter-attack) for the player's Raider, type " +
            std::to_string(At<std::uint32_t>(player + 0x20C)) + " (the request was " + std::to_string(before) + ")");
        Logger::Flush();
    }

    // Raider loadout test key (loadout_test_key, off by default; 2026-11-15). User: "do the step toward custom
    // loadouts" -> "go ahead". Read from the exe (docs\prompt_log.md, "step G2"): a Raider's loadout is
    // XG::Game::RivalBattleEquip (0x6F0 bytes): +8 Raider id, +0xC form entries [variant 0..1][form 0..4] of 0xB0
    // bytes: +0x00 8 skill records {u32 skill id, u32 value, u8 flag} (4 actives, then 4 passives), +0x60 8 emotes,
    // +0x80 button map 4 x {u32 skill id, u32 record index}, +0xA0 the 2 supers. The player's own list
    // GameData::RivalEquipDataList = *( *( *(0x141782510) + 0x18 ) + 0x70 ), entries from +0x10 to +0x18 (0x6F0
    // each). When a Raider is picked the own entry is copied into the per-player block (*(0x14177E470) + 0x38 +
    // slot * 0xCD0, loadout at +0x5C8, Raider id at +0x5D0) and the skill set-up sub_1402F2040 fills button k from
    // records[map[k].index]. The key SWAPS records 0 and 3 of one form (variant 0) - default Raider 400 (Vegeta),
    // form 3 = Scouter <-> the skill of the 4th button - in the list entry and, if the player's block already holds
    // that Raider, in the block too; the map's skill ids follow the records. Pressing again swaps back. It runs on
    // its own thread so it also works in the lobby; that thread only reads and writes data (no game functions),
    // inside a fault guard. In a match (bot update) the player's Raider skill table (ActiveSkillParam = character
    // +0x2F8, +0xC: 5 forms x 4 buttons x 12 bytes) and its path flag (character +0x204: 1 = buttons from the
    // loadout, 0 = from the tables) are logged whenever they change. Private testing in local matches only.
    // Test 2 (same day): the swap only repeated what the game's own Raider Skills screen does (button order is kept
    // by skill id), so the key now REPLACES one record's skill instead: loadout_test_skill (default 80330 = Ki
    // Explode Blast) goes into record loadout_test_record (default 0 = the 1st button's record) of the form; the map
    // entry pointing at that record takes the new id; pressing again puts the original skill back.
    int g_LoadoutTestKey = 0;
    std::uint32_t g_LoadoutTestRaider = 400;
    std::uint32_t g_LoadoutTestForm = 3;
    std::uint32_t g_LoadoutTestSkill = 80330;
    std::uint32_t g_LoadoutTestRecord = 0;
    std::uint32_t g_LoadoutOriginal = 0;                       // the skill that was replaced (for the way back)
    constexpr std::uintptr_t kPlayDataGlobalRva = 0x1782510;   // pointer; +0x18 -> object; its +0x70 -> RivalEquipDataList
    constexpr std::uintptr_t kEquipEntrySize = 0x6F0;
    constexpr std::uintptr_t kEquipForms = 0xC;
    constexpr std::uintptr_t kEquipFormSize = 0xB0;
    constexpr std::uintptr_t kEquipButtonMap = 0x80;
    constexpr std::uintptr_t kPlayerBlockFirst = 0x38;
    constexpr std::uintptr_t kPlayerBlockSize = 0xCD0;
    constexpr std::uintptr_t kPlayerBlockEquip = 0x5C8;
    constexpr std::uintptr_t kCharPathFlag = 0x204;
    constexpr std::uintptr_t kCharActiveSkills = 0x2F8;

    struct LoadoutSwapResult
    {
        int list = 0;                 // 0 no list yet, 1 Raider not in the list, 2 swapped
        std::uint32_t listBefore[4] = {};
        std::uint32_t listAfter[4] = {};
        int block = 0;                // 0 no game / block, 1 block holds another Raider, 2 swapped
        std::uint32_t blockRaider = 0;
        std::uint32_t blockBefore[4] = {};
        std::uint32_t blockAfter[4] = {};
        bool fault = false;
    };

    // What the 4 buttons of one form show: records[map[k].index].id (-1 when the index is not a record).
    void ButtonView(std::uintptr_t form, std::uint32_t* out)
    {
        for (std::uintptr_t k = 0; k < 4; ++k)
        {
            const std::uint32_t index = At<std::uint32_t>(form + kEquipButtonMap + k * 8 + 4);
            out[k] = index < 8 ? At<std::uint32_t>(form + index * 12) : 0xFFFFFFFFu;
        }
    }

    void SwapFormRecords(std::uintptr_t equip, std::uint32_t formIndex, std::uint32_t* before, std::uint32_t* after)
    {
        const std::uintptr_t form = equip + kEquipForms + formIndex * kEquipFormSize;
        ButtonView(form, before);
        std::uint8_t temp[12];
        std::memcpy(temp, reinterpret_cast<const void*>(form), 12);
        std::memcpy(reinterpret_cast<void*>(form), reinterpret_cast<const void*>(form + 36), 12);
        std::memcpy(reinterpret_cast<void*>(form + 36), temp, 12);
        for (std::uintptr_t k = 0; k < 4; ++k)
        {
            const std::uint32_t index = At<std::uint32_t>(form + kEquipButtonMap + k * 8 + 4);
            if (index < 8)
                At<std::uint32_t>(form + kEquipButtonMap + k * 8) = At<std::uint32_t>(form + index * 12);
        }
        ButtonView(form, after);
    }

    void ReplaceFormRecord(std::uintptr_t equip, std::uint32_t formIndex, std::uint32_t* before, std::uint32_t* after)
    {
        const std::uintptr_t form = equip + kEquipForms + formIndex * kEquipFormSize;
        ButtonView(form, before);
        const std::uintptr_t record = form + g_LoadoutTestRecord * 12;
        const std::uint32_t current = At<std::uint32_t>(record);
        std::uint32_t next = g_LoadoutTestSkill;
        if (current == g_LoadoutTestSkill)
            next = g_LoadoutOriginal != 0 ? g_LoadoutOriginal : current;
        else if (g_LoadoutOriginal == 0)
            g_LoadoutOriginal = current;
        At<std::uint32_t>(record) = next;
        for (std::uintptr_t k = 0; k < 4; ++k)
        {
            if (At<std::uint32_t>(form + kEquipButtonMap + k * 8 + 4) == g_LoadoutTestRecord)
                At<std::uint32_t>(form + kEquipButtonMap + k * 8) = next;
        }
        ButtonView(form, after);
    }

    // Plain data only (no C++ objects), so the fault guard can wrap it.
    void LoadoutSwapRaw(LoadoutSwapResult& r)
    {
        __try
        {
            const std::uintptr_t playData = At<std::uintptr_t>(g_Base + kPlayDataGlobalRva);
            const std::uintptr_t holder = playData != 0 ? At<std::uintptr_t>(playData + 0x18) : 0;
            const std::uintptr_t list = holder != 0 ? At<std::uintptr_t>(holder + 0x70) : 0;
            if (list != 0)
            {
                r.list = 1;
                for (std::uintptr_t e = At<std::uintptr_t>(list + 0x10), end = At<std::uintptr_t>(list + 0x18);
                     e != 0 && e < end; e += kEquipEntrySize)
                {
                    if (At<std::uint32_t>(e + 8) == g_LoadoutTestRaider)
                    {
                        ReplaceFormRecord(e, g_LoadoutTestForm, r.listBefore, r.listAfter);
                        r.list = 2;
                        break;
                    }
                }
            }
            const std::uintptr_t game = At<std::uintptr_t>(g_Base + kGameGlobalPtrRva);
            if (game != 0)
            {
                const std::uint32_t slot = At<std::uint32_t>(game + kLocalPlayerSlot);
                if (slot < 8)
                {
                    const std::uintptr_t equip = game + kPlayerBlockFirst + slot * kPlayerBlockSize + kPlayerBlockEquip;
                    r.blockRaider = At<std::uint32_t>(equip + 8);
                    r.block = 1;
                    if (r.blockRaider == g_LoadoutTestRaider)
                    {
                        ReplaceFormRecord(equip, g_LoadoutTestForm, r.blockBefore, r.blockAfter);
                        r.block = 2;
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            r.fault = true;
        }
    }

    std::string Buttons(const std::uint32_t* ids)
    {
        std::string s;
        for (int k = 0; k < 4; ++k)
            s += (k ? ", " : "") + (ids[k] == 0xFFFFFFFFu ? std::string("none") : std::to_string(ids[k]));
        return s;
    }

    DWORD WINAPI LoadoutTestThread(LPVOID)
    {
        bool wasDown = false;
        for (;;)
        {
            Sleep(20);
            const bool down = (GetAsyncKeyState(g_LoadoutTestKey) & 0x8000) != 0;
            if (!down || wasDown)
            {
                wasDown = down;
                continue;
            }
            wasDown = true;
            DWORD frontProcess = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &frontProcess);
            if (frontProcess != GetCurrentProcessId())
                continue;                                          // the key was pressed in another program
            LoadoutSwapResult r;
            LoadoutSwapRaw(r);
            std::string line = "LoadoutTest: key pressed (Raider " + std::to_string(g_LoadoutTestRaider) + ", form " +
                std::to_string(g_LoadoutTestForm) + ", record " + std::to_string(g_LoadoutTestRecord + 1) + " <-> skill " +
                std::to_string(g_LoadoutTestSkill) + "). ";
            if (r.fault)
                line += "STOPPED by a memory fault - nothing more read. ";
            if (r.list == 0)
                line += "Saved list: not there yet. ";
            else if (r.list == 1)
                line += "Saved list: this Raider is not in it. ";
            else
                line += "Saved list buttons: " + Buttons(r.listBefore) + " -> " + Buttons(r.listAfter) + ". ";
            if (r.block == 0)
                line += "Match block: none.";
            else if (r.block == 1)
                line += "Match block: holds Raider " + std::to_string(r.blockRaider) + " - not changed.";
            else
                line += "Match block buttons: " + Buttons(r.blockBefore) + " -> " + Buttons(r.blockAfter) + ".";
            Logger::Info(line);
            Logger::Flush();
        }
    }

    // In a match: the player's Raider skill table and path flag, logged when they change (bot update thread).
    volatile LONG64 g_LastLoadoutTrace = 0;
    void LoadoutTraceTick()
    {
        const LONG64 now = static_cast<LONG64>(GetTickCount64());
        const LONG64 last = g_LastLoadoutTrace;
        if (now - last < 1000 || InterlockedCompareExchange64(&g_LastLoadoutTrace, now, last) != last)
            return;
        const std::uintptr_t player = FindPlayer();
        if (player == 0 || At<std::uintptr_t>(player) != g_Base + kRivalVftableRva)
            return;
        const std::uintptr_t skills = At<std::uintptr_t>(player + kCharActiveSkills);
        std::ostringstream line;
        line << "LoadoutTest: in match - Raider type " << At<std::uint32_t>(player + 0x20C) << ", path flag (+0x204) "
             << At<std::uint32_t>(player + kCharPathFlag);
        const std::uintptr_t form = At<std::uintptr_t>(player + 0x2E0);
        if (form != 0)
            line << ", level " << reinterpret_cast<RivalLevelFn>(g_Base + kRivalLevelRva)(form);
        if (skills == 0)
            line << ", no skill table";
        else
        {
            line << ", skill table (form: 4 buttons)";
            for (std::uintptr_t f = 0; f < 5; ++f)
            {
                line << (f ? " |" : "") << " " << f << ":";
                for (std::uintptr_t k = 0; k < 4; ++k)
                {
                    const std::uint32_t id = At<std::uint32_t>(skills + 0xC + f * 0x30 + k * 0xC);
                    line << (k ? "," : "") << (id == 0xFFFFFFFFu ? std::string("-") : std::to_string(id));
                }
            }
        }
        static std::string lastLine;
        if (line.str() == lastLine)
            return;
        lastLine = line.str();
        Logger::Info(lastLine);
        Logger::Flush();
    }

    // Survivor bots revive downed teammates (survivor_bot_revive, on by default; 2026-11-13). User: "in order to do
    // the 2nd finisher, i am required for survivors to revive one another when they are downed by the raider".
    // Every 250 ms each survivor bot (not downed itself) looks for the nearest downed survivor (Mob, role 0, flag
    // 0x20000000, not itself) within kReviveRange. Far away: it is sent there with the approach state 10001 (target
    // handle at AI+0x184C), the way the follower bot follows the player. Close (kReviveReach) with an interaction
    // offered (sub_140121600): it holds interact (input queue sub_14027B1B0(char+0x5B8, 2) on every update of its
    // own brain), the same press the Raider bot uses for finishers. What the game offers (kind/sub-kind at
    // +0x1A94/+0x1A98) is logged, since the revive interaction's numbers are not known yet. A body that offers
    // nothing for 4 s while the bot stands next to it is skipped by that bot for 60 s.
    // State is kept per bot slot (each bot's brain is updated by one thread at a time; no shared writes).
    // Test 1 (2026-11-13 15:09): bots stood on the body with 1/21 offered and pressed interact, nothing happened.
    // The player's revive (15:38, input_trace) = 1/21 in reach + bit 0x2 held every frame for ~8 s, i.e. the same
    // input; the bots' own brain kept replacing what they did. Now the bot is given the game's own revive action:
    // sub_1402BC6B0(brain, downed character) makes a SimpleAIActionOrientationInteract with kind 6 and the target
    // handle at +0x17AC; its update (sub_1402BA1E0 case 6) presses interact (sub_1402B1B20) every update while the
    // target still has the downed flag 0xA18 & 0x20000000, and reports done when it is gone.
    constexpr std::uintptr_t kStartReviveRva = 0x2BC6B0;            // sub_1402BC6B0(SimpleAI*, target unit)
    constexpr std::uintptr_t kOrientationInteractVftableRva = 0x11AD298;
    using StartReviveFn = void(__fastcall*)(std::uintptr_t, std::uintptr_t);
    bool g_SurvivorRevive = false;
    bool g_ReviveTrace = false;                      // revive_trace=1 (hooks and extra values further below)
    constexpr float kReviveRange = 150.0f;
    constexpr float kReviveReach = 4.0f;
    struct ReviveState
    {
        std::uintptr_t target = 0;
        LONG64 lastCheck = 0;
        LONG64 nothingSince = 0;
        LONG64 lastStatus = 0;
        LONG64 holdSince = 0;
        bool holding = false;
        std::string lastReach;
        SkippedUnit skipped[4] = {};
        std::uint32_t ringPos = 0xFFFFFFFF;           // bot's input ring read position (diagnostics)
        LONG64 lastRingLog = 0;
        std::map<std::uint32_t, int> ringCounts;
    };
    ReviveState g_Revive[kSlotCount];

    // Test 2 (2026-11-13 16:02): the game's own revive action ran on three bots for 8-18 s (step 1, still pressing
    // interact through sub_1402B1B20 = the same input queue bit 0x2 the player holds) and the teammate never got up.
    // The player stood still while holding; the bots' brain keeps steering them every frame. Now, while a bot holds a
    // revive, its brain update is skipped (it stands still) and the bot presses 0x2 itself every update, like the
    // player. Diagnostics: once a second the input bits found in the bot's own input ring (character +0x5B8, entries
    // at (i + 1) * 48, write position +0xC34) are logged, to compare with the player's successful revive (only 0x2).
    // Safety: a revive held for 15 s without the teammate getting up is dropped and that body skipped for 60 s.
    // 2026-11-16: the player's manual revive took 13.1 s and 13.2 s (log revive-compare-1), too close to 15 s -> 25 s.
    constexpr LONG64 kReviveGiveUpMs = 25000;

    // Revive trace 3 (2026-11-15): the character's state machine (character +0x598; built by sub_14012EE30 ->
    // sub_140131E90): +0x80 Possible_current state id, +0xB0 Possible_current state object, +0xD8 + id*8 the
    // state objects (StateInteract = id 12, vtable 0x14118FF38). Raw values, to compare bots with the player.
    // Revive trace 4: the StateInteract object of each slot, caught by InteractActionDetour (the action's owner;
    // owner +8 = character handle). Its fields while reviving: +0x48 Possible_current node, +0xD8 the target slot
    // cached by the claim check class_141192758 (sub_1401819B0: asks sub_140263710 kind 2 once, -1 before),
    // and the reviver's +0x1E60 (the target slot that check reads).
    std::uintptr_t g_StateInteractOf[kSlotCount + 1] = {};

    std::string StateInfo(std::uintptr_t c)
    {
        std::ostringstream s;
        s << "target(+0x1E60) " << At<std::int32_t>(c + 0x1E60);
        const std::uint32_t slot = At<std::uint32_t>(c + kCharSlot);
        const std::uintptr_t si = slot < kSlotCount ? g_StateInteractOf[slot] : 0;
        if (si != 0 && At<std::uintptr_t>(si) == g_Base + 0x118FF38 && CharacterAt(At<std::uint32_t>(si + 8)) == c)
            s << " | StateInteract +10 0x" << std::hex << At<std::uint64_t>(si + 0x10) << std::dec << " +48 " << At<std::int32_t>(si + 0x48)
              << " +4C " << At<std::int32_t>(si + 0x4C) << " +50 " << At<std::int32_t>(si + 0x50)
              << " +D8 " << At<std::int32_t>(si + 0xD8) << " +DC " << At<std::int32_t>(si + 0xDC)
              << " +E0 " << static_cast<int>(At<std::uint8_t>(si + 0xE0)) << " +E2 " << static_cast<int>(At<std::uint8_t>(si + 0xE2));
        else
            s << " | StateInteract not seen yet";
        return s.str();
    }

    void ReviveHold(std::uintptr_t self, std::uint32_t slot)
    {
        ReviveState& st = g_Revive[slot];
        const std::uintptr_t input = At<std::uintptr_t>(self + 0x5B8);
        if (input == 0)
            return;
        reinterpret_cast<PressInputFn>(g_Base + kPressInputRva)(input, 2);
        const std::uint32_t writePos = At<std::uint32_t>(input + 0xC34) & 0x3F;
        if (st.ringPos > 0x3F)
            st.ringPos = writePos;
        while (st.ringPos != writePos)
        {
            ++st.ringCounts[At<std::uint32_t>(input + (st.ringPos + 1) * 48)];
            st.ringPos = (st.ringPos + 1) & 0x3F;
        }
        const LONG64 now = static_cast<LONG64>(GetTickCount64());
        if (now - st.lastRingLog >= 1000 && !st.ringCounts.empty())
        {
            st.lastRingLog = now;
            std::ostringstream line;
            line << "ReviveBot: slot " << slot << " holding, input ring:";
            for (const auto& [bits, n] : st.ringCounts)
                line << " 0x" << std::hex << bits << std::dec << "x" << n;
            const std::uintptr_t inReach = reinterpret_cast<CharacterPtrFn>(g_Base + kInteractionSlotRva)(self);
            line << " | in reach " << (inReach != 0 ? std::to_string(At<std::uint32_t>(self + 0x1A94)) + "/" +
                                                       std::to_string(At<std::uint32_t>(self + 0x1A98)) : std::string("none"));
            if (g_ReviveTrace)
            {
                // Revive trace 2 (2026-11-15): the game's own answers, read-only calls.
                // sub_1402760B0 = Possible_IsInteractHeld(input): latest ring entry bit 0x2, blocked while input
                // byte +0xC3C has bit 0x2 (for characters not in the AI table sub_140207320).
                // sub_1402636B0 = Possible_IsReviveGranted(manager game+0x67E8, reviver slot, target slot).
                using InputQueryFn = std::uint8_t(__fastcall*)(std::uintptr_t);
                using GrantedFn = std::uint8_t(__fastcall*)(std::uintptr_t, std::uint32_t, std::uint32_t);
                const std::uint8_t held = reinterpret_cast<InputQueryFn>(g_Base + 0x2760B0)(input);
                const std::uint8_t aiTable = reinterpret_cast<InputQueryFn>(g_Base + 0x207320)(self);
                const std::int32_t readPos = At<std::int32_t>(input + 0xC30);
                const std::uint32_t entry = (readPos >= 0 && readPos < 64) ? At<std::uint32_t>(input + (readPos + 1) * 48) : 0;
                line << " | held " << static_cast<int>(held) << " c3c 0x" << std::hex << static_cast<int>(At<std::uint8_t>(input + 0xC3C))
                     << " entry 0x" << entry << std::dec << " aiTable " << static_cast<int>(aiTable)
                     << " state 0x" << std::hex << At<std::uint32_t>(self + 0xA10) << std::dec << " | " << StateInfo(self);
                const std::uintptr_t game = At<std::uintptr_t>(g_Base + kGameGlobalPtrRva);
                const std::uintptr_t manager = game != 0 ? At<std::uintptr_t>(game + 0x67E8) : 0;
                if (st.target != 0)
                {
                    const std::uint32_t targetSlot = At<std::uint32_t>(st.target + kCharSlot);
                    line << " | target slot " << targetSlot << " revivedBy(+0x1E80) " << At<std::int32_t>(st.target + 0x1E80);
                    if (manager != 0)
                        line << " granted " << static_cast<int>(reinterpret_cast<GrantedFn>(g_Base + 0x2636B0)(manager, slot, targetSlot));
                }
            }
            Logger::Info(line.str());
            st.ringCounts.clear();
        }
    }

    // Returns true while this bot is busy reviving (the follower logic then leaves it alone).
    bool SurvivorReviveTick(std::uintptr_t ai, std::uintptr_t self, std::uint32_t slot)
    {
        ReviveState& st = g_Revive[slot];
        if ((At<std::uint32_t>(self + kCharFlags) & 0x20000000) != 0 || At<std::uint32_t>(self + kCharRole) != 0)
        {
            st.target = 0;
            st.holding = false;
            return false;
        }
        const LONG64 now = static_cast<LONG64>(GetTickCount64());
        if (now - st.lastCheck >= 250)
        {
            st.lastCheck = now;
            const float* me = reinterpret_cast<const float*>(self + kCharPosition);
            std::uintptr_t best = 0;
            std::uint32_t bestHandle = 0;
            float bestDistance = kReviveRange * kReviveRange;
            for (std::uint32_t h = 1; h < kCharacterTableSize; ++h)
            {
                const std::uintptr_t unit = CharacterAt(h);
                if (unit == 0 || unit == self || At<std::uintptr_t>(unit) != g_Base + kMobVftableRva ||
                    At<std::uint32_t>(unit + kCharRole) != 0 || (At<std::uint32_t>(unit + kCharFlags) & 0x20000000) == 0)
                    continue;
                bool skipped = false;
                for (const auto& s : st.skipped)
                    skipped |= s.unit == unit && now < s.until;
                if (skipped)
                    continue;
                const float* p = reinterpret_cast<const float*>(unit + kCharPosition);
                const float dx = p[0] - me[0], dy = p[1] - me[1], dz = p[2] - me[2];
                const float d = dx * dx + dy * dy + dz * dz;
                if (d < bestDistance)
                {
                    bestDistance = d;
                    best = unit;
                    bestHandle = h;
                }
            }
            if (best != st.target)
            {
                if (best != 0)
                    Logger::Info("ReviveBot: slot " + std::to_string(slot) + " goes to downed survivor slot " +
                                 std::to_string(At<std::uint32_t>(best + kCharSlot)) + " (" +
                                 std::to_string(static_cast<int>(std::sqrt(bestDistance))) + " away)");
                else if (st.target != 0)
                    Logger::Info("ReviveBot: slot " + std::to_string(slot) + " has no downed teammate to revive");
                st.target = best;
                st.nothingSince = 0;
                st.lastReach.clear();
            }
            st.holding = false;
            if (best != 0)
            {
                const float distance = std::sqrt(bestDistance);
                const std::uintptr_t inReach = reinterpret_cast<CharacterPtrFn>(g_Base + kInteractionSlotRva)(self);
                const std::string reach = inReach != 0 ? std::to_string(At<std::uint32_t>(self + 0x1A94)) + "/" +
                                                         std::to_string(At<std::uint32_t>(self + 0x1A98)) : std::string("none");
                if (distance <= kReviveReach && reach != st.lastReach)
                {
                    st.lastReach = reach;
                    Logger::Info("ReviveBot: slot " + std::to_string(slot) + " next to downed slot " +
                                 std::to_string(At<std::uint32_t>(best + kCharSlot)) + " (" +
                                 std::to_string(distance) + " away), in reach " + reach);
                    Logger::Flush();
                }
                const std::uint32_t reachKind = inReach != 0 ? At<std::uint32_t>(self + 0x1A94) : 0xFFFFFFFF;
                const std::uint32_t reachSub = inReach != 0 ? At<std::uint32_t>(self + 0x1A98) : 0xFFFFFFFF;
                if (distance <= kReviveReach && inReach != 0 && reachKind == 1 && (reachSub == 21 || reachSub == 22))
                {
                    if (st.holdSince == 0)
                        st.holdSince = now;
                    st.holding = true;
                    st.nothingSince = 0;
                    if (now - st.holdSince >= kReviveGiveUpMs)
                    {
                        SkippedUnit* s = &st.skipped[0];
                        for (auto& x : st.skipped)
                            if (x.unit == best || now >= x.until) { s = &x; break; }
                        s->unit = best;
                        s->until = now + 60000;
                        Logger::Info("ReviveBot: slot " + std::to_string(slot) +
                                     " - revive held 25 s without success, body skipped for 60 s");
                        st.holding = false;
                        st.target = 0;
                        return false;
                    }
                    const std::uintptr_t action = At<std::uintptr_t>(ai + 0x18);
                    const bool already = action != 0 && At<std::uintptr_t>(action) == g_Base + kOrientationInteractVftableRva &&
                                         At<std::uint32_t>(action + 0x1790) == 6 && At<std::uint32_t>(action + 0x17AC) == bestHandle;
                    if (!already)
                    {
                        reinterpret_cast<StartReviveFn>(g_Base + kStartReviveRva)(ai, best);
                        Logger::Info("ReviveBot: slot " + std::to_string(slot) + " starts the game's revive action on slot " +
                                     std::to_string(At<std::uint32_t>(best + kCharSlot)));
                    }
                    else if (now - st.lastStatus >= 2000)
                    {
                        st.lastStatus = now;
                        Logger::Info("ReviveBot: slot " + std::to_string(slot) + " reviving: step " +
                                     std::to_string(At<std::uint32_t>(action + 0x1700)) + " result " +
                                     std::to_string(At<std::int32_t>(action + 0x1704)) + " t " +
                                     std::to_string(At<float>(action + 0x170C)));
                    }
                }
                else if (distance <= kReviveReach && inReach != 0)
                    st.nothingSince = 0;   // something else is offered (another body closer, an item): keep waiting
                else if (distance <= kReviveReach)
                {
                    if (st.nothingSince == 0)
                        st.nothingSince = now;
                    else if (now - st.nothingSince >= 4000)
                    {
                        SkippedUnit* s = &st.skipped[0];
                        for (auto& x : st.skipped)
                            if (x.unit == best || now >= x.until) { s = &x; break; }
                        s->unit = best;
                        s->until = now + 60000;
                        Logger::Info("ReviveBot: slot " + std::to_string(slot) +
                                     " - this body offers nothing to revive for 4 s, skipped for 60 s");
                        st.target = 0;
                        return false;
                    }
                }
                const std::uint32_t state = At<std::uint32_t>(ai + kAiState);
                const std::int32_t next = At<std::int32_t>(ai + kAiNextState);
                if (!st.holding && distance > 1.5f && state != kStateApproach && state != kStateDowned && next < 0)
                {
                    AssignHandle(ai + kAiFollowTarget, bestHandle);
                    At<std::uint32_t>(ai + kAiNextState) = kStateApproach;
                }
            }
            if (!st.holding)
                st.holdSince = 0;                    // the 15 s limit counts one uninterrupted hold
        }
        return st.target != 0;
    }

    // Bot attack test (2026-11-15, survivor_bot_attack=1). Private testing in local matches only.
    // The game's own battle AI (SimpleAIActionEpisodeBattleVsPlayer, sub_1402B8B00) locks on with
    // sub_140214290(self, target) and picks a command: target 5 m or more away -> 1 (three times in four) or 2;
    // nearer -> 0. sub_1402B1430 turns it into input bits pushed with sub_14027B1B0(character +0x5B8, bits):
    // 0 -> 0x1 (only while character byte +0x435 is set; otherwise nothing), 1 -> 0x100, 2 -> 0x400.
    // This test does the same for our survivor bots against the Raider (within 30 m: walk to it, lock on, press),
    // and logs every 2 s which bits were pressed and byte +0x435, so we learn which press does what.
    constexpr std::uintptr_t kLockOnRva = 0x214290;                 // sub_140214290(UnitCharacter*, target)
    using LockOnFn = std::uint64_t(__fastcall*)(std::uintptr_t, std::uintptr_t);
    bool g_BotAttack = false;
    constexpr float kAttackEngageRange = 30.0f;
    constexpr float kAttackNear = 5.0f;
    struct AttackState
    {
        std::uintptr_t target = 0;
        std::uint32_t targetHandle = 0;
        float distance = 0.0f;
        LONG64 lastCheck = 0;
        LONG64 nextPress = 0;
        LONG64 lastLog = 0;
        std::uint32_t farCount = 0;
        std::map<std::uint32_t, int> pressed;
    };
    AttackState g_Attack[kSlotCount];

    // Returns true while this bot is attacking the Raider (the follower logic then leaves it alone).
    bool SurvivorAttackTick(std::uintptr_t ai, std::uintptr_t self, std::uint32_t slot)
    {
        AttackState& st = g_Attack[slot];
        if ((At<std::uint32_t>(self + kCharFlags) & 0x20000000) != 0 || At<std::uint32_t>(self + kCharRole) != 0)
        {
            st.target = 0;
            return false;
        }
        const LONG64 now = static_cast<LONG64>(GetTickCount64());
        const float* me = reinterpret_cast<const float*>(self + kCharPosition);
        if (now - st.lastCheck >= 250)
        {
            st.lastCheck = now;
            std::uintptr_t best = 0;
            std::uint32_t bestHandle = 0;
            float bestDistance = kAttackEngageRange * kAttackEngageRange;
            for (std::uint32_t h = 1; h < kCharacterTableSize; ++h)
            {
                const std::uintptr_t unit = CharacterAt(h);
                if (unit == 0 || unit == self || At<std::uintptr_t>(unit) != g_Base + kRivalVftableRva)
                    continue;
                const float* p = reinterpret_cast<const float*>(unit + kCharPosition);
                const float dx = p[0] - me[0], dy = p[1] - me[1], dz = p[2] - me[2];
                const float d = dx * dx + dy * dy + dz * dz;
                if (d < bestDistance)
                {
                    bestDistance = d;
                    best = unit;
                    bestHandle = h;
                }
            }
            if (best != st.target)
            {
                if (best != 0)
                    Logger::Info("AttackBot: slot " + std::to_string(slot) + " attacks the Raider (" +
                                 std::to_string(static_cast<int>(std::sqrt(bestDistance))) + " away)");
                else
                    Logger::Info("AttackBot: slot " + std::to_string(slot) + " - Raider out of range, stops attacking");
                Logger::Flush();
                st.target = best;
            }
            st.targetHandle = bestHandle;
            st.distance = std::sqrt(bestDistance);
            // User 2026-11-15: without Dragon Change a survivor's attack is the megaton punch and the Raider slaps
            // them. So only a bot with byte +0x435 set (Possible_DragonChanged) walks up to the Raider.
            if (best != 0 && At<std::uint8_t>(self + 0x435) != 0)
            {
                const std::uint32_t state = At<std::uint32_t>(ai + kAiState);
                const std::int32_t next = At<std::int32_t>(ai + kAiNextState);
                if (st.distance > 4.0f && state != kStateApproach && state != kStateDowned && next < 0)
                {
                    AssignHandle(ai + kAiFollowTarget, bestHandle);
                    At<std::uint32_t>(ai + kAiNextState) = kStateApproach;
                }
            }
        }
        if (st.target == 0 || CharacterAt(st.targetHandle) != st.target)
            return false;
        const std::uintptr_t input = At<std::uintptr_t>(self + 0x5B8);
        if (input == 0)
            return true;
        if (now >= st.nextPress)
        {
            reinterpret_cast<LockOnFn>(g_Base + kLockOnRva)(self, st.target);
            const bool battleByte = At<std::uint8_t>(self + 0x435) != 0;
            // Same rule as the game's AI: near and not Dragon Changed -> press nothing (no megaton punch).
            std::uint32_t bits = 0;
            if (st.distance < kAttackNear)
                bits = battleByte ? 0x1 : 0;
            else
                bits = (++st.farCount % 4 == 0) ? 0x400 : 0x100;
            if (bits != 0)
            {
                reinterpret_cast<PressInputFn>(g_Base + kPressInputRva)(input, bits);
                ++st.pressed[bits];
            }
            else
                ++st.pressed[0];
            st.nextPress = now + (bits == 0x1 ? 300 : 900);
        }
        if (now - st.lastLog >= 2000 && !st.pressed.empty())
        {
            st.lastLog = now;
            std::ostringstream line;
            line << "AttackBot: slot " << slot << " " << static_cast<int>(st.distance) << " m from the Raider, byte+0x435="
                 << static_cast<int>(At<std::uint8_t>(self + 0x435)) << ", pressed:";
            for (const auto& [bits, n] : st.pressed)
                line << " 0x" << std::hex << bits << std::dec << "x" << n;
            Logger::Info(line.str());
            Logger::Flush();
            st.pressed.clear();
        }
        return true;
    }

    // Revive trace (2026-11-15, revive_trace=1). Private testing in local matches only. Pass-through hooks, no
    // behavior change: they log (once a second per slot and step) how far each character gets in the revive chain.
    //   1. sub_140174A90  StateInteract's interact action (class_141190378 slot 2); switch on the offered kind
    //                     (+0x1A94), kind 1 for a survivor -> 2.
    //   2. sub_1400B2450  Possible_StartReviveInteract(?, target slot, character, flag) -> builds the request -> 3.
    //   3. sub_140702B80  Possible_InteractManager_Request(manager, flag, request {slot, kind, target, sub, ...})
    //   4. sub_140701800  Possible_InteractManager_Order(manager, a, b, target, slot, flag) (PacketInteractOrder)
    constexpr std::uintptr_t kInteractActionRva = 0x174A90;
    constexpr std::uintptr_t kStartReviveRequestRva = 0xB2450;
    constexpr std::uintptr_t kInteractRequestRva = 0x702B80;
    constexpr std::uintptr_t kInteractOrderRva = 0x701800;
    using InteractActionFn = std::uint64_t(__fastcall*)(std::uintptr_t);
    using StartReviveRequestFn = void(__fastcall*)(std::uintptr_t, std::uint64_t, std::uintptr_t, std::uint8_t);
    using InteractRequestFn = std::uint8_t(__fastcall*)(std::uintptr_t, std::uint8_t, std::uintptr_t);
    using InteractOrderFn = std::uint8_t(__fastcall*)(std::uintptr_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                                      std::int32_t, std::uint8_t);
    InteractActionFn g_OriginalInteractAction = nullptr;
    StartReviveRequestFn g_OriginalStartReviveRequest = nullptr;
    InteractRequestFn g_OriginalInteractRequest = nullptr;
    InteractOrderFn g_OriginalInteractOrder = nullptr;
    void* g_InteractActionTarget = nullptr;
    void* g_StartReviveRequestTarget = nullptr;
    void* g_InteractRequestTarget = nullptr;
    void* g_InteractOrderTarget = nullptr;
    LONG64 g_ReviveTraceLast[4][kSlotCount + 1] = {};

    bool ReviveTraceDue(int step, std::uint32_t slot)
    {
        if (slot > kSlotCount)
            slot = kSlotCount;
        const LONG64 now = static_cast<LONG64>(GetTickCount64());
        if (now - g_ReviveTraceLast[step][slot] < 1000)
            return false;
        g_ReviveTraceLast[step][slot] = now;
        return true;
    }

    std::string TraceWho(std::uint32_t slot)
    {
        std::string who = "slot " + std::to_string(slot);
        if (slot < kSlotCount && (g_BotSlotMask & (1u << slot)) != 0)
            who += " (bot)";
        return who;
    }

    std::uint64_t __fastcall InteractActionDetour(std::uintptr_t action)
    {
        const std::uintptr_t state = action != 0 ? At<std::uintptr_t>(action) : 0;
        const std::uintptr_t c = state != 0 ? CharacterAt(At<std::uint32_t>(state + 8)) : 0;
        if (IsCharacter(c))
        {
            const std::uint32_t slot = At<std::uint32_t>(c + kCharSlot);
            if (slot < kSlotCount && At<std::uintptr_t>(state) == g_Base + 0x118FF38)
                g_StateInteractOf[slot] = state;
            if (ReviveTraceDue(0, slot))
            {
                Logger::Info("ReviveTrace 1: " + TraceWho(slot) + " in StateInteract, offered " +
                             std::to_string(At<std::int32_t>(c + 0x1A94)) + "/" + std::to_string(At<std::int32_t>(c + 0x1A98)) +
                             ", control " + std::to_string(At<std::uint32_t>(c + 0x1FC)) +
                             ", role " + std::to_string(At<std::uint32_t>(c + kCharRole)));
                Logger::Flush();
            }
        }
        return g_OriginalInteractAction(action);
    }

    void __fastcall StartReviveRequestDetour(std::uintptr_t a1, std::uint64_t target, std::uintptr_t character, std::uint8_t flag)
    {
        if (IsCharacter(character))
        {
            const std::uint32_t slot = At<std::uint32_t>(character + kCharSlot);
            if (ReviveTraceDue(1, slot))
            {
                Logger::Info("ReviveTrace 2: " + TraceWho(slot) + " builds a revive request for slot " +
                             std::to_string(static_cast<std::uint32_t>(target)) + " (flag " + std::to_string(flag) + ")");
                Logger::Flush();
            }
        }
        g_OriginalStartReviveRequest(a1, target, character, flag);
    }

    std::uint8_t __fastcall InteractRequestDetour(std::uintptr_t manager, std::uint8_t flag, std::uintptr_t request)
    {
        const std::uint8_t result = g_OriginalInteractRequest(manager, flag, request);
        if (request != 0)
        {
            const std::uint32_t slot = At<std::uint32_t>(request);
            if (ReviveTraceDue(2, slot))
            {
                Logger::Info("ReviveTrace 3: " + TraceWho(slot) + " interact request kind " +
                             std::to_string(At<std::int32_t>(request + 4)) + " target " + std::to_string(At<std::int32_t>(request + 8)) +
                             " sub " + std::to_string(At<std::int32_t>(request + 0xC)) + " flag " + std::to_string(flag) +
                             " -> manager answered " + std::to_string(result));
                Logger::Flush();
            }
        }
        return result;
    }

    std::uint8_t __fastcall InteractOrderDetour(std::uintptr_t manager, std::uint32_t a, std::uint32_t b, std::uint32_t target,
                                                std::int32_t slot, std::uint8_t flag)
    {
        const std::uint8_t result = g_OriginalInteractOrder(manager, a, b, target, slot, flag);
        if (ReviveTraceDue(3, a))
        {
            Logger::Info("ReviveTrace 4: interact order a=" + std::to_string(a) + " b=" + std::to_string(b) + " target " +
                         std::to_string(target) + " slot " + std::to_string(slot) + " flag " + std::to_string(flag) +
                         " -> " + std::to_string(result));
            Logger::Flush();
        }
        return result;
    }

    // Revive comparison (2026-11-16, revive_trace=1, log only - no behavior change). From the decomp of 2026-11-16:
    // the revive reservations ("exclusive revive" locks) sit behind the pointer at game +0x67E8 (read by the claim
    // step sub_1401819B0): 8 requests of 0x28 bytes at +0x000 {u32 slot, s32 kind, stopwatch +8, bool pending +0x20}
    // and 8 lock slots of 12 bytes at +0x140 {u8 state (1 = taken), s32 owner (reviver's slot), s32 kind}, one per
    // player slot being revived. The claim step starts from the reviver's +0x1E60 (slot of the player it revives) and
    // gives up when that names nobody, before it asks for a lock (kind 2); 2026-11-15 the bots always had -1 there.
    // Every 250 ms the whole picture is built (all 8 slots: downed flag, +0x1E60 reviving, +0x1E80 revived by,
    // offered kind/sub-kind; taken locks; pending requests). Logged at once when it changed ("CHANGE"), and every 2 s
    // while someone is downed. Purpose: compare a MANUAL player revive (hold, no item) with the bots' attempts.
    constexpr std::uintptr_t kReviveLocksPtr = 0x67E8;   // game + 0x67E8 -> revive lock manager
    std::mutex g_ReviveCompareMutex;
    std::string g_ReviveCompareLast;
    volatile LONG64 g_ReviveCompareTick = 0;
    LONG64 g_ReviveCompareFull = 0;

    void ReviveCompareTick()
    {
        const LONG64 now = static_cast<LONG64>(GetTickCount64());
        if (now - g_ReviveCompareTick < 250)
            return;
        std::unique_lock<std::mutex> guard(g_ReviveCompareMutex, std::try_to_lock);   // bot brains run on several threads
        if (!guard.owns_lock() || now - g_ReviveCompareTick < 250)
            return;
        g_ReviveCompareTick = now;

        const std::uintptr_t game = At<std::uintptr_t>(g_Base + kGameGlobalPtrRva);
        if (game == 0 || At<std::uintptr_t>(game + kCharacterManager) == 0)
            return;
        const auto bySlot = reinterpret_cast<CharacterBySlotFn>(g_Base + kCharacterBySlotRva);
        const std::uint32_t playerSlot = At<std::uint32_t>(game + kLocalPlayerSlot);

        std::ostringstream chars;
        bool anyDowned = false;
        for (std::uint32_t s = 0; s < kSlotCount; ++s)
        {
            const std::uintptr_t c = bySlot(At<std::uintptr_t>(game + kCharacterManager), s);
            if (!IsCharacter(c))
                continue;
            const bool downed = (At<std::uint32_t>(c + kCharFlags) & 0x20000000) != 0;
            anyDowned = anyDowned || downed;
            chars << " | " << s << (s == playerSlot ? "(you)" : ((g_BotSlotMask & (1u << s)) != 0 ? "(bot)" : ""))
                  << (downed ? " DOWN" : "") << " reviving " << At<std::int32_t>(c + 0x1E60)
                  << " revivedBy " << At<std::int32_t>(c + 0x1E80)
                  << " offered " << At<std::int32_t>(c + 0x1A94) << "/" << At<std::int32_t>(c + 0x1A98);
        }

        std::ostringstream locks;
        const std::uintptr_t manager = At<std::uintptr_t>(game + kReviveLocksPtr);
        if (manager == 0)
        {
            locks << "lock manager: none";
        }
        else
        {
            locks << "locks:";
            bool anyLock = false;
            for (std::uint32_t s = 0; s < kSlotCount; ++s)
            {
                const std::uintptr_t e = manager + 0x140 + s * 12;
                if (At<std::uint8_t>(e) != 0)
                {
                    locks << " [slot " << s << " state " << static_cast<int>(At<std::uint8_t>(e)) << " owner "
                          << At<std::int32_t>(e + 4) << " kind " << At<std::int32_t>(e + 8) << "]";
                    anyLock = true;
                }
            }
            if (!anyLock)
                locks << " all free";
            locks << "; requests:";
            bool anyRequest = false;
            for (std::uint32_t w = 0; w < kSlotCount; ++w)
            {
                const std::uintptr_t r = manager + w * 0x28;
                if (At<std::uint8_t>(r + 0x20) != 0)
                {
                    locks << " [" << w << " wants slot " << At<std::int32_t>(r) << " kind " << At<std::int32_t>(r + 4) << "]";
                    anyRequest = true;
                }
            }
            if (!anyRequest)
                locks << " none";
        }

        const std::string state = locks.str() + chars.str();
        const bool changed = state != g_ReviveCompareLast;
        if (changed || (anyDowned && now - g_ReviveCompareFull >= 2000))
        {
            g_ReviveCompareFull = now;
            g_ReviveCompareLast = state;
            Logger::Info(std::string(changed ? "ReviveCompare CHANGE: " : "ReviveCompare: ") + state);
            Logger::Flush();
        }
    }

    std::uint64_t __fastcall AiUpdateDetour(std::uintptr_t ai, float dt)
    {
        if (g_InputTrace)
            InputTraceTick();
        if (g_ReviveTrace)
            ReviveCompareTick();
        if (g_ReviveTrace)
        {
            // the player's side, for comparison: once a second while the player is offered a revive (kind 1)
            static LONG64 s_lastPlayerState = 0;
            const LONG64 now = static_cast<LONG64>(GetTickCount64());
            if (now - s_lastPlayerState >= 1000)
            {
                s_lastPlayerState = now;
                const std::uintptr_t player = FindPlayer();
                if (player != 0 && At<std::int32_t>(player + 0x1A94) == 1)
                {
                    const std::uintptr_t input = At<std::uintptr_t>(player + 0x5B8);
                    std::string line = "ReviveTrace P: player slot " + std::to_string(At<std::uint32_t>(player + kCharSlot)) +
                                       " offered 1/" + std::to_string(At<std::int32_t>(player + 0x1A98));
                    if (input != 0)
                    {
                        const std::int32_t readPos = At<std::int32_t>(input + 0xC30);
                        const std::uint32_t entry = (readPos >= 0 && readPos < 64) ? At<std::uint32_t>(input + (readPos + 1) * 48) : 0;
                        std::ostringstream e;
                        e << " entry 0x" << std::hex << entry << " c3c 0x" << static_cast<int>(At<std::uint8_t>(input + 0xC3C));
                        line += e.str();
                    }
                    Logger::Info(line + " | " + StateInfo(player));
                    Logger::Flush();
                }
            }
        }
        if (g_CounterTestKey != 0)
            CounterTestTick();
        if (g_LoadoutTestKey != 0)
            LoadoutTraceTick();
        if (g_RaiderAbsorb)
        {
            RaiderAbsorbTick();
            RaiderHoldInteract();
        }
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
            // Revive diagnostic (2026-11-15): character +0x1FC = who controls it. sub_140206DE0 = Possible_IsRemote
            // (2 or 3) and sub_1402069C0 = Possible_IsControlledHere (0 or 1); the interact action
            // (class_141190378 slot 2 = sub_140174A90) and the interact request (sub_1400B2450 -> sub_140702B80)
            // do nothing for a "remote" character. Logged once per slot and match to see what our bots are.
            static std::uintptr_t s_loggedControl[kSlotCount] = {};
            if (slot < kSlotCount && s_loggedControl[slot] != self)
            {
                s_loggedControl[slot] = self;
                std::string line = "PracticeBots: slot " + std::to_string(slot) + " control +0x1FC = " +
                                   std::to_string(At<std::uint32_t>(self + 0x1FC)) + ", role " +
                                   std::to_string(At<std::uint32_t>(self + kCharRole)) +
                                   ((g_BotSlotMask & (1u << slot)) != 0 ? " (our bot)" : "");
                const std::uintptr_t player = FindPlayer();
                if (player != 0)
                    line += " | player slot " + std::to_string(At<std::uint32_t>(player + kCharSlot)) +
                            " control = " + std::to_string(At<std::uint32_t>(player + 0x1FC));
                Logger::Info(line);
                Logger::Flush();
            }
        }
        bool reviving = false;
        if (g_SurvivorRevive && IsCharacter(self))
        {
            const std::uint32_t slot = At<std::uint32_t>(self + kCharSlot);
            if (slot < kSlotCount && (g_BotSlotMask & (1u << slot)) != 0)
            {
                reviving = SurvivorReviveTick(ai, self, slot);
                if (g_Revive[slot].holding)
                {
                    ReviveHold(self, slot);
                    return 0;                        // brain paused: the bot stands still and holds interact
                }
            }
        }
        bool attacking = false;
        if (g_BotAttack && !reviving && IsCharacter(self))
        {
            const std::uint32_t slot = At<std::uint32_t>(self + kCharSlot);
            if (slot < kSlotCount && (g_BotSlotMask & (1u << slot)) != 0)
                attacking = SurvivorAttackTick(ai, self, slot);
        }
        if (g_FollowerSlot < kSlotCount && !reviving && !attacking)
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

    // Bot slot OWNER (survivor_bot_owner, on by default; 2026-11-16). Found by comparing the player's manual revive
    // with the bots' (log revive-compare-1) and reading the interact manager: a request's gauge (record +0x40 progress
    // vs +0x48 required, record update sub_140701C50) only FINISHES when the requesting player is "controlled on this
    // machine" (sub_1408BE6B0): control 0, or control 1 with the slot's owner (+0xD00) == the local player's slot.
    // The game's own practice Raider bot gets its owner from sub_1408BF360: after sub_1408C0B70 it writes
    // slot +0xD00 = sub_140C60F70() (the host's slot: the session's host when one exists, else the local slot at
    // game +0x66B8). We only called sub_1408C0B70 for survivor bots, so their owner stayed -1 (the reset value,
    // sub_14057B110) and nothing they started through the interact manager (revive; also boxes / civilians) could
    // ever finish. Now the owner is written the same way the game does it.
    constexpr std::uintptr_t kSlotOwner = 0xD00;               // slot +0xD00: owning player's slot (-1 none)
    constexpr std::uintptr_t kHostSlotRva = 0xC60F70;          // sub_140C60F70() -> the host's player slot
    constexpr std::uintptr_t kSessionLayerPtrRva = 0x17811F0;  // global that sub_140C60F70 reads first
    using HostSlotFn = std::int32_t(__fastcall*)();
    bool g_BotOwner = true;

    void AddSurvivorBot(std::uintptr_t slots, std::uint32_t index, int number)
    {
        const std::uintptr_t slot = slots + index * kSlotStride;

        auto makeBotSlot = reinterpret_cast<MakeBotSlotFn>(g_Base + kMakeBotSlotRva);
        makeBotSlot(slots, index, 0 /* avatar */, 100);

        At<std::uint32_t>(slot + kSlotRole) = 0;
        At<std::uint32_t>(slot + kSlotControl) = 1;
        At<std::uint32_t>(slot + kSlotAiMode) = kAiModeOrientation;
        At<std::uint8_t>(slot + kSlotSkipAi) = 0;

        const std::int32_t ownerBefore = At<std::int32_t>(slot + kSlotOwner);
        if (g_BotOwner)
        {
            // same value the game gives its own Raider bot; without the session layer (never seen) use the local slot
            const std::int32_t owner = At<std::uintptr_t>(g_Base + kSessionLayerPtrRva) != 0
                ? reinterpret_cast<HostSlotFn>(g_Base + kHostSlotRva)()
                : At<std::int32_t>(slots + kLocalPlayerSlot);
            At<std::int32_t>(slot + kSlotOwner) = owner;
        }
        Logger::Info("PracticeBots: survivor bot slot " + std::to_string(index) + " owner (+0xD00) " +
                     std::to_string(ownerBefore) + " -> " + std::to_string(At<std::int32_t>(slot + kSlotOwner)) +
                     " (local player slot " + std::to_string(At<std::int32_t>(slots + kLocalPlayerSlot)) + ")");

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

    // Writes `size` bytes at g_Base + rva, only when the bytes there are exactly `expected`.
    bool PatchBytes(std::uintptr_t rva, const std::uint8_t* expected, const std::uint8_t* replacement, std::size_t size, const char* name)
    {
        auto* code = reinterpret_cast<std::uint8_t*>(g_Base + rva);
        if (std::memcmp(code, expected, size) != 0)
        {
            Logger::Error(std::string("PracticeBots: ") + name + " bytes differ from the expected ones - not patched");
            return false;
        }
        DWORD old = 0;
        if (!VirtualProtect(code, size, PAGE_EXECUTE_READWRITE, &old))
        {
            Logger::Error(std::string("PracticeBots: VirtualProtect failed - ") + name + " not patched");
            return false;
        }
        std::memcpy(code, replacement, size);
        VirtualProtect(code, size, old, &old);
        FlushInstructionCache(GetCurrentProcess(), code, size);
        return true;
    }

    // Raider bot starts as Larva Cell (raider_bot_larva, on by default with casual_bots=1; 2026-11-11).
    // The AI Raider spawn path sub_1408B71A0 has, at 0x1408B7592:
    //     if (sub_140206F30(raider))          // Raider (+0x204 == 1), Raider id +0x20C == 7 (Cell), level 1
    //         sub_1408B4AB0(raider, 2, 0);    // Possible_SetRaiderLevel(character, level, force) -> level 2
    // so an AI-controlled Cell skips Larva. A player Cell does not go through this and starts at level 1.
    // Test 1 (2026-11-12): did not help - the bot's +0x20C is 0, so this check never applies to it. Removed.
    // The real spot (level trace 2026-11-12, call chain 0x14066250E <- 0x140662A26 <- 0x140667FD4 <- 0x14066739C):
    // the character spawn routine sub_1406622F0 does, for a Raider whose character id (+0x200) is in the table at
    // 0x1411A8540 (+0x1FC == 1 or 3, sub_140207320) and whose brain is not type 3 (sub_140202AF0, i.e. not a human):
    //     if (raider->+0x204 == 1) {                  // cmp [rcx+204h],1 ; jne 0x14066253A  at 0x1406624E0
    //         sub_140213C60(raider, 2);               // pending level (+0x1224) = 2
    //         sub_1401FE680(raider, 2, 4, ...);       // -> sub_140223640 Possible_ApplyRaiderLevel
    //         sub_140120520(raider +0x598, 2, 1, 0); }
    // Patch 1: that jne becomes jmp, so the AI Raider keeps the level 1 it was spawned with.
    // Patch 2: the practice patrol update (SimpleAIActionPracticePatrol vfunc_4, 0x1402C9110) does nothing while
    // the Raider's level is <= 1 (cmp eax,1 ; jle at 0x1402C9155) - the reason the developers skipped Larva.
    // The jle is replaced with NOPs so Larva Cell patrols (and our absorb walk, which drives the patrol, works).
    constexpr std::uintptr_t kAiRaiderLevel2Rva = 0x6624E0;
    constexpr std::uintptr_t kPatrolLevelGateRva = 0x2C9155;
    void PatchLarvaStart()
    {
        static const std::uint8_t skipExpected[2] = { 0x75, 0x58 };       // jne short 0x14066253A
        static const std::uint8_t skipReplace[2] = { 0xEB, 0x58 };        // jmp short 0x14066253A
        static const std::uint8_t gateExpected[6] = { 0x0F, 0x8E, 0x3A, 0x02, 0x00, 0x00 };   // jle 0x1402C9395
        static const std::uint8_t gateReplace[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        const bool skip = PatchBytes(kAiRaiderLevel2Rva, skipExpected, skipReplace, 2, "AI Raider level-2 start (0x1406624E0)");
        const bool gate = PatchBytes(kPatrolLevelGateRva, gateExpected, gateReplace, 6, "patrol level gate (0x1402C9155)");
        if (skip && gate)
            Logger::Info("PracticeBots: the Raider bot starts as Larva Cell (level 1) and patrols at level 1 (raider_bot_larva)");
    }

    // Level trace (raider_level_trace=1, 2026-11-12): test 1 of raider_bot_larva still showed level 2 from the bot's
    // first frame, so 0x1408B7599 is not the path that sets it. Logs every call to the two level setters with the
    // level asked for and the code address that called it (first 80 calls).
    //   sub_1408B4AB0(character, level, force)  Possible_SetRaiderLevel
    //   sub_1400CDFB0(form, level, kind, id)    Possible_SetFormLevel (form = character +0x2E0)
    constexpr std::uintptr_t kSetRaiderLevelRva = 0x8B4AB0;
    constexpr std::uintptr_t kSetFormLevelRva = 0xCDFB0;
    using SetRaiderLevelFn = void(__fastcall*)(std::uintptr_t, std::uint64_t, std::uint8_t);
    using SetFormLevelFn = void(__fastcall*)(std::uintptr_t, std::uint32_t, std::uint8_t, std::uint32_t);
    SetRaiderLevelFn g_OriginalSetRaiderLevel = nullptr;
    SetFormLevelFn g_OriginalSetFormLevel = nullptr;
    void* g_SetRaiderLevelTarget = nullptr;
    void* g_SetFormLevelTarget = nullptr;
    volatile LONG g_LevelTraceCount = 0;

    void __fastcall SetRaiderLevelDetour(std::uintptr_t character, std::uint64_t level, std::uint8_t force)
    {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_Base;
        if (InterlockedIncrement(&g_LevelTraceCount) <= 80 && character != 0)
        {
            const std::uintptr_t form = At<std::uintptr_t>(character + 0x2E0);
            std::ostringstream line;
            line << "LevelTrace: SetRaiderLevel(level=" << static_cast<std::uint32_t>(level) << ", force=" << int(force)
                 << ") from 0x" << std::hex << (0x140000000ull + caller) << std::dec
                 << " role=" << At<std::uint32_t>(character + 0x204) << " raiderId=" << At<std::uint32_t>(character + 0x20C)
                 << " slot=" << At<std::uint32_t>(character + kCharSlot)
                 << " +1224=" << At<std::uint32_t>(character + 0x1224) << " +122C=" << At<std::uint32_t>(character + 0x122C)
                 << " formLevel=" << (form != 0 ? At<std::uint32_t>(form + 0xE0) : 0);
            Logger::Info(line.str());
            Logger::Flush();
        }
        g_OriginalSetRaiderLevel(character, level, force);
    }

    // Trace 1 (2026-11-12 00:24): at the bot's spawn SetFormLevel(level=2, kind=4, id=7) came from 0x1402239CA in
    // sub_140223640 (Possible_ApplyRaiderLevel(character, level, kind, id)); SetRaiderLevel was never called. ~40
    // places reach sub_140223640 through jmp wrappers, so this logs the whole call chain (exe addresses only).
    constexpr std::uintptr_t kApplyRaiderLevelRva = 0x223640;
    using ApplyRaiderLevelFn = void(__fastcall*)(std::uintptr_t, std::uint32_t, std::uint8_t, std::uint32_t);
    ApplyRaiderLevelFn g_OriginalApplyRaiderLevel = nullptr;
    void* g_ApplyRaiderLevelTarget = nullptr;

    std::string CallChain()
    {
        void* frames[12] = {};
        const USHORT count = RtlCaptureStackBackTrace(1, 12, frames, nullptr);
        std::ostringstream chain;
        for (USHORT i = 0; i < count; ++i)
        {
            const auto address = reinterpret_cast<std::uintptr_t>(frames[i]);
            if (address >= g_Base && address < g_Base + 0x3900000)
                chain << " 0x" << std::hex << (0x140000000ull + (address - g_Base)) << std::dec;
        }
        return chain.str();
    }

    void __fastcall ApplyRaiderLevelDetour(std::uintptr_t character, std::uint32_t level, std::uint8_t kind, std::uint32_t id)
    {
        if (InterlockedIncrement(&g_LevelTraceCount) <= 80 && character != 0)
        {
            const std::uintptr_t form = At<std::uintptr_t>(character + 0x2E0);
            std::ostringstream line;
            line << "LevelTrace: ApplyRaiderLevel(level=" << level << ", kind=" << int(kind) << ", id=" << static_cast<std::int32_t>(id)
                 << ") role=" << At<std::uint32_t>(character + 0x204) << " raiderId=" << At<std::uint32_t>(character + 0x20C)
                 << " slot=" << At<std::uint32_t>(character + kCharSlot)
                 << " formLevel=" << (form != 0 ? At<std::uint32_t>(form + 0xE0) : 0)
                 << " +1224=" << At<std::uint32_t>(character + 0x1224) << " +122C=" << At<std::uint32_t>(character + 0x122C)
                 << " chain:" << CallChain();
            Logger::Info(line.str());
            Logger::Flush();
        }
        g_OriginalApplyRaiderLevel(character, level, kind, id);
    }

    void __fastcall SetFormLevelDetour(std::uintptr_t form, std::uint32_t level, std::uint8_t kind, std::uint32_t id)
    {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_Base;
        if (InterlockedIncrement(&g_LevelTraceCount) <= 80 && form != 0)
        {
            std::ostringstream line;
            line << "LevelTrace: SetFormLevel(level=" << level << ", kind=" << int(kind) << ", id=" << static_cast<std::int32_t>(id)
                 << ") from 0x" << std::hex << (0x140000000ull + caller) << std::dec
                 << " was=" << At<std::uint32_t>(form + 0xE0) << " start(+0xDC)=" << At<std::uint32_t>(form + 0xDC);
            Logger::Info(line.str());
            Logger::Flush();
        }
        g_OriginalSetFormLevel(form, level, kind, id);
    }

    // No teleporting (raider_bot_no_warp, on by default with casual_bots=1; 2026-11-12, user: "cell seems to
    // randomly teleport from a blue beam" / "please remove the random teleportations"). The practice AI warps:
    //   SimpleAIActionPracticeWarp vfunc_4 (0x1402C9560): effect 0xD9819B4A at Cell (call 0x1402C95D5), warp to
    //     action +0x1790 (sub_140213410, call 0x1402C95F2), effect again there (call 0x1402C963B) = the blue beam.
    //   SimpleAIActionPracticeInteract vfunc_4: warps onto the target's interact position (call 0x1402C90CB);
    //     our absorb logic walks there instead.
    // The four calls are replaced with 5-byte NOPs (their results are not used). The spawn placement in the
    // practice AI update (0x1402C055A) is kept.
    void PatchNoWarp()
    {
        static const std::uint8_t nop5[5] = { 0x0F, 0x1F, 0x44, 0x00, 0x00 };
        static const std::uint8_t effect1[5] = { 0xE8, 0x06, 0xB5, 0x9B, 0x00 };
        static const std::uint8_t warp[5] = { 0xE8, 0x19, 0x9E, 0xF4, 0xFF };
        static const std::uint8_t effect2[5] = { 0xE8, 0xA0, 0xB4, 0x9B, 0x00 };
        static const std::uint8_t interactWarp[5] = { 0xE8, 0x40, 0xA3, 0xF4, 0xFF };
        int done = 0;
        done += PatchBytes(0x2C95D5, effect1, nop5, 5, "warp effect 1 (0x1402C95D5)");
        done += PatchBytes(0x2C95F2, warp, nop5, 5, "practice warp (0x1402C95F2)");
        done += PatchBytes(0x2C963B, effect2, nop5, 5, "warp effect 2 (0x1402C963B)");
        done += PatchBytes(0x2C90CB, interactWarp, nop5, 5, "interact warp (0x1402C90CB)");
        Logger::Info("PracticeBots: practice AI teleports removed (" + std::to_string(done) + "/4 patches, raider_bot_no_warp)");
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
    g_BotOwner = GetPrivateProfileIntA("steam", "survivor_bot_owner", 1, SteamConfig::GetPath().c_str()) != 0;
    Logger::Info(std::string("PracticeBots: survivor bots get the host as slot owner like the game's own Raider bot (survivor_bot_owner=") +
                 (g_BotOwner ? "1)" : "0 - OFF)"));
    g_SurvivorRevive = GetPrivateProfileIntA("steam", "survivor_bot_revive", 1, SteamConfig::GetPath().c_str()) != 0;
    if (g_SurvivorRevive)
        Logger::Info("PracticeBots: survivor bots revive downed teammates (survivor_bot_revive=1)");
    g_BotAttack = GetPrivateProfileIntA("steam", "survivor_bot_attack", 0, SteamConfig::GetPath().c_str()) != 0;
    if (g_BotAttack)
        Logger::Info("PracticeBots: bot attack test on - survivor bots lock on to the Raider within 30 m and press "
                     "attack (survivor_bot_attack=1)");
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
        if (GetPrivateProfileIntA("steam", "raider_bot_larva", 1, SteamConfig::GetPath().c_str()) != 0)
            PatchLarvaStart();
        if (GetPrivateProfileIntA("steam", "raider_bot_no_warp", 1, SteamConfig::GetPath().c_str()) != 0)
            PatchNoWarp();
        g_RaiderDestroy = GetPrivateProfileIntA("steam", "raider_bot_destroy", 1, SteamConfig::GetPath().c_str()) != 0;
        g_AimFlip = GetPrivateProfileIntA("steam", "raider_bot_aim_flip", 0, SteamConfig::GetPath().c_str()) != 0;
        g_FaceTarget = GetPrivateProfileIntA("steam", "raider_bot_face_target", 0, SteamConfig::GetPath().c_str()) != 0;
        // (random numbers: BotRandom, seeded on first use)
        if (g_RaiderDestroy)
            Logger::Info(std::string("PracticeBots: the Raider bot destroys a random area after each evolution (raider_bot_destroy") +
                         (g_AimFlip ? ", aim flipped)" : ")"));
        g_EnergyTest = GetPrivateProfileIntA("steam", "raider_bot_energy_test", 0, SteamConfig::GetPath().c_str()) != 0;
        char rate[32] = {};
        GetPrivateProfileStringA("steam", "raider_bot_energy_rate", "1.6", rate, sizeof(rate), SteamConfig::GetPath().c_str());
        g_EnergyRate = static_cast<float>(std::atof(rate));
        Logger::Info("PracticeBots: Raider bot gains " + std::string(rate) + " evolution energy per second (raider_bot_energy_rate)");
        if (g_EnergyTest)
            Logger::Info("PracticeBots: Raider bot energy test on - a stuck evolution bar is filled after 10 s (raider_bot_energy_test)");
        if (GetPrivateProfileIntA("steam", "raider_level_trace", 0, SteamConfig::GetPath().c_str()) != 0)
        {
            Hook(kSetRaiderLevelRva, reinterpret_cast<void*>(&SetRaiderLevelDetour),
                reinterpret_cast<void**>(&g_OriginalSetRaiderLevel), &g_SetRaiderLevelTarget, "sub_1408B4AB0 (level trace)");
            Hook(kSetFormLevelRva, reinterpret_cast<void*>(&SetFormLevelDetour),
                reinterpret_cast<void**>(&g_OriginalSetFormLevel), &g_SetFormLevelTarget, "sub_1400CDFB0 (level trace)");
            Hook(kApplyRaiderLevelRva, reinterpret_cast<void*>(&ApplyRaiderLevelDetour),
                reinterpret_cast<void**>(&g_OriginalApplyRaiderLevel), &g_ApplyRaiderLevelTarget, "sub_140223640 (level trace)");
        }
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
    g_InputTrace = GetPrivateProfileIntA("steam", "input_trace", 0, SteamConfig::GetPath().c_str()) != 0;
    if (g_InputTrace)
        Logger::Info("PracticeBots: input trace on - the player's pressed input bits are logged once a second (input_trace)");
    g_CounterTestKey = GetPrivateProfileIntA("steam", "counter_test_key", 0, SteamConfig::GetPath().c_str());
    if (g_CounterTestKey < 1 || g_CounterTestKey > 254)
        g_CounterTestKey = 0;
    if (g_CounterTestKey != 0)
        Logger::Info("PracticeBots: counter-attack test key on - virtual key " + std::to_string(g_CounterTestKey) +
            " makes the player's Raider play its counter-attack (counter_test_key)");
    g_LoadoutTestKey = GetPrivateProfileIntA("steam", "loadout_test_key", 0, SteamConfig::GetPath().c_str());
    if (g_LoadoutTestKey < 1 || g_LoadoutTestKey > 254)
        g_LoadoutTestKey = 0;
    if (g_LoadoutTestKey != 0)
    {
        g_LoadoutTestRaider = static_cast<std::uint32_t>(
            GetPrivateProfileIntA("steam", "loadout_test_raider", 400, SteamConfig::GetPath().c_str()));
        const int form = GetPrivateProfileIntA("steam", "loadout_test_form", 3, SteamConfig::GetPath().c_str());
        g_LoadoutTestForm = form >= 0 && form <= 4 ? static_cast<std::uint32_t>(form) : 3u;
        g_LoadoutTestSkill = static_cast<std::uint32_t>(
            GetPrivateProfileIntA("steam", "loadout_test_skill", 80330, SteamConfig::GetPath().c_str()));
        const int record = GetPrivateProfileIntA("steam", "loadout_test_record", 0, SteamConfig::GetPath().c_str());
        g_LoadoutTestRecord = record >= 0 && record <= 7 ? static_cast<std::uint32_t>(record) : 0u;
        if (CreateThread(nullptr, 0, &LoadoutTestThread, nullptr, 0, nullptr) != nullptr)
            Logger::Info("PracticeBots: loadout test key on - virtual key " + std::to_string(g_LoadoutTestKey) +
                " puts skill " + std::to_string(g_LoadoutTestSkill) + " into record " + std::to_string(g_LoadoutTestRecord + 1) +
                " of Raider " + std::to_string(g_LoadoutTestRaider) + " form " + std::to_string(g_LoadoutTestForm) +
                " in the saved loadout and back (loadout_test_key)");
        else
            Logger::Error("PracticeBots: loadout test key - could not start its thread");
    }
    if (g_Trace)
        Logger::Info("PracticeBots: bot status trace on (every 2 s per bot)");

    // Always on with bots: the update hook also takes the bots out of the tutorial mode.
    const bool follow = Hook(kAiUpdateRva, reinterpret_cast<void*>(&AiUpdateDetour),
            reinterpret_cast<void**>(&g_OriginalAiUpdate), &g_AiUpdateTarget, "0x1402BE6E0 (tutorial AI update, follower)");

    g_ReviveTrace = GetPrivateProfileIntA("steam", "revive_trace", 0, SteamConfig::GetPath().c_str()) != 0;
    if (g_ReviveTrace)
    {
        Logger::Info("PracticeBots: revive trace on - logs each step of the revive chain (revive_trace=1)");
        Logger::Info("PracticeBots: revive comparison on (2026-11-16) - 'ReviveCompare' lines: locks at game+0x67E8, "
                     "each slot's reviving (+0x1E60) / revivedBy (+0x1E80) / offered kind");
        Hook(kInteractActionRva, reinterpret_cast<void*>(&InteractActionDetour),
             reinterpret_cast<void**>(&g_OriginalInteractAction), &g_InteractActionTarget, "sub_140174A90 (StateInteract action)");
        Hook(kStartReviveRequestRva, reinterpret_cast<void*>(&StartReviveRequestDetour),
             reinterpret_cast<void**>(&g_OriginalStartReviveRequest), &g_StartReviveRequestTarget, "sub_1400B2450 (revive request)");
        Hook(kInteractRequestRva, reinterpret_cast<void*>(&InteractRequestDetour),
             reinterpret_cast<void**>(&g_OriginalInteractRequest), &g_InteractRequestTarget, "sub_140702B80 (interact manager request)");
        Hook(kInteractOrderRva, reinterpret_cast<void*>(&InteractOrderDetour),
             reinterpret_cast<void**>(&g_OriginalInteractOrder), &g_InteractOrderTarget, "sub_140701800 (interact manager order)");
    }

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
    for (void** target : { &g_InteractActionTarget, &g_StartReviveRequestTarget, &g_InteractRequestTarget, &g_InteractOrderTarget })
    {
        if (*target)
            HookManager::Disable(*target);
        *target = nullptr;
    }
    if (g_TutorialGetTarget)
        HookManager::Disable(g_TutorialGetTarget);
    if (g_IsMatchHostTarget)
        HookManager::Disable(g_IsMatchHostTarget);
    if (g_FindRaiderEntryTarget)
        HookManager::Disable(g_FindRaiderEntryTarget);
    if (g_SetRaiderLevelTarget)
        HookManager::Disable(g_SetRaiderLevelTarget);
    if (g_SetFormLevelTarget)
        HookManager::Disable(g_SetFormLevelTarget);
    if (g_ApplyRaiderLevelTarget)
        HookManager::Disable(g_ApplyRaiderLevelTarget);
    g_ApplyRaiderLevelTarget = nullptr;
    g_SetRaiderLevelTarget = nullptr;
    g_SetFormLevelTarget = nullptr;
    g_IsMatchHostTarget = nullptr;
    g_FindRaiderEntryTarget = nullptr;
    g_LobbyStartTarget = nullptr;
    g_TutorialGetTarget = nullptr;
}
