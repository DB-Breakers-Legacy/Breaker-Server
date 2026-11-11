#include "Auth/LocalAuthTicket.h"

namespace
{
    constexpr std::array<uint8_t, 8> Magic = { 'B', 'R', 'V', 'A', 'U', 'T', 'H', '1' };
    constexpr uint32_t Version = 1;
    constexpr uint64_t FnvOffset = 14695981039346656037ull;
    constexpr uint64_t FnvPrime = 1099511628211ull;

    void WriteU32(std::array<uint8_t, LocalAuthTicket::TicketSize>& out, size_t offset, uint32_t value)
    {
        for (size_t i = 0; i < sizeof(value); ++i)
            out[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFFu);
    }

    void WriteU64(std::array<uint8_t, LocalAuthTicket::TicketSize>& out, size_t offset, uint64_t value)
    {
        for (size_t i = 0; i < sizeof(value); ++i)
            out[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFFu);
    }

    uint64_t Checksum(const std::array<uint8_t, LocalAuthTicket::TicketSize>& bytes)
    {
        uint64_t hash = FnvOffset;
        for (size_t i = 0; i < 24; ++i)
        {
            hash ^= bytes[i];
            hash *= FnvPrime;
        }
        return hash;
    }
}

namespace LocalAuthTicket
{
    Ticket Build(uint64_t steamID, uint32_t serial)
    {
        Ticket ticket;
        std::copy(Magic.begin(), Magic.end(), ticket.Bytes.begin());
        WriteU32(ticket.Bytes, 8, Version);
        WriteU64(ticket.Bytes, 12, steamID);
        WriteU32(ticket.Bytes, 20, serial);
        WriteU64(ticket.Bytes, 24, Checksum(ticket.Bytes));
        return ticket;
    }
}
