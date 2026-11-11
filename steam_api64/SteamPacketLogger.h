#pragma once
#include "StdInc.h"

namespace SteamPacketLogger
{
    void LogSend(const char* source, CSteamID target, const void* data, uint32_t size, int channel);
    void LogReceive(const char* source, CSteamID sender, const void* data, uint32_t size, int channel);
}