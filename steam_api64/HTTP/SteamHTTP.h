#pragma once
#include "StdInc.h"

class SteamHTTPInterface final
{
public:
    virtual uint32_t CreateHTTPRequest(int method, const char* absoluteUrl);
    virtual bool SetHTTPRequestContextValue(uint32_t request, uint64_t contextValue);
    virtual bool SetHTTPRequestNetworkActivityTimeout(uint32_t request, uint32_t timeoutSeconds);
    virtual bool SetHTTPRequestHeaderValue(uint32_t request, const char* headerName, const char* headerValue);
    virtual bool SetHTTPRequestGetOrPostParameter(uint32_t request, const char* parameterName, const char* parameterValue);
    virtual bool SendHTTPRequest(uint32_t request, SteamAPICall_t* callHandle);
    virtual bool SendHTTPRequestAndStreamResponse(uint32_t request, SteamAPICall_t* callHandle);
    virtual bool DeferHTTPRequest(uint32_t request);
    virtual bool PrioritizeHTTPRequest(uint32_t request);
    virtual bool GetHTTPResponseHeaderSize(uint32_t request, const char* headerName, uint32_t* responseHeaderSize);
    virtual bool GetHTTPResponseHeaderValue(uint32_t request, const char* headerName, unsigned char* buffer, uint32_t bufferSize);
    virtual bool GetHTTPResponseBodySize(uint32_t request, uint32_t* bodySize);
    virtual bool GetHTTPResponseBodyData(uint32_t request, unsigned char* buffer, uint32_t bufferSize);
    virtual bool GetHTTPStreamingResponseBodyData(uint32_t request, uint32_t offset, unsigned char* buffer, uint32_t bufferSize);
    virtual bool ReleaseHTTPRequest(uint32_t request);
    virtual bool GetHTTPDownloadProgressPct(uint32_t request, float* percentOut);
    virtual bool SetHTTPRequestRawPostBody(uint32_t request, const char* contentType, unsigned char* body, uint32_t bodyLength);
    virtual uint32_t CreateCookieContainer(bool allowResponsesToModify);
    virtual bool ReleaseCookieContainer(uint32_t cookieContainer);
    virtual bool SetCookie(uint32_t cookieContainer, const char* host, const char* url, const char* cookie);
    virtual bool SetHTTPRequestCookieContainer(uint32_t request, uint32_t cookieContainer);
    virtual bool SetHTTPRequestUserAgentInfo(uint32_t request, const char* userAgentInfo);
    virtual bool SetHTTPRequestRequiresVerifiedCertificate(uint32_t request, bool requireVerifiedCertificate);
    virtual bool SetHTTPRequestAbsoluteTimeoutMS(uint32_t request, uint32_t milliseconds);
    virtual bool GetHTTPRequestWasTimedOut(uint32_t request, bool* wasTimedOut);
};

namespace SteamHTTPEmulator
{
    void* GetInterface();
}
