#pragma once

#include "StdInc.h"
#include "Auth/LocalAuthTicket.h"

namespace AuthTicketCallbacks
{
    void QueueSessionTicketIssued(uint32_t handle);
    void QueueWebApiTicketIssued(uint32_t handle, const LocalAuthTicket::Ticket& ticket);
}
