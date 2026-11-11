#include "StdInc.h"
#include "Logger.h"
#include "FakeSteamInterfaces.h"
#include "FakeSteamCore.h"
#include "Auth/SteamAuth.h"
#include "SteamCallbackManager.h"
#include "SteamCallResultManager.h"
#include "SteamDiagnostics.h"
#include "SteamIDManager.h"
#include "SteamInterfaceRouter.h"
#include "SteamLobbyManager.h"
#include "SteamPersonaManager.h"
#include "SteamStatsLocal.h"
#include "SteamVersionLogger.h"

namespace
{
    struct ManualCallbackMsg
    {
        HSteamUser m_hSteamUser;
        int m_iCallback;
        uint8_t* m_pubParam;
        int m_cubParam;
    };

    thread_local std::vector<unsigned char> g_LastManualCallbackData;
    std::vector<unsigned char> g_FlatEncryptedAppTicket;

    bool IsFlatOfflineAvatarImage(int image)
    {
        return image >= 1 && image <= 3;
    }

    uint32_t FlatOfflineAvatarSize(int image)
    {
        switch (image)
        {
        case 1: return 32;
        case 2: return 64;
        case 3: return 128;
        default: return 0;
        }
    }

    void WriteFlatOfflineAvatarRGBA(int image, uint8_t* dest, uint32_t edge)
    {
        const uint8_t accent = image == 1 ? 0x58 : image == 2 ? 0x8A : 0xD8;

        for (uint32_t y = 0; y < edge; ++y)
        {
            for (uint32_t x = 0; x < edge; ++x)
            {
                const uint32_t index = (y * edge + x) * 4;
                const bool ring = x < 2 || y < 2 || x + 3 > edge || y + 3 > edge;
                const bool diagonal = ((x + y) % 17) < 4;

                dest[index + 0] = ring ? 0xF4 : (diagonal ? accent : 0x28);
                dest[index + 1] = ring ? 0xD2 : (diagonal ? 0xA6 : 0x38);
                dest[index + 2] = ring ? 0x4B : (diagonal ? 0x45 : accent);
                dest[index + 3] = 0xFF;
            }
        }
    }

    bool IsFlatWritableMemory(void* ptr, size_t size)
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

    bool IsFlatReadableMemory(const void* ptr, size_t size)
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
        return protect == PAGE_READONLY ||
            protect == PAGE_READWRITE ||
            protect == PAGE_WRITECOPY ||
            protect == PAGE_EXECUTE_READ ||
            protect == PAGE_EXECUTE_READWRITE ||
            protect == PAGE_EXECUTE_WRITECOPY;
    }

    std::string FlatSteamIDText(uint64_t value)
    {
        std::ostringstream ss;
        ss << value << "/0x" << std::hex << value;
        return ss.str();
    }

    bool IsFlatLikelySteamID64(uint64_t value)
    {
        if (value == 0 || value == UINT64_MAX)
            return false;

        const uint32_t accountID = static_cast<uint32_t>(value & 0xffffffffu);
        const uint32_t instance = static_cast<uint32_t>((value >> 32) & 0xfffffu);
        const uint32_t accountType = static_cast<uint32_t>((value >> 52) & 0xfu);
        const uint32_t universe = static_cast<uint32_t>((value >> 56) & 0xffu);

        if (universe == 0 || universe > 6)
            return false;

        if (accountType == 0 || accountType > 10)
            return false;

        if (accountType == 1 && (accountID == 0 || instance == 0))
            return false;

        return true;
    }

    uint64_t ResolveFlatSteamID(uint64_t raw, const char* functionName)
    {
        if (raw == 0 || IsFlatLikelySteamID64(raw))
            return raw;

        uint64_t pointed = 0;
        const void* ptr = reinterpret_cast<const void*>(static_cast<uintptr_t>(raw));
        if (IsFlatReadableMemory(ptr, sizeof(pointed)))
        {
            std::memcpy(&pointed, ptr, sizeof(pointed));
            if (IsFlatLikelySteamID64(pointed))
            {
                const std::string detail =
                    std::string(functionName ? functionName : "unknown") +
                    " rawPointer=" +
                    FlatSteamIDText(raw) +
                    " pointedSteamID=" +
                    FlatSteamIDText(pointed);
                SteamDiagnostics::MarkSteam("FlatSteamIDPointerResolved", detail);
                SteamVersionLogger::LogCall("SteamFriends", "flat.SteamIDPointerResolved", detail);
                return pointed;
            }

            const uint64_t fallback = SteamIDManager::GetSteamID64();
            const std::string detail =
                std::string(functionName ? functionName : "unknown") +
                " rawPointer=" +
                FlatSteamIDText(raw) +
                " fallbackLocal=" +
                FlatSteamIDText(fallback);
            SteamDiagnostics::MarkSteam("FlatSteamIDPointerFallback", detail);
            SteamVersionLogger::LogCall("SteamFriends", "flat.SteamIDPointerFallback", detail);
            return fallback;
        }

        if (raw == UINT64_MAX)
        {
            const uint64_t fallback = SteamIDManager::GetSteamID64();
            SteamDiagnostics::MarkSteam(
                "FlatSteamIDInvalidFallback",
                std::string(functionName ? functionName : "unknown") +
                " raw=" +
                FlatSteamIDText(raw) +
                " fallbackLocal=" +
                FlatSteamIDText(fallback));
            return fallback;
        }

        return raw;
    }

    template <typename T>
    bool FlatSafeWrite(T* out, const T& value)
    {
        if (!out || !IsFlatWritableMemory(out, sizeof(T)))
            return false;

        *out = value;
        return true;
    }

    struct FlatUserStatsReceivedResult
    {
        uint64_t m_nGameID;
        int m_eResult;
        CSteamID m_steamIDUser;
    };

    struct FlatUserStatsStoredResult
    {
        uint64_t m_nGameID;
        int m_eResult;
    };

    struct FlatUserAchievementStoredResult
    {
        uint64_t m_nGameID;
        bool m_bGroupAchievement;
        char m_rgchAchievementName[128];
        uint32_t m_nCurProgress;
        uint32_t m_nMaxProgress;
    };

    struct FlatFriendGameInfo
    {
        uint64_t m_gameID;
        uint32_t m_unGameIP;
        uint16_t m_usGamePort;
        uint16_t m_usQueryPort;
        CSteamID m_steamIDLobby;
    };

    struct FlatLobbyMatchListResult
    {
        uint32_t m_nLobbiesMatching;
    };

    struct FlatLobbyCreatedResult
    {
        int m_eResult;
        uint64_t m_ulSteamIDLobby;
    };

    struct FlatLobbyEnterResult
    {
        uint64_t m_ulSteamIDLobby;
        uint32_t m_rgfChatPermissions;
        bool m_bLocked;
        uint32_t m_EChatRoomEnterResponse;
    };

    struct FlatLobbyDataUpdateResult
    {
        uint64_t m_ulSteamIDLobby;
        uint64_t m_ulSteamIDMember;
        uint8_t m_bSuccess;
    };

    SteamAPICall_t FlatRequestLobbyList()
    {
        FlatLobbyMatchListResult result{};
        result.m_nLobbiesMatching = static_cast<uint32_t>(SteamLobbyManager::RefreshNetworkLobbies(25));
        return SteamCallResultManager::CreateCallResult(510, &result, sizeof(result), true);
    }

    SteamAPICall_t FlatCreateLobby(int lobbyType, int maxMembers)
    {
        CSteamID lobbyID = SteamLobbyManager::CreateLobby(lobbyType, maxMembers);

        FlatLobbyCreatedResult result{};
        result.m_eResult = 1;
        result.m_ulSteamIDLobby = lobbyID;

        FlatLobbyEnterResult enterResult{};
        enterResult.m_ulSteamIDLobby = lobbyID;
        enterResult.m_rgfChatPermissions = 0;
        enterResult.m_bLocked = false;
        enterResult.m_EChatRoomEnterResponse = 1;

        const SteamAPICall_t call = SteamCallResultManager::CreateCallResult(513, &result, sizeof(result), true);
        SteamCallbackManager::PushCallback(513, &result, sizeof(result));
        SteamCallbackManager::PushCallback(504, &enterResult, sizeof(enterResult));

        return call;
    }

    SteamAPICall_t FlatJoinLobby(CSteamID lobbyID)
    {
        SteamLobbyManager::JoinLobby(lobbyID);

        FlatLobbyEnterResult result{};
        result.m_ulSteamIDLobby = lobbyID;
        result.m_rgfChatPermissions = 0;
        result.m_bLocked = false;
        result.m_EChatRoomEnterResponse = 1;
        const SteamAPICall_t call = SteamCallResultManager::CreateCallResult(504, &result, sizeof(result), true);
        SteamCallbackManager::PushCallback(504, &result, sizeof(result));
        return call;
    }
}

extern "C" void* __cdecl BreakersRevived_SteamNetworking();
extern "C" void* __cdecl BreakersRevived_SteamGameServerNetworking();
extern "C" void* __cdecl BreakersRevived_SteamNetworkingUtils();
extern "C" void* __cdecl BreakersRevived_SteamNetworkingMessages();
extern "C" void* __cdecl BreakersRevived_SteamNetworkingSockets();
extern "C" void* __cdecl BreakersRevived_SteamGameServerNetworkingSockets();

extern "C" __declspec(dllexport)
int __cdecl SteamInternal_SteamAPI_Init(const char* interfaceVersions, char* outError)
{
    SteamVersionLogger::LogExportRequest("SteamInternal_SteamAPI_Init");
    NSR_UNUSED(interfaceVersions);
    if (outError)
        outError[0] = '\0';
    return FakeSteamCore::Init() ? 0 : 1;
}

extern "C" __declspec(dllexport)
bool __cdecl SteamAPI_Init()
{
    SteamVersionLogger::LogExportRequest("SteamAPI_Init");
    Logger::Info("SteamAPI_Init: offline emulation success");
    return FakeSteamCore::Init();
}

extern "C" __declspec(dllexport)
bool __cdecl SteamAPI_InitSafe()
{
    SteamVersionLogger::LogExportRequest("SteamAPI_InitSafe");
    Logger::Info("SteamAPI_InitSafe: offline emulation success");
    return FakeSteamCore::Init();
}

extern "C" __declspec(dllexport)
void __cdecl SteamAPI_Shutdown()
{
    SteamVersionLogger::LogExportRequest("SteamAPI_Shutdown");
    Logger::Info("SteamAPI_Shutdown: offline emulation");
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_RunCallbacks()
{
    SteamVersionLogger::LogCall("SteamAPI", "RunCallbacks");
    SteamCallbackManager::RunCallbacks();
}
extern "C" __declspec(dllexport) void __cdecl SteamAPI_ReleaseCurrentThreadMemory() {}
extern "C" __declspec(dllexport) void __cdecl SteamAPI_ManualDispatch_Init() {}
extern "C" __declspec(dllexport) void __cdecl SteamAPI_ManualDispatch_RunFrame(HSteamPipe pipe)
{
    NSR_UNUSED(pipe);
    SteamLobbyManager::Tick();
    SteamCallResultManager::RunCompletedCallResults(64);
}
extern "C" __declspec(dllexport) bool __cdecl SteamAPI_IsSteamRunning() { return true; }
extern "C" __declspec(dllexport) bool __cdecl SteamAPI_IsSteamRunningOnSteamDeck() { return false; }

extern "C" __declspec(dllexport)
bool __cdecl SteamAPI_RestartAppIfNecessary(AppId_t appId)
{
    NSR_UNUSED(appId);
    return false;
}

extern "C" __declspec(dllexport)
void __cdecl SteamAPI_RegisterCallback(void* callback, int callbackId)
{
    SteamCallbackManager::RegisterCallback(callback, callbackId);
}

extern "C" __declspec(dllexport)
void __cdecl SteamAPI_UnregisterCallback(void* callback)
{
    SteamCallbackManager::UnregisterCallback(callback);
}

extern "C" __declspec(dllexport)
void __cdecl SteamAPI_RegisterCallResult(void* callback, SteamAPICall_t call)
{
    SteamCallResultManager::RegisterCallResult(callback, call);
}

extern "C" __declspec(dllexport)
void __cdecl SteamAPI_UnregisterCallResult(void* callback, SteamAPICall_t call)
{
    SteamCallResultManager::UnregisterCallResult(callback, call);
}

extern "C" __declspec(dllexport)
HSteamUser __cdecl SteamAPI_GetHSteamUser()
{
    return FakeSteamCore::UserHandle();
}

extern "C" __declspec(dllexport)
HSteamPipe __cdecl SteamAPI_GetHSteamPipe()
{
    return FakeSteamCore::PipeHandle();
}

extern "C" __declspec(dllexport)
HSteamPipe __cdecl SteamAPI_GetHSteamPipeDeprecated()
{
    return FakeSteamCore::PipeHandle();
}

extern "C" __declspec(dllexport)
const char* __cdecl SteamAPI_GetSteamInstallPath()
{
    return FakeSteamCore::InstallPath();
}

extern "C" __declspec(dllexport)
uint64_t __cdecl SteamAPI_GetUserSteamID()
{
    return SteamIDManager::GetSteamID64();
}

extern "C" __declspec(dllexport)
void __cdecl SteamAPI_ManualDispatch_FreeLastCallback(HSteamPipe pipe)
{
    NSR_UNUSED(pipe);
    g_LastManualCallbackData.clear();
}

extern "C" __declspec(dllexport)
bool __cdecl SteamAPI_ManualDispatch_GetNextCallback(HSteamPipe pipe, void* callbackMsg)
{
    NSR_UNUSED(pipe);

    if (!callbackMsg)
        return false;

    BreakersCallbackMessage message;
    if (!SteamCallbackManager::PopCallback(message))
        return false;

    g_LastManualCallbackData = message.Data;

    auto* out = reinterpret_cast<ManualCallbackMsg*>(callbackMsg);
    out->m_hSteamUser = FakeSteamCore::UserHandle();
    out->m_iCallback = message.CallbackID;
    out->m_pubParam = g_LastManualCallbackData.empty() ? nullptr : g_LastManualCallbackData.data();
    out->m_cubParam = static_cast<int>(g_LastManualCallbackData.size());

    return true;
}

extern "C" __declspec(dllexport)
bool __cdecl SteamAPI_ManualDispatch_GetAPICallResult(
    HSteamPipe pipe,
    SteamAPICall_t call,
    void* callback,
    int callbackSize,
    int callbackExpected,
    bool* failed)
{
    NSR_UNUSED(pipe);
    if (failed) *failed = false;
    int actualCallback = 0;
    bool ok = SteamCallResultManager::GetData(call, callback, callbackSize, &actualCallback);
    return ok && (callbackExpected == 0 || actualCallback == callbackExpected);
}

// Offline core ownership path
extern "C" __declspec(dllexport) void* __cdecl SteamClient() { SteamVersionLogger::LogExportRequest("SteamClient"); return FakeSteamCore::Client(); }
extern "C" __declspec(dllexport) void* __cdecl SteamUser() { SteamVersionLogger::LogExportRequest("SteamUser"); return FakeSteamCore::UserForVersion("SteamUser023"); }
extern "C" __declspec(dllexport) void* __cdecl SteamUtils() { SteamVersionLogger::LogExportRequest("SteamUtils"); return FakeSteamCore::Utils(); }
extern "C" __declspec(dllexport) void* __cdecl SteamApps() { SteamVersionLogger::LogExportRequest("SteamApps"); return FakeSteamCore::Apps(); }

// Fake/emulated interfaces
extern "C" __declspec(dllexport) void* __cdecl SteamFriends() { SteamVersionLogger::LogExportRequest("SteamFriends"); return FakeSteamInterfaces::FriendsForVersion("SteamFriends017"); }
extern "C" __declspec(dllexport) void* __cdecl SteamMatchmaking() { SteamVersionLogger::LogExportRequest("SteamMatchmaking"); return FakeSteamInterfaces::Matchmaking(); }
extern "C" __declspec(dllexport) void* __cdecl SteamMatchmakingServers() { SteamVersionLogger::LogExportRequest("SteamMatchmakingServers"); return FakeSteamInterfaces::MatchmakingServers(); }
extern "C" __declspec(dllexport) void* __cdecl SteamUserStats() { SteamVersionLogger::LogExportRequest("SteamUserStats"); return FakeSteamInterfaces::UserStats(); }
extern "C" __declspec(dllexport) void* __cdecl SteamRemoteStorage()
{
    SteamVersionLogger::LogExportRequest("SteamRemoteStorage");
    return FakeSteamInterfaces::RemoteStorageForVersion("STEAMREMOTESTORAGE_INTERFACE_VERSION012");
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamUser(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    SteamVersionLogger::LogInterfaceRequest("SteamAPI_ISteamClient_GetISteamUser", version);
    return FakeSteamCore::UserForVersion(version);
}

extern "C" __declspec(dllexport) HSteamUser __cdecl SteamAPI_ISteamUser_GetHSteamUser(void* self)
{
    NSR_UNUSED(self);
    SteamVersionLogger::LogCall("SteamUser", "flat.GetHSteamUser");
    return FakeSteamCore::UserHandle();
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUser_BLoggedOn(void* self)
{
    NSR_UNUSED(self);
    SteamVersionLogger::LogCall("SteamUser", "flat.BLoggedOn");
    return true;
}

extern "C" __declspec(dllexport) CSteamID __cdecl SteamAPI_ISteamUser_GetSteamID(void* self)
{
    NSR_UNUSED(self);
    const CSteamID steamID = SteamIDManager::GetLocalSteamID();
    SteamVersionLogger::LogCall("SteamUser", "flat.GetSteamID", "id=" + std::to_string(steamID));
    return steamID;
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamUser_InitiateGameConnection(void* self, void* authBlob, int maxAuthBlob, CSteamID server, uint32_t ip, uint16_t port, bool secure)
{
    NSR_UNUSED(self);
    NSR_UNUSED(server);
    NSR_UNUSED(ip);
    NSR_UNUSED(port);
    NSR_UNUSED(secure);
    SteamVersionLogger::LogCall("SteamUser", "flat.InitiateGameConnection", "bytes=" + std::to_string(maxAuthBlob));

    uint32_t size = 0;
    SteamAuth::GetAuthSessionTicket(authBlob, maxAuthBlob, &size);
    return static_cast<int>(size);
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamUser_TerminateGameConnection(void* self, uint32_t ip, uint16_t port)
{
    NSR_UNUSED(self);
    NSR_UNUSED(ip);
    NSR_UNUSED(port);
    SteamVersionLogger::LogCall("SteamUser", "flat.TerminateGameConnection", "port=" + std::to_string(port));
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED(void* self, void* authBlob, int maxAuthBlob, CSteamID server, uint32_t ip, uint16_t port, bool secure)
{
    return SteamAPI_ISteamUser_InitiateGameConnection(self, authBlob, maxAuthBlob, server, ip, port, secure);
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamUser_TerminateGameConnection_DEPRECATED(void* self, uint32_t ip, uint16_t port)
{
    SteamAPI_ISteamUser_TerminateGameConnection(self, ip, port);
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamUser_TrackAppUsageEvent(void* self, uint64_t gameID, int event, const char* extra)
{
    NSR_UNUSED(self);
    NSR_UNUSED(gameID);
    NSR_UNUSED(event);
    SteamVersionLogger::LogCall("SteamUser", "flat.TrackAppUsageEvent", extra ? extra : "");
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUser_GetUserDataFolder(void* self, char* buffer, int bufferSize)
{
    NSR_UNUSED(self);
    SteamVersionLogger::LogCall("SteamUser", "flat.GetUserDataFolder", "bytes=" + std::to_string(bufferSize));
    if (!buffer || bufferSize <= 0 || !IsFlatWritableMemory(buffer, static_cast<size_t>(bufferSize)))
        return false;

    const std::string folder = (std::filesystem::path(FakeSteamCore::InstallPath()) / "BreakersRevived" / "userdata").string();
    strncpy_s(buffer, static_cast<size_t>(bufferSize), folder.c_str(), _TRUNCATE);
    return true;
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamUser_StartVoiceRecording(void* self)
{
    NSR_UNUSED(self);
    SteamVersionLogger::LogCall("SteamUser", "flat.StartVoiceRecording");
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamUser_StopVoiceRecording(void* self)
{
    NSR_UNUSED(self);
    SteamVersionLogger::LogCall("SteamUser", "flat.StopVoiceRecording");
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamUser_GetAvailableVoice(void* self, uint32_t* compressed, uint32_t* uncompressed, uint32_t sampleRate)
{
    NSR_UNUSED(self);
    SteamVersionLogger::LogCall("SteamUser", "flat.GetAvailableVoice", "rate=" + std::to_string(sampleRate));
    FlatSafeWrite(compressed, 0u);
    FlatSafeWrite(uncompressed, 0u);
    return 3;
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamUser_GetVoice(void* self, bool wantCompressed, void* dest, uint32_t destSize, uint32_t* written, bool wantUncompressed, void* uncompressedDest, uint32_t uncompressedSize, uint32_t* uncompressedWritten, uint32_t sampleRate)
{
    NSR_UNUSED(self);
    NSR_UNUSED(wantCompressed);
    NSR_UNUSED(dest);
    NSR_UNUSED(destSize);
    NSR_UNUSED(wantUncompressed);
    NSR_UNUSED(uncompressedDest);
    NSR_UNUSED(uncompressedSize);
    SteamVersionLogger::LogCall("SteamUser", "flat.GetVoice", "rate=" + std::to_string(sampleRate));
    FlatSafeWrite(written, 0u);
    FlatSafeWrite(uncompressedWritten, 0u);
    return 3;
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamUser_DecompressVoice(void* self, const void* compressed, uint32_t compressedSize, void* dest, uint32_t destSize, uint32_t* written, uint32_t sampleRate)
{
    NSR_UNUSED(self);
    NSR_UNUSED(compressed);
    NSR_UNUSED(compressedSize);
    NSR_UNUSED(dest);
    NSR_UNUSED(destSize);
    SteamVersionLogger::LogCall("SteamUser", "flat.DecompressVoice", "rate=" + std::to_string(sampleRate));
    FlatSafeWrite(written, 0u);
    return 3;
}

extern "C" __declspec(dllexport) uint32_t __cdecl SteamAPI_ISteamUser_GetVoiceOptimalSampleRate(void* self)
{
    NSR_UNUSED(self);
    SteamVersionLogger::LogCall("SteamUser", "flat.GetVoiceOptimalSampleRate");
    return 48000;
}

extern "C" __declspec(dllexport) uint32_t __cdecl SteamAPI_ISteamUser_GetAuthSessionTicket(void* self, void* ticket, int maxTicket, uint32_t* ticketSize, const void* identity)
{
    NSR_UNUSED(self);
    NSR_UNUSED(identity);
    SteamVersionLogger::LogCall("SteamUser", "flat.GetAuthSessionTicket", "bytes=" + std::to_string(maxTicket));
    return SteamAuth::GetAuthSessionTicket(ticket, maxTicket, ticketSize);
}

extern "C" __declspec(dllexport) uint32_t __cdecl SteamAPI_ISteamUser_GetAuthTicketForWebApi(void* self, const char* identity)
{
    NSR_UNUSED(self);
    SteamVersionLogger::LogCall("SteamUser", "flat.GetAuthTicketForWebApi", identity ? identity : "");
    return SteamAuth::GetAuthTicketForWebApi(identity);
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamUser_BeginAuthSession(void* self, const void* ticket, int ticketSize, CSteamID steamID)
{
    NSR_UNUSED(self);
    SteamVersionLogger::LogCall("SteamUser", "flat.BeginAuthSession", "bytes=" + std::to_string(ticketSize));
    return SteamAuth::BeginAuthSession(ticket, ticketSize, steamID);
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamUser_EndAuthSession(void* self, CSteamID steamID)
{
    NSR_UNUSED(self);
    SteamVersionLogger::LogCall("SteamUser", "flat.EndAuthSession");
    SteamAuth::EndAuthSession(steamID);
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamUser_CancelAuthTicket(void* self, uint32_t ticketHandle)
{
    NSR_UNUSED(self);
    SteamVersionLogger::LogCall("SteamUser", "flat.CancelAuthTicket", std::to_string(ticketHandle));
    SteamAuth::CancelAuthTicket(ticketHandle);
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamUser_UserHasLicenseForApp(void* self, CSteamID steamID, AppId_t appID)
{
    NSR_UNUSED(self);
    NSR_UNUSED(steamID);
    SteamVersionLogger::LogCall("SteamUser", "flat.UserHasLicenseForApp", "app=" + std::to_string(appID));
    return 0;
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUser_BIsBehindNAT(void* self)
{
    NSR_UNUSED(self);
    SteamVersionLogger::LogCall("SteamUser", "flat.BIsBehindNAT");
    return false;
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamUser_AdvertiseGame(void* self, CSteamID server, uint32_t ip, uint16_t port)
{
    NSR_UNUSED(self);
    NSR_UNUSED(server);
    NSR_UNUSED(ip);
    NSR_UNUSED(port);
    SteamVersionLogger::LogCall("SteamUser", "flat.AdvertiseGame", "port=" + std::to_string(port));
}

extern "C" __declspec(dllexport) SteamAPICall_t __cdecl SteamAPI_ISteamUser_RequestEncryptedAppTicket(void* self, void* data, int dataSize)
{
    NSR_UNUSED(self);
    SteamVersionLogger::LogCall("SteamUser", "flat.RequestEncryptedAppTicket", "bytes=" + std::to_string(dataSize));
    const char prefix[] = "BREAKERSREVIVED_ENCRYPTED_APP_TICKET";
    g_FlatEncryptedAppTicket.assign(prefix, prefix + sizeof(prefix));
    if (data && dataSize > 0)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        g_FlatEncryptedAppTicket.insert(g_FlatEncryptedAppTicket.end(), bytes, bytes + dataSize);
    }

    struct Result { int m_eResult; };
    Result result{ 1 };
    return SteamCallResultManager::CreateCallResult(154, &result, sizeof(result), true);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUser_GetEncryptedAppTicket(void* self, void* ticket, int maxTicket, uint32_t* ticketSize)
{
    NSR_UNUSED(self);
    SteamVersionLogger::LogCall("SteamUser", "flat.GetEncryptedAppTicket", "bytes=" + std::to_string(maxTicket));
    if (g_FlatEncryptedAppTicket.empty())
        SteamAPI_ISteamUser_RequestEncryptedAppTicket(self, nullptr, 0);

    FlatSafeWrite(ticketSize, static_cast<uint32_t>(g_FlatEncryptedAppTicket.size()));
    if (!ticket || maxTicket < static_cast<int>(g_FlatEncryptedAppTicket.size()) || !IsFlatWritableMemory(ticket, g_FlatEncryptedAppTicket.size()))
        return false;

    memcpy(ticket, g_FlatEncryptedAppTicket.data(), g_FlatEncryptedAppTicket.size());
    return true;
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamUser_GetGameBadgeLevel(void* self, int series, bool foil)
{
    NSR_UNUSED(self);
    NSR_UNUSED(series);
    NSR_UNUSED(foil);
    return 0;
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamUser_GetPlayerSteamLevel(void* self)
{
    NSR_UNUSED(self);
    return 1;
}

extern "C" __declspec(dllexport) SteamAPICall_t __cdecl SteamAPI_ISteamUser_RequestStoreAuthURL(void* self, const char* redirect)
{
    NSR_UNUSED(self);
    NSR_UNUSED(redirect);
    struct Result { char m_szURL[512]; };
    Result result{};
    strcpy_s(result.m_szURL, "about:blank");
    return SteamCallResultManager::CreateCallResult(165, &result, sizeof(result), true);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUser_BIsPhoneVerified(void* self)
{
    NSR_UNUSED(self);
    return true;
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUser_BIsTwoFactorEnabled(void* self)
{
    NSR_UNUSED(self);
    return false;
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUser_BIsPhoneIdentifying(void* self)
{
    NSR_UNUSED(self);
    return false;
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUser_BIsPhoneRequiringVerification(void* self)
{
    NSR_UNUSED(self);
    return false;
}

extern "C" __declspec(dllexport) SteamAPICall_t __cdecl SteamAPI_ISteamUser_GetMarketEligibility(void* self)
{
    NSR_UNUSED(self);
    return SteamCallResultManager::CreateCallResult(166);
}

extern "C" __declspec(dllexport) SteamAPICall_t __cdecl SteamAPI_ISteamUser_GetDurationControl(void* self)
{
    NSR_UNUSED(self);
    return SteamCallResultManager::CreateCallResult(167);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUser_BSetDurationControlOnlineState(void* self, int state)
{
    NSR_UNUSED(self);
    NSR_UNUSED(state);
    return true;
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamGameServer(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    SteamVersionLogger::LogInterfaceRequest("SteamAPI_ISteamClient_GetISteamGameServer", version);
    return FakeSteamCore::GameServer();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamFriends(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(pipe);
    SteamVersionLogger::LogInterfaceRequest("SteamAPI_ISteamClient_GetISteamFriends", version);
    return version && version[0] ? SteamInterfaceRouter::RouteUserInterface(user, version) : FakeSteamInterfaces::FriendsForVersion("SteamFriends015");
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamUtils(void* self, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(pipe);
    SteamVersionLogger::LogInterfaceRequest("SteamAPI_ISteamClient_GetISteamUtils", version);
    return FakeSteamCore::Utils();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamMatchmaking(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(pipe);
    SteamVersionLogger::LogInterfaceRequest("SteamAPI_ISteamClient_GetISteamMatchmaking", version);
    return version && version[0] ? SteamInterfaceRouter::RouteUserInterface(user, version) : FakeSteamInterfaces::Matchmaking();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamMatchmakingServers(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    SteamVersionLogger::LogInterfaceRequest("SteamAPI_ISteamClient_GetISteamMatchmakingServers", version);
    return FakeSteamInterfaces::MatchmakingServers();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamGenericInterface(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(pipe);
    SteamVersionLogger::LogInterfaceRequest("SteamAPI_ISteamClient_GetISteamGenericInterface", version);
    return version && version[0] ? SteamInterfaceRouter::RouteUserInterface(user, version) : FakeSteamInterfaces::Fallback();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamGameServerStats(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    NSR_UNUSED(version);
    return FakeSteamCore::GameServerStats();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamApps(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    SteamVersionLogger::LogInterfaceRequest("SteamAPI_ISteamClient_GetISteamApps", version);
    return FakeSteamCore::Apps();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamNetworking(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    NSR_UNUSED(version);
    return BreakersRevived_SteamNetworking();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamRemoteStorage(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    SteamVersionLogger::LogInterfaceRequest("SteamAPI_ISteamClient_GetISteamRemoteStorage", version);
    return FakeSteamInterfaces::RemoteStorageForVersion(version);
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamScreenshots(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    NSR_UNUSED(version);
    return FakeSteamInterfaces::Screenshots();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamGameSearch(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    NSR_UNUSED(version);
    return FakeSteamInterfaces::GameSearch();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamHTTP(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    NSR_UNUSED(version);
    return FakeSteamInterfaces::HTTP();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamController(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    NSR_UNUSED(version);
    return FakeSteamInterfaces::Controller();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamUGC(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    NSR_UNUSED(version);
    return FakeSteamInterfaces::UGC();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamAppList(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    NSR_UNUSED(version);
    return FakeSteamInterfaces::AppList();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamMusic(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    NSR_UNUSED(version);
    return FakeSteamInterfaces::Music();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamMusicRemote(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    NSR_UNUSED(version);
    return FakeSteamInterfaces::MusicRemote();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamHTMLSurface(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    NSR_UNUSED(version);
    return FakeSteamInterfaces::HTMLSurface();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamInventory(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    NSR_UNUSED(version);
    return FakeSteamInterfaces::Inventory();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamVideo(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    NSR_UNUSED(version);
    return FakeSteamInterfaces::Video();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamParentalSettings(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    NSR_UNUSED(version);
    return FakeSteamInterfaces::ParentalSettings();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamInput(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    NSR_UNUSED(version);
    return FakeSteamInterfaces::Input();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamParties(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    NSR_UNUSED(version);
    return FakeSteamInterfaces::Parties();
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamRemotePlay(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(user);
    NSR_UNUSED(pipe);
    NSR_UNUSED(version);
    return FakeSteamInterfaces::RemotePlay();
}

extern "C" __declspec(dllexport) const char* __cdecl SteamAPI_ISteamFriends_GetPersonaName(void* self)
{
    NSR_UNUSED(self);
    return SteamPersonaManager::GetPersonaName();
}

extern "C" __declspec(dllexport) SteamAPICall_t __cdecl SteamAPI_ISteamFriends_SetPersonaName(void* self, const char* name)
{
    NSR_UNUSED(self);
    if (name)
        SteamPersonaManager::SetPersonaName(name);

    struct SetPersonaNameResult
    {
        bool m_bSuccess;
        bool m_bLocalSuccess;
        int m_result;
    };

    SetPersonaNameResult result{};
    result.m_bSuccess = true;
    result.m_bLocalSuccess = true;
    result.m_result = 1;
    return SteamCallResultManager::CreateCallResult(347, &result, sizeof(result), true);
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamFriends_GetPersonaState(void* self)
{
    NSR_UNUSED(self);
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamFriends_GetFriendCount(void* self, int flags)
{
    NSR_UNUSED(self);
    NSR_UNUSED(flags);
    return SteamPersonaManager::GetFriendCount();
}

extern "C" __declspec(dllexport) uint64_t __cdecl SteamAPI_ISteamFriends_GetFriendByIndex(void* self, int index, int flags)
{
    NSR_UNUSED(self);
    NSR_UNUSED(flags);
    return SteamPersonaManager::GetFriendByIndex(index);
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamFriends_GetFriendRelationship(void* self, uint64_t steamIDFriend)
{
    NSR_UNUSED(self);
    NSR_UNUSED(steamIDFriend);
    return 3;
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamFriends_GetFriendPersonaState(void* self, uint64_t steamIDFriend)
{
    NSR_UNUSED(self);
    NSR_UNUSED(steamIDFriend);
    return 1;
}

extern "C" __declspec(dllexport) const char* __cdecl SteamAPI_ISteamFriends_GetFriendPersonaName(void* self, uint64_t steamIDFriend)
{
    NSR_UNUSED(self);
    steamIDFriend = ResolveFlatSteamID(steamIDFriend, "GetFriendPersonaName");
    return SteamPersonaManager::GetFriendPersonaName(steamIDFriend);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamFriends_GetFriendGamePlayed(void* self, uint64_t steamIDFriend, void* info)
{
    NSR_UNUSED(self);
    steamIDFriend = ResolveFlatSteamID(steamIDFriend, "GetFriendGamePlayed");
    NSR_UNUSED(steamIDFriend);

    CSteamID currentLobby = SteamLobbyManager::GetCurrentLobby();
    FlatFriendGameInfo gameInfo{};
    gameInfo.m_gameID = 1276760;
    gameInfo.m_steamIDLobby = currentLobby;

    if (info && IsFlatWritableMemory(info, sizeof(gameInfo)))
        std::memcpy(info, &gameInfo, sizeof(gameInfo));

    return currentLobby != 0;
}

extern "C" __declspec(dllexport) const char* __cdecl SteamAPI_ISteamFriends_GetFriendPersonaNameHistory(void* self, uint64_t steamIDFriend, int index)
{
    NSR_UNUSED(self);
    NSR_UNUSED(steamIDFriend);
    NSR_UNUSED(index);
    return "";
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamFriends_GetFriendSteamLevel(void* self, uint64_t steamIDFriend)
{
    NSR_UNUSED(self);
    NSR_UNUSED(steamIDFriend);
    return 1;
}

extern "C" __declspec(dllexport) const char* __cdecl SteamAPI_ISteamFriends_GetPlayerNickname(void* self, uint64_t steamIDPlayer)
{
    NSR_UNUSED(self);
    NSR_UNUSED(steamIDPlayer);
    return "";
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamFriends_HasFriend(void* self, uint64_t steamIDFriend, int flags)
{
    NSR_UNUSED(self);
    steamIDFriend = ResolveFlatSteamID(steamIDFriend, "HasFriend");
    NSR_UNUSED(flags);
    for (int i = 0; i < SteamPersonaManager::GetFriendCount(); ++i)
    {
        if (SteamPersonaManager::GetFriendByIndex(i) == steamIDFriend)
            return true;
    }

    return steamIDFriend == SteamIDManager::GetSteamID64();
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamFriends_SetRichPresence(void* self, const char* key, const char* value)
{
    NSR_UNUSED(self);
    return SteamPersonaManager::SetRichPresence(key, value ? value : "");
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamFriends_ClearRichPresence(void* self)
{
    NSR_UNUSED(self);
    SteamPersonaManager::ClearRichPresence();
}

extern "C" __declspec(dllexport) const char* __cdecl SteamAPI_ISteamFriends_GetFriendRichPresence(void* self, uint64_t steamIDFriend, const char* key)
{
    NSR_UNUSED(self);
    steamIDFriend = ResolveFlatSteamID(steamIDFriend, "GetFriendRichPresence");
    NSR_UNUSED(steamIDFriend);
    return SteamPersonaManager::GetRichPresence(key ? key : "");
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamFriends_GetFriendRichPresenceKeyCount(void* self, uint64_t steamIDFriend)
{
    NSR_UNUSED(self);
    steamIDFriend = ResolveFlatSteamID(steamIDFriend, "GetFriendRichPresenceKeyCount");
    NSR_UNUSED(steamIDFriend);
    return SteamPersonaManager::GetRichPresenceKeyCount();
}

extern "C" __declspec(dllexport) const char* __cdecl SteamAPI_ISteamFriends_GetFriendRichPresenceKeyByIndex(void* self, uint64_t steamIDFriend, int index)
{
    NSR_UNUSED(self);
    steamIDFriend = ResolveFlatSteamID(steamIDFriend, "GetFriendRichPresenceKeyByIndex");
    NSR_UNUSED(steamIDFriend);
    return SteamPersonaManager::GetRichPresenceKeyByIndex(index);
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamFriends_RequestFriendRichPresence(void* self, uint64_t steamIDFriend)
{
    NSR_UNUSED(self);
    steamIDFriend = ResolveFlatSteamID(steamIDFriend, "RequestFriendRichPresence");
    NSR_UNUSED(steamIDFriend);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamFriends_InviteUserToGame(void* self, uint64_t steamIDFriend, const char* connectString)
{
    NSR_UNUSED(self);
    steamIDFriend = ResolveFlatSteamID(steamIDFriend, "InviteUserToGame");
    NSR_UNUSED(steamIDFriend);
    NSR_UNUSED(connectString);
    return false;
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamMatchmaking_GetFavoriteGameCount(void* self)
{
    NSR_UNUSED(self);
    return 0;
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamMatchmaking_GetFavoriteGame(void* self, int game, AppId_t* appID, uint32_t* ip, uint16_t* connPort, uint16_t* queryPort, uint32_t* flags, uint32_t* lastPlayed)
{
    NSR_UNUSED(self);
    NSR_UNUSED(game);
    FlatSafeWrite(appID, static_cast<AppId_t>(1276760));
    FlatSafeWrite(ip, 0u);
    FlatSafeWrite(connPort, static_cast<uint16_t>(0));
    FlatSafeWrite(queryPort, static_cast<uint16_t>(0));
    FlatSafeWrite(flags, 0u);
    FlatSafeWrite(lastPlayed, 0u);
    return false;
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamMatchmaking_AddFavoriteGame(void* self, AppId_t appID, uint32_t ip, uint16_t connPort, uint16_t queryPort, uint32_t flags, uint32_t lastPlayed)
{
    NSR_UNUSED(self);
    NSR_UNUSED(appID);
    NSR_UNUSED(ip);
    NSR_UNUSED(connPort);
    NSR_UNUSED(queryPort);
    NSR_UNUSED(flags);
    NSR_UNUSED(lastPlayed);
    return 0;
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamMatchmaking_RemoveFavoriteGame(void* self, AppId_t appID, uint32_t ip, uint16_t connPort, uint16_t queryPort, uint32_t flags)
{
    NSR_UNUSED(self);
    NSR_UNUSED(appID);
    NSR_UNUSED(ip);
    NSR_UNUSED(connPort);
    NSR_UNUSED(queryPort);
    NSR_UNUSED(flags);
    return true;
}

extern "C" __declspec(dllexport) SteamAPICall_t __cdecl SteamAPI_ISteamMatchmaking_RequestLobbyList(void* self)
{
    NSR_UNUSED(self);
    return FlatRequestLobbyList();
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter(void* self, const char* key, const char* value, int comparison)
{
    NSR_UNUSED(self);
    NSR_UNUSED(key);
    NSR_UNUSED(value);
    NSR_UNUSED(comparison);
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter(void* self, const char* key, int value, int comparison)
{
    NSR_UNUSED(self);
    NSR_UNUSED(key);
    NSR_UNUSED(value);
    NSR_UNUSED(comparison);
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamMatchmaking_AddRequestLobbyListNearValueFilter(void* self, const char* key, int value)
{
    NSR_UNUSED(self);
    NSR_UNUSED(key);
    NSR_UNUSED(value);
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable(void* self, int slotsAvailable)
{
    NSR_UNUSED(self);
    NSR_UNUSED(slotsAvailable);
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter(void* self, int distanceFilter)
{
    NSR_UNUSED(self);
    NSR_UNUSED(distanceFilter);
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter(void* self, int maxResults)
{
    NSR_UNUSED(self);
    NSR_UNUSED(maxResults);
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamMatchmaking_AddRequestLobbyListCompatibleMembersFilter(void* self, uint64_t lobbyID)
{
    NSR_UNUSED(self);
    NSR_UNUSED(lobbyID);
}

extern "C" __declspec(dllexport) uint64_t __cdecl SteamAPI_ISteamMatchmaking_GetLobbyByIndex(void* self, int index)
{
    NSR_UNUSED(self);
    return SteamLobbyManager::GetLobbyByIndex(index);
}

extern "C" __declspec(dllexport) SteamAPICall_t __cdecl SteamAPI_ISteamMatchmaking_CreateLobby(void* self, int lobbyType, int maxMembers)
{
    NSR_UNUSED(self);
    return FlatCreateLobby(lobbyType, maxMembers);
}

extern "C" __declspec(dllexport) SteamAPICall_t __cdecl SteamAPI_ISteamMatchmaking_JoinLobby(void* self, uint64_t lobbyID)
{
    NSR_UNUSED(self);
    return FlatJoinLobby(lobbyID);
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamMatchmaking_LeaveLobby(void* self, uint64_t lobbyID)
{
    NSR_UNUSED(self);
    SteamLobbyManager::LeaveLobby(lobbyID);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamMatchmaking_InviteUserToLobby(void* self, uint64_t lobbyID, uint64_t invitee)
{
    NSR_UNUSED(self);
    NSR_UNUSED(lobbyID);
    NSR_UNUSED(invitee);
    return false;
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(void* self, uint64_t lobbyID)
{
    NSR_UNUSED(self);
    return SteamLobbyManager::GetMemberCount(lobbyID);
}

extern "C" __declspec(dllexport) uint64_t __cdecl SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(void* self, uint64_t lobbyID, int member)
{
    NSR_UNUSED(self);
    return SteamLobbyManager::GetMemberByIndex(lobbyID, member);
}

extern "C" __declspec(dllexport) const char* __cdecl SteamAPI_ISteamMatchmaking_GetLobbyData(void* self, uint64_t lobbyID, const char* key)
{
    NSR_UNUSED(self);
    return SteamLobbyManager::GetData(lobbyID, key ? key : "");
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamMatchmaking_SetLobbyData(void* self, uint64_t lobbyID, const char* key, const char* value)
{
    NSR_UNUSED(self);
    return SteamLobbyManager::SetData(lobbyID, key ? key : "", value ? value : "");
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamMatchmaking_GetLobbyDataCount(void* self, uint64_t lobbyID)
{
    NSR_UNUSED(self);
    return SteamLobbyManager::GetDataCount(lobbyID);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex(void* self, uint64_t lobbyID, int index, char* key, int keySize, char* value, int valueSize)
{
    NSR_UNUSED(self);
    std::string keyText;
    std::string valueText;
    if (!SteamLobbyManager::GetDataByIndex(lobbyID, index, keyText, valueText))
    {
        if (key && keySize > 0)
            key[0] = '\0';
        if (value && valueSize > 0)
            value[0] = '\0';
        return false;
    }

    if (key && keySize > 0)
        strncpy_s(key, static_cast<size_t>(keySize), keyText.c_str(), _TRUNCATE);
    if (value && valueSize > 0)
        strncpy_s(value, static_cast<size_t>(valueSize), valueText.c_str(), _TRUNCATE);
    return true;
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamMatchmaking_DeleteLobbyData(void* self, uint64_t lobbyID, const char* key)
{
    NSR_UNUSED(self);
    return SteamLobbyManager::DeleteData(lobbyID, key ? key : "");
}

extern "C" __declspec(dllexport) const char* __cdecl SteamAPI_ISteamMatchmaking_GetLobbyMemberData(void* self, uint64_t lobbyID, uint64_t user, const char* key)
{
    NSR_UNUSED(self);
    return SteamLobbyManager::GetMemberData(lobbyID, user, key ? key : "");
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamMatchmaking_SetLobbyMemberData(void* self, uint64_t lobbyID, const char* key, const char* value)
{
    NSR_UNUSED(self);
    SteamLobbyManager::SetMemberData(lobbyID, SteamIDManager::GetLocalSteamID(), key ? key : "", value ? value : "");
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamMatchmaking_SendLobbyChatMsg(void* self, uint64_t lobbyID, const void* msgBody, int msgBodySize)
{
    NSR_UNUSED(self);
    return SteamLobbyManager::AddChatEntry(lobbyID, SteamIDManager::GetLocalSteamID(), msgBody, msgBodySize, 1) >= 0;
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamMatchmaking_GetLobbyChatEntry(void* self, uint64_t lobbyID, int chatID, CSteamID* user, void* data, int dataSize, int* chatEntryType)
{
    NSR_UNUSED(self);
    return SteamLobbyManager::GetChatEntry(lobbyID, chatID, user, data, dataSize, chatEntryType);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamMatchmaking_RequestLobbyData(void* self, uint64_t lobbyID)
{
    NSR_UNUSED(self);
    SteamLobbyManager::RefreshNetworkLobbies(20);
    FlatLobbyDataUpdateResult result{};
    result.m_ulSteamIDLobby = lobbyID;
    result.m_ulSteamIDMember = lobbyID;
    result.m_bSuccess = 1;
    SteamCallbackManager::PushCallback(505, &result, sizeof(result));
    return true;
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamMatchmaking_SetLobbyGameServer(void* self, uint64_t lobbyID, uint32_t gameServerIP, uint16_t gameServerPort, uint64_t steamIDGameServer)
{
    NSR_UNUSED(self);
    SteamLobbyManager::SetGameServer(lobbyID, gameServerIP, gameServerPort, steamIDGameServer);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamMatchmaking_GetLobbyGameServer(void* self, uint64_t lobbyID, uint32_t* gameServerIP, uint16_t* gameServerPort, CSteamID* steamIDGameServer)
{
    NSR_UNUSED(self);
    return SteamLobbyManager::GetGameServer(lobbyID, gameServerIP, gameServerPort, steamIDGameServer);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit(void* self, uint64_t lobbyID, int maxMembers)
{
    NSR_UNUSED(self);
    return SteamLobbyManager::SetMemberLimit(lobbyID, maxMembers);
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit(void* self, uint64_t lobbyID)
{
    NSR_UNUSED(self);
    return SteamLobbyManager::GetMemberLimit(lobbyID);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamMatchmaking_SetLobbyType(void* self, uint64_t lobbyID, int lobbyType)
{
    NSR_UNUSED(self);
    return SteamLobbyManager::SetType(lobbyID, lobbyType);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamMatchmaking_SetLobbyJoinable(void* self, uint64_t lobbyID, bool joinable)
{
    NSR_UNUSED(self);
    return SteamLobbyManager::SetJoinable(lobbyID, joinable);
}

extern "C" __declspec(dllexport) uint64_t __cdecl SteamAPI_ISteamMatchmaking_GetLobbyOwner(void* self, uint64_t lobbyID)
{
    NSR_UNUSED(self);
    CSteamID owner = SteamLobbyManager::GetOwner(lobbyID);
    return owner ? owner : lobbyID;
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamMatchmaking_SetLobbyOwner(void* self, uint64_t lobbyID, uint64_t newOwner)
{
    NSR_UNUSED(self);
    return SteamLobbyManager::SetOwner(lobbyID, newOwner);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamMatchmaking_SetLinkedLobby(void* self, uint64_t lobbyID, uint64_t linkedLobbyID)
{
    NSR_UNUSED(self);
    NSR_UNUSED(lobbyID);
    NSR_UNUSED(linkedLobbyID);
    return true;
}

extern "C" __declspec(dllexport) void* __cdecl SteamAPI_ISteamClient_GetISteamUserStats(void* self, HSteamUser user, HSteamPipe pipe, const char* version)
{
    NSR_UNUSED(self);
    NSR_UNUSED(pipe);
    return version && version[0] ? SteamInterfaceRouter::RouteUserInterface(user, version) : FakeSteamInterfaces::UserStats();
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUserStats_RequestCurrentStats(void* self)
{
    NSR_UNUSED(self);
    FlatUserStatsReceivedResult result{};
    result.m_nGameID = 1276760;
    result.m_eResult = 1;
    result.m_steamIDUser = SteamIDManager::GetLocalSteamID();
    SteamCallbackManager::PushCallback(1101, &result, sizeof(result));
    return true;
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUserStats_GetStatInt32(void* self, const char* name, int32_t* data)
{
    NSR_UNUSED(self);
    int32_t value = 0;
    const bool ok = SteamStatsLocal::GetInt(name, &value);
    FlatSafeWrite(data, value);
    return ok;
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUserStats_GetStatFloat(void* self, const char* name, float* data)
{
    NSR_UNUSED(self);
    float value = 0.0f;
    const bool ok = SteamStatsLocal::GetFloat(name, &value);
    FlatSafeWrite(data, value);
    return ok;
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUserStats_SetStatInt32(void* self, const char* name, int32_t data)
{
    NSR_UNUSED(self);
    return SteamStatsLocal::SetInt(name, data);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUserStats_SetStatFloat(void* self, const char* name, float data)
{
    NSR_UNUSED(self);
    return SteamStatsLocal::SetFloat(name, data);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUserStats_UpdateAvgRateStat(void* self, const char* name, float count, double sessionLength)
{
    NSR_UNUSED(self);
    NSR_UNUSED(name);
    NSR_UNUSED(count);
    NSR_UNUSED(sessionLength);
    return true;
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUserStats_GetAchievement(void* self, const char* name, bool* achieved)
{
    NSR_UNUSED(self);
    bool value = false;
    const bool ok = SteamStatsLocal::GetAchievement(name, &value);
    FlatSafeWrite(achieved, value);
    return ok;
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUserStats_SetAchievement(void* self, const char* name)
{
    NSR_UNUSED(self);
    return SteamStatsLocal::SetAchievement(name);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUserStats_ClearAchievement(void* self, const char* name)
{
    NSR_UNUSED(self);
    return SteamStatsLocal::ClearAchievement(name);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime(void* self, const char* name, bool* achieved, uint32_t* unlockTime)
{
    NSR_UNUSED(self);
    bool value = false;
    const bool ok = SteamStatsLocal::GetAchievement(name, &value);
    FlatSafeWrite(achieved, value);
    FlatSafeWrite(unlockTime, 0u);
    return ok;
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUserStats_StoreStats(void* self)
{
    NSR_UNUSED(self);
    SteamStatsLocal::Save();
    FlatUserStatsStoredResult result{};
    result.m_nGameID = 1276760;
    result.m_eResult = 1;
    SteamCallbackManager::PushCallback(1102, &result, sizeof(result));
    return true;
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamUserStats_GetAchievementIcon(void* self, const char* name)
{
    NSR_UNUSED(self);
    NSR_UNUSED(name);
    return 0;
}

extern "C" __declspec(dllexport) const char* __cdecl SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute(void* self, const char* name, const char* key)
{
    NSR_UNUSED(self);
    NSR_UNUSED(name);
    if (key && strcmp(key, "hidden") == 0)
        return "0";
    return "";
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUserStats_IndicateAchievementProgress(void* self, const char* name, uint32_t current, uint32_t max)
{
    NSR_UNUSED(self);
    FlatUserAchievementStoredResult result{};
    result.m_nGameID = 1276760;
    result.m_nCurProgress = current;
    result.m_nMaxProgress = max;
    if (name)
        strncpy_s(result.m_rgchAchievementName, name, _TRUNCATE);
    SteamCallbackManager::PushCallback(1103, &result, sizeof(result));
    return true;
}

extern "C" __declspec(dllexport) uint32_t __cdecl SteamAPI_ISteamUserStats_GetNumAchievements(void* self)
{
    NSR_UNUSED(self);
    return SteamStatsLocal::GetAchievementCount();
}

extern "C" __declspec(dllexport) const char* __cdecl SteamAPI_ISteamUserStats_GetAchievementName(void* self, uint32_t achievement)
{
    NSR_UNUSED(self);
    return SteamStatsLocal::GetAchievementName(achievement);
}

extern "C" __declspec(dllexport) void* __cdecl SteamScreenshots() { return FakeSteamInterfaces::Screenshots(); }
extern "C" __declspec(dllexport) void* __cdecl SteamHTTP() { return FakeSteamInterfaces::HTTP(); }
extern "C" __declspec(dllexport) void* __cdecl SteamController() { return FakeSteamInterfaces::Controller(); }
extern "C" __declspec(dllexport) void* __cdecl SteamUGC() { return FakeSteamInterfaces::UGC(); }
extern "C" __declspec(dllexport) void* __cdecl SteamAppList() { return FakeSteamInterfaces::AppList(); }
extern "C" __declspec(dllexport) void* __cdecl SteamMusic() { return FakeSteamInterfaces::Music(); }
extern "C" __declspec(dllexport) void* __cdecl SteamMusicRemote() { return FakeSteamInterfaces::MusicRemote(); }
extern "C" __declspec(dllexport) void* __cdecl SteamHTMLSurface() { return FakeSteamInterfaces::HTMLSurface(); }
extern "C" __declspec(dllexport) void* __cdecl SteamInventory() { return FakeSteamInterfaces::Inventory(); }
extern "C" __declspec(dllexport) void* __cdecl SteamVideo() { return FakeSteamInterfaces::Video(); }
extern "C" __declspec(dllexport) void* __cdecl SteamParentalSettings() { return FakeSteamInterfaces::ParentalSettings(); }
extern "C" __declspec(dllexport) void* __cdecl SteamGameSearch() { return FakeSteamInterfaces::GameSearch(); }
extern "C" __declspec(dllexport) void* __cdecl SteamInput() { return FakeSteamInterfaces::Input(); }
extern "C" __declspec(dllexport) void* __cdecl SteamParties() { return FakeSteamInterfaces::Parties(); }
extern "C" __declspec(dllexport) void* __cdecl SteamRemotePlay() { return FakeSteamInterfaces::RemotePlay(); }
extern "C" __declspec(dllexport) void* __cdecl SteamGameCoordinator() { return FakeSteamInterfaces::GameCoordinator(); }
extern "C" __declspec(dllexport) void* __cdecl SteamUnifiedMessages() { return FakeSteamInterfaces::UnifiedMessages(); }
extern "C" __declspec(dllexport) void* __cdecl SteamAppTicket() { return FakeSteamInterfaces::AppTicket(); }
extern "C" __declspec(dllexport) void* __cdecl SteamTV() { return FakeSteamInterfaces::TV(); }
extern "C" __declspec(dllexport) void* __cdecl SteamMasterServerUpdater() { return FakeSteamInterfaces::MasterServerUpdater(); }

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamFriends_GetSmallFriendAvatar(void* self, uint64_t steamIDFriend)
{
    NSR_UNUSED(self);
    steamIDFriend = ResolveFlatSteamID(steamIDFriend, "GetSmallFriendAvatar");
    NSR_UNUSED(steamIDFriend);
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamFriends_GetMediumFriendAvatar(void* self, uint64_t steamIDFriend)
{
    NSR_UNUSED(self);
    steamIDFriend = ResolveFlatSteamID(steamIDFriend, "GetMediumFriendAvatar");
    NSR_UNUSED(steamIDFriend);
    return 2;
}

extern "C" __declspec(dllexport) int __cdecl SteamAPI_ISteamFriends_GetLargeFriendAvatar(void* self, uint64_t steamIDFriend)
{
    NSR_UNUSED(self);
    steamIDFriend = ResolveFlatSteamID(steamIDFriend, "GetLargeFriendAvatar");
    NSR_UNUSED(steamIDFriend);
    return 3;
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUtils_GetImageSize(void* self, int image, uint32_t* width, uint32_t* height)
{
    NSR_UNUSED(self);

    const uint32_t edge = FlatOfflineAvatarSize(image);
    if (width) *width = edge;
    if (height) *height = edge;
    return IsFlatOfflineAvatarImage(image);
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamUtils_GetImageRGBA(void* self, int image, uint8_t* dest, int destSize)
{
    NSR_UNUSED(self);

    const uint32_t edge = FlatOfflineAvatarSize(image);
    const uint32_t requiredSize = edge * edge * 4;

    if (!IsFlatOfflineAvatarImage(image) ||
        !dest ||
        destSize < static_cast<int>(requiredSize) ||
        !IsFlatWritableMemory(dest, requiredSize))
    {
        return false;
    }

    WriteFlatOfflineAvatarRGBA(image, dest, edge);
    return true;
}

extern "C" __declspec(dllexport) bool __cdecl SteamAPI_ISteamTV_IsBroadcasting(void* self, int* viewers)
{
    NSR_UNUSED(self);
    if (viewers) *viewers = 0;
    return false;
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamTV_AddBroadcastGameData(void* self, const char* key, const char* value)
{
    NSR_UNUSED(self);
    NSR_UNUSED(key);
    NSR_UNUSED(value);
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamTV_RemoveBroadcastGameData(void* self, const char* key)
{
    NSR_UNUSED(self);
    NSR_UNUSED(key);
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamTV_AddTimelineMarker(void* self, const char* name, bool persistent, uint8_t r, uint8_t g, uint8_t b)
{
    NSR_UNUSED(self);
    NSR_UNUSED(name);
    NSR_UNUSED(persistent);
    NSR_UNUSED(r);
    NSR_UNUSED(g);
    NSR_UNUSED(b);
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamTV_RemoveTimelineMarker(void* self)
{
    NSR_UNUSED(self);
}

extern "C" __declspec(dllexport) uint32_t __cdecl SteamAPI_ISteamTV_AddRegion(void* self, const char* elementName, const char* timelineSection, const void* region, int behavior)
{
    NSR_UNUSED(self);
    NSR_UNUSED(elementName);
    NSR_UNUSED(timelineSection);
    NSR_UNUSED(region);
    NSR_UNUSED(behavior);
    return 1;
}

extern "C" __declspec(dllexport) void __cdecl SteamAPI_ISteamTV_RemoveRegion(void* self, uint32_t regionHandle)
{
    NSR_UNUSED(self);
    NSR_UNUSED(regionHandle);
}

// Custom networking
extern "C" __declspec(dllexport) void* __cdecl SteamNetworking() { SteamVersionLogger::LogExportRequest("SteamNetworking"); return BreakersRevived_SteamNetworking(); }
extern "C" __declspec(dllexport) void* __cdecl SteamNetworkingMessages() { SteamVersionLogger::LogExportRequest("SteamNetworkingMessages"); return BreakersRevived_SteamNetworkingMessages(); }
extern "C" __declspec(dllexport) void* __cdecl SteamNetworkingSockets() { SteamVersionLogger::LogExportRequest("SteamNetworkingSockets"); return BreakersRevived_SteamNetworkingSockets(); }
extern "C" __declspec(dllexport) void* __cdecl SteamNetworkingUtils() { SteamVersionLogger::LogExportRequest("SteamNetworkingUtils"); return BreakersRevived_SteamNetworkingUtils(); }

#define EXPORT_STEAM_ACCESSOR(name, target) \
extern "C" __declspec(dllexport) void* __cdecl name() { SteamVersionLogger::LogExportRequest(#name); return target; }

EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUser_v023, FakeSteamCore::UserForVersion("SteamUser023"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUser_v022, FakeSteamCore::UserForVersion("SteamUser022"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUser_v021, FakeSteamCore::UserForVersion("SteamUser021"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUser_v020, FakeSteamCore::UserForVersion("SteamUser020"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUser_v019, FakeSteamCore::UserForVersion("SteamUser019"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUser_v018, FakeSteamCore::UserForVersion("SteamUser018"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUser_v017, FakeSteamCore::UserForVersion("SteamUser017"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUser_v016, FakeSteamCore::UserForVersion("SteamUser016"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUser_v015, FakeSteamCore::UserForVersion("SteamUser015"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUser_v014, FakeSteamCore::UserForVersion("SteamUser014"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUser_v013, FakeSteamCore::UserForVersion("SteamUser013"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUser_v012, FakeSteamCore::UserForVersion("SteamUser012"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUser_v011, FakeSteamCore::UserForVersion("SteamUser011"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUser_v010, FakeSteamCore::UserForVersion("SteamUser010"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUser_v009, FakeSteamCore::UserForVersion("SteamUser009"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamFriends_v017, FakeSteamInterfaces::FriendsForVersion("SteamFriends017"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamFriends_v016, FakeSteamInterfaces::FriendsForVersion("SteamFriends016"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamFriends_v015, FakeSteamInterfaces::FriendsForVersion("SteamFriends015"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamFriends_v014, FakeSteamInterfaces::FriendsForVersion("SteamFriends014"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamFriends_v013, FakeSteamInterfaces::FriendsForVersion("SteamFriends013"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamFriends_v012, FakeSteamInterfaces::FriendsForVersion("SteamFriends012"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamFriends_v011, FakeSteamInterfaces::FriendsForVersion("SteamFriends011"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamFriends_v010, FakeSteamInterfaces::FriendsForVersion("SteamFriends010"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamFriends_v009, FakeSteamInterfaces::FriendsForVersion("SteamFriends009"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamFriends_v008, FakeSteamInterfaces::FriendsForVersion("SteamFriends008"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamFriends_v007, FakeSteamInterfaces::FriendsForVersion("SteamFriends007"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamFriends_v006, FakeSteamInterfaces::FriendsForVersion("SteamFriends006"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamFriends_v005, FakeSteamInterfaces::FriendsForVersion("SteamFriends005"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamFriends_v004, FakeSteamInterfaces::FriendsForVersion("SteamFriends004"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamFriends_v003, FakeSteamInterfaces::FriendsForVersion("SteamFriends003"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUtils_v010, FakeSteamCore::Utils())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerUtils_v010, FakeSteamCore::Utils())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUtils_v009, FakeSteamCore::Utils())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerUtils_v009, FakeSteamCore::Utils())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUtils_v008, FakeSteamCore::Utils())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerUtils_v008, FakeSteamCore::Utils())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamMatchmaking_v009, FakeSteamInterfaces::MatchmakingV009())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamMatchmaking_v008, FakeSteamInterfaces::MatchmakingV008())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamMatchmaking_v007, FakeSteamInterfaces::MatchmakingV007())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamMatchmaking_v006, FakeSteamInterfaces::MatchmakingV006())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamMatchmakingServers_v002, FakeSteamInterfaces::MatchmakingServers())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameSearch_v001, FakeSteamInterfaces::GameSearch())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamParties_v002, FakeSteamInterfaces::Parties())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamRemoteStorage_v001, FakeSteamInterfaces::RemoteStorageForVersion("STEAMREMOTESTORAGE_INTERFACE_VERSION001"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamRemoteStorage_v002, FakeSteamInterfaces::RemoteStorageForVersion("STEAMREMOTESTORAGE_INTERFACE_VERSION002"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamRemoteStorage_v003, FakeSteamInterfaces::RemoteStorageForVersion("STEAMREMOTESTORAGE_INTERFACE_VERSION003"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamRemoteStorage_v004, FakeSteamInterfaces::RemoteStorageForVersion("STEAMREMOTESTORAGE_INTERFACE_VERSION004"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamRemoteStorage_v005, FakeSteamInterfaces::RemoteStorageForVersion("STEAMREMOTESTORAGE_INTERFACE_VERSION005"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamRemoteStorage_v006, FakeSteamInterfaces::RemoteStorageForVersion("STEAMREMOTESTORAGE_INTERFACE_VERSION006"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamRemoteStorage_v007, FakeSteamInterfaces::RemoteStorageForVersion("STEAMREMOTESTORAGE_INTERFACE_VERSION007"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamRemoteStorage_v008, FakeSteamInterfaces::RemoteStorageForVersion("STEAMREMOTESTORAGE_INTERFACE_VERSION008"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamRemoteStorage_v009, FakeSteamInterfaces::RemoteStorageForVersion("STEAMREMOTESTORAGE_INTERFACE_VERSION009"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamRemoteStorage_v010, FakeSteamInterfaces::RemoteStorageForVersion("STEAMREMOTESTORAGE_INTERFACE_VERSION010"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamRemoteStorage_v011, FakeSteamInterfaces::RemoteStorageForVersion("STEAMREMOTESTORAGE_INTERFACE_VERSION011"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamRemoteStorage_v014, FakeSteamInterfaces::RemoteStorageForVersion("STEAMREMOTESTORAGE_INTERFACE_VERSION014"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamRemoteStorage_v013, FakeSteamInterfaces::RemoteStorageForVersion("STEAMREMOTESTORAGE_INTERFACE_VERSION013"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamRemoteStorage_v012, FakeSteamInterfaces::RemoteStorageForVersion("STEAMREMOTESTORAGE_INTERFACE_VERSION012"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamRemoteStorage_v016, FakeSteamInterfaces::RemoteStorageForVersion("STEAMREMOTESTORAGE_INTERFACE_VERSION016"))
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUserStats_v012, FakeSteamInterfaces::UserStats())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUserStats_v011, FakeSteamInterfaces::UserStatsV011())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUserStats_v010, FakeSteamInterfaces::UserStatsLegacy())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUserStats_v009, FakeSteamInterfaces::UserStatsLegacy())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUserStats_v008, FakeSteamInterfaces::UserStatsLegacy())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamApps_v008, FakeSteamCore::Apps())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamApps_v007, FakeSteamCore::Apps())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerApps_v008, FakeSteamCore::Apps())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamNetworking_v006, BreakersRevived_SteamNetworking())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamNetworking_v005, BreakersRevived_SteamNetworking())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerNetworking_v006, BreakersRevived_SteamGameServerNetworking())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerNetworking_v005, BreakersRevived_SteamGameServerNetworking())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamScreenshots_v003, FakeSteamInterfaces::Screenshots())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamMusic_v001, FakeSteamInterfaces::Music())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamMusicRemote_v001, FakeSteamInterfaces::MusicRemote())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamHTTP_v003, FakeSteamInterfaces::HTTP())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamHTTP_v002, FakeSteamInterfaces::HTTP())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerHTTP_v003, FakeSteamInterfaces::HTTP())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerHTTP_v002, FakeSteamInterfaces::HTTP())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamInput_v001, FakeSteamInterfaces::Input())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamInput_v002, FakeSteamInterfaces::Input())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamInput_v005, FakeSteamInterfaces::Input())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamInput_v006, FakeSteamInterfaces::Input())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamController_v007, FakeSteamInterfaces::Controller())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamController_v008, FakeSteamInterfaces::Controller())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamController_v006, FakeSteamInterfaces::Controller())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUGC_v014, FakeSteamInterfaces::UGC())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUGC_v013, FakeSteamInterfaces::UGC())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUGC_v012, FakeSteamInterfaces::UGC())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUGC_v015, FakeSteamInterfaces::UGC())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUGC_v016, FakeSteamInterfaces::UGC())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUGC_v017, FakeSteamInterfaces::UGC())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUGC_v018, FakeSteamInterfaces::UGC())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerUGC_v014, FakeSteamInterfaces::UGC())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerUGC_v013, FakeSteamInterfaces::UGC())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerUGC_v012, FakeSteamInterfaces::UGC())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerUGC_v015, FakeSteamInterfaces::UGC())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerUGC_v016, FakeSteamInterfaces::UGC())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerUGC_v017, FakeSteamInterfaces::UGC())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerUGC_v018, FakeSteamInterfaces::UGC())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamAppList_v001, FakeSteamInterfaces::AppList())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamHTMLSurface_v005, FakeSteamInterfaces::HTMLSurface())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamHTMLSurface_v004, FakeSteamInterfaces::HTMLSurface())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamInventory_v003, FakeSteamInterfaces::Inventory())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamInventory_v002, FakeSteamInterfaces::Inventory())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerInventory_v003, FakeSteamInterfaces::Inventory())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerInventory_v002, FakeSteamInterfaces::Inventory())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamVideo_v002, FakeSteamInterfaces::Video())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamVideo_v001, FakeSteamInterfaces::Video())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamTV_v001, FakeSteamInterfaces::TV())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamParentalSettings_v001, FakeSteamInterfaces::ParentalSettings())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamRemotePlay_v001, FakeSteamInterfaces::RemotePlay())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameCoordinator_v001, FakeSteamInterfaces::GameCoordinator())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamUnifiedMessages_v001, FakeSteamInterfaces::UnifiedMessages())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamAppTicket_v001, FakeSteamInterfaces::AppTicket())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamMasterServerUpdater_v001, FakeSteamInterfaces::MasterServerUpdater())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamNetworkingMessages_v002, BreakersRevived_SteamNetworkingMessages())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamNetworkingMessages_SteamAPI_v002, BreakersRevived_SteamNetworkingMessages())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerNetworkingMessages_v002, BreakersRevived_SteamNetworkingMessages())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002, BreakersRevived_SteamNetworkingMessages())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamNetworkingSockets_SteamAPI_v012, BreakersRevived_SteamNetworkingSockets())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v012, BreakersRevived_SteamGameServerNetworkingSockets())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamNetworkingSockets_SteamAPI_v011, BreakersRevived_SteamNetworkingSockets())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v011, BreakersRevived_SteamGameServerNetworkingSockets())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamNetworkingSockets_SteamAPI_v009, BreakersRevived_SteamNetworkingSockets())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v009, BreakersRevived_SteamGameServerNetworkingSockets())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamNetworkingSockets_v009, BreakersRevived_SteamNetworkingSockets())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerNetworkingSockets_v009, BreakersRevived_SteamGameServerNetworkingSockets())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamNetworkingSockets_v008, BreakersRevived_SteamNetworkingSockets())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerNetworkingSockets_v008, BreakersRevived_SteamGameServerNetworkingSockets())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamNetworkingUtils_SteamAPI_v003, BreakersRevived_SteamNetworkingUtils())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamNetworkingUtils_SteamAPI_v004, BreakersRevived_SteamNetworkingUtils())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamNetworkingUtils_v003, BreakersRevived_SteamNetworkingUtils())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServer_v013, FakeSteamCore::GameServer())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServer_v014, FakeSteamCore::GameServer())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServer_v015, FakeSteamCore::GameServer())
EXPORT_STEAM_ACCESSOR(SteamAPI_SteamGameServerStats_v001, FakeSteamCore::GameServerStats())

#undef EXPORT_STEAM_ACCESSOR

extern "C" __declspec(dllexport)
bool __cdecl SteamGameServer_Init(
    uint32_t ip,
    uint16_t steamPort,
    uint16_t gamePort,
    uint16_t queryPort,
    int serverMode,
    const char* versionString)
{
    NSR_UNUSED(ip);
    NSR_UNUSED(steamPort);
    NSR_UNUSED(gamePort);
    NSR_UNUSED(queryPort);
    NSR_UNUSED(serverMode);
    NSR_UNUSED(versionString);
    Logger::Info("SteamGameServer_Init: offline emulation success");
    return true;
}

extern "C" __declspec(dllexport) void __cdecl SteamGameServer_Shutdown() {}
extern "C" __declspec(dllexport) void __cdecl SteamGameServer_RunCallbacks() { SteamCallbackManager::RunCallbacks(); }

extern "C" __declspec(dllexport)
void* __cdecl SteamGameServer()
{
    return FakeSteamCore::GameServer();
}

extern "C" __declspec(dllexport)
void* __cdecl SteamGameServerStats()
{
    return FakeSteamCore::GameServerStats();
}

extern "C" __declspec(dllexport) void* __cdecl SteamGameServerNetworking() { return BreakersRevived_SteamGameServerNetworking(); }
extern "C" __declspec(dllexport) void* __cdecl SteamGameServerNetworkingSockets() { return BreakersRevived_SteamGameServerNetworkingSockets(); }

extern "C" __declspec(dllexport)
void* __cdecl SteamInternal_CreateInterface(const char* version)
{
    void* routed = SteamInterfaceRouter::RouteCreateInterface(version);
    if (routed)
        return routed;

    Logger::Error(std::string("SteamInternal_CreateInterface fallback for ") + (version ? version : "null"));
    return FakeSteamInterfaces::Fallback();
}

extern "C" __declspec(dllexport)
void* __cdecl SteamInternal_FindOrCreateUserInterface(HSteamUser user, const char* version)
{
    void* routed = SteamInterfaceRouter::RouteUserInterface(user, version);
    if (routed)
        return routed;

    Logger::Error(std::string("SteamInternal_FindOrCreateUserInterface fallback for ") + (version ? version : "null"));
    return FakeSteamInterfaces::Fallback();
}

extern "C" __declspec(dllexport)
void* __cdecl SteamInternal_FindOrCreateGameServerInterface(HSteamUser user, const char* version)
{
    void* routed = SteamInterfaceRouter::RouteGameServerInterface(user, version);
    if (routed)
        return routed;

    Logger::Error(std::string("SteamInternal_FindOrCreateGameServerInterface fallback for ") + (version ? version : "null"));
    return FakeSteamInterfaces::Fallback();
}

extern "C" __declspec(dllexport)
void* __cdecl SteamInternal_ContextInit(void* context)
{
    // Steamworks accessors pass three pointer-sized slots:
    // [0] initializer callback, [1] generation counter, [2] cached interface.
    // The SDK dereferences the address returned here as the interface pointer,
    // so returning the base structure makes it treat slot 0 (code) as an object.
    if (!context)
        return nullptr;

    auto** slots = static_cast<void**>(context);
    using InitFn = void(__cdecl*)(void*);

    if (!slots[2])
    {
        const auto init = reinterpret_cast<InitFn>(slots[0]);
        if (!init)
        {
            Logger::Error("SteamInternal_ContextInit: missing initializer callback");
            return &slots[2];
        }

        init(&slots[2]);
        slots[1] = reinterpret_cast<void*>(static_cast<uintptr_t>(1));

        SteamDiagnostics::MarkSteam(
            "ContextInit",
            std::string("interface=") + (slots[2] ? "ready" : "null"));
    }

    return &slots[2];
}

extern "C" __declspec(dllexport)
void __cdecl SteamInternal_GameServer_Init()
{
    Logger::Info("SteamInternal_GameServer_Init: offline emulation");
}

extern "C" __declspec(dllexport)
void __cdecl Breakpad_SteamWriteMiniDumpSetComment(const char* comment)
{
    NSR_UNUSED(comment);
}

extern "C" __declspec(dllexport)
void __cdecl Breakpad_SteamMiniDumpInit(
    uint32_t appId,
    const char* version,
    const char* date,
    const char* time,
    bool fullMemoryDumps,
    void* context,
    void* callback)
{
    NSR_UNUSED(appId);
    NSR_UNUSED(version);
    NSR_UNUSED(date);
    NSR_UNUSED(time);
    NSR_UNUSED(fullMemoryDumps);
    NSR_UNUSED(context);
    NSR_UNUSED(callback);
}
