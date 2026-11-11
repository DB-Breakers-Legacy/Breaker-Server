#include "StdInc.h"
#include "FakeSteamInterfaces.h"
#include "Logger.h"
#include "SteamLobbyManager.h"
#include "SteamSessionManager.h"
#include "SteamCallResultManager.h"
#include "SteamCallbackManager.h"
#include "SteamIDManager.h"
#include "SteamDiagnostics.h"
#include "SteamVersionLogger.h"

static CSteamID g_LastLobbyListResult = 0;

namespace
{
    constexpr int kCallbackLobbyEnter = 504;
    constexpr int kCallbackLobbyDataUpdate = 505;
    constexpr int kCallbackLobbyChatUpdate = 506;
    constexpr int kCallbackLobbyGameCreated = 509;
    constexpr int kCallbackLobbyMatchList = 510;
    constexpr int kCallbackLobbyCreated = 513;

    struct LobbyMatchListResult
    {
        uint32_t m_nLobbiesMatching;
    };

    struct LobbyCreatedResult
    {
        int m_eResult;
        uint64_t m_ulSteamIDLobby;
    };

    struct LobbyEnterResult
    {
        uint64_t m_ulSteamIDLobby;
        uint32_t m_rgfChatPermissions;
        bool m_bLocked;
        uint32_t m_EChatRoomEnterResponse;
    };

    struct LobbyDataUpdateResult
    {
        uint64_t m_ulSteamIDLobby;
        uint64_t m_ulSteamIDMember;
        uint8_t m_bSuccess;
    };

    struct LobbyChatUpdateResult
    {
        uint64_t m_ulSteamIDLobby;
        uint64_t m_ulSteamIDUserChanged;
        uint64_t m_ulSteamIDMakingChange;
        uint32_t m_rgfChatMemberStateChange;
    };

    struct LobbyGameCreatedResult
    {
        uint64_t m_ulSteamIDLobby;
        uint64_t m_ulSteamIDGameServer;
        uint32_t m_unIP;
        uint16_t m_usPort;
    };

    int g_ResultCountLimit = 600;

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

    bool IsWritableMemory(void* ptr, size_t size)
    {
        if (!IsReadableMemory(ptr, size))
            return false;

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
            return false;

        const DWORD protect = mbi.Protect & 0xff;
        return protect == PAGE_READWRITE ||
            protect == PAGE_WRITECOPY ||
            protect == PAGE_EXECUTE_READWRITE ||
            protect == PAGE_EXECUTE_WRITECOPY;
    }

    template <typename T>
    void SafeWriteValue(T* ptr, const T& value)
    {
        if (IsWritableMemory(ptr, sizeof(T)))
            *ptr = value;
    }

    std::string SafeCString(const char* value, size_t maxLength = 255)
    {
        if (!value)
            return std::string();

        std::string out;
        out.reserve(std::min<size_t>(maxLength, 32));

        for (size_t i = 0; i < maxLength; ++i)
        {
            const char* current = value + i;
            if (!IsReadableMemory(current, 1))
                break;

            const char c = *current;
            if (c == '\0')
                break;

            out.push_back(c);
        }

        return out;
    }

    std::string DisplayCString(const char* value)
    {
        if (!value)
            return "null";

        std::string text = SafeCString(value);
        return text.empty() ? "<empty-or-unreadable>" : text;
    }

    void SafeCopyCString(char* dest, int destSize, const std::string& value)
    {
        if (!dest || destSize <= 0)
            return;

        if (!IsWritableMemory(dest, static_cast<size_t>(destSize)))
            return;

        strncpy_s(dest, static_cast<size_t>(destSize), value.c_str(), _TRUNCATE);
    }

    void TraceMatchmaking(const char* functionName, const std::string& detail = std::string())
    {
        SteamVersionLogger::LogCall("SteamMatchmaking", functionName, detail);
    }

    std::string LobbyDetail(CSteamID lobbyID)
    {
        return "lobby=" + std::to_string(static_cast<unsigned long long>(lobbyID));
    }

    CSteamID* WriteSteamIDReturn(CSteamID* outValue, CSteamID value)
    {
        static thread_local CSteamID fallbackValue = 0;
        fallbackValue = value;

        if (IsWritableMemory(outValue, sizeof(CSteamID)))
        {
            *outValue = value;
            return outValue;
        }

        return &fallbackValue;
    }
}

class FakeSteamMatchmaking final
{
public:
    virtual int GetFavoriteGameCount()
    {
        TraceMatchmaking("GetFavoriteGameCount");
        Logger::Info("SteamMatchmaking::GetFavoriteGameCount");
        return 0;
    }

    virtual bool GetFavoriteGame(
        int game,
        AppId_t* appID,
        uint32_t* ip,
        uint16_t* connPort,
        uint16_t* queryPort,
        uint32_t* flags,
        uint32_t* lastPlayed)
    {
        TraceMatchmaking("GetFavoriteGame", "index=" + std::to_string(game));
        NSR_UNUSED(game);

        Logger::Info("SteamMatchmaking::GetFavoriteGame");

        SafeWriteValue(appID, static_cast<AppId_t>(1276760));
        SafeWriteValue(ip, 0u);
        SafeWriteValue(connPort, static_cast<uint16_t>(0));
        SafeWriteValue(queryPort, static_cast<uint16_t>(0));
        SafeWriteValue(flags, 0u);
        SafeWriteValue(lastPlayed, 0u);

        return false;
    }

    virtual int AddFavoriteGame(
        AppId_t appID,
        uint32_t ip,
        uint16_t connPort,
        uint16_t queryPort,
        uint32_t flags,
        uint32_t lastPlayed)
    {
        TraceMatchmaking("AddFavoriteGame", "app=" + std::to_string(appID));
        NSR_UNUSED(appID);
        NSR_UNUSED(ip);
        NSR_UNUSED(connPort);
        NSR_UNUSED(queryPort);
        NSR_UNUSED(flags);
        NSR_UNUSED(lastPlayed);

        Logger::Info("SteamMatchmaking::AddFavoriteGame");
        return 0;
    }

    virtual bool RemoveFavoriteGame(
        AppId_t appID,
        uint32_t ip,
        uint16_t connPort,
        uint16_t queryPort,
        uint32_t flags)
    {
        TraceMatchmaking("RemoveFavoriteGame", "app=" + std::to_string(appID));
        NSR_UNUSED(appID);
        NSR_UNUSED(ip);
        NSR_UNUSED(connPort);
        NSR_UNUSED(queryPort);
        NSR_UNUSED(flags);

        Logger::Info("SteamMatchmaking::RemoveFavoriteGame");
        return true;
    }

    virtual SteamAPICall_t RequestLobbyList()
    {
        TraceMatchmaking("RequestLobbyList");
        Logger::Info("SteamMatchmaking::RequestLobbyList offline LAN/Radmin discovery");
        SteamDiagnostics::MarkOnline("RequestLobbyList", "offline LAN/Radmin discovery");

        LobbyMatchListResult result{};
        const int found = SteamLobbyManager::RefreshNetworkLobbies(100);
        const int limited = std::max(0, std::min(found, g_ResultCountLimit));
        result.m_nLobbiesMatching = static_cast<uint32_t>(limited);
        g_LastLobbyListResult = result.m_nLobbiesMatching > 0 ? SteamLobbyManager::GetLobbyByIndex(0) : 0;

        Logger::Info("SteamMatchmaking::RequestLobbyList result count=" + std::to_string(result.m_nLobbiesMatching));
        Logger::Flush();

        SteamCallbackManager::PushCallback(kCallbackLobbyMatchList, &result, sizeof(result));
        return SteamCallResultManager::CreateCallResult(
            kCallbackLobbyMatchList,
            &result,
            sizeof(result),
            true);
    }

    virtual void AddRequestLobbyListStringFilter(
        const char* key,
        const char* value,
        int comparison)
    {
        const std::string safeKey = SafeCString(key);
        const std::string safeValue = SafeCString(value);
        TraceMatchmaking(
            "AddRequestLobbyListStringFilter",
            DisplayCString(key) + "=" + DisplayCString(value));
        NSR_UNUSED(comparison);

        Logger::Info(
            std::string("SteamMatchmaking::AddRequestLobbyListStringFilter ") +
            (safeKey.empty() ? "<empty>" : safeKey) +
            "=" +
            (safeValue.empty() ? "<empty>" : safeValue));
    }

    virtual void AddRequestLobbyListNumericalFilter(
        const char* key,
        int value,
        int comparison)
    {
        const std::string safeKey = SafeCString(key);
        TraceMatchmaking(
            "AddRequestLobbyListNumericalFilter",
            (safeKey.empty() ? "<empty>" : safeKey) + "=" + std::to_string(value));
        NSR_UNUSED(key);
        NSR_UNUSED(value);
        NSR_UNUSED(comparison);

        Logger::Info("SteamMatchmaking::AddRequestLobbyListNumericalFilter");
    }

    virtual void AddRequestLobbyListNearValueFilter(
        const char* key,
        int value)
    {
        const std::string safeKey = SafeCString(key);
        TraceMatchmaking(
            "AddRequestLobbyListNearValueFilter",
            (safeKey.empty() ? "<empty>" : safeKey) + "=" + std::to_string(value));
        NSR_UNUSED(key);
        NSR_UNUSED(value);

        Logger::Info("SteamMatchmaking::AddRequestLobbyListNearValueFilter");
    }

    virtual void AddRequestLobbyListFilterSlotsAvailable(int slotsAvailable)
    {
        TraceMatchmaking("AddRequestLobbyListFilterSlotsAvailable", std::to_string(slotsAvailable));
        NSR_UNUSED(slotsAvailable);
        Logger::Info("SteamMatchmaking::AddRequestLobbyListFilterSlotsAvailable");
    }

    virtual void AddRequestLobbyListDistanceFilter(int distanceFilter)
    {
        TraceMatchmaking("AddRequestLobbyListDistanceFilter", std::to_string(distanceFilter));
        NSR_UNUSED(distanceFilter);
        Logger::Info("SteamMatchmaking::AddRequestLobbyListDistanceFilter");
    }

    virtual void AddRequestLobbyListResultCountFilter(int maxResults)
    {
        TraceMatchmaking("AddRequestLobbyListResultCountFilter", std::to_string(maxResults));
        if (maxResults > 0)
            g_ResultCountLimit = std::min(maxResults, 600);
        Logger::Info("SteamMatchmaking::AddRequestLobbyListResultCountFilter");
    }

    virtual void AddRequestLobbyListCompatibleMembersFilter(CSteamID lobbyID)
    {
        TraceMatchmaking("AddRequestLobbyListCompatibleMembersFilter", LobbyDetail(lobbyID));
        NSR_UNUSED(lobbyID);
        Logger::Info("SteamMatchmaking::AddRequestLobbyListCompatibleMembersFilter");
    }

    CSteamID GetLobbyByIndexValue(int index)
    {
        TraceMatchmaking("GetLobbyByIndex", "index=" + std::to_string(index));
        Logger::Info("SteamMatchmaking::GetLobbyByIndex " + std::to_string(index));
        if (index < 0 || index >= g_ResultCountLimit)
            return 0;

        return SteamLobbyManager::GetLobbyByIndex(index);
    }

    virtual CSteamID* GetLobbyByIndex(CSteamID* outLobbyID, int index)
    {
        return WriteSteamIDReturn(outLobbyID, GetLobbyByIndexValue(index));
    }

    virtual SteamAPICall_t CreateLobby(int lobbyType, int maxMembers)
    {
        TraceMatchmaking(
            "CreateLobby",
            "type=" + std::to_string(lobbyType) + " max=" + std::to_string(maxMembers));
        Logger::Info(
            "SteamMatchmaking::CreateLobby type=" +
            std::to_string(lobbyType) +
            " max=" +
            std::to_string(maxMembers));
        SteamDiagnostics::MarkOnline(
            "MatchmakingCreateLobby",
            "type=" +
            std::to_string(lobbyType) +
            " max=" +
            std::to_string(maxMembers));

        CSteamID lobbyID = SteamLobbyManager::CreateLobby(lobbyType, maxMembers);
        SteamSessionManager::JoinLobby(lobbyID);

        g_LastLobbyListResult = lobbyID;

        LobbyCreatedResult result{};
        result.m_eResult = 1;
        result.m_ulSteamIDLobby = lobbyID;

        LobbyEnterResult enterResult{};
        enterResult.m_ulSteamIDLobby = lobbyID;
        enterResult.m_rgfChatPermissions = 0;
        enterResult.m_bLocked = false;
        enterResult.m_EChatRoomEnterResponse = 1;

        Logger::Info("SteamMatchmaking created lobby id=" + std::to_string(lobbyID));
        SteamDiagnostics::MarkOnline(
            "MatchmakingLobbyCreated",
            "id=" +
            std::to_string(static_cast<unsigned long long>(lobbyID)));

        const SteamAPICall_t call = SteamCallResultManager::CreateCallResult(
            kCallbackLobbyCreated,
            &result,
            sizeof(result),
            true);
        SteamCallbackManager::PushCallback(kCallbackLobbyCreated, &result, sizeof(result));
        SteamCallbackManager::PushCallback(kCallbackLobbyEnter, &enterResult, sizeof(enterResult));
        Logger::Flush();
        return call;
    }

    virtual SteamAPICall_t JoinLobby(CSteamID lobbyID)
    {
        TraceMatchmaking("JoinLobby", LobbyDetail(lobbyID));
        Logger::Info("SteamMatchmaking::JoinLobby " + std::to_string(lobbyID));
        SteamDiagnostics::MarkOnline(
            "MatchmakingJoinLobby",
            "id=" +
            std::to_string(static_cast<unsigned long long>(lobbyID)));

        bool joined = SteamLobbyManager::JoinLobby(lobbyID);
        if (joined)
            SteamSessionManager::JoinLobby(lobbyID);

        LobbyEnterResult result{};
        result.m_ulSteamIDLobby = lobbyID;
        result.m_rgfChatPermissions = 0;
        result.m_bLocked = false;
        result.m_EChatRoomEnterResponse = joined ? 1 : 4;

        const SteamAPICall_t call = SteamCallResultManager::CreateCallResult(
            kCallbackLobbyEnter,
            &result,
            sizeof(result),
            true);

        if (joined)
        {
            LobbyChatUpdateResult chatUpdate{};
            chatUpdate.m_ulSteamIDLobby = lobbyID;
            chatUpdate.m_ulSteamIDUserChanged = SteamIDManager::GetSteamID64();
            chatUpdate.m_ulSteamIDMakingChange = SteamIDManager::GetSteamID64();
            chatUpdate.m_rgfChatMemberStateChange = 0x0001;
            SteamCallbackManager::PushCallback(kCallbackLobbyChatUpdate, &chatUpdate, sizeof(chatUpdate));
        }

        SteamCallbackManager::PushCallback(kCallbackLobbyEnter, &result, sizeof(result));
        Logger::Flush();
        return call;
    }

    virtual void LeaveLobby(CSteamID lobbyID)
    {
        TraceMatchmaking("LeaveLobby", LobbyDetail(lobbyID));
        Logger::Info("SteamMatchmaking::LeaveLobby " + std::to_string(lobbyID));
        SteamDiagnostics::MarkOnline(
            "MatchmakingLeaveLobby",
            "id=" +
            std::to_string(static_cast<unsigned long long>(lobbyID)));

        SteamLobbyManager::LeaveLobby(lobbyID);
        SteamSessionManager::LeaveLobby(lobbyID);
    }

    virtual bool InviteUserToLobby(CSteamID lobbyID, CSteamID invitee)
    {
        TraceMatchmaking("InviteUserToLobby", LobbyDetail(lobbyID));
        NSR_UNUSED(lobbyID);
        NSR_UNUSED(invitee);

        Logger::Info("SteamMatchmaking::InviteUserToLobby");
        return false;
    }

    virtual int GetNumLobbyMembers(CSteamID lobbyID)
    {
        TraceMatchmaking("GetNumLobbyMembers", LobbyDetail(lobbyID));
        Logger::Info("SteamMatchmaking::GetNumLobbyMembers");
        return SteamLobbyManager::GetMemberCount(lobbyID);
    }

    CSteamID GetLobbyMemberByIndexValue(CSteamID lobbyID, int member)
    {
        TraceMatchmaking("GetLobbyMemberByIndex", LobbyDetail(lobbyID) + " index=" + std::to_string(member));
        Logger::Info("SteamMatchmaking::GetLobbyMemberByIndex");
        return SteamLobbyManager::GetMemberByIndex(lobbyID, member);
    }

    virtual CSteamID* GetLobbyMemberByIndex(CSteamID* outUserID, CSteamID lobbyID, int member)
    {
        return WriteSteamIDReturn(outUserID, GetLobbyMemberByIndexValue(lobbyID, member));
    }

    virtual const char* GetLobbyData(CSteamID lobbyID, const char* key)
    {
        const std::string safeKey = SafeCString(key);
        TraceMatchmaking("GetLobbyData", LobbyDetail(lobbyID) + " key=" + DisplayCString(key));
        Logger::Info("SteamMatchmaking::GetLobbyData " + (safeKey.empty() ? "<empty>" : safeKey));
        return SteamLobbyManager::GetData(lobbyID, safeKey);
    }

    virtual bool SetLobbyData(CSteamID lobbyID, const char* key, const char* value)
    {
        const std::string safeKey = SafeCString(key);
        const std::string safeValue = SafeCString(value, 4096);
        TraceMatchmaking("SetLobbyData", LobbyDetail(lobbyID) + " key=" + DisplayCString(key));
        Logger::Info("SteamMatchmaking::SetLobbyData " + (safeKey.empty() ? "<empty>" : safeKey));
        return SteamLobbyManager::SetData(lobbyID, safeKey, safeValue);
    }

    virtual int GetLobbyDataCount(CSteamID lobbyID)
    {
        TraceMatchmaking("GetLobbyDataCount", LobbyDetail(lobbyID));
        Logger::Info("SteamMatchmaking::GetLobbyDataCount");
        return SteamLobbyManager::GetDataCount(lobbyID);
    }

    virtual bool GetLobbyDataByIndex(
        CSteamID lobbyID,
        int index,
        char* key,
        int keySize,
        char* value,
        int valueSize)
    {
        TraceMatchmaking("GetLobbyDataByIndex", LobbyDetail(lobbyID) + " index=" + std::to_string(index));
        Logger::Info("SteamMatchmaking::GetLobbyDataByIndex");

        std::string keyText;
        std::string valueText;

        if (!SteamLobbyManager::GetDataByIndex(lobbyID, index, keyText, valueText))
        {
            SafeCopyCString(key, keySize, std::string());
            SafeCopyCString(value, valueSize, std::string());

            return false;
        }

        SafeCopyCString(key, keySize, keyText);
        SafeCopyCString(value, valueSize, valueText);

        return true;
    }

    virtual bool DeleteLobbyData(CSteamID lobbyID, const char* key)
    {
        const std::string safeKey = SafeCString(key);
        TraceMatchmaking("DeleteLobbyData", LobbyDetail(lobbyID) + " key=" + DisplayCString(key));
        Logger::Info("SteamMatchmaking::DeleteLobbyData");
        return SteamLobbyManager::DeleteData(lobbyID, safeKey);
    }

    virtual const char* GetLobbyMemberData(
        CSteamID lobbyID,
        CSteamID user,
        const char* key)
    {
        const std::string safeKey = SafeCString(key);
        TraceMatchmaking(
            "GetLobbyMemberData",
            LobbyDetail(lobbyID) +
            " user=" +
            std::to_string(static_cast<unsigned long long>(user)) +
            " key=" +
            DisplayCString(key));
        return SteamLobbyManager::GetMemberData(lobbyID, user, safeKey);
    }

    virtual void SetLobbyMemberData(
        CSteamID lobbyID,
        const char* key,
        const char* value)
    {
        const std::string safeKey = SafeCString(key);
        const std::string safeValue = SafeCString(value, 4096);
        TraceMatchmaking("SetLobbyMemberData", LobbyDetail(lobbyID) + " key=" + DisplayCString(key));
        Logger::Info("SteamMatchmaking::SetLobbyMemberData");
        SteamLobbyManager::SetMemberData(
            lobbyID,
            SteamIDManager::GetLocalSteamID(),
            safeKey,
            safeValue);
    }

    virtual bool SendLobbyChatMsg(
        CSteamID lobbyID,
        const void* msgBody,
        int msgBodySize)
    {
        TraceMatchmaking("SendLobbyChatMsg", LobbyDetail(lobbyID) + " bytes=" + std::to_string(msgBodySize));
        Logger::Info("SteamMatchmaking::SendLobbyChatMsg");
        return SteamLobbyManager::AddChatEntry(
            lobbyID,
            SteamIDManager::GetLocalSteamID(),
            msgBody,
            msgBodySize,
            1) >= 0;
    }

    virtual int GetLobbyChatEntry(
        CSteamID lobbyID,
        int chatID,
        CSteamID* user,
        void* data,
        int dataSize,
        int* chatEntryType)
    {
        TraceMatchmaking("GetLobbyChatEntry", LobbyDetail(lobbyID) + " chat=" + std::to_string(chatID));
        Logger::Info("SteamMatchmaking::GetLobbyChatEntry");
        return SteamLobbyManager::GetChatEntry(lobbyID, chatID, user, data, dataSize, chatEntryType);
    }

    virtual bool RequestLobbyData(CSteamID lobbyID)
    {
        TraceMatchmaking("RequestLobbyData", LobbyDetail(lobbyID));
        Logger::Info("SteamMatchmaking::RequestLobbyData " + std::to_string(lobbyID));
        SteamDiagnostics::MarkOnline(
            "RequestLobbyData",
            "id=" +
            std::to_string(static_cast<unsigned long long>(lobbyID)));
        SteamLobbyManager::RefreshNetworkLobbies(20);

        LobbyDataUpdateResult result{};
        result.m_ulSteamIDLobby = lobbyID;
        result.m_ulSteamIDMember = lobbyID;
        result.m_bSuccess = 1;

        SteamCallbackManager::PushCallback(kCallbackLobbyDataUpdate, &result, sizeof(result));
        Logger::Flush();
        return true;
    }

    virtual void SetLobbyGameServer(
        CSteamID lobbyID,
        uint32_t gameServerIP,
        uint16_t gameServerPort,
        CSteamID steamIDGameServer)
    {
        TraceMatchmaking(
            "SetLobbyGameServer",
            LobbyDetail(lobbyID) + " port=" + std::to_string(gameServerPort));
        Logger::Info("SteamMatchmaking::SetLobbyGameServer");
        SteamLobbyManager::SetGameServer(lobbyID, gameServerIP, gameServerPort, steamIDGameServer);

        LobbyGameCreatedResult result{};
        result.m_ulSteamIDLobby = lobbyID;
        result.m_ulSteamIDGameServer = steamIDGameServer ? steamIDGameServer : lobbyID;
        result.m_unIP = gameServerIP;
        result.m_usPort = gameServerPort;
        SteamCallbackManager::PushCallback(kCallbackLobbyGameCreated, &result, sizeof(result));
        Logger::Flush();
    }

    virtual bool GetLobbyGameServer(
        CSteamID lobbyID,
        uint32_t* gameServerIP,
        uint16_t* gameServerPort,
        CSteamID* steamIDGameServer)
    {
        TraceMatchmaking("GetLobbyGameServer", LobbyDetail(lobbyID));
        Logger::Info("SteamMatchmaking::GetLobbyGameServer");
        uint32_t ip = 0;
        uint16_t port = 0;
        CSteamID server = 0;
        const bool ok = SteamLobbyManager::GetGameServer(lobbyID, &ip, &port, &server);
        SafeWriteValue(gameServerIP, ip);
        SafeWriteValue(gameServerPort, port);
        SafeWriteValue(steamIDGameServer, server);
        return ok;
    }

    virtual bool SetLobbyMemberLimit(CSteamID lobbyID, int maxMembers)
    {
        TraceMatchmaking("SetLobbyMemberLimit", LobbyDetail(lobbyID) + " max=" + std::to_string(maxMembers));
        Logger::Info("SteamMatchmaking::SetLobbyMemberLimit");
        return SteamLobbyManager::SetMemberLimit(lobbyID, maxMembers);
    }

    virtual int GetLobbyMemberLimit(CSteamID lobbyID)
    {
        TraceMatchmaking("GetLobbyMemberLimit", LobbyDetail(lobbyID));
        Logger::Info("SteamMatchmaking::GetLobbyMemberLimit");
        return SteamLobbyManager::GetMemberLimit(lobbyID);
    }

    virtual bool SetLobbyType(CSteamID lobbyID, int lobbyType)
    {
        TraceMatchmaking("SetLobbyType", LobbyDetail(lobbyID) + " type=" + std::to_string(lobbyType));
        Logger::Info("SteamMatchmaking::SetLobbyType");
        return SteamLobbyManager::SetType(lobbyID, lobbyType);
    }

    virtual bool SetLobbyJoinable(CSteamID lobbyID, bool joinable)
    {
        TraceMatchmaking("SetLobbyJoinable", LobbyDetail(lobbyID) + " joinable=" + (joinable ? "1" : "0"));
        Logger::Info("SteamMatchmaking::SetLobbyJoinable");
        return SteamLobbyManager::SetJoinable(lobbyID, joinable);
    }

    CSteamID GetLobbyOwnerValue(CSteamID lobbyID)
    {
        TraceMatchmaking("GetLobbyOwner", LobbyDetail(lobbyID));
        Logger::Info("SteamMatchmaking::GetLobbyOwner");
        CSteamID owner = SteamLobbyManager::GetOwner(lobbyID);
        return owner ? owner : lobbyID;
    }

    virtual CSteamID* GetLobbyOwner(CSteamID* outOwner, CSteamID lobbyID)
    {
        return WriteSteamIDReturn(outOwner, GetLobbyOwnerValue(lobbyID));
    }

    virtual bool SetLobbyOwner(CSteamID lobbyID, CSteamID newOwner)
    {
        TraceMatchmaking("SetLobbyOwner", LobbyDetail(lobbyID));
        Logger::Info("SteamMatchmaking::SetLobbyOwner");
        return SteamLobbyManager::SetOwner(lobbyID, newOwner);
    }

    virtual bool SetLinkedLobby(CSteamID lobbyID, CSteamID linkedLobbyID)
    {
        TraceMatchmaking("SetLinkedLobby", LobbyDetail(lobbyID));
        NSR_UNUSED(lobbyID);
        NSR_UNUSED(linkedLobbyID);

        Logger::Info("SteamMatchmaking::SetLinkedLobby");
        return true;
    }
};

static FakeSteamMatchmaking g_Interface;

class FakeSteamMatchmaking008 final
{
public:
    virtual int GetFavoriteGameCount() { return g_Interface.GetFavoriteGameCount(); }
    virtual bool GetFavoriteGame(int game, AppId_t* appID, uint32_t* ip, uint16_t* connPort, uint16_t* queryPort, uint32_t* flags, uint32_t* lastPlayed)
    {
        return g_Interface.GetFavoriteGame(game, appID, ip, connPort, queryPort, flags, lastPlayed);
    }
    virtual int AddFavoriteGame(AppId_t appID, uint32_t ip, uint16_t connPort, uint16_t queryPort, uint32_t flags, uint32_t lastPlayed)
    {
        return g_Interface.AddFavoriteGame(appID, ip, connPort, queryPort, flags, lastPlayed);
    }
    virtual bool RemoveFavoriteGame(AppId_t appID, uint32_t ip, uint16_t connPort, uint16_t queryPort, uint32_t flags)
    {
        return g_Interface.RemoveFavoriteGame(appID, ip, connPort, queryPort, flags);
    }
    virtual SteamAPICall_t RequestLobbyList() { return g_Interface.RequestLobbyList(); }
    virtual void AddRequestLobbyListStringFilter(const char* key, const char* value, int comparison)
    {
        g_Interface.AddRequestLobbyListStringFilter(key, value, comparison);
    }
    virtual void AddRequestLobbyListNumericalFilter(const char* key, int value, int comparison)
    {
        g_Interface.AddRequestLobbyListNumericalFilter(key, value, comparison);
    }
    virtual void AddRequestLobbyListNearValueFilter(const char* key, int value)
    {
        g_Interface.AddRequestLobbyListNearValueFilter(key, value);
    }
    virtual void AddRequestLobbyListFilterSlotsAvailable(int slotsAvailable)
    {
        g_Interface.AddRequestLobbyListFilterSlotsAvailable(slotsAvailable);
    }
    virtual void AddRequestLobbyListDistanceFilter(int distanceFilter)
    {
        g_Interface.AddRequestLobbyListDistanceFilter(distanceFilter);
    }
    virtual void AddRequestLobbyListResultCountFilter(int maxResults)
    {
        g_Interface.AddRequestLobbyListResultCountFilter(maxResults);
    }
    virtual CSteamID* GetLobbyByIndex(CSteamID* outLobbyID, int index) { return WriteSteamIDReturn(outLobbyID, g_Interface.GetLobbyByIndexValue(index)); }
    virtual SteamAPICall_t CreateLobby(int lobbyType, int maxMembers) { return g_Interface.CreateLobby(lobbyType, maxMembers); }
    virtual SteamAPICall_t JoinLobby(CSteamID lobbyID) { return g_Interface.JoinLobby(lobbyID); }
    virtual void LeaveLobby(CSteamID lobbyID) { g_Interface.LeaveLobby(lobbyID); }
    virtual bool InviteUserToLobby(CSteamID lobbyID, CSteamID invitee) { return g_Interface.InviteUserToLobby(lobbyID, invitee); }
    virtual int GetNumLobbyMembers(CSteamID lobbyID) { return g_Interface.GetNumLobbyMembers(lobbyID); }
    virtual CSteamID* GetLobbyMemberByIndex(CSteamID* outUserID, CSteamID lobbyID, int member) { return WriteSteamIDReturn(outUserID, g_Interface.GetLobbyMemberByIndexValue(lobbyID, member)); }
    virtual const char* GetLobbyData(CSteamID lobbyID, const char* key) { return g_Interface.GetLobbyData(lobbyID, key); }
    virtual bool SetLobbyData(CSteamID lobbyID, const char* key, const char* value) { return g_Interface.SetLobbyData(lobbyID, key, value); }
    virtual int GetLobbyDataCount(CSteamID lobbyID) { return g_Interface.GetLobbyDataCount(lobbyID); }
    virtual bool GetLobbyDataByIndex(CSteamID lobbyID, int index, char* key, int keySize, char* value, int valueSize)
    {
        return g_Interface.GetLobbyDataByIndex(lobbyID, index, key, keySize, value, valueSize);
    }
    virtual bool DeleteLobbyData(CSteamID lobbyID, const char* key) { return g_Interface.DeleteLobbyData(lobbyID, key); }
    virtual const char* GetLobbyMemberData(CSteamID lobbyID, CSteamID user, const char* key)
    {
        return g_Interface.GetLobbyMemberData(lobbyID, user, key);
    }
    virtual void SetLobbyMemberData(CSteamID lobbyID, const char* key, const char* value)
    {
        g_Interface.SetLobbyMemberData(lobbyID, key, value);
    }
    virtual bool SendLobbyChatMsg(CSteamID lobbyID, const void* msgBody, int msgBodySize)
    {
        return g_Interface.SendLobbyChatMsg(lobbyID, msgBody, msgBodySize);
    }
    virtual int GetLobbyChatEntry(CSteamID lobbyID, int chatID, CSteamID* user, void* data, int dataSize, int* chatEntryType)
    {
        return g_Interface.GetLobbyChatEntry(lobbyID, chatID, user, data, dataSize, chatEntryType);
    }
    virtual bool RequestLobbyData(CSteamID lobbyID) { return g_Interface.RequestLobbyData(lobbyID); }
    virtual void SetLobbyGameServer(CSteamID lobbyID, uint32_t gameServerIP, uint16_t gameServerPort, CSteamID steamIDGameServer)
    {
        g_Interface.SetLobbyGameServer(lobbyID, gameServerIP, gameServerPort, steamIDGameServer);
    }
    virtual bool GetLobbyGameServer(CSteamID lobbyID, uint32_t* gameServerIP, uint16_t* gameServerPort, CSteamID* steamIDGameServer)
    {
        return g_Interface.GetLobbyGameServer(lobbyID, gameServerIP, gameServerPort, steamIDGameServer);
    }
    virtual bool SetLobbyMemberLimit(CSteamID lobbyID, int maxMembers) { return g_Interface.SetLobbyMemberLimit(lobbyID, maxMembers); }
    virtual int GetLobbyMemberLimit(CSteamID lobbyID) { return g_Interface.GetLobbyMemberLimit(lobbyID); }
    virtual bool SetLobbyType(CSteamID lobbyID, int lobbyType) { return g_Interface.SetLobbyType(lobbyID, lobbyType); }
    virtual bool SetLobbyJoinable(CSteamID lobbyID, bool joinable) { return g_Interface.SetLobbyJoinable(lobbyID, joinable); }
    virtual CSteamID* GetLobbyOwner(CSteamID* outOwner, CSteamID lobbyID) { return WriteSteamIDReturn(outOwner, g_Interface.GetLobbyOwnerValue(lobbyID)); }
    virtual bool SetLobbyOwner(CSteamID lobbyID, CSteamID newOwner) { return g_Interface.SetLobbyOwner(lobbyID, newOwner); }
};

class FakeSteamMatchmaking007 final
{
public:
    virtual int GetFavoriteGameCount() { return g_Interface.GetFavoriteGameCount(); }
    virtual bool GetFavoriteGame(int game, AppId_t* appID, uint32_t* ip, uint16_t* connPort, uint16_t* queryPort, uint32_t* flags, uint32_t* lastPlayed)
    {
        return g_Interface.GetFavoriteGame(game, appID, ip, connPort, queryPort, flags, lastPlayed);
    }
    virtual int AddFavoriteGame(AppId_t appID, uint32_t ip, uint16_t connPort, uint16_t queryPort, uint32_t flags, uint32_t lastPlayed)
    {
        return g_Interface.AddFavoriteGame(appID, ip, connPort, queryPort, flags, lastPlayed);
    }
    virtual bool RemoveFavoriteGame(AppId_t appID, uint32_t ip, uint16_t connPort, uint16_t queryPort, uint32_t flags)
    {
        return g_Interface.RemoveFavoriteGame(appID, ip, connPort, queryPort, flags);
    }
    virtual SteamAPICall_t RequestLobbyList() { return g_Interface.RequestLobbyList(); }
    virtual void AddRequestLobbyListStringFilter(const char* key, const char* value, int comparison)
    {
        g_Interface.AddRequestLobbyListStringFilter(key, value, comparison);
    }
    virtual void AddRequestLobbyListNumericalFilter(const char* key, int value, int comparison)
    {
        g_Interface.AddRequestLobbyListNumericalFilter(key, value, comparison);
    }
    virtual void AddRequestLobbyListNearValueFilter(const char* key, int value)
    {
        g_Interface.AddRequestLobbyListNearValueFilter(key, value);
    }
    virtual void AddRequestLobbyListFilterSlotsAvailable(int slotsAvailable)
    {
        g_Interface.AddRequestLobbyListFilterSlotsAvailable(slotsAvailable);
    }
    virtual CSteamID* GetLobbyByIndex(CSteamID* outLobbyID, int index) { return WriteSteamIDReturn(outLobbyID, g_Interface.GetLobbyByIndexValue(index)); }
    virtual SteamAPICall_t CreateLobby(int lobbyType, int maxMembers) { return g_Interface.CreateLobby(lobbyType, maxMembers); }
    virtual SteamAPICall_t JoinLobby(CSteamID lobbyID) { return g_Interface.JoinLobby(lobbyID); }
    virtual void LeaveLobby(CSteamID lobbyID) { g_Interface.LeaveLobby(lobbyID); }
    virtual bool InviteUserToLobby(CSteamID lobbyID, CSteamID invitee) { return g_Interface.InviteUserToLobby(lobbyID, invitee); }
    virtual int GetNumLobbyMembers(CSteamID lobbyID) { return g_Interface.GetNumLobbyMembers(lobbyID); }
    virtual CSteamID* GetLobbyMemberByIndex(CSteamID* outUserID, CSteamID lobbyID, int member) { return WriteSteamIDReturn(outUserID, g_Interface.GetLobbyMemberByIndexValue(lobbyID, member)); }
    virtual const char* GetLobbyData(CSteamID lobbyID, const char* key) { return g_Interface.GetLobbyData(lobbyID, key); }
    virtual bool SetLobbyData(CSteamID lobbyID, const char* key, const char* value) { return g_Interface.SetLobbyData(lobbyID, key, value); }
    virtual int GetLobbyDataCount(CSteamID lobbyID) { return g_Interface.GetLobbyDataCount(lobbyID); }
    virtual bool GetLobbyDataByIndex(CSteamID lobbyID, int index, char* key, int keySize, char* value, int valueSize)
    {
        return g_Interface.GetLobbyDataByIndex(lobbyID, index, key, keySize, value, valueSize);
    }
    virtual bool DeleteLobbyData(CSteamID lobbyID, const char* key) { return g_Interface.DeleteLobbyData(lobbyID, key); }
    virtual const char* GetLobbyMemberData(CSteamID lobbyID, CSteamID user, const char* key)
    {
        return g_Interface.GetLobbyMemberData(lobbyID, user, key);
    }
    virtual void SetLobbyMemberData(CSteamID lobbyID, const char* key, const char* value)
    {
        g_Interface.SetLobbyMemberData(lobbyID, key, value);
    }
    virtual bool SendLobbyChatMsg(CSteamID lobbyID, const void* msgBody, int msgBodySize)
    {
        return g_Interface.SendLobbyChatMsg(lobbyID, msgBody, msgBodySize);
    }
    virtual int GetLobbyChatEntry(CSteamID lobbyID, int chatID, CSteamID* user, void* data, int dataSize, int* chatEntryType)
    {
        return g_Interface.GetLobbyChatEntry(lobbyID, chatID, user, data, dataSize, chatEntryType);
    }
    virtual bool RequestLobbyData(CSteamID lobbyID) { return g_Interface.RequestLobbyData(lobbyID); }
    virtual void SetLobbyGameServer(CSteamID lobbyID, uint32_t gameServerIP, uint16_t gameServerPort, CSteamID steamIDGameServer)
    {
        g_Interface.SetLobbyGameServer(lobbyID, gameServerIP, gameServerPort, steamIDGameServer);
    }
    virtual bool GetLobbyGameServer(CSteamID lobbyID, uint32_t* gameServerIP, uint16_t* gameServerPort, CSteamID* steamIDGameServer)
    {
        return g_Interface.GetLobbyGameServer(lobbyID, gameServerIP, gameServerPort, steamIDGameServer);
    }
    virtual bool SetLobbyMemberLimit(CSteamID lobbyID, int maxMembers) { return g_Interface.SetLobbyMemberLimit(lobbyID, maxMembers); }
    virtual int GetLobbyMemberLimit(CSteamID lobbyID) { return g_Interface.GetLobbyMemberLimit(lobbyID); }
    virtual bool SetLobbyType(CSteamID lobbyID, int lobbyType) { return g_Interface.SetLobbyType(lobbyID, lobbyType); }
    virtual bool SetLobbyJoinable(CSteamID lobbyID, bool joinable) { return g_Interface.SetLobbyJoinable(lobbyID, joinable); }
    virtual CSteamID* GetLobbyOwner(CSteamID* outOwner, CSteamID lobbyID) { return WriteSteamIDReturn(outOwner, g_Interface.GetLobbyOwnerValue(lobbyID)); }
    virtual bool SetLobbyOwner(CSteamID lobbyID, CSteamID newOwner) { return g_Interface.SetLobbyOwner(lobbyID, newOwner); }
};

class FakeSteamMatchmaking006 final
{
public:
    virtual int GetFavoriteGameCount() { return g_Interface.GetFavoriteGameCount(); }
    virtual bool GetFavoriteGame(int game, AppId_t* appID, uint32_t* ip, uint16_t* connPort, uint16_t* queryPort, uint32_t* flags, uint32_t* lastPlayed)
    {
        return g_Interface.GetFavoriteGame(game, appID, ip, connPort, queryPort, flags, lastPlayed);
    }
    virtual int AddFavoriteGame(AppId_t appID, uint32_t ip, uint16_t connPort, uint16_t queryPort, uint32_t flags, uint32_t lastPlayed)
    {
        return g_Interface.AddFavoriteGame(appID, ip, connPort, queryPort, flags, lastPlayed);
    }
    virtual bool RemoveFavoriteGame(AppId_t appID, uint32_t ip, uint16_t connPort, uint16_t queryPort, uint32_t flags)
    {
        return g_Interface.RemoveFavoriteGame(appID, ip, connPort, queryPort, flags);
    }
    virtual SteamAPICall_t RequestLobbyList() { return g_Interface.RequestLobbyList(); }
    virtual void AddRequestLobbyListFilter(const char* key, const char* value)
    {
        g_Interface.AddRequestLobbyListStringFilter(key, value, 0);
    }
    virtual void AddRequestLobbyListNumericalFilter(const char* key, int value, int comparison)
    {
        g_Interface.AddRequestLobbyListNumericalFilter(key, value, comparison);
    }
    virtual void AddRequestLobbyListNearValueFilter(const char* key, int value)
    {
        g_Interface.AddRequestLobbyListNearValueFilter(key, value);
    }
    virtual CSteamID* GetLobbyByIndex(CSteamID* outLobbyID, int index) { return WriteSteamIDReturn(outLobbyID, g_Interface.GetLobbyByIndexValue(index)); }
    virtual SteamAPICall_t CreateLobby(int lobbyType) { return g_Interface.CreateLobby(lobbyType, 4); }
    virtual SteamAPICall_t JoinLobby(CSteamID lobbyID) { return g_Interface.JoinLobby(lobbyID); }
    virtual void LeaveLobby(CSteamID lobbyID) { g_Interface.LeaveLobby(lobbyID); }
    virtual bool InviteUserToLobby(CSteamID lobbyID, CSteamID invitee) { return g_Interface.InviteUserToLobby(lobbyID, invitee); }
    virtual int GetNumLobbyMembers(CSteamID lobbyID) { return g_Interface.GetNumLobbyMembers(lobbyID); }
    virtual CSteamID* GetLobbyMemberByIndex(CSteamID* outUserID, CSteamID lobbyID, int member) { return WriteSteamIDReturn(outUserID, g_Interface.GetLobbyMemberByIndexValue(lobbyID, member)); }
    virtual const char* GetLobbyData(CSteamID lobbyID, const char* key) { return g_Interface.GetLobbyData(lobbyID, key); }
    virtual bool SetLobbyData(CSteamID lobbyID, const char* key, const char* value) { return g_Interface.SetLobbyData(lobbyID, key, value); }
    virtual const char* GetLobbyMemberData(CSteamID lobbyID, CSteamID user, const char* key)
    {
        return g_Interface.GetLobbyMemberData(lobbyID, user, key);
    }
    virtual void SetLobbyMemberData(CSteamID lobbyID, const char* key, const char* value)
    {
        g_Interface.SetLobbyMemberData(lobbyID, key, value);
    }
    virtual bool SendLobbyChatMsg(CSteamID lobbyID, const void* msgBody, int msgBodySize)
    {
        return g_Interface.SendLobbyChatMsg(lobbyID, msgBody, msgBodySize);
    }
    virtual int GetLobbyChatEntry(CSteamID lobbyID, int chatID, CSteamID* user, void* data, int dataSize, int* chatEntryType)
    {
        return g_Interface.GetLobbyChatEntry(lobbyID, chatID, user, data, dataSize, chatEntryType);
    }
    virtual bool RequestLobbyData(CSteamID lobbyID) { return g_Interface.RequestLobbyData(lobbyID); }
    virtual void SetLobbyGameServer(CSteamID lobbyID, uint32_t gameServerIP, uint16_t gameServerPort, CSteamID steamIDGameServer)
    {
        g_Interface.SetLobbyGameServer(lobbyID, gameServerIP, gameServerPort, steamIDGameServer);
    }
    virtual bool GetLobbyGameServer(CSteamID lobbyID, uint32_t* gameServerIP, uint16_t* gameServerPort, CSteamID* steamIDGameServer)
    {
        return g_Interface.GetLobbyGameServer(lobbyID, gameServerIP, gameServerPort, steamIDGameServer);
    }
    virtual bool SetLobbyMemberLimit(CSteamID lobbyID, int maxMembers) { return g_Interface.SetLobbyMemberLimit(lobbyID, maxMembers); }
    virtual int GetLobbyMemberLimit(CSteamID lobbyID) { return g_Interface.GetLobbyMemberLimit(lobbyID); }
    virtual bool SetLobbyType(CSteamID lobbyID, int lobbyType) { return g_Interface.SetLobbyType(lobbyID, lobbyType); }
    virtual CSteamID* GetLobbyOwner(CSteamID* outOwner, CSteamID lobbyID) { return WriteSteamIDReturn(outOwner, g_Interface.GetLobbyOwnerValue(lobbyID)); }
};

static FakeSteamMatchmaking006 g_Interface006;
static FakeSteamMatchmaking007 g_Interface007;
static FakeSteamMatchmaking008 g_Interface008;

void* FakeSteamInterfaces::Matchmaking()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::Matchmaking", "SteamMatchMaking009");
    Logger::Info("SteamMatchmaking: default SteamMatchMaking009 interface returned");
    return &g_Interface;
}

void* FakeSteamInterfaces::MatchmakingV006()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::MatchmakingV006", "SteamMatchMaking006");
    Logger::Info("SteamMatchmaking006: legacy offline interface returned");
    return &g_Interface006;
}

void* FakeSteamInterfaces::MatchmakingV007()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::MatchmakingV007", "SteamMatchMaking007");
    Logger::Info("SteamMatchmaking007: legacy offline interface returned");
    return &g_Interface007;
}

void* FakeSteamInterfaces::MatchmakingV008()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::MatchmakingV008", "SteamMatchMaking008");
    Logger::Info("SteamMatchmaking008: legacy offline interface returned");
    return &g_Interface008;
}

void* FakeSteamInterfaces::MatchmakingV009()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::MatchmakingV009", "SteamMatchMaking009");
    Logger::Info("SteamMatchmaking009: offline interface returned");
    return &g_Interface;
}
