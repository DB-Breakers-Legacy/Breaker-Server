// Real (vtable-backed) stubs for the flat Steam networking interfaces.
//
// The earlier stubs in SteamNetworking.cpp were plain classes without virtual methods, so the object
// handed to the game had no vtable. The game calls these through the vtable: entering practice mode
// it calls ISteamNetworkingUtils004 slot 7 (CheckPingDataUpToDate(1e10f)) from Corn::CSessionSteam's
// constructor (0x1407558B3) and crashed with an access violation reading 0x38 (live test 2026-11-10).
//
// Generated from the SDK headers in sdk_includes (Utils004, Sockets012, Messages002) so the method order
// matches by construction; every method answers a neutral default. Generator: see the commit message.
// Kept apart from StdInc.h: it defines its own CSteamID / CallbackMsg_t, which clash with the SDK's.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "isteamnetworkingutils.h"
#include "isteamnetworkingsockets.h"
#include "isteamnetworkingmessages.h"

// Defined in SteamNetworking.cpp, which has the logger.
void BreakersNetLog(const char* message);
void BreakersNetLogInterface(const char* accessor, const char* version);

namespace
{
    void SteamNetStubTrace(const char* what)
    {
        static std::mutex lock;
        static std::set<std::string> seen;
        std::lock_guard<std::mutex> guard(lock);
        if (seen.insert(what).second)
            BreakersNetLog((std::string("SteamNetworking stub first call: ") + what).c_str());
    }

class BreakersNetworkingUtils final : public ISteamNetworkingUtils
{
public:
    SteamNetworkingMessage_t * AllocateMessage( int cbAllocateBuffer ) override { SteamNetStubTrace("BreakersNetworkingUtils::AllocateMessage"); return nullptr; }
    ESteamNetworkingAvailability GetRelayNetworkStatus( SteamRelayNetworkStatus_t *pDetails ) override { SteamNetStubTrace("BreakersNetworkingUtils::GetRelayNetworkStatus"); if (pDetails) { std::memset(pDetails, 0, sizeof(*pDetails)); pDetails->m_eAvail = k_ESteamNetworkingAvailability_Current; pDetails->m_eAvailNetworkConfig = k_ESteamNetworkingAvailability_Current; pDetails->m_eAvailAnyRelay = k_ESteamNetworkingAvailability_Current; } return k_ESteamNetworkingAvailability_Current; }
    float GetLocalPingLocation( SteamNetworkPingLocation_t &result ) override { SteamNetStubTrace("BreakersNetworkingUtils::GetLocalPingLocation"); std::memset(&result, 0, sizeof(result)); return 0.0f; }
    int EstimatePingTimeBetweenTwoLocations( const SteamNetworkPingLocation_t &location1, const SteamNetworkPingLocation_t &location2 ) override { SteamNetStubTrace("BreakersNetworkingUtils::EstimatePingTimeBetweenTwoLocations"); return 0; }
    int EstimatePingTimeFromLocalHost( const SteamNetworkPingLocation_t &remoteLocation ) override { SteamNetStubTrace("BreakersNetworkingUtils::EstimatePingTimeFromLocalHost"); return 0; }
    void ConvertPingLocationToString( const SteamNetworkPingLocation_t &location, char *pszBuf, int cchBufSize ) override { SteamNetStubTrace("BreakersNetworkingUtils::ConvertPingLocationToString"); if (pszBuf && cchBufSize > 0) pszBuf[0] = 0; }
    bool ParsePingLocationString( const char *pszString, SteamNetworkPingLocation_t &result ) override { SteamNetStubTrace("BreakersNetworkingUtils::ParsePingLocationString"); return false; }
    bool CheckPingDataUpToDate( float flMaxAgeSeconds ) override { SteamNetStubTrace("BreakersNetworkingUtils::CheckPingDataUpToDate"); return true; }
    int GetPingToDataCenter( SteamNetworkingPOPID popID, SteamNetworkingPOPID *pViaRelayPoP ) override { SteamNetStubTrace("BreakersNetworkingUtils::GetPingToDataCenter"); return 0; }
    int GetDirectPingToPOP( SteamNetworkingPOPID popID ) override { SteamNetStubTrace("BreakersNetworkingUtils::GetDirectPingToPOP"); return 0; }
    int GetPOPCount() override { SteamNetStubTrace("BreakersNetworkingUtils::GetPOPCount"); return 0; }
    int GetPOPList( SteamNetworkingPOPID *list, int nListSz ) override { SteamNetStubTrace("BreakersNetworkingUtils::GetPOPList"); return 0; }
    SteamNetworkingMicroseconds GetLocalTimestamp() override { SteamNetStubTrace("BreakersNetworkingUtils::GetLocalTimestamp"); LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (SteamNetworkingMicroseconds)(c.QuadPart * 1000000.0 / f.QuadPart); }
    void SetDebugOutputFunction( ESteamNetworkingSocketsDebugOutputType eDetailLevel, FSteamNetworkingSocketsDebugOutput pfnFunc ) override { SteamNetStubTrace("BreakersNetworkingUtils::SetDebugOutputFunction");  }
    ESteamNetworkingFakeIPType GetIPv4FakeIPType( uint32 nIPv4 ) override { SteamNetStubTrace("BreakersNetworkingUtils::GetIPv4FakeIPType"); return static_cast<ESteamNetworkingFakeIPType>(0); }
    EResult GetRealIdentityForFakeIP( const SteamNetworkingIPAddr &fakeIP, SteamNetworkingIdentity *pOutRealIdentity ) override { SteamNetStubTrace("BreakersNetworkingUtils::GetRealIdentityForFakeIP"); return k_EResultOK; }
    bool SetConfigValue( ESteamNetworkingConfigValue eValue, ESteamNetworkingConfigScope eScopeType, intptr_t scopeObj, ESteamNetworkingConfigDataType eDataType, const void *pArg ) override { SteamNetStubTrace("BreakersNetworkingUtils::SetConfigValue"); return true; }
    ESteamNetworkingGetConfigValueResult GetConfigValue( ESteamNetworkingConfigValue eValue, ESteamNetworkingConfigScope eScopeType, intptr_t scopeObj, ESteamNetworkingConfigDataType *pOutDataType, void *pResult, size_t *cbResult ) override { SteamNetStubTrace("BreakersNetworkingUtils::GetConfigValue"); if (cbResult) *cbResult = 0; return k_ESteamNetworkingGetConfigValue_BadValue; }
    const char * GetConfigValueInfo( ESteamNetworkingConfigValue eValue, ESteamNetworkingConfigDataType *pOutDataType, ESteamNetworkingConfigScope *pOutScope ) override { SteamNetStubTrace("BreakersNetworkingUtils::GetConfigValueInfo"); return nullptr; }
    ESteamNetworkingConfigValue IterateGenericEditableConfigValues( ESteamNetworkingConfigValue eCurrent, bool bEnumerateDevVars ) override { SteamNetStubTrace("BreakersNetworkingUtils::IterateGenericEditableConfigValues"); return static_cast<ESteamNetworkingConfigValue>(0); }
    void SteamNetworkingIPAddr_ToString( const SteamNetworkingIPAddr &addr, char *buf, size_t cbBuf, bool bWithPort ) override { SteamNetStubTrace("BreakersNetworkingUtils::SteamNetworkingIPAddr_ToString");  }
    bool SteamNetworkingIPAddr_ParseString( SteamNetworkingIPAddr *pAddr, const char *pszStr ) override { SteamNetStubTrace("BreakersNetworkingUtils::SteamNetworkingIPAddr_ParseString"); return false; }
    ESteamNetworkingFakeIPType SteamNetworkingIPAddr_GetFakeIPType( const SteamNetworkingIPAddr &addr ) override { SteamNetStubTrace("BreakersNetworkingUtils::SteamNetworkingIPAddr_GetFakeIPType"); return static_cast<ESteamNetworkingFakeIPType>(0); }
    void SteamNetworkingIdentity_ToString( const SteamNetworkingIdentity &identity, char *buf, size_t cbBuf ) override { SteamNetStubTrace("BreakersNetworkingUtils::SteamNetworkingIdentity_ToString");  }
    bool SteamNetworkingIdentity_ParseString( SteamNetworkingIdentity *pIdentity, const char *pszStr ) override { SteamNetStubTrace("BreakersNetworkingUtils::SteamNetworkingIdentity_ParseString"); return false; }
};

class BreakersNetworkingSockets final : public ISteamNetworkingSockets
{
public:
    HSteamListenSocket CreateListenSocketIP( const SteamNetworkingIPAddr &localAddress, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) override { SteamNetStubTrace("BreakersNetworkingSockets::CreateListenSocketIP"); return 0; }
    HSteamNetConnection ConnectByIPAddress( const SteamNetworkingIPAddr &address, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) override { SteamNetStubTrace("BreakersNetworkingSockets::ConnectByIPAddress"); return 0; }
    HSteamListenSocket CreateListenSocketP2P( int nLocalVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) override { SteamNetStubTrace("BreakersNetworkingSockets::CreateListenSocketP2P"); return 0; }
    HSteamNetConnection ConnectP2P( const SteamNetworkingIdentity &identityRemote, int nRemoteVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) override { SteamNetStubTrace("BreakersNetworkingSockets::ConnectP2P"); return 0; }
    EResult AcceptConnection( HSteamNetConnection hConn ) override { SteamNetStubTrace("BreakersNetworkingSockets::AcceptConnection"); return k_EResultOK; }
    bool CloseConnection( HSteamNetConnection hPeer, int nReason, const char *pszDebug, bool bEnableLinger ) override { SteamNetStubTrace("BreakersNetworkingSockets::CloseConnection"); return false; }
    bool CloseListenSocket( HSteamListenSocket hSocket ) override { SteamNetStubTrace("BreakersNetworkingSockets::CloseListenSocket"); return false; }
    bool SetConnectionUserData( HSteamNetConnection hPeer, int64 nUserData ) override { SteamNetStubTrace("BreakersNetworkingSockets::SetConnectionUserData"); return false; }
    int64 GetConnectionUserData( HSteamNetConnection hPeer ) override { SteamNetStubTrace("BreakersNetworkingSockets::GetConnectionUserData"); return 0; }
    void SetConnectionName( HSteamNetConnection hPeer, const char *pszName ) override { SteamNetStubTrace("BreakersNetworkingSockets::SetConnectionName");  }
    bool GetConnectionName( HSteamNetConnection hPeer, char *pszName, int nMaxLen ) override { SteamNetStubTrace("BreakersNetworkingSockets::GetConnectionName"); return false; }
    EResult SendMessageToConnection( HSteamNetConnection hConn, const void *pData, uint32 cbData, int nSendFlags, int64 *pOutMessageNumber ) override { SteamNetStubTrace("BreakersNetworkingSockets::SendMessageToConnection"); return k_EResultOK; }
    void SendMessages( int nMessages, SteamNetworkingMessage_t *const *pMessages, int64 *pOutMessageNumberOrResult ) override { SteamNetStubTrace("BreakersNetworkingSockets::SendMessages");  }
    EResult FlushMessagesOnConnection( HSteamNetConnection hConn ) override { SteamNetStubTrace("BreakersNetworkingSockets::FlushMessagesOnConnection"); return k_EResultOK; }
    int ReceiveMessagesOnConnection( HSteamNetConnection hConn, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) override { SteamNetStubTrace("BreakersNetworkingSockets::ReceiveMessagesOnConnection"); return 0; }
    bool GetConnectionInfo( HSteamNetConnection hConn, SteamNetConnectionInfo_t *pInfo ) override { SteamNetStubTrace("BreakersNetworkingSockets::GetConnectionInfo"); return false; }
    EResult GetConnectionRealTimeStatus( HSteamNetConnection hConn, SteamNetConnectionRealTimeStatus_t *pStatus, int nLanes, SteamNetConnectionRealTimeLaneStatus_t *pLanes ) override { SteamNetStubTrace("BreakersNetworkingSockets::GetConnectionRealTimeStatus"); return k_EResultOK; }
    int GetDetailedConnectionStatus( HSteamNetConnection hConn, char *pszBuf, int cbBuf ) override { SteamNetStubTrace("BreakersNetworkingSockets::GetDetailedConnectionStatus"); return 0; }
    bool GetListenSocketAddress( HSteamListenSocket hSocket, SteamNetworkingIPAddr *address ) override { SteamNetStubTrace("BreakersNetworkingSockets::GetListenSocketAddress"); return false; }
    bool CreateSocketPair( HSteamNetConnection *pOutConnection1, HSteamNetConnection *pOutConnection2, bool bUseNetworkLoopback, const SteamNetworkingIdentity *pIdentity1, const SteamNetworkingIdentity *pIdentity2 ) override { SteamNetStubTrace("BreakersNetworkingSockets::CreateSocketPair"); return false; }
    EResult ConfigureConnectionLanes( HSteamNetConnection hConn, int nNumLanes, const int *pLanePriorities, const uint16 *pLaneWeights ) override { SteamNetStubTrace("BreakersNetworkingSockets::ConfigureConnectionLanes"); return k_EResultOK; }
    bool GetIdentity( SteamNetworkingIdentity *pIdentity ) override { SteamNetStubTrace("BreakersNetworkingSockets::GetIdentity"); if (pIdentity) pIdentity->Clear(); return false; }
    ESteamNetworkingAvailability InitAuthentication() override { SteamNetStubTrace("BreakersNetworkingSockets::InitAuthentication"); return k_ESteamNetworkingAvailability_Current; }
    ESteamNetworkingAvailability GetAuthenticationStatus( SteamNetAuthenticationStatus_t *pDetails ) override { SteamNetStubTrace("BreakersNetworkingSockets::GetAuthenticationStatus"); if (pDetails) std::memset(pDetails, 0, sizeof(*pDetails)); return k_ESteamNetworkingAvailability_Current; }
    HSteamNetPollGroup CreatePollGroup() override { SteamNetStubTrace("BreakersNetworkingSockets::CreatePollGroup"); return 0; }
    bool DestroyPollGroup( HSteamNetPollGroup hPollGroup ) override { SteamNetStubTrace("BreakersNetworkingSockets::DestroyPollGroup"); return false; }
    bool SetConnectionPollGroup( HSteamNetConnection hConn, HSteamNetPollGroup hPollGroup ) override { SteamNetStubTrace("BreakersNetworkingSockets::SetConnectionPollGroup"); return false; }
    int ReceiveMessagesOnPollGroup( HSteamNetPollGroup hPollGroup, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) override { SteamNetStubTrace("BreakersNetworkingSockets::ReceiveMessagesOnPollGroup"); return 0; }
    bool ReceivedRelayAuthTicket( const void *pvTicket, int cbTicket, SteamDatagramRelayAuthTicket *pOutParsedTicket ) override { SteamNetStubTrace("BreakersNetworkingSockets::ReceivedRelayAuthTicket"); return false; }
    int FindRelayAuthTicketForServer( const SteamNetworkingIdentity &identityGameServer, int nRemoteVirtualPort, SteamDatagramRelayAuthTicket *pOutParsedTicket ) override { SteamNetStubTrace("BreakersNetworkingSockets::FindRelayAuthTicketForServer"); return 0; }
    HSteamNetConnection ConnectToHostedDedicatedServer( const SteamNetworkingIdentity &identityTarget, int nRemoteVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) override { SteamNetStubTrace("BreakersNetworkingSockets::ConnectToHostedDedicatedServer"); return 0; }
    uint16 GetHostedDedicatedServerPort() override { SteamNetStubTrace("BreakersNetworkingSockets::GetHostedDedicatedServerPort"); return 0; }
    SteamNetworkingPOPID GetHostedDedicatedServerPOPID() override { SteamNetStubTrace("BreakersNetworkingSockets::GetHostedDedicatedServerPOPID"); return 0; }
    EResult GetHostedDedicatedServerAddress( SteamDatagramHostedAddress *pRouting ) override { SteamNetStubTrace("BreakersNetworkingSockets::GetHostedDedicatedServerAddress"); return k_EResultOK; }
    HSteamListenSocket CreateHostedDedicatedServerListenSocket( int nLocalVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) override { SteamNetStubTrace("BreakersNetworkingSockets::CreateHostedDedicatedServerListenSocket"); return 0; }
    EResult GetGameCoordinatorServerLogin( SteamDatagramGameCoordinatorServerLogin *pLoginInfo, int *pcbSignedBlob, void *pBlob ) override { SteamNetStubTrace("BreakersNetworkingSockets::GetGameCoordinatorServerLogin"); return k_EResultOK; }
    HSteamNetConnection ConnectP2PCustomSignaling( ISteamNetworkingConnectionCustomSignaling *pSignaling, const SteamNetworkingIdentity *pPeerIdentity, int nRemoteVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) override { SteamNetStubTrace("BreakersNetworkingSockets::ConnectP2PCustomSignaling"); return 0; }
    bool ReceivedP2PCustomSignal( const void *pMsg, int cbMsg, ISteamNetworkingCustomSignalingRecvContext *pContext ) override { SteamNetStubTrace("BreakersNetworkingSockets::ReceivedP2PCustomSignal"); return false; }
    bool GetCertificateRequest( int *pcbBlob, void *pBlob, SteamNetworkingErrMsg &errMsg ) override { SteamNetStubTrace("BreakersNetworkingSockets::GetCertificateRequest"); return false; }
    bool SetCertificate( const void *pCertificate, int cbCertificate, SteamNetworkingErrMsg &errMsg ) override { SteamNetStubTrace("BreakersNetworkingSockets::SetCertificate"); return false; }
    void ResetIdentity( const SteamNetworkingIdentity *pIdentity ) override { SteamNetStubTrace("BreakersNetworkingSockets::ResetIdentity");  }
    void RunCallbacks() override { SteamNetStubTrace("BreakersNetworkingSockets::RunCallbacks");  }
    bool BeginAsyncRequestFakeIP( int nNumPorts ) override { SteamNetStubTrace("BreakersNetworkingSockets::BeginAsyncRequestFakeIP"); return false; }
    void GetFakeIP( int idxFirstPort, SteamNetworkingFakeIPResult_t *pInfo ) override { SteamNetStubTrace("BreakersNetworkingSockets::GetFakeIP");  }
    HSteamListenSocket CreateListenSocketP2PFakeIP( int idxFakePort, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) override { SteamNetStubTrace("BreakersNetworkingSockets::CreateListenSocketP2PFakeIP"); return 0; }
    EResult GetRemoteFakeIPForConnection( HSteamNetConnection hConn, SteamNetworkingIPAddr *pOutAddr ) override { SteamNetStubTrace("BreakersNetworkingSockets::GetRemoteFakeIPForConnection"); return k_EResultOK; }
    ISteamNetworkingFakeUDPPort * CreateFakeUDPPort( int idxFakeServerPort ) override { SteamNetStubTrace("BreakersNetworkingSockets::CreateFakeUDPPort"); return nullptr; }
};

class BreakersNetworkingMessages final : public ISteamNetworkingMessages
{
public:
    EResult SendMessageToUser( const SteamNetworkingIdentity &identityRemote, const void *pubData, uint32 cubData, int nSendFlags, int nRemoteChannel ) override { SteamNetStubTrace("BreakersNetworkingMessages::SendMessageToUser"); return k_EResultOK; }
    int ReceiveMessagesOnChannel( int nLocalChannel, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) override { SteamNetStubTrace("BreakersNetworkingMessages::ReceiveMessagesOnChannel"); return 0; }
    bool AcceptSessionWithUser( const SteamNetworkingIdentity &identityRemote ) override { SteamNetStubTrace("BreakersNetworkingMessages::AcceptSessionWithUser"); return false; }
    bool CloseSessionWithUser( const SteamNetworkingIdentity &identityRemote ) override { SteamNetStubTrace("BreakersNetworkingMessages::CloseSessionWithUser"); return false; }
    bool CloseChannelWithUser( const SteamNetworkingIdentity &identityRemote, int nLocalChannel ) override { SteamNetStubTrace("BreakersNetworkingMessages::CloseChannelWithUser"); return false; }
    ESteamNetworkingConnectionState GetSessionConnectionInfo( const SteamNetworkingIdentity &identityRemote, SteamNetConnectionInfo_t *pConnectionInfo, SteamNetConnectionRealTimeStatus_t *pQuickStatus ) override { SteamNetStubTrace("BreakersNetworkingMessages::GetSessionConnectionInfo"); return static_cast<ESteamNetworkingConnectionState>(0); }
};


    BreakersNetworkingUtils g_NetUtils;
    BreakersNetworkingSockets g_NetSockets;
    BreakersNetworkingMessages g_NetMessages;
}

extern "C" void* __cdecl BreakersRevived_SteamNetworkingUtils()
{
    BreakersNetLogInterface("BreakersRevived_SteamNetworkingUtils", STEAMNETWORKINGUTILS_INTERFACE_VERSION);
    return static_cast<ISteamNetworkingUtils*>(&g_NetUtils);
}

extern "C" void* __cdecl BreakersRevived_SteamNetworkingSockets()
{
    BreakersNetLogInterface("BreakersRevived_SteamNetworkingSockets", STEAMNETWORKINGSOCKETS_INTERFACE_VERSION);
    return static_cast<ISteamNetworkingSockets*>(&g_NetSockets);
}

extern "C" void* __cdecl BreakersRevived_SteamGameServerNetworkingSockets()
{
    BreakersNetLogInterface("BreakersRevived_SteamGameServerNetworkingSockets", STEAMNETWORKINGSOCKETS_INTERFACE_VERSION);
    return static_cast<ISteamNetworkingSockets*>(&g_NetSockets);
}

extern "C" void* __cdecl BreakersRevived_SteamNetworkingMessages()
{
    BreakersNetLogInterface("BreakersRevived_SteamNetworkingMessages", STEAMNETWORKINGMESSAGES_INTERFACE_VERSION);
    return static_cast<ISteamNetworkingMessages*>(&g_NetMessages);
}
