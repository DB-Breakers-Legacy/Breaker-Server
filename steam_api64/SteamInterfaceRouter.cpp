#include "StdInc.h"
#include "SteamInterfaceRouter.h"
#include "FakeSteamInterfaces.h"
#include "FakeSteamCore.h"
#include "SteamVersionLogger.h"
#include "Logger.h"

#include <cctype>

extern "C" void* __cdecl BreakersRevived_SteamNetworking();
extern "C" void* __cdecl BreakersRevived_SteamGameServerNetworking();
extern "C" void* __cdecl BreakersRevived_SteamNetworkingUtils();
extern "C" void* __cdecl BreakersRevived_SteamNetworkingMessages();
extern "C" void* __cdecl BreakersRevived_SteamNetworkingSockets();
extern "C" void* __cdecl BreakersRevived_SteamGameServerNetworkingSockets();

static std::string UpperInterfaceName(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

static bool HasInterface(const std::string& value, const char* token)
{
    return value.find(token) != std::string::npos;
}

static void* RouteCommon(const std::string& v)
{
    std::string u = UpperInterfaceName(v);

    if (HasInterface(u, "STEAMNETWORKINGMESSAGES")) return BreakersRevived_SteamNetworkingMessages();
    if (HasInterface(u, "STEAMNETWORKINGSOCKETS")) return BreakersRevived_SteamNetworkingSockets();
    if (HasInterface(u, "STEAMNETWORKINGUTILS")) return BreakersRevived_SteamNetworkingUtils();
    if (HasInterface(u, "STEAMNETWORKING")) return BreakersRevived_SteamNetworking();

    if (HasInterface(u, "STEAMGAMECOORDINATOR")) return FakeSteamInterfaces::GameCoordinator();
    if (HasInterface(u, "STEAMUNIFIEDMESSAGES")) return FakeSteamInterfaces::UnifiedMessages();
    if (HasInterface(u, "STEAMAPPTICKET")) return FakeSteamInterfaces::AppTicket();
    if (HasInterface(u, "STEAMTV")) return FakeSteamInterfaces::TV();
    if (HasInterface(u, "STEAMMASTERSERVERUPDATER")) return FakeSteamInterfaces::MasterServerUpdater();

    if (HasInterface(u, "STEAMFRIENDS")) return FakeSteamInterfaces::FriendsForVersion(v.c_str());
    if (HasInterface(u, "STEAMMATCHMAKINGSERVERS")) return FakeSteamInterfaces::MatchmakingServers();
    if (HasInterface(u, "STEAMMATCHMAKING"))
    {
        if (HasInterface(u, "006"))
            return FakeSteamInterfaces::MatchmakingV006();

        if (HasInterface(u, "007"))
            return FakeSteamInterfaces::MatchmakingV007();

        if (HasInterface(u, "008"))
            return FakeSteamInterfaces::MatchmakingV008();

        if (HasInterface(u, "009"))
            return FakeSteamInterfaces::MatchmakingV009();

        return FakeSteamInterfaces::Matchmaking();
    }
    if (HasInterface(u, "STEAMREMOTESTORAGE")) return FakeSteamInterfaces::RemoteStorageForVersion(v.c_str());
    if (HasInterface(u, "STEAMSCREENSHOTS")) return FakeSteamInterfaces::Screenshots();
    if (HasInterface(u, "STEAMHTTP")) return FakeSteamInterfaces::HTTP();
    if (HasInterface(u, "STEAMCONTROLLER")) return FakeSteamInterfaces::Controller();
    if (HasInterface(u, "STEAMUGC")) return FakeSteamInterfaces::UGC();
    if (HasInterface(u, "STEAMAPPLIST")) return FakeSteamInterfaces::AppList();
    if (HasInterface(u, "STEAMMUSICREMOTE")) return FakeSteamInterfaces::MusicRemote();
    if (HasInterface(u, "STEAMMUSIC")) return FakeSteamInterfaces::Music();
    if (HasInterface(u, "STEAMHTMLSURFACE")) return FakeSteamInterfaces::HTMLSurface();
    if (HasInterface(u, "STEAMINVENTORY")) return FakeSteamInterfaces::Inventory();
    if (HasInterface(u, "STEAMVIDEO")) return FakeSteamInterfaces::Video();
    if (HasInterface(u, "STEAMPARENTALSETTINGS")) return FakeSteamInterfaces::ParentalSettings();
    if (HasInterface(u, "STEAMGAMESEARCH")) return FakeSteamInterfaces::GameSearch();
    if (HasInterface(u, "STEAMINPUT")) return FakeSteamInterfaces::Input();
    if (HasInterface(u, "STEAMPARTIES")) return FakeSteamInterfaces::Parties();
    if (HasInterface(u, "STEAMREMOTEPLAY")) return FakeSteamInterfaces::RemotePlay();

    if (HasInterface(u, "STEAMCLIENT")) return FakeSteamCore::Client();
    if (HasInterface(u, "STEAMGAMESERVERSTATS")) return FakeSteamCore::GameServerStats();
    if (HasInterface(u, "STEAMGAMESERVER")) return FakeSteamCore::GameServer();
    if (HasInterface(u, "STEAMUSERSTATS"))
    {
        if (HasInterface(u, "009") || HasInterface(u, "010") || HasInterface(u, "008"))
            return FakeSteamInterfaces::UserStatsLegacy();

        if (HasInterface(u, "011"))
            return FakeSteamInterfaces::UserStatsV011();

        return FakeSteamInterfaces::UserStats();
    }
    if (HasInterface(u, "STEAMUSER")) return FakeSteamCore::UserForVersion(v.c_str());
    if (HasInterface(u, "STEAMUTILS")) return FakeSteamCore::Utils();
    if (HasInterface(u, "STEAMAPPS")) return FakeSteamCore::Apps();

    Logger::Error("SteamInterfaceRouter: using offline fallback for " + v);
    return FakeSteamInterfaces::Fallback();
}

namespace SteamInterfaceRouter
{
    void* RouteCreateInterface(const char* version)
    {
        SteamVersionLogger::LogInterfaceRequest("SteamInternal_CreateInterface", version);

        std::string v = version ? version : "";
        return RouteCommon(v);
    }

    void* RouteUserInterface(HSteamUser user, const char* version)
    {
        NSR_UNUSED(user);

        SteamVersionLogger::LogInterfaceRequest("SteamInternal_FindOrCreateUserInterface", version);

        std::string v = version ? version : "";
        return RouteCommon(v);
    }

    void* RouteGameServerInterface(HSteamUser user, const char* version)
    {
        NSR_UNUSED(user);

        SteamVersionLogger::LogInterfaceRequest("SteamInternal_FindOrCreateGameServerInterface", version);

        std::string v = version ? version : "";

        std::string u = UpperInterfaceName(v);

        if (HasInterface(u, "STEAMNETWORKINGSOCKETS"))
            return BreakersRevived_SteamGameServerNetworkingSockets();

        if (HasInterface(u, "STEAMNETWORKING"))
            return BreakersRevived_SteamGameServerNetworking();

        return RouteCommon(v);
    }
}
