#include "Auth/AuthTicketCallbacks.h"
#include "SteamCallbackManager.h"

namespace
{
    constexpr int GetAuthSessionTicketCallbackId = 163;
    constexpr int GetTicketForWebApiCallbackId = 168;
    constexpr int SteamResultOk = 1;
    constexpr size_t WebApiTicketCapacity = 2560;

    struct GetAuthSessionTicketResponse
    {
        uint32_t TicketHandle = 0;
        int32_t Result = SteamResultOk;
    };

    struct GetTicketForWebApiResponse
    {
        uint32_t TicketHandle = 0;
        int32_t Result = SteamResultOk;
        int32_t TicketSize = 0;
        std::array<uint8_t, WebApiTicketCapacity> Ticket{};
    };
}

namespace AuthTicketCallbacks
{
    void QueueSessionTicketIssued(uint32_t handle)
    {
        GetAuthSessionTicketResponse response;
        response.TicketHandle = handle;
        SteamCallbackManager::PushCallback(
            GetAuthSessionTicketCallbackId,
            &response,
            sizeof(response));
    }

    void QueueWebApiTicketIssued(uint32_t handle, const LocalAuthTicket::Ticket& ticket)
    {
        GetTicketForWebApiResponse response;
        response.TicketHandle = handle;
        response.TicketSize = static_cast<int32_t>(ticket.Bytes.size());
        std::copy(ticket.Bytes.begin(), ticket.Bytes.end(), response.Ticket.begin());
        SteamCallbackManager::PushCallback(
            GetTicketForWebApiCallbackId,
            &response,
            sizeof(response));
    }
}
