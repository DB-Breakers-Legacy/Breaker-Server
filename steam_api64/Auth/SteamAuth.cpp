#include "StdInc.h"
#include "Auth/SteamAuth.h"
#include "Auth/AuthTicketRegistry.h"
#include "Auth/AuthTicketCallbacks.h"
#include "Auth/LocalAuthTicket.h"
#include "SteamIDManager.h"
#include "Logger.h"

namespace SteamAuth
{
    void Init()
    {
        AuthTicketRegistry::Init();
        Logger::Info("SteamAuth initialized");
    }

    uint32_t GetAuthSessionTicket(void* ticket, int maxTicket, uint32_t* ticketSize)
    {
        const auto issued = AuthTicketRegistry::Issue(SteamIDManager::GetSteamID64());
        const uint32_t size = static_cast<uint32_t>(issued.Ticket.Bytes.size());

        if (ticketSize)
            *ticketSize = size;

        if (ticket && maxTicket >= static_cast<int>(size))
            memcpy(ticket, issued.Ticket.Bytes.data(), size);

        AuthTicketCallbacks::QueueSessionTicketIssued(issued.Handle);

        Logger::Info(
            "SteamAuth::GetAuthSessionTicket handle=" + std::to_string(issued.Handle) +
            " bytes=" + std::to_string(size) +
            " steamid=" + std::to_string(static_cast<unsigned long long>(SteamIDManager::GetSteamID64())));

        return issued.Handle;
    }

    uint32_t GetAuthTicketForWebApi(const char* identity)
    {
        const auto issued = AuthTicketRegistry::Issue(SteamIDManager::GetSteamID64());
        AuthTicketCallbacks::QueueWebApiTicketIssued(issued.Handle, issued.Ticket);

        Logger::Info(
            "SteamAuth::GetAuthTicketForWebApi handle=" + std::to_string(issued.Handle) +
            " identity=" + (identity ? std::string(identity) : std::string()));

        return issued.Handle;
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
        AuthTicketRegistry::Cancel(ticketHandle);
        Logger::Info("SteamAuth::CancelAuthTicket handle=" + std::to_string(ticketHandle));
    }
}
