#include "DX11OverlayInternal.h"
#include "SteamLobbyManager.h"
#include "SteamOfflineServer.h"
#include "SteamDiagnostics.h"

void DrawOnlinePage()
{
    ImGui::TextUnformatted("LAN Online");
    ImGui::Separator();
    ImGui::TextWrapped("This release uses LAN/offline discovery only. Hosted master, account, friend, invite, leaderboard sync, and relay services are removed.");
    ImGui::Spacing();
    ImGui::Text("LAN discovery port: %u", static_cast<unsigned>(SteamOfflineServer::GetPort()));
    ImGui::Text("Visible LAN lobbies: %d", SteamLobbyManager::RefreshNetworkLobbies(0));
    const std::string room = SteamLobbyManager::GetCurrentRoomCode();
    if (!room.empty())
        ImGui::Text("Current room code: %s", room.c_str());
    ImGui::TextWrapped("Last join status: %s", SteamLobbyManager::GetLastJoinError().c_str());
}
