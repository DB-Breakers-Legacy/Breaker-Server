#pragma once

// Raider super selection (experimental, 2026-11-16). Off unless steam_config.ini has raider_supers=1.
// A small window (hotkey raider_supers_key, default F6) lets the player choose the two super attacks of each
// Raider level before a match. Private testing in local matches only.
// 2026-11-17: on the game's own Raider Characters -> RAIDER SUPER ATTACKS page, a key (raider_supers_change_key,
// default F; raider_supers_page=0 switches it off) steps the super under the cursor to the next one on the list.
namespace RaiderSupers
{
    bool Init();
}
