#include "StdInc.h"
#include "CrashHandler.h"
#include "Logger.h"

#include <DbgHelp.h>

#pragma comment(lib, "Dbghelp.lib")

namespace
{
    std::mutex g_CrashMutex;
    LPTOP_LEVEL_EXCEPTION_FILTER g_PreviousFilter = nullptr;
    std::string g_LastDumpPath;
    volatile LONG g_WritingDump = 0;

    std::string Stamp()
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char buffer[64]{};
        sprintf_s(buffer, "%04u%02u%02u_%02u%02u%02u_%03u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        return buffer;
    }

    std::string CrashDirectory()
    {
        std::filesystem::path dir = Logger::GetLogsDirectory();
        if (dir.empty())
        {
            char exePath[MAX_PATH]{};
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            dir = std::filesystem::path(exePath).parent_path() / "BreakersRevived" / "Logs";
        }
        dir /= "Crashes";
        try { std::filesystem::create_directories(dir); } catch (...) {}
        return dir.string();
    }

    LONG WINAPI TopLevelFilter(EXCEPTION_POINTERS* info)
    {
        const std::string dumpPath = CrashHandler::WriteMinidump(info, "top-level-unhandled-exception");
        if (!dumpPath.empty())
            Logger::Fatal("Top-level crash dump written: " + dumpPath);
        Logger::Flush();

        if (g_PreviousFilter)
            return g_PreviousFilter(info);

        return EXCEPTION_CONTINUE_SEARCH;
    }
}

namespace CrashHandler
{
    bool Init()
    {
        std::lock_guard<std::mutex> lock(g_CrashMutex);
        if (!g_PreviousFilter)
            g_PreviousFilter = SetUnhandledExceptionFilter(TopLevelFilter);
        Logger::Info("CrashHandler initialized");
        return true;
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(g_CrashMutex);
        if (g_PreviousFilter)
        {
            SetUnhandledExceptionFilter(g_PreviousFilter);
            g_PreviousFilter = nullptr;
        }
    }

    std::string WriteMinidump(PEXCEPTION_POINTERS info, const std::string& reason)
    {
        if (InterlockedCompareExchange(&g_WritingDump, 1, 0) != 0)
            return g_LastDumpPath;

        std::string dumpPath;
        HANDLE file = INVALID_HANDLE_VALUE;

        const std::filesystem::path path = std::filesystem::path(CrashDirectory()) / ("crash_" + Stamp() + ".dmp");
        dumpPath = path.string();

        file = CreateFileA(dumpPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            InterlockedExchange(&g_WritingDump, 0);
            return std::string();
        }

        MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
        exceptionInfo.ThreadId = GetCurrentThreadId();
        exceptionInfo.ExceptionPointers = info;
        exceptionInfo.ClientPointers = FALSE;

        const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithDataSegs |
            MiniDumpWithHandleData |
            MiniDumpWithThreadInfo |
            MiniDumpWithUnloadedModules |
            MiniDumpWithIndirectlyReferencedMemory);

        const BOOL ok = MiniDumpWriteDump(
            GetCurrentProcess(),
            GetCurrentProcessId(),
            file,
            dumpType,
            info ? &exceptionInfo : nullptr,
            nullptr,
            nullptr);

        CloseHandle(file);

        if (!ok)
        {
            Logger::Error("MiniDumpWriteDump failed reason=" + reason + " error=" + std::to_string(GetLastError()));
            dumpPath.clear();
        }
        else
        {
            g_LastDumpPath = dumpPath;
            Logger::Fatal("Crash dump written reason=" + reason + " path=" + dumpPath);
        }

        InterlockedExchange(&g_WritingDump, 0);
        return dumpPath;
    }

    std::string LastDumpPath()
    {
        std::lock_guard<std::mutex> lock(g_CrashMutex);
        return g_LastDumpPath.empty() ? "none" : g_LastDumpPath;
    }
}
