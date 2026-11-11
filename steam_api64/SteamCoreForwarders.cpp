#include "StdInc.h"
#include "SteamCoreForwarders.h"
#include "FakeSteamCore.h"

namespace SteamCoreForwarders
{
    void* Client()
    {
        return FakeSteamCore::Client();
    }

    void* User()
    {
        return FakeSteamCore::User();
    }

    void* Apps()
    {
        return FakeSteamCore::Apps();
    }

    void* Utils()
    {
        return FakeSteamCore::Utils();
    }
}
