#include "StdInc.h"
#include "FakeSteamInterfaces.h"
#include "Logger.h"
#include "SteamStatsLocal.h"
#include "SteamCallbackManager.h"
#include "SteamCallResultManager.h"
#include "SteamIDManager.h"
#include "SteamDiagnostics.h"
#include "SteamVersionLogger.h"

typedef uint64_t SteamLeaderboard_t;
typedef uint64_t SteamLeaderboardEntries_t;
typedef uint64_t UGCHandle_t;

namespace
{
    constexpr uint64_t kOfflineGameID = 1276760;
    constexpr SteamLeaderboard_t kOfflineLeaderboard = 1;
    constexpr SteamLeaderboardEntries_t kOfflineLeaderboardEntries = 1;
    constexpr int kResultOK = 1;

    struct UserStatsReceivedResult
    {
        uint64_t m_nGameID;
        int m_eResult;
        CSteamID m_steamIDUser;
    };

    struct UserStatsStoredResult
    {
        uint64_t m_nGameID;
        int m_eResult;
    };

    struct UserAchievementStoredResult
    {
        uint64_t m_nGameID;
        bool m_bGroupAchievement;
        char m_rgchAchievementName[128];
        uint32_t m_nCurProgress;
        uint32_t m_nMaxProgress;
    };

    struct LeaderboardFindResult
    {
        SteamLeaderboard_t m_hSteamLeaderboard;
        uint8_t m_bLeaderboardFound;
    };

    struct LeaderboardScoresDownloadedResult
    {
        SteamLeaderboard_t m_hSteamLeaderboard;
        SteamLeaderboardEntries_t m_hSteamLeaderboardEntries;
        int m_cEntryCount;
    };

    struct LeaderboardEntryStub
    {
        CSteamID m_steamIDUser;
        int32_t m_nGlobalRank;
        int32_t m_nScore;
        int32_t m_cDetails;
        UGCHandle_t m_hUGC;
    };

    struct LeaderboardScoreUploadedResult
    {
        uint8_t m_bSuccess;
        SteamLeaderboard_t m_hSteamLeaderboard;
        int32_t m_nScore;
        uint8_t m_bScoreChanged;
        int m_nGlobalRankNew;
        int m_nGlobalRankPrevious;
    };

    struct LeaderboardUGCSetResult
    {
        int m_eResult;
        SteamLeaderboard_t m_hSteamLeaderboard;
    };

    struct NumberOfCurrentPlayersResult
    {
        uint8_t m_bSuccess;
        int32_t m_cPlayers;
    };

    struct GlobalAchievementPercentagesReadyResult
    {
        uint64_t m_nGameID;
        int m_eResult;
    };

    struct GlobalStatsReceivedResult
    {
        uint64_t m_nGameID;
        int m_eResult;
    };

    std::string g_DisplayAttributeValue;
    std::mutex g_LeaderboardMutex;
    std::map<std::string, SteamLeaderboard_t> g_LeaderboardByName;
    std::map<SteamLeaderboard_t, std::string> g_LeaderboardNames;
    std::map<SteamLeaderboard_t, std::vector<LeaderboardEntryStub>> g_LeaderboardRows;
    std::map<SteamLeaderboardEntries_t, std::vector<LeaderboardEntryStub>> g_DownloadedEntries;
    std::map<SteamLeaderboard_t, ULONGLONG> g_LeaderboardLastRefreshTick;
    std::map<SteamLeaderboard_t, ULONGLONG> g_LeaderboardLastEnsureTick;
    std::set<SteamLeaderboard_t> g_LeaderboardRefreshInFlight;
    SteamLeaderboard_t g_NextLeaderboardHandle = kOfflineLeaderboard;
    SteamLeaderboardEntries_t g_NextEntriesHandle = kOfflineLeaderboardEntries;

    void TraceUserStats(const char* functionName, const std::string& detail = std::string())
    {
        SteamVersionLogger::LogCall("SteamUserStats", functionName, detail);
    }

    UserStatsReceivedResult MakeUserStatsReceived(CSteamID user)
    {
        UserStatsReceivedResult result{};
        result.m_nGameID = kOfflineGameID;
        result.m_eResult = kResultOK;
        result.m_steamIDUser = user ? user : SteamIDManager::GetLocalSteamID();
        return result;
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

    template <typename T>
    bool SafeWrite(T* out, const T& value, const char* fieldName)
    {
        if (!out)
            return false;

        if (!IsWritableMemory(out, sizeof(T)))
        {
            Logger::Error(std::string("SteamUserStats skipped invalid output write: ") + fieldName);
            return false;
        }

        *out = value;
        return true;
    }

    std::string LeaderboardPath()
    {
        char path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, path, MAX_PATH);

        std::filesystem::path base(path);
        std::filesystem::path out = base.parent_path() / "BreakersRevived" / "leaderboards.ini";

        try
        {
            std::filesystem::create_directories(out.parent_path());
        }
        catch (...)
        {
        }

        return out.string();
    }

    std::string CleanLeaderboardName(const char* name)
    {
        std::string value = name && name[0] ? name : "BreakersRevivedLeaderboard";

        for (char& c : value)
        {
            if (c == '\r' || c == '\n' || c == '[' || c == ']')
                c = '_';
        }

        return value;
    }

    std::string LeaderboardSection(const std::string& name)
    {
        return "leaderboard_" + name;
    }

    void RankRows(std::vector<LeaderboardEntryStub>& rows)
    {
        std::sort(
            rows.begin(),
            rows.end(),
            [](const LeaderboardEntryStub& a, const LeaderboardEntryStub& b)
            {
                if (a.m_nScore != b.m_nScore)
                    return a.m_nScore > b.m_nScore;

                return a.m_steamIDUser < b.m_steamIDUser;
            });

        for (size_t i = 0; i < rows.size(); ++i)
            rows[i].m_nGlobalRank = static_cast<int32_t>(i + 1);
    }

    std::vector<LeaderboardEntryStub> MakeDefaultLeaderboardRows(const std::string& name)
    {
        const std::string path = LeaderboardPath();
        const std::string section = LeaderboardSection(name);

        LeaderboardEntryStub local{};
        local.m_steamIDUser = SteamIDManager::GetLocalSteamID();
        local.m_nGlobalRank = 1;
        local.m_nScore = GetPrivateProfileIntA(section.c_str(), "local_score", 0, path.c_str());
        local.m_cDetails = 0;
        local.m_hUGC = 0;

        std::vector<LeaderboardEntryStub> rows;
        rows.push_back(local);
        RankRows(rows);
        return rows;
    }

    SteamLeaderboard_t GetLeaderboardHandle(const char* name)
    {
        const std::string cleanName = CleanLeaderboardName(name);
        std::lock_guard<std::mutex> lock(g_LeaderboardMutex);

        auto it = g_LeaderboardByName.find(cleanName);
        if (it != g_LeaderboardByName.end())
            return it->second;

        SteamLeaderboard_t handle = g_NextLeaderboardHandle++;
        if (handle == 0)
            handle = g_NextLeaderboardHandle++;

        g_LeaderboardByName[cleanName] = handle;
        g_LeaderboardNames[handle] = cleanName;
        g_LeaderboardRows[handle] = MakeDefaultLeaderboardRows(cleanName);
        return handle;
    }

    std::vector<LeaderboardEntryStub> SnapshotLeaderboardRows(SteamLeaderboard_t leaderboard)
    {
        std::lock_guard<std::mutex> lock(g_LeaderboardMutex);

        SteamLeaderboard_t handle = leaderboard ? leaderboard : kOfflineLeaderboard;
        auto nameIt = g_LeaderboardNames.find(handle);
        if (nameIt == g_LeaderboardNames.end())
        {
            const std::string fallback = "BreakersRevivedLeaderboard";
            g_LeaderboardNames[handle] = fallback;
            g_LeaderboardByName[fallback] = handle;
            g_LeaderboardRows[handle] = MakeDefaultLeaderboardRows(fallback);
        }

        auto& rows = g_LeaderboardRows[handle];
        if (rows.empty())
            rows = MakeDefaultLeaderboardRows(g_LeaderboardNames[handle]);

        RankRows(rows);
        return rows;
    }

    std::string LeaderboardNameForHandle(SteamLeaderboard_t leaderboard)
    {
        std::lock_guard<std::mutex> lock(g_LeaderboardMutex);

        SteamLeaderboard_t handle = leaderboard ? leaderboard : kOfflineLeaderboard;
        auto it = g_LeaderboardNames.find(handle);
        return it == g_LeaderboardNames.end() ? "BreakersRevivedLeaderboard" : it->second;
    }
    std::vector<LeaderboardEntryStub> RefreshLeaderboardLocal(SteamLeaderboard_t leaderboard, int limit)
    {
        NSR_UNUSED(limit);
        const SteamLeaderboard_t handle = leaderboard ? leaderboard : kOfflineLeaderboard;
        return SnapshotLeaderboardRows(handle);
    }
    void EnsureLocalLeaderboardRow(SteamLeaderboard_t leaderboard)
    {
        NSR_UNUSED(leaderboard);
    }

    std::map<std::string, std::string> BuildPlayerStatsSnapshot()
    {
        std::map<std::string, std::string> stats;

        for (const SteamStatsLocal::StatInfo& stat : SteamStatsLocal::GetStatSnapshot())
        {
            if (!stat.Name.empty())
                stats[stat.Name] = stat.Value;
        }

        stats["achievements_unlocked"] = std::to_string(SteamStatsLocal::GetUnlockedAchievementCount());
        stats["achievements_total"] = std::to_string(SteamStatsLocal::GetAchievementCount());
        return stats;
    }

    SteamLeaderboardEntries_t StoreDownloadedRows(const std::vector<LeaderboardEntryStub>& rows)
    {
        std::lock_guard<std::mutex> lock(g_LeaderboardMutex);

        SteamLeaderboardEntries_t handle = g_NextEntriesHandle++;
        if (handle == 0)
            handle = g_NextEntriesHandle++;

        g_DownloadedEntries[handle] = rows;
        return handle;
    }

    bool GetDownloadedRow(SteamLeaderboardEntries_t entries, int index, LeaderboardEntryStub& out)
    {
        std::lock_guard<std::mutex> lock(g_LeaderboardMutex);

        auto it = g_DownloadedEntries.find(entries);
        if (it == g_DownloadedEntries.end())
            return false;

        if (index < 0 || index >= static_cast<int>(it->second.size()))
            return false;

        out = it->second[index];
        return true;
    }

    void SetLocalLeaderboardScore(SteamLeaderboard_t leaderboard, int32_t score)
    {
        std::lock_guard<std::mutex> lock(g_LeaderboardMutex);

        SteamLeaderboard_t handle = leaderboard ? leaderboard : kOfflineLeaderboard;

        auto nameIt = g_LeaderboardNames.find(handle);
        if (nameIt == g_LeaderboardNames.end())
        {
            const std::string fallback = "BreakersRevivedLeaderboard";
            g_LeaderboardNames[handle] = fallback;
            g_LeaderboardByName[fallback] = handle;
            g_LeaderboardRows[handle] = MakeDefaultLeaderboardRows(fallback);
            nameIt = g_LeaderboardNames.find(handle);
        }

        auto& rows = g_LeaderboardRows[handle];
        bool updated = false;
        const CSteamID localID = SteamIDManager::GetLocalSteamID();

        for (LeaderboardEntryStub& row : rows)
        {
            if (row.m_steamIDUser == localID)
            {
                row.m_nScore = score;
                updated = true;
                break;
            }
        }

        if (!updated)
        {
            LeaderboardEntryStub row{};
            row.m_steamIDUser = localID;
            row.m_nScore = score;
            rows.push_back(row);
        }

        RankRows(rows);

        const std::string path = LeaderboardPath();
        const std::string section = LeaderboardSection(nameIt->second);
        WritePrivateProfileStringA(section.c_str(), "local_score", std::to_string(score).c_str(), path.c_str());
    }

    bool SafeCopy(void* out, size_t outSize, const void* data, size_t dataSize, const char* fieldName)
    {
        if (!out || outSize == 0)
            return false;

        if (!IsWritableMemory(out, outSize))
        {
            Logger::Error(std::string("SteamUserStats skipped invalid buffer write: ") + fieldName);
            return false;
        }

        std::memset(out, 0, outSize);
        if (data && dataSize > 0)
            std::memcpy(out, data, std::min(outSize, dataSize));

        return true;
    }
}

class FakeSteamUserStats final
{
public:
    virtual bool RequestCurrentStats()
    {
        TraceUserStats("RequestCurrentStats");
        Logger::Info("SteamUserStats::RequestCurrentStats");
        SteamDiagnostics::MarkSteam("RequestCurrentStats");
        UserStatsReceivedResult result = MakeUserStatsReceived(SteamIDManager::GetLocalSteamID());
        SteamCallbackManager::PushCallback(1101, &result, sizeof(result));
        return true;
    }

    virtual bool GetStat(const char* name, int32_t* data)
    {
        TraceUserStats("GetStatInt", name ? name : "null");
        Logger::Info(std::string("SteamUserStats::GetStat int ") + (name ? name : "null"));
        int32_t value = 0;
        const bool ok = SteamStatsLocal::GetInt(name, &value);
        SafeWrite(data, value, "GetStat int");
        return ok;
    }

    virtual bool GetStat(const char* name, float* data)
    {
        TraceUserStats("GetStatFloat", name ? name : "null");
        Logger::Info(std::string("SteamUserStats::GetStat float ") + (name ? name : "null"));
        float value = 0.0f;
        const bool ok = SteamStatsLocal::GetFloat(name, &value);
        SafeWrite(data, value, "GetStat float");
        return ok;
    }

    virtual bool SetStat(const char* name, int32_t data)
    {
        TraceUserStats("SetStatInt", std::string(name ? name : "null") + "=" + std::to_string(data));
        Logger::Info(std::string("SteamUserStats::SetStat int ") + (name ? name : "null"));
        return SteamStatsLocal::SetInt(name, data);
    }

    virtual bool SetStat(const char* name, float data)
    {
        TraceUserStats("SetStatFloat", std::string(name ? name : "null") + "=" + std::to_string(data));
        Logger::Info(std::string("SteamUserStats::SetStat float ") + (name ? name : "null"));
        return SteamStatsLocal::SetFloat(name, data);
    }

    virtual bool UpdateAvgRateStat(const char* name, float count, double sessionLength)
    {
        TraceUserStats("UpdateAvgRateStat", name ? name : "null");
        NSR_UNUSED(name);
        NSR_UNUSED(count);
        NSR_UNUSED(sessionLength);
        Logger::Info("SteamUserStats::UpdateAvgRateStat");
        return true;
    }

    virtual bool GetAchievement(const char* name, bool* achieved)
    {
        TraceUserStats("GetAchievement", name ? name : "null");
        Logger::Info(std::string("SteamUserStats::GetAchievement ") + (name ? name : "null"));
        bool value = false;
        const bool ok = SteamStatsLocal::GetAchievement(name, &value);
        SafeWrite(achieved, value, "GetAchievement");
        return ok;
    }

    virtual bool SetAchievement(const char* name)
    {
        TraceUserStats("SetAchievement", name ? name : "null");
        Logger::Info(std::string("SteamUserStats::SetAchievement ") + (name ? name : "null"));
        SteamDiagnostics::MarkSteam("SetAchievement", name ? name : "null");
        return SteamStatsLocal::SetAchievement(name);
    }

    virtual bool ClearAchievement(const char* name)
    {
        TraceUserStats("ClearAchievement", name ? name : "null");
        Logger::Info(std::string("SteamUserStats::ClearAchievement ") + (name ? name : "null"));
        SteamDiagnostics::MarkSteam("ClearAchievement", name ? name : "null");
        return SteamStatsLocal::ClearAchievement(name);
    }

    virtual bool GetAchievementAndUnlockTime(const char* name, bool* achieved, uint32_t* unlockTime)
    {
        TraceUserStats("GetAchievementAndUnlockTime", name ? name : "null");
        Logger::Info(std::string("SteamUserStats::GetAchievementAndUnlockTime ") + (name ? name : "null"));

        SafeWrite(unlockTime, 0u, "GetAchievementAndUnlockTime time");

        bool value = false;
        const bool ok = SteamStatsLocal::GetAchievement(name, &value);
        SafeWrite(achieved, value, "GetAchievementAndUnlockTime achieved");
        return ok;
    }

    virtual bool StoreStats()
    {
        TraceUserStats("StoreStats");
        Logger::Info("SteamUserStats::StoreStats");
        SteamDiagnostics::MarkSteam("StoreStats");
        SteamStatsLocal::Save();

        UserStatsStoredResult result{};
        result.m_nGameID = kOfflineGameID;
        result.m_eResult = kResultOK;
        SteamCallbackManager::PushCallback(1102, &result, sizeof(result));

        return true;
    }

    virtual int GetAchievementIcon(const char* name)
    {
        TraceUserStats("GetAchievementIcon", name ? name : "null");
        NSR_UNUSED(name);
        Logger::Info("SteamUserStats::GetAchievementIcon");
        return 0;
    }

    virtual const char* GetAchievementDisplayAttribute(const char* name, const char* key)
    {
        TraceUserStats(
            "GetAchievementDisplayAttribute",
            std::string(name ? name : "null") + " key=" + (key ? key : "null"));
        Logger::Info(std::string("SteamUserStats::GetAchievementDisplayAttribute ") + (key ? key : "null"));

        if (key && strcmp(key, "hidden") == 0)
            return "0";

        if (key && strcmp(key, "name") == 0)
        {
            g_DisplayAttributeValue = name && name[0] ? name : "Achievement";
            return g_DisplayAttributeValue.c_str();
        }

        if (key && strcmp(key, "desc") == 0)
        {
            g_DisplayAttributeValue = std::string("Offline achievement slot for ") + (name && name[0] ? name : "achievement");
            return g_DisplayAttributeValue.c_str();
        }

        return "";
    }

    virtual bool IndicateAchievementProgress(const char* name, uint32_t current, uint32_t max)
    {
        TraceUserStats("IndicateAchievementProgress", std::string(name ? name : "null") + " " + std::to_string(current) + "/" + std::to_string(max));
        Logger::Info("SteamUserStats::IndicateAchievementProgress");

        UserAchievementStoredResult result{};
        result.m_nGameID = kOfflineGameID;
        result.m_bGroupAchievement = false;
        result.m_nCurProgress = current;
        result.m_nMaxProgress = max;

        if (name)
            strncpy_s(result.m_rgchAchievementName, name, _TRUNCATE);

        SteamCallbackManager::PushCallback(1103, &result, sizeof(result));
        return true;
    }

    virtual uint32_t GetNumAchievements()
    {
        TraceUserStats("GetNumAchievements");
        Logger::Info("SteamUserStats::GetNumAchievements");
        return SteamStatsLocal::GetAchievementCount();
    }

    virtual const char* GetAchievementName(uint32_t achievement)
    {
        TraceUserStats("GetAchievementName", std::to_string(achievement));
        Logger::Info("SteamUserStats::GetAchievementName");
        return SteamStatsLocal::GetAchievementName(achievement);
    }

    virtual SteamAPICall_t RequestUserStats(CSteamID user)
    {
        TraceUserStats("RequestUserStats", std::to_string(static_cast<unsigned long long>(user)));
        Logger::Info("SteamUserStats::RequestUserStats");

        UserStatsReceivedResult result = MakeUserStatsReceived(user);
        return SteamCallResultManager::CreateCallResult(1101, &result, sizeof(result), true);
    }

    virtual bool GetUserStat(CSteamID user, const char* name, int32_t* data)
    {
        TraceUserStats("GetUserStatInt", name ? name : "null");
        NSR_UNUSED(user);
        Logger::Info("SteamUserStats::GetUserStat int");
        SafeWrite(data, 0, "GetUserStat int");
        return true;
    }

    virtual bool GetUserStat(CSteamID user, const char* name, float* data)
    {
        TraceUserStats("GetUserStatFloat", name ? name : "null");
        NSR_UNUSED(user);
        NSR_UNUSED(name);
        Logger::Info("SteamUserStats::GetUserStat float");
        SafeWrite(data, 0.0f, "GetUserStat float");
        return true;
    }

    virtual bool GetUserAchievement(CSteamID user, const char* name, bool* achieved)
    {
        TraceUserStats("GetUserAchievement", name ? name : "null");
        NSR_UNUSED(user);
        NSR_UNUSED(name);
        Logger::Info("SteamUserStats::GetUserAchievement");
        SafeWrite(achieved, false, "GetUserAchievement");
        return true;
    }

    virtual bool GetUserAchievementAndUnlockTime(CSteamID user, const char* name, bool* achieved, uint32_t* unlockTime)
    {
        TraceUserStats("GetUserAchievementAndUnlockTime", name ? name : "null");
        NSR_UNUSED(user);
        NSR_UNUSED(name);
        Logger::Info("SteamUserStats::GetUserAchievementAndUnlockTime");
        SafeWrite(achieved, false, "GetUserAchievementAndUnlockTime achieved");
        SafeWrite(unlockTime, 0u, "GetUserAchievementAndUnlockTime time");
        return true;
    }

    virtual bool ResetAllStats(bool achievementsToo)
    {
        TraceUserStats("ResetAllStats", achievementsToo ? "stats+achievements" : "stats only");
        Logger::Info("SteamUserStats::ResetAllStats");
        SteamDiagnostics::MarkSteam(
            "ResetAllStats",
            achievementsToo ? "stats+achievements" : "stats only");
        return SteamStatsLocal::ResetAll(achievementsToo);
    }

    virtual SteamAPICall_t FindOrCreateLeaderboard(const char* name, int sortMethod, int displayType)
    {
        TraceUserStats("FindOrCreateLeaderboard", name ? name : "null");
        NSR_UNUSED(sortMethod);
        NSR_UNUSED(displayType);
        Logger::Info(std::string("SteamUserStats::FindOrCreateLeaderboard ") + (name ? name : "null"));

        LeaderboardFindResult result{};
        result.m_hSteamLeaderboard = GetLeaderboardHandle(name);
        result.m_bLeaderboardFound = 1;
        EnsureLocalLeaderboardRow(result.m_hSteamLeaderboard);
        RefreshLeaderboardLocal(result.m_hSteamLeaderboard, 100);
        return SteamCallResultManager::CreateCallResult(1104, &result, sizeof(result), true);
    }

    virtual SteamAPICall_t FindLeaderboard(const char* name)
    {
        TraceUserStats("FindLeaderboard", name ? name : "null");
        Logger::Info(std::string("SteamUserStats::FindLeaderboard ") + (name ? name : "null"));

        LeaderboardFindResult result{};
        result.m_hSteamLeaderboard = GetLeaderboardHandle(name);
        result.m_bLeaderboardFound = 1;
        EnsureLocalLeaderboardRow(result.m_hSteamLeaderboard);
        RefreshLeaderboardLocal(result.m_hSteamLeaderboard, 100);
        return SteamCallResultManager::CreateCallResult(1104, &result, sizeof(result), true);
    }

    virtual const char* GetLeaderboardName(SteamLeaderboard_t leaderboard)
    {
        TraceUserStats("GetLeaderboardName", std::to_string(leaderboard));
        Logger::Info("SteamUserStats::GetLeaderboardName");

        static thread_local std::string s_Name;
        {
            std::lock_guard<std::mutex> lock(g_LeaderboardMutex);
            auto it = g_LeaderboardNames.find(leaderboard ? leaderboard : kOfflineLeaderboard);
            s_Name = it == g_LeaderboardNames.end() ? "BreakersRevivedLeaderboard" : it->second;
        }

        return s_Name.c_str();
    }

    virtual int GetLeaderboardEntryCount(SteamLeaderboard_t leaderboard)
    {
        TraceUserStats("GetLeaderboardEntryCount", std::to_string(leaderboard));
        Logger::Info("SteamUserStats::GetLeaderboardEntryCount");
        return static_cast<int>(RefreshLeaderboardLocal(leaderboard, 100).size());
    }

    virtual int GetLeaderboardSortMethod(SteamLeaderboard_t leaderboard)
    {
        TraceUserStats("GetLeaderboardSortMethod", std::to_string(leaderboard));
        NSR_UNUSED(leaderboard);
        Logger::Info("SteamUserStats::GetLeaderboardSortMethod");
        return 0;
    }

    virtual int GetLeaderboardDisplayType(SteamLeaderboard_t leaderboard)
    {
        TraceUserStats("GetLeaderboardDisplayType", std::to_string(leaderboard));
        NSR_UNUSED(leaderboard);
        Logger::Info("SteamUserStats::GetLeaderboardDisplayType");
        return 0;
    }

    virtual SteamAPICall_t DownloadLeaderboardEntries(SteamLeaderboard_t leaderboard, int request, int start, int end)
    {
        TraceUserStats("DownloadLeaderboardEntries", "start=" + std::to_string(start) + " end=" + std::to_string(end));
        NSR_UNUSED(request);
        Logger::Info("SteamUserStats::DownloadLeaderboardEntries");

        std::vector<LeaderboardEntryStub> rows = RefreshLeaderboardLocal(leaderboard, std::max(10, end > 0 ? end : 50));

        if (start > 0 || end > 0)
        {
            const int first = std::max(0, start <= 0 ? 0 : start - 1);
            const int last = std::min(static_cast<int>(rows.size()), end <= 0 ? static_cast<int>(rows.size()) : end);

            if (first < last && first < static_cast<int>(rows.size()))
                rows = std::vector<LeaderboardEntryStub>(rows.begin() + first, rows.begin() + last);
            else
                rows.clear();
        }

        LeaderboardScoresDownloadedResult result{};
        result.m_hSteamLeaderboard = leaderboard ? leaderboard : kOfflineLeaderboard;
        result.m_hSteamLeaderboardEntries = StoreDownloadedRows(rows);
        result.m_cEntryCount = static_cast<int>(rows.size());
        return SteamCallResultManager::CreateCallResult(1105, &result, sizeof(result), true);
    }

    virtual SteamAPICall_t DownloadLeaderboardEntriesForUsers(SteamLeaderboard_t leaderboard, CSteamID* users, int count)
    {
        TraceUserStats("DownloadLeaderboardEntriesForUsers", "count=" + std::to_string(count));
        Logger::Info("SteamUserStats::DownloadLeaderboardEntriesForUsers");

        std::vector<LeaderboardEntryStub> sourceRows = RefreshLeaderboardLocal(leaderboard, std::max(10, count > 0 ? count : 50));
        std::vector<LeaderboardEntryStub> rows;

        if (users && count > 0)
        {
            for (int i = 0; i < count; ++i)
            {
                LeaderboardEntryStub row{};
                row.m_steamIDUser = users[i];
                row.m_nScore = 0;
                row.m_nGlobalRank = i + 1;

                for (const LeaderboardEntryStub& existing : sourceRows)
                {
                    if (existing.m_steamIDUser == users[i])
                    {
                        row = existing;
                        break;
                    }
                }

                rows.push_back(row);
            }
        }
        else
        {
            rows = sourceRows;
        }

        RankRows(rows);

        LeaderboardScoresDownloadedResult result{};
        result.m_hSteamLeaderboard = leaderboard ? leaderboard : kOfflineLeaderboard;
        result.m_hSteamLeaderboardEntries = StoreDownloadedRows(rows);
        result.m_cEntryCount = static_cast<int>(rows.size());
        return SteamCallResultManager::CreateCallResult(1105, &result, sizeof(result), true);
    }

    virtual bool GetDownloadedLeaderboardEntry(SteamLeaderboardEntries_t entries, int index, void* entry, int32_t* details, int detailsMax)
    {
        TraceUserStats("GetDownloadedLeaderboardEntry", "index=" + std::to_string(index));
        Logger::Info("SteamUserStats::GetDownloadedLeaderboardEntry");

        LeaderboardEntryStub stub{};
        if (!GetDownloadedRow(entries, index, stub))
            return false;

        if (entry)
            SafeCopy(entry, sizeof(stub), &stub, sizeof(stub), "GetDownloadedLeaderboardEntry");

        if (details && detailsMax > 0)
            SafeCopy(details, sizeof(int32_t) * static_cast<size_t>(detailsMax), nullptr, 0, "GetDownloadedLeaderboardEntry details");

        return true;
    }

    virtual SteamAPICall_t UploadLeaderboardScore(SteamLeaderboard_t leaderboard, int method, int32_t score, const int32_t* details, int detailsCount)
    {
        TraceUserStats("UploadLeaderboardScore", "score=" + std::to_string(score));
        NSR_UNUSED(method);
        NSR_UNUSED(details);
        NSR_UNUSED(detailsCount);
        Logger::Info("SteamUserStats::UploadLeaderboardScore");

        SetLocalLeaderboardScore(leaderboard, score);

        LeaderboardScoreUploadedResult result{};
        result.m_bSuccess = 1;
        result.m_hSteamLeaderboard = leaderboard ? leaderboard : kOfflineLeaderboard;
        result.m_nScore = score;
        result.m_bScoreChanged = 1;
        result.m_nGlobalRankNew = 1;
        result.m_nGlobalRankPrevious = 0;
        return SteamCallResultManager::CreateCallResult(1106, &result, sizeof(result), true);
    }

    virtual SteamAPICall_t AttachLeaderboardUGC(SteamLeaderboard_t leaderboard, UGCHandle_t ugc)
    {
        TraceUserStats("AttachLeaderboardUGC", "ugc=" + std::to_string(ugc));
        NSR_UNUSED(ugc);
        Logger::Info("SteamUserStats::AttachLeaderboardUGC");

        LeaderboardUGCSetResult result{};
        result.m_eResult = kResultOK;
        result.m_hSteamLeaderboard = leaderboard ? leaderboard : kOfflineLeaderboard;
        return SteamCallResultManager::CreateCallResult(1111, &result, sizeof(result), true);
    }

    virtual SteamAPICall_t GetNumberOfCurrentPlayers()
    {
        TraceUserStats("GetNumberOfCurrentPlayers");
        Logger::Info("SteamUserStats::GetNumberOfCurrentPlayers");

        NumberOfCurrentPlayersResult result{};
        result.m_bSuccess = 1;
        result.m_cPlayers = 1;
        return SteamCallResultManager::CreateCallResult(1107, &result, sizeof(result), true);
    }

    virtual SteamAPICall_t RequestGlobalAchievementPercentages()
    {
        TraceUserStats("RequestGlobalAchievementPercentages");
        Logger::Info("SteamUserStats::RequestGlobalAchievementPercentages");

        GlobalAchievementPercentagesReadyResult result{};
        result.m_nGameID = kOfflineGameID;
        result.m_eResult = kResultOK;
        return SteamCallResultManager::CreateCallResult(1110, &result, sizeof(result), true);
    }

    virtual int GetMostAchievedAchievementInfo(char* name, uint32_t nameLen, float* percent, bool* achieved)
    {
        TraceUserStats("GetMostAchievedAchievementInfo");
        Logger::Info("SteamUserStats::GetMostAchievedAchievementInfo");

        if (name && nameLen > 0)
            SafeWrite(name, '\0', "GetMostAchievedAchievementInfo name");

        SafeWrite(percent, 0.0f, "GetMostAchievedAchievementInfo percent");
        SafeWrite(achieved, false, "GetMostAchievedAchievementInfo achieved");

        return -1;
    }

    virtual int GetNextMostAchievedAchievementInfo(int previous, char* name, uint32_t nameLen, float* percent, bool* achieved)
    {
        TraceUserStats("GetNextMostAchievedAchievementInfo", "previous=" + std::to_string(previous));
        NSR_UNUSED(previous);
        Logger::Info("SteamUserStats::GetNextMostAchievedAchievementInfo");

        if (name && nameLen > 0)
            SafeWrite(name, '\0', "GetNextMostAchievedAchievementInfo name");

        SafeWrite(percent, 0.0f, "GetNextMostAchievedAchievementInfo percent");
        SafeWrite(achieved, false, "GetNextMostAchievedAchievementInfo achieved");

        return -1;
    }

    virtual bool GetAchievementAchievedPercent(const char* name, float* percent)
    {
        TraceUserStats("GetAchievementAchievedPercent", name ? name : "null");
        NSR_UNUSED(name);
        Logger::Info("SteamUserStats::GetAchievementAchievedPercent");

        SafeWrite(percent, 0.0f, "GetAchievementAchievedPercent");

        return false;
    }

    virtual SteamAPICall_t RequestGlobalStats(int historyDays)
    {
        TraceUserStats("RequestGlobalStats", "days=" + std::to_string(historyDays));
        NSR_UNUSED(historyDays);
        Logger::Info("SteamUserStats::RequestGlobalStats");

        GlobalStatsReceivedResult result{};
        result.m_nGameID = kOfflineGameID;
        result.m_eResult = kResultOK;
        return SteamCallResultManager::CreateCallResult(1112, &result, sizeof(result), true);
    }

    virtual bool GetGlobalStat(const char* name, int64_t* data)
    {
        TraceUserStats("GetGlobalStatInt64", name ? name : "null");
        NSR_UNUSED(name);
        Logger::Info("SteamUserStats::GetGlobalStat int64");

        SafeWrite(data, static_cast<int64_t>(0), "GetGlobalStat int64");

        return true;
    }

    virtual bool GetGlobalStat(const char* name, double* data)
    {
        TraceUserStats("GetGlobalStatDouble", name ? name : "null");
        NSR_UNUSED(name);
        Logger::Info("SteamUserStats::GetGlobalStat double");

        SafeWrite(data, 0.0, "GetGlobalStat double");

        return true;
    }

    virtual int32_t GetGlobalStatHistory(const char* name, int64_t* data, uint32_t size)
    {
        TraceUserStats("GetGlobalStatHistoryInt64", std::string(name ? name : "null") + " size=" + std::to_string(size));
        NSR_UNUSED(name);
        NSR_UNUSED(size);
        Logger::Info("SteamUserStats::GetGlobalStatHistory int64");

        if (size > 0)
            SafeWrite(data, static_cast<int64_t>(0), "GetGlobalStatHistory int64");

        return 0;
    }

    virtual int32_t GetGlobalStatHistory(const char* name, double* data, uint32_t size)
    {
        TraceUserStats("GetGlobalStatHistoryDouble", std::string(name ? name : "null") + " size=" + std::to_string(size));
        NSR_UNUSED(name);
        NSR_UNUSED(size);
        Logger::Info("SteamUserStats::GetGlobalStatHistory double");

        if (size > 0)
            SafeWrite(data, 0.0, "GetGlobalStatHistory double");

        return 0;
    }

    virtual bool GetAchievementProgressLimits(const char* name, int32_t* minProgress, int32_t* maxProgress)
    {
        TraceUserStats("GetAchievementProgressLimitsInt32", name ? name : "null");
        NSR_UNUSED(name);
        Logger::Info("SteamUserStats::GetAchievementProgressLimits int32");

        SafeWrite(minProgress, 0, "GetAchievementProgressLimits int32 min");
        SafeWrite(maxProgress, 100, "GetAchievementProgressLimits int32 max");

        return true;
    }

    virtual bool GetAchievementProgressLimits(const char* name, float* minProgress, float* maxProgress)
    {
        TraceUserStats("GetAchievementProgressLimitsFloat", name ? name : "null");
        NSR_UNUSED(name);
        Logger::Info("SteamUserStats::GetAchievementProgressLimits float");

        SafeWrite(minProgress, 0.0f, "GetAchievementProgressLimits float min");
        SafeWrite(maxProgress, 100.0f, "GetAchievementProgressLimits float max");

        return true;
    }
};

static FakeSteamUserStats g_Interface;

class FakeSteamUserStats010 final
{
public:
    virtual bool RequestCurrentStats() { return g_Interface.RequestCurrentStats(); }
    virtual bool GetStat(const char* name, int32_t* data) { return g_Interface.GetStat(name, data); }
    virtual bool GetStat(const char* name, float* data) { return g_Interface.GetStat(name, data); }
    virtual bool SetStat(const char* name, int32_t data) { return g_Interface.SetStat(name, data); }
    virtual bool SetStat(const char* name, float data) { return g_Interface.SetStat(name, data); }
    virtual bool UpdateAvgRateStat(const char* name, float count, double sessionLength) { return g_Interface.UpdateAvgRateStat(name, count, sessionLength); }
    virtual bool GetAchievement(const char* name, bool* achieved) { return g_Interface.GetAchievement(name, achieved); }
    virtual bool SetAchievement(const char* name) { return g_Interface.SetAchievement(name); }
    virtual bool ClearAchievement(const char* name) { return g_Interface.ClearAchievement(name); }
    virtual bool GetAchievementAndUnlockTime(const char* name, bool* achieved, uint32_t* unlockTime) { return g_Interface.GetAchievementAndUnlockTime(name, achieved, unlockTime); }
    virtual bool StoreStats() { return g_Interface.StoreStats(); }
    virtual int GetAchievementIcon(const char* name) { return g_Interface.GetAchievementIcon(name); }
    virtual const char* GetAchievementDisplayAttribute(const char* name, const char* key) { return g_Interface.GetAchievementDisplayAttribute(name, key); }
    virtual bool IndicateAchievementProgress(const char* name, uint32_t current, uint32_t max) { return g_Interface.IndicateAchievementProgress(name, current, max); }
    virtual SteamAPICall_t RequestUserStats(CSteamID user) { return g_Interface.RequestUserStats(user); }
    virtual bool GetUserStat(CSteamID user, const char* name, int32_t* data) { return g_Interface.GetUserStat(user, name, data); }
    virtual bool GetUserStat(CSteamID user, const char* name, float* data) { return g_Interface.GetUserStat(user, name, data); }
    virtual bool GetUserAchievement(CSteamID user, const char* name, bool* achieved) { return g_Interface.GetUserAchievement(user, name, achieved); }
    virtual bool GetUserAchievementAndUnlockTime(CSteamID user, const char* name, bool* achieved, uint32_t* unlockTime) { return g_Interface.GetUserAchievementAndUnlockTime(user, name, achieved, unlockTime); }
    virtual bool ResetAllStats(bool achievementsToo) { return g_Interface.ResetAllStats(achievementsToo); }
    virtual SteamAPICall_t FindOrCreateLeaderboard(const char* name, int sortMethod, int displayType) { return g_Interface.FindOrCreateLeaderboard(name, sortMethod, displayType); }
    virtual SteamAPICall_t FindLeaderboard(const char* name) { return g_Interface.FindLeaderboard(name); }
    virtual const char* GetLeaderboardName(SteamLeaderboard_t leaderboard) { return g_Interface.GetLeaderboardName(leaderboard); }
    virtual int GetLeaderboardEntryCount(SteamLeaderboard_t leaderboard) { return g_Interface.GetLeaderboardEntryCount(leaderboard); }
    virtual int GetLeaderboardSortMethod(SteamLeaderboard_t leaderboard) { return g_Interface.GetLeaderboardSortMethod(leaderboard); }
    virtual int GetLeaderboardDisplayType(SteamLeaderboard_t leaderboard) { return g_Interface.GetLeaderboardDisplayType(leaderboard); }
    virtual SteamAPICall_t DownloadLeaderboardEntries(SteamLeaderboard_t leaderboard, int request, int start, int end) { return g_Interface.DownloadLeaderboardEntries(leaderboard, request, start, end); }
    virtual SteamAPICall_t DownloadLeaderboardEntriesForUsers(SteamLeaderboard_t leaderboard, CSteamID* users, int count) { return g_Interface.DownloadLeaderboardEntriesForUsers(leaderboard, users, count); }
    virtual bool GetDownloadedLeaderboardEntry(SteamLeaderboardEntries_t entries, int index, void* entry, int32_t* details, int detailsMax) { return g_Interface.GetDownloadedLeaderboardEntry(entries, index, entry, details, detailsMax); }
    virtual SteamAPICall_t UploadLeaderboardScore(SteamLeaderboard_t leaderboard, int method, int32_t score, const int32_t* details, int detailsCount) { return g_Interface.UploadLeaderboardScore(leaderboard, method, score, details, detailsCount); }
    virtual SteamAPICall_t AttachLeaderboardUGC(SteamLeaderboard_t leaderboard, UGCHandle_t ugc) { return g_Interface.AttachLeaderboardUGC(leaderboard, ugc); }
    virtual SteamAPICall_t GetNumberOfCurrentPlayers() { return g_Interface.GetNumberOfCurrentPlayers(); }
    virtual SteamAPICall_t RequestGlobalAchievementPercentages() { return g_Interface.RequestGlobalAchievementPercentages(); }
    virtual int GetMostAchievedAchievementInfo(char* name, uint32_t nameLen, float* percent, bool* achieved) { return g_Interface.GetMostAchievedAchievementInfo(name, nameLen, percent, achieved); }
    virtual int GetNextMostAchievedAchievementInfo(int previous, char* name, uint32_t nameLen, float* percent, bool* achieved) { return g_Interface.GetNextMostAchievedAchievementInfo(previous, name, nameLen, percent, achieved); }
    virtual bool GetAchievementAchievedPercent(const char* name, float* percent) { return g_Interface.GetAchievementAchievedPercent(name, percent); }
    virtual SteamAPICall_t RequestGlobalStats(int historyDays) { return g_Interface.RequestGlobalStats(historyDays); }
    virtual bool GetGlobalStat(const char* name, int64_t* data) { return g_Interface.GetGlobalStat(name, data); }
    virtual bool GetGlobalStat(const char* name, double* data) { return g_Interface.GetGlobalStat(name, data); }
    virtual int32_t GetGlobalStatHistory(const char* name, int64_t* data, uint32_t size) { return g_Interface.GetGlobalStatHistory(name, data, size); }
    virtual int32_t GetGlobalStatHistory(const char* name, double* data, uint32_t size) { return g_Interface.GetGlobalStatHistory(name, data, size); }
};

class FakeSteamUserStats011 final
{
public:
    virtual bool RequestCurrentStats() { return g_Interface.RequestCurrentStats(); }
    virtual bool GetStat(const char* name, int32_t* data) { return g_Interface.GetStat(name, data); }
    virtual bool GetStat(const char* name, float* data) { return g_Interface.GetStat(name, data); }
    virtual bool SetStat(const char* name, int32_t data) { return g_Interface.SetStat(name, data); }
    virtual bool SetStat(const char* name, float data) { return g_Interface.SetStat(name, data); }
    virtual bool UpdateAvgRateStat(const char* name, float count, double sessionLength) { return g_Interface.UpdateAvgRateStat(name, count, sessionLength); }
    virtual bool GetAchievement(const char* name, bool* achieved) { return g_Interface.GetAchievement(name, achieved); }
    virtual bool SetAchievement(const char* name) { return g_Interface.SetAchievement(name); }
    virtual bool ClearAchievement(const char* name) { return g_Interface.ClearAchievement(name); }
    virtual bool GetAchievementAndUnlockTime(const char* name, bool* achieved, uint32_t* unlockTime) { return g_Interface.GetAchievementAndUnlockTime(name, achieved, unlockTime); }
    virtual bool StoreStats() { return g_Interface.StoreStats(); }
    virtual int GetAchievementIcon(const char* name) { return g_Interface.GetAchievementIcon(name); }
    virtual const char* GetAchievementDisplayAttribute(const char* name, const char* key) { return g_Interface.GetAchievementDisplayAttribute(name, key); }
    virtual bool IndicateAchievementProgress(const char* name, uint32_t current, uint32_t max) { return g_Interface.IndicateAchievementProgress(name, current, max); }
    virtual uint32_t GetNumAchievements() { return g_Interface.GetNumAchievements(); }
    virtual const char* GetAchievementName(uint32_t achievement) { return g_Interface.GetAchievementName(achievement); }
    virtual SteamAPICall_t RequestUserStats(CSteamID user) { return g_Interface.RequestUserStats(user); }
    virtual bool GetUserStat(CSteamID user, const char* name, int32_t* data) { return g_Interface.GetUserStat(user, name, data); }
    virtual bool GetUserStat(CSteamID user, const char* name, float* data) { return g_Interface.GetUserStat(user, name, data); }
    virtual bool GetUserAchievement(CSteamID user, const char* name, bool* achieved) { return g_Interface.GetUserAchievement(user, name, achieved); }
    virtual bool GetUserAchievementAndUnlockTime(CSteamID user, const char* name, bool* achieved, uint32_t* unlockTime) { return g_Interface.GetUserAchievementAndUnlockTime(user, name, achieved, unlockTime); }
    virtual bool ResetAllStats(bool achievementsToo) { return g_Interface.ResetAllStats(achievementsToo); }
    virtual SteamAPICall_t FindOrCreateLeaderboard(const char* name, int sortMethod, int displayType) { return g_Interface.FindOrCreateLeaderboard(name, sortMethod, displayType); }
    virtual SteamAPICall_t FindLeaderboard(const char* name) { return g_Interface.FindLeaderboard(name); }
    virtual const char* GetLeaderboardName(SteamLeaderboard_t leaderboard) { return g_Interface.GetLeaderboardName(leaderboard); }
    virtual int GetLeaderboardEntryCount(SteamLeaderboard_t leaderboard) { return g_Interface.GetLeaderboardEntryCount(leaderboard); }
    virtual int GetLeaderboardSortMethod(SteamLeaderboard_t leaderboard) { return g_Interface.GetLeaderboardSortMethod(leaderboard); }
    virtual int GetLeaderboardDisplayType(SteamLeaderboard_t leaderboard) { return g_Interface.GetLeaderboardDisplayType(leaderboard); }
    virtual SteamAPICall_t DownloadLeaderboardEntries(SteamLeaderboard_t leaderboard, int request, int start, int end) { return g_Interface.DownloadLeaderboardEntries(leaderboard, request, start, end); }
    virtual SteamAPICall_t DownloadLeaderboardEntriesForUsers(SteamLeaderboard_t leaderboard, CSteamID* users, int count) { return g_Interface.DownloadLeaderboardEntriesForUsers(leaderboard, users, count); }
    virtual bool GetDownloadedLeaderboardEntry(SteamLeaderboardEntries_t entries, int index, void* entry, int32_t* details, int detailsMax) { return g_Interface.GetDownloadedLeaderboardEntry(entries, index, entry, details, detailsMax); }
    virtual SteamAPICall_t UploadLeaderboardScore(SteamLeaderboard_t leaderboard, int method, int32_t score, const int32_t* details, int detailsCount) { return g_Interface.UploadLeaderboardScore(leaderboard, method, score, details, detailsCount); }
    virtual SteamAPICall_t AttachLeaderboardUGC(SteamLeaderboard_t leaderboard, UGCHandle_t ugc) { return g_Interface.AttachLeaderboardUGC(leaderboard, ugc); }
    virtual SteamAPICall_t GetNumberOfCurrentPlayers() { return g_Interface.GetNumberOfCurrentPlayers(); }
    virtual SteamAPICall_t RequestGlobalAchievementPercentages() { return g_Interface.RequestGlobalAchievementPercentages(); }
    virtual int GetMostAchievedAchievementInfo(char* name, uint32_t nameLen, float* percent, bool* achieved) { return g_Interface.GetMostAchievedAchievementInfo(name, nameLen, percent, achieved); }
    virtual int GetNextMostAchievedAchievementInfo(int previous, char* name, uint32_t nameLen, float* percent, bool* achieved) { return g_Interface.GetNextMostAchievedAchievementInfo(previous, name, nameLen, percent, achieved); }
    virtual bool GetAchievementAchievedPercent(const char* name, float* percent) { return g_Interface.GetAchievementAchievedPercent(name, percent); }
    virtual SteamAPICall_t RequestGlobalStats(int historyDays) { return g_Interface.RequestGlobalStats(historyDays); }
    virtual bool GetGlobalStat(const char* name, int64_t* data) { return g_Interface.GetGlobalStat(name, data); }
    virtual bool GetGlobalStat(const char* name, double* data) { return g_Interface.GetGlobalStat(name, data); }
    virtual int32_t GetGlobalStatHistory(const char* name, int64_t* data, uint32_t size) { return g_Interface.GetGlobalStatHistory(name, data, size); }
    virtual int32_t GetGlobalStatHistory(const char* name, double* data, uint32_t size) { return g_Interface.GetGlobalStatHistory(name, data, size); }
    virtual bool InstallPS3Trophies() { TraceUserStats("InstallPS3Trophies"); Logger::Info("SteamUserStats011::InstallPS3Trophies"); return true; }
    virtual uint64_t GetTrophySpaceRequiredBeforeInstall() { TraceUserStats("GetTrophySpaceRequiredBeforeInstall"); Logger::Info("SteamUserStats011::GetTrophySpaceRequiredBeforeInstall"); return 0; }
    virtual bool SetUserStatsData(const void* data, uint32_t dataSize)
    {
        TraceUserStats("SetUserStatsData", "bytes=" + std::to_string(dataSize));
        NSR_UNUSED(data);
        NSR_UNUSED(dataSize);
        Logger::Info("SteamUserStats011::SetUserStatsData");
        return true;
    }
    virtual bool GetUserStatsData(void* data, uint32_t dataSize, uint32_t* written)
    {
        TraceUserStats("GetUserStatsData", "bytes=" + std::to_string(dataSize));
        Logger::Info("SteamUserStats011::GetUserStatsData");
        SafeCopy(data, dataSize, nullptr, 0, "GetUserStatsData data");
        SafeWrite(written, 0u, "GetUserStatsData written");
        return true;
    }
};

static FakeSteamUserStats010 g_Interface010;
static FakeSteamUserStats011 g_Interface011;

void* FakeSteamInterfaces::UserStats()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::UserStats", "SteamUserStatsCurrent");
    Logger::Info("SteamUserStats: current offline emulated interface returned");
    return &g_Interface;
}

void* FakeSteamInterfaces::UserStatsLegacy()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::UserStatsLegacy", "SteamUserStats010");
    Logger::Info("SteamUserStats010: legacy offline emulated interface returned");
    return &g_Interface010;
}

void* FakeSteamInterfaces::UserStatsV011()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::UserStatsV011", "SteamUserStats011");
    Logger::Info("SteamUserStats011: offline emulated interface returned");
    return &g_Interface011;
}
