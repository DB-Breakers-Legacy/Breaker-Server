#pragma once
#include "StdInc.h"

namespace SteamInterfaceRouter
{
    void* RouteCreateInterface(const char* version);
    void* RouteUserInterface(HSteamUser user, const char* version);
    void* RouteGameServerInterface(HSteamUser user, const char* version);
}