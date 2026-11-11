#pragma once
#include "StdInc.h"

namespace SteamStatsLocal
{
    struct AchievementInfo
    {
        std::string Name;
        std::string DisplayName;
        std::string Description;
        bool Achieved = false;
    };

    struct StatInfo
    {
        std::string Name;
        std::string Type;
        std::string Value;
    };

    void Init();

    bool GetInt(const char* name, int32_t* value);
    bool SetInt(const char* name, int32_t value);

    bool GetFloat(const char* name, float* value);
    bool SetFloat(const char* name, float value);

    bool GetAchievement(const char* name, bool* achieved);
    bool SetAchievement(const char* name);
    bool ClearAchievement(const char* name);
    uint32_t GetAchievementCount();
    uint32_t GetUnlockedAchievementCount();
    const char* GetAchievementName(uint32_t index);
    std::vector<AchievementInfo> GetAchievementSnapshot();
    void UnlockAllAchievements();
    void ClearAllAchievements();

    std::vector<StatInfo> GetStatSnapshot();
    void ClearStats();

    bool ResetAll(bool achievementsToo);

    void Save();
}
