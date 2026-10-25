#pragma once
#include "StdInc.h"

namespace SteamAuth
{
    void Init();

    uint32_t GetAuthSessionTicket(void* ticket, int maxTicket, uint32_t* ticketSize);
    int BeginAuthSession(const void* ticket, int ticketSize, CSteamID steamID);
    void EndAuthSession(CSteamID steamID);
    void CancelAuthTicket(uint32_t ticketHandle);
}