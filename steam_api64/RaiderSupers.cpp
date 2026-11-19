#include "StdInc.h"

#include "RaiderSupers.h"

#include "HookManager.h"
#include "Logger.h"
#include "SteamConfig.h"

#include <intrin.h>

#pragma comment(lib, "gdi32.lib")
#pragma intrinsic(_ReturnAddress)

// Raider super selection (experimental, 2026-11-16).
//
// Project lead: "a mix between number 1 [swap a super], and building a menu that allows raiders to select their
// supers and swap between them before a match begins as well as per level" - "we can start with cell".
//
// What the game does (read from the exe, docs\prompt_log.md "raider supers"):
// - The two supers of a Raider level are cells 18 / 19 of its RivalCharaInfoList row. Everything that needs them -
//   the match (cooldowns, firing), the HUD and the Raider Characters screens, 54 functions - asks ONE function,
//   sub_140692840(map, out, Raider id, level, variant), which fills a row struct: +0x00 Raider id, +0x04 level,
//   +0x08 variant, +0x40 / +0x44 the two super ids.
// - The player's saved loadout (GameData RivalEquipDataList, one 0x6F0-byte entry per owned Raider) only keeps the
//   ORDER: per level two pairs {super id, table slot} at entry + 0xC + level * 0xB0 + 0xA0; the pair's place = the
//   button, its second value = which of the level's two supers.
// - Test 87 (data only) showed a Raider can run a super that was made for survivors (Cell + Super Spirit Bomb).
//
// What this module does:
// - a detour on sub_140692840 puts the player's chosen super ids into the returned row (the table itself is not
//   touched), so the screens, the HUD and the match all follow;
// - the ids inside the saved loadout's pairs are kept in step (data writes only, inside a fault guard), so the
//   game's own "swap the two supers" screen keeps working;
// - a small window (hotkey, default F6, only while the game is in front) offers per level and per slot the supers
//   that are known to load for that Raider. Choosing the super the other slot already has swaps the two.
//   Choices are stored in steam_config.ini as raider_super_<Raider>_<level>_<slot 1 or 2>=<super id> (0 = stock).
// - 2026-11-17, first step into the game's own screen: on Raider Characters -> RAIDER SUPER ATTACKS a key
//   (raider_supers_change_key, default F) steps the super under the cursor to the next one on the list; the page is
//   redrawn with the game's own calls. See "the game's own RAIDER SUPER ATTACKS page" below.
//
// Limits: the choice applies to EVERY Raider of that kind in the match (also a bot); other players' games do not
// know about it - local practice matches only. Only supers whose effect packs and sound banks are on that Raider's
// load lists can be offered (data side: modding\raider_super.py).

namespace
{
    constexpr std::uintptr_t kRowGetterRva = 0x692840;         // sub_140692840
    constexpr std::uintptr_t kPlayDataGlobalRva = 0x1782510;   // pointer; +0x18 -> object; its +0x70 -> RivalEquipDataList
    constexpr std::uintptr_t kEquipEntrySize = 0x6F0;
    constexpr std::uintptr_t kEquipRaiderId = 0x8;
    constexpr std::uintptr_t kEquipForms = 0xC;
    constexpr std::uintptr_t kEquipFormSize = 0xB0;
    constexpr std::uintptr_t kFormSupers = 0xA0;               // 2 x {u32 super id, u32 table slot}
    constexpr std::uintptr_t kRowRaider = 0x00;
    constexpr std::uintptr_t kRowLevel = 0x04;
    constexpr std::uintptr_t kRowVariant = 0x08;
    constexpr std::uintptr_t kRowSupers = 0x40;

    struct Candidate
    {
        std::uint32_t id;
        const char* name;
    };

    struct Level
    {
        std::uint32_t raider;
        std::uint32_t level;
        const char* label;
    };

    // Names = the game's own English texts (ui_text_en, the supers' name keys).
    const Candidate kCellSupers[] = {
        {4100100, "Full Power Energy Wave  (stage 2)"},
        {4100101, "Special Beam Cannon  (stage 2)"},
        {4100200, "Gravity Impact  (stage 3)"},
        {4100201, "Full Power Energy Blast Volley  (stage 3)"},
        {4100300, "Perfect Kamehameha  (stage 4)"},
        {4100301, "All Clear  (stage 4)"},
        {4100110, "Super Spirit Bomb  (from the survivors)"},
        // Data test 90: Broly's Planet Crusher build (go up, aim at the ground, throw) as Cell's own super; lands as
        // Super Broly's blast + Z Broly's pillar. First step of the lead's "Raider version" of Super Spirit Bomb.
        {4100120, "Super Spirit Bomb  (Raider version, aimed)"},
    };
    constexpr int kCandidateCount = static_cast<int>(sizeof(kCellSupers) / sizeof(kCellSupers[0]));

    const Level kLevels[] = {
        {100, 2, "Stage 2  -  Cell (1st Form)"},
        {100, 3, "Stage 3  -  Cell (2nd Form)"},
        {100, 4, "Stage 4  -  Perfect Cell"},
    };
    constexpr int kLevelCount = static_cast<int>(sizeof(kLevels) / sizeof(kLevels[0]));

    // Face-fitted copies (2026-11-16, data test 89: modding\stage_fit.py + raider_super.give_fitted_copy).
    // Project lead, test 88: "perfect cells face looks off when performing gravity impact". A super's body animation
    // places the face bones where the face of the stage it was made for has them; on another stage that squashes
    // the face. The data therefore carries, for every (super, other stage) pair, a copy of the super whose animation
    // is re-fitted to that stage's face: id 4100000 + (stage - 1) * 100 + 10 + the super's place in kCellSupers.
    // The menu keeps showing and storing the super itself; only the id handed to the game is the stage's copy.
    // All Clear has no face animation and needs no copy. Used only with raider_supers_fitted=1 (= the installed
    // data has the copies; without them the game would be given ids it does not know).
    struct Fitted
    {
        std::uint32_t super;
        std::uint32_t level;
        std::uint32_t fitted;
    };

    const Fitted kFitted[] = {
        {4100100, 3, 4100210},
        {4100100, 4, 4100310},
        {4100101, 3, 4100211},
        {4100101, 4, 4100311},
        {4100200, 2, 4100112},
        {4100200, 4, 4100312},
        {4100201, 2, 4100113},
        {4100201, 4, 4100313},
        {4100300, 2, 4100114},
        {4100300, 3, 4100214},
        {4100110, 2, 4100116},
        {4100110, 3, 4100216},
        {4100110, 4, 4100316},
        {4100120, 2, 4100117},                                  // test 90 (Broly's animation, face fitted per stage)
        {4100120, 3, 4100217},
        {4100120, 4, 4100317},
    };

    std::uintptr_t g_Base = 0;
    bool g_UseFitted = false;
    int g_MenuKey = VK_F6;
    std::atomic<std::uint32_t> g_Choice[kLevelCount][2];       // 0 = the table's own super
    std::atomic<std::uint32_t> g_Stock[kLevelCount][2];        // what the table holds (seen by the detour)
    std::atomic<std::uint32_t> g_Served[kLevelCount];          // times a row was handed out with a change (for the log)

    template <typename T>
    T& At(std::uintptr_t address)
    {
        return *reinterpret_cast<T*>(address);
    }

    const char* NameOf(std::uint32_t id)
    {
        for (const Candidate& c : kCellSupers)
            if (c.id == id)
                return c.name;
        return nullptr;
    }

    bool Known(std::uint32_t id)
    {
        return NameOf(id) != nullptr;
    }

    std::uint32_t Effective(int level, int slot)
    {
        const std::uint32_t choice = g_Choice[level][slot].load();
        return choice != 0 ? choice : g_Stock[level][slot].load();
    }

    // The id the game gets for a super at a Raider level: the level's face-fitted copy if the data has one.
    std::uint32_t ForLevel(std::uint32_t id, std::uint32_t level)
    {
        if (g_UseFitted)
            for (const Fitted& f : kFitted)
                if (f.super == id && f.level == level)
                    return f.fitted;
        return id;
    }

    // The super a face-fitted copy belongs to (or the id itself).
    std::uint32_t BaseOf(std::uint32_t id)
    {
        for (const Fitted& f : kFitted)
            if (f.fitted == id)
                return f.super;
        return id;
    }

    std::string KeyName(int level, int slot)
    {
        return "raider_super_" + std::to_string(kLevels[level].raider) + "_" + std::to_string(kLevels[level].level) +
            "_" + std::to_string(slot + 1);
    }

    // ---------------------------------------------------------------------------------------------- the detour
    using RowFn = bool(__fastcall*)(void* map, std::uint8_t* out, std::uint32_t raider, std::uint32_t level,
        std::uint32_t variant);
    RowFn g_OriginalRow = nullptr;

    bool __fastcall RowDetour(void* map, std::uint8_t* out, std::uint32_t raider, std::uint32_t level,
        std::uint32_t variant)
    {
        const bool ok = g_OriginalRow(map, out, raider, level, variant);
        if (out == nullptr)
            return ok;
        const std::uint32_t rowRaider = *reinterpret_cast<std::uint32_t*>(out + kRowRaider);
        const std::uint32_t rowLevel = *reinterpret_cast<std::uint32_t*>(out + kRowLevel);
        if (*reinterpret_cast<std::uint32_t*>(out + kRowVariant) != 0)
            return ok;
        for (int i = 0; i < kLevelCount; ++i)
        {
            if (kLevels[i].raider != rowRaider || kLevels[i].level != rowLevel)
                continue;
            std::uint32_t* supers = reinterpret_cast<std::uint32_t*>(out + kRowSupers);
            bool changed = false;
            for (int s = 0; s < 2; ++s)
            {
                if (!Known(supers[s]))
                    return ok;                                  // not the row we think it is: leave it alone
                g_Stock[i][s].store(supers[s]);
            }
            for (int s = 0; s < 2; ++s)
            {
                const std::uint32_t choice = g_Choice[i][s].load();
                const std::uint32_t want = choice != 0 ? ForLevel(choice, rowLevel) : supers[s];
                if (want != supers[s])
                {
                    supers[s] = want;
                    changed = true;
                }
            }
            if (changed && g_Served[i].fetch_add(1) == 0)
                Logger::Info(std::string("RaiderSupers: ") + kLevels[i].label + " now handed out with supers " +
                    std::to_string(supers[0]) + ", " + std::to_string(supers[1]) + " (table: " +
                    std::to_string(g_Stock[i][0].load()) + ", " + std::to_string(g_Stock[i][1].load()) + ")");
            break;
        }
        return ok;
    }

    // ------------------------------------------------------------------------ the aim marker ("danger zone")
    // Project lead, data test 90 (Cell's aimed Super Spirit Bomb = Broly's Planet Crusher build): "its functional,
    // however the danger zone is a straight forward line from perfect kamehameha".
    // Read from the exe (2026-11-16):
    // - When an aimed super starts aiming, its state calls sub_1400d3230(Raider unit). That fills a 24-byte marker
    //   setting at unit + 0x2690 by the Raider's FAMILY (unit + 0x20C: Cell 0, Frieza 1, Buu 2, Vegeta 3, Ginyu 4,
    //   Broly 5, Zamasu 6, Baby 7, Gamma 8, Golden Frieza 9, Cell + Cell Jr 10, Buu (Gohan) 11, Z Broly 12) and level:
    //     +0x00 u8   1 = strip (line) marker, 0 = circle marker
    //     +0x04 u32  strip look while aiming      +0x08 u32  strip look once fired
    //     +0x0C u32  circle look while aiming     +0x10 u32  circle look once thrown
    //     +0x14 u32  name hash of the aim camera's row in data/Common/CameraCsv/MapBreakCameraParam.mbcp
    //   Cell (every level): strip 0 / 1, camera 0x40418AC9. Broly level 2 (Planet Crusher): circle 6 / 7,
    //   camera 0xB4707B6F (also the game's fallback).
    // - The Raider's update (sub_1400d8330) draws the strip (sub_1400d6040) or the circle (sub_1400d2e60) from that
    //   setting alone; both use common tables, so nothing of Broly's has to be loaded.
    // - Five skill-state kinds call it (XG::Game::StateSkill setup sub_1401a6760, kinds 6 / 43 / 49 / 67 / 80).
    //   Kinds 6, 67 and 80 jump into it from a per-frame callback, so the call returns into the callback invoker,
    //   game + 0x10E4F1. Kinds 43 and 49 call it once from their own code (returns game + 0x1A0BC4 / 0x1C3A5B).
    //   SEEN in the game log, 2026-11-16 22:59: the aimed Spirit Bomb (Planet Crusher's build) calls every frame
    //   from game + 0x10E4F1. Possible_: Perfect Kamehameha is kind 49 (aims in the action's last part and sets
    //   steering limits) - not confirmed; the log line below names each new caller, so a test shows it.
    // What this does: right after the game filled the setting for a Cell whose level carries the aimed Spirit Bomb,
    // Broly's level 2 setting is written over it. If that level also carries Perfect Kamehameha (which needs Cell's
    // own strip), only a call that comes through the callback invoker gets the circle.
    //
    // Aiming with the crosshair (project lead on the first marker build: "the marker is fixed, however the marker
    // should be aimed with the crosshair"; test videos\super spirit bomb test 2.mp4: the circle sat low and a little
    // to the right of the crosshair, close to Cell). Read from the exe: the Raider's update asks
    // sub_140206f10(unit) = "is this a Cell-family Raider" (unit + 0x204 == 1 and family 0 or 10) in three places,
    // all made for Perfect Kamehameha: (1) call at game + 0xD868D: the aim direction is tilted away from the camera
    // direction (10 degrees of pitch, 7.5 of yaw, fading out when looking steeply down), (2) game + 0xD8940: Cell's body is
    // turned 25 degrees off the aim direction, (3) game + 0xD94CE: a ground check for level 4. Broly gets none of
    // these, his aim point is the camera's centre ray. So while a Cell aims with the circle setting written above,
    // those three calls (and only those, told apart by their return addresses) are answered "no".
    // (A first attempt changed the family number inside the level row instead; that told the whole game that Cell
    //  was a Broly and was never used in a match - patches\RaiderSupers.family-override-attempt-2026-11-16.cpp.)
    constexpr std::uintptr_t kMarkerSetupRva = 0xD3230;         // sub_1400d3230(BattleRival*)
    constexpr std::uintptr_t kLevelGetterRva = 0xCD9E0;         // sub_1400cd9e0(object at unit + 0x2E0) -> level
    constexpr std::uintptr_t kStateInvokerReturnRva = 0x10E4F1; // where a per-frame state callback returns to
    constexpr std::uintptr_t kIsCellFamilyRva = 0x206F10;       // sub_140206f10(unit) -> 1 for a Cell-family Raider
    constexpr std::uintptr_t kAimTiltReturnRva = 0xD8692;       // the three calls inside sub_1400d8330 (see above)
    constexpr std::uintptr_t kAimYawReturnRva = 0xD8945;
    constexpr std::uintptr_t kAimGroundReturnRva = 0xD94D3;
    constexpr std::uintptr_t kUnitFamily = 0x20C;
    constexpr std::uintptr_t kUnitLevelHolder = 0x2E0;
    constexpr std::uintptr_t kUnitAiming = 0x2689;              // u8: 1 while an aimed super is aiming
    constexpr std::uintptr_t kUnitMarker = 0x2690;
    constexpr std::uint32_t kFamilyCell = 0;
    constexpr std::uint32_t kAimedBomb = 4100120;
    constexpr std::uint32_t kPerfectKamehameha = 4100300;
    constexpr std::uint32_t kCircleAiming = 6;                  // Broly level 2
    constexpr std::uint32_t kCircleThrown = 7;
    constexpr std::uint32_t kCrusherCamera = 0xB4707B6F;

    using MarkerFn = std::uint64_t(__fastcall*)(std::uint8_t* unit);
    using LevelFn = std::uint32_t(__fastcall*)(void* holder);
    MarkerFn g_OriginalMarker = nullptr;

    // Plain data and one call of the game's own level getter, so the fault guard can wrap it.
    // Returns 1 = circle written, 0 = left as the game set it, -1 = fault.
    int MarkerFixRaw(std::uint8_t* unit, std::uintptr_t fromRva, std::uint32_t* family, std::uint32_t* level)
    {
        __try
        {
            *family = *reinterpret_cast<std::uint32_t*>(unit + kUnitFamily);
            if (*family != kFamilyCell)
                return 0;
            void* holder = *reinterpret_cast<void**>(unit + kUnitLevelHolder);
            if (holder == nullptr)
                return 0;
            *level = reinterpret_cast<LevelFn>(g_Base + kLevelGetterRva)(holder);
            bool bomb = false, beam = false;
            for (int i = 0; i < kLevelCount; ++i)
            {
                if (kLevels[i].level != *level)
                    continue;
                for (int s = 0; s < 2; ++s)
                {
                    const std::uint32_t id = Effective(i, s);
                    bomb = bomb || id == kAimedBomb;
                    beam = beam || id == kPerfectKamehameha;
                }
            }
            if (!bomb || (beam && fromRva != kStateInvokerReturnRva))
                return 0;
            std::uint8_t* marker = unit + kUnitMarker;
            marker[0] = 0;                                      // circle, not strip
            marker[1] = 0;
            *reinterpret_cast<std::uint32_t*>(marker + 0x0C) = kCircleAiming;
            *reinterpret_cast<std::uint32_t*>(marker + 0x10) = kCircleThrown;
            *reinterpret_cast<std::uint32_t*>(marker + 0x14) = kCrusherCamera;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    std::uint64_t __fastcall MarkerDetour(std::uint8_t* unit)
    {
        const std::uintptr_t fromRva = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_Base;
        const std::uint64_t result = g_OriginalMarker(unit);
        if (unit == nullptr)
            return result;
        std::uint32_t family = 0xFFFFFFFF, level = 0;
        const int done = MarkerFixRaw(unit, fromRva, &family, &level);
        // Called every frame while aiming: log only when the caller, the level or the outcome changes.
        static std::atomic<std::uint64_t> lastKey{~0ull};
        static std::atomic<int> logged{0};
        const std::uint64_t key = (static_cast<std::uint64_t>(fromRva) << 16) ^ (static_cast<std::uint64_t>(family) << 8) ^
            (static_cast<std::uint64_t>(level) << 4) ^ static_cast<std::uint64_t>(done + 1);
        if (lastKey.exchange(key) != key && logged.fetch_add(1) < 60)
        {
            char from[32];
            sprintf_s(from, "game+0x%llX", static_cast<unsigned long long>(fromRva));
            Logger::Info("RaiderSupers: aim marker set up (family " + std::to_string(family) + ", level " +
                std::to_string(level) + ", called from " + from + ") -> " +
                (done == 1 ? "circle (Planet Crusher's) written for the aimed Spirit Bomb"
                    : done == 0 ? "left as the game set it" : "FAULT while reading the unit, left alone"));
        }
        return result;
    }

    using IsCellFamilyFn = std::uint8_t(__fastcall*)(std::uint8_t* unit);
    IsCellFamilyFn g_OriginalIsCellFamily = nullptr;
    std::atomic<std::uint32_t> g_AimAnswered{0};

    // Plain data, so the fault guard can wrap it: is this Cell aiming right now with the circle setting we wrote?
    bool CircleAimRaw(std::uint8_t* unit)
    {
        __try
        {
            const std::uint8_t* marker = unit + kUnitMarker;
            return *reinterpret_cast<std::uint32_t*>(unit + kUnitFamily) == kFamilyCell && unit[kUnitAiming] != 0 &&
                marker[0] == 0 && *reinterpret_cast<const std::uint32_t*>(marker + 0x0C) == kCircleAiming &&
                *reinterpret_cast<const std::uint32_t*>(marker + 0x14) == kCrusherCamera;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::uint8_t __fastcall IsCellFamilyDetour(std::uint8_t* unit)
    {
        const std::uintptr_t fromRva = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_Base;
        const std::uint8_t result = g_OriginalIsCellFamily(unit);
        if (result == 0 || unit == nullptr ||
            (fromRva != kAimTiltReturnRva && fromRva != kAimYawReturnRva && fromRva != kAimGroundReturnRva))
            return result;
        if (!CircleAimRaw(unit))
            return result;
        if (g_AimAnswered.fetch_add(1) == 0)
            Logger::Info("RaiderSupers: aimed Spirit Bomb - Cell's Perfect Kamehameha aim tilt is switched off while "
                "it aims (the circle follows the crosshair like Planet Crusher's)");
        return 0;
    }

    // ------------------------------------------------------------------------- the saved loadout's super pairs
    // Plain data only, so the fault guard can wrap it. Returns the number of ids rewritten, -1 on a fault.
    int SyncPairsRaw()
    {
        int written = 0;
        __try
        {
            const std::uintptr_t playData = At<std::uintptr_t>(g_Base + kPlayDataGlobalRva);
            const std::uintptr_t holder = playData != 0 ? At<std::uintptr_t>(playData + 0x18) : 0;
            const std::uintptr_t list = holder != 0 ? At<std::uintptr_t>(holder + 0x70) : 0;
            if (list == 0)
                return 0;
            for (std::uintptr_t e = At<std::uintptr_t>(list + 0x10), end = At<std::uintptr_t>(list + 0x18);
                 e != 0 && e < end; e += kEquipEntrySize)
            {
                for (int i = 0; i < kLevelCount; ++i)
                {
                    if (At<std::uint32_t>(e + kEquipRaiderId) != kLevels[i].raider)
                        continue;
                    const std::uintptr_t pairs = e + kEquipForms + kLevels[i].level * kEquipFormSize + kFormSupers;
                    for (int p = 0; p < 2; ++p)
                    {
                        const std::uint32_t id = At<std::uint32_t>(pairs + p * 8);
                        const std::uint32_t slot = At<std::uint32_t>(pairs + p * 8 + 4);
                        if (slot > 1 || !Known(BaseOf(id)))
                            continue;                           // not what we expect there: do not touch it
                        const std::uint32_t want = ForLevel(Effective(i, static_cast<int>(slot)), kLevels[i].level);
                        if (want != 0 && want != id)
                        {
                            At<std::uint32_t>(pairs + p * 8) = want;
                            ++written;
                        }
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
        return written;
    }

    void SyncPairs()
    {
        static bool faultLogged = false;
        const int n = SyncPairsRaw();
        if (n > 0)
            Logger::Info("RaiderSupers: saved loadout brought in step (" + std::to_string(n) + " super ids)");
        else if (n < 0 && !faultLogged)
        {
            faultLogged = true;
            Logger::Warn("RaiderSupers: could not read the saved loadout (fault) - order swapping may not follow");
        }
    }

    // ------------------------------------------------------------------------------------------------ the window
    constexpr int kComboBase = 1000;                            // control id = base + level * 2 + slot
    constexpr UINT_PTR kTimerId = 1;
    constexpr UINT kRefillMessage = WM_APP + 1;                 // a choice was changed elsewhere: show it
    HWND g_Window = nullptr;
    HWND g_Combo[kLevelCount][2] = {};
    HWND g_GameWindow = nullptr;
    HFONT g_Font = nullptr;
    bool g_KeyWasDown = false;
    bool g_Filling = false;

    void FillCombos()
    {
        g_Filling = true;
        for (int i = 0; i < kLevelCount; ++i)
        {
            for (int s = 0; s < 2; ++s)
            {
                HWND combo = g_Combo[i][s];
                SendMessageA(combo, CB_RESETCONTENT, 0, 0);
                const std::uint32_t stock = g_Stock[i][s].load();
                std::string first = "Stock";
                if (const char* name = NameOf(stock))
                    first += std::string(":  ") + name;
                SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(first.c_str()));
                int selected = 0;
                for (int c = 0; c < kCandidateCount; ++c)
                {
                    SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kCellSupers[c].name));
                    if (g_Choice[i][s].load() == kCellSupers[c].id)
                        selected = c + 1;
                }
                SendMessageA(combo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
            }
        }
        g_Filling = false;
    }

    void Store(int level, int slot, std::uint32_t id)
    {
        g_Choice[level][slot].store(id);
        g_Served[level].store(0);
        WritePrivateProfileStringA("steam", KeyName(level, slot).c_str(), std::to_string(id).c_str(),
            SteamConfig::GetPath().c_str());
    }

    void Choose(int level, int slot, int index)
    {
        const std::uint32_t id = index <= 0 ? 0u : kCellSupers[index - 1].id;
        const std::uint32_t before = Effective(level, slot);
        const std::uint32_t wanted = id != 0 ? id : g_Stock[level][slot].load();
        const int other = 1 - slot;
        if (wanted != 0 && wanted == Effective(level, other) && before != 0 && before != wanted)
            Store(level, other, before == g_Stock[level][other].load() ? 0u : before);   // same super twice = swap
        Store(level, slot, id == g_Stock[level][slot].load() ? 0u : id);
        Logger::Info(std::string("RaiderSupers: ") + kLevels[level].label + " -> super 1 = " +
            std::to_string(Effective(level, 0)) + ", super 2 = " + std::to_string(Effective(level, 1)));
        SyncPairs();
        FillCombos();
    }

    bool GameInFront(HWND* window)
    {
        HWND front = GetForegroundWindow();
        DWORD pid = 0;
        if (front == nullptr || GetWindowThreadProcessId(front, &pid) == 0 || pid != GetCurrentProcessId())
            return false;
        if (window != nullptr)
            *window = front;
        return true;
    }

    void ShowMenu(bool show)
    {
        if (show)
        {
            HWND front = nullptr;
            if (GameInFront(&front) && front != g_Window)
                g_GameWindow = front;
            FillCombos();
            RECT game = {}, own = {};
            GetWindowRect(g_Window, &own);
            if (g_GameWindow != nullptr && GetWindowRect(g_GameWindow, &game))
            {
                const int x = game.left + ((game.right - game.left) - (own.right - own.left)) / 2;
                const int y = game.top + ((game.bottom - game.top) - (own.bottom - own.top)) / 2;
                SetWindowPos(g_Window, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE);
            }
            ShowWindow(g_Window, SW_SHOW);
            SetForegroundWindow(g_Window);
        }
        else
        {
            ShowWindow(g_Window, SW_HIDE);
            if (g_GameWindow != nullptr && IsWindow(g_GameWindow))
                SetForegroundWindow(g_GameWindow);
        }
    }

    LRESULT CALLBACK MenuProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_TIMER:
        {
            const bool down = (GetAsyncKeyState(g_MenuKey) & 0x8000) != 0;
            if (down && !g_KeyWasDown && GameInFront(nullptr))
                ShowMenu(!IsWindowVisible(window));
            g_KeyWasDown = down;
            static int ticks = 0;
            if (++ticks % 10 == 0)                               // about twice a second
                SyncPairs();
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(wparam) == CBN_SELCHANGE && !g_Filling)
            {
                const int id = LOWORD(wparam) - kComboBase;
                if (id >= 0 && id < kLevelCount * 2)
                    Choose(id / 2, id % 2,
                        static_cast<int>(SendMessageA(reinterpret_cast<HWND>(lparam), CB_GETCURSEL, 0, 0)));
            }
            else if (LOWORD(wparam) == IDCANCEL || LOWORD(wparam) == IDOK)
                ShowMenu(false);
            return 0;
        case kRefillMessage:
            FillCombos();
            return 0;
        case WM_CLOSE:
            ShowMenu(false);
            return 0;
        }
        return DefWindowProcA(window, message, wparam, lparam);
    }

    // --------------------------------------------- the game's own RAIDER SUPER ATTACKS page (step 1, 2026-11-17)
    // Project lead on the F6 window: "eventually it will be embedded into the games menu rather than through a
    // windows thing"; 2026-11-17, asked where the menu work should start: "start with 1" (= the game's own screen).
    // Read from the exe (docs\prompt_log.md, 2026-11-17 "super attack page"):
    // - Raider Characters is XG::Game::MenuLobbyLv. Its update (0x1409650F0) runs one handler per page, chosen by
    //   menu + 0x2AF0: 0 = the Raider list (sub_1409682a0), 1 = RAIDER SUPER ATTACKS (sub_14096baf0), 2 = RAIDER
    //   SKILLS (sub_14096a9c0), 3 / 4 = two more pages, 5 / 6 = dialogs.
    // - menu + 0x2AD8 .. + 0x2AE0 = the Raiders (0x1C0 bytes each, + 0 = Raider id, and per level a vector of
    //   176-byte row records at + 0xE0 + level * 0x20: + 0 kind (0 = super), + 4 id, then the texts). The Raider
    //   shown is number (menu + 0x2B20) + (menu + 0x2B24); menu + 0x2AF8 = the level (the Q / E tabs);
    //   menu + 0x29E4 = 1 on the second page some Raiders have at level 4.
    // - menu + 0x2B88 = the list on screen: rows = pointers at menu + 0x2B90 .. + 0x2B98 (a row: + 0x30 the id, or
    //   -1 for the headline; + 0x34 the kind), menu + 0x2BA8 = cursor, + 0x2BAC = scroll, + 0x36B4 = 1 while
    //   "R switch" waits for the second super, + 0x36B8 = the row picked first.
    // - "R switch" ends in sub_140975d50(menu, 0): it swaps the two pairs in the saved loadout (sub_1406f8300) and
    //   then redraws with sub_140973970 (the Raider's rows of this level again, from the level row and the saved
    //   pairs), sub_140967910 (the list), sub_140971c40, the list's "cursor to" call (vtable slot 8),
    //   sub_14096f3b0 (the panel on the right: preview, cooldown, text), sub_140972a30, sub_140976210,
    //   sub_1409729a0 and a sound.
    // What this does: a detour on the page's handler. When the change key (raider_supers_change_key, default F) is
    // pressed while the cursor is on a super, that slot's choice steps to the next super on the list (the one the
    // other slot holds is skipped), is stored exactly like a choice from the F6 window, and the page is redrawn
    // with the same calls "R switch" uses. Everything runs on the game's own thread, inside that handler's call.
    // Possible_ (not confirmed in the game): 0x294B1E44 is the menus' "not possible" sound.
    constexpr std::uintptr_t kSuperPageRva = 0x96BAF0;          // sub_14096baf0(menu): the page, every frame
    constexpr std::uintptr_t kPageRowsRva = 0x973970;           // sub_140973970(menu, Raider entry, level, page 2)
    constexpr std::uintptr_t kPageListRva = 0x967910;           // sub_140967910(menu, 0 = supers)
    constexpr std::uintptr_t kPageLayoutRva = 0x971C40;         // sub_140971c40(menu)
    constexpr std::uintptr_t kPagePanelRva = 0x96F3B0;          // sub_14096f3b0(menu, row, level, page 2, 0.0f, 1)
    constexpr std::uintptr_t kPageCursorRva = 0x972A30;         // sub_140972a30(menu, cursor, scroll, 1)
    constexpr std::uintptr_t kPageDoneRva = 0x976210;           // sub_140976210(menu)
    constexpr std::uintptr_t kPagePickRva = 0x9729A0;           // sub_1409729a0(menu, picked row, 0 = not picking)
    constexpr std::uintptr_t kSoundPlayRva = 0x48B9F0;          // sub_14048b9f0(player, out handle, sound, 0)
    constexpr std::uintptr_t kSoundPlayerGlobalRva = 0x177E488;
    constexpr std::uint32_t kSoundSwitched = 0xC34B1427;        // what "R switch" plays when it is done
    constexpr std::uint32_t kSoundRefused = 0x294B1E44;
    constexpr std::uintptr_t kMenuPage = 0x2AF0;
    constexpr std::uintptr_t kMenuRaiders = 0x2AD8;
    constexpr std::uintptr_t kMenuRaidersEnd = 0x2AE0;
    constexpr std::uintptr_t kMenuRaiderSize = 0x1C0;
    constexpr std::uintptr_t kMenuRaiderCursor = 0x2B20;
    constexpr std::uintptr_t kMenuRaiderScroll = 0x2B24;
    constexpr std::uintptr_t kMenuLevel = 0x2AF8;
    constexpr std::uintptr_t kMenuSecondPage = 0x29E4;
    constexpr std::uintptr_t kMenuList = 0x2B88;
    constexpr std::uintptr_t kMenuRows = 0x2B90;
    constexpr std::uintptr_t kMenuRowsEnd = 0x2B98;
    constexpr std::uintptr_t kMenuCursor = 0x2BA8;
    constexpr std::uintptr_t kMenuScroll = 0x2BAC;
    constexpr std::uintptr_t kMenuPicking = 0x36B4;
    constexpr std::uintptr_t kMenuPicked = 0x36B8;
    constexpr std::uintptr_t kRowId = 0x30;
    constexpr std::uintptr_t kRowKind = 0x34;
    constexpr std::uint32_t kPageSupers = 1;

    struct PageInfo
    {
        std::uint32_t page, raider, level, secondPage, rowId, rowKind, picking;
        std::int32_t cursor, scroll, rows;
    };

    using PageFn = void(__fastcall*)(std::uint8_t* menu);
    using PageRowsFn = void(__fastcall*)(std::uint8_t* menu, std::uintptr_t entry, std::uint32_t level,
        std::uint32_t secondPage);
    using PageListFn = void(__fastcall*)(std::uint8_t* menu, std::uint32_t mode);
    using ListCursorFn = void(__fastcall*)(std::uint8_t* list, std::uint32_t cursor);
    using PagePanelFn = void(__fastcall*)(std::uint8_t* menu, std::uintptr_t row, std::uint32_t level,
        std::uint32_t secondPage, float zero, std::uint8_t one);
    using PageCursorFn = void(__fastcall*)(std::uint8_t* menu, std::int64_t cursor, std::int64_t scroll,
        std::uint32_t one);
    using PagePickFn = void(__fastcall*)(std::uint8_t* menu, std::uint32_t row, std::uint32_t picking);
    using SoundPlayFn = void*(__fastcall*)(void* player, void** handle, std::uint32_t sound, std::uint32_t zero);
    using ReleaseFn = void(__fastcall*)(void* object);

    PageFn g_OriginalSuperPage = nullptr;
    int g_ChangeKey = 'F';
    bool g_ChangeWasDown = false;

    // Plain reads only, so the fault guard can wrap them. false = the page is not in a state we understand.
    bool ReadPageRaw(std::uint8_t* menu, PageInfo* p)
    {
        __try
        {
            const std::uintptr_t m = reinterpret_cast<std::uintptr_t>(menu);
            p->page = At<std::uint32_t>(m + kMenuPage);
            p->level = At<std::uint32_t>(m + kMenuLevel);
            p->secondPage = At<std::uint32_t>(m + kMenuSecondPage);
            p->picking = At<std::uint8_t>(m + kMenuPicking);
            p->cursor = At<std::int32_t>(m + kMenuCursor);
            p->scroll = At<std::int32_t>(m + kMenuScroll);
            const std::uintptr_t raiders = At<std::uintptr_t>(m + kMenuRaiders);
            const std::uintptr_t raidersEnd = At<std::uintptr_t>(m + kMenuRaidersEnd);
            const std::int64_t shown = static_cast<std::int64_t>(At<std::int32_t>(m + kMenuRaiderCursor)) +
                At<std::int32_t>(m + kMenuRaiderScroll);
            if (raiders == 0 || raidersEnd < raiders || shown < 0 ||
                shown >= static_cast<std::int64_t>((raidersEnd - raiders) / kMenuRaiderSize))
                return false;
            p->raider = At<std::uint32_t>(raiders + static_cast<std::uintptr_t>(shown) * kMenuRaiderSize);
            const std::uintptr_t rows = At<std::uintptr_t>(m + kMenuRows);
            const std::uintptr_t rowsEnd = At<std::uintptr_t>(m + kMenuRowsEnd);
            if (rows == 0 || rowsEnd < rows)
                return false;
            p->rows = static_cast<std::int32_t>((rowsEnd - rows) >> 3);
            const std::int64_t at = static_cast<std::int64_t>(p->cursor) + p->scroll;
            if (at < 0 || at >= p->rows)
                return false;
            const std::uintptr_t row = At<std::uintptr_t>(rows + static_cast<std::uintptr_t>(at) * 8);
            if (row == 0)
                return false;
            p->rowId = At<std::uint32_t>(row + kRowId);
            p->rowKind = At<std::uint32_t>(row + kRowKind);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The redraw of "R switch" (sub_140975d50 from game + 0x975E7A on), call for call. 1 = done, -1 = fault.
    int RedrawPageRaw(std::uint8_t* menu)
    {
        __try
        {
            const std::uintptr_t m = reinterpret_cast<std::uintptr_t>(menu);
            const std::uint32_t cursor = At<std::uint32_t>(m + kMenuCursor);
            const std::uintptr_t entry = At<std::uintptr_t>(m + kMenuRaiders) + static_cast<std::uintptr_t>(
                static_cast<std::int64_t>(At<std::int32_t>(m + kMenuRaiderCursor)) +
                At<std::int32_t>(m + kMenuRaiderScroll)) * kMenuRaiderSize;
            reinterpret_cast<PageRowsFn>(g_Base + kPageRowsRva)(menu, entry, At<std::uint32_t>(m + kMenuLevel),
                At<std::uint32_t>(m + kMenuSecondPage));
            reinterpret_cast<PageListFn>(g_Base + kPageListRva)(menu, 0);
            reinterpret_cast<PageFn>(g_Base + kPageLayoutRva)(menu);
            std::uint8_t* list = menu + kMenuList;
            reinterpret_cast<ListCursorFn>(At<void**>(m + kMenuList)[8])(list, cursor);
            std::uintptr_t row = 0;
            const std::uintptr_t rows = At<std::uintptr_t>(m + kMenuRows);
            const std::int32_t count = static_cast<std::int32_t>((At<std::uintptr_t>(m + kMenuRowsEnd) - rows) >> 3);
            const std::int32_t at = At<std::int32_t>(m + kMenuScroll) + At<std::int32_t>(m + kMenuCursor);
            if (count > at && at >= 0)
                row = At<std::uintptr_t>(rows + static_cast<std::uintptr_t>(at) * 8);
            reinterpret_cast<PagePanelFn>(g_Base + kPagePanelRva)(menu, row, At<std::uint32_t>(m + kMenuLevel),
                At<std::uint32_t>(m + kMenuSecondPage), 0.0f, 1);
            reinterpret_cast<PageCursorFn>(g_Base + kPageCursorRva)(menu, At<std::int32_t>(m + kMenuCursor),
                At<std::int32_t>(m + kMenuScroll), 1);
            reinterpret_cast<PageFn>(g_Base + kPageDoneRva)(menu);
            reinterpret_cast<PagePickFn>(g_Base + kPagePickRva)(menu, At<std::uint32_t>(m + kMenuPicked), 0);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // One of the menu's own sounds, played the way the menu does it (play, then let go of the handle).
    void MenuSoundRaw(std::uint32_t sound)
    {
        __try
        {
            void* player = At<void*>(g_Base + kSoundPlayerGlobalRva);
            if (player == nullptr)
                return;
            void* handle = nullptr;
            reinterpret_cast<SoundPlayFn>(g_Base + kSoundPlayRva)(player, &handle, sound, 0);
            if (handle != nullptr)
                reinterpret_cast<ReleaseFn>((*reinterpret_cast<void***>(handle))[6])(handle);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void ChangeOnPage(std::uint8_t* menu)
    {
        static int refusals = 0;
        PageInfo p = {};
        if (!ReadPageRaw(menu, &p))
        {
            if (refusals++ < 20)
                Logger::Warn("RaiderSupers: change key - the Raider Super Attacks page could not be read, nothing done");
            return;
        }
        if (p.page != kPageSupers)
            return;                                             // the handler left the page in this very frame
        int level = -1, slot = -1;
        for (int i = 0; i < kLevelCount; ++i)
            if (kLevels[i].raider == p.raider && kLevels[i].level == p.level)
                level = i;
        const std::uint32_t shown = BaseOf(p.rowId);
        const char* why = nullptr;
        if (level < 0 || p.secondPage != 0)
            why = "no other supers are set up for this Raider and stage";
        else if (p.picking != 0)
            why = "an R switch is under way";
        else if (p.rowKind != 0 || p.rowId == 0xFFFFFFFF)
            why = "the cursor is not on a super";
        else
        {
            for (int s = 0; s < 2; ++s)
                if (Effective(level, s) == shown)
                    slot = s;
            if (slot < 0)
                why = "the super on this row is not one the stage is handed out with";
        }
        std::uint32_t next = 0;
        if (why == nullptr)
        {
            const std::uint32_t other = Effective(level, 1 - slot);
            int at = 0;
            for (int c = 0; c < kCandidateCount; ++c)
                if (kCellSupers[c].id == shown)
                    at = c;
            for (int step = 1; step <= kCandidateCount && next == 0; ++step)
            {
                const std::uint32_t id = kCellSupers[(at + step) % kCandidateCount].id;
                if (id != other && id != shown)
                    next = id;
            }
            if (next == 0)
                why = "there is no other super to step to";
        }
        if (why != nullptr)
        {
            if (refusals++ < 20)
                Logger::Info(std::string("RaiderSupers: change key refused (Raider ") + std::to_string(p.raider) +
                    ", stage " + std::to_string(p.level) + ", row id " + std::to_string(p.rowId) + "): " + why);
            MenuSoundRaw(kSoundRefused);
            return;
        }
        Store(level, slot, next == g_Stock[level][slot].load() ? 0u : next);
        SyncPairs();
        const int drawn = RedrawPageRaw(menu);
        if (drawn == 1)
            MenuSoundRaw(kSoundSwitched);
        const char* name = NameOf(next);
        Logger::Info(std::string("RaiderSupers: change key on the game's page - ") + kLevels[level].label +
            ", super " + std::to_string(slot + 1) + ": " + std::to_string(shown) + " -> " + std::to_string(next) +
            " (" + (name != nullptr ? name : "?") + "), handed to the game as " +
            std::to_string(ForLevel(next, kLevels[level].level)) +
            (drawn == 1 ? "; page redrawn" : "; FAULT while redrawing the page - leave the page and come back"));
        if (g_Window != nullptr)
            PostMessageA(g_Window, kRefillMessage, 0, 0);
    }

    void __fastcall SuperPageDetour(std::uint8_t* menu)
    {
        g_OriginalSuperPage(menu);
        const bool down = (GetAsyncKeyState(g_ChangeKey) & 0x8000) != 0;
        const bool pressed = down && !g_ChangeWasDown;
        g_ChangeWasDown = down;
        HWND front = nullptr;
        if (!pressed || menu == nullptr || !GameInFront(&front) || front == g_Window)
            return;
        ChangeOnPage(menu);
    }

    HWND Label(HWND parent, const char* text, int x, int y, int w, int h)
    {
        HWND label = CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr,
            GetModuleHandleA(nullptr), nullptr);
        SendMessageA(label, WM_SETFONT, reinterpret_cast<WPARAM>(g_Font), TRUE);
        return label;
    }

    DWORD WINAPI MenuThread(LPVOID)
    {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = MenuProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = "BreakersRaiderSupersMenu";
        if (RegisterClassA(&wc) == 0)
        {
            Logger::Error("RaiderSupers: could not register the menu window class");
            return 0;
        }
        const int labelW = 190, comboW = 300, rowH = 34, left = 14, top = 44;
        const int width = left * 2 + labelW + comboW * 2 + 16, height = top + rowH * kLevelCount + 96;
        g_Window = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName,
            "Raider supers - Cell (experimental)", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
            CW_USEDEFAULT, width, height, nullptr, nullptr, wc.hInstance, nullptr);
        if (g_Window == nullptr)
        {
            Logger::Error("RaiderSupers: could not create the menu window");
            return 0;
        }
        g_Font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        Label(g_Window, "Super 1", left + labelW, 16, comboW, 20);
        Label(g_Window, "Super 2", left + labelW + comboW + 8, 16, comboW, 20);
        for (int i = 0; i < kLevelCount; ++i)
        {
            const int y = top + i * rowH;
            Label(g_Window, kLevels[i].label, left, y + 4, labelW, 20);
            for (int s = 0; s < 2; ++s)
            {
                g_Combo[i][s] = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                    CBS_DROPDOWNLIST, left + labelW + s * (comboW + 8), y, comboW, 260, g_Window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kComboBase + i * 2 + s)), wc.hInstance, nullptr);
                SendMessageA(g_Combo[i][s], WM_SETFONT, reinterpret_cast<WPARAM>(g_Font), TRUE);
            }
        }
        Label(g_Window, "Choose before a match. Picking the super the other slot already has swaps the two.",
            left, top + rowH * kLevelCount + 8, width - left * 2, 18);
        Label(g_Window, "Applies to every Cell in the match (also a bot). Local practice matches only.",
            left, top + rowH * kLevelCount + 28, width - left * 2, 18);
        SetTimer(g_Window, kTimerId, 50, nullptr);
        Logger::Info("RaiderSupers: menu ready (key " + std::to_string(g_MenuKey) + ")");
        MSG msg;
        while (GetMessageA(&msg, nullptr, 0, 0) > 0)
        {
            if (IsDialogMessageA(g_Window, &msg))
                continue;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        return 0;
    }
}

bool RaiderSupers::Init()
{
    const std::string& config = SteamConfig::GetPath();
    if (GetPrivateProfileIntA("steam", "raider_supers", 0, config.c_str()) == 0)
        return true;
    g_Base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_Base == 0)
        return false;
    g_MenuKey = GetPrivateProfileIntA("steam", "raider_supers_key", VK_F6, config.c_str());
    if (g_MenuKey < 1 || g_MenuKey > 254)
        g_MenuKey = VK_F6;
    g_UseFitted = GetPrivateProfileIntA("steam", "raider_supers_fitted", 0, config.c_str()) != 0;
    Logger::Info(std::string("RaiderSupers: face-fitted copies of borrowed supers ") +
        (g_UseFitted ? "ON (raider_supers_fitted=1)" : "off (raider_supers_fitted=0)"));
    for (int i = 0; i < kLevelCount; ++i)
    {
        for (int s = 0; s < 2; ++s)
        {
            const std::uint32_t id = GetPrivateProfileIntA("steam", KeyName(i, s).c_str(), 0, config.c_str());
            g_Choice[i][s].store(Known(id) ? id : 0u);
            if (id != 0)
                Logger::Info("RaiderSupers: " + KeyName(i, s) + " = " + std::to_string(id) +
                    (Known(id) ? "" : " (not a super on the list - ignored)"));
        }
    }
    void* target = reinterpret_cast<void*>(g_Base + kRowGetterRva);
    if (!HookManager::Create(target, reinterpret_cast<void*>(&RowDetour), reinterpret_cast<void**>(&g_OriginalRow)) ||
        !HookManager::Enable(target))
    {
        Logger::Error("RaiderSupers: hook on sub_140692840 (Raider level row) failed");
        return false;
    }
    Logger::Info("RaiderSupers: hook on sub_140692840 (Raider level row) installed (raider_supers=1)");
    if (GetPrivateProfileIntA("steam", "raider_supers_marker", 1, config.c_str()) != 0)
    {
        void* marker = reinterpret_cast<void*>(g_Base + kMarkerSetupRva);
        if (HookManager::Create(marker, reinterpret_cast<void*>(&MarkerDetour),
                reinterpret_cast<void**>(&g_OriginalMarker)) && HookManager::Enable(marker))
            Logger::Info("RaiderSupers: hook on sub_1400d3230 (aim marker setting) installed - the aimed Spirit Bomb "
                "gets Planet Crusher's circle (raider_supers_marker=1)");
        else
            Logger::Error("RaiderSupers: hook on sub_1400d3230 (aim marker setting) failed - the aimed Spirit Bomb "
                "keeps Cell's line marker");
        void* family = reinterpret_cast<void*>(g_Base + kIsCellFamilyRva);
        if (HookManager::Create(family, reinterpret_cast<void*>(&IsCellFamilyDetour),
                reinterpret_cast<void**>(&g_OriginalIsCellFamily)) && HookManager::Enable(family))
            Logger::Info("RaiderSupers: hook on sub_140206f10 (Cell-family check) installed - the aimed Spirit Bomb "
                "is aimed with the crosshair");
        else
            Logger::Error("RaiderSupers: hook on sub_140206f10 (Cell-family check) failed - the aimed Spirit Bomb's "
                "circle stays off the crosshair");
    }
    else
        Logger::Info("RaiderSupers: aim marker fix off (raider_supers_marker=0)");
    if (GetPrivateProfileIntA("steam", "raider_supers_page", 1, config.c_str()) != 0)
    {
        g_ChangeKey = GetPrivateProfileIntA("steam", "raider_supers_change_key", 'F', config.c_str());
        if (g_ChangeKey < 1 || g_ChangeKey > 254)
            g_ChangeKey = 'F';
        void* page = reinterpret_cast<void*>(g_Base + kSuperPageRva);
        if (HookManager::Create(page, reinterpret_cast<void*>(&SuperPageDetour),
                reinterpret_cast<void**>(&g_OriginalSuperPage)) && HookManager::Enable(page))
            Logger::Info("RaiderSupers: hook on sub_14096baf0 (the game's Raider Super Attacks page) installed - "
                "change key " + std::to_string(g_ChangeKey) + " steps the super under the cursor "
                "(raider_supers_page=1, raider_supers_change_key)");
        else
            Logger::Error("RaiderSupers: hook on sub_14096baf0 (the game's Raider Super Attacks page) failed - "
                "only the F6 window can change supers");
    }
    else
        Logger::Info("RaiderSupers: change key on the game's Raider Super Attacks page off (raider_supers_page=0)");
    HANDLE thread = CreateThread(nullptr, 0, MenuThread, nullptr, 0, nullptr);
    if (thread != nullptr)
        CloseHandle(thread);
    return thread != nullptr;
}
