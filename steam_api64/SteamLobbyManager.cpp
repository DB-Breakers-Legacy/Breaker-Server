#include "StdInc.h"
#include "SteamLobbyManager.h"
#include "SteamOfflineServer.h"
#include "SteamConfig.h"
#include "SteamDiagnostics.h"
#include "Logger.h"

namespace
{
    std::mutex g_Mutex;
    std::map<uint64_t, SteamLobbyManager::BreakersLobby> g_Lobbies;
    std::map<uint64_t, std::vector<SteamLobbyManager::ChatEntry>> g_ChatEntries;
    std::vector<uint64_t> g_LastLobbyList;
    uint64_t g_NextLobbyID = 50000;
    uint64_t g_CurrentLobbyID = 0;
    uint64_t g_LastLanHeartbeatMs = 0;
    std::string g_ReturnString;
    std::string g_LastJoinError = "idle";

    uint64_t Key(CSteamID id)
    {
        return SteamIDManager::ToUint64(id);
    }

    CSteamID MakeID(uint64_t id)
    {
        return SteamIDManager::FromUint64(id);
    }


    constexpr uint64_t kSteamUniversePublic = 1;
    constexpr uint64_t kSteamAccountTypeIndividual = 1;
    constexpr uint64_t kSteamAccountTypeChat = 8;
    constexpr uint64_t kSteamChatInstanceFlagLobby = 0x40000;
    constexpr uint64_t kSteamDesktopInstance = 1;

    bool IsLikelyLobbySteamID(uint64_t value)
    {
        if (value == 0 || value == UINT64_MAX)
            return false;

        const uint64_t universe = (value >> 56) & 0xffull;
        const uint64_t type = (value >> 52) & 0xfull;
        const uint64_t instance = (value >> 32) & 0xfffffull;
        const uint64_t account = value & 0xffffffffull;

        return universe != 0 &&
            universe <= 6 &&
            type == kSteamAccountTypeChat &&
            (instance & kSteamChatInstanceFlagLobby) != 0 &&
            account != 0;
    }

    uint64_t MakeLobbySteamIDFromSeed(uint64_t seed)
    {
        uint32_t account = static_cast<uint32_t>(seed ^ (seed >> 32) ^ 0x5A17C0DEu);
        if (account == 0)
            account = 1;

        return (kSteamUniversePublic << 56) |
            (kSteamAccountTypeChat << 52) |
            (kSteamChatInstanceFlagLobby << 32) |
            static_cast<uint64_t>(account);
    }

    bool IsLikelyUserSteamID(uint64_t value)
    {
        if (value == 0 || value == UINT64_MAX)
            return false;

        const uint64_t universe = (value >> 56) & 0xffull;
        const uint64_t type = (value >> 52) & 0xfull;
        const uint64_t instance = (value >> 32) & 0xfffffull;
        const uint64_t account = value & 0xffffffffull;

        return universe != 0 &&
            universe <= 6 &&
            type == kSteamAccountTypeIndividual &&
            instance == kSteamDesktopInstance &&
            account != 0;
    }

    uint64_t MakeUserSteamIDFromSeed(uint64_t seed)
    {
        uint32_t account = static_cast<uint32_t>(seed ^ (seed >> 32) ^ 0x51A7E5u);
        if (account == 0)
            account = 1;

        return (kSteamUniversePublic << 56) |
            (kSteamAccountTypeIndividual << 52) |
            (kSteamDesktopInstance << 32) |
            static_cast<uint64_t>(account);
    }

    uint64_t NormalizeUserSteamID(uint64_t raw)
    {
        if (IsLikelyUserSteamID(raw))
            return raw;

        return MakeUserSteamIDFromSeed(raw ? raw : SteamIDManager::GetSteamID64());
    }

    uint64_t NormalizeLobbySteamID(uint64_t raw)
    {
        if (IsLikelyLobbySteamID(raw))
            return raw;

        return MakeLobbySteamIDFromSeed(raw ? raw : static_cast<uint64_t>(GetTickCount64()));
    }

    uint64_t GenerateLobbyIDUnlocked()
    {
        const uint64_t steamID = SteamIDManager::GetSteamID64();
        const uint64_t tick = static_cast<uint64_t>(GetTickCount64());
        uint64_t seed = steamID ^ (tick << 7) ^ g_NextLobbyID++;
        uint64_t candidate = NormalizeLobbySteamID(seed);

        while (candidate == 0 || g_Lobbies.find(candidate) != g_Lobbies.end())
        {
            seed += 0x9E3779B97F4A7C15ull;
            candidate = NormalizeLobbySteamID(seed);
        }

        return candidate;
    }

    const SteamLobbyManager::BreakersLobby* GetLobbyUnlocked(CSteamID lobbyID)
    {
        auto it = g_Lobbies.find(Key(lobbyID));
        return it == g_Lobbies.end() ? nullptr : &it->second;
    }

    SteamLobbyManager::BreakersLobby* GetMutableLobbyUnlocked(CSteamID lobbyID)
    {
        auto it = g_Lobbies.find(Key(lobbyID));
        return it == g_Lobbies.end() ? nullptr : &it->second;
    }

    void EnsureLocalMember(SteamLobbyManager::BreakersLobby& lobby)
    {
        CSteamID local = SteamIDManager::GetLocalSteamID();
        uint64_t localKey = Key(local);

        for (const auto& member : lobby.Members)
        {
            if (Key(member) == localKey)
                return;
        }

        lobby.Members.push_back(local);
    }

    void EnsureDefaultMemberDataUnlocked(SteamLobbyManager::BreakersLobby& lobby, CSteamID member)
    {
        const uint64_t memberKey = Key(member);
        auto& data = lobby.MemberData[memberKey];

        if (data.find("LOBBY_MIC_DEVICE") == data.end())
            data["LOBBY_MIC_DEVICE"] = "0";

        if (data.find("mic_device") == data.end())
            data["mic_device"] = "0";

        if (data.find("voice") == data.end())
            data["voice"] = "0";

        if (data.find("name") == data.end())
        {
            if (memberKey == SteamIDManager::GetSteamID64())
                data["name"] = SteamIDManager::GetPersonaName();
            else
                data["name"] = "Breakers Player";
        }
    }

    void EnsureDefaultMemberDataUnlocked(SteamLobbyManager::BreakersLobby& lobby)
    {
        for (CSteamID member : lobby.Members)
            EnsureDefaultMemberDataUnlocked(lobby, member);
    }

    uint32_t IPv4ToHostOrder(const std::string& host)
    {
        in_addr address{};

        if (inet_pton(AF_INET, host.c_str(), &address) != 1)
            return 0x7F000001u;

        return ntohl(address.S_un.S_addr);
    }

    SteamOfflineServer::LobbyRecord BuildRecord(const SteamLobbyManager::BreakersLobby& lobby)
    {
        SteamOfflineServer::LobbyRecord record{};
        record.LobbyID = Key(lobby.LobbyID);
        record.OwnerID = Key(lobby.OwnerID);
        record.Name = "Breakers Revived Lobby";
        record.RoomCode = "";
        record.Host = lobby.Host.empty() ? SteamOfflineServer::GetLocalAddress() : lobby.Host;
        record.Port = lobby.GameServerPort ? lobby.GameServerPort : static_cast<uint16_t>(SteamConfig::Get().CustomServerPort);
        record.Members = static_cast<int>(lobby.Members.size());
        record.MaxMembers = lobby.MaxMembers;
        record.Joinable = lobby.Joinable;
        record.Data = lobby.Data;

        auto nameIt = record.Data.find("name");
        if (nameIt != record.Data.end() && !nameIt->second.empty())
            record.Name = nameIt->second;

        auto roomIt = record.Data.find("room");
        if (roomIt != record.Data.end())
            record.RoomCode = SteamOfflineServer::NormalizeRoomCode(roomIt->second);

        auto protectedIt = record.Data.find("password_protected");
        record.PasswordProtected = protectedIt != record.Data.end() &&
            (protectedIt->second == "1" || protectedIt->second == "true");

        auto hashIt = record.Data.find("password_hash");
        if (hashIt != record.Data.end())
            record.PasswordHash = hashIt->second;

        return record;
    }

    void AdvertiseIfLocalLobby(const SteamLobbyManager::BreakersLobby& lobby)
    {
        if (Key(lobby.OwnerID) != SteamIDManager::GetSteamID64())
            return;

        SteamOfflineServer::LobbyRecord record = BuildRecord(lobby);
        SteamOfflineServer::AdvertiseLobby(record);

    }

    void UpsertOfflineRecordUnlocked(const SteamOfflineServer::LobbyRecord& record)
    {
        if (record.LobbyID == 0)
            return;

        const uint64_t safeLobbyID = NormalizeLobbySteamID(record.LobbyID);
        const uint64_t safeOwnerID = NormalizeUserSteamID(record.OwnerID ? record.OwnerID : SteamIDManager::GetSteamID64());

        SteamLobbyManager::BreakersLobby& lobby = g_Lobbies[safeLobbyID];
        lobby.LobbyID = MakeID(safeLobbyID);
        lobby.OwnerID = MakeID(safeOwnerID);
        lobby.GameServerID = lobby.OwnerID;
        lobby.GameServerIP = IPv4ToHostOrder(record.Host);
        lobby.GameServerPort = record.Port;
        lobby.Host = record.Host;
        lobby.MaxMembers = record.MaxMembers > 0 ? record.MaxMembers : 4;
        lobby.Joinable = record.Joinable;
        lobby.PrivateLobby = false;
        lobby.Data = record.Data;
        lobby.Data["name"] = record.Name;
        lobby.Data["mode"] = "offline";
        lobby.Data["server"] = "offline-lan";
        lobby.Data["host"] = record.Host;
        lobby.Data["port"] = std::to_string(record.Port);
        lobby.Data["members"] = std::to_string(std::max(1, record.Members));
        lobby.Data["max_members"] = std::to_string(lobby.MaxMembers);
        lobby.Data["room"] = SteamOfflineServer::NormalizeRoomCode(record.RoomCode);
        lobby.Data["room_code"] = SteamOfflineServer::NormalizeRoomCode(record.RoomCode);
        lobby.Data["password_protected"] = record.PasswordProtected ? "1" : "0";

        if (lobby.Members.empty())
        {
            lobby.Members.push_back(lobby.OwnerID);

            if (record.Members > 1)
                lobby.Members.push_back(SteamIDManager::GetLocalSteamID());
        }

        int targetMembers = std::max(1, std::min(record.Members, lobby.MaxMembers));
        while (static_cast<int>(lobby.Members.size()) < targetMembers)
        {
            uint64_t syntheticID = NormalizeUserSteamID(safeOwnerID + static_cast<uint64_t>(lobby.Members.size()) + 1);
            lobby.Members.push_back(MakeID(syntheticID));
        }

        while (static_cast<int>(lobby.Members.size()) > targetMembers)
            lobby.Members.pop_back();

        EnsureDefaultMemberDataUnlocked(lobby);
    }

    std::vector<uint64_t> BuildVisibleLobbyListUnlocked()
    {
        std::vector<uint64_t> ids;

        for (const auto& item : g_Lobbies)
        {
            const auto& lobby = item.second;

            if (!lobby.Joinable)
                continue;

            if (lobby.MaxMembers <= 0)
                continue;

            if (lobby.Members.empty())
                continue;

            ids.push_back(item.first);
        }

        return ids;
    }

    bool IsPasswordProtected(const SteamLobbyManager::BreakersLobby& lobby)
    {
        auto it = lobby.Data.find("password_protected");
        return it != lobby.Data.end() && (it->second == "1" || it->second == "true");
    }

    std::string LobbyRoomCode(const SteamLobbyManager::BreakersLobby& lobby)
    {
        auto it = lobby.Data.find("room");
        if (it != lobby.Data.end())
            return SteamOfflineServer::NormalizeRoomCode(it->second);

        it = lobby.Data.find("room_code");
        return it == lobby.Data.end() ? std::string() : SteamOfflineServer::NormalizeRoomCode(it->second);
    }

    void SetLastJoinErrorUnlocked(const std::string& error)
    {
        g_LastJoinError = error.empty() ? "join failed" : error;
    }
}

namespace SteamLobbyManager
{
    bool Init()
    {
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            g_Lobbies.clear();
            g_ChatEntries.clear();
            g_LastLobbyList.clear();
            g_CurrentLobbyID = 0;
            g_NextLobbyID = 50000;
            g_LastLanHeartbeatMs = 0;
        }

        bool serverOk = SteamOfflineServer::Init();
        if (!serverOk)
            Logger::Error("SteamLobbyManager: offline discovery socket failed; lobby emulation will stay local-only");

        Logger::Info("SteamLobbyManager initialized");
        SteamDiagnostics::MarkOnline(
            "LobbyManagerInit",
            serverOk ? "offline discovery socket ready" : "offline discovery local-only");
        return true;
    }

    void Shutdown()
    {
        SteamOfflineServer::Shutdown();

        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Lobbies.clear();
        g_ChatEntries.clear();
        g_LastLobbyList.clear();
        g_CurrentLobbyID = 0;
        g_LastLanHeartbeatMs = 0;
    }

    void Tick()
    {
        const uint64_t now = static_cast<uint64_t>(GetTickCount64());
        BreakersLobby snapshot{};
        bool shouldAdvertise = false;

        {
            std::lock_guard<std::mutex> lock(g_Mutex);

            if (now - g_LastLanHeartbeatMs < 30000)
                return;

            g_LastLanHeartbeatMs = now;

            const BreakersLobby* lobby = GetLobbyUnlocked(MakeID(g_CurrentLobbyID));
            if (lobby && Key(lobby->OwnerID) == SteamIDManager::GetSteamID64())
            {
                snapshot = *lobby;
                shouldAdvertise = true;
            }
        }

        if (shouldAdvertise)
            AdvertiseIfLocalLobby(snapshot);
    }

    CSteamID CreateLobby()
    {
        return CreateLobby(2, 4);
    }

    CSteamID CreateLobby(int maxMembers)
    {
        return CreateLobby(2, maxMembers);
    }

    CSteamID CreateLobby(int type, int maxMembers)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        uint64_t lobbyID = GenerateLobbyIDUnlocked();

        BreakersLobby lobby{};
        lobby.LobbyID = MakeID(lobbyID);
        lobby.OwnerID = SteamIDManager::GetLocalSteamID();
        lobby.MaxMembers = maxMembers > 0 ? maxMembers : 4;
        lobby.Joinable = true;
        lobby.PrivateLobby = false;
        lobby.Host = SteamOfflineServer::GetLocalAddress();
        lobby.GameServerIP = IPv4ToHostOrder(lobby.Host);
        lobby.GameServerPort = SteamOfflineServer::GetPort();

        lobby.Data["name"] = "Breakers Revived Lobby";
        lobby.Data["mode"] = "offline";
        lobby.Data["server"] = "offline-lan";
        lobby.Data["host"] = lobby.Host;
        lobby.Data["port"] = std::to_string(lobby.GameServerPort);
        lobby.Data["members"] = std::to_string(lobby.MaxMembers);
        lobby.Data["room"] = SteamOfflineServer::GenerateRoomCode(lobbyID);
        lobby.Data["room_code"] = lobby.Data["room"];
        lobby.Data["password_protected"] = "0";
        lobby.Data["type"] = std::to_string(type);
        lobby.Data["search_visible"] = "1";

        EnsureLocalMember(lobby);
        EnsureDefaultMemberDataUnlocked(lobby);

        g_Lobbies[lobbyID] = lobby;
        g_CurrentLobbyID = lobbyID;
        g_LastLobbyList = BuildVisibleLobbyListUnlocked();

        AdvertiseIfLocalLobby(lobby);
        SteamOfflineServer::MarkJoinedLobby(lobbyID);

        Logger::Info("SteamLobbyManager created lobby " + std::to_string(static_cast<unsigned long long>(lobbyID)));
        SteamDiagnostics::MarkOnline(
            "CreateLobby",
            "id=" +
            std::to_string(static_cast<unsigned long long>(lobbyID)) +
            " host=" +
            lobby.Host +
            ":" +
            std::to_string(lobby.GameServerPort) +
            " room=" +
            lobby.Data["room"]);

        return lobby.LobbyID;
    }

    CSteamID CreateLobbyWithOptions(int type, int maxMembers, const std::string& name, const std::string& password)
    {
        CSteamID lobbyID = CreateLobby(type, maxMembers);
        const std::string cleanName = name.empty() ? "Breakers Revived Lobby" : name;
        SetData(lobbyID, "name", cleanName);

        std::string roomCode = GetData(lobbyID, "room");
        roomCode = SteamOfflineServer::NormalizeRoomCode(roomCode);

        if (roomCode.empty())
        {
            roomCode = SteamOfflineServer::GenerateRoomCode(Key(lobbyID));
            SetData(lobbyID, "room", roomCode);
            SetData(lobbyID, "room_code", roomCode);
        }

        if (password.empty())
        {
            SetData(lobbyID, "password_protected", "0");
            SetData(lobbyID, "password_hash", "");
        }
        else
        {
            SetData(lobbyID, "password_protected", "1");
            SetData(lobbyID, "password_hash", SteamOfflineServer::HashPassword(password, roomCode));
        }

        Logger::Info("SteamLobbyManager created option lobby room=" + roomCode);
        return lobbyID;
    }

    bool JoinLobbyWithPassword(CSteamID lobbyID, const std::string& password)
    {
        uint64_t id = Key(lobbyID);
        std::string roomCode;
        bool passwordProtected = false;
        bool revivedMaster = false;

        {
            std::lock_guard<std::mutex> lock(g_Mutex);

            if (id == 0)
                id = GenerateLobbyIDUnlocked();

            auto it = g_Lobbies.find(id);

            if (it == g_Lobbies.end())
            {
                SteamOfflineServer::LobbyRecord record{};

                if (SteamOfflineServer::GetLobby(id, record))
                {
                    UpsertOfflineRecordUnlocked(record);
                    it = g_Lobbies.find(id);
                }
            }

            if (it == g_Lobbies.end())
            {
                BreakersLobby lobby{};
                lobby.LobbyID = MakeID(id);
                lobby.OwnerID = SteamIDManager::GetLocalSteamID();
                lobby.MaxMembers = 4;
                lobby.Joinable = true;
                lobby.PrivateLobby = false;
                lobby.Host = "127.0.0.1";
                lobby.GameServerIP = 0x7F000001u;
                lobby.GameServerPort = SteamConfig::Get().CustomServerPort > 0
                    ? static_cast<uint16_t>(SteamConfig::Get().CustomServerPort)
                    : static_cast<uint16_t>(47584);
                lobby.Data["name"] = "Breakers Revived Lobby";
                lobby.Data["mode"] = "offline";
                lobby.Data["server"] = "offline-lan";
                lobby.Data["host"] = lobby.Host;
                lobby.Data["port"] = std::to_string(lobby.GameServerPort);
                lobby.Data["members"] = std::to_string(lobby.MaxMembers);
                lobby.Data["room"] = SteamOfflineServer::GenerateRoomCode(id);
                lobby.Data["room_code"] = lobby.Data["room"];
                lobby.Data["password_protected"] = "0";

                EnsureLocalMember(lobby);
                EnsureDefaultMemberDataUnlocked(lobby);
                g_Lobbies[id] = lobby;
                it = g_Lobbies.find(id);
            }

            if (it != g_Lobbies.end())
            {
                roomCode = LobbyRoomCode(it->second);
                passwordProtected = IsPasswordProtected(it->second);
            }
        }

        SteamOfflineServer::LobbyRecord accepted{};
        bool hasAccepted = false;

        if (passwordProtected)
        {
            const bool joined = SteamOfflineServer::JoinByRoomCode(roomCode, password, accepted, 1500);

            if (!joined)
            {
                std::lock_guard<std::mutex> lock(g_Mutex);
                SetLastJoinErrorUnlocked(SteamOfflineServer::LastJoinStatus());
                Logger::Error("SteamLobbyManager join denied room=" + roomCode + " reason=" + g_LastJoinError);
                SteamDiagnostics::MarkOnline("JoinLobbyDenied", "room=" + roomCode + " reason=" + g_LastJoinError);
                Logger::Flush();
                return false;
            }

            hasAccepted = accepted.LobbyID != 0;
        }

        std::lock_guard<std::mutex> lock(g_Mutex);

        if (hasAccepted)
        {
            UpsertOfflineRecordUnlocked(accepted);
            const uint64_t acceptedID = NormalizeLobbySteamID(accepted.LobbyID);
            if (g_Lobbies.find(acceptedID) != g_Lobbies.end())
                id = acceptedID;
        }

        auto it = g_Lobbies.find(id);
        if (it == g_Lobbies.end())
            return false;

        EnsureLocalMember(it->second);
        EnsureDefaultMemberDataUnlocked(it->second);
        g_CurrentLobbyID = id;
        SteamOfflineServer::MarkJoinedLobby(id);
        g_LastJoinError = "joined";

        Logger::Info("SteamLobbyManager joined lobby " + std::to_string(static_cast<unsigned long long>(id)));
        SteamDiagnostics::MarkOnline(
            "JoinLobby",
            "id=" +
            std::to_string(static_cast<unsigned long long>(id)) +
            " members=" +
            std::to_string(static_cast<unsigned long long>(it->second.Members.size())));
        Logger::Flush();
        return true;
    }

    bool JoinLobby(CSteamID lobbyID)
    {
        return JoinLobbyWithPassword(lobbyID, std::string());
    }

    bool JoinRoomCode(const std::string& roomCode, const std::string& password)
    {
        SteamOfflineServer::LobbyRecord record{};

        if (!SteamOfflineServer::JoinByRoomCode(roomCode, password, record, 1600))
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            SetLastJoinErrorUnlocked(SteamOfflineServer::LastJoinStatus());
            return false;

        }

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            UpsertOfflineRecordUnlocked(record);
        }

        return JoinLobbyWithPassword(MakeID(NormalizeLobbySteamID(record.LobbyID)), password);
    }

    bool JoinAddress(const std::string& host, uint16_t port, const std::string& password)
    {
        SteamOfflineServer::LobbyRecord record{};

        if (!SteamOfflineServer::JoinByAddress(host, port, password, record, 1600))
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            SetLastJoinErrorUnlocked(SteamOfflineServer::LastJoinStatus());
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            UpsertOfflineRecordUnlocked(record);
        }

        return JoinLobbyWithPassword(MakeID(NormalizeLobbySteamID(record.LobbyID)), password);
    }

    void LeaveLobby(CSteamID lobbyID)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        uint64_t id = Key(lobbyID);

        if (id == 0)
            id = g_CurrentLobbyID;

        auto it = g_Lobbies.find(id);

        if (it != g_Lobbies.end())
        {
            uint64_t localKey = SteamIDManager::GetSteamID64();
            const bool localOwnsLobby = Key(it->second.OwnerID) == localKey;

            auto& members = it->second.Members;

            members.erase(
                std::remove_if(
                    members.begin(),
                    members.end(),
                    [&](CSteamID member)
                    {
                        return Key(member) == localKey;
                    }),
                members.end());

            if (localOwnsLobby || members.empty())
            {
                if (localOwnsLobby)
            

                g_ChatEntries.erase(id);
                g_Lobbies.erase(it);
            }
        }

        if (g_CurrentLobbyID == id)
            g_CurrentLobbyID = 0;

        SteamOfflineServer::ClearLocalLobby(id);
        g_LastLobbyList = BuildVisibleLobbyListUnlocked();

        Logger::Info("SteamLobbyManager left lobby " + std::to_string(static_cast<unsigned long long>(id)));
        SteamDiagnostics::MarkOnline(
            "LeaveLobby",
            "id=" +
            std::to_string(static_cast<unsigned long long>(id)));
    }

    void LeaveCurrentLobby()
    {
        LeaveLobby(MakeID(g_CurrentLobbyID));
    }

    CSteamID GetCurrentLobby()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return MakeID(g_CurrentLobbyID);
    }

    uint64_t GetCurrentLobbyID()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_CurrentLobbyID;
    }

    std::string GetCurrentRoomCode()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        const BreakersLobby* lobby = GetLobbyUnlocked(MakeID(g_CurrentLobbyID));
        return lobby ? LobbyRoomCode(*lobby) : std::string();
    }

    std::string GetLastJoinError()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_LastJoinError;
    }

    const BreakersLobby* GetLobby(CSteamID lobbyID)
    {
        static thread_local BreakersLobby snapshot;
        std::lock_guard<std::mutex> lock(g_Mutex);

        const BreakersLobby* lobby = GetLobbyUnlocked(lobbyID);
        if (!lobby)
            return nullptr;

        snapshot = *lobby;
        return &snapshot;
    }

    BreakersLobby* GetMutableLobby(CSteamID lobbyID)
    {
        return GetMutableLobbyUnlocked(lobbyID);
    }

    int GetLobbyCount()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return static_cast<int>(BuildVisibleLobbyListUnlocked().size());
    }

    CSteamID GetLobbyByIndex(int index)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        if (index < 0)
            return 0;

        if (!g_LastLobbyList.empty())
        {
            if (index >= static_cast<int>(g_LastLobbyList.size()))
                return 0;

            auto it = g_Lobbies.find(g_LastLobbyList[index]);
            return it == g_Lobbies.end() ? 0 : it->second.LobbyID;
        }

        std::vector<uint64_t> visible = BuildVisibleLobbyListUnlocked();

        if (index >= static_cast<int>(visible.size()))
            return 0;

        auto it = g_Lobbies.find(visible[index]);
        return it == g_Lobbies.end() ? 0 : it->second.LobbyID;
    }

    int RefreshNetworkLobbies(int waitMs)
    {
        std::vector<SteamOfflineServer::LobbyRecord> records;

        SteamOfflineServer::RefreshNow(waitMs);
        records = SteamOfflineServer::GetLobbies();

        std::lock_guard<std::mutex> lock(g_Mutex);

        std::map<uint64_t, bool> seen;

        for (const auto& record : records)
        {
            if (record.LobbyID == 0)
                continue;

            const uint64_t safeRecordID = NormalizeLobbySteamID(record.LobbyID);
            auto existing = g_Lobbies.find(safeRecordID);
            seen[safeRecordID] = true;
            UpsertOfflineRecordUnlocked(record);
        }

        for (auto it = g_Lobbies.begin(); it != g_Lobbies.end();)
        {
            const bool keepCurrent = g_CurrentLobbyID != 0 && it->first == g_CurrentLobbyID;
            const bool seenInRefresh = seen.find(it->first) != seen.end();
            const bool invalid = it->second.MaxMembers <= 0 || it->second.Members.empty();

            if ((!keepCurrent && !seenInRefresh) || invalid)
            {
                g_ChatEntries.erase(it->first);
                it = g_Lobbies.erase(it);
                continue;
            }

            ++it;
        }

        g_LastLobbyList = BuildVisibleLobbyListUnlocked();

        Logger::Info("SteamLobbyManager refreshed offline lobby list count=" + std::to_string(g_LastLobbyList.size()));
        SteamDiagnostics::MarkOnline(
            "RefreshLobbies",
            "count=" +
            std::to_string(static_cast<unsigned long long>(g_LastLobbyList.size())));
        return static_cast<int>(g_LastLobbyList.size());
    }

    bool SetData(CSteamID lobbyID, const std::string& key, const std::string& value)
    {
        if (key.empty())
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        BreakersLobby* lobby = GetMutableLobbyUnlocked(lobbyID);

        if (!lobby)
            return false;

        lobby->Data[key] = value;
        AdvertiseIfLocalLobby(*lobby);
        const bool sensitive = key.find("password") != std::string::npos;
        SteamDiagnostics::MarkOnline("SetLobbyData", key + "=" + (sensitive ? "<hidden>" : value));
        return true;
    }

    bool SetLobbyData(CSteamID lobbyID, const std::string& key, const std::string& value)
    {
        return SetData(lobbyID, key, value);
    }

    bool DeleteData(CSteamID lobbyID, const std::string& key)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        BreakersLobby* lobby = GetMutableLobbyUnlocked(lobbyID);

        if (!lobby)
            return false;

        return lobby->Data.erase(key) > 0;
    }

    const char* GetData(CSteamID lobbyID, const std::string& key)
    {
        static thread_local std::string returnString;
        returnString.clear();

        std::lock_guard<std::mutex> lock(g_Mutex);
        const BreakersLobby* lobby = GetLobbyUnlocked(lobbyID);

        if (!lobby)
            return returnString.c_str();

        auto it = lobby->Data.find(key);

        if (it == lobby->Data.end())
            return returnString.c_str();

        returnString = it->second;
        return returnString.c_str();
    }

    const char* GetLobbyData(CSteamID lobbyID, const std::string& key)
    {
        return GetData(lobbyID, key);
    }

    int GetDataCount(CSteamID lobbyID)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        const BreakersLobby* lobby = GetLobbyUnlocked(lobbyID);
        return lobby ? static_cast<int>(lobby->Data.size()) : 0;
    }

    bool GetDataByIndex(CSteamID lobbyID, int index, std::string& key, std::string& value)
    {
        key.clear();
        value.clear();

        std::lock_guard<std::mutex> lock(g_Mutex);
        const BreakersLobby* lobby = GetLobbyUnlocked(lobbyID);

        if (!lobby || index < 0 || index >= static_cast<int>(lobby->Data.size()))
            return false;

        auto it = lobby->Data.begin();
        std::advance(it, index);
        key = it->first;
        value = it->second;
        return true;
    }

    int GetMemberCount(CSteamID lobbyID)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        const BreakersLobby* lobby = GetLobbyUnlocked(lobbyID);
        if (!lobby)
            return 0;

        if (g_CurrentLobbyID == 0 || Key(lobbyID) != g_CurrentLobbyID)
            return 0;

        return std::max(1, static_cast<int>(lobby->Members.size()));
    }

    int GetNumLobbyMembers(CSteamID lobbyID)
    {
        return GetMemberCount(lobbyID);
    }

    CSteamID GetMemberByIndex(CSteamID lobbyID, int index)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        const BreakersLobby* lobby = GetLobbyUnlocked(lobbyID);

        if (!lobby)
            return index == 0 ? SteamIDManager::GetLocalSteamID() : 0;

        if (index < 0 || index >= static_cast<int>(lobby->Members.size()))
            return 0;

        return lobby->Members[index];
    }

    CSteamID GetLobbyMemberByIndex(CSteamID lobbyID, int index)
    {
        return GetMemberByIndex(lobbyID, index);
    }

    void SetMemberData(CSteamID lobbyID, CSteamID user, const std::string& key, const std::string& value)
    {
        if (key.empty())
            return;

        std::lock_guard<std::mutex> lock(g_Mutex);
        BreakersLobby* lobby = GetMutableLobbyUnlocked(lobbyID);

        if (!lobby)
            return;

        lobby->MemberData[Key(user)][key] = value;
    }

    const char* GetMemberData(CSteamID lobbyID, CSteamID user, const std::string& key)
    {
        static thread_local std::string returnString;
        returnString.clear();

        std::lock_guard<std::mutex> lock(g_Mutex);
        const BreakersLobby* lobby = GetLobbyUnlocked(lobbyID);

        if (!lobby)
            return returnString.c_str();

        auto knownMember = std::find_if(
            lobby->Members.begin(),
            lobby->Members.end(),
            [&](CSteamID member)
            {
                return Key(member) == Key(user);
            });

        if (knownMember == lobby->Members.end())
        {
            if (key == "LOBBY_MIC_DEVICE" || key == "mic_device" || key == "voice")
            {
                returnString = "0";
                return returnString.c_str();
            }

            if (key == "name")
            {
                returnString = "Breakers Player";
                return returnString.c_str();
            }

            return returnString.c_str();
        }

        auto userIt = lobby->MemberData.find(Key(user));

        if (userIt == lobby->MemberData.end())
        {
            if (key == "LOBBY_MIC_DEVICE" || key == "mic_device" || key == "voice")
            {
                returnString = "0";
                return returnString.c_str();
            }

            if (key == "name")
            {
                returnString = Key(user) == SteamIDManager::GetSteamID64()
                    ? SteamIDManager::GetPersonaName()
                    : "Breakers Player";
                return returnString.c_str();
            }

            return returnString.c_str();
        }

        auto valueIt = userIt->second.find(key);

        if (valueIt == userIt->second.end())
        {
            if (key == "LOBBY_MIC_DEVICE" || key == "mic_device" || key == "voice")
            {
                returnString = "0";
                return returnString.c_str();
            }

            if (key == "name")
            {
                returnString = Key(user) == SteamIDManager::GetSteamID64()
                    ? SteamIDManager::GetPersonaName()
                    : "Breakers Player";
                return returnString.c_str();
            }

            return returnString.c_str();
        }

        returnString = valueIt->second;
        return returnString.c_str();
    }

    bool SetMemberLimit(CSteamID lobbyID, int maxMembers)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        BreakersLobby* lobby = GetMutableLobbyUnlocked(lobbyID);

        if (!lobby || maxMembers <= 0)
            return false;

        lobby->MaxMembers = maxMembers;
        lobby->Data["members"] = std::to_string(maxMembers);
        AdvertiseIfLocalLobby(*lobby);
        return true;
    }

    int GetMemberLimit(CSteamID lobbyID)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        const BreakersLobby* lobby = GetLobbyUnlocked(lobbyID);
        return lobby ? lobby->MaxMembers : 4;
    }

    bool SetJoinable(CSteamID lobbyID, bool joinable)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        BreakersLobby* lobby = GetMutableLobbyUnlocked(lobbyID);

        if (!lobby)
            return false;

        lobby->Joinable = joinable;
        AdvertiseIfLocalLobby(*lobby);
        return true;
    }

    bool SetType(CSteamID lobbyID, int lobbyType)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        BreakersLobby* lobby = GetMutableLobbyUnlocked(lobbyID);

        if (!lobby)
            return false;

        lobby->PrivateLobby = false;
        lobby->Data["type"] = std::to_string(lobbyType);
        lobby->Data["search_visible"] = "1";
        AdvertiseIfLocalLobby(*lobby);
        return true;
    }

    CSteamID GetOwner(CSteamID lobbyID)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        const BreakersLobby* lobby = GetLobbyUnlocked(lobbyID);
        return lobby ? lobby->OwnerID : SteamIDManager::GetLocalSteamID();
    }

    bool SetOwner(CSteamID lobbyID, CSteamID owner)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        BreakersLobby* lobby = GetMutableLobbyUnlocked(lobbyID);

        if (!lobby || owner == 0)
            return false;

        lobby->OwnerID = owner;
        AdvertiseIfLocalLobby(*lobby);
        return true;
    }

    void SetGameServer(CSteamID lobbyID, uint32_t ip, uint16_t port, CSteamID serverID)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        BreakersLobby* lobby = GetMutableLobbyUnlocked(lobbyID);

        if (!lobby)
            return;

        lobby->GameServerIP = ip;
        lobby->GameServerPort = port;
        lobby->GameServerID = serverID;
        AdvertiseIfLocalLobby(*lobby);
    }

    bool GetGameServer(CSteamID lobbyID, uint32_t* ip, uint16_t* port, CSteamID* serverID)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        const BreakersLobby* lobby = GetLobbyUnlocked(lobbyID);

        if (!lobby)
        {
            if (ip) *ip = 0x7F000001u;
            if (port) *port = SteamConfig::Get().CustomServerPort > 0
                ? static_cast<uint16_t>(SteamConfig::Get().CustomServerPort)
                : static_cast<uint16_t>(47584);
            if (serverID) *serverID = lobbyID ? lobbyID : SteamIDManager::GetLocalSteamID();
            return true;
        }

        if (ip) *ip = lobby->GameServerIP;
        if (port) *port = lobby->GameServerPort;
        if (serverID) *serverID = lobby->GameServerID ? lobby->GameServerID : lobby->LobbyID;
        return true;
    }

    int AddChatEntry(CSteamID lobbyID, CSteamID user, const void* data, int size, int type)
    {
        if (!data || size <= 0)
            return -1;

        uint64_t id = Key(lobbyID);
        std::lock_guard<std::mutex> lock(g_Mutex);

        ChatEntry entry{};
        entry.User = user;
        entry.Type = type;
        entry.Data.resize(static_cast<size_t>(size));
        memcpy(entry.Data.data(), data, static_cast<size_t>(size));

        auto& list = g_ChatEntries[id];
        list.push_back(entry);
        return static_cast<int>(list.size()) - 1;
    }

    int GetChatEntry(CSteamID lobbyID, int index, CSteamID* user, void* data, int dataSize, int* type)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        auto it = g_ChatEntries.find(Key(lobbyID));

        if (it == g_ChatEntries.end() || index < 0 || index >= static_cast<int>(it->second.size()))
            return 0;

        const ChatEntry& entry = it->second[index];

        if (user) *user = entry.User;
        if (type) *type = entry.Type;

        int copySize = std::min(dataSize, static_cast<int>(entry.Data.size()));

        if (data && copySize > 0)
            memcpy(data, entry.Data.data(), static_cast<size_t>(copySize));

        return static_cast<int>(entry.Data.size());
    }

    const std::map<uint64_t, BreakersLobby>& GetLobbies()
    {
        static thread_local std::map<uint64_t, BreakersLobby> snapshot;
        std::lock_guard<std::mutex> lock(g_Mutex);
        snapshot = g_Lobbies;
        return snapshot;
    }
}

