#pragma once

// Practice-mode survivor bots (experimental). Off unless steam_config.ini has survivor_bots=1.
namespace PracticeBots
{
    bool Init();
    void Shutdown();
}
