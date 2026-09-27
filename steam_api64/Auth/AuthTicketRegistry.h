#pragma once

#include "StdInc.h"
#include "Auth/LocalAuthTicket.h"

namespace AuthTicketRegistry
{
    struct IssuedTicket
    {
        uint32_t Handle = 0;
        LocalAuthTicket::Ticket Ticket;
    };

    void Init();
    IssuedTicket Issue(uint64_t steamID);
    void Cancel(uint32_t handle);
    bool IsActive(uint32_t handle);
}
