#include "StdInc.h"
#include "SteamIDManager.h"
#include "SteamConfig.h"
#include "Logger.h"

namespace
{
    std::string g_FallbackPersona = "BreakersRevived";
    std::mutex g_EffectiveMutex;
    uint64_t g_EffectiveSteamID = 0;
    uint64_t g_EffectiveBaseSteamID = 0;
    DWORD g_EffectiveProcessID = 0;
    HANDLE g_InstanceSlotMutex = nullptr;
    uint32_t g_InstanceSlot = 0;

    void ReleaseInstanceSlot()
    {
        if (g_InstanceSlotMutex)
        {
            CloseHandle(g_InstanceSlotMutex);
            g_InstanceSlotMutex = nullptr;
        }

        g_InstanceSlot = 0;
    }

    uint32_t AcquireInstanceSlot(uint64_t baseID)
    {
        if (g_InstanceSlotMutex)
            return g_InstanceSlot;

        for (uint32_t slot = 1; slot <= 16; ++slot)
        {
            const std::string name =
                "Local\\BreakersRevivedSteamIDSlot_" +
                std::to_string(static_cast<unsigned long long>(baseID)) +
                "_" +
                std::to_string(slot);

            HANDLE handle = CreateMutexA(nullptr, TRUE, name.c_str());
            if (!handle)
                continue;

            if (GetLastError() != ERROR_ALREADY_EXISTS)
            {
                g_InstanceSlotMutex = handle;
                g_InstanceSlot = slot;
                return slot;
            }

            CloseHandle(handle);
        }

        return 1;
    }

    uint64_t BuildEffectiveSteamID()
    {
        const SteamConfig::Config& config = SteamConfig::Get();
        if (!config.AutoInstanceSteamID)
            return config.SteamID;

        return config.SteamID + AcquireInstanceSlot(config.SteamID);
    }
}

namespace SteamIDManager
{
    bool Init()
    {
        const uint64_t effectiveID = GetSteamID64();
        const uint64_t configuredID = SteamConfig::Get().SteamID;
        Logger::Info(
            "SteamIDManager initialized id=" +
            std::to_string(static_cast<unsigned long long>(effectiveID)) +
            " configured=" +
            std::to_string(static_cast<unsigned long long>(configuredID)) +
            " auto_instance=" +
            std::to_string(SteamConfig::Get().AutoInstanceSteamID ? 1 : 0));
        return true;
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(g_EffectiveMutex);
        ReleaseInstanceSlot();
        g_EffectiveSteamID = 0;
        g_EffectiveBaseSteamID = 0;
        g_EffectiveProcessID = 0;
    }

    CSteamID FromUint64(uint64_t value)
    {
        return static_cast<CSteamID>(value);
    }

    CSteamID GetLocalSteamID()
    {
        return FromUint64(GetSteamID64());
    }

    CSteamID GetLocalUser()
    {
        return GetLocalSteamID();
    }

    uint64_t ToUint64(CSteamID value)
    {
        return static_cast<uint64_t>(value);
    }

    uint64_t GetSteamID64()
    {
        const uint64_t baseID = SteamConfig::Get().SteamID;
        const DWORD processID = GetCurrentProcessId();

        std::lock_guard<std::mutex> lock(g_EffectiveMutex);
        if (g_EffectiveSteamID == 0 ||
            g_EffectiveBaseSteamID != baseID ||
            g_EffectiveProcessID != processID)
        {
            g_EffectiveBaseSteamID = baseID;
            g_EffectiveProcessID = processID;
            ReleaseInstanceSlot();
            g_EffectiveSteamID = BuildEffectiveSteamID();
        }

        return g_EffectiveSteamID;
    }

    uint32_t GetAccountID()
    {
        return static_cast<uint32_t>(GetSteamID64() & 0xFFFFFFFFu);
    }

    const char* GetPersonaName()
    {
        const std::string& name = SteamConfig::Get().PersonaName;
        return name.empty() ? g_FallbackPersona.c_str() : name.c_str();
    }

    const std::string& GetPersonaNameString()
    {
        const std::string& name = SteamConfig::Get().PersonaName;
        return name.empty() ? g_FallbackPersona : name;
    }

    void SetPersonaName(const std::string& name)
    {
        SteamConfig::Mutable().PersonaName = name.empty() ? g_FallbackPersona : name;
        SteamConfig::Save();
    }
}
