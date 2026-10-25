#pragma once
#include "StdInc.h"

struct HttpResponse
{
    bool RequestSuccessful = false;
    int StatusCode = 0;
    bool TimedOut = false;
    std::vector<unsigned char> Body;
    std::map<std::string, std::string> Headers;

    void Clear();
    bool TryGetHeader(const std::string& name, std::string& value) const;
};
