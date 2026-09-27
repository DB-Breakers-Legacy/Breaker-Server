#pragma once
#include "StdInc.h"

namespace SteamPersonaManager
{
    bool Init();
    void Shutdown();

    const char* GetPersonaName();
    void SetPersonaName(const std::string& name);

    int GetFriendCount();
    CSteamID GetFriendByIndex(int index);
    uint64_t GetFriendByIndex64(int index);

    const char* GetFriendPersonaName(CSteamID steamID);

    bool SetRichPresence(const char* key, const char* value);
    bool SetRichPresence(const std::string& key, const std::string& value);

    void ClearRichPresence();

    const char* GetRichPresence(const char* key);
    const char* GetRichPresence(const std::string& key);

    int GetRichPresenceKeyCount();
    const char* GetRichPresenceKeyByIndex(int index);
}