#include "StdInc.h"
#include "HTTP/HttpResponse.h"
#include <cctype>

namespace
{
    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }
}

void HttpResponse::Clear()
{
    RequestSuccessful = false;
    StatusCode = 0;
    TimedOut = false;
    Body.clear();
    Headers.clear();
}

bool HttpResponse::TryGetHeader(const std::string& name, std::string& value) const
{
    const auto it = Headers.find(Lower(name));
    if (it == Headers.end())
        return false;

    value = it->second;
    return true;
}
