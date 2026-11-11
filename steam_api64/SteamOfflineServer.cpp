#include "StdInc.h"
#include "SteamOfflineServer.h"
#include "SteamConfig.h"
#include "SteamIDManager.h"
#include "SteamP2PManager.h"
#include "SteamCallbackManager.h"
#include "Logger.h"

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iomanip>

namespace
{
    constexpr const char* kMagic = "BREAKERSREVIVED_LOBBY_V1";
    constexpr int kPacketMax = 4096;

    std::mutex g_Mutex;
    std::mutex g_LifecycleMutex;
    SOCKET g_Socket = INVALID_SOCKET;
    std::thread g_Thread;
    std::atomic<bool> g_Running{ false };
    bool g_Initialized = false;
    bool g_HasLocalLobby = false;
    uint16_t g_Port = 47584;
    uint16_t g_BoundPort = 0;
    uint64_t g_JoinedLobbyID = 0;
    uint64_t g_LastBroadcastMs = 0;
    std::string g_LastJoinRoom;
    std::string g_LastJoinStatus = "idle";
    bool g_LastJoinAccepted = false;
    SteamOfflineServer::LobbyRecord g_LastJoinRecord;
    SteamOfflineServer::LobbyRecord g_LocalLobby;
    std::map<uint64_t, SteamOfflineServer::LobbyRecord> g_RemoteLobbies;
    std::map<uint64_t, bool> g_P2PSessionRequestSent;
    std::atomic<uint64_t> g_LastRelayPollMs{ 0 };

    struct PeerEndpoint
    {
        std::string Host;
        uint16_t Port = 0;
        uint64_t LastSeenMs = 0;
    };

    std::map<uint64_t, PeerEndpoint> g_PeerEndpoints;

    uint64_t NowMs()
    {
        return static_cast<uint64_t>(GetTickCount64());
    }

    std::string CleanValue(std::string value)
    {
        value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
        value.erase(std::remove(value.begin(), value.end(), '\n'), value.end());
        return value;
    }

    std::string NormalizeRoomCodeInternal(const std::string& roomCode)
    {
        std::string out;
        out.reserve(8);

        for (unsigned char c : roomCode)
        {
            if (!std::isalnum(c))
                continue;

            out.push_back(static_cast<char>(std::toupper(c)));

            if (out.size() >= 8)
                break;
        }

        return out;
    }

    std::string HashPasswordInternal(const std::string& password, const std::string& roomCode)
    {
        if (password.empty())
            return std::string();

        std::string normalizedRoom = NormalizeRoomCodeInternal(roomCode);
        std::string material = normalizedRoom + ":" + password + ":BREAKERSREVIVED";
        uint64_t hash = 1469598103934665603ull;

        for (unsigned char c : material)
        {
            hash ^= static_cast<uint64_t>(c);
            hash *= 1099511628211ull;
        }

        std::ostringstream ss;
        ss << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << hash;
        return ss.str();
    }

    std::string GenerateRoomCodeInternal(uint64_t lobbyID)
    {
        constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        uint64_t seed =
            lobbyID ^
            SteamIDManager::GetSteamID64() ^
            (NowMs() << 13) ^
            0x9E3779B97F4A7C15ull;

        std::string code;
        code.reserve(6);

        for (int i = 0; i < 6; ++i)
        {
            seed ^= seed >> 12;
            seed ^= seed << 25;
            seed ^= seed >> 27;
            seed *= 2685821657736338717ull;
            code.push_back(alphabet[seed % (ARRAYSIZE(alphabet) - 1)]);
        }

        return code;
    }

    bool IsSensitiveDataKey(const std::string& key)
    {
        return key == "password" ||
            key == "password_hash" ||
            key == "passwordHash";
    }

    std::string AddressToString(const sockaddr_in& address)
    {
        char host[64]{};
        inet_ntop(AF_INET, const_cast<in_addr*>(&address.sin_addr), host, sizeof(host));
        return host[0] ? host : "127.0.0.1";
    }

    uint16_t AddressPort(const sockaddr_in& address)
    {
        return ntohs(address.sin_port);
    }

    void StorePeerEndpointUnlocked(uint64_t steamID, const sockaddr_in& address)
    {
        if (steamID == 0)
            return;

        PeerEndpoint endpoint{};
        endpoint.Host = AddressToString(address);
        endpoint.Port = AddressPort(address);
        endpoint.LastSeenMs = NowMs();

        if (endpoint.Port == 0)
            return;

        g_PeerEndpoints[steamID] = endpoint;
    }

    bool FindPeerEndpointUnlocked(uint64_t steamID, std::string& outHost, uint16_t& outPort)
    {
        auto peerIt = g_PeerEndpoints.find(steamID);
        if (peerIt != g_PeerEndpoints.end())
        {
            outHost = peerIt->second.Host;
            outPort = peerIt->second.Port;
            return !outHost.empty() && outPort != 0;
        }

        for (const auto& item : g_RemoteLobbies)
        {
            const SteamOfflineServer::LobbyRecord& record = item.second;
            if (record.OwnerID == steamID)
            {
                outHost = record.Host;
                outPort = record.Port;
                return !outHost.empty() && outPort != 0;
            }
        }

        if (g_HasLocalLobby && g_LocalLobby.OwnerID == steamID)
        {
            outHost = g_LocalLobby.Host;
            outPort = g_LocalLobby.Port;
            return !outHost.empty() && outPort != 0;
        }

        return false;
    }

    uint16_t ConfiguredPort()
    {
        const SteamConfig::Config& config = SteamConfig::Get();
        return config.CustomServerPort > 0
            ? static_cast<uint16_t>(config.CustomServerPort)
            : static_cast<uint16_t>(47584);
    }

    std::string BuildMessage(const char* type, const SteamOfflineServer::LobbyRecord* lobby)
    {
        std::ostringstream ss;
        ss << kMagic << "\n";
        ss << "type=" << type << "\n";

        if (lobby)
        {
            ss << "lobby=" << lobby->LobbyID << "\n";
            ss << "owner=" << lobby->OwnerID << "\n";
            ss << "name=" << CleanValue(lobby->Name) << "\n";
            ss << "room=" << CleanValue(lobby->RoomCode) << "\n";
            ss << "host=" << CleanValue(lobby->Host) << "\n";
            ss << "port=" << lobby->Port << "\n";
            ss << "members=" << lobby->Members << "\n";
            ss << "max=" << lobby->MaxMembers << "\n";
            ss << "joinable=" << (lobby->Joinable ? 1 : 0) << "\n";
            ss << "password=" << (lobby->PasswordProtected ? 1 : 0) << "\n";

            for (const auto& item : lobby->Data)
            {
                if (!item.first.empty() && !IsSensitiveDataKey(item.first))
                    ss << "data." << CleanValue(item.first) << "=" << CleanValue(item.second) << "\n";
            }
        }

        return ss.str();
    }

    std::string BuildJoinMessage(const std::string& roomCode, const std::string& password)
    {
        const std::string normalizedRoom = NormalizeRoomCodeInternal(roomCode);

        std::ostringstream ss;
        ss << kMagic << "\n";
        ss << "type=join\n";
        ss << "room=" << normalizedRoom << "\n";
        ss << "steamid=" << SteamIDManager::GetSteamID64() << "\n";
        ss << "name=" << CleanValue(SteamIDManager::GetPersonaNameString()) << "\n";
        ss << "password_hash=" << HashPasswordInternal(password, normalizedRoom) << "\n";
        return ss.str();
    }

    std::string BuildJoinRejectMessage(const std::string& roomCode, const std::string& reason)
    {
        std::ostringstream ss;
        ss << kMagic << "\n";
        ss << "type=join_reject\n";
        ss << "room=" << NormalizeRoomCodeInternal(roomCode) << "\n";
        ss << "reason=" << CleanValue(reason) << "\n";
        return ss.str();
    }

    std::string BuildP2PPacket(CSteamID target, const void* data, uint32_t size, int channel)
    {
        std::string packet;
        packet.resize(32 + size);

        const CSteamID sender = SteamIDManager::GetLocalSteamID();

        memcpy(&packet[0], "BreakersP2P2", 7);
        memcpy(&packet[8], &sender, sizeof(CSteamID));
        memcpy(&packet[16], &target, sizeof(CSteamID));
        memcpy(&packet[24], &channel, sizeof(int));
        memcpy(&packet[28], &size, sizeof(uint32_t));

        if (data && size > 0)
            memcpy(&packet[32], data, size);

        return packet;
    }

    bool HandleP2PPacket(const char* data, int size, const sockaddr_in& from)
    {
        if (!data || size < 32)
            return false;

        if (memcmp(data, "BreakersP2P2", 7) != 0)
            return false;

        CSteamID sender = 0;
        CSteamID target = 0;
        int channel = 0;
        uint32_t payloadSize = 0;

        memcpy(&sender, data + 8, sizeof(CSteamID));
        memcpy(&target, data + 16, sizeof(CSteamID));
        memcpy(&channel, data + 24, sizeof(int));
        memcpy(&payloadSize, data + 28, sizeof(uint32_t));

        if (payloadSize > static_cast<uint32_t>(size - 32))
            return true;

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            StorePeerEndpointUnlocked(static_cast<uint64_t>(sender), from);
        }

        const CSteamID local = SteamIDManager::GetLocalSteamID();

        if (sender == local)
            return true;

        bool pushSessionRequest = false;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            uint64_t senderKey = static_cast<uint64_t>(sender);
            if (senderKey != 0 && g_P2PSessionRequestSent.find(senderKey) == g_P2PSessionRequestSent.end())
            {
                g_P2PSessionRequestSent[senderKey] = true;
                pushSessionRequest = true;
            }
        }

        if (pushSessionRequest)
            SteamCallbackManager::PushCallback(1202, &sender, sizeof(sender));

        uint64_t joinedLobby = 0;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            joinedLobby = g_JoinedLobbyID;
        }

        if (target != 0 && target != local && target != joinedLobby)
            return true;

        SteamP2PManager::PushIncoming(sender, data + 32, payloadSize, channel);
        return true;
    }

    bool ParseMessage(const char* data, int size, std::map<std::string, std::string>& values)
    {
        values.clear();

        if (!data || size <= 0)
            return false;

        std::string text(data, data + size);
        std::istringstream ss(text);
        std::string line;

        if (!std::getline(ss, line))
            return false;

        if (CleanValue(line) != kMagic)
            return false;

        while (std::getline(ss, line))
        {
            line = CleanValue(line);
            size_t equals = line.find('=');

            if (equals == std::string::npos)
                continue;

            values[line.substr(0, equals)] = line.substr(equals + 1);
        }

        return true;
    }

    uint64_t ReadU64(const std::map<std::string, std::string>& values, const char* key, uint64_t fallback = 0)
    {
        auto it = values.find(key);
        if (it == values.end())
            return fallback;

        return std::strtoull(it->second.c_str(), nullptr, 10);
    }

    int ReadInt(const std::map<std::string, std::string>& values, const char* key, int fallback = 0)
    {
        auto it = values.find(key);
        if (it == values.end())
            return fallback;

        return std::atoi(it->second.c_str());
    }

    std::string ReadString(const std::map<std::string, std::string>& values, const char* key, const char* fallback)
    {
        auto it = values.find(key);
        return it == values.end() ? std::string(fallback) : it->second;
    }

    SteamOfflineServer::LobbyRecord RecordFromMessage(
        const std::map<std::string, std::string>& values,
        const sockaddr_in& from)
    {
        SteamOfflineServer::LobbyRecord record{};
        record.LobbyID = ReadU64(values, "lobby");
        record.OwnerID = ReadU64(values, "owner");
        record.Name = ReadString(values, "name", "Breakers Revived LAN Lobby");
        record.RoomCode = NormalizeRoomCodeInternal(ReadString(values, "room", ""));
        record.Host = AddressToString(from);
        record.Port = static_cast<uint16_t>(ReadInt(values, "port", ConfiguredPort()));
        record.Members = ReadInt(values, "members", 1);
        record.MaxMembers = std::max(1, ReadInt(values, "max", 4));
        record.Joinable = ReadInt(values, "joinable", 1) != 0;
        record.PasswordProtected = ReadInt(values, "password", 0) != 0;
        record.LastSeenMs = NowMs();

        for (const auto& item : values)
        {
            const std::string prefix = "data.";

            if (item.first.rfind(prefix, 0) == 0)
                record.Data[item.first.substr(prefix.size())] = item.second;
        }

        record.Data["host"] = record.Host;
        record.Data["port"] = std::to_string(record.Port);
        record.Data["server"] = "offline-lan";
        record.Data["mode"] = "offline";
        record.Data["room"] = record.RoomCode;
        record.Data["room_code"] = record.RoomCode;
        record.Data["password_protected"] = record.PasswordProtected ? "1" : "0";

        return record;
    }

    bool RoomMatches(const SteamOfflineServer::LobbyRecord& record, const std::string& roomCode)
    {
        return !roomCode.empty() && NormalizeRoomCodeInternal(record.RoomCode) == roomCode;
    }

    bool LocalPasswordMatches(const std::string& requestedHash, const SteamOfflineServer::LobbyRecord& local)
    {
        if (!local.PasswordProtected)
            return true;

        if (requestedHash.empty() || local.PasswordHash.empty())
            return false;

        return requestedHash == local.PasswordHash;
    }

    bool FindRecordByRoomUnlocked(const std::string& roomCode, SteamOfflineServer::LobbyRecord& outRecord)
    {
        if (g_HasLocalLobby && RoomMatches(g_LocalLobby, roomCode))
        {
            outRecord = g_LocalLobby;
            return true;
        }

        for (const auto& item : g_RemoteLobbies)
        {
            if (RoomMatches(item.second, roomCode))
            {
                outRecord = item.second;
                return true;
            }
        }

        return false;
    }

    bool EnsureSocket()
    {
        if (g_Socket != INVALID_SOCKET)
            return true;

        if (!SteamConfig::Get().EnableEmbeddedMasterServer)
            return false;

        if (g_Initialized && !g_Running)
            return false;

        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);

        g_Port = ConfiguredPort();
        g_Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        if (g_Socket == INVALID_SOCKET)
        {
            Logger::Error("SteamOfflineServer: UDP socket creation failed");
            return false;
        }

        BOOL enabled = TRUE;
        setsockopt(g_Socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
        setsockopt(g_Socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled));

        u_long nonBlocking = 1;
        ioctlsocket(g_Socket, FIONBIO, &nonBlocking);

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(g_Port);

        if (bind(g_Socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
        {
            Logger::Error("SteamOfflineServer: UDP bind failed on port " + std::to_string(g_Port) + "; retrying client-mode ephemeral port");

            local.sin_port = htons(0);
            if (bind(g_Socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
            {
                Logger::Error("SteamOfflineServer: UDP bind failed in client mode");
                closesocket(g_Socket);
                g_Socket = INVALID_SOCKET;
                return false;
            }
        }

        sockaddr_in bound{};
        int boundSize = sizeof(bound);
        if (getsockname(g_Socket, reinterpret_cast<sockaddr*>(&bound), &boundSize) == 0)
            g_BoundPort = ntohs(bound.sin_port);
        else
            g_BoundPort = g_Port;

        Logger::Info(
            "SteamOfflineServer: embedded LAN/master discovery listening on UDP " +
            std::to_string(g_BoundPort) +
            " target=" +
            std::to_string(g_Port));
        return true;
    }

    void SendTo(const sockaddr_in& address, const std::string& message)
    {
        if (!EnsureSocket())
            return;

        sendto(
            g_Socket,
            message.c_str(),
            static_cast<int>(message.size()),
            0,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address));
    }

    void SendToHost(const std::string& host, uint16_t port, const std::string& message)
    {
        if (host.empty())
            return;

        std::string target = host == "localhost" ? "127.0.0.1" : host;

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);

        if (inet_pton(AF_INET, target.c_str(), &address.sin_addr) != 1)
            return;

        SendTo(address, message);
    }

    void Broadcast(const std::string& message)
    {
        sockaddr_in broadcast{};
        broadcast.sin_family = AF_INET;
        broadcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        broadcast.sin_port = htons(g_Port);
        SendTo(broadcast, message);

        const SteamConfig::Config& config = SteamConfig::Get();
        SendToHost(config.CustomServerHost, g_Port, message);
        SendToHost("26.255.255.255", g_Port, message);
        SendToHost("127.0.0.1", g_Port, message);
    }

    void StoreRemoteLobby(const SteamOfflineServer::LobbyRecord& record, const sockaddr_in* from = nullptr)
    {
        if (record.LobbyID == 0)
            return;

        std::lock_guard<std::mutex> lock(g_Mutex);

        if (g_HasLocalLobby && record.LobbyID == g_LocalLobby.LobbyID)
            return;

        g_RemoteLobbies[record.LobbyID] = record;

        if (from && record.OwnerID != 0)
            StorePeerEndpointUnlocked(record.OwnerID, *from);
    }

    void ReplyToDiscover(const sockaddr_in& from)
    {
        SteamOfflineServer::LobbyRecord local{};

        {
            std::lock_guard<std::mutex> lock(g_Mutex);

            if (!g_HasLocalLobby || !g_LocalLobby.Joinable)
                return;

            local = g_LocalLobby;
        }

        SendTo(from, BuildMessage("reply", &local));
    }

    void ReplyToJoinRequest(const std::map<std::string, std::string>& values, const sockaddr_in& from)
    {
        const std::string roomCode = NormalizeRoomCodeInternal(ReadString(values, "room", ""));
        const std::string passwordHash = ReadString(values, "password_hash", "");
        const uint64_t requesterSteamID = ReadU64(values, "steamid");
        SteamOfflineServer::LobbyRecord local{};
        bool accepted = false;
        std::string reason = "room not found";

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            StorePeerEndpointUnlocked(requesterSteamID, from);

            if (g_HasLocalLobby && RoomMatches(g_LocalLobby, roomCode))
            {
                local = g_LocalLobby;

                if (!g_LocalLobby.Joinable)
                {
                    reason = "room is closed";
                }
                else if (!LocalPasswordMatches(passwordHash, g_LocalLobby))
                {
                    reason = "bad password";
                }
                else
                {
                    accepted = true;

                    int maxMembers = std::max(1, g_LocalLobby.MaxMembers);
                    if (g_LocalLobby.Members < maxMembers)
                        ++g_LocalLobby.Members;

                    local = g_LocalLobby;
                }
            }
        }

        if (accepted)
        {
            SendTo(from, BuildMessage("join_ok", &local));
            Logger::Info("SteamOfflineServer accepted room join " + roomCode);
            return;
        }

        SendTo(from, BuildJoinRejectMessage(roomCode, reason));
        Logger::Info("SteamOfflineServer rejected room join " + roomCode + " reason=" + reason);
    }

    void HandleJoinAccepted(const std::map<std::string, std::string>& values, const sockaddr_in& from)
    {
        SteamOfflineServer::LobbyRecord record = RecordFromMessage(values, from);
        StoreRemoteLobby(record, &from);

        std::lock_guard<std::mutex> lock(g_Mutex);
        StorePeerEndpointUnlocked(record.OwnerID, from);

        if (!g_LastJoinRoom.empty() && RoomMatches(record, g_LastJoinRoom))
        {
            g_LastJoinAccepted = true;
            g_LastJoinRecord = record;
            g_LastJoinStatus = "accepted";
        }
    }

    void HandleJoinRejected(const std::map<std::string, std::string>& values)
    {
        const std::string roomCode = NormalizeRoomCodeInternal(ReadString(values, "room", ""));
        const std::string reason = ReadString(values, "reason", "rejected");

        std::lock_guard<std::mutex> lock(g_Mutex);

        if (g_LastJoinRoom.empty() || g_LastJoinRoom == roomCode)
            g_LastJoinStatus = reason;
    }

    void HandlePacket(const char* data, int size, const sockaddr_in& from)
    {
        if (HandleP2PPacket(data, size, from))
            return;

        std::map<std::string, std::string> values;

        if (!ParseMessage(data, size, values))
            return;

        std::string type = ReadString(values, "type", "");

        if (type == "discover")
        {
            ReplyToDiscover(from);
            return;
        }

        if (type == "join")
        {
            ReplyToJoinRequest(values, from);
            return;
        }

        if (type == "reply" || type == "advertise")
        {
            SteamOfflineServer::LobbyRecord record = RecordFromMessage(values, from);
            StoreRemoteLobby(record, &from);
            return;
        }

        if (type == "join_ok")
        {
            HandleJoinAccepted(values, from);
            return;
        }

        if (type == "join_reject")
        {
            HandleJoinRejected(values);
        }
    }

    void PollSocket()
    {
        if (!EnsureSocket())
            return;

        for (;;)
        {
            char buffer[kPacketMax]{};
            sockaddr_in from{};
            int fromLen = sizeof(from);

            int received = recvfrom(
                g_Socket,
                buffer,
                sizeof(buffer),
                0,
                reinterpret_cast<sockaddr*>(&from),
                &fromLen);

            if (received <= 0)
                break;

            HandlePacket(buffer, received, from);
        }
    }
    void PollRelayThrottled(uint64_t = 35) {}

    void BroadcastLocalLobby()
    {
        SteamOfflineServer::LobbyRecord local{};

        {
            std::lock_guard<std::mutex> lock(g_Mutex);

            if (!g_HasLocalLobby || !g_LocalLobby.Joinable)
                return;

            local = g_LocalLobby;
        }

        Broadcast(BuildMessage("advertise", &local));
    }

    void Worker()
    {
        while (g_Running)
        {
            PollSocket();

            uint64_t now = NowMs();

            if (now - g_LastBroadcastMs > 2000)
            {
                BroadcastLocalLobby();
                g_LastBroadcastMs = now;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }
}

namespace SteamOfflineServer
{
    bool Init()
    {
        std::lock_guard<std::mutex> lifecycle(g_LifecycleMutex);

        if (g_Initialized)
            return true;

        g_Running = true;

        if (!EnsureSocket())
        {
            g_Running = false;
            return false;
        }

        g_Initialized = true;
        g_Thread = std::thread(Worker);
        Logger::Info("SteamOfflineServer initialized fully offline");
        return true;
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lifecycle(g_LifecycleMutex);

        g_Running = false;

        if (g_Socket != INVALID_SOCKET)
        {
            closesocket(g_Socket);
            g_Socket = INVALID_SOCKET;
        }

        if (g_Thread.joinable())
            g_Thread.join();

        std::lock_guard<std::mutex> lock(g_Mutex);
        g_RemoteLobbies.clear();
        g_PeerEndpoints.clear();
        g_P2PSessionRequestSent.clear();
        g_HasLocalLobby = false;
        g_Initialized = false;
        g_BoundPort = 0;
    }

    void AdvertiseLobby(const LobbyRecord& record)
    {
        Init();

        LobbyRecord copy = record;
        copy.Port = copy.Port ? copy.Port : (g_BoundPort ? g_BoundPort : ConfiguredPort());
        copy.RoomCode = NormalizeRoomCodeInternal(copy.RoomCode.empty() ? copy.Data["room"] : copy.RoomCode);
        if (copy.RoomCode.empty())
            copy.RoomCode = GenerateRoomCodeInternal(copy.LobbyID);

        auto protectedIt = copy.Data.find("password_protected");
        if (protectedIt != copy.Data.end())
            copy.PasswordProtected = protectedIt->second == "1" || protectedIt->second == "true";

        auto hashIt = copy.Data.find("password_hash");
        if (hashIt != copy.Data.end())
            copy.PasswordHash = hashIt->second;

        copy.Data["room"] = copy.RoomCode;
        copy.Data["room_code"] = copy.RoomCode;
        copy.Data["password_protected"] = copy.PasswordProtected ? "1" : "0";
        copy.LastSeenMs = NowMs();

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            g_LocalLobby = copy;
            g_HasLocalLobby = copy.LobbyID != 0;
        }

        BroadcastLocalLobby();
    }

    void ClearLocalLobby(uint64_t lobbyID)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        if (lobbyID == 0 || g_LocalLobby.LobbyID == lobbyID)
            g_HasLocalLobby = false;
    }

    void MarkJoinedLobby(uint64_t lobbyID)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_JoinedLobbyID = lobbyID;
    }

    void Poll()
    {
        PollSocket();
        PollRelayThrottled();
    }

    int RefreshNow(int waitMs)
    {
        if (!Init())
            return static_cast<int>(GetLobbies().size());

        Broadcast(BuildMessage("discover", nullptr));

        const uint64_t until = NowMs() + static_cast<uint64_t>(std::max(0, waitMs));

        do
        {
            PollSocket();

            if (waitMs <= 0)
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        while (NowMs() < until);

        return static_cast<int>(GetLobbies().size());
    }

    int RefreshHost(const std::string& host, uint16_t port, int waitMs)
    {
        if (!Init())
            return static_cast<int>(GetLobbies().size());

        SendToHost(host, port ? port : ConfiguredPort(), BuildMessage("discover", nullptr));

        const uint64_t until = NowMs() + static_cast<uint64_t>(std::max(0, waitMs));

        do
        {
            PollSocket();

            if (waitMs <= 0)
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        while (NowMs() < until);

        return static_cast<int>(GetLobbies().size());
    }

    bool SendP2PPacket(CSteamID target, const void* data, uint32_t size, int channel)
    {
        if (!data || size == 0)
            return false;

        if (!Init())
            return false;

        const std::string packet = BuildP2PPacket(target, data, size, channel);

        std::string host;
        uint16_t port = 0;

        if (target != 0 && GetBestEndpoint(static_cast<uint64_t>(target), host, port) && !host.empty())
            SendToHost(host, port ? port : ConfiguredPort(), packet);

        Broadcast(packet);
        return true;
    }

    std::vector<LobbyRecord> GetLobbies()
    {
        std::vector<LobbyRecord> out;
        const uint64_t now = NowMs();

        std::lock_guard<std::mutex> lock(g_Mutex);

        if (g_HasLocalLobby)
            out.push_back(g_LocalLobby);

        for (auto it = g_RemoteLobbies.begin(); it != g_RemoteLobbies.end();)
        {
            if (now - it->second.LastSeenMs > 15000)
            {
                it = g_RemoteLobbies.erase(it);
                continue;
            }

            out.push_back(it->second);
            ++it;
        }

        return out;
    }

    bool GetLobby(uint64_t lobbyID, LobbyRecord& outRecord)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        if (g_HasLocalLobby && g_LocalLobby.LobbyID == lobbyID)
        {
            outRecord = g_LocalLobby;
            return true;
        }

        auto it = g_RemoteLobbies.find(lobbyID);

        if (it == g_RemoteLobbies.end())
            return false;

        outRecord = it->second;
        return true;
    }

    bool GetBestEndpoint(uint64_t remoteOrLobbyID, std::string& outHost, uint16_t& outPort)
    {
        LobbyRecord record{};

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            if (remoteOrLobbyID != 0 && FindPeerEndpointUnlocked(remoteOrLobbyID, outHost, outPort))
                return true;
        }

        if (remoteOrLobbyID != 0 && GetLobby(remoteOrLobbyID, record))
        {
            outHost = record.Host;
            outPort = record.Port;
            return true;
        }

        if (g_JoinedLobbyID != 0 && GetLobby(g_JoinedLobbyID, record))
        {
            outHost = record.Host;
            outPort = record.Port;
            return true;
        }

        const SteamConfig::Config& config = SteamConfig::Get();
        outHost = config.CustomServerHost.empty() ? "127.0.0.1" : config.CustomServerHost;
        outPort = config.CustomServerPort > 0 ? static_cast<uint16_t>(config.CustomServerPort) : static_cast<uint16_t>(47584);
        return !outHost.empty();
    }

    bool JoinByRoomCode(const std::string& roomCode, const std::string& password, LobbyRecord& outRecord, int waitMs)
    {
        outRecord = LobbyRecord{};

        if (!Init())
            return false;

        const std::string normalizedRoom = NormalizeRoomCodeInternal(roomCode);
        if (normalizedRoom.empty())
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            g_LastJoinStatus = "empty room code";
            return false;
        }

        std::string directHost;
        uint16_t directPort = 0;

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            g_LastJoinRoom = normalizedRoom;
            g_LastJoinAccepted = false;
            g_LastJoinRecord = LobbyRecord{};
            g_LastJoinStatus = "searching";

            LobbyRecord found{};
            if (FindRecordByRoomUnlocked(normalizedRoom, found))
            {
                directHost = found.Host;
                directPort = found.Port;

                if (!found.PasswordProtected)
                {
                    g_LastJoinAccepted = true;
                    g_LastJoinRecord = found;
                    g_LastJoinStatus = "accepted";
                    outRecord = found;
                    return true;
                }
            }
        }

        std::string joinMessage = BuildJoinMessage(normalizedRoom, password);
        Broadcast(joinMessage);
        if (!directHost.empty())
            SendToHost(directHost, directPort ? directPort : ConfiguredPort(), joinMessage);

        Broadcast(BuildMessage("discover", nullptr));

        const uint64_t until = NowMs() + static_cast<uint64_t>(std::max(0, waitMs));

        do
        {
            PollSocket();

            {
                std::lock_guard<std::mutex> lock(g_Mutex);
                if (g_LastJoinAccepted)
                {
                    outRecord = g_LastJoinRecord;
                    return true;
                }
            }

            if (waitMs <= 0)
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        while (NowMs() < until);

        {
            std::lock_guard<std::mutex> lock(g_Mutex);

            LobbyRecord found{};
            if (FindRecordByRoomUnlocked(normalizedRoom, found))
            {
                if (!found.PasswordProtected)
                {
                    g_LastJoinStatus = "accepted";
                    outRecord = found;
                    return true;
                }

                g_LastJoinStatus = "password required";
            }
            else if (g_LastJoinStatus == "searching")
            {
                g_LastJoinStatus = "room not found";
            }
        }

        return false;
    }

    bool JoinByAddress(const std::string& host, uint16_t port, const std::string& password, LobbyRecord& outRecord, int waitMs)
    {
        outRecord = LobbyRecord{};

        if (host.empty())
            return false;

        RefreshHost(host, port, 250);

        std::vector<LobbyRecord> records = GetLobbies();
        std::string target = host == "localhost" ? "127.0.0.1" : host;

        for (const LobbyRecord& record : records)
        {
            if (record.Host != target)
                continue;

            if (record.PasswordProtected)
                return JoinByRoomCode(record.RoomCode, password, outRecord, waitMs);

            outRecord = record;
            return true;
        }

        std::lock_guard<std::mutex> lock(g_Mutex);
        g_LastJoinStatus = "host not found";
        return false;
    }

    std::string GetLocalAddress()
    {
        char hostname[256]{};

        if (gethostname(hostname, sizeof(hostname)) == 0)
        {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;

            addrinfo* result = nullptr;

            if (getaddrinfo(hostname, nullptr, &hints, &result) == 0)
            {
                for (addrinfo* item = result; item; item = item->ai_next)
                {
                    auto* address = reinterpret_cast<sockaddr_in*>(item->ai_addr);
                    std::string host = AddressToString(*address);

                    if (host != "127.0.0.1")
                    {
                        freeaddrinfo(result);
                        return host;
                    }
                }

                freeaddrinfo(result);
            }
        }

        return "127.0.0.1";
    }

    std::string GenerateRoomCode(uint64_t lobbyID)
    {
        return GenerateRoomCodeInternal(lobbyID);
    }

    std::string NormalizeRoomCode(const std::string& roomCode)
    {
        return NormalizeRoomCodeInternal(roomCode);
    }

    std::string HashPassword(const std::string& password, const std::string& roomCode)
    {
        return HashPasswordInternal(password, roomCode);
    }

    std::string LastJoinStatus()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_LastJoinStatus;
    }

    bool IsRunning()
    {
        return g_Running && g_Socket != INVALID_SOCKET;
    }

    uint16_t GetPort()
    {
        return g_BoundPort ? g_BoundPort : (g_Port ? g_Port : ConfiguredPort());
    }
}

