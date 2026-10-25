#include "StdInc.h"
#include "SteamStatsLocal.h"
#include "Logger.h"

#include <cstdlib>

static std::mutex g_Mutex;
static std::map<std::string, int32_t> g_IntStats;
static std::map<std::string, float> g_FloatStats;
static std::map<std::string, bool> g_Achievements;
static std::string g_ReturnString;
static std::string g_Path;

namespace
{
    constexpr const char* kDefaultAchievements[] =
    {
        "ACHIEVEMENT_00",
        "ACHIEVEMENT_01",
        "ACHIEVEMENT_02",
        "ACHIEVEMENT_03",
        "ACHIEVEMENT_04",
        "ACHIEVEMENT_05",
        "ACHIEVEMENT_06",
        "ACHIEVEMENT_07",
        "ACHIEVEMENT_08",
        "ACHIEVEMENT_09",
        "ACHIEVEMENT_10",
        "ACHIEVEMENT_11",
        "ACHIEVEMENT_12",
        "ACHIEVEMENT_13",
        "ACHIEVEMENT_14",
        "ACHIEVEMENT_15",
        "ACHIEVEMENT_16",
        "ACHIEVEMENT_17",
        "ACHIEVEMENT_18",
        "ACHIEVEMENT_19",
        "ACHIEVEMENT_20",
        "ACHIEVEMENT_21",
        "ACHIEVEMENT_22",
        "ACHIEVEMENT_23",
        "ACHIEVEMENT_24",
        "ACHIEVEMENT_25",
        "ACHIEVEMENT_26",
        "ACHIEVEMENT_27",
        "ACHIEVEMENT_28",
        "ACHIEVEMENT_29",
        "ACHIEVEMENT_30",
        "ACHIEVEMENT_31",
        "ACHIEVEMENT_32",
        "ACHIEVEMENT_33",
        "ACHIEVEMENT_34",
        "ACHIEVEMENT_35",
        "ACHIEVEMENT_36",
        "ACHIEVEMENT_37",
        "ACHIEVEMENT_38",
        "ACHIEVEMENT_39",
        "ACHIEVEMENT_40",
        "ACHIEVEMENT_41",
        "ACHIEVEMENT_42",
        "ACHIEVEMENT_43",
        "ACHIEVEMENT_44",
        "ACHIEVEMENT_45",
        "ACHIEVEMENT_46",
        "ACHIEVEMENT_47",
        "ACHIEVEMENT_48",
        "ACHIEVEMENT_49"
    };

    std::string GetExeFolder()
    {
        char path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path().string();
    }

    const std::string& StatsPath()
    {
        if (g_Path.empty())
        {
            g_Path = (std::filesystem::path(GetExeFolder()) / "BreakersRevived" / "steam_stats.ini").string();

            try
            {
                std::filesystem::create_directories(std::filesystem::path(g_Path).parent_path());
            }
            catch (...)
            {
            }
        }

        return g_Path;
    }

    void SeedDefaultAchievements()
    {
        for (const char* name : kDefaultAchievements)
            g_Achievements.try_emplace(name, false);
    }

    void LoadAchievementSection()
    {
        const std::string& path = StatsPath();

        for (const char* name : kDefaultAchievements)
            g_Achievements[name] = GetPrivateProfileIntA("achievements", name, 0, path.c_str()) != 0;
    }

    void SaveMapValue(const char* section, const std::string& key, const std::string& value)
    {
        WritePrivateProfileStringA(section, key.c_str(), value.c_str(), StatsPath().c_str());
    }

    void DeleteSection(const char* section)
    {
        WritePrivateProfileStringA(section, nullptr, nullptr, StatsPath().c_str());
    }

    std::string MakeAchievementDisplayName(const std::string& name)
    {
        if (name.rfind("ACHIEVEMENT_", 0) != 0)
            return name;

        std::string number = name.substr(sizeof("ACHIEVEMENT_") - 1);
        return "Achievement " + number;
    }

    std::string MakeAchievementDescription(const std::string& name)
    {
        return "Offline achievement slot for " + name;
    }

    std::string FloatToString(float value)
    {
        std::ostringstream ss;
        ss << value;
        return ss.str();
    }
}

namespace SteamStatsLocal
{
    void Init()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        SeedDefaultAchievements();
        LoadAchievementSection();
        Logger::Info("SteamStatsLocal initialized");
    }

    bool GetInt(const char* name, int32_t* value)
    {
        if (!name || !value)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        *value = g_IntStats[name];
        return true;
    }

    bool SetInt(const char* name, int32_t value)
    {
        if (!name)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        g_IntStats[name] = value;

        Logger::Info(std::string("SteamStatsLocal int stat set: ") + name);

        return true;
    }

    bool GetFloat(const char* name, float* value)
    {
        if (!name || !value)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        *value = g_FloatStats[name];
        return true;
    }

    bool SetFloat(const char* name, float value)
    {
        if (!name)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        g_FloatStats[name] = value;

        Logger::Info(std::string("SteamStatsLocal float stat set: ") + name);

        return true;
    }

    bool GetAchievement(const char* name, bool* achieved)
    {
        if (!name || !achieved)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        if (g_Achievements.find(name) == g_Achievements.end())
            g_Achievements[name] = false;

        *achieved = g_Achievements[name];
        return true;
    }

    bool SetAchievement(const char* name)
    {
        if (!name)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Achievements[name] = true;
        SaveMapValue("achievements", name, "1");

        Logger::Info(std::string("SteamStatsLocal achievement set: ") + name);

        return true;
    }

    bool ClearAchievement(const char* name)
    {
        if (!name)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Achievements[name] = false;
        SaveMapValue("achievements", name, "0");

        return true;
    }

    uint32_t GetAchievementCount()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        SeedDefaultAchievements();
        return static_cast<uint32_t>(g_Achievements.size());
    }

    uint32_t GetUnlockedAchievementCount()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        SeedDefaultAchievements();
        uint32_t count = 0;

        for (const auto& item : g_Achievements)
        {
            if (item.second)
                ++count;
        }

        return count;
    }

    const char* GetAchievementName(uint32_t index)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        SeedDefaultAchievements();

        if (index >= g_Achievements.size())
        {
            g_ReturnString.clear();
            return g_ReturnString.c_str();
        }

        auto it = g_Achievements.begin();
        std::advance(it, index);
        g_ReturnString = it->first;
        return g_ReturnString.c_str();
    }

    std::vector<AchievementInfo> GetAchievementSnapshot()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        SeedDefaultAchievements();

        std::vector<AchievementInfo> snapshot;
        snapshot.reserve(g_Achievements.size());

        for (const auto& item : g_Achievements)
        {
            AchievementInfo info{};
            info.Name = item.first;
            info.DisplayName = MakeAchievementDisplayName(item.first);
            info.Description = MakeAchievementDescription(item.first);
            info.Achieved = item.second;
            snapshot.push_back(info);
        }

        return snapshot;
    }

    void UnlockAllAchievements()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        SeedDefaultAchievements();

        for (auto& item : g_Achievements)
        {
            item.second = true;
            SaveMapValue("achievements", item.first, "1");
        }

        Logger::Info("SteamStatsLocal all achievements unlocked");
    }

    void ClearAllAchievements()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        SeedDefaultAchievements();

        for (auto& item : g_Achievements)
        {
            item.second = false;
            SaveMapValue("achievements", item.first, "0");
        }

        Logger::Info("SteamStatsLocal all achievements locked");
    }

    std::vector<StatInfo> GetStatSnapshot()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        std::vector<StatInfo> snapshot;
        snapshot.reserve(g_IntStats.size() + g_FloatStats.size());

        for (const auto& item : g_IntStats)
        {
            StatInfo info{};
            info.Name = item.first;
            info.Type = "int";
            info.Value = std::to_string(item.second);
            snapshot.push_back(info);
        }

        for (const auto& item : g_FloatStats)
        {
            StatInfo info{};
            info.Name = item.first;
            info.Type = "float";
            info.Value = FloatToString(item.second);
            snapshot.push_back(info);
        }

        return snapshot;
    }

    void ClearStats()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_IntStats.clear();
        g_FloatStats.clear();
        DeleteSection("int_stats");
        DeleteSection("float_stats");
        Logger::Info("SteamStatsLocal stats cleared");
    }

    bool ResetAll(bool achievementsToo)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_IntStats.clear();
        g_FloatStats.clear();
        DeleteSection("int_stats");
        DeleteSection("float_stats");

        if (achievementsToo)
        {
            SeedDefaultAchievements();

            for (auto& item : g_Achievements)
                item.second = false;

            for (const auto& item : g_Achievements)
                SaveMapValue("achievements", item.first, "0");
        }

        Logger::Info("SteamStatsLocal reset");
        return true;
    }

    void Save()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        for (const auto& item : g_IntStats)
            SaveMapValue("int_stats", item.first, std::to_string(item.second));

        for (const auto& item : g_FloatStats)
            SaveMapValue("float_stats", item.first, std::to_string(item.second));

        for (const auto& item : g_Achievements)
            SaveMapValue("achievements", item.first, item.second ? "1" : "0");

        Logger::Info("SteamStatsLocal saved");
    }
}
