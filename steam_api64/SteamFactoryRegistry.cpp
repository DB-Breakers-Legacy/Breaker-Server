#include "StdInc.h"
#include "SteamFactoryRegistry.h"
#include "Logger.h"

static std::mutex g_Mutex;
static std::map<std::string, void*> g_Registry;

namespace SteamFactoryRegistry
{
    void Register(const std::string& name, void* ptr)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        g_Registry[name] = ptr;

        Logger::Info("[STEAM REGISTRY] registered " + name);
    }

    void* Find(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        auto it = g_Registry.find(name);

        if (it == g_Registry.end())
            return nullptr;

        Logger::Info("[STEAM REGISTRY] resolved " + name);

        return it->second;
    }

    void Dump()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);

        Logger::Info("[STEAM REGISTRY] dump begin");

        for (const auto& item : g_Registry)
            Logger::Info("  " + item.first);

        Logger::Info("[STEAM REGISTRY] dump end");
    }
}