#pragma once
#include "StdInc.h"
#include "HTTP/HttpRequest.h"

namespace HttpCallbacks
{
    SteamAPICall_t QueueCompleted(const HttpRequest& request);
    void QueueStreaming(const HttpRequest& request);
}
