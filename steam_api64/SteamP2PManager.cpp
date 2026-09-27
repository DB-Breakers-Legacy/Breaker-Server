#include "StdInc.h"
#include "SteamP2PManager.h"
#include "SteamIDManager.h"
#include "SteamOfflineServer.h"
#include "SteamPacketLogger.h"
#include "Logger.h"

static std::mutex g_Mutex;
static std::vector<BreakersP2PPacket> g_Incoming;
static std::atomic<uint64_t> g_LastPollMs{ 0 };

namespace
{
    void PollOfflineServerThrottled(uint64_t minIntervalMs = 5)
    {
        const uint64_t now = static_cast<uint64_t>(GetTickCount64());
        uint64_t previous = g_LastPollMs.load(std::memory_order_relaxed);

        if (now - previous < minIntervalMs)
            return;

        if (!g_LastPollMs.compare_exchange_strong(
            previous,
            now,
            std::memory_order_relaxed,
            std::memory_order_relaxed))
        {
            return;
        }

        SteamOfflineServer::Poll();
    }
}

namespace SteamP2PManager
{
    void Init()
    {
        SteamOfflineServer::Init();
        Logger::Info("SteamP2PManager initialized");
    }

    bool Send(CSteamID target, const void* data, uint32_t size, int channel)
    {
        if (!data || size == 0)
            return false;

        SteamPacketLogger::LogSend("SteamP2PManager", target, data, size, channel);

        if (target == 0 || target == SteamIDManager::GetLocalSteamID())
            PushIncoming(SteamIDManager::GetLocalSteamID(), data, size, channel);

        return SteamOfflineServer::SendP2PPacket(target, data, size, channel);
    }

    bool HasPacket(uint32_t* size, int channel)
    {
        PollOfflineServerThrottled();

        std::lock_guard<std::mutex> lock(g_Mutex);

        for (const BreakersP2PPacket& packet : g_Incoming)
        {
            if (packet.Channel == channel)
            {
                if (size)
                    *size = static_cast<uint32_t>(packet.Data.size());

                return true;
            }
        }

        if (size)
            *size = 0;

        return false;
    }

    bool Read(void* dest, uint32_t destSize, uint32_t* msgSize, CSteamID* sender, int channel)
    {
        PollOfflineServerThrottled(0);

        std::lock_guard<std::mutex> lock(g_Mutex);

        for (auto it = g_Incoming.begin(); it != g_Incoming.end(); ++it)
        {
            if (it->Channel != channel)
                continue;

            uint32_t size = static_cast<uint32_t>(it->Data.size());

            if (msgSize)
                *msgSize = size;

            if (sender)
                *sender = it->Sender;

            if (!dest || destSize < size)
                return false;

            memcpy(dest, it->Data.data(), size);

            SteamPacketLogger::LogReceive("SteamP2PManager", it->Sender, dest, size, channel);

            g_Incoming.erase(it);
            return true;
        }

        if (msgSize)
            *msgSize = 0;

        return false;
    }

    void PushIncoming(CSteamID sender, const void* data, uint32_t size, int channel)
    {
        if (!data || size == 0)
            return;

        std::lock_guard<std::mutex> lock(g_Mutex);

        BreakersP2PPacket packet{};
        packet.Sender = sender;
        packet.Target = SteamIDManager::GetLocalSteamID();
        packet.Channel = channel;
        packet.Data.resize(size);

        memcpy(packet.Data.data(), data, size);

        g_Incoming.push_back(packet);
    }
}
