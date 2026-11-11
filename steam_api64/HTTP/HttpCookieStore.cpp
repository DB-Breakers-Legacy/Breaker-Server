#include "StdInc.h"
#include "HTTP/HttpCookieStore.h"

namespace
{
    struct CookieContainer
    {
        bool AllowResponsesToModify = false;
        std::vector<std::string> Cookies;
    };

    std::mutex g_Mutex;
    uint32_t g_NextHandle = 1;
    std::unordered_map<uint32_t, CookieContainer> g_Containers;
}

namespace HttpCookieStore
{
    uint32_t Create(bool allowResponsesToModify)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        uint32_t handle = g_NextHandle++;
        if (handle == 0)
            handle = g_NextHandle++;

        g_Containers[handle] = CookieContainer{ allowResponsesToModify, {} };
        return handle;
    }

    bool Release(uint32_t handle)
    {
        if (handle == 0)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_Containers.erase(handle) != 0;
    }

    bool Set(uint32_t handle, const char* host, const char* url, const char* cookie)
    {
        if (handle == 0 || !host || !url || !cookie)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        auto it = g_Containers.find(handle);
        if (it == g_Containers.end())
            return false;

        it->second.Cookies.push_back(std::string(host) + "|" + url + "|" + cookie);
        return true;
    }

    bool Exists(uint32_t handle)
    {
        if (handle == 0)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_Containers.find(handle) != g_Containers.end();
    }
}
