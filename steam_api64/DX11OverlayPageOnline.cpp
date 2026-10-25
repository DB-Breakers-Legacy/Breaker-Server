#include "DX11OverlayInternal.h"
#include "SteamLobbyManager.h"
#include "SteamOfflineServer.h"
#include "SteamDiagnostics.h"
#include "SteamConfig.h"

namespace
{
    char g_LobbyName[64] = "Breakers Revived Lobby";
    char g_Host[128] = "127.0.0.1";
    char g_RoomCode[16] = "";
    char g_Password[64] = "";
    int g_Port = 47584;

    void ClampPort()
    {
        if (g_Port <= 0 || g_Port > 65535)
            g_Port = SteamOfflineServer::GetPort() ? SteamOfflineServer::GetPort() : 47584;
    }
}

void DrawOnlinePage()
{
    ImGui::TextUnformatted("LAN / Custom Multiplayer");
    ImGui::Separator();
    ImGui::TextWrapped(
        "Safe multiplayer mode: local fake-Steam lobbies, LAN/VPN discovery, room-code join, and direct custom-server host join. Official Steam/Namco traffic remains blocked.");
    ImGui::Spacing();

    const SteamConfig::Config& config = SteamConfig::Get();
    ImGui::Text("Mode: %s", SteamConfig::ModeName(config.CurrentMode));
    ImGui::Text("LAN only: %s", config.LanOnly ? "yes" : "no, configured custom_server is allowed");
    ImGui::Text("Discovery/P2P UDP port: %u", static_cast<unsigned>(SteamOfflineServer::GetPort()));

    const int visible = SteamLobbyManager::RefreshNetworkLobbies(0);
    ImGui::Text("Visible lobbies: %d", visible);

    const std::string room = SteamLobbyManager::GetCurrentRoomCode();
    if (!room.empty())
        ImGui::Text("Current room code: %s", room.c_str());

    ImGui::TextWrapped("Last join status: %s", SteamLobbyManager::GetLastJoinError().c_str());
    ImGui::Separator();

    ImGui::InputText("Lobby name", g_LobbyName, IM_ARRAYSIZE(g_LobbyName));
    ImGui::InputText("Password (optional)", g_Password, IM_ARRAYSIZE(g_Password), ImGuiInputTextFlags_Password);

    if (ImGui::Button("Host lobby"))
    {
        SteamLobbyManager::CreateLobbyWithOptions(2, 4, g_LobbyName, g_Password);
    }

    ImGui::SameLine();
    if (ImGui::Button("Leave lobby"))
    {
        SteamLobbyManager::LeaveCurrentLobby();
    }

    ImGui::Separator();
    ImGui::InputText("Room code", g_RoomCode, IM_ARRAYSIZE(g_RoomCode));
    if (ImGui::Button("Join by room code"))
    {
        SteamLobbyManager::JoinRoomCode(g_RoomCode, g_Password);
    }

    ImGui::Separator();
    ImGui::InputText("Host/IP", g_Host, IM_ARRAYSIZE(g_Host));
    ImGui::InputInt("Port", &g_Port);
    ClampPort();
    if (ImGui::Button("Refresh host"))
    {
        SteamOfflineServer::RefreshHost(g_Host, static_cast<uint16_t>(g_Port), 500);
    }

    ImGui::SameLine();
    if (ImGui::Button("Join host"))
    {
        SteamLobbyManager::JoinAddress(g_Host, static_cast<uint16_t>(g_Port), g_Password);
    }

    ImGui::Spacing();
    ImGui::TextWrapped(
        "For internet play, use a VPN range such as Radmin/Hamachi/Tailscale, or set steam_config.ini mode=custom, lan_only=0, custom_server=<your IP/domain>, custom_port=47584.");
}
