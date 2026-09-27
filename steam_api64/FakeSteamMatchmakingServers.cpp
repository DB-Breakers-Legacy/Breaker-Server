#include "StdInc.h"
#include "FakeSteamInterfaces.h"
#include "SteamOfflineServer.h"
#include "SteamVersionLogger.h"
#include "Logger.h"

namespace
{
    constexpr AppId_t kBreakersAppId = 1276760;
    using HServerListRequest = void*;
    using HServerQuery = int;
    constexpr HServerQuery HSERVERQUERY_INVALID = -1;

    enum EMatchMakingServerResponse
    {
        eServerResponded = 0,
        eServerFailedToRespond,
        eNoServersListedOnMasterServer
    };

    struct MatchMakingKeyValuePair_t
    {
        char m_szKey[256];
        char m_szValue[256];
    };

    class servernetadr_t
    {
    public:
        void Init(uint32_t ip, uint16_t queryPort, uint16_t connectionPort)
        {
            m_unIP = ip;
            m_usQueryPort = queryPort;
            m_usConnectionPort = connectionPort;
        }

        uint16_t GetQueryPort() const { return m_usQueryPort; }
        void SetQueryPort(uint16_t port) { m_usQueryPort = port; }
        uint16_t GetConnectionPort() const { return m_usConnectionPort; }
        void SetConnectionPort(uint16_t port) { m_usConnectionPort = port; }
        uint32_t GetIP() const { return m_unIP; }
        void SetIP(uint32_t ip) { m_unIP = ip; }

        const char* GetConnectionAddressString() const { return ToString(m_unIP, m_usConnectionPort); }
        const char* GetQueryAddressString() const { return ToString(m_unIP, m_usQueryPort); }

    private:
        const char* ToString(uint32_t ip, uint16_t port) const
        {
            static char buffers[4][64]{};
            static int index = 0;
            unsigned char* bytes = reinterpret_cast<unsigned char*>(&ip);
            sprintf_s(
                buffers[index],
                "%u.%u.%u.%u:%u",
                static_cast<unsigned>(bytes[3]),
                static_cast<unsigned>(bytes[2]),
                static_cast<unsigned>(bytes[1]),
                static_cast<unsigned>(bytes[0]),
                static_cast<unsigned>(port));
            const char* out = buffers[index];
            index = (index + 1) % ARRAYSIZE(buffers);
            return out;
        }

        uint16_t m_usConnectionPort = 0;
        uint16_t m_usQueryPort = 0;
        uint32_t m_unIP = 0;
    };

    class gameserveritem_t
    {
    public:
        gameserveritem_t()
        {
        }

        const char* GetName() const
        {
            return m_szServerName[0] ? m_szServerName : m_NetAdr.GetConnectionAddressString();
        }

        void SetName(const char* name)
        {
            strncpy_s(m_szServerName, name ? name : "Breakers Revived Offline Server", _TRUNCATE);
        }

        servernetadr_t m_NetAdr;
        int m_nPing = 1;
        bool m_bHadSuccessfulResponse = true;
        bool m_bDoNotRefresh = false;
        char m_szGameDir[32]{};
        char m_szMap[32]{};
        char m_szGameDescription[64]{};
        uint32_t m_nAppID = kBreakersAppId;
        int m_nPlayers = 1;
        int m_nMaxPlayers = 4;
        int m_nBotPlayers = 0;
        bool m_bPassword = false;
        bool m_bSecure = false;
        uint32_t m_ulTimeLastPlayed = 0;
        int m_nServerVersion = 1;

    private:
        char m_szServerName[64]{};

    public:
        char m_szGameTags[128]{};
        CSteamID m_steamID = 0;
    };

    class ISteamMatchmakingServerListResponse
    {
    public:
        virtual void ServerResponded(HServerListRequest request, int server) = 0;
        virtual void ServerFailedToRespond(HServerListRequest request, int server) = 0;
        virtual void RefreshComplete(HServerListRequest request, EMatchMakingServerResponse response) = 0;
    };

    class ISteamMatchmakingPingResponse
    {
    public:
        virtual void ServerResponded(gameserveritem_t& server) = 0;
        virtual void ServerFailedToRespond() = 0;
    };

    class ISteamMatchmakingPlayersResponse
    {
    public:
        virtual void AddPlayerToList(const char* name, int score, float timePlayed) = 0;
        virtual void PlayersFailedToRespond() = 0;
        virtual void PlayersRefreshComplete() = 0;
    };

    class ISteamMatchmakingRulesResponse
    {
    public:
        virtual void RulesResponded(const char* rule, const char* value) = 0;
        virtual void RulesFailedToRespond() = 0;
        virtual void RulesRefreshComplete() = 0;
    };

    struct ServerRequest
    {
        HServerListRequest Handle = nullptr;
        ISteamMatchmakingServerListResponse* Response = nullptr;
        bool Refreshing = false;
        std::vector<gameserveritem_t> Servers;
    };

    std::mutex g_Mutex;
    uintptr_t g_NextRequest = 100;
    HServerQuery g_NextQuery = 1;
    std::map<HServerListRequest, ServerRequest> g_Requests;

    void TraceMatchmakingServers(const char* functionName, const std::string& detail = std::string())
    {
        SteamVersionLogger::LogCall("SteamMatchmakingServers", functionName, detail);
    }

    bool IsReadableMemory(const void* ptr, size_t size)
    {
        if (!ptr || size == 0)
            return false;

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
            return false;

        if (mbi.State != MEM_COMMIT)
            return false;

        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            return false;

        const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
        const uintptr_t end = start + size;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        return end >= start && end <= regionEnd;
    }

    bool HasReadableVTable(void* object, size_t slots)
    {
        if (!IsReadableMemory(object, sizeof(void*)))
            return false;

        void* const* vtable = *reinterpret_cast<void***>(object);
        if (!IsReadableMemory(vtable, sizeof(void*) * slots))
            return false;

        for (size_t i = 0; i < slots; ++i)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!vtable[i] || VirtualQuery(vtable[i], &mbi, sizeof(mbi)) == 0)
                return false;

            const DWORD protect = mbi.Protect & 0xff;
            if (protect != PAGE_EXECUTE &&
                protect != PAGE_EXECUTE_READ &&
                protect != PAGE_EXECUTE_READWRITE &&
                protect != PAGE_EXECUTE_WRITECOPY)
            {
                return false;
            }
        }

        return true;
    }

    uint32_t IPv4ToHostOrder(const std::string& host)
    {
        in_addr address{};

        if (inet_pton(AF_INET, host.c_str(), &address) != 1)
            return 0x7F000001u;

        return ntohl(address.S_un.S_addr);
    }

    gameserveritem_t MakeServerItem(const SteamOfflineServer::LobbyRecord& lobby)
    {
        gameserveritem_t item{};
        item.m_NetAdr.Init(IPv4ToHostOrder(lobby.Host), lobby.Port, lobby.Port);
        item.m_nPing = lobby.Host == "127.0.0.1" ? 1 : 35;
        item.m_bHadSuccessfulResponse = true;
        item.m_bDoNotRefresh = false;
        item.m_nAppID = kBreakersAppId;
        item.m_nPlayers = std::max(1, lobby.Members);
        item.m_nMaxPlayers = std::max(1, lobby.MaxMembers);
        item.m_bPassword = false;
        item.m_bSecure = false;
        item.m_nServerVersion = 1;
        item.m_steamID = lobby.OwnerID ? lobby.OwnerID : lobby.LobbyID;
        item.SetName(lobby.Name.c_str());
        strncpy_s(item.m_szGameDir, "breakersrevived", _TRUNCATE);
        strncpy_s(item.m_szMap, "offline", _TRUNCATE);
        strncpy_s(item.m_szGameDescription, "DRAGON BALL: THE BREAKERS Revived Offline", _TRUNCATE);
        strncpy_s(item.m_szGameTags, "offline,lan,radmin,breakersrevived", _TRUNCATE);
        return item;
    }

    gameserveritem_t MakeServerItem(uint32_t ip, uint16_t port)
    {
        SteamOfflineServer::LobbyRecord lobby{};
        lobby.LobbyID = static_cast<uint64_t>(ip) << 16 | port;
        lobby.OwnerID = lobby.LobbyID;
        lobby.Name = "Breakers Revived Direct Server";
        lobby.Host = "127.0.0.1";
        lobby.Port = port;
        lobby.Members = 1;
        lobby.MaxMembers = 4;

        gameserveritem_t item = MakeServerItem(lobby);
        item.m_NetAdr.Init(ip ? ip : 0x7F000001u, port, port);
        return item;
    }

    void NotifyListResponse(HServerListRequest handle)
    {
        ISteamMatchmakingServerListResponse* response = nullptr;
        int count = 0;

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            auto it = g_Requests.find(handle);

            if (it == g_Requests.end())
                return;

            response = it->second.Response;
            count = static_cast<int>(it->second.Servers.size());
            it->second.Refreshing = false;
        }

        if (!response || !HasReadableVTable(response, 3))
            return;

        for (int i = 0; i < count; ++i)
            response->ServerResponded(handle, i);

        response->RefreshComplete(
            handle,
            count > 0 ? eServerResponded : eNoServersListedOnMasterServer);
    }

    HServerListRequest CreateListRequest(
        const char* source,
        AppId_t appID,
        ISteamMatchmakingServerListResponse* response)
    {
        TraceMatchmakingServers(source, "app=" + std::to_string(appID));
        NSR_UNUSED(appID);

        SteamOfflineServer::RefreshNow(15);
        std::vector<SteamOfflineServer::LobbyRecord> lobbies = SteamOfflineServer::GetLobbies();

        ServerRequest request{};
        request.Handle = reinterpret_cast<HServerListRequest>(++g_NextRequest);
        request.Response = response;
        request.Refreshing = true;

        for (const auto& lobby : lobbies)
        {
            if (lobby.Joinable)
                request.Servers.push_back(MakeServerItem(lobby));
        }

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            g_Requests[request.Handle] = request;
        }

        Logger::Info(
            std::string("SteamMatchmakingServers::") +
            source +
            " offline count=" +
            std::to_string(request.Servers.size()));

        NotifyListResponse(request.Handle);
        return request.Handle;
    }

    class FakeSteamMatchmakingServers final
    {
    public:
        virtual HServerListRequest RequestInternetServerList(AppId_t appID, MatchMakingKeyValuePair_t** filters, uint32_t filterCount, ISteamMatchmakingServerListResponse* response)
        {
            NSR_UNUSED(filters);
            NSR_UNUSED(filterCount);
            return CreateListRequest("RequestInternetServerList", appID, response);
        }

        virtual HServerListRequest RequestLANServerList(AppId_t appID, ISteamMatchmakingServerListResponse* response)
        {
            return CreateListRequest("RequestLANServerList", appID, response);
        }

        virtual HServerListRequest RequestFriendsServerList(AppId_t appID, MatchMakingKeyValuePair_t** filters, uint32_t filterCount, ISteamMatchmakingServerListResponse* response)
        {
            NSR_UNUSED(filters);
            NSR_UNUSED(filterCount);
            return CreateListRequest("RequestFriendsServerList", appID, response);
        }

        virtual HServerListRequest RequestFavoritesServerList(AppId_t appID, MatchMakingKeyValuePair_t** filters, uint32_t filterCount, ISteamMatchmakingServerListResponse* response)
        {
            NSR_UNUSED(filters);
            NSR_UNUSED(filterCount);
            return CreateListRequest("RequestFavoritesServerList", appID, response);
        }

        virtual HServerListRequest RequestHistoryServerList(AppId_t appID, MatchMakingKeyValuePair_t** filters, uint32_t filterCount, ISteamMatchmakingServerListResponse* response)
        {
            NSR_UNUSED(filters);
            NSR_UNUSED(filterCount);
            return CreateListRequest("RequestHistoryServerList", appID, response);
        }

        virtual HServerListRequest RequestSpectatorServerList(AppId_t appID, MatchMakingKeyValuePair_t** filters, uint32_t filterCount, ISteamMatchmakingServerListResponse* response)
        {
            NSR_UNUSED(filters);
            NSR_UNUSED(filterCount);
            return CreateListRequest("RequestSpectatorServerList", appID, response);
        }

        virtual void ReleaseRequest(HServerListRequest request)
        {
            TraceMatchmakingServers("ReleaseRequest");
            std::lock_guard<std::mutex> lock(g_Mutex);
            g_Requests.erase(request);
            Logger::Info("SteamMatchmakingServers::ReleaseRequest");
        }

        virtual gameserveritem_t* GetServerDetails(HServerListRequest request, int server)
        {
            TraceMatchmakingServers("GetServerDetails", "index=" + std::to_string(server));
            std::lock_guard<std::mutex> lock(g_Mutex);
            auto it = g_Requests.find(request);

            if (it == g_Requests.end())
                return nullptr;

            if (server < 0 || server >= static_cast<int>(it->second.Servers.size()))
                return nullptr;

            return &it->second.Servers[server];
        }

        virtual void CancelQuery(HServerListRequest request)
        {
            TraceMatchmakingServers("CancelQuery");
            std::lock_guard<std::mutex> lock(g_Mutex);
            auto it = g_Requests.find(request);

            if (it != g_Requests.end())
                it->second.Refreshing = false;
        }

        virtual void RefreshQuery(HServerListRequest request)
        {
            TraceMatchmakingServers("RefreshQuery");
            std::vector<SteamOfflineServer::LobbyRecord> lobbies;
            SteamOfflineServer::RefreshNow(15);
            lobbies = SteamOfflineServer::GetLobbies();

            {
                std::lock_guard<std::mutex> lock(g_Mutex);
                auto it = g_Requests.find(request);

                if (it == g_Requests.end())
                    return;

                it->second.Servers.clear();

                for (const auto& lobby : lobbies)
                {
                    if (lobby.Joinable)
                        it->second.Servers.push_back(MakeServerItem(lobby));
                }

                it->second.Refreshing = true;
            }

            NotifyListResponse(request);
        }

        virtual bool IsRefreshing(HServerListRequest request)
        {
            TraceMatchmakingServers("IsRefreshing");
            std::lock_guard<std::mutex> lock(g_Mutex);
            auto it = g_Requests.find(request);
            return it != g_Requests.end() && it->second.Refreshing;
        }

        virtual int GetServerCount(HServerListRequest request)
        {
            TraceMatchmakingServers("GetServerCount");
            std::lock_guard<std::mutex> lock(g_Mutex);
            auto it = g_Requests.find(request);
            return it == g_Requests.end() ? 0 : static_cast<int>(it->second.Servers.size());
        }

        virtual void RefreshServer(HServerListRequest request, int server)
        {
            TraceMatchmakingServers("RefreshServer", "index=" + std::to_string(server));
            NSR_UNUSED(server);
            NotifyListResponse(request);
        }

        virtual HServerQuery PingServer(uint32_t ip, uint16_t port, ISteamMatchmakingPingResponse* response)
        {
            TraceMatchmakingServers("PingServer", "port=" + std::to_string(port));
            HServerQuery query = ++g_NextQuery;

            if (response && HasReadableVTable(response, 2))
            {
                gameserveritem_t item = MakeServerItem(ip, port);
                response->ServerResponded(item);
            }

            return query;
        }

        virtual HServerQuery PlayerDetails(uint32_t ip, uint16_t port, ISteamMatchmakingPlayersResponse* response)
        {
            TraceMatchmakingServers("PlayerDetails", "port=" + std::to_string(port));
            NSR_UNUSED(ip);
            NSR_UNUSED(port);

            HServerQuery query = ++g_NextQuery;

            if (response && HasReadableVTable(response, 3))
            {
                response->AddPlayerToList("BreakersRevived", 0, 0.0f);
                response->PlayersRefreshComplete();
            }

            return query;
        }

        virtual HServerQuery ServerRules(uint32_t ip, uint16_t port, ISteamMatchmakingRulesResponse* response)
        {
            TraceMatchmakingServers("ServerRules", "port=" + std::to_string(port));
            NSR_UNUSED(ip);
            NSR_UNUSED(port);

            HServerQuery query = ++g_NextQuery;

            if (response && HasReadableVTable(response, 3))
            {
                response->RulesResponded("breakersrevived", "offline");
                response->RulesResponded("transport", "lan-radmin");
                response->RulesRefreshComplete();
            }

            return query;
        }

        virtual void CancelServerQuery(HServerQuery query)
        {
            TraceMatchmakingServers("CancelServerQuery", std::to_string(query));
            NSR_UNUSED(query);
        }
    };

    FakeSteamMatchmakingServers g_Interface;
}

void* FakeSteamInterfaces::MatchmakingServers()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::MatchmakingServers", "SteamMatchmakingServers002");
    Logger::Info("SteamMatchmakingServers: offline LAN/Radmin server-list interface returned");
    return &g_Interface;
}
