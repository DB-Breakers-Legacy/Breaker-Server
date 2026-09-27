#include "StdInc.h"
#include "SteamPersonaManager.h"
#include "SteamIDManager.h"
#include "Logger.h"

namespace
{
    std::vector<CSteamID> g_Friends;
    std::unordered_map<uint64_t, std::string> g_Names;
    std::map<std::string, std::string> g_RichPresence;

    std::string g_DefaultName = "Breakers Player";
    std::string g_ReturnString;
}

namespace SteamPersonaManager
{
    bool Init()
    {
        g_Friends.clear();
        g_Names.clear();
        g_RichPresence.clear();

        CSteamID local = SteamIDManager::GetLocalSteamID();

        g_Friends.push_back(local);
        g_Names[SteamIDManager::ToUint64(local)] = SteamIDManager::GetPersonaName();

        g_RichPresence["status"] = "Offline";
        g_RichPresence["connect"] = "";
        g_RichPresence["steam_display"] = "#Status_Offline";

        Logger::Info("SteamPersonaManager initialized");
        return true;
    }

    void Shutdown()
    {
        g_Friends.clear();
        g_Names.clear();
        g_RichPresence.clear();
    }

    const char* GetPersonaName()
    {
        return SteamIDManager::GetPersonaName();
    }

    void SetPersonaName(const std::string& name)
    {
        SteamIDManager::SetPersonaName(name);

        if (!g_Friends.empty())
            g_Names[SteamIDManager::ToUint64(g_Friends[0])] = SteamIDManager::GetPersonaName();
    }

    int GetFriendCount()
    {
        return static_cast<int>(g_Friends.size());
    }

    CSteamID GetFriendByIndex(int index)
    {
        if (index < 0 || index >= static_cast<int>(g_Friends.size()))
            return 0;

        return g_Friends[index];
    }

    uint64_t GetFriendByIndex64(int index)
    {
        return SteamIDManager::ToUint64(GetFriendByIndex(index));
    }

    const char* GetFriendPersonaName(CSteamID steamID)
    {
        auto it = g_Names.find(SteamIDManager::ToUint64(steamID));

        if (it != g_Names.end())
            return it->second.c_str();

        return g_DefaultName.c_str();
    }

    bool SetRichPresence(const char* key, const char* value)
    {
        if (!key || key[0] == '\0')
            return false;

        g_RichPresence[key] = value ? value : "";

        Logger::Info(
            "SteamPersonaManager rich presence set " +
            std::string(key) +
            "=" +
            g_RichPresence[key]);

        return true;
    }

    bool SetRichPresence(const std::string& key, const std::string& value)
    {
        return SetRichPresence(key.c_str(), value.c_str());
    }

    void ClearRichPresence()
    {
        g_RichPresence.clear();
        Logger::Info("SteamPersonaManager rich presence cleared");
    }

    const char* GetRichPresence(const char* key)
    {
        if (!key)
        {
            g_ReturnString.clear();
            return g_ReturnString.c_str();
        }

        auto it = g_RichPresence.find(key);

        if (it == g_RichPresence.end())
        {
            g_ReturnString.clear();
            return g_ReturnString.c_str();
        }

        g_ReturnString = it->second;
        return g_ReturnString.c_str();
    }

    const char* GetRichPresence(const std::string& key)
    {
        return GetRichPresence(key.c_str());
    }

    int GetRichPresenceKeyCount()
    {
        return static_cast<int>(g_RichPresence.size());
    }

    const char* GetRichPresenceKeyByIndex(int index)
    {
        if (index < 0 || index >= static_cast<int>(g_RichPresence.size()))
        {
            g_ReturnString.clear();
            return g_ReturnString.c_str();
        }

        auto it = g_RichPresence.begin();
        std::advance(it, index);

        g_ReturnString = it->first;
        return g_ReturnString.c_str();
    }
}