#include "StdInc.h"
#include "OnlineCheckBypass.h"
#include "HookManager.h"
#include "Logger.h"

namespace
{
    constexpr std::uintptr_t kOnlineCheckRva = 0x57C2C0;
    constexpr std::uintptr_t kEosReadyCheckRva = 0x07D760;

    using OnlineCheckFn = char(__fastcall*)(std::int64_t, std::uint64_t*);
    using EosReadyCheckFn = char(__fastcall*)();

    void* g_OnlineCheckTarget = nullptr;
    void* g_EosReadyCheckTarget = nullptr;

    OnlineCheckFn g_OriginalOnlineCheck = nullptr;
    EosReadyCheckFn g_OriginalEosReadyCheck = nullptr;

    std::string g_SignatureReport = "Breakers startup bypass hooks have not been initialized.";

    char __fastcall ForceOnlineState(std::int64_t a1, std::uint64_t* a2)
    {
        NSR_UNUSED(a1);
        NSR_UNUSED(a2);
        return 1;
    }

    char __fastcall ForceEosReadyState()
    {
        return 1;
    }

    bool IsAddressInsideMainModule(std::uintptr_t address)
    {
        const auto module = GetModuleHandleW(nullptr);
        if (!module)
            return false;

        const auto base = reinterpret_cast<std::uintptr_t>(module);
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        const auto end = base + static_cast<std::uintptr_t>(nt->OptionalHeader.SizeOfImage);
        return address >= base && address < end;
    }

    bool CreateAndEnableHook(
        std::uintptr_t base,
        std::uintptr_t rva,
        void* detour,
        void** original,
        void** target,
        const char* name)
    {
        const auto targetAddress = base + rva;

        if (!IsAddressInsideMainModule(targetAddress))
        {
            std::ostringstream message;
            message << "OnlineCheckBypass: " << name
                    << " RVA 0x" << std::hex << std::uppercase << rva
                    << " is outside the loaded executable image";
            Logger::Error(message.str());
            return false;
        }

        *target = reinterpret_cast<void*>(targetAddress);

        if (!HookManager::Create(*target, detour, original))
        {
            std::ostringstream message;
            message << "OnlineCheckBypass: failed to create " << name
                    << " hook at RVA 0x" << std::hex << std::uppercase << rva;
            Logger::Error(message.str());
            *target = nullptr;
            *original = nullptr;
            return false;
        }

        if (!HookManager::Enable(*target))
        {
            std::ostringstream message;
            message << "OnlineCheckBypass: failed to enable " << name
                    << " hook at RVA 0x" << std::hex << std::uppercase << rva;
            Logger::Error(message.str());
            *target = nullptr;
            *original = nullptr;
            return false;
        }

        return true;
    }
}

bool OnlineCheckBypass::Init()
{
    const auto module = GetModuleHandleW(nullptr);
    if (!module)
    {
        g_SignatureReport = "Failed: main executable module was not available.";
        Logger::Error("OnlineCheckBypass: main executable module was not available");
        return false;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(module);

    const bool onlineHooked = CreateAndEnableHook(
        base,
        kOnlineCheckRva,
        reinterpret_cast<void*>(&ForceOnlineState),
        reinterpret_cast<void**>(&g_OriginalOnlineCheck),
        &g_OnlineCheckTarget,
        "sub_14057C2C0");

    const bool eosReadyHooked = CreateAndEnableHook(
        base,
        kEosReadyCheckRva,
        reinterpret_cast<void*>(&ForceEosReadyState),
        reinterpret_cast<void**>(&g_OriginalEosReadyCheck),
        &g_EosReadyCheckTarget,
        "sub_14007D760");

    if (!onlineHooked || !eosReadyHooked)
    {
        g_SignatureReport = "Failed: one or more Breakers forced-true startup hooks could not be installed.";
        return false;
    }

    g_SignatureReport =
        "Active: sub_14057C2C0 (RVA 0x57C2C0) and sub_14007D760 (RVA 0x7D760) are forced to return true.";

    Logger::Info("OnlineCheckBypass: sub_14057C2C0 forced to return true");
    Logger::Info("OnlineCheckBypass: sub_14007D760 EOS/EAC ready check forced to return true");
    return true;
}

void OnlineCheckBypass::RevalidateSignatures()
{
    if (!g_OnlineCheckTarget || !g_EosReadyCheckTarget)
    {
        g_SignatureReport = "Inactive: one or more forced-true startup hooks are not installed.";
        return;
    }

    const bool onlineValid = IsAddressInsideMainModule(
        reinterpret_cast<std::uintptr_t>(g_OnlineCheckTarget));
    const bool eosReadyValid = IsAddressInsideMainModule(
        reinterpret_cast<std::uintptr_t>(g_EosReadyCheckTarget));

    if (onlineValid && eosReadyValid)
    {
        g_SignatureReport =
            "Active: sub_14057C2C0 (RVA 0x57C2C0) and sub_14007D760 (RVA 0x7D760) are forced to return true.";
        return;
    }

    g_SignatureReport = "Warning: one or more hooked addresses are no longer inside the main executable image.";
}

const char* OnlineCheckBypass::SignatureReport()
{
    return g_SignatureReport.c_str();
}

void OnlineCheckBypass::Shutdown()
{
    if (g_EosReadyCheckTarget)
        HookManager::Disable(g_EosReadyCheckTarget);

    if (g_OnlineCheckTarget)
        HookManager::Disable(g_OnlineCheckTarget);

    g_EosReadyCheckTarget = nullptr;
    g_OnlineCheckTarget = nullptr;
    g_OriginalEosReadyCheck = nullptr;
    g_OriginalOnlineCheck = nullptr;
    g_SignatureReport = "Inactive: forced-true startup hooks were shut down.";
}
