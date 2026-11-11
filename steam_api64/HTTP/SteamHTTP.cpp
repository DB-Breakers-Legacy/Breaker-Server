#include "StdInc.h"
#include "HTTP/SteamHTTP.h"
#include "HTTP/HttpRequestManager.h"
#include "HTTP/HttpTransport.h"
#include "HTTP/HttpCallbacks.h"
#include "HTTP/HttpCookieStore.h"
#include "SteamVersionLogger.h"
#include "Logger.h"

namespace
{
    SteamHTTPInterface g_SteamHTTP;

    void Trace(const char* functionName, const std::string& detail = {})
    {
        SteamVersionLogger::LogCall("SteamHTTP", functionName, detail);
    }

    bool ExecuteRequest(uint32_t handle, bool streaming, SteamAPICall_t* callHandle)
    {
        if (!callHandle)
            return false;

        *callHandle = 0;

        if (!HttpRequestManager::MarkStreaming(handle, streaming))
            return false;

        HttpRequest request;
        if (!HttpRequestManager::Get(handle, request))
            return false;

        if (!HttpRequestManager::MarkSent(handle))
            return false;

        request.Sent = true;
        request.Streaming = streaming;
        request.Response = HttpTransport::Execute(request);

        if (!HttpRequestManager::StoreResponse(handle, request.Response))
            return false;

        if (!HttpRequestManager::Get(handle, request))
            return false;

        if (streaming)
            HttpCallbacks::QueueStreaming(request);

        *callHandle = HttpCallbacks::QueueCompleted(request);
        return *callHandle != 0;
    }
}

uint32_t SteamHTTPInterface::CreateHTTPRequest(int method, const char* absoluteUrl)
{
    const uint32_t handle = HttpRequestManager::Create(method, absoluteUrl);
    Trace("CreateHTTPRequest", "method=" + std::to_string(method) + " handle=" + std::to_string(handle) + " url=" + (absoluteUrl ? absoluteUrl : "null"));
    return handle;
}

bool SteamHTTPInterface::SetHTTPRequestContextValue(uint32_t request, uint64_t contextValue)
{
    Trace("SetHTTPRequestContextValue", "handle=" + std::to_string(request) + " context=" + std::to_string(contextValue));
    return HttpRequestManager::SetContext(request, contextValue);
}

bool SteamHTTPInterface::SetHTTPRequestNetworkActivityTimeout(uint32_t request, uint32_t timeoutSeconds)
{
    Trace("SetHTTPRequestNetworkActivityTimeout", "handle=" + std::to_string(request) + " seconds=" + std::to_string(timeoutSeconds));
    return HttpRequestManager::SetNetworkTimeout(request, timeoutSeconds);
}

bool SteamHTTPInterface::SetHTTPRequestHeaderValue(uint32_t request, const char* headerName, const char* headerValue)
{
    Trace("SetHTTPRequestHeaderValue", "handle=" + std::to_string(request) + " name=" + (headerName ? headerName : "null"));
    return HttpRequestManager::SetHeader(request, headerName, headerValue);
}

bool SteamHTTPInterface::SetHTTPRequestGetOrPostParameter(uint32_t request, const char* parameterName, const char* parameterValue)
{
    Trace("SetHTTPRequestGetOrPostParameter", "handle=" + std::to_string(request) + " name=" + (parameterName ? parameterName : "null"));
    return HttpRequestManager::SetParameter(request, parameterName, parameterValue);
}

bool SteamHTTPInterface::SendHTTPRequest(uint32_t request, SteamAPICall_t* callHandle)
{
    Trace("SendHTTPRequest", "handle=" + std::to_string(request));
    return ExecuteRequest(request, false, callHandle);
}

bool SteamHTTPInterface::SendHTTPRequestAndStreamResponse(uint32_t request, SteamAPICall_t* callHandle)
{
    Trace("SendHTTPRequestAndStreamResponse", "handle=" + std::to_string(request));
    return ExecuteRequest(request, true, callHandle);
}

bool SteamHTTPInterface::DeferHTTPRequest(uint32_t request)
{
    Trace("DeferHTTPRequest", "handle=" + std::to_string(request));
    HttpRequest value;
    return HttpRequestManager::Get(request, value) && value.Sent;
}

bool SteamHTTPInterface::PrioritizeHTTPRequest(uint32_t request)
{
    Trace("PrioritizeHTTPRequest", "handle=" + std::to_string(request));
    HttpRequest value;
    return HttpRequestManager::Get(request, value) && value.Sent;
}

bool SteamHTTPInterface::GetHTTPResponseHeaderSize(uint32_t request, const char* headerName, uint32_t* responseHeaderSize)
{
    if (!headerName || !responseHeaderSize)
        return false;

    Trace("GetHTTPResponseHeaderSize", "handle=" + std::to_string(request) + " name=" + headerName);

    HttpResponse response;
    std::string value;
    if (!HttpRequestManager::GetResponse(request, response) || !response.TryGetHeader(headerName, value))
        return false;

    *responseHeaderSize = static_cast<uint32_t>(value.size() + 1);
    return true;
}

bool SteamHTTPInterface::GetHTTPResponseHeaderValue(uint32_t request, const char* headerName, unsigned char* buffer, uint32_t bufferSize)
{
    if (!headerName || !buffer)
        return false;

    Trace("GetHTTPResponseHeaderValue", "handle=" + std::to_string(request) + " name=" + headerName);

    HttpResponse response;
    std::string value;
    if (!HttpRequestManager::GetResponse(request, response) || !response.TryGetHeader(headerName, value))
        return false;

    const uint32_t required = static_cast<uint32_t>(value.size() + 1);
    if (bufferSize < required)
        return false;

    std::memcpy(buffer, value.c_str(), required);
    return true;
}

bool SteamHTTPInterface::GetHTTPResponseBodySize(uint32_t request, uint32_t* bodySize)
{
    if (!bodySize)
        return false;

    Trace("GetHTTPResponseBodySize", "handle=" + std::to_string(request));

    HttpResponse response;
    if (!HttpRequestManager::GetResponse(request, response))
        return false;

    *bodySize = static_cast<uint32_t>(response.Body.size());
    return true;
}

bool SteamHTTPInterface::GetHTTPResponseBodyData(uint32_t request, unsigned char* buffer, uint32_t bufferSize)
{
    Trace("GetHTTPResponseBodyData", "handle=" + std::to_string(request) + " bytes=" + std::to_string(bufferSize));

    HttpRequest value;
    if (!HttpRequestManager::Get(request, value))
        return false;

    const bool traceGet = value.Method == 1;
    if (!value.Sent || value.Streaming)
    {
        if (traceGet)
        {
            Logger::Info(
                "SteamHTTP GET body rejected handle=" + std::to_string(request) +
                " sent=" + (value.Sent ? "1" : "0") +
                " streaming=" + (value.Streaming ? "1" : "0"));
        }
        return false;
    }

    const auto& body = value.Response.Body;
    if (body.size() != bufferSize)
    {
        if (traceGet)
        {
            Logger::Info(
                "SteamHTTP GET body size mismatch handle=" + std::to_string(request) +
                " requested=" + std::to_string(bufferSize) +
                " stored=" + std::to_string(static_cast<unsigned long long>(body.size())));
        }
        return false;
    }

    if (!body.empty() && !buffer)
    {
        if (traceGet)
            Logger::Info("SteamHTTP GET body rejected null buffer handle=" + std::to_string(request));
        return false;
    }

    if (!body.empty())
        std::memcpy(buffer, body.data(), body.size());

    if (traceGet)
    {
        Logger::Info(
            "SteamHTTP GET body copied handle=" + std::to_string(request) +
            " bytes=" + std::to_string(static_cast<unsigned long long>(body.size())));
    }

    return true;
}

bool SteamHTTPInterface::GetHTTPStreamingResponseBodyData(uint32_t request, uint32_t offset, unsigned char* buffer, uint32_t bufferSize)
{
    Trace("GetHTTPStreamingResponseBodyData", "handle=" + std::to_string(request) + " offset=" + std::to_string(offset) + " bytes=" + std::to_string(bufferSize));

    HttpRequest value;
    if (!HttpRequestManager::Get(request, value) || !value.Sent || !value.Streaming)
        return false;
    if (!buffer && bufferSize > 0)
        return false;
    if (static_cast<uint64_t>(offset) + bufferSize > value.Response.Body.size())
        return false;

    if (bufferSize > 0)
        std::memcpy(buffer, value.Response.Body.data() + offset, bufferSize);
    return true;
}

bool SteamHTTPInterface::ReleaseHTTPRequest(uint32_t request)
{
    Trace("ReleaseHTTPRequest", "handle=" + std::to_string(request));
    return HttpRequestManager::Release(request);
}

bool SteamHTTPInterface::GetHTTPDownloadProgressPct(uint32_t request, float* percentOut)
{
    if (!percentOut)
        return false;

    HttpRequest value;
    if (!HttpRequestManager::Get(request, value))
        return false;

    *percentOut = value.Sent ? 1.0f : 0.0f;
    return true;
}

bool SteamHTTPInterface::SetHTTPRequestRawPostBody(uint32_t request, const char* contentType, unsigned char* body, uint32_t bodyLength)
{
    Trace("SetHTTPRequestRawPostBody", "handle=" + std::to_string(request) + " bytes=" + std::to_string(bodyLength));
    return HttpRequestManager::SetRawPostBody(request, contentType, body, bodyLength);
}

uint32_t SteamHTTPInterface::CreateCookieContainer(bool allowResponsesToModify)
{
    const uint32_t handle = HttpCookieStore::Create(allowResponsesToModify);
    Trace("CreateCookieContainer", "handle=" + std::to_string(handle));
    return handle;
}

bool SteamHTTPInterface::ReleaseCookieContainer(uint32_t cookieContainer)
{
    Trace("ReleaseCookieContainer", "handle=" + std::to_string(cookieContainer));
    return HttpCookieStore::Release(cookieContainer);
}

bool SteamHTTPInterface::SetCookie(uint32_t cookieContainer, const char* host, const char* url, const char* cookie)
{
    Trace("SetCookie", "handle=" + std::to_string(cookieContainer));
    return HttpCookieStore::Set(cookieContainer, host, url, cookie);
}

bool SteamHTTPInterface::SetHTTPRequestCookieContainer(uint32_t request, uint32_t cookieContainer)
{
    Trace("SetHTTPRequestCookieContainer", "handle=" + std::to_string(request) + " cookie=" + std::to_string(cookieContainer));
    return HttpRequestManager::SetCookieContainer(request, cookieContainer);
}

bool SteamHTTPInterface::SetHTTPRequestUserAgentInfo(uint32_t request, const char* userAgentInfo)
{
    Trace("SetHTTPRequestUserAgentInfo", "handle=" + std::to_string(request));
    return HttpRequestManager::SetUserAgentInfo(request, userAgentInfo);
}

bool SteamHTTPInterface::SetHTTPRequestRequiresVerifiedCertificate(uint32_t request, bool requireVerifiedCertificate)
{
    Trace("SetHTTPRequestRequiresVerifiedCertificate", "handle=" + std::to_string(request) + " value=" + (requireVerifiedCertificate ? "1" : "0"));
    return HttpRequestManager::SetRequiresVerifiedCertificate(request, requireVerifiedCertificate);
}

bool SteamHTTPInterface::SetHTTPRequestAbsoluteTimeoutMS(uint32_t request, uint32_t milliseconds)
{
    Trace("SetHTTPRequestAbsoluteTimeoutMS", "handle=" + std::to_string(request) + " ms=" + std::to_string(milliseconds));
    return HttpRequestManager::SetAbsoluteTimeout(request, milliseconds);
}

bool SteamHTTPInterface::GetHTTPRequestWasTimedOut(uint32_t request, bool* wasTimedOut)
{
    if (!wasTimedOut)
        return false;

    HttpResponse response;
    if (!HttpRequestManager::GetResponse(request, response))
        return false;

    *wasTimedOut = response.TimedOut;
    return true;
}

namespace SteamHTTPEmulator
{
    void* GetInterface()
    {
        return &g_SteamHTTP;
    }
}
