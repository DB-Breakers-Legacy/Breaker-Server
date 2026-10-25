#pragma once
#include "StdInc.h"

namespace FakeSteamInterfaces
{
    void* Client();
    void* User();
    void* Utils();
    void* Apps();
    void* GameServer();
    void* GameServerStats();

    void* Friends();
    void* FriendsForVersion(const char* version);
    void* Matchmaking();
    void* MatchmakingV006();
    void* MatchmakingV007();
    void* MatchmakingV008();
    void* MatchmakingV009();
    void* MatchmakingServers();
    void* UserStats();
    void* UserStatsLegacy();
    void* UserStatsV011();
    void* RemoteStorage();
    void* RemoteStorageForVersion(const char* version);
    void* Screenshots();
    void* HTTP();
    void* Controller();
    void* UGC();
    void* AppList();
    void* Music();
    void* MusicRemote();
    void* HTMLSurface();
    void* Inventory();
    void* Video();
    void* ParentalSettings();
    void* GameSearch();
    void* Input();
    void* Parties();
    void* RemotePlay();

    void* GameCoordinator();
    void* UnifiedMessages();
    void* AppTicket();
    void* TV();
    void* MasterServerUpdater();
    void* Fallback();
}
