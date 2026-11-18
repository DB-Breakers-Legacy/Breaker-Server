#include "StdInc.h"

#include "Logger.h"
#include "SteamProxy.h"
#include "HookManager.h"
#include "NetworkHooks.h"
#include "OnlineCheckBypass.h"
#include "PracticeBots.h"
#include "LobbyMap.h"
#include "RaiderSupers.h"
#include "AreaDestroyStack.h"
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

    // ORDER MATTERS (2026-11-17, found after an evening of "the game is stuck on loading"):
    // this thread runs next to the game's main thread. About 0.64 s after the DLL loads, the game registers its Steam
    // callbacks (SteamAPI_RegisterCallback 331, 1101 ... 152) and asks for its stats (RequestCurrentStats).
    // SteamCallbackManager::Init() and SteamCallResultManager::Init() EMPTY their lists. When they ran after the
    // game had registered, the registrations were wiped, the game never got its answers and sat on the loading
    // screen forever (main thread just looping frames, no request ever sent to the server).
    // They used to run after every feature hook (each hook costs about 45 ms) and after the backend probe (about
    // 0.3 s when the local server is down - which is why "server not running" also looked like a loading hang).
    // So: everything of the Steam emulator that the game talks to is set up FIRST, the slow things come after.
    SteamConfig::Init();
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
    Logger::Info("Steam emulator parts ready (before the feature hooks - see the note in dllmain.cpp)");

    if (!OnlineCheckBypass::Init())
        Logger::Error("Breakers forced-true startup hooks failed to initialize");

    if (!PracticeBots::Init())
        Logger::Error("Practice survivor bot hooks failed to initialize");
    if (!LobbyMap::Init())
        Logger::Error("Lobby map hook failed to initialize");
    if (!RaiderSupers::Init())
        Logger::Error("Raider super selection failed to initialize");
    if (!AreaDestroyStack::Init())
        Logger::Error("Area destruction stacking failed to initialize");
    BackendStatus::Check();

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
