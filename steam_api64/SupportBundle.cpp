#include "StdInc.h"
#include "SupportBundle.h"
#include "Logger.h"
#include "SteamDiagnostics.h"
#include "CrashHandler.h"

namespace
{
    std::string Stamp()
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char buffer[64]{};
        sprintf_s(buffer, "%04u%02u%02u_%02u%02u%02u_%03u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        return buffer;
    }

    void CopyIfExists(const std::filesystem::path& from, const std::filesystem::path& to)
    {
        try
        {
            if (!from.empty() && std::filesystem::exists(from))
                std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing);
        }
        catch (...)
        {
        }
    }

    std::string ProcessPath()
    {
        char exePath[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        return exePath;
    }
}

namespace SupportBundle
{
    std::string Write(const std::string& reason)
    {
        Logger::Flush();

        std::filesystem::path root = Logger::GetLogsDirectory();
        if (root.empty())
            root = std::filesystem::path(ProcessPath()).parent_path() / "BreakersRevived" / "Logs";

        std::filesystem::path bundleDir = root / "SupportBundles" / ("support_" + Stamp());

        try
        {
            std::filesystem::create_directories(bundleDir);
        }
        catch (...)
        {
            return std::string();
        }

        const std::string diagnosticsPath = SteamDiagnostics::WriteReport();
        Logger::WriteRecentLines((bundleDir / "recent_log_lines.txt").string(), 700);
        CopyIfExists(Logger::GetLogPath(), bundleDir / "main_log.txt");
        CopyIfExists(diagnosticsPath, bundleDir / "SteamDiagnosticsReport.txt");

        std::ofstream summary(bundleDir / "support_summary.txt", std::ios::out | std::ios::trunc);
        if (summary.is_open())
        {
            summary << "Breakers Revival Support Bundle\n";
            summary << "Reason=" << (reason.empty() ? "manual" : reason) << "\n";
            summary << "Process=" << ProcessPath() << "\n";
            summary << "LogPath=" << Logger::GetLogPath() << "\n";
            summary << "DiagnosticsPath=" << diagnosticsPath << "\n";
            summary << "LastException=" << SteamDiagnostics::LastException() << "\n";
            summary << "ExceptionCount=" << SteamDiagnostics::ExceptionCount() << "\n";
            summary << "LastBreadcrumb=" << SteamDiagnostics::LastBreadcrumb() << "\n";
            summary << "LastDump=" << CrashHandler::LastDumpPath() << "\n";
            summary << "RecentLineCount=" << Logger::SnapshotRecent(700).size() << "\n";
        }

        const std::string result = bundleDir.string();
        Logger::Info("Support bundle written: " + result);
        return result;
    }
}
