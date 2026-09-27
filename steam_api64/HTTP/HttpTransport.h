#pragma once
#include "StdInc.h"
#include "HTTP/HttpRequest.h"
#include "HTTP/HttpResponse.h"

namespace HttpTransport
{
    HttpResponse Execute(const HttpRequest& request);
}
