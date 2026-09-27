#include "StdInc.h"
#include "Logger.h"

static std::ofstream g_Log;
static std::mutex g_LogMutex;

static bool g_ConsoleReady = false;
static bool g_ConsoleAllocatedByUs = false;
static bool g_LoggerInitialized = false;
static std::string g_LastInfoLine;
static uint32_t g_RepeatedInfoCount = 0;
static uint32_t g_InfoLinesSinceFlush = 0;
static Logger::Level g_MinimumLevel = Logger::Level::Trace;
static std::string g_LogPath;
static std::string g_LogsDirectory;
static std::vector<std::string> g_RecentLines;
static constexpr size_t kMaxRecentLines = 700;

static std::string ToLowerString(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    return text;
}

static std::string GetProcessPath()
{
    char exePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    return exePath;
}

static std::string GetProcessName()
{
    std::string path = GetProcessPath();

    size_t slash = path.find_last_of("\\/");

    if (slash != std::string::npos)
        path = path.substr(slash + 1);

    return ToLowerString(path);
}

static std::string GetGameDirectory()
{
    std::string path = GetProcessPath();

    size_t slash = path.find_last_of("\\/");

    if (slash != std::string::npos)
        return path.substr(0, slash + 1);

    return "";
}

static bool IsRealGameProcess()
{
    std::string exe = GetProcessName();

    if (exe.empty())
        return false;

    if (exe == "explorer.exe" || exe == "devenv.exe" || exe == "msbuild.exe")
        return false;

    return exe.size() > 4 && exe.substr(exe.size() - 4) == ".exe";
}

static std::string TimestampText()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);

    char buffer[64]{};
    sprintf_s(
        buffer,
        "%04u-%02u-%02u %02u:%02u:%02u.%03u",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds);
    return buffer;
}

static void PushRecent(const std::string& line)
{
    g_RecentLines.push_back(line);

    if (g_RecentLines.size() > kMaxRecentLines)
        g_RecentLines.erase(g_RecentLines.begin(), g_RecentLines.begin() + (g_RecentLines.size() - kMaxRecentLines));
}

static void InitConsole()
{
    // Do not attach or allocate a console inside the game process.
    // Breakers treats top-level windows owned by its process as part of its
    // focus state; an in-process console can make the game pick the console
    // as its active window, which reverses foreground audio/input behavior.
    // Logging remains available through the file and OutputDebugStringA.
    g_ConsoleReady = false;
    g_ConsoleAllocatedByUs = false;
}

static bool ShouldWrite(Logger::Level level)
{
    return static_cast<int>(level) >= static_cast<int>(g_MinimumLevel);
}

static void WriteLine(Logger::Level level, const char* category, const std::string& text, bool flushNow)
{
    if (!ShouldWrite(level))
        return;

    std::lock_guard<std::mutex> lock(g_LogMutex);

    std::string line = "[" + TimestampText() + "] [" + std::to_string(GetCurrentThreadId()) + "] [" + Logger::LevelName(level) + "]";
    if (category && category[0])
        line += " [" + std::string(category) + "]";
    line += " " + text;

    if (level == Logger::Level::Info && line == g_LastInfoLine)
    {
        ++g_RepeatedInfoCount;
        return;
    }

    if (g_Log.is_open())
    {
        if (g_RepeatedInfoCount > 0)
        {
            const std::string repeated = "[" + TimestampText() + "] [" + std::to_string(GetCurrentThreadId()) + "] [INFO] previous message repeated " + std::to_string(g_RepeatedInfoCount) + " times";
            g_Log << repeated << '\n';
            PushRecent(repeated);
            g_RepeatedInfoCount = 0;
        }

        g_Log << line << '\n';
        PushRecent(line);

        if (flushNow || ++g_InfoLinesSinceFlush >= 64)
        {
            g_Log.flush();
            g_InfoLinesSinceFlush = 0;
        }
    }
    else
    {
        PushRecent(line);
    }

    OutputDebugStringA((line + "\n").c_str());

    if (g_ConsoleReady)
        std::cout << line << std::endl;

    if (level == Logger::Level::Info)
        g_LastInfoLine = line;
}

bool Logger::Init()
{
    if (g_LoggerInitialized)
        return true;

    g_LoggerInitialized = true;

    std::string gameDir = GetGameDirectory();
    std::string baseDir = gameDir + "BreakersRevived";

    g_LogsDirectory = baseDir + "\\Logs";

    CreateDirectoryA(baseDir.c_str(), nullptr);
    CreateDirectoryA(g_LogsDirectory.c_str(), nullptr);

    g_LogPath = g_LogsDirectory + "\\BreakersRevived.log";

    DeleteFileA(g_LogPath.c_str());

    g_Log.open(g_LogPath, std::ios::out | std::ios::trunc);

    if (!g_Log.is_open())
        return false;

    if (IsRealGameProcess())
        InitConsole();

    Info("Logger initialized");
    Info("Fresh log created");
    Info("Process: " + GetProcessPath());
    Info("Process name: " + GetProcessName());
    Info("Log path: " + g_LogPath);

    return true;
}

void Logger::Shutdown()
{
    Info("Logger shutdown");
    Flush();

    {
        std::lock_guard<std::mutex> lock(g_LogMutex);

        if (g_Log.is_open())
            g_Log.close();
    }

    if (g_ConsoleReady && g_ConsoleAllocatedByUs)
        FreeConsole();

    g_ConsoleReady = false;
    g_ConsoleAllocatedByUs = false;
    g_LoggerInitialized = false;
    g_LastInfoLine.clear();
    g_RepeatedInfoCount = 0;
    g_InfoLinesSinceFlush = 0;
}

void Logger::Trace(const std::string& category, const std::string& text)
{
    WriteLine(Level::Trace, category.c_str(), text, false);
}

void Logger::Info(const std::string& text)
{
    WriteLine(Level::Info, nullptr, text, false);
}

void Logger::Warn(const std::string& text)
{
    WriteLine(Level::Warn, nullptr, text, false);
}

void Logger::Error(const std::string& text)
{
    WriteLine(Level::Error, nullptr, text, true);
}

void Logger::Fatal(const std::string& text)
{
    WriteLine(Level::Fatal, nullptr, text, true);
}

void Logger::Mark(const std::string& text)
{
    WriteLine(Level::Info, "MARK", text, true);
}

void Logger::Flush()
{
    std::lock_guard<std::mutex> lock(g_LogMutex);

    if (g_Log.is_open())
    {
        if (g_RepeatedInfoCount > 0)
        {
            const std::string repeated = "[" + TimestampText() + "] [" + std::to_string(GetCurrentThreadId()) + "] [INFO] previous message repeated " + std::to_string(g_RepeatedInfoCount) + " times";
            g_Log << repeated << '\n';
            PushRecent(repeated);
            g_RepeatedInfoCount = 0;
        }

        g_Log.flush();
        g_InfoLinesSinceFlush = 0;
    }
}

void Logger::SetMinimumLevel(Level level)
{
    g_MinimumLevel = level;
    Info(std::string("Logger minimum level set to ") + LevelName(level));
}

Logger::Level Logger::GetMinimumLevel()
{
    return g_MinimumLevel;
}

bool Logger::SetMinimumLevelFromText(const std::string& levelText)
{
    const std::string level = ToLowerString(levelText);
    if (level == "trace" || level == "verbose" || level == "debug")
    {
        SetMinimumLevel(Level::Trace);
        return true;
    }
    if (level == "info")
    {
        SetMinimumLevel(Level::Info);
        return true;
    }
    if (level == "warn" || level == "warning")
    {
        SetMinimumLevel(Level::Warn);
        return true;
    }
    if (level == "error")
    {
        SetMinimumLevel(Level::Error);
        return true;
    }
    if (level == "fatal")
    {
        SetMinimumLevel(Level::Fatal);
        return true;
    }
    return false;
}

const char* Logger::LevelName(Level level)
{
    switch (level)
    {
    case Level::Trace: return "TRACE";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
    case Level::Fatal: return "FATAL";
    default: return "INFO";
    }
}

std::string Logger::GetLogPath()
{
    std::lock_guard<std::mutex> lock(g_LogMutex);
    return g_LogPath;
}

std::string Logger::GetLogsDirectory()
{
    std::lock_guard<std::mutex> lock(g_LogMutex);
    return g_LogsDirectory;
}

std::vector<std::string> Logger::SnapshotRecent(size_t maxItems)
{
    std::lock_guard<std::mutex> lock(g_LogMutex);

    if (maxItems == 0 || g_RecentLines.size() <= maxItems)
        return g_RecentLines;

    return std::vector<std::string>(g_RecentLines.end() - maxItems, g_RecentLines.end());
}

bool Logger::WriteRecentLines(const std::string& path, size_t maxItems)
{
    const std::vector<std::string> lines = SnapshotRecent(maxItems);
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open())
        return false;

    for (const std::string& line : lines)
        out << line << '\n';

    return true;
}

void Logger::ExportInitialized(const char* exportName)
{
    Info(std::string(exportName) + ": initialized");
}
