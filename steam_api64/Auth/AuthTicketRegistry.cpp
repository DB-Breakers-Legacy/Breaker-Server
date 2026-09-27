#include "Auth/AuthTicketRegistry.h"

namespace
{
    std::atomic<uint32_t> g_NextHandle{ 1 };
    std::mutex g_Mutex;
    std::set<uint32_t> g_ActiveHandles;
}

namespace AuthTicketRegistry
{
    void Init()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_ActiveHandles.clear();
        g_NextHandle.store(1);
    }

    IssuedTicket Issue(uint64_t steamID)
    {
        IssuedTicket issued;
        issued.Handle = g_NextHandle.fetch_add(1);
        if (issued.Handle == 0)
            issued.Handle = g_NextHandle.fetch_add(1);

        issued.Ticket = LocalAuthTicket::Build(steamID, issued.Handle);

        std::lock_guard<std::mutex> lock(g_Mutex);
        g_ActiveHandles.insert(issued.Handle);
        return issued;
    }

    void Cancel(uint32_t handle)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_ActiveHandles.erase(handle);
    }

    bool IsActive(uint32_t handle)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_ActiveHandles.find(handle) != g_ActiveHandles.end();
    }
}
