#include "StdInc.h"
#include "SteamSessionManager.h"
#include "SteamLobbyManager.h"
#include "SteamIDManager.h"
#include "Logger.h"

namespace
{
    uint64_t g_CurrentLobby = 0;
}

namespace SteamSessionManager
{
    bool Init()
    {
        g_CurrentLobby = 0;
        Logger::Info("SteamSessionManager initialized");
        return true;
    }

    void Shutdown()
    {
        g_CurrentLobby = 0;
    }

    void JoinLobby(CSteamID lobbyID)
    {
        uint64_t id = SteamIDManager::ToUint64(lobbyID);

        if (id == 0)
            return;

        SteamLobbyManager::JoinLobby(lobbyID);
        g_CurrentLobby = id;

        Logger::Info("SteamSessionManager joined lobby: " + std::to_string(static_cast<unsigned long long>(g_CurrentLobby)));
    }

    void LeaveLobby(CSteamID lobbyID)
    {
        uint64_t id = SteamIDManager::ToUint64(lobbyID);

        if (id == 0)
            id = g_CurrentLobby;

        if (id != 0)
            SteamLobbyManager::LeaveLobby(SteamIDManager::FromUint64(id));

        if (g_CurrentLobby == id)
            g_CurrentLobby = 0;
    }

    void LeaveLobby()
    {
        LeaveLobby(SteamIDManager::FromUint64(g_CurrentLobby));
    }

    CSteamID GetCurrentLobby()
    {
        return SteamIDManager::FromUint64(g_CurrentLobby);
    }

    uint64_t GetCurrentLobbyID()
    {
        return g_CurrentLobby;
    }

    bool IsInLobby()
    {
        return g_CurrentLobby != 0;
    }
}