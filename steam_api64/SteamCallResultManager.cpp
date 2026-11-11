#include "StdInc.h"
#include "SteamCallResultManager.h"
#include "SteamCallbackManager.h"
#include "SteamDiagnostics.h"
#include "Logger.h"

namespace
{
    constexpr int kCallbackSteamAPICallCompleted = 703;

    struct CallbackBaseShim
    {
        virtual void Run(void* data) = 0;
        virtual void Run(void* data, bool failed, SteamAPICall_t call) = 0;
        virtual int GetCallbackSizeBytes() = 0;

        uint8_t Flags;
        int CallbackID;
    };

    constexpr uint8_t kCallbackRegistered = 0x01;

    std::mutex g_Mutex;
    SteamAPICall_t g_NextCall = 1;
    std::vector<SteamCallResultManager::CallResultRecord> g_Results;
    std::map<SteamAPICall_t, std::vector<void*>> g_Registered;

    struct SteamAPICallCompletedResult
    {
        SteamAPICall_t m_hAsyncCall;
        int m_iCallback;
        uint32_t m_cubParam;
    };

    std::string PointerText(const void* ptr)
    {
        std::ostringstream ss;
        ss << "0x" << std::hex << reinterpret_cast<uintptr_t>(ptr);
        return ss.str();
    }

    bool IsReadableMemory(const void* ptr, size_t size)
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

        return end >= start && end <= regionEnd;
    }

    bool IsWritableMemory(void* ptr, size_t size)
    {
        if (!IsReadableMemory(ptr, size))
            return false;

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
            return false;

        const DWORD protect = mbi.Protect & 0xff;
        return protect == PAGE_READWRITE ||
            protect == PAGE_WRITECOPY ||
            protect == PAGE_EXECUTE_READWRITE ||
            protect == PAGE_EXECUTE_WRITECOPY;
    }

    bool IsExecutableMemory(const void* ptr)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!ptr || VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
            return false;

        const DWORD protect = mbi.Protect & 0xff;
        return protect == PAGE_EXECUTE ||
            protect == PAGE_EXECUTE_READ ||
            protect == PAGE_EXECUTE_READWRITE ||
            protect == PAGE_EXECUTE_WRITECOPY;
    }

    bool HasReadableVTable(CallbackBaseShim* base)
    {
        if (!IsReadableMemory(base, sizeof(void*)))
            return false;

        void* const* vtable = *reinterpret_cast<void***>(base);
        if (!IsReadableMemory(vtable, sizeof(void*) * 3))
            return false;

        return IsExecutableMemory(vtable[0]) &&
            IsExecutableMemory(vtable[1]) &&
            IsExecutableMemory(vtable[2]);
    }

    int SafeGetCallbackSize(CallbackBaseShim* base)
    {
        if (!HasReadableVTable(base))
            return 0;

        // MSVC C2712 fix: do not use __try in C++ functions with object unwinding.
        const int size = base->GetCallbackSizeBytes();
        return (size > 0 && size < 1024 * 1024) ? size : 0;
    }

    void MarkCallResultRegistered(void* callback)
    {
        if (!callback)
            return;

        auto* base = reinterpret_cast<CallbackBaseShim*>(callback);

        if (!IsWritableMemory(base, sizeof(CallbackBaseShim)))
        {
            Logger::Error("SteamCallResultManager skipped invalid register flag write");
            return;
        }

        base->Flags |= kCallbackRegistered;
    }

    void MarkCallResultUnregistered(void* callback)
    {
        if (!callback)
            return;

        auto* base = reinterpret_cast<CallbackBaseShim*>(callback);

        if (!IsWritableMemory(base, sizeof(CallbackBaseShim)))
        {
            Logger::Error("SteamCallResultManager skipped invalid unregister flag write");
            return;
        }

        base->Flags &= static_cast<uint8_t>(~kCallbackRegistered);
    }

    std::vector<unsigned char> BuildDispatchPayload(void* callback, const std::vector<unsigned char>& data)
    {
        int expectedSize = 0;

        if (callback)
            expectedSize = SafeGetCallbackSize(reinterpret_cast<CallbackBaseShim*>(callback));

        size_t payloadSize = std::max<size_t>(data.size(), static_cast<size_t>(expectedSize));
        std::vector<unsigned char> payload(payloadSize);

        if (!data.empty())
            std::memcpy(payload.data(), data.data(), data.size());

        return payload;
    }

    bool SafeRunCallResult(CallbackBaseShim* base, void* payloadData, SteamAPICall_t call)
    {
        if (!HasReadableVTable(base))
            return false;

        __try
        {
            base->Run(payloadData, false, call);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            NSR_UNUSED(call);
            OutputDebugStringA("SteamCallResultManager protected crash in call result dispatch\n");
            return false;
        }
    }

    bool DispatchCallResult(void* callback, SteamAPICall_t call, const std::vector<unsigned char>& data)
    {
        if (!callback)
            return false;

        std::vector<unsigned char> payload = BuildDispatchPayload(callback, data);
        void* payloadData = payload.empty() ? nullptr : payload.data();

        auto* base = reinterpret_cast<CallbackBaseShim*>(callback);
        if (!HasReadableVTable(base))
        {
            Logger::Error(
                "SteamCallResultManager skipped invalid call result call=" +
                std::to_string(static_cast<unsigned long long>(call)) +
                " callback=" +
                PointerText(callback));
            SteamDiagnostics::MarkSteam(
                "CallResultInvalid",
                "call=" +
                std::to_string(static_cast<unsigned long long>(call)) +
                " callback=" +
                PointerText(callback));
            return false;
        }

        SteamDiagnostics::MarkSteam(
            "CallResultRunBegin",
            "call=" +
            std::to_string(static_cast<unsigned long long>(call)) +
            " bytes=" +
            std::to_string(static_cast<unsigned long long>(data.size())) +
            " payload=" +
            std::to_string(static_cast<unsigned long long>(payload.size())) +
            " callback=" +
            PointerText(callback));

        const bool ok = SafeRunCallResult(base, payloadData, call);

        SteamDiagnostics::MarkSteam(
            ok ? "CallResultRunEnd" : "CallResultRunFailed",
            "call=" +
            std::to_string(static_cast<unsigned long long>(call)) +
            " callback=" +
            PointerText(callback));

        return ok;
    }
}

namespace SteamCallResultManager
{
    bool Init()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_NextCall = 1;
        g_Results.clear();
        g_Registered.clear();
        Logger::Info("SteamCallResultManager initialized");
        return true;
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Results.clear();
        g_Registered.clear();
    }

    SteamAPICall_t CreateRaw(int callbackID, const void* data, size_t size)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        CallResultRecord record{};
        record.Call = g_NextCall++;
        record.CallbackID = callbackID;
        record.Completed = true;

        if (data && size > 0)
        {
            record.Data.resize(size);
            std::memcpy(record.Data.data(), data, size);
        }

        g_Results.push_back(record);

        SteamAPICallCompletedResult completed{};
        completed.m_hAsyncCall = record.Call;
        completed.m_iCallback = callbackID;
        completed.m_cubParam = static_cast<uint32_t>(record.Data.size());
        SteamCallbackManager::PushCallback(kCallbackSteamAPICallCompleted, &completed, sizeof(completed));

        Logger::Info(
            "SteamCallResultManager created call=" +
            std::to_string(static_cast<unsigned long long>(record.Call)) +
            " callbackID=" +
            std::to_string(callbackID));

        SteamDiagnostics::MarkSteam(
            "CreateCallResult",
            "call=" +
            std::to_string(static_cast<unsigned long long>(record.Call)) +
            " callback=" +
            std::to_string(callbackID) +
            " bytes=" +
            std::to_string(static_cast<unsigned long long>(record.Data.size())));

        return record.Call;
    }

    SteamAPICall_t CreateCall(int callbackID, const void* data, size_t size)
    {
        return CreateRaw(callbackID, data, size);
    }

    SteamAPICall_t CreateCallResult(int callbackID)
    {
        return CreateRaw(callbackID, nullptr, 0);
    }

    SteamAPICall_t CreateCallResult(int callbackID, const void* data, size_t size)
    {
        return CreateRaw(callbackID, data, size);
    }

    SteamAPICall_t CreateCallResult(int callbackID, const void* data, size_t size, bool)
    {
        return CreateRaw(callbackID, data, size);
    }

    bool GetData(SteamAPICall_t call, void* outData, int outSize, int* callbackID)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        for (const auto& result : g_Results)
        {
            if (result.Call != call)
                continue;

            if (callbackID)
                *callbackID = result.CallbackID;

            if (outData && outSize > 0)
            {
                std::memset(outData, 0, static_cast<size_t>(outSize));

                if (!result.Data.empty())
                {
                    size_t copySize = std::min<size_t>(static_cast<size_t>(outSize), result.Data.size());
                    std::memcpy(outData, result.Data.data(), copySize);
                }
            }

            return true;
        }

        return false;
    }

    bool IsCompleted(SteamAPICall_t call)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        for (const auto& result : g_Results)
        {
            if (result.Call == call)
                return result.Completed;
        }

        return false;
    }

    bool Complete(SteamAPICall_t call)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        for (auto& result : g_Results)
        {
            if (result.Call != call)
                continue;

            result.Completed = true;

            if (result.CallbackID != 0)
            {
                SteamDiagnostics::MarkSteam(
                    "CompleteCallResult",
                    "call=" +
                    std::to_string(static_cast<unsigned long long>(call)) +
                    " callback=" +
                    std::to_string(result.CallbackID));
            }

            return true;
        }

        return false;
    }

    void Register(void* callResult, SteamAPICall_t call)
    {
        if (!callResult)
            return;

        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Registered[call].push_back(callResult);
        MarkCallResultRegistered(callResult);

        const auto resultIt = std::find_if(
            g_Results.begin(),
            g_Results.end(),
            [call](const CallResultRecord& result) { return result.Call == call; });

        if (resultIt != g_Results.end() && resultIt->CallbackID == 2101)
        {
            Logger::Info(
                "SteamHTTP call result registered call=" +
                std::to_string(static_cast<unsigned long long>(call)) +
                " bytes=" +
                std::to_string(static_cast<unsigned long long>(resultIt->Data.size())));
        }

        SteamDiagnostics::MarkSteam(
            "RegisterCallResult",
            "call=" +
            std::to_string(static_cast<unsigned long long>(call)));
    }

    void Unregister(void* callResult, SteamAPICall_t call)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        if (call != 0)
        {
            auto& list = g_Registered[call];
            list.erase(std::remove(list.begin(), list.end(), callResult), list.end());
            MarkCallResultUnregistered(callResult);
            SteamDiagnostics::MarkSteam(
                "UnregisterCallResult",
                "call=" +
                std::to_string(static_cast<unsigned long long>(call)));
            return;
        }

        for (auto& item : g_Registered)
        {
            auto& list = item.second;
            list.erase(std::remove(list.begin(), list.end(), callResult), list.end());
        }

        MarkCallResultUnregistered(callResult);
        SteamDiagnostics::MarkSteam("UnregisterCallResult", "all calls");
    }

    void RegisterCallResult(void* callResult, SteamAPICall_t call)
    {
        Register(callResult, call);
    }

    void UnregisterCallResult(void* callResult, SteamAPICall_t call)
    {
        Unregister(callResult, call);
    }

    void UnregisterCallResult(void* callResult)
    {
        Unregister(callResult, 0);
    }

    int RunCompletedCallResults(int maxDispatch)
    {
        struct PendingDispatch
        {
            void* Callback = nullptr;
            SteamAPICall_t Call = 0;
            int CallbackID = 0;
            std::vector<unsigned char> Data;
        };

        std::vector<PendingDispatch> pending;

        {
            std::lock_guard<std::mutex> lock(g_Mutex);

            for (auto it = g_Registered.begin(); it != g_Registered.end() && static_cast<int>(pending.size()) < maxDispatch;)
            {
                const SteamAPICall_t call = it->first;

                auto resultIt = std::find_if(
                    g_Results.begin(),
                    g_Results.end(),
                    [call](const CallResultRecord& result) {
                        return result.Call == call && result.Completed;
                    });

                if (resultIt == g_Results.end())
                {
                    ++it;
                    continue;
                }

                for (void* callback : it->second)
                {
                    if (static_cast<int>(pending.size()) >= maxDispatch)
                        break;

                    pending.push_back({ callback, call, resultIt->CallbackID, resultIt->Data });
                }

                it = g_Registered.erase(it);
            }
        }

        for (const PendingDispatch& dispatch : pending)
        {
            if (dispatch.CallbackID == 2101)
            {
                Logger::Info(
                    "SteamHTTP call result dispatch call=" +
                    std::to_string(static_cast<unsigned long long>(dispatch.Call)) +
                    " bytes=" +
                    std::to_string(static_cast<unsigned long long>(dispatch.Data.size())));
            }

            MarkCallResultUnregistered(dispatch.Callback);
            const bool dispatched = DispatchCallResult(dispatch.Callback, dispatch.Call, dispatch.Data);

            if (dispatch.CallbackID == 2101)
            {
                Logger::Info(
                    "SteamHTTP call result finished call=" +
                    std::to_string(static_cast<unsigned long long>(dispatch.Call)) +
                    " success=" +
                    (dispatched ? "1" : "0"));
            }
        }

        if (!pending.empty())
        {
            SteamDiagnostics::MarkSteam(
                "RunCompletedCallResults",
                "dispatched=" +
                std::to_string(static_cast<unsigned long long>(pending.size())));
        }

        return static_cast<int>(pending.size());
    }

    int Count()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return static_cast<int>(g_Results.size());
    }

    int PendingCount()
    {
        return Count();
    }

    const std::vector<CallResultRecord>& GetResults()
    {
        return g_Results;
    }

    std::vector<CallResultRecord> SnapshotResults()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_Results;
    }
}
