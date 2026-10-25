#include "StdInc.h"
#include "FakeSteamInterfaces.h"
#include "FakeSteamBase.h"
#include "SteamCallbackManager.h"
#include "SteamIDManager.h"
#include "SteamVersionLogger.h"
#include "HTTP/SteamHTTP.h"
#include "Input/SteamInputEmulator.h"

namespace
{
    constexpr uint32_t kOfflineAppId = 1276760;
    constexpr int kEResultOK = 1;

    void CopyBytes(void* out, uint32_t outSize, const void* data, uint32_t dataSize)
    {
        if (!out || outSize == 0 || !data || dataSize == 0)
            return;

        std::memcpy(out, data, std::min(outSize, dataSize));
    }

    void TraceCall(const char* interfaceName, const char* functionName, const std::string& detail = std::string())
    {
        SteamVersionLogger::LogCall(interfaceName, functionName, detail);
    }
}

#define DEFINE_FAKE_INTERFACE(functionName, className, logName) \
class className final : public FakeSteamBase \
{ \
protected: \
    const char* InterfaceName() const override { return logName; } \
public: \
    virtual bool Init() { TraceCall(logName, "Init"); Logger::Info(std::string(logName) + "::Init"); return true; } \
    virtual bool Shutdown() { TraceCall(logName, "Shutdown"); Logger::Info(std::string(logName) + "::Shutdown"); return true; } \
    virtual bool IsEnabled() { TraceCall(logName, "IsEnabled"); Logger::Info(std::string(logName) + "::IsEnabled"); return false; } \
    virtual int GetCount() { TraceCall(logName, "GetCount"); Logger::Info(std::string(logName) + "::GetCount"); return 0; } \
    virtual bool Request() { TraceCall(logName, "Request"); Logger::Info(std::string(logName) + "::Request"); return true; } \
}; \
static className g_##className; \
void* FakeSteamInterfaces::functionName() \
{ \
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::" #functionName, logName); \
    Logger::Info(std::string(logName) + ": emulated generic interface returned"); \
    return &g_##className; \
}

DEFINE_FAKE_INTERFACE(Screenshots, FakeSteamScreenshots, "SteamScreenshots")

void* FakeSteamInterfaces::HTTP()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::HTTP", "STEAMHTTP_INTERFACE_VERSION003");
    Logger::Info("SteamHTTP: emulated ISteamHTTP003 interface returned");
    return SteamHTTPEmulator::GetInterface();
}
DEFINE_FAKE_INTERFACE(Controller, FakeSteamController, "SteamController")
DEFINE_FAKE_INTERFACE(UGC, FakeSteamUGC, "SteamUGC")
DEFINE_FAKE_INTERFACE(AppList, FakeSteamAppList, "SteamAppList")
DEFINE_FAKE_INTERFACE(Music, FakeSteamMusic, "SteamMusic")
DEFINE_FAKE_INTERFACE(MusicRemote, FakeSteamMusicRemote, "SteamMusicRemote")
DEFINE_FAKE_INTERFACE(HTMLSurface, FakeSteamHTMLSurface, "SteamHTMLSurface")
DEFINE_FAKE_INTERFACE(Inventory, FakeSteamInventory, "SteamInventory")
DEFINE_FAKE_INTERFACE(Video, FakeSteamVideo, "SteamVideo")
DEFINE_FAKE_INTERFACE(ParentalSettings, FakeSteamParentalSettings, "SteamParentalSettings")
DEFINE_FAKE_INTERFACE(GameSearch, FakeSteamGameSearch, "SteamGameSearch")
void* FakeSteamInterfaces::Input()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::Input", "SteamInput006");
    Logger::Info("SteamInput: emulated ISteamInput006 interface returned");
    return SteamInputEmulator::GetInterface();
}
DEFINE_FAKE_INTERFACE(Parties, FakeSteamParties, "SteamParties")
DEFINE_FAKE_INTERFACE(RemotePlay, FakeSteamRemotePlay, "SteamRemotePlay")

class FakeSteamGameCoordinator final
{
public:
    virtual int SendMessage_(uint32_t msgType, const void* data, uint32_t dataSize)
    {
        TraceCall("SteamGameCoordinator", "SendMessage_", "type=" + std::to_string(msgType) + " bytes=" + std::to_string(dataSize));
        NSR_UNUSED(msgType);
        NSR_UNUSED(data);
        NSR_UNUSED(dataSize);
        Logger::Info("SteamGameCoordinator::SendMessage_");
        return 0;
    }

    virtual bool IsMessageAvailable(uint32_t* messageSize)
    {
        TraceCall("SteamGameCoordinator", "IsMessageAvailable");
        if (messageSize)
            *messageSize = 0;

        Logger::Info("SteamGameCoordinator::IsMessageAvailable");
        return false;
    }

    virtual int RetrieveMessage(uint32_t* msgType, void* dest, uint32_t destSize, uint32_t* messageSize)
    {
        TraceCall("SteamGameCoordinator", "RetrieveMessage", "destBytes=" + std::to_string(destSize));
        NSR_UNUSED(dest);
        NSR_UNUSED(destSize);

        if (msgType)
            *msgType = 0;

        if (messageSize)
            *messageSize = 0;

        Logger::Info("SteamGameCoordinator::RetrieveMessage");
        return 1;
    }
};

class FakeSteamUnifiedMessages final
{
public:
    virtual uint64_t SendMethod(const char* serviceMethod, const void* requestBuffer, uint32_t requestSize, uint64_t context)
    {
        TraceCall(
            "SteamUnifiedMessages",
            "SendMethod",
            std::string(serviceMethod ? serviceMethod : "null") + " bytes=" + std::to_string(requestSize));
        NSR_UNUSED(serviceMethod);
        NSR_UNUSED(requestBuffer);
        NSR_UNUSED(requestSize);

        static std::atomic<uint64_t> s_NextHandle{ 1 };
        const uint64_t handle = s_NextHandle.fetch_add(1);

        struct Result
        {
            uint64_t m_hHandle;
            uint64_t m_unContext;
            int m_eResult;
            uint32_t m_unResponseSize;
        };

        Result result{ handle, context, kEResultOK, 0 };
        SteamCallbackManager::PushCallback(2501, &result, sizeof(result));

        Logger::Info("SteamUnifiedMessages::SendMethod");
        return handle;
    }

    virtual bool GetMethodResponseInfo(uint64_t handle, uint32_t* responseSize, int* result)
    {
        TraceCall("SteamUnifiedMessages", "GetMethodResponseInfo", "handle=" + std::to_string(handle));
        NSR_UNUSED(handle);

        if (responseSize)
            *responseSize = 0;

        if (result)
            *result = kEResultOK;

        Logger::Info("SteamUnifiedMessages::GetMethodResponseInfo");
        return true;
    }

    virtual bool GetMethodResponseData(uint64_t handle, void* responseBuffer, uint32_t responseBufferSize, bool autoRelease)
    {
        TraceCall("SteamUnifiedMessages", "GetMethodResponseData", "handle=" + std::to_string(handle) + " bytes=" + std::to_string(responseBufferSize));
        NSR_UNUSED(handle);
        NSR_UNUSED(responseBuffer);
        NSR_UNUSED(responseBufferSize);
        NSR_UNUSED(autoRelease);

        Logger::Info("SteamUnifiedMessages::GetMethodResponseData");
        return true;
    }

    virtual bool ReleaseMethod(uint64_t handle)
    {
        TraceCall("SteamUnifiedMessages", "ReleaseMethod", "handle=" + std::to_string(handle));
        NSR_UNUSED(handle);
        Logger::Info("SteamUnifiedMessages::ReleaseMethod");
        return true;
    }

    virtual bool SendNotification(const char* serviceNotification, const void* notificationBuffer, uint32_t notificationSize)
    {
        TraceCall(
            "SteamUnifiedMessages",
            "SendNotification",
            std::string(serviceNotification ? serviceNotification : "null") + " bytes=" + std::to_string(notificationSize));
        NSR_UNUSED(serviceNotification);
        NSR_UNUSED(notificationBuffer);
        NSR_UNUSED(notificationSize);

        Logger::Info("SteamUnifiedMessages::SendNotification");
        return true;
    }
};

class FakeSteamAppTicket final
{
public:
    virtual uint32_t GetAppOwnershipTicketData(
        uint32_t appId,
        void* buffer,
        uint32_t bufferLength,
        uint32_t* appIdOffset,
        uint32_t* steamIdOffset,
        uint32_t* signatureOffset,
        uint32_t* signatureSize)
    {
        TraceCall("SteamAppTicket", "GetAppOwnershipTicketData", "app=" + std::to_string(appId) + " bytes=" + std::to_string(bufferLength));
        const uint32_t resolvedAppId = appId != 0 ? appId : kOfflineAppId;
        const uint64_t steamId = SteamIDManager::GetSteamID64();
        const char tag[] = "BREAKERSREVIVED_OFFLINE_APP_TICKET";

        std::vector<unsigned char> ticket(sizeof(resolvedAppId) + sizeof(steamId) + sizeof(tag));
        size_t offset = 0;
        std::memcpy(ticket.data() + offset, &resolvedAppId, sizeof(resolvedAppId));
        offset += sizeof(resolvedAppId);
        std::memcpy(ticket.data() + offset, &steamId, sizeof(steamId));
        offset += sizeof(steamId);
        std::memcpy(ticket.data() + offset, tag, sizeof(tag));

        if (appIdOffset)
            *appIdOffset = 0;

        if (steamIdOffset)
            *steamIdOffset = sizeof(resolvedAppId);

        if (signatureOffset)
            *signatureOffset = static_cast<uint32_t>(sizeof(resolvedAppId) + sizeof(steamId));

        if (signatureSize)
            *signatureSize = static_cast<uint32_t>(sizeof(tag));

        CopyBytes(buffer, bufferLength, ticket.data(), static_cast<uint32_t>(ticket.size()));
        Logger::Info("SteamAppTicket::GetAppOwnershipTicketData");
        return static_cast<uint32_t>(ticket.size());
    }
};

class FakeSteamTV final
{
public:
    virtual bool IsBroadcasting(int* viewers)
    {
        TraceCall("SteamTV", "IsBroadcasting");
        if (viewers)
            *viewers = 0;

        Logger::Info("SteamTV::IsBroadcasting");
        return false;
    }

    virtual void AddBroadcastGameData(const char* key, const char* value)
    {
        TraceCall("SteamTV", "AddBroadcastGameData", std::string(key ? key : "null"));
        NSR_UNUSED(key);
        NSR_UNUSED(value);
        Logger::Info("SteamTV::AddBroadcastGameData");
    }

    virtual void RemoveBroadcastGameData(const char* key)
    {
        TraceCall("SteamTV", "RemoveBroadcastGameData", std::string(key ? key : "null"));
        NSR_UNUSED(key);
        Logger::Info("SteamTV::RemoveBroadcastGameData");
    }

    virtual void AddTimelineMarker(const char* name, bool persistent, uint8_t r, uint8_t g, uint8_t b)
    {
        TraceCall("SteamTV", "AddTimelineMarker", std::string(name ? name : "null"));
        NSR_UNUSED(name);
        NSR_UNUSED(persistent);
        NSR_UNUSED(r);
        NSR_UNUSED(g);
        NSR_UNUSED(b);
        Logger::Info("SteamTV::AddTimelineMarker");
    }

    virtual void RemoveTimelineMarker()
    {
        TraceCall("SteamTV", "RemoveTimelineMarker");
        Logger::Info("SteamTV::RemoveTimelineMarker");
    }

    virtual uint32_t AddRegion(const char* elementName, const char* timelineSection, const void* region, int behavior)
    {
        TraceCall("SteamTV", "AddRegion", std::string(elementName ? elementName : "null"));
        NSR_UNUSED(elementName);
        NSR_UNUSED(timelineSection);
        NSR_UNUSED(region);
        NSR_UNUSED(behavior);
        Logger::Info("SteamTV::AddRegion");
        return 1;
    }

    virtual void RemoveRegion(uint32_t regionHandle)
    {
        TraceCall("SteamTV", "RemoveRegion", "handle=" + std::to_string(regionHandle));
        NSR_UNUSED(regionHandle);
        Logger::Info("SteamTV::RemoveRegion");
    }
};

class FakeSteamMasterServerUpdater final
{
public:
    virtual void SetActive(bool active) { TraceCall("SteamMasterServerUpdater", "SetActive", active ? "true" : "false"); NSR_UNUSED(active); Logger::Info("SteamMasterServerUpdater::SetActive"); }
    virtual void SetHeartbeatInterval(int interval) { TraceCall("SteamMasterServerUpdater", "SetHeartbeatInterval", std::to_string(interval)); NSR_UNUSED(interval); Logger::Info("SteamMasterServerUpdater::SetHeartbeatInterval"); }
    virtual bool HandleIncomingPacket(const void* data, int dataSize, uint32_t sourceIp, uint16_t sourcePort)
    {
        TraceCall("SteamMasterServerUpdater", "HandleIncomingPacket", "bytes=" + std::to_string(dataSize));
        NSR_UNUSED(data);
        NSR_UNUSED(dataSize);
        NSR_UNUSED(sourceIp);
        NSR_UNUSED(sourcePort);
        Logger::Info("SteamMasterServerUpdater::HandleIncomingPacket");
        return false;
    }
    virtual int GetNextOutgoingPacket(void* out, int outSize, uint32_t* netAddress, uint16_t* port)
    {
        TraceCall("SteamMasterServerUpdater", "GetNextOutgoingPacket", "bytes=" + std::to_string(outSize));
        NSR_UNUSED(out);
        NSR_UNUSED(outSize);

        if (netAddress)
            *netAddress = 0;

        if (port)
            *port = 0;

        Logger::Info("SteamMasterServerUpdater::GetNextOutgoingPacket");
        return 0;
    }
    virtual void SetBasicServerData(unsigned short protocolVersion, bool dedicatedServer, const char* regionName, const char* productName, unsigned short maxClients, bool passwordProtected, const char* description)
    {
        TraceCall("SteamMasterServerUpdater", "SetBasicServerData", "product=" + std::string(productName ? productName : "null"));
        NSR_UNUSED(protocolVersion);
        NSR_UNUSED(dedicatedServer);
        NSR_UNUSED(regionName);
        NSR_UNUSED(productName);
        NSR_UNUSED(maxClients);
        NSR_UNUSED(passwordProtected);
        NSR_UNUSED(description);
        Logger::Info("SteamMasterServerUpdater::SetBasicServerData");
    }
    virtual void ClearAllKeyValues() { TraceCall("SteamMasterServerUpdater", "ClearAllKeyValues"); Logger::Info("SteamMasterServerUpdater::ClearAllKeyValues"); }
    virtual void SetKeyValue(const char* key, const char* value)
    {
        TraceCall("SteamMasterServerUpdater", "SetKeyValue", std::string(key ? key : "null"));
        NSR_UNUSED(key);
        NSR_UNUSED(value);
        Logger::Info("SteamMasterServerUpdater::SetKeyValue");
    }
    virtual void NotifyShutdown() { TraceCall("SteamMasterServerUpdater", "NotifyShutdown"); Logger::Info("SteamMasterServerUpdater::NotifyShutdown"); }
    virtual bool WasRestartRequested() { TraceCall("SteamMasterServerUpdater", "WasRestartRequested"); Logger::Info("SteamMasterServerUpdater::WasRestartRequested"); return false; }
    virtual void ForceHeartbeat() { TraceCall("SteamMasterServerUpdater", "ForceHeartbeat"); Logger::Info("SteamMasterServerUpdater::ForceHeartbeat"); }
    virtual bool AddMasterServer(const char* serverAddress)
    {
        TraceCall("SteamMasterServerUpdater", "AddMasterServer", std::string(serverAddress ? serverAddress : "null"));
        NSR_UNUSED(serverAddress);
        Logger::Info("SteamMasterServerUpdater::AddMasterServer");
        return false;
    }
    virtual bool RemoveMasterServer(const char* serverAddress)
    {
        TraceCall("SteamMasterServerUpdater", "RemoveMasterServer", std::string(serverAddress ? serverAddress : "null"));
        NSR_UNUSED(serverAddress);
        Logger::Info("SteamMasterServerUpdater::RemoveMasterServer");
        return true;
    }
    virtual int GetNumMasterServers() { TraceCall("SteamMasterServerUpdater", "GetNumMasterServers"); Logger::Info("SteamMasterServerUpdater::GetNumMasterServers"); return 0; }
    virtual int GetMasterServerAddress(int server, char* out, int outSize)
    {
        TraceCall("SteamMasterServerUpdater", "GetMasterServerAddress", "index=" + std::to_string(server));
        NSR_UNUSED(server);

        if (out && outSize > 0)
            out[0] = '\0';

        Logger::Info("SteamMasterServerUpdater::GetMasterServerAddress");
        return 0;
    }
};

class FakeSteamFallbackInterface final : public FakeSteamBase
{
protected:
    const char* InterfaceName() const override { return "SteamFallback"; }
};

static FakeSteamGameCoordinator g_FakeSteamGameCoordinator;
static FakeSteamUnifiedMessages g_FakeSteamUnifiedMessages;
static FakeSteamAppTicket g_FakeSteamAppTicket;
static FakeSteamTV g_FakeSteamTV;
static FakeSteamMasterServerUpdater g_FakeSteamMasterServerUpdater;
static FakeSteamFallbackInterface g_FakeSteamFallbackInterface;

void* FakeSteamInterfaces::GameCoordinator()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::GameCoordinator", "SteamGameCoordinator");
    Logger::Info("SteamGameCoordinator: offline emulated interface returned");
    return &g_FakeSteamGameCoordinator;
}

void* FakeSteamInterfaces::UnifiedMessages()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::UnifiedMessages", "SteamUnifiedMessages");
    Logger::Info("SteamUnifiedMessages: offline emulated interface returned");
    return &g_FakeSteamUnifiedMessages;
}

void* FakeSteamInterfaces::AppTicket()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::AppTicket", "SteamAppTicket");
    Logger::Info("SteamAppTicket: offline emulated interface returned");
    return &g_FakeSteamAppTicket;
}

void* FakeSteamInterfaces::TV()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::TV", "SteamTV");
    Logger::Info("SteamTV: offline emulated interface returned");
    return &g_FakeSteamTV;
}

void* FakeSteamInterfaces::MasterServerUpdater()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::MasterServerUpdater", "SteamMasterServerUpdater");
    Logger::Info("SteamMasterServerUpdater: offline emulated interface returned");
    return &g_FakeSteamMasterServerUpdater;
}

void* FakeSteamInterfaces::Fallback()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::Fallback", "SteamFallback");
    Logger::Info("SteamFallback: offline catch-all interface returned");
    return &g_FakeSteamFallbackInterface;
}
