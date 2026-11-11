#include "StdInc.h"
#include "SteamStorageLocal.h"
#include "SteamDiagnostics.h"
#include "Logger.h"

#include <chrono>
#include <cctype>

static std::string GetStorageRoot()
{
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);

    std::string base = path;
    size_t slash = base.find_last_of("\\/");

    if (slash != std::string::npos)
        base = base.substr(0, slash + 1);

    std::string root = base + "BreakersRevived\\storage\\";
    CreateDirectoryA((base + "BreakersRevived").c_str(), nullptr);
    CreateDirectoryA(root.c_str(), nullptr);

    return root;
}

static std::string SafePath(const char* name)
{
    std::string file = name ? name : "unknown.bin";

    for (char& c : file)
    {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }

    return GetStorageRoot() + file;
}

static std::string UpperText(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

static bool IsLegacySaveName(const char*)
{
    return false;
}

static std::string PointerText(const void* ptr)
{
    std::ostringstream ss;
    ss << "0x" << std::hex << reinterpret_cast<uintptr_t>(ptr);
    return ss.str();
}

static uint64_t NowMs()
{
    return static_cast<uint64_t>(GetTickCount64());
}

static std::string TimestampText()
{
    SYSTEMTIME time{};
    GetLocalTime(&time);

    char text[64]{};
    sprintf_s(
        text,
        "%04u%02u%02u-%02u%02u%02u",
        static_cast<unsigned int>(time.wYear),
        static_cast<unsigned int>(time.wMonth),
        static_cast<unsigned int>(time.wDay),
        static_cast<unsigned int>(time.wHour),
        static_cast<unsigned int>(time.wMinute),
        static_cast<unsigned int>(time.wSecond));
    return text;
}

static void PushSaveWarning(const std::string& message)
{
    static std::mutex s_WarningMutex;
    static uint64_t s_LastWarningMs = 0;

    const uint64_t now = NowMs();

    {
        std::lock_guard<std::mutex> lock(s_WarningMutex);
        if (now - s_LastWarningMs < 1800)
            return;

        s_LastWarningMs = now;
    }

    Logger::Error("Save Warning: " + message);
}

static bool WriteBytesToFile(const std::string& path, const std::vector<unsigned char>& bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);

    if (!file.is_open())
        return false;

    if (!bytes.empty())
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

    return file.good();
}

static size_t WritableBytesFrom(void* ptr, size_t requested)
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
    const bool writable =
        protect == PAGE_READWRITE ||
        protect == PAGE_WRITECOPY ||
        protect == PAGE_EXECUTE_READWRITE ||
        protect == PAGE_EXECUTE_WRITECOPY;

    if (!writable)
        return 0;

    const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;

    if (regionEnd <= start)
        return 0;

    return std::min<size_t>(requested, regionEnd - start);
}

static size_t ReadableBytesFrom(const void* ptr, size_t requested)
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

static size_t CopyFromReadableMemory(std::vector<unsigned char>& out, const void* src, size_t size)
{
    out.clear();

    if (!src || size == 0)
        return 0;

    out.resize(size);

    size_t copied = 0;
    while (copied < size)
    {
        const unsigned char* in = static_cast<const unsigned char*>(src) + copied;
        const size_t chunk = ReadableBytesFrom(in, size - copied);

        if (chunk == 0)
        {
            SteamDiagnostics::MarkSteam(
                "StorageWriteCopyStop",
                "offset=" +
                std::to_string(static_cast<unsigned long long>(copied)) +
                " src=" +
                PointerText(in));
            Logger::Error(
                "SteamStorageLocal stopped write copy at invalid source offset " +
                std::to_string(static_cast<unsigned long long>(copied)));
            break;
        }

        std::memcpy(out.data() + copied, in, chunk);
        copied += chunk;
    }

    out.resize(copied);
    return copied;
}

static size_t CopyToWritableMemory(void* dest, const unsigned char* src, size_t size)
{
    if (!dest || !src || size == 0)
        return 0;

    size_t copied = 0;
    while (copied < size)
    {
        unsigned char* out = static_cast<unsigned char*>(dest) + copied;
        const size_t chunk = WritableBytesFrom(out, size - copied);

        if (chunk == 0)
        {
            SteamDiagnostics::MarkSteam(
                "StorageReadCopyStop",
                "offset=" +
                std::to_string(static_cast<unsigned long long>(copied)) +
                " dest=" +
                PointerText(out));
            Logger::Error(
                "SteamStorageLocal stopped copy at invalid buffer offset " +
                std::to_string(static_cast<unsigned long long>(copied)));
            break;
        }

        std::memcpy(out, src + copied, chunk);
        copied += chunk;
    }

    return copied;
}

static bool LooksSuspiciousLegacySaveBytes(
    const char* name,
    const unsigned char* bytes,
    size_t byteCount,
    const char* breadcrumb)
{
    if (!IsLegacySaveName(name) || !bytes || byteCount < 16)
        return false;

    uint64_t firstQword = 0;
    std::memcpy(&firstQword, bytes, sizeof(firstQword));

    const uint64_t pointerBand = (firstQword >> 40) & 0xff;
    const bool pointerLikeHeader = pointerBand == 0x7e || pointerBand == 0x7f;

    const size_t zeroCount = static_cast<size_t>(
        std::count(bytes, bytes + byteCount, static_cast<unsigned char>(0)));
    const size_t nonZeroCount = byteCount - zeroCount;
    const bool mostlyZero = byteCount > 4096 && (zeroCount * 100ull / byteCount) >= 90;
    const bool almostBlank = byteCount > 4096 && nonZeroCount <= 64;

    constexpr size_t kBlockSize = 1024;
    size_t blockCount = 0;
    size_t blankBlocks = 0;
    size_t activeBlocks = 0;

    for (size_t offset = 0; offset < byteCount; offset += kBlockSize)
    {
        const size_t end = std::min(offset + kBlockSize, byteCount);
        const bool blankBlock = std::all_of(
            bytes + offset,
            bytes + end,
            [](unsigned char value) { return value == 0; });

        ++blockCount;
        if (blankBlock)
            ++blankBlocks;
        else
            ++activeBlocks;
    }

    const bool sparsePointerDump = pointerLikeHeader && mostlyZero && activeBlocks <= 4;

    if (!sparsePointerDump && !almostBlank)
        return false;

    SteamDiagnostics::MarkSteam(
        breadcrumb && breadcrumb[0] ? breadcrumb : "StorageSuspiciousLegacySave",
        std::string(name ? name : "null") +
        " bytes=" +
        std::to_string(static_cast<unsigned long long>(byteCount)) +
        " zeros=" +
        std::to_string(static_cast<unsigned long long>(zeroCount)) +
        " nonZero=" +
        std::to_string(static_cast<unsigned long long>(nonZeroCount)) +
        " blocks=" +
        std::to_string(static_cast<unsigned long long>(activeBlocks)) +
        "/" +
        std::to_string(static_cast<unsigned long long>(blockCount)) +
        " blankBlocks=" +
        std::to_string(static_cast<unsigned long long>(blankBlocks)) +
        " sparsePointerDump=" +
        std::to_string(sparsePointerDump ? 1 : 0) +
        " almostBlank=" +
        std::to_string(almostBlank ? 1 : 0) +
        " first=" +
        PointerText(reinterpret_cast<const void*>(static_cast<uintptr_t>(firstQword))));
    return true;
}

static bool LooksSuspiciousLegacySave(const char* name, const std::string& path)
{
    if (!IsLegacySaveName(name))
        return false;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return false;

    const std::streamoff endPos = file.tellg();
    if (endPos < 16 || endPos > 2 * 1024 * 1024)
        return false;

    const size_t fileSize = static_cast<size_t>(endPos);
    std::vector<unsigned char> bytes(fileSize);

    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

    const size_t readBytes = static_cast<size_t>(std::max<std::streamsize>(0, file.gcount()));
    if (readBytes < 16)
        return false;

    if (!LooksSuspiciousLegacySaveBytes(
        name,
        bytes.data(),
        readBytes,
        "StorageSuspiciousLegacySave"))
    {
        return false;
    }

    Logger::Info("SteamStorageLocal noticed unusual sparse/blank legacy-save.dat layout; it will still be loaded");
    return true;
}

static void StoreRejectedLegacyWrite(const char* name, const std::vector<unsigned char>& bytes)
{
    if (!IsLegacySaveName(name) || bytes.empty())
        return;

    static std::mutex s_BackupMutex;
    static bool s_BackupWrittenThisRun = false;

    {
        std::lock_guard<std::mutex> lock(s_BackupMutex);
        if (s_BackupWrittenThisRun)
            return;

        s_BackupWrittenThisRun = true;
    }

    const std::string rejectedPath = SafePath(name) + ".rejected-write-" + TimestampText();

    if (WriteBytesToFile(rejectedPath, bytes))
    {
        SteamDiagnostics::MarkSteam(
            "StorageSuspiciousLegacyWriteBackedUp",
            rejectedPath +
            " bytes=" +
            std::to_string(static_cast<unsigned long long>(bytes.size())));
        Logger::Info("SteamStorageLocal backed up unusual legacy-save.dat write to " + rejectedPath);
    }
}

static bool IsRestorableLegacyBackupName(const std::string& name)
{
    return name.find("legacy-save.dat.corrupt-") == 0 ||
        name.find("legacy-save.dat.rejected-write-") == 0 ||
        name == "legacy-save.dat.sparse-backup";
}

static bool TryRestoreLatestLegacyBackup(const std::string& root)
{
    std::error_code ec;
    const std::filesystem::path rootPath(root);
    const std::filesystem::path currentPath = rootPath / "legacy-save.dat";

    if (std::filesystem::exists(currentPath, ec))
        return false;

    if (!std::filesystem::exists(rootPath, ec))
        return false;

    bool found = false;
    std::filesystem::path bestPath;
    std::filesystem::file_time_type bestWriteTime{};

    for (const auto& item : std::filesystem::directory_iterator(rootPath, ec))
    {
        if (ec)
            break;

        std::error_code itemEc;
        if (!item.is_regular_file(itemEc) || itemEc)
            continue;

        const std::string fileName = item.path().filename().string();
        if (!IsRestorableLegacyBackupName(fileName))
            continue;

        const auto writeTime = item.last_write_time(itemEc);
        if (itemEc)
            continue;

        if (!found || writeTime > bestWriteTime)
        {
            found = true;
            bestPath = item.path();
            bestWriteTime = writeTime;
        }
    }

    if (!found)
        return false;

    const std::string source = bestPath.string();
    const std::string target = currentPath.string();

    if (!CopyFileA(source.c_str(), target.c_str(), FALSE))
    {
        Logger::Error(
            "SteamStorageLocal failed to restore backed up legacy-save.dat from " +
            source +
            " error=" +
            std::to_string(GetLastError()));
        return false;
    }

    SteamDiagnostics::MarkSteam(
        "StorageRestoredLegacySaveBackup",
        source +
        " -> " +
        target);
    Logger::Info("SteamStorageLocal restored backed up legacy-save.dat from " + source);
    PushSaveWarning("Restored backed-up save. Loading it normally.");
    return true;
}

static std::vector<std::filesystem::directory_entry> ListStoredFiles()
{
    std::vector<std::filesystem::directory_entry> files;
    std::error_code ec;
    std::filesystem::path root = GetStorageRoot();

    if (!std::filesystem::exists(root, ec))
        return files;

    for (const auto& item : std::filesystem::directory_iterator(root, ec))
    {
        if (!ec && item.is_regular_file(ec))
        {
            const std::string name = item.path().filename().string();
            if (name.find(".sparse-backup") != std::string::npos ||
                name.find(".corrupt-") != std::string::npos ||
                name.find(".rejected-write-") != std::string::npos)
                continue;

            files.push_back(item);
        }
    }

    std::sort(
        files.begin(),
        files.end(),
        [](const auto& a, const auto& b)
        {
            return a.path().filename().string() < b.path().filename().string();
        });

    return files;
}

namespace SteamStorageLocal
{
    void Init()
    {
        const std::string root = GetStorageRoot();
        const std::string legacyPath = root + "legacy-save.dat";

        TryRestoreLatestLegacyBackup(root);

        if (LooksSuspiciousLegacySave("legacy-save.dat", legacyPath))
        {
            SteamDiagnostics::MarkSteam("StorageStartupSuspiciousLegacySaveAllowed", "legacy-save.dat");
            PushSaveWarning("Save looks unusual; loading anyway.");
        }

        Logger::Info("SteamStorageLocal initialized");
    }

    bool WriteFile(const char* name, const void* data, int32_t size)
    {
        if (size < 0)
            return false;

        constexpr int32_t kMaxSafeWrite = 32 * 1024 * 1024;
        if (size > kMaxSafeWrite)
        {
            Logger::Error(
                std::string("SteamStorageLocal refused oversized write ") +
                (name ? name : "null") +
                " size=" +
                std::to_string(size));
            return false;
        }

        SteamDiagnostics::MarkSteam(
            "StorageWriteBegin",
            std::string(name ? name : "null") +
            " bytes=" +
            std::to_string(size) +
            " src=" +
            PointerText(data));

        std::vector<unsigned char> buffer;
        if (size > 0)
        {
            const size_t copied = CopyFromReadableMemory(buffer, data, static_cast<size_t>(size));
            if (copied != static_cast<size_t>(size))
            {
                SteamDiagnostics::MarkSteam(
                    "StorageWriteInvalidSource",
                    std::string(name ? name : "null") +
                    " copied=" +
                    std::to_string(static_cast<unsigned long long>(copied)) +
                    " requested=" +
                    std::to_string(size));
                return false;
            }
        }

        const bool suspiciousLegacyWrite = LooksSuspiciousLegacySaveBytes(
            name,
            buffer.empty() ? nullptr : buffer.data(),
            buffer.size(),
            "StorageWriteSuspiciousLegacySave");

        const std::string path = SafePath(name);

        if (suspiciousLegacyWrite)
        {
            StoreRejectedLegacyWrite(name, buffer);
            PushSaveWarning("Save write looks unusual; saving anyway.");
            SteamDiagnostics::MarkSteam("StorageWriteSuspiciousLegacySaveAllowed", name ? name : "null");
            Logger::Info("SteamStorageLocal allowed unusual legacy-save.dat write instead of blocking the save");
        }

        std::ofstream file(path, std::ios::binary | std::ios::trunc);

        if (!file.is_open())
            return false;

        if (!buffer.empty())
            file.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));

        if (!file.good())
            return false;

        Logger::Info(std::string("SteamStorageLocal wrote ") + (name ? name : "null"));
        SteamDiagnostics::MarkSteam(
            "StorageWriteEnd",
            std::string(name ? name : "null") +
            " bytes=" +
            std::to_string(static_cast<unsigned long long>(buffer.size())));

        return true;
    }

    int32_t ReadFile(const char* name, void* data, int32_t maxSize)
    {
        if (!data || maxSize <= 0)
            return 0;

        constexpr int32_t kMaxSafeRead = 32 * 1024 * 1024;
        if (maxSize > kMaxSafeRead)
        {
            Logger::Error(
                std::string("SteamStorageLocal refused oversized read ") +
                (name ? name : "null") +
                " size=" +
                std::to_string(maxSize));
            return 0;
        }

        SteamDiagnostics::MarkSteam(
            "StorageReadBegin",
            std::string(name ? name : "null") +
            " requested=" +
            std::to_string(maxSize) +
            " dest=" +
            PointerText(data));

        const std::string path = SafePath(name);

        if (WritableBytesFrom(data, 1) == 0)
        {
            Logger::Error(
                std::string("SteamStorageLocal refused invalid read buffer for ") +
                (name ? name : "null"));
            SteamDiagnostics::MarkSteam(
                "StorageReadInvalidBuffer",
                std::string(name ? name : "null") +
                " dest=" +
                PointerText(data));
            return 0;
        }

        std::ifstream file(path, std::ios::binary | std::ios::ate);

        if (!file.is_open())
        {
            Logger::Info(std::string("SteamStorageLocal read missing file ") + (name ? name : "null"));
            SteamDiagnostics::MarkSteam("StorageReadMissing", name ? name : "null");
            return 0;
        }

        const std::streamoff endPos = file.tellg();
        if (endPos <= 0)
        {
            SteamDiagnostics::MarkSteam("StorageReadEmpty", name ? name : "null");
            return 0;
        }

        const size_t fileSize = static_cast<size_t>(endPos);
        const size_t wanted = std::min<size_t>(static_cast<size_t>(maxSize), fileSize);

        SteamDiagnostics::MarkSteam(
            "StorageReadFileOpen",
            std::string(name ? name : "null") +
            " fileBytes=" +
            std::to_string(static_cast<unsigned long long>(fileSize)) +
            " copyBytes=" +
            std::to_string(static_cast<unsigned long long>(wanted)));

        std::vector<unsigned char> buffer(wanted);
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));

        const size_t readBytes = static_cast<size_t>(std::max<std::streamsize>(0, file.gcount()));
        if (readBytes == 0)
        {
            SteamDiagnostics::MarkSteam("StorageReadNoBytes", name ? name : "null");
            return 0;
        }

        if (LooksSuspiciousLegacySaveBytes(
            name,
            buffer.empty() ? nullptr : buffer.data(),
            std::min(buffer.size(), readBytes),
            "StorageReadSuspiciousLegacySaveRaw"))
        {
            PushSaveWarning("Save looks unusual; loading anyway.");
            SteamDiagnostics::MarkSteam("StorageReadSuspiciousLegacySaveAllowed", name ? name : "null");
            Logger::Info("SteamStorageLocal allowed unusual legacy-save.dat read without sanitizing it");
        }

        const size_t copied = CopyToWritableMemory(data, buffer.data(), readBytes);

        SteamDiagnostics::MarkSteam(
            "StorageReadEnd",
            std::string(name ? name : "null") +
            " read=" +
            std::to_string(static_cast<unsigned long long>(readBytes)) +
            " copied=" +
            std::to_string(static_cast<unsigned long long>(copied)));

        return static_cast<int32_t>(copied);
    }

    bool Exists(const char* name)
    {
        const std::string path = SafePath(name);
        std::ifstream file(path, std::ios::binary);
        return file.good();
    }

    bool DeleteFileLocal(const char* name)
    {
        return DeleteFileA(SafePath(name).c_str()) != FALSE;
    }

    int32_t Size(const char* name)
    {
        const std::string path = SafePath(name);

        std::ifstream file(path, std::ios::binary | std::ios::ate);

        if (!file.is_open())
            return 0;

        return static_cast<int32_t>(file.tellg());
    }

    int64_t Timestamp(const char* name)
    {
        std::error_code ec;
        auto time = std::filesystem::last_write_time(SafePath(name), ec);

        if (ec)
            return 0;

        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
            time.time_since_epoch()).count();

        return static_cast<int64_t>(seconds);
    }

    int32_t FileCount()
    {
        return static_cast<int32_t>(ListStoredFiles().size());
    }

    const char* FileNameAndSize(int index, int32_t* size)
    {
        static std::string s_ReturnName;
        auto files = ListStoredFiles();

        if (index < 0 || index >= static_cast<int>(files.size()))
        {
            if (size) *size = 0;
            s_ReturnName.clear();
            return s_ReturnName.c_str();
        }

        const auto& file = files[index];
        std::error_code ec;

        if (size)
            *size = static_cast<int32_t>(file.file_size(ec));

        s_ReturnName = file.path().filename().string();
        return s_ReturnName.c_str();
    }
}
