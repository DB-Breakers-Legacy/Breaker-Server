#pragma once
#include "StdInc.h"

namespace SteamCallResultManager
{
    struct CallResultRecord
    {
        SteamAPICall_t Call = 0;
        int CallbackID = 0;
        std::vector<unsigned char> Data;
        bool Completed = true;
    };

    bool Init();
    void Shutdown();

    SteamAPICall_t CreateRaw(int callbackID, const void* data = nullptr, size_t size = 0);

    SteamAPICall_t CreateCall(int callbackID, const void* data = nullptr, size_t size = 0);
    SteamAPICall_t CreateCallResult(int callbackID);
    SteamAPICall_t CreateCallResult(int callbackID, const void* data, size_t size);
    SteamAPICall_t CreateCallResult(int callbackID, const void* data, size_t size, bool completed);

    template <typename T>
    SteamAPICall_t CreateCallResult(int callbackID, const T& data)
    {
        return CreateRaw(callbackID, &data, sizeof(T));
    }

    bool GetData(SteamAPICall_t call, void* outData, int outSize, int* callbackID = nullptr);
    bool IsCompleted(SteamAPICall_t call);
    bool Complete(SteamAPICall_t call);

    void Register(void* callResult, SteamAPICall_t call);
    void Unregister(void* callResult, SteamAPICall_t call = 0);

    void RegisterCallResult(void* callResult, SteamAPICall_t call);
    void UnregisterCallResult(void* callResult, SteamAPICall_t call);
    void UnregisterCallResult(void* callResult);
    int RunCompletedCallResults(int maxDispatch = 64);

    int Count();
    int PendingCount();

    const std::vector<CallResultRecord>& GetResults();
    std::vector<CallResultRecord> SnapshotResults();
}
