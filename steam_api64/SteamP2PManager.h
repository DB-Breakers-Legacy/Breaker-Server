#pragma once
#include "StdInc.h"

struct BreakersP2PPacket
{
    CSteamID Sender = 0;
    CSteamID Target = 0;
    int Channel = 0;
    std::vector<uint8_t> Data;
};

namespace SteamP2PManager
{
    void Init();

    bool Send(CSteamID target, const void* data, uint32_t size, int channel);
    bool HasPacket(uint32_t* size, int channel);
    bool Read(void* dest, uint32_t destSize, uint32_t* msgSize, CSteamID* sender, int channel);

    void PushIncoming(CSteamID sender, const void* data, uint32_t size, int channel);
}