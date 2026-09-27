#include "StdInc.h"
#include "FakeSteamInterfaces.h"
#include "Logger.h"
#include "SteamCallResultManager.h"
#include "SteamDiagnostics.h"
#include "SteamIDManager.h"
#include "SteamStorageLocal.h"
#include "SteamVersionLogger.h"

typedef uint64_t UGCHandle_t;
typedef uint64_t UGCFileWriteStreamHandle_t;
typedef uint64_t PublishedFileUpdateHandle_t;
typedef uint64_t PublishedFileId_t;

namespace
{
    constexpr int kResultOK = 1;
    constexpr AppId_t kOfflineAppID = 1276760;
    constexpr UGCHandle_t kInvalidUGCHandle = 0xffffffffffffffffull;
    constexpr PublishedFileUpdateHandle_t kInvalidPublishedFileUpdateHandle = 0xffffffffffffffffull;

    struct RemoteStorageFileWriteAsyncCompleteResult
    {
        int m_eResult;
    };

    struct RemoteStorageFileReadAsyncCompleteResult
    {
        SteamAPICall_t m_hFileReadAsync;
        int m_eResult;
        uint32_t m_nOffset;
        uint32_t m_cubRead;
    };

    struct RemoteStorageFileShareResult
    {
        int m_eResult;
        UGCHandle_t m_hFile;
        char m_rgchFilename[260];
    };

    struct RemoteStorageDownloadUGCResult
    {
        int m_eResult;
        UGCHandle_t m_hFile;
        AppId_t m_nAppID;
        int32_t m_nSizeInBytes;
        char m_pchFileName[260];
        uint64_t m_ulSteamIDOwner;
    };

    std::mutex g_AsyncReadMutex;
    std::map<SteamAPICall_t, std::vector<unsigned char>> g_AsyncReadData;

    void TraceRemoteStorage(const char* functionName, const std::string& detail = std::string())
    {
        SteamVersionLogger::LogCall("SteamRemoteStorage", functionName, detail);
    }

    std::string FileDetail(const char* file, int64_t bytes = -1)
    {
        std::string detail = file ? file : "null";
        if (bytes >= 0)
            detail += " bytes=" + std::to_string(bytes);

        return detail;
    }

    SteamAPICall_t EmptyCallResult(int callbackID)
    {
        return SteamCallResultManager::CreateCallResult(callbackID, nullptr, 0, true);
    }

    bool IsWritableMemory(void* ptr, size_t size)
    {
        if (!ptr || size == 0)
            return false;

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
            return false;

        if (mbi.State != MEM_COMMIT)
            return false;

        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            return false;

        const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
        const uintptr_t end = start + size;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;

        if (end < start || end > regionEnd)
            return false;

        const DWORD protect = mbi.Protect & 0xff;
        return protect == PAGE_READWRITE ||
            protect == PAGE_WRITECOPY ||
            protect == PAGE_EXECUTE_READWRITE ||
            protect == PAGE_EXECUTE_WRITECOPY;
    }

    bool SafeZeroBuffer(void* ptr, size_t size, const char* label)
    {
        if (!ptr || size == 0)
            return false;

        if (!IsWritableMemory(ptr, size))
        {
            Logger::Error(std::string("SteamRemoteStorage refused invalid buffer: ") + label);
            return false;
        }

        memset(ptr, 0, size);
        return true;
    }

    bool GetQuota32(int32_t* total, int32_t* available)
    {
        constexpr int32_t quota = 1024 * 1024 * 1024;
        if (total) *total = quota;
        if (available) *available = quota;
        return true;
    }

    bool UGCDownloadProgress32(UGCHandle_t content, int32_t* downloaded, int32_t* expected)
    {
        NSR_UNUSED(content);
        if (downloaded) *downloaded = 0;
        if (expected) *expected = 0;
        return false;
    }
}

class FakeSteamRemoteStorage final
{
public:
    virtual bool FileWrite(const char* file, const void* data, int32_t size)
    {
        TraceRemoteStorage("FileWrite", FileDetail(file, size));
        Logger::Info(std::string("SteamRemoteStorage::FileWrite ") + (file ? file : "null"));
        SteamDiagnostics::MarkSteam(
            "RemoteStorageFileWrite",
            std::string(file ? file : "null") +
            " bytes=" +
            std::to_string(size));

        const bool ok = SteamStorageLocal::WriteFile(file, data, size);
        SteamDiagnostics::MarkSteam(
            "RemoteStorageFileWriteDone",
            std::string(file ? file : "null") +
            " ok=" +
            (ok ? "1" : "0"));

        if (!ok)
        {
            Logger::Error(
                std::string("SteamRemoteStorage::FileWrite failed safely for ") +
                (file ? file : "null"));
        }

        return ok;
    }

    virtual int32_t FileRead(const char* file, void* data, int32_t size)
    {
        TraceRemoteStorage("FileRead", FileDetail(file, size));
        Logger::Info(std::string("SteamRemoteStorage::FileRead ") + (file ? file : "null"));
        SteamDiagnostics::MarkSteam(
            "RemoteStorageFileRead",
            std::string(file ? file : "null") +
            " bytes=" +
            std::to_string(size));

        const int32_t read = SteamStorageLocal::ReadFile(file, data, size);
        SteamDiagnostics::MarkSteam(
            "RemoteStorageFileReadDone",
            std::string(file ? file : "null") +
            " read=" +
            std::to_string(read));
        return read;
    }

    virtual SteamAPICall_t FileWriteAsync(const char* file, const void* data, uint32_t size)
    {
        TraceRemoteStorage("FileWriteAsync", FileDetail(file, size));
        Logger::Info(std::string("SteamRemoteStorage::FileWriteAsync ") + (file ? file : "null"));
        const bool ok = SteamStorageLocal::WriteFile(file, data, static_cast<int32_t>(size));

        RemoteStorageFileWriteAsyncCompleteResult result{};
        result.m_eResult = ok ? kResultOK : 2;
        return SteamCallResultManager::CreateCallResult(1331, &result, sizeof(result), true);
    }

    virtual SteamAPICall_t FileReadAsync(const char* file, uint32_t offset, uint32_t size)
    {
        TraceRemoteStorage(
            "FileReadAsync",
            FileDetail(file, size) + " offset=" + std::to_string(offset));
        Logger::Info("SteamRemoteStorage::FileReadAsync");

        std::vector<unsigned char> readData;
        int resultCode = 2;
        uint32_t bytesRead = 0;

        const int32_t fileSize = SteamStorageLocal::Size(file);
        if (fileSize >= 0 && SteamStorageLocal::Exists(file))
        {
            std::vector<unsigned char> fileData(static_cast<size_t>(fileSize));
            const int32_t totalRead = fileSize > 0 ? SteamStorageLocal::ReadFile(file, fileData.data(), fileSize) : 0;

            if (totalRead >= 0 && offset <= static_cast<uint32_t>(std::max<int32_t>(0, totalRead)))
            {
                const uint32_t available = static_cast<uint32_t>(std::max<int32_t>(0, totalRead)) - offset;
                bytesRead = std::min(size, available);
                readData.assign(fileData.begin() + offset, fileData.begin() + offset + bytesRead);
                resultCode = kResultOK;
            }
        }

        RemoteStorageFileReadAsyncCompleteResult result{};
        result.m_eResult = resultCode;
        result.m_nOffset = offset;
        result.m_cubRead = bytesRead;

        const SteamAPICall_t call = SteamCallResultManager::CreateCallResult(1332, &result, sizeof(result), true);
        {
            std::lock_guard<std::mutex> lock(g_AsyncReadMutex);
            g_AsyncReadData[call] = std::move(readData);
        }

        return call;
    }

    virtual bool FileReadAsyncComplete(SteamAPICall_t call, void* data, uint32_t size)
    {
        TraceRemoteStorage(
            "FileReadAsyncComplete",
            "call=" + std::to_string(static_cast<unsigned long long>(call)) + " bytes=" + std::to_string(size));
        Logger::Info("SteamRemoteStorage::FileReadAsyncComplete");

        std::vector<unsigned char> readData;
        {
            std::lock_guard<std::mutex> lock(g_AsyncReadMutex);
            auto it = g_AsyncReadData.find(call);
            if (it != g_AsyncReadData.end())
            {
                readData = std::move(it->second);
                g_AsyncReadData.erase(it);
            }
        }

        if (readData.empty())
        {
            if (data && size > 0)
                SafeZeroBuffer(data, size, "FileReadAsyncComplete");

            return false;
        }

        if (!data || size == 0 || !IsWritableMemory(data, size))
        {
            Logger::Error("SteamRemoteStorage::FileReadAsyncComplete refused invalid destination");
            return false;
        }

        std::memset(data, 0, size);
        std::memcpy(data, readData.data(), std::min<size_t>(size, readData.size()));
        return true;
    }

    virtual bool FileForget(const char* file)
    {
        TraceRemoteStorage("FileForget", FileDetail(file));
        NSR_UNUSED(file);
        Logger::Info("SteamRemoteStorage::FileForget");
        return true;
    }

    virtual bool FileDelete(const char* file)
    {
        TraceRemoteStorage("FileDelete", FileDetail(file));
        Logger::Info(std::string("SteamRemoteStorage::FileDelete ") + (file ? file : "null"));
        return SteamStorageLocal::DeleteFileLocal(file);
    }

    virtual SteamAPICall_t FileShare(const char* file)
    {
        TraceRemoteStorage("FileShare", FileDetail(file));
        Logger::Info(std::string("SteamRemoteStorage::FileShare ") + (file ? file : "null"));

        RemoteStorageFileShareResult result{};
        result.m_eResult = kResultOK;
        result.m_hFile = 1;

        if (file)
            strncpy_s(result.m_rgchFilename, file, _TRUNCATE);

        return SteamCallResultManager::CreateCallResult(1307, &result, sizeof(result), true);
    }

    virtual bool SetSyncPlatforms(const char* file, int platforms)
    {
        TraceRemoteStorage("SetSyncPlatforms", FileDetail(file) + " platforms=" + std::to_string(platforms));
        NSR_UNUSED(file);
        NSR_UNUSED(platforms);
        Logger::Info("SteamRemoteStorage::SetSyncPlatforms");
        return true;
    }

    virtual UGCFileWriteStreamHandle_t FileWriteStreamOpen(const char* file)
    {
        TraceRemoteStorage("FileWriteStreamOpen", FileDetail(file));
        NSR_UNUSED(file);
        Logger::Info("SteamRemoteStorage::FileWriteStreamOpen");
        return 1;
    }

    virtual bool FileWriteStreamWriteChunk(UGCFileWriteStreamHandle_t writeHandle, const void* data, int32_t size)
    {
        TraceRemoteStorage("FileWriteStreamWriteChunk", "handle=" + std::to_string(writeHandle) + " bytes=" + std::to_string(size));
        NSR_UNUSED(writeHandle);
        NSR_UNUSED(data);
        NSR_UNUSED(size);
        Logger::Info("SteamRemoteStorage::FileWriteStreamWriteChunk");
        return true;
    }

    virtual bool FileWriteStreamClose(UGCFileWriteStreamHandle_t writeHandle)
    {
        TraceRemoteStorage("FileWriteStreamClose", "handle=" + std::to_string(writeHandle));
        NSR_UNUSED(writeHandle);
        Logger::Info("SteamRemoteStorage::FileWriteStreamClose");
        return true;
    }

    virtual bool FileWriteStreamCancel(UGCFileWriteStreamHandle_t writeHandle)
    {
        TraceRemoteStorage("FileWriteStreamCancel", "handle=" + std::to_string(writeHandle));
        NSR_UNUSED(writeHandle);
        Logger::Info("SteamRemoteStorage::FileWriteStreamCancel");
        return true;
    }

    virtual bool FileExists(const char* file)
    {
        TraceRemoteStorage("FileExists", FileDetail(file));
        Logger::Info(std::string("SteamRemoteStorage::FileExists ") + (file ? file : "null"));
        return SteamStorageLocal::Exists(file);
    }

    virtual bool FilePersisted(const char* file)
    {
        TraceRemoteStorage("FilePersisted", FileDetail(file));
        Logger::Info(std::string("SteamRemoteStorage::FilePersisted ") + (file ? file : "null"));
        return SteamStorageLocal::Exists(file);
    }

    virtual int32_t GetFileSize(const char* file)
    {
        TraceRemoteStorage("GetFileSize", FileDetail(file));
        Logger::Info(std::string("SteamRemoteStorage::GetFileSize ") + (file ? file : "null"));
        SteamDiagnostics::MarkSteam("RemoteStorageGetFileSize", file ? file : "null");
        return SteamStorageLocal::Size(file);
    }

    virtual int64_t GetFileTimestamp(const char* file)
    {
        TraceRemoteStorage("GetFileTimestamp", FileDetail(file));
        Logger::Info("SteamRemoteStorage::GetFileTimestamp");
        return SteamStorageLocal::Timestamp(file);
    }

    virtual int GetSyncPlatforms(const char* file)
    {
        TraceRemoteStorage("GetSyncPlatforms", FileDetail(file));
        NSR_UNUSED(file);
        Logger::Info("SteamRemoteStorage::GetSyncPlatforms");
        return 0;
    }

    virtual int32_t GetFileCount()
    {
        TraceRemoteStorage("GetFileCount");
        Logger::Info("SteamRemoteStorage::GetFileCount");
        return SteamStorageLocal::FileCount();
    }

    virtual const char* GetFileNameAndSize(int index, int32_t* size)
    {
        TraceRemoteStorage("GetFileNameAndSize", "index=" + std::to_string(index));
        Logger::Info("SteamRemoteStorage::GetFileNameAndSize");
        return SteamStorageLocal::FileNameAndSize(index, size);
    }

    virtual bool GetQuota(uint64_t* total, uint64_t* available)
    {
        TraceRemoteStorage("GetQuota");
        Logger::Info("SteamRemoteStorage::GetQuota");
        if (total) *total = 1024ull * 1024ull * 1024ull;
        if (available) *available = 1024ull * 1024ull * 1024ull;
        return true;
    }

    virtual bool IsCloudEnabledForAccount()
    {
        TraceRemoteStorage("IsCloudEnabledForAccount");
        Logger::Info("SteamRemoteStorage::IsCloudEnabledForAccount");
        return true;
    }

    virtual bool IsCloudEnabledForApp()
    {
        TraceRemoteStorage("IsCloudEnabledForApp");
        Logger::Info("SteamRemoteStorage::IsCloudEnabledForApp");
        return true;
    }

    virtual void SetCloudEnabledForApp(bool enabled)
    {
        TraceRemoteStorage("SetCloudEnabledForApp", enabled ? "true" : "false");
        NSR_UNUSED(enabled);
        Logger::Info("SteamRemoteStorage::SetCloudEnabledForApp");
    }

    virtual SteamAPICall_t UGCDownload(UGCHandle_t content, uint32_t priority)
    {
        TraceRemoteStorage(
            "UGCDownload",
            "content=" + std::to_string(content) + " priority=" + std::to_string(priority));
        NSR_UNUSED(priority);
        Logger::Info("SteamRemoteStorage::UGCDownload");

        RemoteStorageDownloadUGCResult result{};
        result.m_eResult = kResultOK;
        result.m_hFile = content;
        result.m_nAppID = kOfflineAppID;
        result.m_nSizeInBytes = 0;
        result.m_ulSteamIDOwner = SteamIDManager::GetSteamID64();

        return SteamCallResultManager::CreateCallResult(1317, &result, sizeof(result), true);
    }

    virtual bool GetUGCDownloadProgress(UGCHandle_t content, int32_t* downloaded, int32_t* expected)
    {
        TraceRemoteStorage("GetUGCDownloadProgress", "content=" + std::to_string(content));
        NSR_UNUSED(content);
        Logger::Info("SteamRemoteStorage::GetUGCDownloadProgress");

        if (downloaded) *downloaded = 0;
        if (expected) *expected = 0;

        return false;
    }

    virtual bool GetUGCDetails(UGCHandle_t content, AppId_t* appID, char** name, int32_t* fileSize, CSteamID* owner)
    {
        TraceRemoteStorage("GetUGCDetails", "content=" + std::to_string(content));
        NSR_UNUSED(content);
        Logger::Info("SteamRemoteStorage::GetUGCDetails");

        if (appID) *appID = kOfflineAppID;
        if (name) *name = const_cast<char*>("");
        if (fileSize) *fileSize = 0;
        if (owner) *owner = SteamIDManager::GetLocalSteamID();

        return false;
    }

    virtual int32_t UGCRead(UGCHandle_t content, void* data, int32_t size, uint32_t offset, int action)
    {
        TraceRemoteStorage(
            "UGCRead",
            "content=" + std::to_string(content) +
            " bytes=" + std::to_string(size) +
            " offset=" + std::to_string(offset));
        NSR_UNUSED(content);
        NSR_UNUSED(offset);
        NSR_UNUSED(action);
        Logger::Info("SteamRemoteStorage::UGCRead");

        if (data && size > 0)
            SafeZeroBuffer(data, static_cast<size_t>(size), "UGCRead");

        return 0;
    }

    virtual int32_t GetCachedUGCCount()
    {
        TraceRemoteStorage("GetCachedUGCCount");
        Logger::Info("SteamRemoteStorage::GetCachedUGCCount");
        return 0;
    }

    virtual UGCHandle_t GetCachedUGCHandle(int32_t index)
    {
        TraceRemoteStorage("GetCachedUGCHandle", "index=" + std::to_string(index));
        NSR_UNUSED(index);
        Logger::Info("SteamRemoteStorage::GetCachedUGCHandle");
        return kInvalidUGCHandle;
    }

    virtual SteamAPICall_t PublishWorkshopFile(const char* file, const char* previewFile, AppId_t appID, const char* title, const char* description, int visibility, void* tags, int fileType)
    {
        TraceRemoteStorage("PublishWorkshopFile", FileDetail(file) + " app=" + std::to_string(appID));
        NSR_UNUSED(file);
        NSR_UNUSED(previewFile);
        NSR_UNUSED(appID);
        NSR_UNUSED(title);
        NSR_UNUSED(description);
        NSR_UNUSED(visibility);
        NSR_UNUSED(tags);
        NSR_UNUSED(fileType);
        Logger::Info("SteamRemoteStorage::PublishWorkshopFile");
        return EmptyCallResult(1329);
    }

    virtual PublishedFileUpdateHandle_t CreatePublishedFileUpdateRequest(PublishedFileId_t publishedFileID)
    {
        TraceRemoteStorage("CreatePublishedFileUpdateRequest", "fileID=" + std::to_string(publishedFileID));
        NSR_UNUSED(publishedFileID);
        Logger::Info("SteamRemoteStorage::CreatePublishedFileUpdateRequest");
        return kInvalidPublishedFileUpdateHandle;
    }

    virtual bool UpdatePublishedFileFile(PublishedFileUpdateHandle_t updateHandle, const char* file)
    {
        TraceRemoteStorage("UpdatePublishedFileFile", "handle=" + std::to_string(updateHandle) + " " + FileDetail(file));
        NSR_UNUSED(updateHandle);
        NSR_UNUSED(file);
        Logger::Info("SteamRemoteStorage::UpdatePublishedFileFile");
        return true;
    }

    virtual bool UpdatePublishedFilePreviewFile(PublishedFileUpdateHandle_t updateHandle, const char* previewFile)
    {
        TraceRemoteStorage("UpdatePublishedFilePreviewFile", "handle=" + std::to_string(updateHandle) + " " + FileDetail(previewFile));
        NSR_UNUSED(updateHandle);
        NSR_UNUSED(previewFile);
        Logger::Info("SteamRemoteStorage::UpdatePublishedFilePreviewFile");
        return true;
    }

    virtual bool UpdatePublishedFileTitle(PublishedFileUpdateHandle_t updateHandle, const char* title)
    {
        TraceRemoteStorage("UpdatePublishedFileTitle", "handle=" + std::to_string(updateHandle) + " title=" + std::string(title ? title : "null"));
        NSR_UNUSED(updateHandle);
        NSR_UNUSED(title);
        Logger::Info("SteamRemoteStorage::UpdatePublishedFileTitle");
        return true;
    }

    virtual bool UpdatePublishedFileDescription(PublishedFileUpdateHandle_t updateHandle, const char* description)
    {
        TraceRemoteStorage("UpdatePublishedFileDescription", "handle=" + std::to_string(updateHandle));
        NSR_UNUSED(updateHandle);
        NSR_UNUSED(description);
        Logger::Info("SteamRemoteStorage::UpdatePublishedFileDescription");
        return true;
    }

    virtual bool UpdatePublishedFileVisibility(PublishedFileUpdateHandle_t updateHandle, int visibility)
    {
        TraceRemoteStorage("UpdatePublishedFileVisibility", "handle=" + std::to_string(updateHandle) + " visibility=" + std::to_string(visibility));
        NSR_UNUSED(updateHandle);
        NSR_UNUSED(visibility);
        Logger::Info("SteamRemoteStorage::UpdatePublishedFileVisibility");
        return true;
    }

    virtual bool UpdatePublishedFileTags(PublishedFileUpdateHandle_t updateHandle, void* tags)
    {
        TraceRemoteStorage("UpdatePublishedFileTags", "handle=" + std::to_string(updateHandle));
        NSR_UNUSED(updateHandle);
        NSR_UNUSED(tags);
        Logger::Info("SteamRemoteStorage::UpdatePublishedFileTags");
        return true;
    }

    virtual SteamAPICall_t CommitPublishedFileUpdate(PublishedFileUpdateHandle_t updateHandle)
    {
        TraceRemoteStorage("CommitPublishedFileUpdate", "handle=" + std::to_string(updateHandle));
        NSR_UNUSED(updateHandle);
        Logger::Info("SteamRemoteStorage::CommitPublishedFileUpdate");
        return EmptyCallResult(1316);
    }

    virtual SteamAPICall_t GetPublishedFileDetails(PublishedFileId_t publishedFileID, uint32_t maxSecondsOld)
    {
        TraceRemoteStorage("GetPublishedFileDetails", "fileID=" + std::to_string(publishedFileID) + " maxAge=" + std::to_string(maxSecondsOld));
        NSR_UNUSED(publishedFileID);
        NSR_UNUSED(maxSecondsOld);
        Logger::Info("SteamRemoteStorage::GetPublishedFileDetails");
        return EmptyCallResult(1318);
    }

    virtual SteamAPICall_t DeletePublishedFile(PublishedFileId_t publishedFileID)
    {
        TraceRemoteStorage("DeletePublishedFile", "fileID=" + std::to_string(publishedFileID));
        NSR_UNUSED(publishedFileID);
        Logger::Info("SteamRemoteStorage::DeletePublishedFile");
        return EmptyCallResult(1311);
    }

    virtual SteamAPICall_t EnumerateUserPublishedFiles(uint32_t startIndex)
    {
        TraceRemoteStorage("EnumerateUserPublishedFiles", "start=" + std::to_string(startIndex));
        NSR_UNUSED(startIndex);
        Logger::Info("SteamRemoteStorage::EnumerateUserPublishedFiles");
        return EmptyCallResult(1312);
    }

    virtual SteamAPICall_t SubscribePublishedFile(PublishedFileId_t publishedFileID)
    {
        TraceRemoteStorage("SubscribePublishedFile", "fileID=" + std::to_string(publishedFileID));
        NSR_UNUSED(publishedFileID);
        Logger::Info("SteamRemoteStorage::SubscribePublishedFile");
        return EmptyCallResult(1313);
    }

    virtual SteamAPICall_t EnumerateUserSubscribedFiles(uint32_t startIndex)
    {
        TraceRemoteStorage("EnumerateUserSubscribedFiles", "start=" + std::to_string(startIndex));
        NSR_UNUSED(startIndex);
        Logger::Info("SteamRemoteStorage::EnumerateUserSubscribedFiles");
        return EmptyCallResult(1314);
    }

    virtual SteamAPICall_t UnsubscribePublishedFile(PublishedFileId_t publishedFileID)
    {
        TraceRemoteStorage("UnsubscribePublishedFile", "fileID=" + std::to_string(publishedFileID));
        NSR_UNUSED(publishedFileID);
        Logger::Info("SteamRemoteStorage::UnsubscribePublishedFile");
        return EmptyCallResult(1315);
    }

    virtual bool UpdatePublishedFileSetChangeDescription(PublishedFileUpdateHandle_t updateHandle, const char* changeDescription)
    {
        TraceRemoteStorage("UpdatePublishedFileSetChangeDescription", "handle=" + std::to_string(updateHandle));
        NSR_UNUSED(updateHandle);
        NSR_UNUSED(changeDescription);
        Logger::Info("SteamRemoteStorage::UpdatePublishedFileSetChangeDescription");
        return true;
    }

    virtual SteamAPICall_t GetPublishedItemVoteDetails(PublishedFileId_t publishedFileID)
    {
        TraceRemoteStorage("GetPublishedItemVoteDetails", "fileID=" + std::to_string(publishedFileID));
        NSR_UNUSED(publishedFileID);
        Logger::Info("SteamRemoteStorage::GetPublishedItemVoteDetails");
        return EmptyCallResult(1320);
    }

    virtual SteamAPICall_t UpdateUserPublishedItemVote(PublishedFileId_t publishedFileID, bool voteUp)
    {
        TraceRemoteStorage("UpdateUserPublishedItemVote", "fileID=" + std::to_string(publishedFileID) + " up=" + (voteUp ? "1" : "0"));
        NSR_UNUSED(publishedFileID);
        NSR_UNUSED(voteUp);
        Logger::Info("SteamRemoteStorage::UpdateUserPublishedItemVote");
        return EmptyCallResult(1324);
    }

    virtual SteamAPICall_t GetUserPublishedItemVoteDetails(PublishedFileId_t publishedFileID)
    {
        TraceRemoteStorage("GetUserPublishedItemVoteDetails", "fileID=" + std::to_string(publishedFileID));
        NSR_UNUSED(publishedFileID);
        Logger::Info("SteamRemoteStorage::GetUserPublishedItemVoteDetails");
        return EmptyCallResult(1325);
    }

    virtual SteamAPICall_t EnumerateUserSharedWorkshopFiles(CSteamID steamID, uint32_t startIndex, void* requiredTags, void* excludedTags)
    {
        TraceRemoteStorage("EnumerateUserSharedWorkshopFiles", "start=" + std::to_string(startIndex));
        NSR_UNUSED(steamID);
        NSR_UNUSED(startIndex);
        NSR_UNUSED(requiredTags);
        NSR_UNUSED(excludedTags);
        Logger::Info("SteamRemoteStorage::EnumerateUserSharedWorkshopFiles");
        return EmptyCallResult(1326);
    }

    virtual SteamAPICall_t PublishVideo(int videoProvider, const char* videoAccount, const char* videoIdentifier, const char* previewFile, AppId_t appID, const char* title, const char* description, int visibility, void* tags)
    {
        TraceRemoteStorage("PublishVideo", "app=" + std::to_string(appID));
        NSR_UNUSED(videoProvider);
        NSR_UNUSED(videoAccount);
        NSR_UNUSED(videoIdentifier);
        NSR_UNUSED(previewFile);
        NSR_UNUSED(appID);
        NSR_UNUSED(title);
        NSR_UNUSED(description);
        NSR_UNUSED(visibility);
        NSR_UNUSED(tags);
        Logger::Info("SteamRemoteStorage::PublishVideo");
        return EmptyCallResult(1329);
    }

    virtual SteamAPICall_t SetUserPublishedFileAction(PublishedFileId_t publishedFileID, int action)
    {
        TraceRemoteStorage("SetUserPublishedFileAction", "fileID=" + std::to_string(publishedFileID) + " action=" + std::to_string(action));
        NSR_UNUSED(publishedFileID);
        NSR_UNUSED(action);
        Logger::Info("SteamRemoteStorage::SetUserPublishedFileAction");
        return EmptyCallResult(1327);
    }

    virtual SteamAPICall_t EnumeratePublishedFilesByUserAction(int action, uint32_t startIndex)
    {
        TraceRemoteStorage("EnumeratePublishedFilesByUserAction", "action=" + std::to_string(action) + " start=" + std::to_string(startIndex));
        NSR_UNUSED(action);
        NSR_UNUSED(startIndex);
        Logger::Info("SteamRemoteStorage::EnumeratePublishedFilesByUserAction");
        return EmptyCallResult(1328);
    }

    virtual SteamAPICall_t EnumeratePublishedWorkshopFiles(int enumerationType, uint32_t startIndex, uint32_t count, uint32_t days, void* tags, void* userTags)
    {
        TraceRemoteStorage(
            "EnumeratePublishedWorkshopFiles",
            "type=" + std::to_string(enumerationType) +
            " start=" + std::to_string(startIndex) +
            " count=" + std::to_string(count));
        NSR_UNUSED(enumerationType);
        NSR_UNUSED(startIndex);
        NSR_UNUSED(count);
        NSR_UNUSED(days);
        NSR_UNUSED(tags);
        NSR_UNUSED(userTags);
        Logger::Info("SteamRemoteStorage::EnumeratePublishedWorkshopFiles");
        return EmptyCallResult(1319);
    }

    virtual SteamAPICall_t UGCDownloadToLocation(UGCHandle_t content, const char* location, uint32_t priority)
    {
        TraceRemoteStorage(
            "UGCDownloadToLocation",
            "content=" + std::to_string(content) +
            " location=" + std::string(location ? location : "null"));
        NSR_UNUSED(location);
        return UGCDownload(content, priority);
    }

    virtual int32_t GetLocalFileChangeCount()
    {
        TraceRemoteStorage("GetLocalFileChangeCount");
        Logger::Info("SteamRemoteStorage::GetLocalFileChangeCount");
        return 0;
    }

    virtual const char* GetLocalFileChange(int file, int* changeType, int* filePathType)
    {
        TraceRemoteStorage("GetLocalFileChange", "index=" + std::to_string(file));
        NSR_UNUSED(file);
        Logger::Info("SteamRemoteStorage::GetLocalFileChange");

        if (changeType) *changeType = 0;
        if (filePathType) *filePathType = 0;

        return "";
    }

    virtual bool BeginFileWriteBatch()
    {
        TraceRemoteStorage("BeginFileWriteBatch");
        Logger::Info("SteamRemoteStorage::BeginFileWriteBatch");
        return true;
    }

    virtual bool EndFileWriteBatch()
    {
        TraceRemoteStorage("EndFileWriteBatch");
        Logger::Info("SteamRemoteStorage::EndFileWriteBatch");
        return true;
    }
};

static FakeSteamRemoteStorage g_Interface;

class FakeSteamRemoteStorage001 final
{
public:
    virtual bool FileWrite(const char* file, const void* data, int32_t size) { return g_Interface.FileWrite(file, data, size); }
    virtual int32_t GetFileSize(const char* file) { return g_Interface.GetFileSize(file); }
    virtual int32_t FileRead(const char* file, void* data, int32_t size) { return g_Interface.FileRead(file, data, size); }
    virtual bool FileExists(const char* file) { return g_Interface.FileExists(file); }
    virtual bool FileDelete(const char* file) { return g_Interface.FileDelete(file); }
    virtual int32_t GetFileCount() { return g_Interface.GetFileCount(); }
    virtual const char* GetFileNameAndSize(int index, int32_t* size) { return g_Interface.GetFileNameAndSize(index, size); }
    virtual bool GetQuota(int32_t* total, int32_t* available) { return GetQuota32(total, available); }
};

class FakeSteamRemoteStorage002 final
{
public:
    virtual bool FileWrite(const char* file, const void* data, int32_t size) { return g_Interface.FileWrite(file, data, size); }
    virtual int32_t GetFileSize(const char* file) { return g_Interface.GetFileSize(file); }
    virtual int32_t FileRead(const char* file, void* data, int32_t size) { return g_Interface.FileRead(file, data, size); }
    virtual bool FileExists(const char* file) { return g_Interface.FileExists(file); }
    virtual int32_t GetFileCount() { return g_Interface.GetFileCount(); }
    virtual const char* GetFileNameAndSize(int index, int32_t* size) { return g_Interface.GetFileNameAndSize(index, size); }
    virtual bool GetQuota(int32_t* total, int32_t* available) { return GetQuota32(total, available); }
};

class FakeSteamRemoteStorage003 final
{
public:
    virtual bool FileWrite(const char* file, const void* data, int32_t size) { return g_Interface.FileWrite(file, data, size); }
    virtual int32_t FileRead(const char* file, void* data, int32_t size) { return g_Interface.FileRead(file, data, size); }
    virtual bool FileForget(const char* file) { return g_Interface.FileForget(file); }
    virtual bool FileDelete(const char* file) { return g_Interface.FileDelete(file); }
    virtual SteamAPICall_t FileShare(const char* file) { return g_Interface.FileShare(file); }
    virtual bool FileExists(const char* file) { return g_Interface.FileExists(file); }
    virtual bool FilePersisted(const char* file) { return g_Interface.FilePersisted(file); }
    virtual int32_t GetFileSize(const char* file) { return g_Interface.GetFileSize(file); }
    virtual int64_t GetFileTimestamp(const char* file) { return g_Interface.GetFileTimestamp(file); }
    virtual int32_t GetFileCount() { return g_Interface.GetFileCount(); }
    virtual const char* GetFileNameAndSize(int index, int32_t* size) { return g_Interface.GetFileNameAndSize(index, size); }
    virtual bool GetQuota(int32_t* total, int32_t* available) { return GetQuota32(total, available); }
    virtual bool IsCloudEnabledForAccount() { return g_Interface.IsCloudEnabledForAccount(); }
    virtual bool IsCloudEnabledForApp() { return g_Interface.IsCloudEnabledForApp(); }
    virtual void SetCloudEnabledForApp(bool enabled) { g_Interface.SetCloudEnabledForApp(enabled); }
    virtual SteamAPICall_t UGCDownload(UGCHandle_t content) { return g_Interface.UGCDownload(content, 0); }
    virtual bool GetUGCDetails(UGCHandle_t content, AppId_t* appID, char** name, int32_t* fileSize, CSteamID* owner) { return g_Interface.GetUGCDetails(content, appID, name, fileSize, owner); }
    virtual int32_t UGCRead(UGCHandle_t content, void* data, int32_t size) { return g_Interface.UGCRead(content, data, size, 0, 0); }
    virtual int32_t GetCachedUGCCount() { return g_Interface.GetCachedUGCCount(); }
    virtual UGCHandle_t GetCachedUGCHandle(int32_t index) { return g_Interface.GetCachedUGCHandle(index); }
};

class FakeSteamRemoteStorage005
{
public:
    virtual bool FileWrite(const char* file, const void* data, int32_t size) { return g_Interface.FileWrite(file, data, size); }
    virtual int32_t FileRead(const char* file, void* data, int32_t size) { return g_Interface.FileRead(file, data, size); }
    virtual bool FileForget(const char* file) { return g_Interface.FileForget(file); }
    virtual bool FileDelete(const char* file) { return g_Interface.FileDelete(file); }
    virtual SteamAPICall_t FileShare(const char* file) { return g_Interface.FileShare(file); }
    virtual bool SetSyncPlatforms(const char* file, int platforms) { return g_Interface.SetSyncPlatforms(file, platforms); }
    virtual bool FileExists(const char* file) { return g_Interface.FileExists(file); }
    virtual bool FilePersisted(const char* file) { return g_Interface.FilePersisted(file); }
    virtual int32_t GetFileSize(const char* file) { return g_Interface.GetFileSize(file); }
    virtual int64_t GetFileTimestamp(const char* file) { return g_Interface.GetFileTimestamp(file); }
    virtual int GetSyncPlatforms(const char* file) { return g_Interface.GetSyncPlatforms(file); }
    virtual int32_t GetFileCount() { return g_Interface.GetFileCount(); }
    virtual const char* GetFileNameAndSize(int index, int32_t* size) { return g_Interface.GetFileNameAndSize(index, size); }
    virtual bool GetQuota(int32_t* total, int32_t* available) { return GetQuota32(total, available); }
    virtual bool IsCloudEnabledForAccount() { return g_Interface.IsCloudEnabledForAccount(); }
    virtual bool IsCloudEnabledForApp() { return g_Interface.IsCloudEnabledForApp(); }
    virtual void SetCloudEnabledForApp(bool enabled) { g_Interface.SetCloudEnabledForApp(enabled); }
    virtual SteamAPICall_t UGCDownload(UGCHandle_t content) { return g_Interface.UGCDownload(content, 0); }
    virtual bool GetUGCDetails(UGCHandle_t content, AppId_t* appID, char** name, int32_t* fileSize, CSteamID* owner) { return g_Interface.GetUGCDetails(content, appID, name, fileSize, owner); }
    virtual int32_t UGCRead(UGCHandle_t content, void* data, int32_t size) { return g_Interface.UGCRead(content, data, size, 0, 0); }
    virtual int32_t GetCachedUGCCount() { return g_Interface.GetCachedUGCCount(); }
    virtual UGCHandle_t GetCachedUGCHandle(int32_t index) { return g_Interface.GetCachedUGCHandle(index); }
    virtual void GetFileListFromServer() { TraceRemoteStorage("GetFileListFromServer"); }
    virtual bool FileFetch(const char* file) { TraceRemoteStorage("FileFetch", FileDetail(file)); NSR_UNUSED(file); return true; }
    virtual bool FilePersist(const char* file) { return g_Interface.FileExists(file); }
    virtual bool SynchronizeToClient() { TraceRemoteStorage("SynchronizeToClient"); return true; }
    virtual bool SynchronizeToServer() { TraceRemoteStorage("SynchronizeToServer"); return true; }
    virtual bool ResetFileRequestState() { TraceRemoteStorage("ResetFileRequestState"); return true; }
    virtual SteamAPICall_t PublishFile(const char* file, const char* previewFile, AppId_t appID, const char* title, const char* description, int visibility, void* tags) { return g_Interface.PublishWorkshopFile(file, previewFile, appID, title, description, visibility, tags, 0); }
    virtual SteamAPICall_t PublishWorkshopFile(const char* file, const char* previewFile, AppId_t appID, const char* title, const char* description, void* tags) { return g_Interface.PublishWorkshopFile(file, previewFile, appID, title, description, 0, tags, 0); }
    virtual SteamAPICall_t UpdatePublishedFile(void* request) { NSR_UNUSED(request); return EmptyCallResult(1316); }
    virtual SteamAPICall_t GetPublishedFileDetails(PublishedFileId_t fileID) { return g_Interface.GetPublishedFileDetails(fileID, 0); }
    virtual SteamAPICall_t DeletePublishedFile(PublishedFileId_t fileID) { return g_Interface.DeletePublishedFile(fileID); }
    virtual SteamAPICall_t EnumerateUserPublishedFiles(uint32_t startIndex) { return g_Interface.EnumerateUserPublishedFiles(startIndex); }
    virtual SteamAPICall_t SubscribePublishedFile(PublishedFileId_t fileID) { return g_Interface.SubscribePublishedFile(fileID); }
    virtual SteamAPICall_t EnumerateUserSubscribedFiles(uint32_t startIndex) { return g_Interface.EnumerateUserSubscribedFiles(startIndex); }
    virtual SteamAPICall_t UnsubscribePublishedFile(PublishedFileId_t fileID) { return g_Interface.UnsubscribePublishedFile(fileID); }
};

class FakeSteamRemoteStorage007 : public FakeSteamRemoteStorage005
{
public:
    virtual bool GetUGCDownloadProgress(UGCHandle_t content, int32_t* downloaded, int32_t* expected) { return UGCDownloadProgress32(content, downloaded, expected); }
    virtual SteamAPICall_t PublishWorkshopFileV2(const char* file, const char* previewFile, AppId_t appID, const char* title, const char* description, int visibility, void* tags, int fileType) { return g_Interface.PublishWorkshopFile(file, previewFile, appID, title, description, visibility, tags, fileType); }
    virtual PublishedFileUpdateHandle_t CreatePublishedFileUpdateRequest(PublishedFileId_t fileID) { return g_Interface.CreatePublishedFileUpdateRequest(fileID); }
    virtual bool UpdatePublishedFileFile(PublishedFileUpdateHandle_t handle, const char* file) { return g_Interface.UpdatePublishedFileFile(handle, file); }
    virtual bool UpdatePublishedFilePreviewFile(PublishedFileUpdateHandle_t handle, const char* previewFile) { return g_Interface.UpdatePublishedFilePreviewFile(handle, previewFile); }
    virtual bool UpdatePublishedFileTitle(PublishedFileUpdateHandle_t handle, const char* title) { return g_Interface.UpdatePublishedFileTitle(handle, title); }
    virtual bool UpdatePublishedFileDescription(PublishedFileUpdateHandle_t handle, const char* description) { return g_Interface.UpdatePublishedFileDescription(handle, description); }
    virtual bool UpdatePublishedFileVisibility(PublishedFileUpdateHandle_t handle, int visibility) { return g_Interface.UpdatePublishedFileVisibility(handle, visibility); }
    virtual bool UpdatePublishedFileTags(PublishedFileUpdateHandle_t handle, void* tags) { return g_Interface.UpdatePublishedFileTags(handle, tags); }
    virtual SteamAPICall_t CommitPublishedFileUpdate(PublishedFileUpdateHandle_t handle) { return g_Interface.CommitPublishedFileUpdate(handle); }
    virtual bool UpdatePublishedFileSetChangeDescription(PublishedFileUpdateHandle_t handle, const char* changeDescription) { return g_Interface.UpdatePublishedFileSetChangeDescription(handle, changeDescription); }
    virtual SteamAPICall_t GetPublishedItemVoteDetails(PublishedFileId_t fileID) { return g_Interface.GetPublishedItemVoteDetails(fileID); }
    virtual SteamAPICall_t UpdateUserPublishedItemVote(PublishedFileId_t fileID, bool voteUp) { return g_Interface.UpdateUserPublishedItemVote(fileID, voteUp); }
    virtual SteamAPICall_t GetUserPublishedItemVoteDetails(PublishedFileId_t fileID) { return g_Interface.GetUserPublishedItemVoteDetails(fileID); }
    virtual SteamAPICall_t EnumerateUserSharedWorkshopFiles(CSteamID steamID, uint32_t startIndex, void* requiredTags, void* excludedTags) { return g_Interface.EnumerateUserSharedWorkshopFiles(steamID, startIndex, requiredTags, excludedTags); }
    virtual SteamAPICall_t PublishVideo(int provider, const char* account, const char* identifier, const char* previewFile, AppId_t appID, const char* title, const char* description, int visibility, void* tags) { return g_Interface.PublishVideo(provider, account, identifier, previewFile, appID, title, description, visibility, tags); }
    virtual SteamAPICall_t SetUserPublishedFileAction(PublishedFileId_t fileID, int action) { return g_Interface.SetUserPublishedFileAction(fileID, action); }
    virtual SteamAPICall_t EnumeratePublishedFilesByUserAction(int action, uint32_t startIndex) { return g_Interface.EnumeratePublishedFilesByUserAction(action, startIndex); }
    virtual SteamAPICall_t EnumeratePublishedWorkshopFiles(int type, uint32_t startIndex, uint32_t count, uint32_t days, void* tags, void* userTags) { return g_Interface.EnumeratePublishedWorkshopFiles(type, startIndex, count, days, tags, userTags); }
};

class FakeSteamRemoteStorage009
{
public:
    virtual bool FileWrite(const char* file, const void* data, int32_t size) { return g_Interface.FileWrite(file, data, size); }
    virtual int32_t FileRead(const char* file, void* data, int32_t size) { return g_Interface.FileRead(file, data, size); }
    virtual bool FileForget(const char* file) { return g_Interface.FileForget(file); }
    virtual bool FileDelete(const char* file) { return g_Interface.FileDelete(file); }
    virtual SteamAPICall_t FileShare(const char* file) { return g_Interface.FileShare(file); }
    virtual bool SetSyncPlatforms(const char* file, int platforms) { return g_Interface.SetSyncPlatforms(file, platforms); }
    virtual UGCFileWriteStreamHandle_t FileWriteStreamOpen(const char* file) { return g_Interface.FileWriteStreamOpen(file); }
    virtual bool FileWriteStreamWriteChunk(UGCFileWriteStreamHandle_t handle, const void* data, int32_t size) { return g_Interface.FileWriteStreamWriteChunk(handle, data, size); }
    virtual bool FileWriteStreamClose(UGCFileWriteStreamHandle_t handle) { return g_Interface.FileWriteStreamClose(handle); }
    virtual bool FileWriteStreamCancel(UGCFileWriteStreamHandle_t handle) { return g_Interface.FileWriteStreamCancel(handle); }
    virtual bool FileExists(const char* file) { return g_Interface.FileExists(file); }
    virtual bool FilePersisted(const char* file) { return g_Interface.FilePersisted(file); }
    virtual int32_t GetFileSize(const char* file) { return g_Interface.GetFileSize(file); }
    virtual int64_t GetFileTimestamp(const char* file) { return g_Interface.GetFileTimestamp(file); }
    virtual int GetSyncPlatforms(const char* file) { return g_Interface.GetSyncPlatforms(file); }
    virtual int32_t GetFileCount() { return g_Interface.GetFileCount(); }
    virtual const char* GetFileNameAndSize(int index, int32_t* size) { return g_Interface.GetFileNameAndSize(index, size); }
    virtual bool GetQuota(int32_t* total, int32_t* available) { return GetQuota32(total, available); }
    virtual bool IsCloudEnabledForAccount() { return g_Interface.IsCloudEnabledForAccount(); }
    virtual bool IsCloudEnabledForApp() { return g_Interface.IsCloudEnabledForApp(); }
    virtual void SetCloudEnabledForApp(bool enabled) { g_Interface.SetCloudEnabledForApp(enabled); }
    virtual SteamAPICall_t UGCDownload(UGCHandle_t content) { return g_Interface.UGCDownload(content, 0); }
    virtual bool GetUGCDownloadProgress(UGCHandle_t content, int32_t* downloaded, int32_t* expected) { return UGCDownloadProgress32(content, downloaded, expected); }
    virtual bool GetUGCDetails(UGCHandle_t content, AppId_t* appID, char** name, int32_t* fileSize, CSteamID* owner) { return g_Interface.GetUGCDetails(content, appID, name, fileSize, owner); }
    virtual int32_t UGCRead(UGCHandle_t content, void* data, int32_t size, uint32_t offset) { return g_Interface.UGCRead(content, data, size, offset, 0); }
    virtual int32_t GetCachedUGCCount() { return g_Interface.GetCachedUGCCount(); }
    virtual UGCHandle_t GetCachedUGCHandle(int32_t index) { return g_Interface.GetCachedUGCHandle(index); }
    virtual SteamAPICall_t PublishWorkshopFile(const char* file, const char* previewFile, AppId_t appID, const char* title, const char* description, int visibility, void* tags, int fileType) { return g_Interface.PublishWorkshopFile(file, previewFile, appID, title, description, visibility, tags, fileType); }
    virtual PublishedFileUpdateHandle_t CreatePublishedFileUpdateRequest(PublishedFileId_t fileID) { return g_Interface.CreatePublishedFileUpdateRequest(fileID); }
    virtual bool UpdatePublishedFileFile(PublishedFileUpdateHandle_t handle, const char* file) { return g_Interface.UpdatePublishedFileFile(handle, file); }
    virtual bool UpdatePublishedFilePreviewFile(PublishedFileUpdateHandle_t handle, const char* previewFile) { return g_Interface.UpdatePublishedFilePreviewFile(handle, previewFile); }
    virtual bool UpdatePublishedFileTitle(PublishedFileUpdateHandle_t handle, const char* title) { return g_Interface.UpdatePublishedFileTitle(handle, title); }
    virtual bool UpdatePublishedFileDescription(PublishedFileUpdateHandle_t handle, const char* description) { return g_Interface.UpdatePublishedFileDescription(handle, description); }
    virtual bool UpdatePublishedFileVisibility(PublishedFileUpdateHandle_t handle, int visibility) { return g_Interface.UpdatePublishedFileVisibility(handle, visibility); }
    virtual bool UpdatePublishedFileTags(PublishedFileUpdateHandle_t handle, void* tags) { return g_Interface.UpdatePublishedFileTags(handle, tags); }
    virtual SteamAPICall_t CommitPublishedFileUpdate(PublishedFileUpdateHandle_t handle) { return g_Interface.CommitPublishedFileUpdate(handle); }
    virtual SteamAPICall_t GetPublishedFileDetails(PublishedFileId_t fileID) { return g_Interface.GetPublishedFileDetails(fileID, 0); }
    virtual SteamAPICall_t DeletePublishedFile(PublishedFileId_t fileID) { return g_Interface.DeletePublishedFile(fileID); }
    virtual SteamAPICall_t EnumerateUserPublishedFiles(uint32_t startIndex) { return g_Interface.EnumerateUserPublishedFiles(startIndex); }
    virtual SteamAPICall_t SubscribePublishedFile(PublishedFileId_t fileID) { return g_Interface.SubscribePublishedFile(fileID); }
    virtual SteamAPICall_t EnumerateUserSubscribedFiles(uint32_t startIndex) { return g_Interface.EnumerateUserSubscribedFiles(startIndex); }
    virtual SteamAPICall_t UnsubscribePublishedFile(PublishedFileId_t fileID) { return g_Interface.UnsubscribePublishedFile(fileID); }
    virtual bool UpdatePublishedFileSetChangeDescription(PublishedFileUpdateHandle_t handle, const char* changeDescription) { return g_Interface.UpdatePublishedFileSetChangeDescription(handle, changeDescription); }
    virtual SteamAPICall_t GetPublishedItemVoteDetails(PublishedFileId_t fileID) { return g_Interface.GetPublishedItemVoteDetails(fileID); }
    virtual SteamAPICall_t UpdateUserPublishedItemVote(PublishedFileId_t fileID, bool voteUp) { return g_Interface.UpdateUserPublishedItemVote(fileID, voteUp); }
    virtual SteamAPICall_t GetUserPublishedItemVoteDetails(PublishedFileId_t fileID) { return g_Interface.GetUserPublishedItemVoteDetails(fileID); }
    virtual SteamAPICall_t EnumerateUserSharedWorkshopFiles(CSteamID steamID, uint32_t startIndex, void* requiredTags, void* excludedTags) { return g_Interface.EnumerateUserSharedWorkshopFiles(steamID, startIndex, requiredTags, excludedTags); }
    virtual SteamAPICall_t PublishVideo(int provider, const char* account, const char* identifier, const char* previewFile, AppId_t appID, const char* title, const char* description, int visibility, void* tags) { return g_Interface.PublishVideo(provider, account, identifier, previewFile, appID, title, description, visibility, tags); }
    virtual SteamAPICall_t SetUserPublishedFileAction(PublishedFileId_t fileID, int action) { return g_Interface.SetUserPublishedFileAction(fileID, action); }
    virtual SteamAPICall_t EnumeratePublishedFilesByUserAction(int action, uint32_t startIndex) { return g_Interface.EnumeratePublishedFilesByUserAction(action, startIndex); }
    virtual SteamAPICall_t EnumeratePublishedWorkshopFiles(int type, uint32_t startIndex, uint32_t count, uint32_t days, void* tags, void* userTags) { return g_Interface.EnumeratePublishedWorkshopFiles(type, startIndex, count, days, tags, userTags); }
};

class FakeSteamRemoteStorage012 : public FakeSteamRemoteStorage009
{
public:
    virtual SteamAPICall_t UGCDownloadV2(UGCHandle_t content, uint32_t priority) { return g_Interface.UGCDownload(content, priority); }
    virtual int32_t UGCReadV2(UGCHandle_t content, void* data, int32_t size, uint32_t offset, int action) { return g_Interface.UGCRead(content, data, size, offset, action); }
    virtual void GetFileListFromServer() { TraceRemoteStorage("GetFileListFromServer"); }
    virtual bool FileFetch(const char* file) { TraceRemoteStorage("FileFetch", FileDetail(file)); NSR_UNUSED(file); return true; }
    virtual bool FilePersist(const char* file) { return g_Interface.FileExists(file); }
    virtual bool SynchronizeToClient() { TraceRemoteStorage("SynchronizeToClient"); return true; }
    virtual bool SynchronizeToServer() { TraceRemoteStorage("SynchronizeToServer"); return true; }
    virtual bool ResetFileRequestState() { TraceRemoteStorage("ResetFileRequestState"); return true; }
    virtual SteamAPICall_t GetPublishedFileDetailsV2(PublishedFileId_t fileID, uint32_t maxSecondsOld) { return g_Interface.GetPublishedFileDetails(fileID, maxSecondsOld); }
    virtual SteamAPICall_t UGCDownloadToLocation(UGCHandle_t content, const char* location, uint32_t priority) { return g_Interface.UGCDownloadToLocation(content, location, priority); }
};

class FakeSteamRemoteStorage013 final
{
public:
    virtual bool FileWrite(const char* file, const void* data, int32_t size) { return g_Interface.FileWrite(file, data, size); }
    virtual int32_t FileRead(const char* file, void* data, int32_t size) { return g_Interface.FileRead(file, data, size); }
    virtual SteamAPICall_t FileWriteAsync(const char* file, const void* data, uint32_t size) { return g_Interface.FileWriteAsync(file, data, size); }
    virtual SteamAPICall_t FileReadAsync(const char* file, uint32_t offset, uint32_t size) { return g_Interface.FileReadAsync(file, offset, size); }
    virtual bool FileReadAsyncComplete(SteamAPICall_t call, void* data, uint32_t size) { return g_Interface.FileReadAsyncComplete(call, data, size); }
    virtual bool FileForget(const char* file) { return g_Interface.FileForget(file); }
    virtual bool FileDelete(const char* file) { return g_Interface.FileDelete(file); }
    virtual SteamAPICall_t FileShare(const char* file) { return g_Interface.FileShare(file); }
    virtual bool SetSyncPlatforms(const char* file, int platforms) { return g_Interface.SetSyncPlatforms(file, platforms); }
    virtual UGCFileWriteStreamHandle_t FileWriteStreamOpen(const char* file) { return g_Interface.FileWriteStreamOpen(file); }
    virtual bool FileWriteStreamWriteChunk(UGCFileWriteStreamHandle_t handle, const void* data, int32_t size) { return g_Interface.FileWriteStreamWriteChunk(handle, data, size); }
    virtual bool FileWriteStreamClose(UGCFileWriteStreamHandle_t handle) { return g_Interface.FileWriteStreamClose(handle); }
    virtual bool FileWriteStreamCancel(UGCFileWriteStreamHandle_t handle) { return g_Interface.FileWriteStreamCancel(handle); }
    virtual bool FileExists(const char* file) { return g_Interface.FileExists(file); }
    virtual bool FilePersisted(const char* file) { return g_Interface.FilePersisted(file); }
    virtual int32_t GetFileSize(const char* file) { return g_Interface.GetFileSize(file); }
    virtual int64_t GetFileTimestamp(const char* file) { return g_Interface.GetFileTimestamp(file); }
    virtual int GetSyncPlatforms(const char* file) { return g_Interface.GetSyncPlatforms(file); }
    virtual int32_t GetFileCount() { return g_Interface.GetFileCount(); }
    virtual const char* GetFileNameAndSize(int index, int32_t* size) { return g_Interface.GetFileNameAndSize(index, size); }
    virtual bool GetQuota(int32_t* total, int32_t* available) { return GetQuota32(total, available); }
    virtual bool IsCloudEnabledForAccount() { return g_Interface.IsCloudEnabledForAccount(); }
    virtual bool IsCloudEnabledForApp() { return g_Interface.IsCloudEnabledForApp(); }
    virtual void SetCloudEnabledForApp(bool enabled) { g_Interface.SetCloudEnabledForApp(enabled); }
    virtual SteamAPICall_t UGCDownload(UGCHandle_t content, uint32_t priority) { return g_Interface.UGCDownload(content, priority); }
    virtual bool GetUGCDownloadProgress(UGCHandle_t content, int32_t* downloaded, int32_t* expected) { return UGCDownloadProgress32(content, downloaded, expected); }
    virtual bool GetUGCDetails(UGCHandle_t content, AppId_t* appID, char** name, int32_t* fileSize, CSteamID* owner) { return g_Interface.GetUGCDetails(content, appID, name, fileSize, owner); }
    virtual int32_t UGCRead(UGCHandle_t content, void* data, int32_t size, uint32_t offset, int action) { return g_Interface.UGCRead(content, data, size, offset, action); }
    virtual int32_t GetCachedUGCCount() { return g_Interface.GetCachedUGCCount(); }
    virtual UGCHandle_t GetCachedUGCHandle(int32_t index) { return g_Interface.GetCachedUGCHandle(index); }
    virtual void GetFileListFromServer() { TraceRemoteStorage("GetFileListFromServer"); }
    virtual bool FileFetch(const char* file) { TraceRemoteStorage("FileFetch", FileDetail(file)); NSR_UNUSED(file); return true; }
    virtual bool FilePersist(const char* file) { return g_Interface.FileExists(file); }
    virtual bool SynchronizeToClient() { TraceRemoteStorage("SynchronizeToClient"); return true; }
    virtual bool SynchronizeToServer() { TraceRemoteStorage("SynchronizeToServer"); return true; }
    virtual bool ResetFileRequestState() { TraceRemoteStorage("ResetFileRequestState"); return true; }
    virtual SteamAPICall_t PublishWorkshopFile(const char* file, const char* previewFile, AppId_t appID, const char* title, const char* description, int visibility, void* tags, int fileType) { return g_Interface.PublishWorkshopFile(file, previewFile, appID, title, description, visibility, tags, fileType); }
    virtual PublishedFileUpdateHandle_t CreatePublishedFileUpdateRequest(PublishedFileId_t fileID) { return g_Interface.CreatePublishedFileUpdateRequest(fileID); }
    virtual bool UpdatePublishedFileFile(PublishedFileUpdateHandle_t handle, const char* file) { return g_Interface.UpdatePublishedFileFile(handle, file); }
    virtual bool UpdatePublishedFilePreviewFile(PublishedFileUpdateHandle_t handle, const char* previewFile) { return g_Interface.UpdatePublishedFilePreviewFile(handle, previewFile); }
    virtual bool UpdatePublishedFileTitle(PublishedFileUpdateHandle_t handle, const char* title) { return g_Interface.UpdatePublishedFileTitle(handle, title); }
    virtual bool UpdatePublishedFileDescription(PublishedFileUpdateHandle_t handle, const char* description) { return g_Interface.UpdatePublishedFileDescription(handle, description); }
    virtual bool UpdatePublishedFileVisibility(PublishedFileUpdateHandle_t handle, int visibility) { return g_Interface.UpdatePublishedFileVisibility(handle, visibility); }
    virtual bool UpdatePublishedFileTags(PublishedFileUpdateHandle_t handle, void* tags) { return g_Interface.UpdatePublishedFileTags(handle, tags); }
    virtual SteamAPICall_t CommitPublishedFileUpdate(PublishedFileUpdateHandle_t handle) { return g_Interface.CommitPublishedFileUpdate(handle); }
    virtual SteamAPICall_t GetPublishedFileDetails(PublishedFileId_t fileID, uint32_t maxSecondsOld) { return g_Interface.GetPublishedFileDetails(fileID, maxSecondsOld); }
    virtual SteamAPICall_t DeletePublishedFile(PublishedFileId_t fileID) { return g_Interface.DeletePublishedFile(fileID); }
    virtual SteamAPICall_t EnumerateUserPublishedFiles(uint32_t startIndex) { return g_Interface.EnumerateUserPublishedFiles(startIndex); }
    virtual SteamAPICall_t SubscribePublishedFile(PublishedFileId_t fileID) { return g_Interface.SubscribePublishedFile(fileID); }
    virtual SteamAPICall_t EnumerateUserSubscribedFiles(uint32_t startIndex) { return g_Interface.EnumerateUserSubscribedFiles(startIndex); }
    virtual SteamAPICall_t UnsubscribePublishedFile(PublishedFileId_t fileID) { return g_Interface.UnsubscribePublishedFile(fileID); }
    virtual bool UpdatePublishedFileSetChangeDescription(PublishedFileUpdateHandle_t handle, const char* changeDescription) { return g_Interface.UpdatePublishedFileSetChangeDescription(handle, changeDescription); }
    virtual SteamAPICall_t GetPublishedItemVoteDetails(PublishedFileId_t fileID) { return g_Interface.GetPublishedItemVoteDetails(fileID); }
    virtual SteamAPICall_t UpdateUserPublishedItemVote(PublishedFileId_t fileID, bool voteUp) { return g_Interface.UpdateUserPublishedItemVote(fileID, voteUp); }
    virtual SteamAPICall_t GetUserPublishedItemVoteDetails(PublishedFileId_t fileID) { return g_Interface.GetUserPublishedItemVoteDetails(fileID); }
    virtual SteamAPICall_t EnumerateUserSharedWorkshopFiles(CSteamID steamID, uint32_t startIndex, void* requiredTags, void* excludedTags) { return g_Interface.EnumerateUserSharedWorkshopFiles(steamID, startIndex, requiredTags, excludedTags); }
    virtual SteamAPICall_t PublishVideo(int provider, const char* account, const char* identifier, const char* previewFile, AppId_t appID, const char* title, const char* description, int visibility, void* tags) { return g_Interface.PublishVideo(provider, account, identifier, previewFile, appID, title, description, visibility, tags); }
    virtual SteamAPICall_t SetUserPublishedFileAction(PublishedFileId_t fileID, int action) { return g_Interface.SetUserPublishedFileAction(fileID, action); }
    virtual SteamAPICall_t EnumeratePublishedFilesByUserAction(int action, uint32_t startIndex) { return g_Interface.EnumeratePublishedFilesByUserAction(action, startIndex); }
    virtual SteamAPICall_t EnumeratePublishedWorkshopFiles(int type, uint32_t startIndex, uint32_t count, uint32_t days, void* tags, void* userTags) { return g_Interface.EnumeratePublishedWorkshopFiles(type, startIndex, count, days, tags, userTags); }
    virtual SteamAPICall_t UGCDownloadToLocation(UGCHandle_t content, const char* location, uint32_t priority) { return g_Interface.UGCDownloadToLocation(content, location, priority); }
};

static FakeSteamRemoteStorage001 g_Interface001;
static FakeSteamRemoteStorage002 g_Interface002;
static FakeSteamRemoteStorage003 g_Interface003;
static FakeSteamRemoteStorage005 g_Interface005;
static FakeSteamRemoteStorage007 g_Interface007;
static FakeSteamRemoteStorage009 g_Interface009;
static FakeSteamRemoteStorage012 g_Interface012;
static FakeSteamRemoteStorage013 g_Interface013;

void* FakeSteamInterfaces::RemoteStorage()
{
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::RemoteStorage", "current");
    Logger::Info("SteamRemoteStorage: local save interface returned");
    return &g_Interface;
}

void* FakeSteamInterfaces::RemoteStorageForVersion(const char* version)
{
    const std::string value = version ? version : "";
    SteamVersionLogger::LogInterfaceRequest("FakeSteamInterfaces::RemoteStorageForVersion", value.c_str());

    if (value.find("001") != std::string::npos)
    {
        Logger::Info("SteamRemoteStorage001: local save interface returned");
        return &g_Interface001;
    }

    if (value.find("002") != std::string::npos)
    {
        Logger::Info("SteamRemoteStorage002: local save interface returned");
        return &g_Interface002;
    }

    if (value.find("003") != std::string::npos)
    {
        Logger::Info("SteamRemoteStorage003: local save interface returned");
        return &g_Interface003;
    }

    if (value.find("004") != std::string::npos || value.find("005") != std::string::npos)
    {
        Logger::Info("SteamRemoteStorage004/005: local save interface returned");
        return &g_Interface005;
    }

    if (value.find("006") != std::string::npos || value.find("007") != std::string::npos)
    {
        Logger::Info("SteamRemoteStorage006/007: local save interface returned");
        return &g_Interface007;
    }

    if (value.find("008") != std::string::npos || value.find("009") != std::string::npos)
    {
        Logger::Info("SteamRemoteStorage008/009: local save interface returned");
        return &g_Interface009;
    }

    if (value.find("010") != std::string::npos ||
        value.find("011") != std::string::npos ||
        value.find("012") != std::string::npos)
    {
        Logger::Info("SteamRemoteStorage010/011/012: local save interface returned");
        return &g_Interface012;
    }

    if (value.find("013") != std::string::npos)
    {
        Logger::Info("SteamRemoteStorage013: local save interface returned");
        return &g_Interface013;
    }

    Logger::Info("SteamRemoteStorage current: local save interface returned");
    return &g_Interface;
}
