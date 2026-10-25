#include "StdInc.h"
#include "HTTP/HttpCallbacks.h"
#include "SteamCallResultManager.h"
#include "SteamCallbackManager.h"
#include "Logger.h"

namespace
{
    constexpr int kHTTPRequestCompleted = 2101;
    constexpr int kHTTPRequestHeadersReceived = 2102;
    constexpr int kHTTPRequestDataReceived = 2103;

    struct HTTPRequestCompletedPayload
    {
        uint32_t RequestHandle = 0;
        uint64_t ContextValue = 0;
        bool RequestSuccessful = false;
        int StatusCode = 0;
        uint32_t BodySize = 0;
    };

    struct HTTPRequestHeadersReceivedPayload
    {
        uint32_t RequestHandle = 0;
        uint64_t ContextValue = 0;
    };

    struct HTTPRequestDataReceivedPayload
    {
        uint32_t RequestHandle = 0;
        uint64_t ContextValue = 0;
        uint32_t Offset = 0;
        uint32_t BytesReceived = 0;
    };

    static_assert(offsetof(HTTPRequestCompletedPayload, ContextValue) == 8, "Steam HTTP callback layout mismatch");
    static_assert(offsetof(HTTPRequestCompletedPayload, RequestSuccessful) == 16, "Steam HTTP callback layout mismatch");
    static_assert(offsetof(HTTPRequestCompletedPayload, StatusCode) == 20, "Steam HTTP callback layout mismatch");
    static_assert(offsetof(HTTPRequestCompletedPayload, BodySize) == 24, "Steam HTTP callback layout mismatch");
    static_assert(sizeof(HTTPRequestCompletedPayload) == 32, "Steam HTTP callback size mismatch");
}

namespace HttpCallbacks
{
    SteamAPICall_t QueueCompleted(const HttpRequest& request)
    {
        HTTPRequestCompletedPayload payload{};
        payload.RequestHandle = request.Handle;
        payload.ContextValue = request.ContextValue;
        payload.RequestSuccessful = request.Response.RequestSuccessful;
        payload.StatusCode = request.Response.StatusCode;
        payload.BodySize = static_cast<uint32_t>(request.Response.Body.size());

        const SteamAPICall_t call = SteamCallResultManager::CreateCallResult(
            kHTTPRequestCompleted,
            &payload,
            sizeof(payload));

        if (request.Method == 1)
        {
            Logger::Info(
                "SteamHTTP GET completion handle=" + std::to_string(request.Handle) +
                " call=" + std::to_string(static_cast<unsigned long long>(call)) +
                " context=" + std::to_string(static_cast<unsigned long long>(request.ContextValue)) +
                " success=" + (request.Response.RequestSuccessful ? "1" : "0") +
                " status=" + std::to_string(request.Response.StatusCode) +
                " body=" + std::to_string(static_cast<unsigned long long>(request.Response.Body.size())));
        }

        return call;
    }

    void QueueStreaming(const HttpRequest& request)
    {
        HTTPRequestHeadersReceivedPayload headers{};
        headers.RequestHandle = request.Handle;
        headers.ContextValue = request.ContextValue;
        SteamCallbackManager::PushCallback(kHTTPRequestHeadersReceived, &headers, sizeof(headers));

        if (!request.Response.Body.empty())
        {
            HTTPRequestDataReceivedPayload data{};
            data.RequestHandle = request.Handle;
            data.ContextValue = request.ContextValue;
            data.Offset = 0;
            data.BytesReceived = static_cast<uint32_t>(request.Response.Body.size());
            SteamCallbackManager::PushCallback(kHTTPRequestDataReceived, &data, sizeof(data));
        }
    }
}
