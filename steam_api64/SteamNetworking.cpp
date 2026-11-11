#include "StdInc.h"
#include "Logger.h"
#include "SteamConfig.h"
#include "SteamIDManager.h"
#include "SteamOfflineServer.h"
#include "SteamP2PManager.h"
#include "SteamVersionLogger.h"

#include <WinSock2.h>
#include <Ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

typedef uint32_t SNetSocket_t;
typedef uint32_t SNetListenSocket_t;

enum EP2PSend
{
    k_EP2PSendUnreliable = 0,
    k_EP2PSendUnreliableNoDelay = 1,
    k_EP2PSendReliable = 2,
    k_EP2PSendReliableWithBuffering = 3
};

struct P2PSessionState_t
{
    uint8_t m_bConnectionActive;
    uint8_t m_bConnecting;
    uint8_t m_eP2PSessionError;
    uint8_t m_bUsingRelay;
    int32_t m_nBytesQueuedForSend;
    int32_t m_nPacketsQueuedForSend;
    uint32_t m_nRemoteIP;
    uint16_t m_nRemotePort;
};

enum ESNetSocketConnectionType
{
    k_ESNetSocketConnectionTypeNotConnected = 0,
    k_ESNetSocketConnectionTypeUDP = 1,
    k_ESNetSocketConnectionTypeUDPRelay = 2
};

static const char* BREAKERS_DEFAULT_P2P_HOST = "127.0.0.1";
static const uint16_t BREAKERS_DEFAULT_P2P_PORT = 47584;

static bool IsWritableMemory(void* ptr, size_t size)
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

    if (end < start || end > regionEnd)
        return false;

    const DWORD protect = mbi.Protect & 0xff;
    return protect == PAGE_READWRITE ||
        protect == PAGE_WRITECOPY ||
        protect == PAGE_EXECUTE_READWRITE ||
        protect == PAGE_EXECUTE_WRITECOPY;
}

template <typename T>
static void SafeWriteValue(T* ptr, const T& value)
{
    if (IsWritableMemory(ptr, sizeof(T)))
        *ptr = value;
}

struct QueuedPacket
{
    CSteamID sender{};
    int channel{};
    std::vector<uint8_t> data;
};

class BreakersSteamNetworking
{
private:
    SOCKET socketHandle = INVALID_SOCKET;
    sockaddr_in serverAddress{};
    bool initialized = false;
    uint64_t lastPollMs = 0;

    std::mutex packetMutex;
    std::vector<QueuedPacket> packets;

private:
    void QueuePacket(CSteamID sender, const void* data, uint32_t size, int channel)
    {
        if (!data || size == 0)
            return;

        QueuedPacket packet{};
        packet.sender = sender;
        packet.channel = channel;
        packet.data.resize(size);
        memcpy(packet.data.data(), data, size);

        std::lock_guard<std::mutex> lock(packetMutex);
        packets.push_back(packet);
    }

private:
    bool EnsureSocket()
    {
        if (initialized)
            return socketHandle != INVALID_SOCKET;

        initialized = true;

        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);

        socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        if (socketHandle == INVALID_SOCKET)
        {
            Logger::Error("BreakersSteamNetworking: socket creation failed");
            return false;
        }

        u_long nonBlocking = 1;
        ioctlsocket(socketHandle, FIONBIO, &nonBlocking);

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(0);

        bind(
            socketHandle,
            reinterpret_cast<sockaddr*>(&local),
            sizeof(local));

        std::string host;
        uint16_t port = 0;
        SteamOfflineServer::GetBestEndpoint(0, host, port);

        if (host.empty())
            host = BREAKERS_DEFAULT_P2P_HOST;

        if (port == 0)
            port = BREAKERS_DEFAULT_P2P_PORT;

        serverAddress = {};
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_port = htons(port);

        if (host == "localhost")
            host = BREAKERS_DEFAULT_P2P_HOST;

        if (inet_pton(AF_INET, host.c_str(), &serverAddress.sin_addr) != 1)
        {
            Logger::Error("BreakersSteamNetworking: invalid custom P2P host, falling back to 127.0.0.1");
            host = BREAKERS_DEFAULT_P2P_HOST;
            inet_pton(AF_INET, host.c_str(), &serverAddress.sin_addr);
        }

        Logger::Info(
            "BreakersSteamNetworking: custom LAN/Radmin endpoint " +
            host +
            ":" +
            std::to_string(port));

        return true;
    }

    void Poll()
    {
        if (!EnsureSocket())
            return;

        for (;;)
        {
            uint8_t buffer[8192]{};

            sockaddr_in from{};
            int fromLen = sizeof(from);

            int received = recvfrom(
                socketHandle,
                reinterpret_cast<char*>(buffer),
                sizeof(buffer),
                0,
                reinterpret_cast<sockaddr*>(&from),
                &fromLen);

            if (received <= 0)
                break;

            if (received < 24)
                continue;

            if (memcmp(buffer, "BreakersP2P1", 7) != 0)
                continue;

            QueuedPacket packet{};

            uint32_t size = 0;

            memcpy(&packet.sender, buffer + 8, sizeof(CSteamID));
            memcpy(&packet.channel, buffer + 16, sizeof(int));
            memcpy(&size, buffer + 20, sizeof(uint32_t));

            if (size > static_cast<uint32_t>(received - 24))
                continue;

            packet.data.resize(size);

            memcpy(
                packet.data.data(),
                buffer + 24,
                size);
            QueuePacket(packet.sender, packet.data.data(), static_cast<uint32_t>(packet.data.size()), packet.channel);

            Logger::Info(
                "BreakersSteamNetworking: packet received size=" +
                std::to_string(size));
        }
    }

    void PollThrottled(uint64_t minIntervalMs = 5)
    {
        const uint64_t now = static_cast<uint64_t>(GetTickCount64());
        if (now - lastPollMs < minIntervalMs)
            return;

        lastPollMs = now;
        Poll();
    }

public:
    virtual bool SendP2PPacket(
        CSteamID steamIDRemote,
        const void* data,
        uint32_t size,
        EP2PSend sendType,
        int channel)
    {
        SteamVersionLogger::LogCall(
            "SteamNetworking",
            "SendP2PPacket",
            "bytes=" + std::to_string(size) + " channel=" + std::to_string(channel));
        NSR_UNUSED(sendType);

        if (!data || size == 0)
            return false;

        const bool sent = SteamP2PManager::Send(steamIDRemote, data, size, channel);

        Logger::Info(
            "BreakersSteamNetworking: SendP2PPacket emulated size=" +
            std::to_string(size) +
            " channel=" +
            std::to_string(channel) +
            " udp=" +
            std::to_string(sent ? 1 : 0));

        return sent;
    }

    virtual bool IsP2PPacketAvailable(uint32_t* size, int channel)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "IsP2PPacketAvailable", "channel=" + std::to_string(channel));
        PollThrottled();

        uint32_t localSize = 0;
        const bool available = SteamP2PManager::HasPacket(&localSize, channel);
        SafeWriteValue(size, localSize);
        return available;
    }

    virtual bool ReadP2PPacket(
        void* dest,
        uint32_t destSize,
        uint32_t* msgSize,
        CSteamID* remote,
        int channel)
    {
        SteamVersionLogger::LogCall(
            "SteamNetworking",
            "ReadP2PPacket",
            "dest=" + std::to_string(destSize) + " channel=" + std::to_string(channel));
        PollThrottled(0);
        uint32_t localMsgSize = 0;
        CSteamID localRemote = 0;
        const bool ok = SteamP2PManager::Read(dest, destSize, &localMsgSize, &localRemote, channel);

        SafeWriteValue(msgSize, localMsgSize);
        SafeWriteValue(remote, localRemote);

        if (ok)
        {
            Logger::Info(
                "BreakersSteamNetworking: ReadP2PPacket size=" +
                std::to_string(localMsgSize));
        }

        return ok;
    }

    virtual bool AcceptP2PSessionWithUser(CSteamID steamIDRemote)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "AcceptP2PSessionWithUser", std::to_string(steamIDRemote));
        Logger::Info(
            "BreakersSteamNetworking: AcceptP2PSessionWithUser " +
            std::to_string(steamIDRemote));

        return true;
    }

    virtual bool CloseP2PSessionWithUser(CSteamID steamIDRemote)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "CloseP2PSessionWithUser", std::to_string(steamIDRemote));
        Logger::Info(
            "BreakersSteamNetworking: CloseP2PSessionWithUser " +
            std::to_string(steamIDRemote));

        return true;
    }

    virtual bool CloseP2PChannelWithUser(CSteamID steamIDRemote, int channel)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "CloseP2PChannelWithUser", "channel=" + std::to_string(channel));
        Logger::Info(
            "BreakersSteamNetworking: CloseP2PChannelWithUser " +
            std::to_string(steamIDRemote) +
            " channel=" +
            std::to_string(channel));

        return true;
    }

    virtual bool GetP2PSessionState(
        CSteamID steamIDRemote,
        P2PSessionState_t* state)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "GetP2PSessionState", std::to_string(steamIDRemote));
        NSR_UNUSED(steamIDRemote);

        if (!IsWritableMemory(state, sizeof(P2PSessionState_t)))
            return false;

        P2PSessionState_t localState{};
        localState.m_bConnectionActive = 1;
        localState.m_bConnecting = 0;
        localState.m_eP2PSessionError = 0;
        localState.m_bUsingRelay = 0;
        localState.m_nBytesQueuedForSend = 0;
        localState.m_nPacketsQueuedForSend = 0;

        std::string host;
        uint16_t port = 0;
        SteamOfflineServer::GetBestEndpoint(static_cast<uint64_t>(steamIDRemote), host, port);

        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons(port ? port : BREAKERS_DEFAULT_P2P_PORT);

        if (host == "localhost" || host.empty())
            host = BREAKERS_DEFAULT_P2P_HOST;

        if (inet_pton(AF_INET, host.c_str(), &endpoint.sin_addr) != 1)
            inet_pton(AF_INET, BREAKERS_DEFAULT_P2P_HOST, &endpoint.sin_addr);

        localState.m_nRemoteIP = ntohl(endpoint.sin_addr.S_un.S_addr);
        localState.m_nRemotePort = ntohs(endpoint.sin_port);
        *state = localState;

        return true;
    }

    virtual bool AllowP2PPacketRelay(bool allow)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "AllowP2PPacketRelay", allow ? "true" : "false");
        Logger::Info(
            std::string("BreakersSteamNetworking: AllowP2PPacketRelay ") +
            (allow ? "true" : "false"));

        return true;
    }

    virtual SNetListenSocket_t CreateListenSocket(
        int virtualPort,
        uint32_t ip,
        uint16_t port,
        bool relay)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "CreateListenSocket", "port=" + std::to_string(port));
        NSR_UNUSED(virtualPort);
        NSR_UNUSED(ip);
        NSR_UNUSED(port);
        NSR_UNUSED(relay);

        Logger::Info("BreakersSteamNetworking: CreateListenSocket");
        return 1;
    }

    virtual SNetSocket_t CreateP2PConnectionSocket(
        CSteamID target,
        int virtualPort,
        int timeout,
        bool relay)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "CreateP2PConnectionSocket", std::to_string(target));
        NSR_UNUSED(virtualPort);
        NSR_UNUSED(timeout);
        NSR_UNUSED(relay);

        Logger::Info(
            "BreakersSteamNetworking: CreateP2PConnectionSocket " +
            std::to_string(target));

        return 1;
    }

    virtual SNetSocket_t CreateConnectionSocket(
        uint32_t ip,
        uint16_t port,
        int timeout)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "CreateConnectionSocket", "port=" + std::to_string(port));
        NSR_UNUSED(ip);
        NSR_UNUSED(port);
        NSR_UNUSED(timeout);

        Logger::Info("BreakersSteamNetworking: CreateConnectionSocket");
        return 1;
    }

    virtual bool DestroySocket(SNetSocket_t socket, bool notifyRemote)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "DestroySocket", std::to_string(socket));
        NSR_UNUSED(socket);
        NSR_UNUSED(notifyRemote);

        Logger::Info("BreakersSteamNetworking: DestroySocket");
        return true;
    }

    virtual bool DestroyListenSocket(SNetListenSocket_t socket, bool notifyRemote)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "DestroyListenSocket", std::to_string(socket));
        NSR_UNUSED(socket);
        NSR_UNUSED(notifyRemote);

        Logger::Info("BreakersSteamNetworking: DestroyListenSocket");
        return true;
    }

    virtual bool SendDataOnSocket(
        SNetSocket_t socket,
        void* data,
        uint32_t size,
        bool reliable)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "SendDataOnSocket", "bytes=" + std::to_string(size));
        NSR_UNUSED(socket);
        NSR_UNUSED(reliable);

        if (!data || size == 0)
            return false;

        Logger::Info(
            "BreakersSteamNetworking: SendDataOnSocket size=" +
            std::to_string(size));
        return true;
    }

    virtual bool IsDataAvailableOnSocket(SNetSocket_t socket, uint32_t* size)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "IsDataAvailableOnSocket", std::to_string(socket));
        NSR_UNUSED(socket);

        SafeWriteValue(size, 0u);

        return false;
    }

    virtual bool RetrieveDataFromSocket(
        SNetSocket_t socket,
        void* dest,
        uint32_t destSize,
        uint32_t* msgSize)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "RetrieveDataFromSocket", "dest=" + std::to_string(destSize));
        NSR_UNUSED(socket);
        NSR_UNUSED(dest);
        NSR_UNUSED(destSize);

        SafeWriteValue(msgSize, 0u);

        return false;
    }

    virtual bool IsDataAvailable(
        SNetListenSocket_t listenSocket,
        uint32_t* size,
        SNetSocket_t* socket)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "IsDataAvailable", std::to_string(listenSocket));
        NSR_UNUSED(listenSocket);

        SafeWriteValue(size, 0u);
        SafeWriteValue(socket, static_cast<SNetSocket_t>(0));

        return false;
    }

    virtual bool RetrieveData(
        SNetListenSocket_t listenSocket,
        void* dest,
        uint32_t destSize,
        uint32_t* msgSize,
        SNetSocket_t* socket)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "RetrieveData", "dest=" + std::to_string(destSize));
        NSR_UNUSED(listenSocket);
        NSR_UNUSED(dest);
        NSR_UNUSED(destSize);

        SafeWriteValue(msgSize, 0u);
        SafeWriteValue(socket, static_cast<SNetSocket_t>(0));

        return false;
    }

    virtual bool GetSocketInfo(
        SNetSocket_t socket,
        CSteamID* remote,
        int* socketStatus,
        uint32_t* remoteIP,
        uint16_t* remotePort)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "GetSocketInfo", std::to_string(socket));
        NSR_UNUSED(socket);

        SafeWriteValue(remote, SteamIDManager::GetLocalSteamID());
        SafeWriteValue(socketStatus, 1);
        SafeWriteValue(remoteIP, 0x7F000001u);
        SafeWriteValue(remotePort, BREAKERS_DEFAULT_P2P_PORT);

        return true;
    }

    virtual bool GetListenSocketInfo(
        SNetListenSocket_t listenSocket,
        uint32_t* ip,
        uint16_t* port)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "GetListenSocketInfo", std::to_string(listenSocket));
        NSR_UNUSED(listenSocket);

        SafeWriteValue(ip, 0u);
        SafeWriteValue(port, static_cast<uint16_t>(0));

        return true;
    }

    virtual ESNetSocketConnectionType GetSocketConnectionType(SNetSocket_t socket)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "GetSocketConnectionType", std::to_string(socket));
        NSR_UNUSED(socket);
        return k_ESNetSocketConnectionTypeUDP;
    }

    virtual int GetMaxPacketSize(SNetSocket_t socket)
    {
        SteamVersionLogger::LogCall("SteamNetworking", "GetMaxPacketSize", std::to_string(socket));
        NSR_UNUSED(socket);
        return 1200;
    }
};

class BreakersSteamNetworkingMessages
{
public:
    int SendMessageToUser(
        const void* identity,
        const void* data,
        uint32_t size,
        int flags,
        int channel)
    {
        NSR_UNUSED(identity);
        NSR_UNUSED(flags);

        Logger::Info(
            "BreakersSteamNetworkingMessages: SendMessageToUser size=" +
            std::to_string(size));

        return 1;
    }
};

class BreakersSteamNetworkingSockets
{
public:
    bool InitAuthentication()
    {
        Logger::Info("BreakersSteamNetworkingSockets: InitAuthentication");
        return true;
    }
};

class BreakersSteamNetworkingUtils
{
public:
    bool InitializeRelayAccess()
    {
        Logger::Info("BreakersSteamNetworkingUtils: InitializeRelayAccess");
        return true;
    }

    int GetRelayNetworkStatus(void* details)
    {
        NSR_UNUSED(details);

        Logger::Info("BreakersSteamNetworkingUtils: GetRelayNetworkStatus");
        return 1;
    }
};

static BreakersSteamNetworking g_Networking;
static BreakersSteamNetworkingMessages g_Messages;
static BreakersSteamNetworkingSockets g_Sockets;
static BreakersSteamNetworkingUtils g_Utils;

extern "C" void* __cdecl BreakersRevived_SteamNetworking()
{
    SteamVersionLogger::LogInterfaceRequest("BreakersRevived_SteamNetworking", "SteamNetworking006");
    return &g_Networking;
}

extern "C" void* __cdecl BreakersRevived_SteamGameServerNetworking()
{
    SteamVersionLogger::LogInterfaceRequest("BreakersRevived_SteamGameServerNetworking", "SteamNetworking006");
    return &g_Networking;
}

extern "C" void* __cdecl BreakersRevived_SteamNetworkingMessages()
{
    SteamVersionLogger::LogInterfaceRequest("BreakersRevived_SteamNetworkingMessages", "SteamNetworkingMessages002");
    return &g_Messages;
}

extern "C" void* __cdecl BreakersRevived_SteamNetworkingSockets()
{
    SteamVersionLogger::LogInterfaceRequest("BreakersRevived_SteamNetworkingSockets", "SteamNetworkingSockets012");
    return &g_Sockets;
}

extern "C" void* __cdecl BreakersRevived_SteamGameServerNetworkingSockets()
{
    SteamVersionLogger::LogInterfaceRequest("BreakersRevived_SteamGameServerNetworkingSockets", "SteamNetworkingSockets012");
    return &g_Sockets;
}

extern "C" void* __cdecl BreakersRevived_SteamNetworkingUtils()
{
    SteamVersionLogger::LogInterfaceRequest("BreakersRevived_SteamNetworkingUtils", "SteamNetworkingUtils004");
    return &g_Utils;
}
