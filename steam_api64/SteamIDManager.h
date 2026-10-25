#pragma once
#include "StdInc.h"

namespace SteamIDManager
{
    bool Init();
    void Shutdown();

    CSteamID FromUint64(uint64_t value);
    CSteamID GetLocalSteamID();
    CSteamID GetLocalUser();

    uint64_t ToUint64(CSteamID value);
    uint64_t GetSteamID64();
    uint32_t GetAccountID();

    const char* GetPersonaName();
    const std::string& GetPersonaNameString();
    void SetPersonaName(const std::string& name);
}