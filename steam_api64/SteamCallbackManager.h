#pragma once
#include "StdInc.h"

struct BreakersCallbackMessage
{
    int CallbackID = 0;
    std::vector<unsigned char> Data;
};

namespace SteamCallbackManager
{
    bool Init();
    void RegisterCallback(void* callback, int callbackID);
    void UnregisterCallback(void* callback);
    void PushCallback(int callbackID, const void* data, size_t size);
    bool PopCallback(BreakersCallbackMessage& outMessage);
    int Count();
    void RunCallbacks();
}
