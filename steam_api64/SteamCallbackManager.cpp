#include "StdInc.h"
#include "SteamCallbackManager.h"
#include "SteamCallResultManager.h"
#include "SteamDiagnostics.h"
#include "SteamLobbyManager.h"
#include "Logger.h"

namespace
{
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
    std::map<int, std::vector<void*>> g_Callbacks;
    std::queue<BreakersCallbackMessage> g_Queue;

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

    void MarkCallbackRegistered(void* callback, int callbackID)
    {
        if (!callback)
            return;

        auto* base = reinterpret_cast<CallbackBaseShim*>(callback);

        if (!IsWritableMemory(base, sizeof(CallbackBaseShim)))
        {
            Logger::Error("SteamCallbackManager skipped invalid register flag write");
            return;
        }

        base->Flags |= kCallbackRegistered;
        base->CallbackID = callbackID;
    }

    void MarkCallbackUnregistered(void* callback)
    {
        if (!callback)
            return;

        auto* base = reinterpret_cast<CallbackBaseShim*>(callback);

        if (!IsWritableMemory(base, sizeof(CallbackBaseShim)))
        {
            Logger::Error("SteamCallbackManager skipped invalid unregister flag write");
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

    bool SafeRunCallback(CallbackBaseShim* base, void* payloadData)
    {
        if (!HasReadableVTable(base))
            return false;

        __try
        {
            base->Run(payloadData);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            OutputDebugStringA("SteamCallbackManager protected crash in callback dispatch\n");
            return false;
        }
    }

    bool DispatchCallback(void* callback, const BreakersCallbackMessage& message)
    {
        if (!callback)
            return false;

        std::vector<unsigned char> payload = BuildDispatchPayload(callback, message.Data);
        void* payloadData = payload.empty() ? nullptr : payload.data();

        auto* base = reinterpret_cast<CallbackBaseShim*>(callback);
        if (!HasReadableVTable(base))
        {
            Logger::Error(
                "SteamCallbackManager skipped invalid callback id=" +
                std::to_string(message.CallbackID) +
                " callback=" +
                PointerText(callback));
            SteamDiagnostics::MarkSteam(
                "CallbackInvalid",
                "id=" +
                std::to_string(message.CallbackID) +
                " callback=" +
                PointerText(callback));
            return false;
        }

        SteamDiagnostics::MarkSteam(
            "CallbackRunBegin",
            "id=" +
            std::to_string(message.CallbackID) +
            " bytes=" +
            std::to_string(static_cast<unsigned long long>(message.Data.size())) +
            " payload=" +
            std::to_string(static_cast<unsigned long long>(payload.size())) +
            " callback=" +
            PointerText(callback));

        const bool ok = SafeRunCallback(base, payloadData);

        SteamDiagnostics::MarkSteam(
            ok ? "CallbackRunEnd" : "CallbackRunFailed",
            "id=" +
            std::to_string(message.CallbackID) +
            " callback=" +
            PointerText(callback));

        return ok;
    }
}

namespace SteamCallbackManager
{
    bool Init()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        while (!g_Queue.empty()) g_Queue.pop();
        g_Callbacks.clear();
        Logger::Info("SteamCallbackManager initialized");
        return true;
    }

    void RegisterCallback(void* callback, int callbackID)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (!callback) return;
        g_Callbacks[callbackID].push_back(callback);
        MarkCallbackRegistered(callback, callbackID);
        Logger::Info("SteamAPI_RegisterCallback id=" + std::to_string(callbackID));
        SteamDiagnostics::MarkSteam("RegisterCallback", "id=" + std::to_string(callbackID));
    }

    void UnregisterCallback(void* callback)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto& kv : g_Callbacks)
        {
            auto& list = kv.second;
            list.erase(std::remove(list.begin(), list.end(), callback), list.end());
        }
        MarkCallbackUnregistered(callback);
        Logger::Info("SteamAPI_UnregisterCallback");
        SteamDiagnostics::MarkSteam("UnregisterCallback");
    }

    void PushCallback(int callbackID, const void* data, size_t size)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        BreakersCallbackMessage msg{};
        msg.CallbackID = callbackID;
        if (data && size)
        {
            msg.Data.resize(size);
            std::memcpy(msg.Data.data(), data, size);
        }
        g_Queue.push(msg);
        SteamDiagnostics::MarkSteam(
            "PushCallback",
            "id=" +
            std::to_string(callbackID) +
            " bytes=" +
            std::to_string(static_cast<unsigned long long>(msg.Data.size())));
    }

    bool PopCallback(BreakersCallbackMessage& outMessage)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (g_Queue.empty()) return false;
        outMessage = g_Queue.front();
        g_Queue.pop();
        return true;
    }

    int Count()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return (int)g_Queue.size();
    }

    void RunCallbacks()
    {
        SteamLobbyManager::Tick();
        SteamCallResultManager::RunCompletedCallResults(64);

        int dispatched = 0;
        for (int processed = 0; processed < 64; ++processed)
        {
            BreakersCallbackMessage msg;
            std::vector<void*> callbacks;

            {
                std::lock_guard<std::mutex> lock(g_Mutex);

                if (g_Queue.empty())
                    break;

                msg = g_Queue.front();
                g_Queue.pop();

                auto it = g_Callbacks.find(msg.CallbackID);
                if (it != g_Callbacks.end())
                    callbacks = it->second;
            }

            SteamDiagnostics::MarkSteam(
                "RunCallbackDispatch",
                "id=" +
                std::to_string(msg.CallbackID) +
                " bytes=" +
                std::to_string(static_cast<unsigned long long>(msg.Data.size())) +
                " receivers=" +
                std::to_string(static_cast<unsigned long long>(callbacks.size())));

            for (void* callback : callbacks)
            {
                if (DispatchCallback(callback, msg))
                    ++dispatched;
            }
        }

        if (dispatched > 0)
        {
            SteamDiagnostics::MarkSteam(
                "RunCallbacks",
                "dispatched=" +
                std::to_string(dispatched));
        }
    }
}
