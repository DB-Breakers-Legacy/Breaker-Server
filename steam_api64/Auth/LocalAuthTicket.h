#pragma once

#include "StdInc.h"

namespace LocalAuthTicket
{
    constexpr size_t TicketSize = 32;

    struct Ticket
    {
        std::array<uint8_t, TicketSize> Bytes{};
    };

    Ticket Build(uint64_t steamID, uint32_t serial);
}
