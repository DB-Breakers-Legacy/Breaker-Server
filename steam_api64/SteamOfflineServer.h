#pragma once
#include "StdInc.h"

namespace SteamOfflineServer
{
    struct LobbyRecord
    {
        uint64_t LobbyID = 0;
        uint64_t OwnerID = 0;
        std::string Name = "Breakers Revived Lobby";
        std::string RoomCode;
        std::string Host = "127.0.0.1";
        uint16_t Port = 47584;
        int Members = 1;
        int MaxMembers = 4;
        bool Joinable = true;
        bool PasswordProtected = false;
        std::string PasswordHash;
        uint64_t LastSeenMs = 0;
        std::map<std::string, std::string> Data;
    };

    bool Init();
    void Shutdown();

    void AdvertiseLobby(const LobbyRecord& record);
    void ClearLocalLobby(uint64_t lobbyID);
    void MarkJoinedLobby(uint64_t lobbyID);

    void Poll();
    int RefreshNow(int waitMs = 25);
    int RefreshHost(const std::string& host, uint16_t port, int waitMs = 250);
    bool SendP2PPacket(CSteamID target, const void* data, uint32_t size, int channel);
    std::vector<LobbyRecord> GetLobbies();
    bool GetLobby(uint64_t lobbyID, LobbyRecord& outRecord);
    bool GetBestEndpoint(uint64_t remoteOrLobbyID, std::string& outHost, uint16_t& outPort);
    bool JoinByRoomCode(const std::string& roomCode, const std::string& password, LobbyRecord& outRecord, int waitMs = 1500);
    bool JoinByAddress(const std::string& host, uint16_t port, const std::string& password, LobbyRecord& outRecord, int waitMs = 1500);

    std::string GetLocalAddress();
    std::string GenerateRoomCode(uint64_t lobbyID);
    std::string NormalizeRoomCode(const std::string& roomCode);
    std::string HashPassword(const std::string& password, const std::string& roomCode);
    std::string LastJoinStatus();
    bool IsRunning();
    uint16_t GetPort();
}
