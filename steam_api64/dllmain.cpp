#include "StdInc.h"

#include "Logger.h"
#include "SteamProxy.h"
#include "HookManager.h"
#include "NetworkHooks.h"
#include "OnlineCheckBypass.h"
#include "BackendStatus.h"

#include "SteamConfig.h"
#include "SteamIDManager.h"
#include "SteamSessionManager.h"
#include "SteamLobbyManager.h"
#include "SteamP2PManager.h"
#include "Auth/SteamAuth.h"
#include "SteamCallbackManager.h"
#include "SteamCallResultManager.h"
#include "SteamDiagnostics.h"
#include "SteamFactoryRegistry.h"
#include "SteamPersonaManager.h"
#include "SteamStorageLocal.h"
#include "SteamStatsLocal.h"
#include "FakeSteamCore.h"

static HANDLE g_MainThread = nullptr;

static DWORD WINAPI MainThread(LPVOID)
{
    if (!Logger::Init())
        return 0;

    Logger::Info("BreakersRevived Steam emulator loaded");
    SteamDiagnostics::Init();
    SteamDiagnostics::MarkSteam("Startup", "Steam emulator loaded");

    if (!SteamProxy::Init())
    {
        Logger::Error("Steam proxy failed to initialize");
        return 0;
    }

    if (!HookManager::Init())
    {
        Logger::Error("Hook manager failed to initialize");
        return 0;
    }

    if (!OnlineCheckBypass::Init())
        Logger::Error("Breakers forced-true startup hooks failed to initialize");

    SteamConfig::Init();
    BackendStatus::Check();
    SteamIDManager::Init();
    SteamSessionManager::Init();
    SteamLobbyManager::Init();
    SteamP2PManager::Init();
    SteamAuth::Init();
    SteamCallbackManager::Init();
    SteamCallResultManager::Init();
    FakeSteamCore::Init();
    SteamPersonaManager::Init();
    SteamStorageLocal::Init();
    SteamStatsLocal::Init();

    SteamFactoryRegistry::Dump();

    if (!NetworkHooks::Init())
        Logger::Error("Network hooks failed to initialize");

    Logger::Info("BreakersRevived Steam emulator initialized");
    Logger::Info("Runtime mode: Steam emulator + local backend redirect + LAN/VPN networking");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        g_MainThread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        NetworkHooks::Shutdown();
        OnlineCheckBypass::Shutdown();
        FakeSteamCore::Shutdown();
        HookManager::Shutdown();
        SteamDiagnostics::Shutdown();
        Logger::Shutdown();
        break;
    }

    return TRUE;
}
