#include "StdInc.h"
#include "FakeSteamCore.h"
#include "FakeSteamInterfaces.h"
#include "Logger.h"
#include "Auth/SteamAuth.h"
#include "SteamCallbackManager.h"
#include "SteamCallResultManager.h"
#include "SteamConfig.h"
#include "SteamIDManager.h"
#include "SteamStatsLocal.h"
#include "SteamStorageLocal.h"
#include "SteamVersionLogger.h"

#include <cctype>
#include <ctime>

extern "C" void* __cdecl BreakersRevived_SteamNetworking();
extern "C" void* __cdecl BreakersRevived_SteamGameServerNetworking();
extern "C" void* __cdecl BreakersRevived_SteamNetworkingUtils();
extern "C" void* __cdecl BreakersRevived_SteamNetworkingMessages();
extern "C" void* __cdecl BreakersRevived_SteamNetworkingSockets();
extern "C" void* __cdecl BreakersRevived_SteamGameServerNetworkingSockets();

namespace
{
    constexpr HSteamUser kLocalUser = 1;
    constexpr HSteamPipe kLocalPipe = 1;
    constexpr AppId_t kBreakersAppId = 1276760;

    bool g_Initialized = false;
    std::string g_InstallPath;
    std::string g_UserDataFolder;
    std::vector<unsigned char> g_LastEncryptedTicket;

#pragma pack(push, 1)
    struct OfflineSteamIPAddress
    {
        union
        {
            uint32_t m_unIPv4;
            uint8_t m_rgubIPv6[16];
            uint64_t m_ipv6Qword[2];
        };

        int m_eType;
    };
#pragma pack(pop)

    std::string ExeFolder()
    {
        char path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path().string();
    }

    void CopyString(char* out, int outSize, const std::string& value)
    {
        if (!out || outSize <= 0)
            return;

        strncpy_s(out, static_cast<size_t>(outSize), value.c_str(), _TRUNCATE);
    }

    std::string UpperInterfaceName(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return value;
    }

    bool HasInterface(const std::string& value, const char* token)
    {
        return value.find(token) != std::string::npos;
    }

    bool IsOfflineAvatarImage(int image)
    {
        return image >= 1 && image <= 3;
    }

    uint32_t OfflineAvatarSize(int image)
    {
        switch (image)
        {
        case 1: return 32;
        case 2: return 64;
        case 3: return 128;
        default: return 0;
        }
    }

    void WriteOfflineAvatarRGBA(int image, uint8_t* dest, uint32_t edge)
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

    bool IsWritableMemory(void* ptr, size_t size)
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

    void SafeWriteUInt32(uint32_t* ptr, uint32_t value)
    {
        if (IsWritableMemory(ptr, sizeof(uint32_t)))
            *ptr = value;
    }

    void TraceCore(const char* interfaceName, const char* functionName, const std::string& detail = std::string())
    {
        SteamVersionLogger::LogCall(interfaceName, functionName, detail);
    }

    HSteamUser OfflineGetHSteamUser()
    {
        TraceCore("SteamUser", "GetHSteamUser");
        return kLocalUser;
    }

    bool OfflineBLoggedOn()
    {
        TraceCore("SteamUser", "BLoggedOn");
        return true;
    }

    CSteamID OfflineSteamIDObjectReturn(CSteamID steamID)
    {
        thread_local uint64_t s_ReturnSteamID = 0;
        s_ReturnSteamID = static_cast<uint64_t>(steamID);
        return static_cast<CSteamID>(reinterpret_cast<uintptr_t>(&s_ReturnSteamID));
    }

    CSteamID OfflineGetSteamID()
    {
        const CSteamID steamID = SteamIDManager::GetLocalSteamID();
        const CSteamID objectReturn = OfflineSteamIDObjectReturn(steamID);
        TraceCore(
            "SteamUser",
            "GetSteamID",
            "id=" +
            std::to_string(static_cast<unsigned long long>(steamID)) +
            " object=" +
            std::to_string(static_cast<unsigned long long>(objectReturn)));
        return objectReturn;
    }

    int OfflineInitiateGameConnection(void* authBlob, int maxAuthBlob, CSteamID server, uint32_t ip, uint16_t port, bool secure)
    {
        TraceCore("SteamUser", "InitiateGameConnection", "bytes=" + std::to_string(maxAuthBlob));
        NSR_UNUSED(server);
        NSR_UNUSED(ip);
        NSR_UNUSED(port);
        NSR_UNUSED(secure);

        uint32_t size = 0;
        SteamAuth::GetAuthSessionTicket(authBlob, maxAuthBlob, &size);
        return static_cast<int>(size);
    }

    void OfflineTerminateGameConnection(uint32_t ip, uint16_t port)
    {
        TraceCore("SteamUser", "TerminateGameConnection", "port=" + std::to_string(port));
        NSR_UNUSED(ip);
        NSR_UNUSED(port);
    }

    void OfflineTrackAppUsageEvent(uint64_t gameID, int event, const char* extra)
    {
        TraceCore("SteamUser", "TrackAppUsageEvent", extra ? extra : "");
        NSR_UNUSED(gameID);
        NSR_UNUSED(event);
        NSR_UNUSED(extra);
    }

    bool OfflineGetUserDataFolder(char* buffer, int bufferSize)
    {
        TraceCore("SteamUser", "GetUserDataFolder", "bytes=" + std::to_string(bufferSize));
        CopyString(buffer, bufferSize, g_UserDataFolder);
        return buffer && bufferSize > 0;
    }

    void OfflineStartVoiceRecording()
    {
        TraceCore("SteamUser", "StartVoiceRecording");
    }

    void OfflineStopVoiceRecording()
    {
        TraceCore("SteamUser", "StopVoiceRecording");
    }

    int OfflineGetCompressedVoice(void* dest, uint32_t destSize, uint32_t* written)
    {
        TraceCore("SteamUser", "GetCompressedVoice", "bytes=" + std::to_string(destSize));
        NSR_UNUSED(dest);
        SafeWriteUInt32(written, 0);
        return 3;
    }

    int OfflineDecompressVoice(const void* compressed, uint32_t compressedSize, void* dest, uint32_t destSize, uint32_t* written)
    {
        TraceCore("SteamUser", "DecompressVoiceLegacy", "bytes=" + std::to_string(compressedSize));
        NSR_UNUSED(compressed);
        NSR_UNUSED(dest);
        NSR_UNUSED(destSize);
        SafeWriteUInt32(written, 0);
        return 3;
    }

    int OfflineGetAvailableVoice(uint32_t* compressed, uint32_t* uncompressed, uint32_t sampleRate)
    {
        TraceCore("SteamUser", "GetAvailableVoice", "rate=" + std::to_string(sampleRate));
        SafeWriteUInt32(compressed, 0);
        SafeWriteUInt32(uncompressed, 0);
        return 3;
    }

    int OfflineGetVoice(bool wantCompressed, void* dest, uint32_t destSize, uint32_t* written, bool wantUncompressed, void* uncompressedDest, uint32_t uncompressedSize, uint32_t* uncompressedWritten, uint32_t sampleRate)
    {
        TraceCore("SteamUser", "GetVoice", "rate=" + std::to_string(sampleRate));
        NSR_UNUSED(wantCompressed);
        NSR_UNUSED(dest);
        NSR_UNUSED(destSize);
        NSR_UNUSED(wantUncompressed);
        NSR_UNUSED(uncompressedDest);
        NSR_UNUSED(uncompressedSize);
        SafeWriteUInt32(written, 0);
        SafeWriteUInt32(uncompressedWritten, 0);
        return 3;
    }

    int OfflineDecompressVoiceModern(const void* compressed, uint32_t compressedSize, void* dest, uint32_t destSize, uint32_t* written, uint32_t sampleRate)
    {
        TraceCore("SteamUser", "DecompressVoice", "rate=" + std::to_string(sampleRate));
        NSR_UNUSED(compressed);
        NSR_UNUSED(compressedSize);
        NSR_UNUSED(dest);
        NSR_UNUSED(destSize);
        SafeWriteUInt32(written, 0);
        return 3;
    }

    uint32_t OfflineGetVoiceOptimalSampleRate()
    {
        TraceCore("SteamUser", "GetVoiceOptimalSampleRate");
        return 48000;
    }

    uint32_t OfflineGetAuthSessionTicket(void* ticket, int maxTicket, uint32_t* ticketSize)
    {
        TraceCore("SteamUser", "GetAuthSessionTicket", "bytes=" + std::to_string(maxTicket));
        return SteamAuth::GetAuthSessionTicket(ticket, maxTicket, ticketSize);
    }

    int OfflineBeginAuthSession(const void* ticket, int ticketSize, CSteamID steamID)
    {
        TraceCore("SteamUser", "BeginAuthSession", "bytes=" + std::to_string(ticketSize));
        return SteamAuth::BeginAuthSession(ticket, ticketSize, steamID);
    }

    void OfflineEndAuthSession(CSteamID steamID)
    {
        TraceCore("SteamUser", "EndAuthSession");
        SteamAuth::EndAuthSession(steamID);
    }

    void OfflineCancelAuthTicket(uint32_t ticketHandle)
    {
        TraceCore("SteamUser", "CancelAuthTicket", std::to_string(ticketHandle));
        SteamAuth::CancelAuthTicket(ticketHandle);
    }

    int OfflineUserHasLicenseForApp(CSteamID steamID, AppId_t appID)
    {
        TraceCore("SteamUser", "UserHasLicenseForApp", "app=" + std::to_string(appID));
        NSR_UNUSED(steamID);
        NSR_UNUSED(appID);
        return 0;
    }

    bool OfflineBIsBehindNAT()
    {
        TraceCore("SteamUser", "BIsBehindNAT");
        return false;
    }

    void OfflineAdvertiseGame(CSteamID server, uint32_t ip, uint16_t port)
    {
        TraceCore("SteamUser", "AdvertiseGame", "port=" + std::to_string(port));
        NSR_UNUSED(server);
        NSR_UNUSED(ip);
        NSR_UNUSED(port);
    }

    SteamAPICall_t OfflineRequestEncryptedAppTicket(void* data, int dataSize)
    {
        TraceCore("SteamUser", "RequestEncryptedAppTicket", "bytes=" + std::to_string(dataSize));
        const char prefix[] = "BREAKERSREVIVED_ENCRYPTED_APP_TICKET";
        g_LastEncryptedTicket.assign(prefix, prefix + sizeof(prefix));
        if (data && dataSize > 0)
        {
            const auto* bytes = static_cast<const unsigned char*>(data);
            g_LastEncryptedTicket.insert(g_LastEncryptedTicket.end(), bytes, bytes + dataSize);
        }

        struct Result { int m_eResult; };
        Result result{ 1 };
        return SteamCallResultManager::CreateCallResult(154, &result, sizeof(result), true);
    }

    bool OfflineGetEncryptedAppTicket(void* ticket, int maxTicket, uint32_t* ticketSize)
    {
        TraceCore("SteamUser", "GetEncryptedAppTicket", "bytes=" + std::to_string(maxTicket));
        if (g_LastEncryptedTicket.empty())
            OfflineRequestEncryptedAppTicket(nullptr, 0);

        if (ticketSize)
            *ticketSize = static_cast<uint32_t>(g_LastEncryptedTicket.size());

        if (!ticket || maxTicket < static_cast<int>(g_LastEncryptedTicket.size()))
            return false;

        memcpy(ticket, g_LastEncryptedTicket.data(), g_LastEncryptedTicket.size());
        return true;
    }

    int OfflineGetGameBadgeLevel(int series, bool foil)
    {
        TraceCore("SteamUser", "GetGameBadgeLevel", "series=" + std::to_string(series));
        NSR_UNUSED(series);
        NSR_UNUSED(foil);
        return 0;
    }

    int OfflineGetPlayerSteamLevel()
    {
        TraceCore("SteamUser", "GetPlayerSteamLevel");
        return 1;
    }

    SteamAPICall_t OfflineRequestStoreAuthURL(const char* redirect)
    {
        TraceCore("SteamUser", "RequestStoreAuthURL", redirect ? redirect : "");
        struct Result { char m_szURL[512]; };
        Result result{};
        strcpy_s(result.m_szURL, "about:blank");
        return SteamCallResultManager::CreateCallResult(165, &result, sizeof(result), true);
    }

    SteamAPICall_t OfflineGetMarketEligibility()
    {
        TraceCore("SteamUser", "GetMarketEligibility");
        return SteamCallResultManager::CreateCallResult(166);
    }

    SteamAPICall_t OfflineGetDurationControl()
    {
        TraceCore("SteamUser", "GetDurationControl");
        return SteamCallResultManager::CreateCallResult(167);
    }

    void* RouteVersion(const char* version)
    {
        std::string v = version ? version : "";
        std::string u = UpperInterfaceName(v);
        SteamVersionLogger::LogInterfaceRequest("FakeSteamCore::RouteVersion", v.c_str());

        if (HasInterface(u, "STEAMNETWORKINGMESSAGES")) return BreakersRevived_SteamNetworkingMessages();
        if (HasInterface(u, "STEAMNETWORKINGSOCKETS")) return BreakersRevived_SteamNetworkingSockets();
        if (HasInterface(u, "STEAMNETWORKINGUTILS")) return BreakersRevived_SteamNetworkingUtils();
        if (HasInterface(u, "STEAMNETWORKING")) return BreakersRevived_SteamNetworking();

        if (HasInterface(u, "STEAMGAMECOORDINATOR")) return FakeSteamInterfaces::GameCoordinator();
        if (HasInterface(u, "STEAMUNIFIEDMESSAGES")) return FakeSteamInterfaces::UnifiedMessages();
        if (HasInterface(u, "STEAMAPPTICKET")) return FakeSteamInterfaces::AppTicket();
        if (HasInterface(u, "STEAMTV")) return FakeSteamInterfaces::TV();
        if (HasInterface(u, "STEAMMASTERSERVERUPDATER")) return FakeSteamInterfaces::MasterServerUpdater();

        if (HasInterface(u, "STEAMCLIENT")) return FakeSteamCore::Client();
        if (HasInterface(u, "STEAMGAMESERVERSTATS")) return FakeSteamCore::GameServerStats();
        if (HasInterface(u, "STEAMGAMESERVER")) return FakeSteamCore::GameServer();
        if (HasInterface(u, "STEAMUSERSTATS"))
        {
            if (HasInterface(u, "009") || HasInterface(u, "010") || HasInterface(u, "008"))
                return FakeSteamInterfaces::UserStatsLegacy();

            if (HasInterface(u, "011"))
                return FakeSteamInterfaces::UserStatsV011();

            return FakeSteamInterfaces::UserStats();
        }
        if (HasInterface(u, "STEAMUSER")) return FakeSteamCore::UserForVersion(v.c_str());
        if (HasInterface(u, "STEAMUTILS")) return FakeSteamCore::Utils();
        if (HasInterface(u, "STEAMAPPS")) return FakeSteamCore::Apps();

        if (HasInterface(u, "STEAMFRIENDS")) return FakeSteamInterfaces::FriendsForVersion(v.c_str());
        if (HasInterface(u, "STEAMMATCHMAKINGSERVERS")) return FakeSteamInterfaces::MatchmakingServers();
        if (HasInterface(u, "STEAMMATCHMAKING"))
        {
            if (HasInterface(u, "006"))
                return FakeSteamInterfaces::MatchmakingV006();

            if (HasInterface(u, "007"))
                return FakeSteamInterfaces::MatchmakingV007();

            if (HasInterface(u, "008"))
                return FakeSteamInterfaces::MatchmakingV008();

            if (HasInterface(u, "009"))
                return FakeSteamInterfaces::MatchmakingV009();

            return FakeSteamInterfaces::Matchmaking();
        }
        if (HasInterface(u, "STEAMREMOTESTORAGE")) return FakeSteamInterfaces::RemoteStorageForVersion(v.c_str());
        if (HasInterface(u, "STEAMSCREENSHOTS")) return FakeSteamInterfaces::Screenshots();
        if (HasInterface(u, "STEAMHTTP")) return FakeSteamInterfaces::HTTP();
        if (HasInterface(u, "STEAMCONTROLLER")) return FakeSteamInterfaces::Controller();
        if (HasInterface(u, "STEAMUGC")) return FakeSteamInterfaces::UGC();
        if (HasInterface(u, "STEAMAPPLIST")) return FakeSteamInterfaces::AppList();
        if (HasInterface(u, "STEAMMUSICREMOTE")) return FakeSteamInterfaces::MusicRemote();
        if (HasInterface(u, "STEAMMUSIC")) return FakeSteamInterfaces::Music();
        if (HasInterface(u, "STEAMHTMLSURFACE")) return FakeSteamInterfaces::HTMLSurface();
        if (HasInterface(u, "STEAMINVENTORY")) return FakeSteamInterfaces::Inventory();
        if (HasInterface(u, "STEAMVIDEO")) return FakeSteamInterfaces::Video();
        if (HasInterface(u, "STEAMPARENTALSETTINGS")) return FakeSteamInterfaces::ParentalSettings();
        if (HasInterface(u, "STEAMGAMESEARCH")) return FakeSteamInterfaces::GameSearch();
        if (HasInterface(u, "STEAMINPUT")) return FakeSteamInterfaces::Input();
        if (HasInterface(u, "STEAMPARTIES")) return FakeSteamInterfaces::Parties();
        if (HasInterface(u, "STEAMREMOTEPLAY")) return FakeSteamInterfaces::RemotePlay();

        Logger::Error("SteamClient: using offline fallback for " + v);
        return FakeSteamInterfaces::Fallback();
    }

    class OfflineSteamUser final
    {
    public:
        virtual HSteamUser GetHSteamUser() { return OfflineGetHSteamUser(); }
        virtual bool BLoggedOn() { return OfflineBLoggedOn(); }
        virtual CSteamID GetSteamID() { return OfflineGetSteamID(); }

        virtual int InitiateGameConnection(void* authBlob, int maxAuthBlob, CSteamID server, uint32_t ip, uint16_t port, bool secure)
        {
            return OfflineInitiateGameConnection(authBlob, maxAuthBlob, server, ip, port, secure);
        }

        virtual void TerminateGameConnection(uint32_t ip, uint16_t port)
        {
            OfflineTerminateGameConnection(ip, port);
        }

        virtual void TrackAppUsageEvent(uint64_t gameID, int event, const char* extra)
        {
            OfflineTrackAppUsageEvent(gameID, event, extra);
        }

        virtual bool GetUserDataFolder(char* buffer, int bufferSize)
        {
            return OfflineGetUserDataFolder(buffer, bufferSize);
        }

        virtual void StartVoiceRecording() { OfflineStartVoiceRecording(); }
        virtual void StopVoiceRecording() { OfflineStopVoiceRecording(); }

        virtual int GetAvailableVoice(uint32_t* compressed, uint32_t* uncompressed, uint32_t sampleRate)
        {
            return OfflineGetAvailableVoice(compressed, uncompressed, sampleRate);
        }

        virtual int GetVoice(bool wantCompressed, void* dest, uint32_t destSize, uint32_t* written, bool wantUncompressed, void* uncompressedDest, uint32_t uncompressedSize, uint32_t* uncompressedWritten, uint32_t sampleRate)
        {
            return OfflineGetVoice(wantCompressed, dest, destSize, written, wantUncompressed, uncompressedDest, uncompressedSize, uncompressedWritten, sampleRate);
        }

        virtual int DecompressVoice(const void* compressed, uint32_t compressedSize, void* dest, uint32_t destSize, uint32_t* written, uint32_t sampleRate)
        {
            return OfflineDecompressVoiceModern(compressed, compressedSize, dest, destSize, written, sampleRate);
        }

        virtual uint32_t GetVoiceOptimalSampleRate() { return OfflineGetVoiceOptimalSampleRate(); }
        virtual uint32_t GetAuthSessionTicket(void* ticket, int maxTicket, uint32_t* ticketSize, const void* identity)
        {
            NSR_UNUSED(identity);
            return OfflineGetAuthSessionTicket(ticket, maxTicket, ticketSize);
        }
        virtual uint32_t GetAuthTicketForWebApi(const char* identity)
        {
            TraceCore("SteamUser", "GetAuthTicketForWebApi", identity ? identity : "null");
            NSR_UNUSED(identity);
            return SteamAuth::GetAuthTicketForWebApi(identity);
        }
        virtual int BeginAuthSession(const void* ticket, int ticketSize, CSteamID steamID) { return OfflineBeginAuthSession(ticket, ticketSize, steamID); }
        virtual void EndAuthSession(CSteamID steamID) { OfflineEndAuthSession(steamID); }
        virtual void CancelAuthTicket(uint32_t ticketHandle) { OfflineCancelAuthTicket(ticketHandle); }
        virtual int UserHasLicenseForApp(CSteamID steamID, AppId_t appID)
        {
            return OfflineUserHasLicenseForApp(steamID, appID);
        }
        virtual bool BIsBehindNAT() { return OfflineBIsBehindNAT(); }
        virtual void AdvertiseGame(CSteamID server, uint32_t ip, uint16_t port)
        {
            OfflineAdvertiseGame(server, ip, port);
        }
        virtual SteamAPICall_t RequestEncryptedAppTicket(void* data, int dataSize)
        {
            return OfflineRequestEncryptedAppTicket(data, dataSize);
        }
        virtual bool GetEncryptedAppTicket(void* ticket, int maxTicket, uint32_t* ticketSize)
        {
            return OfflineGetEncryptedAppTicket(ticket, maxTicket, ticketSize);
        }
        virtual int GetGameBadgeLevel(int series, bool foil)
        {
            NSR_UNUSED(series);
            NSR_UNUSED(foil);
            return 0;
        }
        virtual int GetPlayerSteamLevel() { return 1; }
        virtual SteamAPICall_t RequestStoreAuthURL(const char* redirect)
        {
            NSR_UNUSED(redirect);
            struct Result { char m_szURL[512]; };
            Result result{};
            strcpy_s(result.m_szURL, "about:blank");
            return SteamCallResultManager::CreateCallResult(165, &result, sizeof(result), true);
        }
        virtual bool BIsPhoneVerified() { return true; }
        virtual bool BIsTwoFactorEnabled() { return false; }
        virtual bool BIsPhoneIdentifying() { return false; }
        virtual bool BIsPhoneRequiringVerification() { return false; }
        virtual SteamAPICall_t GetMarketEligibility() { return SteamCallResultManager::CreateCallResult(166); }
        virtual SteamAPICall_t GetDurationControl() { return SteamCallResultManager::CreateCallResult(167); }
        virtual bool BSetDurationControlOnlineState(int state)
        {
            NSR_UNUSED(state);
            return true;
        }
    };

    class OfflineSteamUser012 final
    {
    public:
        virtual HSteamUser GetHSteamUser() { return OfflineGetHSteamUser(); }
        virtual bool BLoggedOn() { return OfflineBLoggedOn(); }
        virtual CSteamID GetSteamID() { return OfflineGetSteamID(); }
        virtual int InitiateGameConnection(void* authBlob, int maxAuthBlob, CSteamID server, uint32_t ip, uint16_t port, bool secure)
        {
            return OfflineInitiateGameConnection(authBlob, maxAuthBlob, server, ip, port, secure);
        }
        virtual void TerminateGameConnection(uint32_t ip, uint16_t port) { OfflineTerminateGameConnection(ip, port); }
        virtual void TrackAppUsageEvent(uint64_t gameID, int event, const char* extra) { OfflineTrackAppUsageEvent(gameID, event, extra); }
        virtual bool GetUserDataFolder(char* buffer, int bufferSize) { return OfflineGetUserDataFolder(buffer, bufferSize); }
        virtual void StartVoiceRecording() { OfflineStartVoiceRecording(); }
        virtual void StopVoiceRecording() { OfflineStopVoiceRecording(); }
        virtual int GetCompressedVoice(void* dest, uint32_t destSize, uint32_t* written) { return OfflineGetCompressedVoice(dest, destSize, written); }
        virtual int DecompressVoice(void* compressed, uint32_t compressedSize, void* dest, uint32_t destSize, uint32_t* written)
        {
            return OfflineDecompressVoice(compressed, compressedSize, dest, destSize, written);
        }
        virtual uint32_t GetAuthSessionTicket(void* ticket, int maxTicket, uint32_t* ticketSize) { return OfflineGetAuthSessionTicket(ticket, maxTicket, ticketSize); }
        virtual int BeginAuthSession(const void* ticket, int ticketSize, CSteamID steamID) { return OfflineBeginAuthSession(ticket, ticketSize, steamID); }
        virtual void EndAuthSession(CSteamID steamID) { OfflineEndAuthSession(steamID); }
        virtual void CancelAuthTicket(uint32_t ticketHandle) { OfflineCancelAuthTicket(ticketHandle); }
        virtual int UserHasLicenseForApp(CSteamID steamID, AppId_t appID) { return OfflineUserHasLicenseForApp(steamID, appID); }
    };

    class OfflineSteamUser016 final
    {
    public:
        virtual HSteamUser GetHSteamUser() { return OfflineGetHSteamUser(); }
        virtual bool BLoggedOn() { return OfflineBLoggedOn(); }
        virtual CSteamID GetSteamID() { return OfflineGetSteamID(); }
        virtual int InitiateGameConnection(void* authBlob, int maxAuthBlob, CSteamID server, uint32_t ip, uint16_t port, bool secure)
        {
            return OfflineInitiateGameConnection(authBlob, maxAuthBlob, server, ip, port, secure);
        }
        virtual void TerminateGameConnection(uint32_t ip, uint16_t port) { OfflineTerminateGameConnection(ip, port); }
        virtual void TrackAppUsageEvent(uint64_t gameID, int event, const char* extra) { OfflineTrackAppUsageEvent(gameID, event, extra); }
        virtual bool GetUserDataFolder(char* buffer, int bufferSize) { return OfflineGetUserDataFolder(buffer, bufferSize); }
        virtual void StartVoiceRecording() { OfflineStartVoiceRecording(); }
        virtual void StopVoiceRecording() { OfflineStopVoiceRecording(); }
        virtual int GetAvailableVoice(uint32_t* compressed, uint32_t* uncompressed, uint32_t sampleRate)
        {
            return OfflineGetAvailableVoice(compressed, uncompressed, sampleRate);
        }
        virtual int GetVoice(bool wantCompressed, void* dest, uint32_t destSize, uint32_t* written, bool wantUncompressed, void* uncompressedDest, uint32_t uncompressedSize, uint32_t* uncompressedWritten, uint32_t sampleRate)
        {
            return OfflineGetVoice(wantCompressed, dest, destSize, written, wantUncompressed, uncompressedDest, uncompressedSize, uncompressedWritten, sampleRate);
        }
        virtual int DecompressVoice(const void* compressed, uint32_t compressedSize, void* dest, uint32_t destSize, uint32_t* written, uint32_t sampleRate)
        {
            return OfflineDecompressVoiceModern(compressed, compressedSize, dest, destSize, written, sampleRate);
        }
        virtual uint32_t GetVoiceOptimalSampleRate() { return OfflineGetVoiceOptimalSampleRate(); }
        virtual uint32_t GetAuthSessionTicket(void* ticket, int maxTicket, uint32_t* ticketSize) { return OfflineGetAuthSessionTicket(ticket, maxTicket, ticketSize); }
        virtual int BeginAuthSession(const void* ticket, int ticketSize, CSteamID steamID) { return OfflineBeginAuthSession(ticket, ticketSize, steamID); }
        virtual void EndAuthSession(CSteamID steamID) { OfflineEndAuthSession(steamID); }
        virtual void CancelAuthTicket(uint32_t ticketHandle) { OfflineCancelAuthTicket(ticketHandle); }
        virtual int UserHasLicenseForApp(CSteamID steamID, AppId_t appID) { return OfflineUserHasLicenseForApp(steamID, appID); }
        virtual bool BIsBehindNAT() { return OfflineBIsBehindNAT(); }
        virtual void AdvertiseGame(CSteamID server, uint32_t ip, uint16_t port) { OfflineAdvertiseGame(server, ip, port); }
        virtual SteamAPICall_t RequestEncryptedAppTicket(void* data, int dataSize) { return OfflineRequestEncryptedAppTicket(data, dataSize); }
        virtual bool GetEncryptedAppTicket(void* ticket, int maxTicket, uint32_t* ticketSize) { return OfflineGetEncryptedAppTicket(ticket, maxTicket, ticketSize); }
    };

    class OfflineSteamUser017 final
    {
    public:
        virtual HSteamUser GetHSteamUser() { return OfflineGetHSteamUser(); }
        virtual bool BLoggedOn() { return OfflineBLoggedOn(); }
        virtual CSteamID GetSteamID() { return OfflineGetSteamID(); }
        virtual int InitiateGameConnection(void* authBlob, int maxAuthBlob, CSteamID server, uint32_t ip, uint16_t port, bool secure)
        {
            return OfflineInitiateGameConnection(authBlob, maxAuthBlob, server, ip, port, secure);
        }
        virtual void TerminateGameConnection(uint32_t ip, uint16_t port) { OfflineTerminateGameConnection(ip, port); }
        virtual void TrackAppUsageEvent(uint64_t gameID, int event, const char* extra) { OfflineTrackAppUsageEvent(gameID, event, extra); }
        virtual bool GetUserDataFolder(char* buffer, int bufferSize) { return OfflineGetUserDataFolder(buffer, bufferSize); }
        virtual void StartVoiceRecording() { OfflineStartVoiceRecording(); }
        virtual void StopVoiceRecording() { OfflineStopVoiceRecording(); }
        virtual int GetAvailableVoice(uint32_t* compressed, uint32_t* uncompressed, uint32_t sampleRate)
        {
            return OfflineGetAvailableVoice(compressed, uncompressed, sampleRate);
        }
        virtual int GetVoice(bool wantCompressed, void* dest, uint32_t destSize, uint32_t* written, bool wantUncompressed, void* uncompressedDest, uint32_t uncompressedSize, uint32_t* uncompressedWritten, uint32_t sampleRate)
        {
            return OfflineGetVoice(wantCompressed, dest, destSize, written, wantUncompressed, uncompressedDest, uncompressedSize, uncompressedWritten, sampleRate);
        }
        virtual int DecompressVoice(const void* compressed, uint32_t compressedSize, void* dest, uint32_t destSize, uint32_t* written, uint32_t sampleRate)
        {
            return OfflineDecompressVoiceModern(compressed, compressedSize, dest, destSize, written, sampleRate);
        }
        virtual uint32_t GetVoiceOptimalSampleRate() { return OfflineGetVoiceOptimalSampleRate(); }
        virtual uint32_t GetAuthSessionTicket(void* ticket, int maxTicket, uint32_t* ticketSize) { return OfflineGetAuthSessionTicket(ticket, maxTicket, ticketSize); }
        virtual int BeginAuthSession(const void* ticket, int ticketSize, CSteamID steamID) { return OfflineBeginAuthSession(ticket, ticketSize, steamID); }
        virtual void EndAuthSession(CSteamID steamID) { OfflineEndAuthSession(steamID); }
        virtual void CancelAuthTicket(uint32_t ticketHandle) { OfflineCancelAuthTicket(ticketHandle); }
        virtual int UserHasLicenseForApp(CSteamID steamID, AppId_t appID) { return OfflineUserHasLicenseForApp(steamID, appID); }
        virtual bool BIsBehindNAT() { return OfflineBIsBehindNAT(); }
        virtual void AdvertiseGame(CSteamID server, uint32_t ip, uint16_t port) { OfflineAdvertiseGame(server, ip, port); }
        virtual SteamAPICall_t RequestEncryptedAppTicket(void* data, int dataSize) { return OfflineRequestEncryptedAppTicket(data, dataSize); }
        virtual bool GetEncryptedAppTicket(void* ticket, int maxTicket, uint32_t* ticketSize) { return OfflineGetEncryptedAppTicket(ticket, maxTicket, ticketSize); }
        virtual int GetGameBadgeLevel(int series, bool foil) { return OfflineGetGameBadgeLevel(series, foil); }
        virtual int GetPlayerSteamLevel() { return OfflineGetPlayerSteamLevel(); }
    };

    class OfflineSteamUser021 final
    {
    public:
        virtual HSteamUser GetHSteamUser() { return OfflineGetHSteamUser(); }
        virtual bool BLoggedOn() { return OfflineBLoggedOn(); }
        virtual CSteamID GetSteamID() { return OfflineGetSteamID(); }
        virtual int InitiateGameConnection(void* authBlob, int maxAuthBlob, CSteamID server, uint32_t ip, uint16_t port, bool secure)
        {
            return OfflineInitiateGameConnection(authBlob, maxAuthBlob, server, ip, port, secure);
        }
        virtual void TerminateGameConnection(uint32_t ip, uint16_t port) { OfflineTerminateGameConnection(ip, port); }
        virtual void TrackAppUsageEvent(uint64_t gameID, int event, const char* extra) { OfflineTrackAppUsageEvent(gameID, event, extra); }
        virtual bool GetUserDataFolder(char* buffer, int bufferSize) { return OfflineGetUserDataFolder(buffer, bufferSize); }
        virtual void StartVoiceRecording() { OfflineStartVoiceRecording(); }
        virtual void StopVoiceRecording() { OfflineStopVoiceRecording(); }
        virtual int GetAvailableVoice(uint32_t* compressed, uint32_t* uncompressed, uint32_t sampleRate)
        {
            return OfflineGetAvailableVoice(compressed, uncompressed, sampleRate);
        }
        virtual int GetVoice(bool wantCompressed, void* dest, uint32_t destSize, uint32_t* written, bool wantUncompressed, void* uncompressedDest, uint32_t uncompressedSize, uint32_t* uncompressedWritten, uint32_t sampleRate)
        {
            return OfflineGetVoice(wantCompressed, dest, destSize, written, wantUncompressed, uncompressedDest, uncompressedSize, uncompressedWritten, sampleRate);
        }
        virtual int DecompressVoice(const void* compressed, uint32_t compressedSize, void* dest, uint32_t destSize, uint32_t* written, uint32_t sampleRate)
        {
            return OfflineDecompressVoiceModern(compressed, compressedSize, dest, destSize, written, sampleRate);
        }
        virtual uint32_t GetVoiceOptimalSampleRate() { return OfflineGetVoiceOptimalSampleRate(); }
        virtual uint32_t GetAuthSessionTicket(void* ticket, int maxTicket, uint32_t* ticketSize) { return OfflineGetAuthSessionTicket(ticket, maxTicket, ticketSize); }
        virtual int BeginAuthSession(const void* ticket, int ticketSize, CSteamID steamID) { return OfflineBeginAuthSession(ticket, ticketSize, steamID); }
        virtual void EndAuthSession(CSteamID steamID) { OfflineEndAuthSession(steamID); }
        virtual void CancelAuthTicket(uint32_t ticketHandle) { OfflineCancelAuthTicket(ticketHandle); }
        virtual int UserHasLicenseForApp(CSteamID steamID, AppId_t appID) { return OfflineUserHasLicenseForApp(steamID, appID); }
        virtual bool BIsBehindNAT() { return OfflineBIsBehindNAT(); }
        virtual void AdvertiseGame(CSteamID server, uint32_t ip, uint16_t port) { OfflineAdvertiseGame(server, ip, port); }
        virtual SteamAPICall_t RequestEncryptedAppTicket(void* data, int dataSize) { return OfflineRequestEncryptedAppTicket(data, dataSize); }
        virtual bool GetEncryptedAppTicket(void* ticket, int maxTicket, uint32_t* ticketSize) { return OfflineGetEncryptedAppTicket(ticket, maxTicket, ticketSize); }
        virtual int GetGameBadgeLevel(int series, bool foil) { return OfflineGetGameBadgeLevel(series, foil); }
        virtual int GetPlayerSteamLevel() { return OfflineGetPlayerSteamLevel(); }
        virtual SteamAPICall_t RequestStoreAuthURL(const char* redirect) { return OfflineRequestStoreAuthURL(redirect); }
        virtual bool BIsPhoneVerified() { return true; }
        virtual bool BIsTwoFactorEnabled() { return false; }
        virtual bool BIsPhoneIdentifying() { return false; }
        virtual bool BIsPhoneRequiringVerification() { return false; }
        virtual SteamAPICall_t GetMarketEligibility() { return OfflineGetMarketEligibility(); }
        virtual SteamAPICall_t GetDurationControl() { return OfflineGetDurationControl(); }
        virtual bool BSetDurationControlOnlineState(int state) { NSR_UNUSED(state); return true; }
    };

    class OfflineSteamUser022 final
    {
    public:
        virtual HSteamUser GetHSteamUser() { return OfflineGetHSteamUser(); }
        virtual bool BLoggedOn() { return OfflineBLoggedOn(); }
        virtual CSteamID GetSteamID() { return OfflineGetSteamID(); }
        virtual int InitiateGameConnection(void* authBlob, int maxAuthBlob, CSteamID server, uint32_t ip, uint16_t port, bool secure)
        {
            return OfflineInitiateGameConnection(authBlob, maxAuthBlob, server, ip, port, secure);
        }
        virtual void TerminateGameConnection(uint32_t ip, uint16_t port) { OfflineTerminateGameConnection(ip, port); }
        virtual void TrackAppUsageEvent(uint64_t gameID, int event, const char* extra) { OfflineTrackAppUsageEvent(gameID, event, extra); }
        virtual bool GetUserDataFolder(char* buffer, int bufferSize) { return OfflineGetUserDataFolder(buffer, bufferSize); }
        virtual void StartVoiceRecording() { OfflineStartVoiceRecording(); }
        virtual void StopVoiceRecording() { OfflineStopVoiceRecording(); }
        virtual int GetAvailableVoice(uint32_t* compressed, uint32_t* uncompressed, uint32_t sampleRate)
        {
            return OfflineGetAvailableVoice(compressed, uncompressed, sampleRate);
        }
        virtual int GetVoice(bool wantCompressed, void* dest, uint32_t destSize, uint32_t* written, bool wantUncompressed, void* uncompressedDest, uint32_t uncompressedSize, uint32_t* uncompressedWritten, uint32_t sampleRate)
        {
            return OfflineGetVoice(wantCompressed, dest, destSize, written, wantUncompressed, uncompressedDest, uncompressedSize, uncompressedWritten, sampleRate);
        }
        virtual int DecompressVoice(const void* compressed, uint32_t compressedSize, void* dest, uint32_t destSize, uint32_t* written, uint32_t sampleRate)
        {
            return OfflineDecompressVoiceModern(compressed, compressedSize, dest, destSize, written, sampleRate);
        }
        virtual uint32_t GetVoiceOptimalSampleRate() { return OfflineGetVoiceOptimalSampleRate(); }
        virtual uint32_t GetAuthSessionTicket(void* ticket, int maxTicket, uint32_t* ticketSize, const void* identity)
        {
            NSR_UNUSED(identity);
            return OfflineGetAuthSessionTicket(ticket, maxTicket, ticketSize);
        }
        virtual int BeginAuthSession(const void* ticket, int ticketSize, CSteamID steamID) { return OfflineBeginAuthSession(ticket, ticketSize, steamID); }
        virtual void EndAuthSession(CSteamID steamID) { OfflineEndAuthSession(steamID); }
        virtual void CancelAuthTicket(uint32_t ticketHandle) { OfflineCancelAuthTicket(ticketHandle); }
        virtual int UserHasLicenseForApp(CSteamID steamID, AppId_t appID) { return OfflineUserHasLicenseForApp(steamID, appID); }
        virtual bool BIsBehindNAT() { return OfflineBIsBehindNAT(); }
        virtual void AdvertiseGame(CSteamID server, uint32_t ip, uint16_t port) { OfflineAdvertiseGame(server, ip, port); }
        virtual SteamAPICall_t RequestEncryptedAppTicket(void* data, int dataSize) { return OfflineRequestEncryptedAppTicket(data, dataSize); }
        virtual bool GetEncryptedAppTicket(void* ticket, int maxTicket, uint32_t* ticketSize) { return OfflineGetEncryptedAppTicket(ticket, maxTicket, ticketSize); }
        virtual int GetGameBadgeLevel(int series, bool foil) { return OfflineGetGameBadgeLevel(series, foil); }
        virtual int GetPlayerSteamLevel() { return OfflineGetPlayerSteamLevel(); }
        virtual SteamAPICall_t RequestStoreAuthURL(const char* redirect) { return OfflineRequestStoreAuthURL(redirect); }
        virtual bool BIsPhoneVerified() { return true; }
        virtual bool BIsTwoFactorEnabled() { return false; }
        virtual bool BIsPhoneIdentifying() { return false; }
        virtual bool BIsPhoneRequiringVerification() { return false; }
        virtual SteamAPICall_t GetMarketEligibility() { return OfflineGetMarketEligibility(); }
        virtual SteamAPICall_t GetDurationControl() { return OfflineGetDurationControl(); }
        virtual bool BSetDurationControlOnlineState(int state) { NSR_UNUSED(state); return true; }
    };

    class OfflineSteamApps final
    {
    public:
        virtual bool BIsSubscribed() { TraceCore("SteamApps", "BIsSubscribed"); return true; }
        virtual bool BIsLowViolence() { return false; }
        virtual bool BIsCybercafe() { return false; }
        virtual bool BIsVACBanned() { TraceCore("SteamApps", "BIsVACBanned"); return false; }
        virtual const char* GetCurrentGameLanguage() { TraceCore("SteamApps", "GetCurrentGameLanguage"); return "english"; }
        virtual const char* GetAvailableGameLanguages() { TraceCore("SteamApps", "GetAvailableGameLanguages"); return "english"; }
        virtual bool BIsSubscribedApp(AppId_t appID) { TraceCore("SteamApps", "BIsSubscribedApp", std::to_string(appID)); NSR_UNUSED(appID); return true; }
        virtual bool BIsDlcInstalled(AppId_t appID) { TraceCore("SteamApps", "BIsDlcInstalled", std::to_string(appID)); NSR_UNUSED(appID); return true; }
        virtual uint32_t GetEarliestPurchaseUnixTime(AppId_t appID) { NSR_UNUSED(appID); return 1262304000u; }
        virtual bool BIsSubscribedFromFreeWeekend() { return false; }
        virtual int GetDLCCount() { return 0; }
        virtual bool BGetDLCDataByIndex(int index, AppId_t* appID, bool* available, char* name, int nameSize)
        {
            NSR_UNUSED(index);
            if (appID) *appID = 0;
            if (available) *available = false;
            if (name && nameSize > 0) name[0] = '\0';
            return false;
        }
        virtual void InstallDLC(AppId_t appID) { NSR_UNUSED(appID); }
        virtual void UninstallDLC(AppId_t appID) { NSR_UNUSED(appID); }
        virtual void RequestAppProofOfPurchaseKey(AppId_t appID) { NSR_UNUSED(appID); }
        virtual bool GetCurrentBetaName(char* name, int nameSize)
        {
            CopyString(name, nameSize, "public");
            return name && nameSize > 0;
        }
        virtual bool MarkContentCorrupt(bool missingFilesOnly) { NSR_UNUSED(missingFilesOnly); return false; }
        virtual uint32_t GetInstalledDepots(AppId_t appID, uint32_t* depots, uint32_t maxDepots)
        {
            NSR_UNUSED(appID);
            if (depots && maxDepots > 0) depots[0] = kBreakersAppId;
            return maxDepots > 0 ? 1u : 0u;
        }
        virtual uint32_t GetAppInstallDir(AppId_t appID, char* folder, uint32_t folderSize)
        {
            TraceCore("SteamApps", "GetAppInstallDir", "app=" + std::to_string(appID));
            NSR_UNUSED(appID);
            CopyString(folder, static_cast<int>(folderSize), g_InstallPath);
            return static_cast<uint32_t>(g_InstallPath.size());
        }
        virtual bool BIsAppInstalled(AppId_t appID) { TraceCore("SteamApps", "BIsAppInstalled", std::to_string(appID)); NSR_UNUSED(appID); return true; }
        virtual CSteamID GetAppOwner() { TraceCore("SteamApps", "GetAppOwner"); return OfflineSteamIDObjectReturn(SteamIDManager::GetLocalSteamID()); }
        virtual const char* GetLaunchQueryParam(const char* key) { TraceCore("SteamApps", "GetLaunchQueryParam", key ? key : "null"); NSR_UNUSED(key); return ""; }
        virtual bool GetDlcDownloadProgress(AppId_t appID, uint64_t* downloaded, uint64_t* total)
        {
            NSR_UNUSED(appID);
            if (downloaded) *downloaded = 0;
            if (total) *total = 0;
            return false;
        }
        virtual int GetAppBuildId() { TraceCore("SteamApps", "GetAppBuildId"); return 1; }
        virtual void RequestAllProofOfPurchaseKeys() {}
        virtual SteamAPICall_t GetFileDetails(const char* file)
        {
            NSR_UNUSED(file);
            return SteamCallResultManager::CreateCallResult(1023);
        }
        virtual int GetLaunchCommandLine(char* commandLine, int size)
        {
            if (commandLine && size > 0) commandLine[0] = '\0';
            return 0;
        }
        virtual bool BIsSubscribedFromFamilySharing() { return false; }
        virtual bool BIsTimedTrial(uint32_t* allowed, uint32_t* played)
        {
            if (allowed) *allowed = 0;
            if (played) *played = 0;
            return false;
        }
        virtual bool SetDlcContext(AppId_t appID) { NSR_UNUSED(appID); return true; }
    };

    class OfflineSteamGameServer final
    {
    public:
        virtual bool InitGameServer(uint32_t ip, uint16_t gamePort, uint16_t queryPort, uint32_t flags, AppId_t appID, const char* version)
        {
            NSR_UNUSED(ip);
            NSR_UNUSED(gamePort);
            NSR_UNUSED(queryPort);
            NSR_UNUSED(flags);
            NSR_UNUSED(appID);
            NSR_UNUSED(version);
            m_LoggedOn = true;
            return true;
        }

        virtual void SetProduct(const char* product) { m_Product = product ? product : ""; }
        virtual void SetGameDescription(const char* description) { m_Description = description ? description : ""; }
        virtual void SetModDir(const char* modDir) { m_ModDir = modDir ? modDir : ""; }
        virtual void SetDedicatedServer(bool dedicated) { m_Dedicated = dedicated; }
        virtual void LogOn(const char* token) { NSR_UNUSED(token); m_LoggedOn = true; }
        virtual void LogOnAnonymous() { m_LoggedOn = true; }
        virtual void LogOff() { m_LoggedOn = false; }
        virtual bool BLoggedOn() { return m_LoggedOn; }
        virtual bool BSecure() { return false; }
        virtual CSteamID GetSteamID() { return OfflineSteamIDObjectReturn(SteamIDManager::GetLocalSteamID()); }
        virtual bool WasRestartRequested() { return false; }
        virtual void SetMaxPlayerCount(int playersMax) { m_MaxPlayers = playersMax; }
        virtual void SetBotPlayerCount(int botPlayers) { m_BotPlayers = botPlayers; }
        virtual void SetServerName(const char* serverName) { m_ServerName = serverName ? serverName : ""; }
        virtual void SetMapName(const char* mapName) { m_MapName = mapName ? mapName : ""; }
        virtual void SetPasswordProtected(bool passwordProtected) { m_PasswordProtected = passwordProtected; }
        virtual void SetSpectatorPort(uint16_t spectatorPort) { m_SpectatorPort = spectatorPort; }
        virtual void SetSpectatorServerName(const char* spectatorServerName) { m_SpectatorServerName = spectatorServerName ? spectatorServerName : ""; }
        virtual void ClearAllKeyValues() { m_KeyValues.clear(); }
        virtual void SetKeyValue(const char* key, const char* value)
        {
            if (!key || !key[0])
                return;

            m_KeyValues[key] = value ? value : "";
        }
        virtual void SetGameTags(const char* tags) { m_GameTags = tags ? tags : ""; }
        virtual void SetGameData(const char* data) { m_GameData = data ? data : ""; }
        virtual void SetRegion(const char* region) { m_Region = region ? region : ""; }
        virtual void SetAdvertiseServerActive(bool active)
        {
            m_AdvertiseActive = false;
            NSR_UNUSED(active);
        }
        virtual uint32_t GetAuthSessionTicket(void* ticket, int maxTicket, uint32_t* ticketSize, const void* identity)
        {
            NSR_UNUSED(identity);
            return SteamAuth::GetAuthSessionTicket(ticket, maxTicket, ticketSize);
        }
        virtual int BeginAuthSession(const void* ticket, int ticketSize, CSteamID steamID) { return SteamAuth::BeginAuthSession(ticket, ticketSize, steamID); }
        virtual void EndAuthSession(CSteamID steamID) { SteamAuth::EndAuthSession(steamID); }
        virtual void CancelAuthTicket(uint32_t ticketHandle) { SteamAuth::CancelAuthTicket(ticketHandle); }
        virtual int UserHasLicenseForApp(CSteamID steamID, AppId_t appID)
        {
            NSR_UNUSED(steamID);
            NSR_UNUSED(appID);
            return 0;
        }
        virtual bool RequestUserGroupStatus(CSteamID steamIDUser, CSteamID steamIDGroup)
        {
            struct Result
            {
                CSteamID m_SteamIDUser;
                CSteamID m_SteamIDGroup;
                bool m_bMember;
                bool m_bOfficer;
            };

            Result result{ steamIDUser, steamIDGroup, false, false };
            SteamCallbackManager::PushCallback(208, &result, sizeof(result));
            return true;
        }
        virtual void GetGameplayStats()
        {
            struct Result
            {
                int m_eResult;
                int32_t m_nRank;
                uint32_t m_unTotalConnects;
                uint32_t m_unTotalMinutesPlayed;
            };

            Result result{ 1, 0, 0, 0 };
            SteamCallbackManager::PushCallback(207, &result, sizeof(result));
        }
        virtual SteamAPICall_t GetServerReputation()
        {
            struct Result
            {
                int m_eResult;
                uint32_t m_unReputationScore;
                bool m_bBanned;
                uint32_t m_unBannedIP;
                uint16_t m_usBannedPort;
                uint64_t m_ulBannedGameID;
                uint32_t m_unBanExpires;
            };

            Result result{ 1, 100, false, 0, 0, 0, 0 };
            return SteamCallResultManager::CreateCallResult(209, &result, sizeof(result), true);
        }
        virtual OfflineSteamIPAddress GetPublicIP()
        {
            OfflineSteamIPAddress ip{};
            ip.m_unIPv4 = 0x7F000001u;
            ip.m_eType = 0;
            return ip;
        }
        virtual bool HandleIncomingPacket(const void* data, int dataSize, uint32_t sourceIP, uint16_t sourcePort)
        {
            NSR_UNUSED(data);
            NSR_UNUSED(dataSize);
            NSR_UNUSED(sourceIP);
            NSR_UNUSED(sourcePort);
            return false;
        }
        virtual int GetNextOutgoingPacket(void* out, int maxOut, uint32_t* netAddress, uint16_t* port)
        {
            NSR_UNUSED(out);
            NSR_UNUSED(maxOut);
            if (netAddress) *netAddress = 0;
            if (port) *port = 0;
            return 0;
        }
        virtual SteamAPICall_t AssociateWithClan(CSteamID clanID)
        {
            NSR_UNUSED(clanID);
            struct Result { int m_eResult; };
            Result result{ 1 };
            return SteamCallResultManager::CreateCallResult(210, &result, sizeof(result), true);
        }
        virtual SteamAPICall_t ComputeNewPlayerCompatibility(CSteamID newPlayer)
        {
            struct Result
            {
                int m_eResult;
                int m_cPlayersThatDontLikeCandidate;
                int m_cPlayersThatCandidateDoesntLike;
                int m_cClanPlayersThatDontLikeCandidate;
                CSteamID m_SteamIDCandidate;
            };

            Result result{ 1, 0, 0, 0, newPlayer };
            return SteamCallResultManager::CreateCallResult(211, &result, sizeof(result), true);
        }
        virtual bool SendUserConnectAndAuthenticate(uint32_t ipClient, const void* authBlob, uint32_t authBlobSize, CSteamID* steamIDUser)
        {
            NSR_UNUSED(ipClient);
            NSR_UNUSED(authBlob);
            NSR_UNUSED(authBlobSize);

            CSteamID id = SteamIDManager::GetLocalSteamID();
            if (steamIDUser)
                *steamIDUser = id;

            struct Result
            {
                CSteamID m_SteamID;
                CSteamID m_OwnerSteamID;
            };

            Result result{ id, id };
            SteamCallbackManager::PushCallback(201, &result, sizeof(result));
            return true;
        }
        virtual CSteamID CreateUnauthenticatedUserConnection()
        {
            return ++m_NextUnauthenticatedUser;
        }
        virtual void SendUserDisconnect(CSteamID steamIDUser) { NSR_UNUSED(steamIDUser); }
        virtual bool BUpdateUserData(CSteamID steamIDUser, const char* playerName, uint32_t score)
        {
            NSR_UNUSED(steamIDUser);
            NSR_UNUSED(playerName);
            NSR_UNUSED(score);
            return true;
        }
        virtual void SetMasterServerHeartbeatInterval_DEPRECATED(int interval) { NSR_UNUSED(interval); }
        virtual void ForceMasterServerHeartbeat_DEPRECATED() {}

    private:
        bool m_LoggedOn = true;
        bool m_Dedicated = false;
        bool m_PasswordProtected = false;
        bool m_AdvertiseActive = false;
        int m_MaxPlayers = 8;
        int m_BotPlayers = 0;
        uint16_t m_SpectatorPort = 0;
        CSteamID m_NextUnauthenticatedUser = 76561199006070000ull;
        std::string m_Product = "BreakersRevived";
        std::string m_Description = "DRAGON BALL: THE BREAKERS Revived";
        std::string m_ModDir;
        std::string m_ServerName = "Offline Session";
        std::string m_MapName;
        std::string m_SpectatorServerName;
        std::string m_GameTags;
        std::string m_GameData;
        std::string m_Region;
        std::map<std::string, std::string> m_KeyValues;
    };

    class OfflineSteamGameServerStats final
    {
    public:
        virtual SteamAPICall_t RequestUserStats(CSteamID steamIDUser)
        {
            struct Result
            {
                int m_eResult;
                CSteamID m_steamIDUser;
            };

            Result result{ 1, steamIDUser };
            return SteamCallResultManager::CreateCallResult(1800, &result, sizeof(result), true);
        }
        virtual bool GetUserStat(CSteamID steamIDUser, const char* name, int32_t* data)
        {
            NSR_UNUSED(steamIDUser);
            return SteamStatsLocal::GetInt(name, data);
        }
        virtual bool GetUserStat(CSteamID steamIDUser, const char* name, float* data)
        {
            NSR_UNUSED(steamIDUser);
            return SteamStatsLocal::GetFloat(name, data);
        }
        virtual bool GetUserAchievement(CSteamID steamIDUser, const char* name, bool* achieved)
        {
            NSR_UNUSED(steamIDUser);
            return SteamStatsLocal::GetAchievement(name, achieved);
        }
        virtual bool SetUserStat(CSteamID steamIDUser, const char* name, int32_t data)
        {
            NSR_UNUSED(steamIDUser);
            return SteamStatsLocal::SetInt(name, data);
        }
        virtual bool SetUserStat(CSteamID steamIDUser, const char* name, float data)
        {
            NSR_UNUSED(steamIDUser);
            return SteamStatsLocal::SetFloat(name, data);
        }
        virtual bool UpdateUserAvgRateStat(CSteamID steamIDUser, const char* name, float countThisSession, double sessionLength)
        {
            NSR_UNUSED(steamIDUser);
            if (sessionLength <= 0.0)
                return SteamStatsLocal::SetFloat(name, countThisSession);

            return SteamStatsLocal::SetFloat(name, static_cast<float>(countThisSession / sessionLength));
        }
        virtual bool SetUserAchievement(CSteamID steamIDUser, const char* name)
        {
            NSR_UNUSED(steamIDUser);
            return SteamStatsLocal::SetAchievement(name);
        }
        virtual bool ClearUserAchievement(CSteamID steamIDUser, const char* name)
        {
            NSR_UNUSED(steamIDUser);
            return SteamStatsLocal::ClearAchievement(name);
        }
        virtual SteamAPICall_t StoreUserStats(CSteamID steamIDUser)
        {
            SteamStatsLocal::Save();

            struct Result
            {
                int m_eResult;
                CSteamID m_steamIDUser;
            };

            Result result{ 1, steamIDUser };
            return SteamCallResultManager::CreateCallResult(1801, &result, sizeof(result), true);
        }
    };

    class OfflineSteamUtils final
    {
    public:
        virtual uint32_t GetSecondsSinceAppActive() { return static_cast<uint32_t>(GetTickCount64() / 1000); }
        virtual uint32_t GetSecondsSinceComputerActive() { return static_cast<uint32_t>(GetTickCount64() / 1000); }
        virtual int GetConnectedUniverse() { return 1; }
        virtual uint32_t GetServerRealTime() { return static_cast<uint32_t>(std::time(nullptr)); }
        virtual const char* GetIPCountry() { TraceCore("SteamUtils", "GetIPCountry"); return "US"; }
        virtual bool GetImageSize(int image, uint32_t* width, uint32_t* height)
        {
            TraceCore("SteamUtils", "GetImageSize", "image=" + std::to_string(image));
            const uint32_t edge = OfflineAvatarSize(image);
            if (width) *width = edge;
            if (height) *height = edge;
            return IsOfflineAvatarImage(image);
        }
        virtual bool GetImageRGBA(int image, uint8_t* dest, int destSize)
        {
            TraceCore("SteamUtils", "GetImageRGBA", "image=" + std::to_string(image) + " bytes=" + std::to_string(destSize));
            const uint32_t edge = OfflineAvatarSize(image);
            const uint32_t requiredSize = edge * edge * 4;

            if (!IsOfflineAvatarImage(image) ||
                !dest ||
                destSize < static_cast<int>(requiredSize) ||
                !IsWritableMemory(dest, requiredSize))
            {
                return false;
            }

            WriteOfflineAvatarRGBA(image, dest, edge);
            return true;
        }
        virtual bool GetCSERIPPort(uint32_t* ip, uint16_t* port)
        {
            if (ip) *ip = 0x0100007F;
            if (port) *port = 0;
            return true;
        }
        virtual uint8_t GetCurrentBatteryPower() { return 255; }
        virtual uint32_t GetAppID() { TraceCore("SteamUtils", "GetAppID"); return kBreakersAppId; }
        virtual void SetOverlayNotificationPosition(int pos) { NSR_UNUSED(pos); }
        virtual bool IsAPICallCompleted(SteamAPICall_t call, bool* failed)
        {
            TraceCore("SteamUtils", "IsAPICallCompleted", "call=" + std::to_string(static_cast<unsigned long long>(call)));
            if (failed) *failed = false;
            return SteamCallResultManager::IsCompleted(call);
        }
        virtual int GetAPICallFailureReason(SteamAPICall_t call)
        {
            NSR_UNUSED(call);
            return 0;
        }
        virtual bool GetAPICallResult(SteamAPICall_t call, void* callback, int callbackSize, int callbackExpected, bool* failed)
        {
            TraceCore("SteamUtils", "GetAPICallResult", "call=" + std::to_string(static_cast<unsigned long long>(call)));
            if (failed) *failed = false;
            int id = 0;
            bool ok = SteamCallResultManager::GetData(call, callback, callbackSize, &id);
            return ok && (callbackExpected == 0 || id == callbackExpected);
        }
        virtual void RunFrame() { SteamCallbackManager::RunCallbacks(); }
        virtual uint32_t GetIPCCallCount() { return 0; }
        virtual void SetWarningMessageHook(void* hook) { NSR_UNUSED(hook); }
        virtual bool IsOverlayEnabled() { TraceCore("SteamUtils", "IsOverlayEnabled"); return false; }
        virtual bool BOverlayNeedsPresent() { TraceCore("SteamUtils", "BOverlayNeedsPresent"); return false; }
        virtual SteamAPICall_t CheckFileSignature(const char* file)
        {
            NSR_UNUSED(file);
            return SteamCallResultManager::CreateCallResult(705);
        }
        virtual bool ShowGamepadTextInput(int mode, int lineMode, const char* description, uint32_t maxChars, const char* existing)
        {
            NSR_UNUSED(mode);
            NSR_UNUSED(lineMode);
            NSR_UNUSED(description);
            NSR_UNUSED(maxChars);
            NSR_UNUSED(existing);
            return false;
        }
        virtual uint32_t GetEnteredGamepadTextLength() { return 0; }
        virtual bool GetEnteredGamepadTextInput(char* text, uint32_t textSize)
        {
            if (text && textSize > 0) text[0] = '\0';
            return false;
        }
        virtual const char* GetSteamUILanguage() { return "english"; }
        virtual bool IsSteamRunningInVR() { return false; }
        virtual void SetOverlayNotificationInset(int x, int y) { NSR_UNUSED(x); NSR_UNUSED(y); }
        virtual bool IsSteamInBigPictureMode() { return false; }
        virtual void StartVRDashboard() {}
        virtual bool IsVRHeadsetStreamingEnabled() { return false; }
        virtual void SetVRHeadsetStreamingEnabled(bool enabled) { NSR_UNUSED(enabled); }
        virtual bool IsSteamChinaLauncher() { return false; }
        virtual bool InitFilterText(uint32_t options) { NSR_UNUSED(options); return true; }
        virtual int FilterText(int context, CSteamID source, const char* input, char* output, uint32_t outputSize)
        {
            NSR_UNUSED(context);
            NSR_UNUSED(source);
            CopyString(output, static_cast<int>(outputSize), input ? input : "");
            return 0;
        }
        virtual int GetIPv6ConnectivityState(int protocol) { NSR_UNUSED(protocol); return 0; }
        virtual bool IsSteamRunningOnSteamDeck() { return false; }
        virtual bool ShowFloatingGamepadTextInput(int mode, int x, int y, int w, int h)
        {
            NSR_UNUSED(mode); NSR_UNUSED(x); NSR_UNUSED(y); NSR_UNUSED(w); NSR_UNUSED(h);
            return false;
        }
        virtual void SetGameLauncherMode(bool launcherMode) { NSR_UNUSED(launcherMode); }
        virtual bool DismissFloatingGamepadTextInput() { return true; }
    };

    class OfflineSteamClient final
    {
    public:
        virtual HSteamPipe CreateSteamPipe() { return kLocalPipe; }
        virtual bool BReleaseSteamPipe(HSteamPipe pipe) { NSR_UNUSED(pipe); return true; }
        virtual HSteamUser ConnectToGlobalUser(HSteamPipe pipe) { NSR_UNUSED(pipe); return kLocalUser; }
        virtual HSteamUser CreateLocalUser(HSteamPipe* pipe, int accountType)
        {
            NSR_UNUSED(accountType);
            if (pipe) *pipe = kLocalPipe;
            return kLocalUser;
        }
        virtual void ReleaseUser(HSteamPipe pipe, HSteamUser user) { NSR_UNUSED(pipe); NSR_UNUSED(user); }
        virtual void* GetISteamUser(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamUser", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamCore::UserForVersion(version); }
        virtual void* GetISteamGameServer(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamGameServer", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamCore::GameServer(); }
        virtual void SetLocalIPBinding(const void* ip, uint16_t port) { NSR_UNUSED(ip); NSR_UNUSED(port); }
        virtual void* GetISteamFriends(HSteamUser user, HSteamPipe pipe, const char* version)
        {
            SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamFriends", version);
            NSR_UNUSED(user);
            NSR_UNUSED(pipe);
            return version && version[0] ? RouteVersion(version) : FakeSteamInterfaces::FriendsForVersion("SteamFriends015");
        }
        virtual void* GetISteamUtils(HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamUtils", version); NSR_UNUSED(pipe); return FakeSteamCore::Utils(); }
        virtual void* GetISteamMatchmaking(HSteamUser user, HSteamPipe pipe, const char* version)
        {
            SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamMatchmaking", version);
            NSR_UNUSED(user);
            NSR_UNUSED(pipe);
            return version && version[0] ? RouteVersion(version) : FakeSteamInterfaces::Matchmaking();
        }
        virtual void* GetISteamMatchmakingServers(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamMatchmakingServers", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::MatchmakingServers(); }
        virtual void* GetISteamGenericInterface(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamGenericInterface", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return RouteVersion(version); }
        virtual void* GetISteamUserStats(HSteamUser user, HSteamPipe pipe, const char* version)
        {
            SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamUserStats", version);
            NSR_UNUSED(user);
            NSR_UNUSED(pipe);
            return version && version[0] ? RouteVersion(version) : FakeSteamInterfaces::UserStats();
        }
        virtual void* GetISteamGameServerStats(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamGameServerStats", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamCore::GameServerStats(); }
        virtual void* GetISteamApps(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamApps", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamCore::Apps(); }
        virtual void* GetISteamNetworking(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamNetworking", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return BreakersRevived_SteamNetworking(); }
        virtual void* GetISteamRemoteStorage(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamRemoteStorage", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::RemoteStorageForVersion(version); }
        virtual void* GetISteamScreenshots(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamScreenshots", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::Screenshots(); }
        virtual void* GetISteamGameSearch(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamGameSearch", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::GameSearch(); }
        virtual void RunFrame() { SteamCallbackManager::RunCallbacks(); }
        virtual uint32_t GetIPCCallCount() { return 0; }
        virtual void SetWarningMessageHook(void* hook) { NSR_UNUSED(hook); }
        virtual bool BShutdownIfAllPipesClosed() { return false; }
        virtual void* GetISteamHTTP(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamHTTP", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::HTTP(); }
        virtual void* DEPRECATED_GetISteamUnifiedMessages(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamUnifiedMessages", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::UnifiedMessages(); }
        virtual void* GetISteamController(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamController", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::Controller(); }
        virtual void* GetISteamUGC(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamUGC", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::UGC(); }
        virtual void* GetISteamAppList(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamAppList", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::AppList(); }
        virtual void* GetISteamMusic(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamMusic", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::Music(); }
        virtual void* GetISteamMusicRemote(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamMusicRemote", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::MusicRemote(); }
        virtual void* GetISteamHTMLSurface(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamHTMLSurface", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::HTMLSurface(); }
        virtual void DEPRECATED_Set_SteamAPI_CPostAPIResultInProcess(void* fn) { NSR_UNUSED(fn); }
        virtual void DEPRECATED_Remove_SteamAPI_CPostAPIResultInProcess(void* fn) { NSR_UNUSED(fn); }
        virtual void Set_SteamAPI_CCheckCallbackRegisteredInProcess(void* fn) { NSR_UNUSED(fn); }
        virtual void* GetISteamInventory(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamInventory", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::Inventory(); }
        virtual void* GetISteamVideo(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamVideo", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::Video(); }
        virtual void* GetISteamParentalSettings(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamParentalSettings", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::ParentalSettings(); }
        virtual void* GetISteamInput(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamInput", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::Input(); }
        virtual void* GetISteamParties(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamParties", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::Parties(); }
        virtual void* GetISteamRemotePlay(HSteamUser user, HSteamPipe pipe, const char* version) { SteamVersionLogger::LogInterfaceRequest("OfflineSteamClient::GetISteamRemotePlay", version); NSR_UNUSED(user); NSR_UNUSED(pipe); return FakeSteamInterfaces::RemotePlay(); }
        virtual void DestroyAllInterfaces() {}
    };

    OfflineSteamClient g_Client;
    OfflineSteamUser g_User;
    OfflineSteamUser012 g_User012;
    OfflineSteamUser016 g_User016;
    OfflineSteamUser017 g_User017;
    OfflineSteamUser021 g_User021;
    OfflineSteamUser022 g_User022;
    OfflineSteamUtils g_Utils;
    OfflineSteamApps g_Apps;
    OfflineSteamGameServer g_GameServer;
    OfflineSteamGameServerStats g_GameServerStats;
}

namespace FakeSteamCore
{
    bool Init()
    {
        if (g_Initialized)
            return true;

        g_Initialized = true;
        g_InstallPath = ExeFolder();
        g_UserDataFolder = (std::filesystem::path(g_InstallPath) / "BreakersRevived" / "userdata").string();

        try
        {
            std::filesystem::create_directories(g_UserDataFolder);
        }
        catch (...)
        {
        }

        Logger::Info("FakeSteamCore initialized fully offline");
        return true;
    }

    void Shutdown()
    {
        g_Initialized = false;
        g_LastEncryptedTicket.clear();
    }

    HSteamUser UserHandle() { return kLocalUser; }
    HSteamPipe PipeHandle() { return kLocalPipe; }
    void* Client() { Init(); SteamVersionLogger::LogInterfaceRequest("FakeSteamCore::Client", "SteamClient"); return &g_Client; }
    void* User() { Init(); SteamVersionLogger::LogInterfaceRequest("FakeSteamCore::User", "SteamUser"); return &g_User; }
    void* UserForVersion(const char* version)
    {
        Init();
        const std::string v = version ? version : "";
        const std::string u = UpperInterfaceName(v);
        SteamVersionLogger::LogInterfaceRequest("FakeSteamCore::UserForVersion", v.empty() ? "SteamUser021" : v.c_str());

        if (HasInterface(u, "009") ||
            HasInterface(u, "010") ||
            HasInterface(u, "011") ||
            HasInterface(u, "012") ||
            HasInterface(u, "013") ||
            HasInterface(u, "014"))
        {
            return &g_User012;
        }

        if (HasInterface(u, "015") || HasInterface(u, "016"))
            return &g_User016;

        if (HasInterface(u, "017") || HasInterface(u, "018"))
            return &g_User017;

        if (v.empty() || HasInterface(u, "019") || HasInterface(u, "020") || HasInterface(u, "021"))
            return &g_User021;

        if (HasInterface(u, "022"))
            return &g_User022;

        if (HasInterface(u, "023"))
            return &g_User;

        Logger::Error("FakeSteamCore::UserForVersion unknown version: " + v + "; using SteamUser023 layout");
        return &g_User;
    }
    void* Utils() { Init(); SteamVersionLogger::LogInterfaceRequest("FakeSteamCore::Utils", "SteamUtils"); return &g_Utils; }
    void* Apps() { Init(); SteamVersionLogger::LogInterfaceRequest("FakeSteamCore::Apps", "SteamApps"); return &g_Apps; }
    void* GameServer() { Init(); SteamVersionLogger::LogInterfaceRequest("FakeSteamCore::GameServer", "SteamGameServer"); return &g_GameServer; }
    void* GameServerStats() { Init(); SteamVersionLogger::LogInterfaceRequest("FakeSteamCore::GameServerStats", "SteamGameServerStats"); return &g_GameServerStats; }
    const char* InstallPath() { Init(); return g_InstallPath.c_str(); }
}

void* FakeSteamInterfaces::Client() { return FakeSteamCore::Client(); }
void* FakeSteamInterfaces::User() { return FakeSteamCore::UserForVersion("SteamUser021"); }
void* FakeSteamInterfaces::Utils() { return FakeSteamCore::Utils(); }
void* FakeSteamInterfaces::Apps() { return FakeSteamCore::Apps(); }
void* FakeSteamInterfaces::GameServer() { return FakeSteamCore::GameServer(); }
void* FakeSteamInterfaces::GameServerStats() { return FakeSteamCore::GameServerStats(); }
