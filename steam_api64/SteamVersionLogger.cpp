#include "StdInc.h"
#include "SteamVersionLogger.h"
#include "SteamDiagnostics.h"
#include "Logger.h"

namespace
{
    std::mutex g_Mutex;
    std::map<std::string, uint64_t> g_ExportRequests;
    std::vector<SteamVersionLogger::InterfaceRequestInfo> g_InterfaceRequests;
    std::vector<SteamVersionLogger::CallRequestInfo> g_CallRequests;
    std::string g_LastInterfaceRequest;
    std::string g_LastCallRequest;
    uint64_t g_InterfaceRequestCount = 0;
    uint64_t g_CallRequestCount = 0;

    bool CanReadTextPointer(const char* value)
    {
        if (!value)
            return false;

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(value, &mbi, sizeof(mbi)) == 0)
            return false;

        if (mbi.State != MEM_COMMIT)
            return false;

        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            return false;

        const DWORD protect = mbi.Protect & 0xff;
        return protect == PAGE_READONLY ||
            protect == PAGE_READWRITE ||
            protect == PAGE_WRITECOPY ||
            protect == PAGE_EXECUTE_READ ||
            protect == PAGE_EXECUTE_READWRITE ||
            protect == PAGE_EXECUTE_WRITECOPY;
    }

    std::string NormalizeText(const char* value, const char* fallback)
    {
        if (!CanReadTextPointer(value))
            return fallback;

        std::string text;
        text.reserve(64);

        for (size_t i = 0; i < 128; ++i)
        {
            const char* current = value + i;
            if (!CanReadTextPointer(current))
                break;

            const char c = *current;
            if (c == '\0')
                break;

            text.push_back(c);
        }

        return text.empty() ? fallback : text;
    }

    bool ShouldLogCount(uint64_t itemCount)
    {
        return itemCount == 1 ||
            itemCount == 10 ||
            itemCount == 100 ||
            itemCount == 1000 ||
            itemCount == 10000 ||
            (itemCount % 100000) == 0;
    }

    bool IsHighFrequencyCall(const std::string& name)
    {
        return name == "SteamAPI::RunCallbacks" ||
            name == "SteamUtils::IsAPICallCompleted" ||
            name == "SteamNetworking::IsP2PPacketAvailable" ||
            name == "SteamUser::GetAvailableVoice" ||
            name == "SteamUser::GetVoice";
    }

    bool IsHighFrequencyExport(const std::string& name)
    {
        return name == "SteamNetworking" ||
            name == "SteamUser";
    }

    bool IsHighFrequencyInterface(const std::string& source, const std::string& version)
    {
        return (source == "BreakersRevived_SteamNetworking" && version == "SteamNetworking006") ||
            (source == "FakeSteamCore::UserForVersion" && version.rfind("SteamUser", 0) == 0);
    }
}

namespace SteamVersionLogger
{
    void LogExportRequest(const char* name)
    {
        const std::string safeName = NormalizeText(name, "null");
        uint64_t itemCount = 0;

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            itemCount = ++g_ExportRequests[safeName];
        }

        if (!IsHighFrequencyExport(safeName) && ShouldLogCount(itemCount))
        {
            Logger::Info(
                "[STEAM EXPORT] " +
                safeName +
                " x" +
                std::to_string(static_cast<unsigned long long>(itemCount)));
        }
    }

    void LogInterfaceRequest(const char* source, const char* version)
    {
        const std::string safeSource = NormalizeText(source, "unknown");
        const std::string safeVersion = NormalizeText(version, "null");

        uint64_t itemCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            ++g_InterfaceRequestCount;
            g_LastInterfaceRequest = safeSource + " -> " + safeVersion;

            auto it = std::find_if(
                g_InterfaceRequests.begin(),
                g_InterfaceRequests.end(),
                [&](const InterfaceRequestInfo& item) {
                    return item.Source == safeSource && item.Version == safeVersion;
                });

            if (it == g_InterfaceRequests.end())
            {
                g_InterfaceRequests.push_back({ safeSource, safeVersion, 1 });
                itemCount = 1;
            }
            else
            {
                itemCount = ++it->Count;
            }
        }

        const bool highFrequency = IsHighFrequencyInterface(safeSource, safeVersion);
        if (!highFrequency)
            SteamDiagnostics::MarkSteam("InterfaceRequest", safeSource + " -> " + safeVersion);

        if (!highFrequency && ShouldLogCount(itemCount))
        {
            Logger::Info(
                "[STEAM INTERFACE] " +
                safeSource +
                " requested: " +
                safeVersion +
                " x" +
                std::to_string(static_cast<unsigned long long>(itemCount)));
        }
    }

    void LogCall(const char* interfaceName, const char* functionName, const std::string& detail)
    {
        const std::string safeInterface = NormalizeText(interfaceName, "Steam");
        const std::string safeFunction = NormalizeText(functionName, "unknown");
        const std::string name = safeInterface + "::" + safeFunction;
        const std::string display = detail.empty() ? name : (name + " | " + detail);

        uint64_t itemCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            ++g_CallRequestCount;
            g_LastCallRequest = display;

            auto it = std::find_if(
                g_CallRequests.begin(),
                g_CallRequests.end(),
                [&](const CallRequestInfo& item) {
                    return item.Name == name;
                });

            if (it == g_CallRequests.end())
            {
                g_CallRequests.push_back({ name, 1 });
                itemCount = 1;
            }
            else
            {
                itemCount = ++it->Count;
            }
        }

        if (IsHighFrequencyCall(name))
            return;

        if (ShouldLogCount(itemCount))
        {
            SteamDiagnostics::MarkSteam("Call", display);
            Logger::Info(
                "[STEAM CALL] " +
                display +
                " x" +
                std::to_string(static_cast<unsigned long long>(itemCount)));
        }
        else
        {
            SteamDiagnostics::MarkSteam("Call", display);
        }
    }

    std::vector<InterfaceRequestInfo> SnapshotInterfaceRequests()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_InterfaceRequests;
    }

    std::vector<CallRequestInfo> SnapshotCallRequests()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_CallRequests;
    }

    std::string LastInterfaceRequest()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_LastInterfaceRequest;
    }

    std::string LastCallRequest()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_LastCallRequest;
    }

    uint64_t InterfaceRequestCount()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_InterfaceRequestCount;
    }

    uint64_t CallRequestCount()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_CallRequestCount;
    }

    bool Contains(const std::string& text, const char* needle)
    {
        if (!needle)
            return false;

        return text.find(needle) != std::string::npos;
    }
}
