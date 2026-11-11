#pragma once
#include "StdInc.h"
#include "HTTP/HttpResponse.h"

struct HttpRequest
{
    uint32_t Handle = 0;
    int Method = 0;
    std::string Url;
    uint64_t ContextValue = 0;

    uint32_t NetworkActivityTimeoutSeconds = 60;
    uint32_t AbsoluteTimeoutMs = 0;
    bool RequireVerifiedCertificate = true;
    bool Sent = false;
    bool Streaming = false;

    uint32_t CookieContainer = 0;
    std::string UserAgentInfo;
    std::map<std::string, std::string> Headers;
    std::vector<std::pair<std::string, std::string>> Parameters;
    std::string RawPostContentType;
    std::vector<unsigned char> RawPostBody;

    HttpResponse Response;
};
