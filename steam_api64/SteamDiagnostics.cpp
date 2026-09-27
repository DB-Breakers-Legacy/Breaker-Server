#include "StdInc.h"
#include "SteamDiagnostics.h"
#include "Logger.h"
#include "CrashHandler.h"
#include "SupportBundle.h"

#include <iomanip>

namespace
{
    constexpr size_t kMaxBreadcrumbs = 256;

    std::mutex g_Mutex;
    std::vector<SteamDiagnostics::Breadcrumb> g_Breadcrumbs;
    std::string g_LastException;
    uint32_t g_ExceptionCount = 0;
    PVOID g_ExceptionHandler = nullptr;

    std::string GetGameDirectory()
    {
        char path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path().string();
    }

    std::string GetReportPath()
    {
        std::filesystem::path dir = std::filesystem::path(GetGameDirectory()) / "BreakersRevived" / "Logs";

        try
        {
            std::filesystem::create_directories(dir);
        }
        catch (...)
        {
        }

        return (dir / "SteamDiagnosticsReport.txt").string();
    }

    std::string PointerText(const void* ptr)
    {
        std::ostringstream ss;
        ss << "0x" << std::hex << reinterpret_cast<uintptr_t>(ptr);
        return ss.str();
    }

    std::string ExceptionCodeText(DWORD code)
    {
        std::ostringstream ss;
        ss << "0x" << std::hex << code;
        return ss.str();
    }

    std::string ModuleText(const void* address)
    {
        if (!address)
            return "null";

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0 || !mbi.AllocationBase)
            return "unknown";

        char path[MAX_PATH]{};
        if (GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), path, MAX_PATH) == 0)
            return "unknown";

        std::ostringstream ss;
        ss << path << " base=" << PointerText(mbi.AllocationBase);
        return ss.str();
    }

    size_t ReadableBytesFrom(const void* ptr, size_t requested)
    {
        if (!ptr || requested == 0)
            return 0;

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
            return 0;

        if (mbi.State != MEM_COMMIT)
            return 0;

        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            return 0;

        const DWORD protect = mbi.Protect & 0xff;
        const bool readable =
            protect == PAGE_READONLY ||
            protect == PAGE_READWRITE ||
            protect == PAGE_WRITECOPY ||
            protect == PAGE_EXECUTE ||
            protect == PAGE_EXECUTE_READ ||
            protect == PAGE_EXECUTE_READWRITE ||
            protect == PAGE_EXECUTE_WRITECOPY;

        if (!readable)
            return 0;

        const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= start)
            return 0;

        return std::min<size_t>(requested, regionEnd - start);
    }

    std::string BytesText(const void* address, size_t maxBytes)
    {
        if (!address || maxBytes == 0)
            return "none";

        const size_t readable = ReadableBytesFrom(address, maxBytes);
        if (readable == 0)
            return "unreadable";

        const auto* bytes = static_cast<const unsigned char*>(address);
        std::ostringstream ss;
        ss << std::hex << std::setfill('0');

        for (size_t i = 0; i < readable; ++i)
        {
            if (i != 0)
                ss << ' ';

            ss << std::setw(2) << static_cast<unsigned int>(bytes[i]);
        }

        return ss.str();
    }

    std::string StackQwordText(const CONTEXT* context)
    {
#if defined(_M_X64)
        if (!context)
            return std::string();

        const void* stack = reinterpret_cast<const void*>(context->Rsp);
        const size_t readable = ReadableBytesFrom(stack, sizeof(uint64_t) * 6);
        if (readable < sizeof(uint64_t))
            return " stack=unreadable";

        std::ostringstream ss;
        ss << " stack=";

        const size_t count = readable / sizeof(uint64_t);
        const uint64_t* qwords = static_cast<const uint64_t*>(stack);
        for (size_t i = 0; i < std::min<size_t>(count, 6); ++i)
        {
            if (i != 0)
                ss << ',';

            ss << PointerText(reinterpret_cast<const void*>(static_cast<uintptr_t>(qwords[i])));
        }

        return ss.str();
#else
        NSR_UNUSED(context);
        return std::string();
#endif
    }

    std::string AccessViolationText(const EXCEPTION_RECORD* record)
    {
        if (!record || record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2)
            return std::string();

        const ULONG_PTR operation = record->ExceptionInformation[0];
        const void* target = reinterpret_cast<const void*>(record->ExceptionInformation[1]);

        const char* operationText = "read";
        if (operation == 1)
            operationText = "write";
        else if (operation == 8)
            operationText = "execute";

        return std::string(" av=") +
            operationText +
            " target=" +
            PointerText(target) +
            " targetModule=" +
            ModuleText(target);
    }

    std::string ContextText(const CONTEXT* context)
    {
        if (!context)
            return std::string();

        std::ostringstream ss;
#if defined(_M_X64)
        ss
            << " rip="
            << PointerText(reinterpret_cast<const void*>(context->Rip))
            << " rsp="
            << PointerText(reinterpret_cast<const void*>(context->Rsp))
            << " rbp="
            << PointerText(reinterpret_cast<const void*>(context->Rbp))
            << " rax="
            << PointerText(reinterpret_cast<const void*>(context->Rax))
            << " rbx="
            << PointerText(reinterpret_cast<const void*>(context->Rbx))
            << " rcx="
            << PointerText(reinterpret_cast<const void*>(context->Rcx))
            << " rdx="
            << PointerText(reinterpret_cast<const void*>(context->Rdx))
            << " rsi="
            << PointerText(reinterpret_cast<const void*>(context->Rsi))
            << " rdi="
            << PointerText(reinterpret_cast<const void*>(context->Rdi))
            << " r8="
            << PointerText(reinterpret_cast<const void*>(context->R8))
            << " r9="
            << PointerText(reinterpret_cast<const void*>(context->R9));
#elif defined(_M_IX86)
        ss
            << " eip="
            << PointerText(reinterpret_cast<const void*>(context->Eip))
            << " esp="
            << PointerText(reinterpret_cast<const void*>(context->Esp))
            << " ecx="
            << PointerText(reinterpret_cast<const void*>(context->Ecx))
            << " edx="
            << PointerText(reinterpret_cast<const void*>(context->Edx));
#endif
        return ss.str();
    }

    bool ShouldCaptureException(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
            return true;
        default:
            return false;
        }
    }

    LONG CALLBACK VectoredExceptionHandler(PEXCEPTION_POINTERS info)
    {
        if (!info || !info->ExceptionRecord)
            return EXCEPTION_CONTINUE_SEARCH;

        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (!ShouldCaptureException(code))
            return EXCEPTION_CONTINUE_SEARCH;

        const void* address = info->ExceptionRecord->ExceptionAddress;

        std::string detail =
            "code=" +
            ExceptionCodeText(code) +
            " address=" +
            PointerText(address) +
            " module=" +
            ModuleText(address) +
            " codeBytes=" +
            BytesText(address, 16) +
            AccessViolationText(info->ExceptionRecord) +
            ContextText(info->ContextRecord) +
            StackQwordText(info->ContextRecord);

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            ++g_ExceptionCount;
            if (g_ExceptionCount > 16)
                return EXCEPTION_CONTINUE_SEARCH;

            g_LastException = detail;
        }

        std::string previousBreadcrumb = SteamDiagnostics::LastBreadcrumb();
        SteamDiagnostics::Mark("Crash", "Unhandled exception", detail);
        Logger::Error("SteamDiagnostics captured exception: " + detail);
        Logger::Error("SteamDiagnostics previous breadcrumb: " + previousBreadcrumb);
        const std::string dumpPath = CrashHandler::WriteMinidump(info, "vectored-exception");
        if (!dumpPath.empty())
            Logger::Error("SteamDiagnostics minidump: " + dumpPath);
        const std::string reportPath = SteamDiagnostics::WriteReport();
        if (!reportPath.empty())
            Logger::Error("SteamDiagnostics report: " + reportPath);
        const std::string bundlePath = SupportBundle::Write("crash");
        if (!bundlePath.empty())
            Logger::Error("SteamDiagnostics support bundle: " + bundlePath);
        Logger::Flush();

        return EXCEPTION_CONTINUE_SEARCH;
    }

    std::string FormatBreadcrumb(const SteamDiagnostics::Breadcrumb& item)
    {
        std::ostringstream ss;
        ss
            << item.TickMs
            << "ms ["
            << item.ThreadId
            << "] "
            << item.Category
            << ": "
            << item.Name;

        if (!item.Detail.empty())
            ss << " | " << item.Detail;

        return ss.str();
    }
}

namespace SteamDiagnostics
{
    bool Init()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        if (!g_ExceptionHandler)
            g_ExceptionHandler = AddVectoredExceptionHandler(1, VectoredExceptionHandler);

        g_Breadcrumbs.clear();
        g_LastException.clear();
        g_ExceptionCount = 0;

        CrashHandler::Init();

        Logger::Info("SteamDiagnostics initialized");
        return true;
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        if (g_ExceptionHandler)
        {
            RemoveVectoredExceptionHandler(g_ExceptionHandler);
            g_ExceptionHandler = nullptr;
        }

        g_Breadcrumbs.clear();
        CrashHandler::Shutdown();
    }

    void Mark(const char* category, const std::string& name, const std::string& detail)
    {
        Breadcrumb item{};
        item.TickMs = static_cast<uint64_t>(GetTickCount64());
        item.ThreadId = GetCurrentThreadId();
        item.Category = category && category[0] ? category : "General";
        item.Name = name;
        item.Detail = detail;

        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Breadcrumbs.push_back(item);

        if (g_Breadcrumbs.size() > kMaxBreadcrumbs)
            g_Breadcrumbs.erase(g_Breadcrumbs.begin(), g_Breadcrumbs.begin() + (g_Breadcrumbs.size() - kMaxBreadcrumbs));
    }

    void MarkSteam(const std::string& name, const std::string& detail)
    {
        Mark("Steam", name, detail);
    }

    void MarkOnline(const std::string& name, const std::string& detail)
    {
        Mark("Online", name, detail);
    }

    std::vector<Breadcrumb> Snapshot(size_t maxItems)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        if (maxItems == 0 || g_Breadcrumbs.size() <= maxItems)
            return g_Breadcrumbs;

        return std::vector<Breadcrumb>(g_Breadcrumbs.end() - maxItems, g_Breadcrumbs.end());
    }

    std::string LastBreadcrumb()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        if (g_Breadcrumbs.empty())
            return "none";

        return FormatBreadcrumb(g_Breadcrumbs.back());
    }

    std::string LastException()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_LastException.empty() ? "none" : g_LastException;
    }

    uint32_t ExceptionCount()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_ExceptionCount;
    }

    std::string WriteReport()
    {
        const std::string path = GetReportPath();
        std::vector<Breadcrumb> breadcrumbs = Snapshot(kMaxBreadcrumbs);

        std::ofstream out(path, std::ios::out | std::ios::trunc);

        if (!out.is_open())
            return std::string();

        out << "Breakers Revived Steam Diagnostics\n";
        out << "Last exception: " << LastException() << "\n";
        out << "Exception count: " << ExceptionCount() << "\n";
        out << "Last breadcrumb: " << LastBreadcrumb() << "\n\n";
        out << "Recent breadcrumbs\n";

        for (const Breadcrumb& item : breadcrumbs)
            out << FormatBreadcrumb(item) << "\n";

        out.close();
        Logger::Info("SteamDiagnostics wrote report " + path);
        return path;
    }
}
