#pragma once
#include "StdInc.h"

namespace SteamDiagnostics
{
    struct Breadcrumb
    {
        uint64_t TickMs = 0;
        uint32_t ThreadId = 0;
        std::string Category;
        std::string Name;
        std::string Detail;
    };

    bool Init();
    void Shutdown();

    void Mark(const char* category, const std::string& name, const std::string& detail = std::string());
    void MarkSteam(const std::string& name, const std::string& detail = std::string());
    void MarkOnline(const std::string& name, const std::string& detail = std::string());

    std::vector<Breadcrumb> Snapshot(size_t maxItems = 200);
    std::string LastBreadcrumb();
    std::string LastException();
    uint32_t ExceptionCount();
    std::string WriteReport();
}
