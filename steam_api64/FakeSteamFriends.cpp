#include "StdInc.h"
#include "FakeSteamInterfaces.h"
#include "Logger.h"
#include "SteamDiagnostics.h"
#include "SteamIDManager.h"
#include "SteamPersonaManager.h"
#include "SteamCallbackManager.h"
#include "SteamCallResultManager.h"
#include "SteamVersionLogger.h"

#include <cctype>

namespace
{
    struct FriendGameInfoStub
    {
        uint64_t GameID;
        uint32_t GameIP;
        uint16_t GamePort;
        uint16_t QueryPort;
        CSteamID LobbyID;
    };

    struct SetPersonaNameResponseResult
    {
        bool m_bSuccess;
        bool m_bLocalSuccess;
        int m_result;
    };

    struct AvatarImageLoadedResult
    {
        CSteamID m_steamID;
        int m_iImage;
        int m_iWide;
        int m_iTall;
    };

    void TraceFriends(const char* functionName, const std::string& detail = std::string())
    {
        SteamVersionLogger::LogCall("SteamFriends", functionName, detail);
    }

    std::string SteamIDText(uint64_t value)
    {
        std::ostringstream ss;
        ss << value << "/0x" << std::hex << value;
        return ss.str();
    }

    bool IsReadableMemory(const void* ptr, size_t size)
    {
        if (!ptr || size == 0)
            return false;

        const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
        if (address < 0x10000ull || address > 0x00007FFFFFFFFFFFull)
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

    bool IsLikelySteamID64(uint64_t value)
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

    bool TryReadPointedSteamID(uint64_t raw, uint64_t& pointed)
    {
        pointed = 0;

        const void* ptr = reinterpret_cast<const void*>(static_cast<uintptr_t>(raw));
        if (!IsReadableMemory(ptr, sizeof(pointed)))
            return false;

        std::memcpy(&pointed, ptr, sizeof(pointed));
        return true;
    }

    bool LooksLikeProcessPointer(uint64_t value)
    {
        if (value < 0x10000ull || value == UINT64_MAX)
            return value == UINT64_MAX;

        if (IsLikelySteamID64(value))
            return false;

        MEMORY_BASIC_INFORMATION mbi{};
        return VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(value)), &mbi, sizeof(mbi)) != 0;
    }

    CSteamID ResolveSteamIDArgument(CSteamID rawID, const char* functionName)
    {
        const uint64_t raw = static_cast<uint64_t>(rawID);
        if (raw == 0 || IsLikelySteamID64(raw))
            return rawID;

        uint64_t pointed = 0;
        if (TryReadPointedSteamID(raw, pointed) && IsLikelySteamID64(pointed))
        {
            const std::string detail =
                std::string(functionName ? functionName : "unknown") +
                " rawPointer=" +
                SteamIDText(raw) +
                " pointedSteamID=" +
                SteamIDText(pointed);
            SteamDiagnostics::MarkSteam("FriendsSteamIDPointerResolved", detail);
            TraceFriends("SteamIDPointerResolved", detail);
            return static_cast<CSteamID>(pointed);
        }

        if (LooksLikeProcessPointer(raw))
        {
            const CSteamID fallback = SteamIDManager::GetLocalSteamID();
            const std::string detail =
                std::string(functionName ? functionName : "unknown") +
                " rawPointer=" +
                SteamIDText(raw) +
                " fallbackLocal=" +
                SteamIDText(static_cast<uint64_t>(fallback));
            SteamDiagnostics::MarkSteam("FriendsSteamIDPointerFallback", detail);
            TraceFriends("SteamIDPointerFallback", detail);
            return fallback;
        }

        return rawID;
    }

    std::string SteamIDDetail(CSteamID rawID, CSteamID resolvedID)
    {
        const uint64_t raw = static_cast<uint64_t>(rawID);
        const uint64_t resolved = static_cast<uint64_t>(resolvedID);

        if (raw == resolved)
            return SteamIDText(resolved);

        return "raw=" + SteamIDText(raw) + " resolved=" + SteamIDText(resolved);
    }

    std::string UpperInterfaceName(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return value;
    }

    std::string SafeVersionString(const char* value, const char* fallback)
    {
        if (!value || !IsReadableMemory(value, 1))
            return fallback;

        std::string text;
        text.reserve(32);

        for (size_t i = 0; i < 64; ++i)
        {
            const char* current = value + i;
            if (!IsReadableMemory(current, 1))
                break;

            const char c = *current;
            if (c == '\0')
                break;

            text.push_back(c);
        }

        return text.empty() ? fallback : text;
    }

    bool IsWritableMemory(void* ptr, size_t size)
    {
        if (!ptr || size == 0)
            return false;

        const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
        if (address < 0x10000ull || address > 0x00007FFFFFFFFFFFull)
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

    bool SafeZero(void* ptr, size_t size, const char* fieldName)
    {
        if (!ptr || size == 0)
            return false;

        if (!IsWritableMemory(ptr, size))
        {
            Logger::Error(std::string("SteamFriends skipped invalid buffer write: ") + fieldName);
            return false;
        }

        std::memset(ptr, 0, size);
        return true;
    }

    bool SafeCopy(void* out, size_t outSize, const void* data, size_t dataSize, const char* fieldName)
    {
        if (!SafeZero(out, outSize, fieldName))
            return false;

        if (data && dataSize > 0)
            std::memcpy(out, data, std::min(outSize, dataSize));

        return true;
    }

    void SafeWriteInt(int* out, int value, const char* fieldName)
    {
        if (!out)
            return;

        if (!IsWritableMemory(out, sizeof(*out)))
        {
            Logger::Error(std::string("SteamFriends skipped invalid int write: ") + fieldName);
            return;
        }

        *out = value;
    }

    void PushAvatarLoaded(CSteamID id, int image, int edge)
    {
        id = ResolveSteamIDArgument(id, "PushAvatarLoaded");
        NSR_UNUSED(id);
        NSR_UNUSED(image);
        NSR_UNUSED(edge);
        return;

        AvatarImageLoadedResult result{};
        result.m_steamID = id;
        result.m_iImage = image;
        result.m_iWide = edge;
        result.m_iTall = edge;
        SteamCallbackManager::PushCallback(334, &result, sizeof(result));
    }
}

class FakeSteamFriends final
{
public:
    virtual const char* GetPersonaName()
    {
        TraceFriends("GetPersonaName");
        Logger::Info("SteamFriends::GetPersonaName");
        return SteamPersonaManager::GetPersonaName();
    }

    virtual SteamAPICall_t SetPersonaName(const char* name)
    {
        TraceFriends("SetPersonaName", name ? name : "null");
        Logger::Info("SteamFriends::SetPersonaName");

        if (name)
            SteamPersonaManager::SetPersonaName(name);

        SetPersonaNameResponseResult result{};
        result.m_bSuccess = true;
        result.m_bLocalSuccess = true;
        result.m_result = 1;
        return SteamCallResultManager::CreateCallResult(347, &result, sizeof(result), true);
    }

    virtual int GetPersonaState()
    {
        TraceFriends("GetPersonaState");
        Logger::Info("SteamFriends::GetPersonaState");
        return 1;
    }

    virtual int GetFriendCount(int flags)
    {
        TraceFriends("GetFriendCount", "flags=" + std::to_string(flags));
        NSR_UNUSED(flags);
        Logger::Info("SteamFriends::GetFriendCount");
        return 0;
    }

    CSteamID GetFriendByIndexValue(int index, int flags)
    {
        TraceFriends("GetFriendByIndex", "index=" + std::to_string(index) + " flags=" + std::to_string(flags));
        NSR_UNUSED(flags);
        Logger::Info("SteamFriends::GetFriendByIndex");
        return SteamPersonaManager::GetFriendByIndex(index);
    }

    virtual CSteamID GetFriendByIndex(int index, int flags)
    {
        return GetFriendByIndexValue(index, flags);
    }

    virtual int GetFriendRelationship(CSteamID id)
    {
        id = ResolveSteamIDArgument(id, "GetFriendRelationship");
        TraceFriends("GetFriendRelationship", std::to_string(static_cast<unsigned long long>(id)));
        NSR_UNUSED(id);
        Logger::Info("SteamFriends::GetFriendRelationship");
        return 0;
    }

    virtual int GetFriendPersonaState(CSteamID id)
    {
        id = ResolveSteamIDArgument(id, "GetFriendPersonaState");
        TraceFriends("GetFriendPersonaState", std::to_string(static_cast<unsigned long long>(id)));
        NSR_UNUSED(id);
        Logger::Info("SteamFriends::GetFriendPersonaState");
        return 1;
    }

    virtual const char* GetFriendPersonaName(CSteamID id)
    {
        id = ResolveSteamIDArgument(id, "GetFriendPersonaName");
        TraceFriends("GetFriendPersonaName", std::to_string(static_cast<unsigned long long>(id)));
        Logger::Info("SteamFriends::GetFriendPersonaName");
        return SteamPersonaManager::GetFriendPersonaName(id);
    }

    virtual bool GetFriendGamePlayed(CSteamID id, void* info)
    {
        id = ResolveSteamIDArgument(id, "GetFriendGamePlayed");
        TraceFriends("GetFriendGamePlayed", std::to_string(static_cast<unsigned long long>(id)));
        NSR_UNUSED(id);
        Logger::Info("SteamFriends::GetFriendGamePlayed");

        if (info)
        {
            FriendGameInfoStub gameInfo{};
            SafeCopy(info, sizeof(gameInfo), &gameInfo, sizeof(gameInfo), "GetFriendGamePlayed");
        }

        return false;
    }

    virtual const char* GetFriendPersonaNameHistory(CSteamID id, int index)
    {
        NSR_UNUSED(id);
        NSR_UNUSED(index);
        Logger::Info("SteamFriends::GetFriendPersonaNameHistory");
        return "";
    }

    virtual int GetFriendSteamLevel(CSteamID id)
    {
        NSR_UNUSED(id);
        return 0;
    }

    virtual const char* GetPlayerNickname(CSteamID id)
    {
        NSR_UNUSED(id);
        Logger::Info("SteamFriends::GetPlayerNickname");
        return "";
    }

    virtual int GetFriendsGroupCount()
    {
        Logger::Info("SteamFriends::GetFriendsGroupCount");
        return 0;
    }

    virtual int16_t GetFriendsGroupIDByIndex(int index)
    {
        NSR_UNUSED(index);
        Logger::Info("SteamFriends::GetFriendsGroupIDByIndex");
        return -1;
    }

    virtual const char* GetFriendsGroupName(int16_t group)
    {
        NSR_UNUSED(group);
        Logger::Info("SteamFriends::GetFriendsGroupName");
        return "";
    }

    virtual int GetFriendsGroupMembersCount(int16_t group)
    {
        NSR_UNUSED(group);
        Logger::Info("SteamFriends::GetFriendsGroupMembersCount");
        return 0;
    }

    virtual void GetFriendsGroupMembersList(
        int16_t group,
        CSteamID* outMembers,
        int count)
    {
        NSR_UNUSED(group);
        Logger::Info("SteamFriends::GetFriendsGroupMembersList");

        if (outMembers && count > 0)
            SafeZero(outMembers, sizeof(CSteamID) * static_cast<size_t>(count), "GetFriendsGroupMembersList");
    }

    virtual bool HasFriend(CSteamID id, int flags)
    {
        id = ResolveSteamIDArgument(id, "HasFriend");
        NSR_UNUSED(flags);
        Logger::Info("SteamFriends::HasFriend");
        for (int i = 0; i < SteamPersonaManager::GetFriendCount(); ++i)
        {
            if (SteamPersonaManager::GetFriendByIndex(i) == id)
                return true;
        }

        return false;
    }

    virtual int GetClanCount()
    {
        Logger::Info("SteamFriends::GetClanCount");
        return 0;
    }

    CSteamID GetClanByIndexValue(int index)
    {
        NSR_UNUSED(index);
        Logger::Info("SteamFriends::GetClanByIndex");
        return 0;
    }

    virtual CSteamID GetClanByIndex(int index)
    {
        return GetClanByIndexValue(index);
    }

    virtual const char* GetClanName(CSteamID id)
    {
        NSR_UNUSED(id);
        Logger::Info("SteamFriends::GetClanName");
        return "";
    }

    virtual const char* GetClanTag(CSteamID id)
    {
        NSR_UNUSED(id);
        return "";
    }

    virtual bool GetClanActivityCounts(
        CSteamID id,
        int* online,
        int* inGame,
        int* chatting)
    {
        NSR_UNUSED(id);
        NSR_UNUSED(online);
        NSR_UNUSED(inGame);
        NSR_UNUSED(chatting);
        Logger::Info("SteamFriends::GetClanActivityCounts");
        return false;
    }

    virtual SteamAPICall_t DownloadClanActivityCounts(CSteamID* clans, int count)
    {
        NSR_UNUSED(clans);
        NSR_UNUSED(count);
        Logger::Info("SteamFriends::DownloadClanActivityCounts");
        return 0;
    }

    virtual int GetFriendCountFromSource(CSteamID source)
    {
        NSR_UNUSED(source);
        Logger::Info("SteamFriends::GetFriendCountFromSource");
        return 0;
    }

    CSteamID GetFriendFromSourceByIndexValue(CSteamID source, int index)
    {
        NSR_UNUSED(source);
        NSR_UNUSED(index);
        Logger::Info("SteamFriends::GetFriendFromSourceByIndex");
        return 0;
    }

    virtual CSteamID GetFriendFromSourceByIndex(CSteamID source, int index)
    {
        return GetFriendFromSourceByIndexValue(source, index);
    }

    virtual bool IsUserInSource(CSteamID user, CSteamID source)
    {
        NSR_UNUSED(user);
        NSR_UNUSED(source);
        Logger::Info("SteamFriends::IsUserInSource");
        return false;
    }

    virtual void SetInGameVoiceSpeaking(CSteamID user, bool speaking)
    {
        NSR_UNUSED(user);
        NSR_UNUSED(speaking);
        Logger::Info("SteamFriends::SetInGameVoiceSpeaking");
    }

    virtual void ActivateGameOverlay(const char* dialog)
    {
        TraceFriends("ActivateGameOverlay", dialog ? dialog : "null");
        std::string value = dialog ? dialog : "";
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (value.find("invite") != std::string::npos ||
            value.find("lobbyinvite") != std::string::npos ||
            value.find("friend") != std::string::npos)
        {
        }
        Logger::Info("SteamFriends::ActivateGameOverlay");
    }

    virtual void ActivateGameOverlayToUser(const char* dialog, CSteamID id)
    {
        id = ResolveSteamIDArgument(id, "ActivateGameOverlayToUser");
        TraceFriends("ActivateGameOverlayToUser", dialog ? dialog : "null");
        NSR_UNUSED(dialog);
        NSR_UNUSED(id);
        Logger::Info("SteamFriends::ActivateGameOverlayToUser");
    }

    virtual void ActivateGameOverlayToWebPage(const char* url)
    {
        TraceFriends("ActivateGameOverlayToWebPage", url ? url : "null");
        NSR_UNUSED(url);
        Logger::Info("SteamFriends::ActivateGameOverlayToWebPage");
    }

    virtual void ActivateGameOverlayToStore(AppId_t appId, int flag)
    {
        TraceFriends("ActivateGameOverlayToStore", "app=" + std::to_string(appId));
        NSR_UNUSED(appId);
        NSR_UNUSED(flag);
        Logger::Info("SteamFriends::ActivateGameOverlayToStore");
    }

    virtual void SetPlayedWith(CSteamID id)
    {
        id = ResolveSteamIDArgument(id, "SetPlayedWith");
        NSR_UNUSED(id);
        Logger::Info("SteamFriends::SetPlayedWith");
    }

    virtual void ActivateGameOverlayInviteDialog(CSteamID lobby)
    {
        lobby = ResolveSteamIDArgument(lobby, "ActivateGameOverlayInviteDialog");
        TraceFriends("ActivateGameOverlayInviteDialog", std::to_string(static_cast<unsigned long long>(lobby)));
        NSR_UNUSED(lobby);
        Logger::Info("SteamFriends::ActivateGameOverlayInviteDialog");
    }

    virtual int GetSmallFriendAvatar(CSteamID id)
    {
        const CSteamID resolved = ResolveSteamIDArgument(id, "GetSmallFriendAvatar");
        TraceFriends("GetSmallFriendAvatar", SteamIDDetail(id, resolved));
        Logger::Info("SteamFriends::GetSmallFriendAvatar");
        NSR_UNUSED(resolved);
        return 0;
    }

    virtual int GetMediumFriendAvatar(CSteamID id)
    {
        const CSteamID resolved = ResolveSteamIDArgument(id, "GetMediumFriendAvatar");
        TraceFriends("GetMediumFriendAvatar", SteamIDDetail(id, resolved));
        Logger::Info("SteamFriends::GetMediumFriendAvatar");
        NSR_UNUSED(resolved);
        return 0;
    }

    virtual int GetLargeFriendAvatar(CSteamID id)
    {
        const CSteamID resolved = ResolveSteamIDArgument(id, "GetLargeFriendAvatar");
        TraceFriends("GetLargeFriendAvatar", SteamIDDetail(id, resolved));
        NSR_UNUSED(resolved);
        return 0;
    }

    virtual bool RequestUserInformation(CSteamID id, bool nameOnly)
    {
        // SteamFriends015 takes CSteamID by value. Do not pointer-probe garbage values here.
        // Legacy crash logs showed a pointer-looking value immediately before an execute-null crash.
        const uint64_t raw = static_cast<uint64_t>(id);
        if (!IsLikelySteamID64(raw))
        {
            const CSteamID local = SteamIDManager::GetLocalSteamID();
            TraceFriends(
                "RequestUserInformationFallbackLocal",
                "raw=" + SteamIDText(raw) + " fallbackLocal=" + SteamIDText(static_cast<uint64_t>(local)) +
                    (nameOnly ? " nameOnly=1" : " nameOnly=0"));
            id = local;
        }
        else
        {
            TraceFriends(
                "RequestUserInformation",
                SteamIDText(raw) + (nameOnly ? " nameOnly=1" : " nameOnly=0"));
        }

        NSR_UNUSED(id);
        NSR_UNUSED(nameOnly);
        Logger::Info("SteamFriends015::RequestUserInformation offline-cache-hit");
        return false;
    }

    virtual SteamAPICall_t RequestClanOfficerList(CSteamID clan)
    {
        NSR_UNUSED(clan);
        Logger::Info("SteamFriends::RequestClanOfficerList");
        return 0;
    }

    CSteamID GetClanOwnerValue(CSteamID clan)
    {
        NSR_UNUSED(clan);
        Logger::Info("SteamFriends::GetClanOwner");
        return 0;
    }

    virtual CSteamID GetClanOwner(CSteamID clan)
    {
        return GetClanOwnerValue(clan);
    }

    virtual int GetClanOfficerCount(CSteamID clan)
    {
        NSR_UNUSED(clan);
        Logger::Info("SteamFriends::GetClanOfficerCount");
        return 0;
    }

    CSteamID GetClanOfficerByIndexValue(CSteamID clan, int index)
    {
        NSR_UNUSED(clan);
        NSR_UNUSED(index);
        Logger::Info("SteamFriends::GetClanOfficerByIndex");
        return 0;
    }

    virtual CSteamID GetClanOfficerByIndex(CSteamID clan, int index)
    {
        return GetClanOfficerByIndexValue(clan, index);
    }

    virtual uint32_t GetUserRestrictions()
    {
        Logger::Info("SteamFriends::GetUserRestrictions");
        return 0;
    }

    virtual bool SetRichPresence(const char* key, const char* value)
    {
        TraceFriends("SetRichPresence", std::string(key ? key : "null") + "=" + (value ? value : "null"));
        Logger::Info("SteamFriends::SetRichPresence");

        if (key)
            SteamPersonaManager::SetRichPresence(key, value ? value : "");

        return true;
    }

    virtual void ClearRichPresence()
    {
        TraceFriends("ClearRichPresence");
        Logger::Info("SteamFriends::ClearRichPresence");
        SteamPersonaManager::ClearRichPresence();
    }

    virtual const char* GetFriendRichPresence(CSteamID id, const char* key)
    {
        id = ResolveSteamIDArgument(id, "GetFriendRichPresence");
        TraceFriends("GetFriendRichPresence", std::string(key ? key : "null"));
        NSR_UNUSED(id);
        Logger::Info("SteamFriends::GetFriendRichPresence");
        return SteamPersonaManager::GetRichPresence(key ? key : "");
    }

    virtual int GetFriendRichPresenceKeyCount(CSteamID id)
    {
        id = ResolveSteamIDArgument(id, "GetFriendRichPresenceKeyCount");
        TraceFriends("GetFriendRichPresenceKeyCount", std::to_string(static_cast<unsigned long long>(id)));
        NSR_UNUSED(id);
        Logger::Info("SteamFriends::GetFriendRichPresenceKeyCount");
        return SteamPersonaManager::GetRichPresenceKeyCount();
    }

    virtual const char* GetFriendRichPresenceKeyByIndex(CSteamID id, int index)
    {
        id = ResolveSteamIDArgument(id, "GetFriendRichPresenceKeyByIndex");
        TraceFriends("GetFriendRichPresenceKeyByIndex", "index=" + std::to_string(index));
        NSR_UNUSED(id);
        Logger::Info("SteamFriends::GetFriendRichPresenceKeyByIndex");
        return SteamPersonaManager::GetRichPresenceKeyByIndex(index);
    }

    virtual void RequestFriendRichPresence(CSteamID id)
    {
        id = ResolveSteamIDArgument(id, "RequestFriendRichPresence");
        TraceFriends("RequestFriendRichPresence", std::to_string(static_cast<unsigned long long>(id)));
        NSR_UNUSED(id);
        Logger::Info("SteamFriends::RequestFriendRichPresence");
    }

    virtual bool InviteUserToGame(CSteamID id, const char* connectString)
    {
        id = ResolveSteamIDArgument(id, "InviteUserToGame");
        TraceFriends("InviteUserToGame", connectString ? connectString : "null");
        NSR_UNUSED(id);
        NSR_UNUSED(connectString);
        Logger::Info("SteamFriends::InviteUserToGame");
        return false;
    }

    virtual int GetCoplayFriendCount()
    {
        Logger::Info("SteamFriends::GetCoplayFriendCount");
        return 0;
    }

    CSteamID GetCoplayFriendValue(int index)
    {
        NSR_UNUSED(index);
        Logger::Info("SteamFriends::GetCoplayFriend");
        return 0;
    }

    virtual CSteamID GetCoplayFriend(int index)
    {
        return GetCoplayFriendValue(index);
    }

    virtual int GetFriendCoplayTime(CSteamID id)
    {
        NSR_UNUSED(id);
        Logger::Info("SteamFriends::GetFriendCoplayTime");
        return 0;
    }

    virtual AppId_t GetFriendCoplayGame(CSteamID id)
    {
        NSR_UNUSED(id);
        Logger::Info("SteamFriends::GetFriendCoplayGame");
        return 0;
    }

    virtual SteamAPICall_t JoinClanChatRoom(CSteamID clan)
    {
        NSR_UNUSED(clan);
        Logger::Info("SteamFriends::JoinClanChatRoom");
        return 0;
    }

    virtual bool LeaveClanChatRoom(CSteamID clan)
    {
        NSR_UNUSED(clan);
        Logger::Info("SteamFriends::LeaveClanChatRoom");
        return true;
    }

    virtual int GetClanChatMemberCount(CSteamID clan)
    {
        NSR_UNUSED(clan);
        Logger::Info("SteamFriends::GetClanChatMemberCount");
        return 0;
    }

    CSteamID GetChatMemberByIndexValue(CSteamID clan, int user)
    {
        NSR_UNUSED(clan);
        NSR_UNUSED(user);
        Logger::Info("SteamFriends::GetChatMemberByIndex");
        return 0;
    }

    virtual CSteamID GetChatMemberByIndex(CSteamID clan, int user)
    {
        return GetChatMemberByIndexValue(clan, user);
    }

    virtual bool SendClanChatMessage(CSteamID chat, const char* text)
    {
        NSR_UNUSED(chat);
        NSR_UNUSED(text);
        Logger::Info("SteamFriends::SendClanChatMessage");
        return false;
    }

    virtual int GetClanChatMessage(
        CSteamID chat,
        int message,
        void* text,
        int maxText,
        int* entryType,
        CSteamID* chatter)
    {
        TraceFriends("GetClanChatMessage", "message=" + std::to_string(message));
        NSR_UNUSED(chat);
        NSR_UNUSED(message);
        Logger::Info("SteamFriends::GetClanChatMessage");

        if (text && maxText > 0)
            SafeZero(text, static_cast<size_t>(maxText), "GetClanChatMessage text");

        if (entryType)
            *entryType = 0;

        if (chatter)
            *chatter = 0;

        return 0;
    }

    virtual bool IsClanChatAdmin(CSteamID chat, CSteamID user)
    {
        NSR_UNUSED(chat);
        NSR_UNUSED(user);
        Logger::Info("SteamFriends::IsClanChatAdmin");
        return false;
    }

    virtual bool IsClanChatWindowOpenInSteam(CSteamID chat)
    {
        NSR_UNUSED(chat);
        Logger::Info("SteamFriends::IsClanChatWindowOpenInSteam");
        return false;
    }

    virtual bool OpenClanChatWindowInSteam(CSteamID chat)
    {
        NSR_UNUSED(chat);
        Logger::Info("SteamFriends::OpenClanChatWindowInSteam");
        return false;
    }

    virtual bool CloseClanChatWindowInSteam(CSteamID chat)
    {
        NSR_UNUSED(chat);
        Logger::Info("SteamFriends::CloseClanChatWindowInSteam");
        return true;
    }

    virtual bool SetListenForFriendsMessages(bool enabled)
    {
        TraceFriends("SetListenForFriendsMessages", enabled ? "true" : "false");
        NSR_UNUSED(enabled);
        Logger::Info("SteamFriends::SetListenForFriendsMessages");
        return true;
    }

    virtual bool ReplyToFriendMessage(CSteamID id, const char* message)
    {
        TraceFriends("ReplyToFriendMessage", message ? message : "null");
        NSR_UNUSED(id);
        NSR_UNUSED(message);
        Logger::Info("SteamFriends::ReplyToFriendMessage");
        return false;
    }

    virtual int GetFriendMessage(
        CSteamID id,
        int message,
        void* data,
        int dataSize,
        int* entryType)
    {
        TraceFriends("GetFriendMessage", "message=" + std::to_string(message));
        NSR_UNUSED(id);
        NSR_UNUSED(message);
        Logger::Info("SteamFriends::GetFriendMessage");

        if (data && dataSize > 0)
            SafeZero(data, static_cast<size_t>(dataSize), "GetFriendMessage data");

        if (entryType)
            *entryType = 0;

        return 0;
    }

    virtual SteamAPICall_t GetFollowerCount(CSteamID id)
    {
        NSR_UNUSED(id);
        Logger::Info("SteamFriends::GetFollowerCount");
        return 0;
    }

    virtual SteamAPICall_t IsFollowing(CSteamID id)
    {
        NSR_UNUSED(id);
        Logger::Info("SteamFriends::IsFollowing");
        return 0;
    }

    virtual SteamAPICall_t EnumerateFollowingList(uint32_t start)
    {
        NSR_UNUSED(start);
        Logger::Info("SteamFriends::EnumerateFollowingList");
        return 0;
    }

    virtual bool IsClanPublic(CSteamID clan)
    {
        NSR_UNUSED(clan);
        Logger::Info("SteamFriends::IsClanPublic");
        return false;
    }

    virtual bool IsClanOfficialGameGroup(CSteamID clan)
    {
        NSR_UNUSED(clan);
        Logger::Info("SteamFriends::IsClanOfficialGameGroup");
        return false;
    }

    virtual int GetNumChatsWithUnreadPriorityMessages()
    {
        Logger::Info("SteamFriends::GetNumChatsWithUnreadPriorityMessages");
        return 0;
    }

    virtual void ActivateGameOverlayRemotePlayTogetherInviteDialog(CSteamID lobby)
    {
        TraceFriends("ActivateGameOverlayRemotePlayTogetherInviteDialog", std::to_string(static_cast<unsigned long long>(lobby)));
        NSR_UNUSED(lobby);
        Logger::Info("SteamFriends::ActivateGameOverlayRemotePlayTogetherInviteDialog");
    }

    virtual bool RegisterProtocolInOverlayBrowser(const char* protocol)
    {
        TraceFriends("RegisterProtocolInOverlayBrowser", protocol ? protocol : "null");
        NSR_UNUSED(protocol);
        Logger::Info("SteamFriends::RegisterProtocolInOverlayBrowser");
        return false;
    }

    virtual void ActivateGameOverlayInviteDialogConnectString(const char* connectString)
    {
        TraceFriends("ActivateGameOverlayInviteDialogConnectString", connectString ? connectString : "null");
        NSR_UNUSED(connectString);
        Logger::Info("SteamFriends::ActivateGameOverlayInviteDialogConnectString");
    }

    virtual SteamAPICall_t RequestEquippedProfileItems(CSteamID id)
    {
        id = ResolveSteamIDArgument(id, "RequestEquippedProfileItems");
        TraceFriends("RequestEquippedProfileItems", std::to_string(static_cast<unsigned long long>(id)));
        Logger::Info("SteamFriends::RequestEquippedProfileItems");

        struct EquippedProfileItemsResult
        {
            int m_eResult;
            CSteamID m_steamID;
            bool m_bHasAnimatedAvatar;
            bool m_bHasAvatarFrame;
            bool m_bHasProfileModifier;
            bool m_bHasProfileBackground;
            bool m_bHasMiniProfileBackground;
        };

        EquippedProfileItemsResult result{};
        result.m_eResult = 1;
        result.m_steamID = id;

        return SteamCallResultManager::CreateCallResult(
            351,
            &result,
            sizeof(result),
            true);
    }

    virtual bool BHasEquippedProfileItem(CSteamID id, int itemType)
    {
        id = ResolveSteamIDArgument(id, "BHasEquippedProfileItem");
        TraceFriends("BHasEquippedProfileItem", "type=" + std::to_string(itemType));
        NSR_UNUSED(id);
        NSR_UNUSED(itemType);
        Logger::Info("SteamFriends::BHasEquippedProfileItem");
        return false;
    }

    virtual const char* GetProfileItemPropertyString(CSteamID id, int itemType, int prop)
    {
        id = ResolveSteamIDArgument(id, "GetProfileItemPropertyString");
        TraceFriends("GetProfileItemPropertyString", "type=" + std::to_string(itemType) + " prop=" + std::to_string(prop));
        NSR_UNUSED(id);
        NSR_UNUSED(itemType);
        NSR_UNUSED(prop);
        Logger::Info("SteamFriends::GetProfileItemPropertyString");
        return "";
    }

    virtual uint32_t GetProfileItemPropertyUint(CSteamID id, int itemType, int prop)
    {
        id = ResolveSteamIDArgument(id, "GetProfileItemPropertyUint");
        TraceFriends("GetProfileItemPropertyUint", "type=" + std::to_string(itemType) + " prop=" + std::to_string(prop));
        NSR_UNUSED(id);
        NSR_UNUSED(itemType);
        NSR_UNUSED(prop);
        Logger::Info("SteamFriends::GetProfileItemPropertyUint");
        return 0;
    }
};

static FakeSteamFriends g_Interface;

void* FakeSteamInterfaces::Friends()
{
    return FakeSteamInterfaces::FriendsForVersion("SteamFriends015");
}

void* FakeSteamInterfaces::FriendsForVersion(const char* version)
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::Friends", "SteamFriends");
    const std::string requested = SafeVersionString(version, "SteamFriends015");
    const std::string upper = UpperInterfaceName(requested);
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::FriendsForVersion", requested.c_str());

    if (upper.find("STEAMFRIENDS") != std::string::npos)
        Logger::Info("SteamFriends: offline emulated interface returned for " + requested);
    else
        Logger::Info("SteamFriends: offline emulated interface returned");

    return &g_Interface;
}
