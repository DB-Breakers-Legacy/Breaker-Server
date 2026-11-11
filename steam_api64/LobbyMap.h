#pragma once

// Experimental: load another map as the lobby (steam_config.ini lobby_map=<map number>, e.g. 8000 = Time Nest).
// Off unless lobby_map is set.
namespace LobbyMap
{
    bool Init();
    void Shutdown();
}
