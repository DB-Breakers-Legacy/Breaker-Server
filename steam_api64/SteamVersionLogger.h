#pragma once
#include "StdInc.h"

namespace SteamVersionLogger
{
    struct InterfaceRequestInfo
    {
        std::string Source;
        std::string Version;
        uint64_t Count = 0;
    };

    struct CallRequestInfo
    {
        std::string Name;
        uint64_t Count = 0;
    };

    void LogExportRequest(const char* name);
    void LogInterfaceRequest(const char* source, const char* version);
    void LogCall(const char* interfaceName, const char* functionName, const std::string& detail = std::string());
    std::vector<InterfaceRequestInfo> SnapshotInterfaceRequests();
    std::vector<CallRequestInfo> SnapshotCallRequests();
    std::string LastInterfaceRequest();
    std::string LastCallRequest();
    uint64_t InterfaceRequestCount();
    uint64_t CallRequestCount();
    bool Contains(const std::string& text, const char* needle);
}
