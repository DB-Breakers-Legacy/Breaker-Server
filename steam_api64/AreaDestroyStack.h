#pragma once

// Area destruction stacking (experimental, 2026-11-17). Off unless steam_config.ini has raider_destroy_stack=1.
// An area destruction the Raider did not use before evolving is carried over to the next level instead of being
// lost. raider_destroy_bonus=1 adds the test rule "nothing destroyed before level 4 = 4 destructions at level 4".
// Private testing in local matches only.
namespace AreaDestroyStack
{
    bool Init();
}
