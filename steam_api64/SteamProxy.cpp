#include "StdInc.h"
#include "SteamProxy.h"
#include "Logger.h"

static HMODULE g_RealSteamModule = nullptr;
static std::string g_RealSteamPath = "offline";

namespace SteamProxy
{
    bool Init()
    {
        Logger::Info("SteamProxy disabled: running fully offline");
        g_RealSteamModule = nullptr;
        g_RealSteamPath = "offline";
        return true;
    }

    FARPROC GetExport(const char* name)
    {
        Logger::Info(std::string("SteamProxy offline: blocked real export ") + (name ? name : "null"));
        return nullptr;
    }

    HMODULE GetRealModule()
    {
        return g_RealSteamModule;
    }

    const std::string& GetRealSteamPath()
    {
        return g_RealSteamPath;
    }
}
