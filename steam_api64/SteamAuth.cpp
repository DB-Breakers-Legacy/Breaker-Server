#include "StdInc.h"
#include "SteamAuth.h"
#include "Logger.h"

static uint32_t g_NextTicket = 1;

namespace SteamAuth
{
    void Init()
    {
        Logger::Info("SteamAuth initialized");
    }

    uint32_t GetAuthSessionTicket(void* ticket, int maxTicket, uint32_t* ticketSize)
    {
        static const char fakeTicket[] = "BREAKERSREVIVED_AUTH_TICKET";

        uint32_t size = sizeof(fakeTicket);

        if (ticket && maxTicket >= static_cast<int>(size))
            memcpy(ticket, fakeTicket, size);

        if (ticketSize)
            *ticketSize = size;

        uint32_t handle = g_NextTicket++;

        Logger::Info("SteamAuth::GetAuthSessionTicket handle=" + std::to_string(handle));

        return handle;
    }

    int BeginAuthSession(const void* ticket, int ticketSize, CSteamID steamID)
    {
        NSR_UNUSED(ticket);
        NSR_UNUSED(ticketSize);

        Logger::Info("SteamAuth::BeginAuthSession steamID=" + std::to_string(static_cast<uint64_t>(steamID)));

        return 0;
    }

    void EndAuthSession(CSteamID steamID)
    {
        Logger::Info("SteamAuth::EndAuthSession steamID=" + std::to_string(static_cast<uint64_t>(steamID)));
    }

    void CancelAuthTicket(uint32_t ticketHandle)
    {
        Logger::Info("SteamAuth::CancelAuthTicket handle=" + std::to_string(ticketHandle));
    }
}