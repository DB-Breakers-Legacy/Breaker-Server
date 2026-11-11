#pragma once
#include "StdInc.h"

namespace SteamSessionManager
{
    bool Init();
    void Shutdown();

    void JoinLobby(CSteamID lobbyID);
    void LeaveLobby(CSteamID lobbyID);
    void LeaveLobby();

    CSteamID GetCurrentLobby();
    uint64_t GetCurrentLobbyID();
    bool IsInLobby();
}