#pragma once
#include "StdInc.h"
#include "HTTP/HttpRequest.h"

namespace HttpRequestManager
{
    uint32_t Create(int method, const char* url);
    bool SetContext(uint32_t handle, uint64_t context);
    bool SetNetworkTimeout(uint32_t handle, uint32_t seconds);
    bool SetHeader(uint32_t handle, const char* name, const char* value);
    bool SetParameter(uint32_t handle, const char* name, const char* value);
    bool SetRawPostBody(uint32_t handle, const char* contentType, const unsigned char* data, uint32_t size);
    bool SetUserAgentInfo(uint32_t handle, const char* value);
    bool SetCookieContainer(uint32_t handle, uint32_t cookieContainer);
    bool SetRequiresVerifiedCertificate(uint32_t handle, bool required);
    bool SetAbsoluteTimeout(uint32_t handle, uint32_t milliseconds);

    bool MarkStreaming(uint32_t handle, bool streaming);
    bool MarkSent(uint32_t handle);
    bool StoreResponse(uint32_t handle, const HttpResponse& response);

    bool Get(uint32_t handle, HttpRequest& request);
    bool GetResponse(uint32_t handle, HttpResponse& response);
    bool Release(uint32_t handle);
    bool Exists(uint32_t handle);
}
