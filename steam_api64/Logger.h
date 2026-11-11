#pragma once

#include "StdInc.h"

namespace Logger
{
    enum class Level
    {
        Trace = 0,
        Info = 1,
        Warn = 2,
        Error = 3,
        Fatal = 4
    };

    bool Init();
    void Shutdown();

    void Trace(const std::string& category, const std::string& text);
    void Info(const std::string& text);
    void Warn(const std::string& text);
    void Error(const std::string& text);
    void Fatal(const std::string& text);
    void Mark(const std::string& text);
    void Flush();

    void SetMinimumLevel(Level level);
    Level GetMinimumLevel();
    bool SetMinimumLevelFromText(const std::string& levelText);
    const char* LevelName(Level level);

    std::string GetLogPath();
    std::string GetLogsDirectory();
    std::vector<std::string> SnapshotRecent(size_t maxItems = 200);
    bool WriteRecentLines(const std::string& path, size_t maxItems = 500);

    void ExportInitialized(const char* exportName);
}
