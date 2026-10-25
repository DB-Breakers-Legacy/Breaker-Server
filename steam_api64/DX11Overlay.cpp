#include "DX11OverlayInternal.h"
#include "MinHook.h"

namespace DX11Overlay
{
    bool Init()
    {
        if (g_HookReady)
            return true;

        IDXGISwapChain* dummySwapChain = nullptr;

        if (!CreateDummySwapChain(&dummySwapChain))
        {
            Logger::Error("DX11 overlay: failed to create dummy swapchain");
            return false;
        }

        void** vtable = *reinterpret_cast<void***>(dummySwapChain);

        void* presentTarget = vtable[8];
        void* resizeTarget = vtable[13];

        dummySwapChain->Release();

        if (MH_CreateHook(
            presentTarget,
            reinterpret_cast<void*>(PresentHook),
            reinterpret_cast<void**>(&g_OriginalPresent)) != MH_OK)
        {
            Logger::Error("DX11 overlay: failed to create Present hook");
            return false;
        }

        if (MH_EnableHook(presentTarget) != MH_OK)
        {
            Logger::Error("DX11 overlay: failed to enable Present hook");
            return false;
        }

        if (MH_CreateHook(
            resizeTarget,
            reinterpret_cast<void*>(ResizeBuffersHook),
            reinterpret_cast<void**>(&g_OriginalResizeBuffers)) != MH_OK)
        {
            Logger::Error("DX11 overlay: failed to create ResizeBuffers hook");
            return false;
        }

        if (MH_EnableHook(resizeTarget) != MH_OK)
        {
            Logger::Error("DX11 overlay: failed to enable ResizeBuffers hook");
            return false;
        }

        g_HookReady = true;
        Logger::Info("DX11 overlay initialized");
        return true;
    }

    void Shutdown()
    {
        ShutdownImGui();

        if (g_OriginalPresent)
            MH_DisableHook(reinterpret_cast<void*>(g_OriginalPresent));

        if (g_OriginalResizeBuffers)
            MH_DisableHook(reinterpret_cast<void*>(g_OriginalResizeBuffers));

        g_HookReady = false;
        Logger::Info("DX11 overlay shutdown");
    }
}
