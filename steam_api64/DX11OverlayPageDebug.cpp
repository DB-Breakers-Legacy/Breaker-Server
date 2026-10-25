#include "DX11OverlayInternal.h"
#include "SteamCallbackManager.h"
#include "SteamCallResultManager.h"
#include "SteamConfig.h"
#include "SteamDiagnostics.h"
#include "SteamIDManager.h"
#include "SteamLobbyManager.h"
#include "SteamOfflineServer.h"
#include "SteamVersionLogger.h"

void DrawDebugPage()
{
    if (g_SelectedSubPage == 0)
    {
        ImGui::Text("Debug > Performance");
        HelpMarker("Runtime performance counters for the overlay and hooked renderer.");
        ImGui::Separator();

        ImGui::Checkbox("Show FPS graph", &g_DebugFpsGraph);
        ImGui::Checkbox("Show frame time graph", &g_DebugFrameTimeGraph);
        ImGui::Checkbox("Show min / avg / max FPS", &g_DebugShowMinAvgMax);
        ImGui::Checkbox("Show Present counter", &g_DebugShowPresentCount);
        ImGui::Checkbox("Show ResizeBuffers counter", &g_DebugShowResizeCount);
        ImGui::Checkbox("Show ImGui stats", &g_DebugShowImGuiStats);

        if (g_DebugFpsGraph)
        {
            ImGui::PlotLines(
                "FPS",
                g_FpsHistory,
                ARRAYSIZE(g_FpsHistory),
                g_FpsHistoryOffset,
                nullptr,
                0.0f,
                144.0f,
                ImVec2(460.0f, 120.0f));
        }

        ImGui::Text("Current FPS: %d", g_CurrentFps);
        ImGui::Text("Frame time: %.3f ms", g_CurrentFps > 0 ? 1000.0f / static_cast<float>(g_CurrentFps) : 0.0f);
    }
    else if (g_SelectedSubPage == 1)
    {
        ImGui::Text("Debug > Render");
        HelpMarker("Renderer, swapchain, and render-target diagnostic information.");
        ImGui::Separator();

        ImGui::Checkbox("Show render handles", &g_DebugShowRenderHandles);
        ImGui::Checkbox("Show swapchain info", &g_DebugShowSwapchainInfo);
        ImGui::Checkbox("Show window state", &g_DebugShowWindowState);
        ImGui::Checkbox("Show refresh rate", &g_DebugShowRefreshRate);

        if (ImGui::Button("Force recreate render target", ImVec2(220.0f, 24.0f)))
        {
            CleanupRenderTarget();
            Logger::Info("DX11 overlay: render target manually recreated");
        }

        if (g_DebugShowRenderHandles)
        {
            ImGui::Separator();
            ImGui::Text("HWND: 0x%p", g_GameWindow);
            ImGui::Text("Device: 0x%p", g_Device);
            ImGui::Text("Context: 0x%p", g_Context);
            ImGui::Text("RTV: 0x%p", g_RenderTargetView);
        }
    }
    else if (g_SelectedSubPage == 2)
    {
        ImGui::Text("Debug > Steam");
        HelpMarker("Steam proxy/emulation debug options.");
        ImGui::Separator();

        ImGui::Checkbox("Log Steam interface calls", &g_DebugLogSteamInterfaces);
        ImGui::Checkbox("Log callbacks", &g_DebugLogCallbacks);
        ImGui::Checkbox("Log call results", &g_DebugLogCallResults);
        ImGui::Checkbox("Show fake SteamID", &g_DebugShowSteamId);
        ImGui::Checkbox("Show lobby state", &g_DebugShowLobbyState);

        ImGui::Separator();
        ImGui::Text("Steam proxy: loaded");
        ImGui::Text("Remote storage: local");
        ImGui::Text("Stats: emulated");
        ImGui::Text("Matchmaking: emulated");

        SteamConfig::Config& config = SteamConfig::Mutable();

        ImGui::Separator();
        ImGui::Text("Steam safety config");
        ImGui::Checkbox("Block official Steam/Bandai traffic", &config.BlockOfficialTraffic);
        ImGui::Checkbox("Local storage", &config.EnableLocalStorage);
        ImGui::Checkbox("Local stats", &config.EnableLocalStats);
        ImGui::Checkbox("Local matchmaking", &config.EnableLocalMatchmaking);

        if (ImGui::Button("Save Steam Debug Config", ImVec2(210.0f, 24.0f)))
            SteamConfig::Save();

        ImGui::SameLine();

        if (ImGui::Button("Write Diagnostics Report", ImVec2(210.0f, 24.0f)))
            SteamDiagnostics::WriteReport();

        ImGui::Separator();
        ImGui::Text("SteamID: %llu", static_cast<unsigned long long>(SteamIDManager::GetSteamID64()));
        ImGui::Text("Current lobby: %llu", static_cast<unsigned long long>(SteamLobbyManager::GetCurrentLobbyID()));
        ImGui::Text("Queued callbacks: %d", SteamCallbackManager::Count());
        ImGui::Text("Stored call results: %d", SteamCallResultManager::Count());
        ImGui::Text("Interface requests: %llu", static_cast<unsigned long long>(SteamVersionLogger::InterfaceRequestCount()));

        const std::string lastRequest = SteamVersionLogger::LastInterfaceRequest();
        ImGui::TextWrapped("Last interface: %s", lastRequest.empty() ? "none" : lastRequest.c_str());

        if (ImGui::Button("Create offline lobby", ImVec2(180.0f, 24.0f)))
        {
            const CSteamID lobby = SteamLobbyManager::CreateLobby(4);
            Logger::Info("DX11 overlay: created offline lobby " + std::to_string(static_cast<unsigned long long>(lobby)));
        }

        ImGui::SameLine();
        if (ImGui::Button("Refresh LAN lobbies", ImVec2(180.0f, 24.0f)))
        {
            const int count = SteamLobbyManager::RefreshNetworkLobbies(10);
            Logger::Info("DX11 overlay: refreshed offline LAN lobbies count=" + std::to_string(count));
        }

        ImGui::SameLine();
        if (ImGui::Button("Pump callbacks", ImVec2(160.0f, 24.0f)))
        {
            SteamCallbackManager::RunCallbacks();
            Logger::Info("DX11 overlay: manually pumped Steam callbacks");
        }

        ImGui::Separator();
        ImGui::Text("Recent interface routes");
        const std::vector<SteamVersionLogger::InterfaceRequestInfo> requests = SteamVersionLogger::SnapshotInterfaceRequests();
        if (ImGui::BeginChild("steam_interface_routes", ImVec2(0.0f, 180.0f), true))
        {
            if (ImGui::BeginTable("steam_interface_routes_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable))
            {
                ImGui::TableSetupColumn("Source");
                ImGui::TableSetupColumn("Version");
                ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableHeadersRow();

                for (const auto& request : requests)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextWrapped("%s", request.Source.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s", request.Version.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%llu", static_cast<unsigned long long>(request.Count));
                }

                ImGui::EndTable();
            }
        }
        ImGui::EndChild();

        if (ImGui::Button("Dump Steam registry", ImVec2(180.0f, 24.0f)))
        {
            for (const auto& request : requests)
            {
                Logger::Info(
                    "Steam registry route: " +
                    request.Source +
                    " -> " +
                    request.Version +
                    " x" +
                    std::to_string(static_cast<unsigned long long>(request.Count)));
            }
        }
    }
    else if (g_SelectedSubPage == 3)
    {
        ImGui::Text("Debug > Network");
        HelpMarker("Network hook diagnostics for DNS and socket-related code.");
        ImGui::Separator();

        ImGui::Checkbox("Log DNS requests", &g_DebugLogDns);
        ImGui::Checkbox("Log socket connects", &g_DebugLogSocketConnects);
        ImGui::Checkbox("Show last blocked host/IP", &g_DebugShowLastBlockedAddress);
        ImGui::Checkbox("Show network hook status", &g_DebugShowNetworkHookStatus);
        ImGui::Checkbox("Force block traffic", &g_DebugForceBlockTraffic);
        ImGui::Checkbox("LAN only", &g_DebugLanOnly);

        SteamConfig::Config& config = SteamConfig::Mutable();

        ImGui::Separator();
        ImGui::Text("Online routing debug");
        ImGui::Checkbox("Config LAN only", &config.LanOnly);
        ImGui::Checkbox("Config packet logging", &config.LogPackets);
        ImGui::Text("Discovery socket: %s", SteamOfflineServer::IsRunning() ? "running" : "stopped");
        ImGui::Text("Local endpoint: %s:%u", SteamOfflineServer::GetLocalAddress().c_str(), static_cast<unsigned int>(SteamOfflineServer::GetPort()));

        if (ImGui::Button("Start Local Discovery", ImVec2(190.0f, 24.0f)))
            SteamOfflineServer::Init();

        ImGui::SameLine();

        if (ImGui::Button("Stop Local Discovery", ImVec2(190.0f, 24.0f)))
            SteamOfflineServer::Shutdown();

        ImGui::SameLine();

        if (ImGui::Button("Save Network Config", ImVec2(180.0f, 24.0f)))
            SteamConfig::Save();

        ImGui::Separator();
        ImGui::Text("connect hook: enabled");
        ImGui::Text("WSAConnect hook: enabled");
        ImGui::Text("getaddrinfo hook: enabled");
        ImGui::Text("GetAddrInfoW hook: enabled");
    }
    else if (g_SelectedSubPage == 4)
    {
        ImGui::Text("Debug > Storage");
        HelpMarker("Local storage and save debug options.");
        ImGui::Separator();

        ImGui::Checkbox("Log storage reads", &g_DebugLogStorageReads);
        ImGui::Checkbox("Log storage writes", &g_DebugLogStorageWrites);
        ImGui::Checkbox("Show storage folder", &g_DebugShowStorageFolder);
        ImGui::Checkbox("Show last save file", &g_DebugShowLastSaveFile);

        ImGui::Separator();
        ImGui::Text("Storage mode: local");
        ImGui::Text("Remote storage: emulated");
    }
    else if (g_SelectedSubPage == 5)
    {
        ImGui::Text("Debug > Mods");
        HelpMarker("Mod loader diagnostics.");
        ImGui::Separator();

        ImGui::Checkbox("Show mods folder", &g_DebugShowModsFolder);
        ImGui::Checkbox("Log mod errors", &g_DebugLogModErrors);
        ImGui::Checkbox("Show loaded mod count", &g_DebugShowLoadedModCount);

        ImGui::Separator();
        ImGui::Text("Mods folder: BreakersRevived\\mods");
        ImGui::Text("Loaded mods: 0");
    }
    else
    {
        ImGui::Text("Debug > Tools");
        HelpMarker("Emergency/debug actions.");
        ImGui::Separator();

        if (ImGui::Button("Write test log", ImVec2(180.0f, 24.0f)))
            Logger::Info("DX11 overlay: test log from debug tools");

        if (ImGui::Button("Emergency disable overlay", ImVec2(220.0f, 24.0f)))
            g_OverlayEnabled = false;
    }
}
