#pragma once
#include "StdInc.h"

namespace SteamStorageLocal
{
    void Init();

    bool WriteFile(const char* name, const void* data, int32_t size);
    int32_t ReadFile(const char* name, void* data, int32_t maxSize);
    bool Exists(const char* name);
    bool DeleteFileLocal(const char* name);
    int32_t Size(const char* name);
    int64_t Timestamp(const char* name);
    int32_t FileCount();
    const char* FileNameAndSize(int index, int32_t* size);
}
