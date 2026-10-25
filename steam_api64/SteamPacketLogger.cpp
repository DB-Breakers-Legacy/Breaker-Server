#include "StdInc.h"
#include "SteamPacketLogger.h"
#include "Logger.h"

static std::string HexPreview(const void* data, uint32_t size)
{
    if (!data || size == 0)
        return "";

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    uint32_t count = size > 16 ? 16 : size;

    std::ostringstream ss;

    for (uint32_t i = 0; i < count; i++)
    {
        char buffer[8]{};
        sprintf_s(buffer, "%02X ", bytes[i]);
        ss << buffer;
    }

    return ss.str();
}

namespace SteamPacketLogger
{
    void LogSend(const char* source, CSteamID target, const void* data, uint32_t size, int channel)
    {
        Logger::Info(
            std::string("[STEAM PACKET SEND] ") +
            (source ? source : "unknown") +
            " target=" +
            std::to_string(static_cast<uint64_t>(target)) +
            " size=" +
            std::to_string(size) +
            " channel=" +
            std::to_string(channel) +
            " preview=" +
            HexPreview(data, size));
    }

    void LogReceive(const char* source, CSteamID sender, const void* data, uint32_t size, int channel)
    {
        Logger::Info(
            std::string("[STEAM PACKET RECV] ") +
            (source ? source : "unknown") +
            " sender=" +
            std::to_string(static_cast<uint64_t>(sender)) +
            " size=" +
            std::to_string(size) +
            " channel=" +
            std::to_string(channel) +
            " preview=" +
            HexPreview(data, size));
    }
}