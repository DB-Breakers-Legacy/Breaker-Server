#include "DX11OverlayInternal.h"
#include "FrameworkAddon.h"
#include "MinHook.h"

namespace
{
    constexpr double kTargetFrameSeconds = 1.0 / 60.0;
    LARGE_INTEGER g_FramePaceFrequency{};
    LARGE_INTEGER g_LastPacedPresent{};
    bool g_FramePacerReady = false;
    bool g_TimerResolutionRaised = false;

    void RaiseTimerResolutionForPacing()
    {
        if (g_TimerResolutionRaised)
            return;

        HMODULE winmm = LoadLibraryA("winmm.dll");
        if (!winmm)
            return;

        using TimeBeginPeriodFn = UINT(WINAPI*)(UINT);
        TimeBeginPeriodFn timeBeginPeriodFn = reinterpret_cast<TimeBeginPeriodFn>(GetProcAddress(winmm, "timeBeginPeriod"));
        if (timeBeginPeriodFn && timeBeginPeriodFn(1) == 0)
            g_TimerResolutionRaised = true;
    }

    void ResetFramePacer()
    {
        g_LastPacedPresent.QuadPart = 0;
    }

    void PaceFrameForLowLatency60()
    {
        if (g_GraphicsVsync || !g_GraphicsWindowedFullscreen)
        {
            ResetFramePacer();
            return;
        }

        RaiseTimerResolutionForPacing();

        if (!g_FramePacerReady)
        {
            QueryPerformanceFrequency(&g_FramePaceFrequency);
            g_FramePacerReady = g_FramePaceFrequency.QuadPart > 0;
        }

        if (!g_FramePacerReady)
            return;

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);

        if (g_LastPacedPresent.QuadPart == 0)
        {
            g_LastPacedPresent = now;
            return;
        }

        const LONGLONG targetTicks = static_cast<LONGLONG>(static_cast<double>(g_FramePaceFrequency.QuadPart) * kTargetFrameSeconds);
        const LONGLONG targetPresent = g_LastPacedPresent.QuadPart + targetTicks;

        while (now.QuadPart < targetPresent)
        {
            const double remainingMs = static_cast<double>(targetPresent - now.QuadPart) * 1000.0 / static_cast<double>(g_FramePaceFrequency.QuadPart);

            if (remainingMs > 2.0)
                Sleep(1);
            else if (remainingMs > 0.25)
                Sleep(0);
            else
                YieldProcessor();

            QueryPerformanceCounter(&now);
        }

        if (now.QuadPart > targetPresent + (targetTicks * 4))
            g_LastPacedPresent = now;
        else
            g_LastPacedPresent.QuadPart = targetPresent;
    }

    UINT EffectivePresentSyncInterval(UINT requestedSyncInterval)
    {
        if (g_GraphicsVsync)
            return requestedSyncInterval > 0 ? requestedSyncInterval : 1;

        return 0;
    }
}

void CleanupRenderTarget()
{
    SAFE_RELEASE(g_RenderTargetView);
}

bool CreateRenderTarget(IDXGISwapChain* swapChain)
{
    if (g_RenderTargetView)
        return true;

    if (!swapChain || !g_Device)
        return false;

    ID3D11Texture2D* backBuffer = nullptr;

    HRESULT hr = swapChain->GetBuffer(
        0,
        __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&backBuffer));

    if (FAILED(hr) || !backBuffer)
    {
        Logger::Error("DX11 overlay: failed to get backbuffer");
        return false;
    }

    hr = g_Device->CreateRenderTargetView(backBuffer, nullptr, &g_RenderTargetView);
    backBuffer->Release();

    if (FAILED(hr) || !g_RenderTargetView)
    {
        Logger::Error("DX11 overlay: failed to create render target view");
        return false;
    }

    Logger::Info("DX11 overlay: render target created");
    return true;
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_MenuOpen && g_ImGuiReady)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = g_DrawMouseCursor;

        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
            return TRUE;

        if (g_BlockGameInputWhenOpen)
        {
            switch (msg)
            {
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_CHAR:
                return TRUE;
            }
        }
    }

    if (g_OriginalWndProc)
        return CallWindowProcA(g_OriginalWndProc, hwnd, msg, wParam, lParam);

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

bool HookGameWindow()
{
    if (!g_GameWindow || g_OriginalWndProc)
        return true;

    g_OriginalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(
            g_GameWindow,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(OverlayWndProc)));

    if (!g_OriginalWndProc)
    {
        Logger::Error("DX11 overlay: failed to hook game window proc");
        return false;
    }

    Logger::Info("DX11 overlay: game window proc hooked");
    return true;
}

void ApplyStyle()
{
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = ImVec2(9.0f, 8.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.025f, 0.030f, 0.038f, 0.94f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.015f, 0.018f, 0.024f, 0.62f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.020f, 0.024f, 0.032f, 0.98f);
    colors[ImGuiCol_Button] = ImVec4(0.10f, 0.28f, 0.50f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.16f, 0.40f, 0.70f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.48f, 0.84f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.32f, 0.62f, 1.00f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.12f, 0.32f, 0.56f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.44f, 0.74f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.22f, 0.50f, 0.86f, 1.00f);
}

bool InitImGui(IDXGISwapChain* swapChain)
{
    if (g_ImGuiReady)
        return true;

    if (!swapChain)
        return false;

    HRESULT hr = swapChain->GetDevice(
        __uuidof(ID3D11Device),
        reinterpret_cast<void**>(&g_Device));

    if (FAILED(hr) || !g_Device)
    {
        Logger::Error("DX11 overlay: failed to get D3D11 device");
        return false;
    }

    g_Device->GetImmediateContext(&g_Context);

    if (!g_Context)
    {
        Logger::Error("DX11 overlay: failed to get D3D11 context");
        SAFE_RELEASE(g_Device);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    hr = swapChain->GetDesc(&desc);

    if (FAILED(hr) || !desc.OutputWindow)
    {
        Logger::Error("DX11 overlay: failed to get game window from swapchain");
        SAFE_RELEASE(g_Context);
        SAFE_RELEASE(g_Device);
        return false;
    }

    g_GameWindow = desc.OutputWindow;

    if (!CreateRenderTarget(swapChain))
    {
        SAFE_RELEASE(g_Context);
        SAFE_RELEASE(g_Device);
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.MouseDrawCursor = true;

    ApplyStyle();

    ImGui_ImplWin32_Init(g_GameWindow);
    ImGui_ImplDX11_Init(g_Device, g_Context);

    HookGameWindow();

    g_ImGuiReady = true;

    Logger::Info("DX11 overlay: ImGui initialized in-game");
    return true;
}

void ShutdownImGui()
{
    FrameworkAddon::Shutdown();

    if (g_ImGuiReady)
    {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_ImGuiReady = false;
    }

    if (g_GameWindow && g_OriginalWndProc)
    {
        SetWindowLongPtrA(g_GameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_OriginalWndProc));
        g_OriginalWndProc = nullptr;
    }

    CleanupRenderTarget();

    SAFE_RELEASE(g_Context);
    SAFE_RELEASE(g_Device);
}

void DrawAlwaysStatusBar()
{
    if (!g_OverlayEnabled || !g_ShowStatusBar)
        return;

    ImGui::SetNextWindowPos(ImVec2(6.0f, 5.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.42f);

    ImGui::Begin(
        "##BreakersStatusOnly",
        nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav);

    std::string text;

    if (g_StatusShowName)
        text += "Breakers Revived";

    if (g_StatusShowBuild)
    {
        if (!text.empty()) text += " | ";
        text += "Build ";
        text += __DATE__;
        text += " ";
        text += __TIME__;
    }

    if (g_StatusShowFps)
    {
        if (!text.empty()) text += " | ";
        text += "FPS ";
        text += std::to_string(g_CurrentFps);
    }

    if (g_StatusShowHotkey)
    {
        if (!text.empty()) text += " | ";
        text += "F1";
    }

    ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

void DrawImGui(IDXGISwapChain* swapChain)
{
    if (!InitImGui(swapChain))
        return;

    if (!CreateRenderTarget(swapChain))
        return;

    UpdateFps();
    UpdateHotkeys();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    DrawAlwaysStatusBar();
    DrawAnimatedTopMenu();

    ImGui::Render();

    g_Context->OMSetRenderTargets(1, &g_RenderTargetView, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

HRESULT __stdcall PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    g_PresentSeen = true;
    const bool loginGateActive = false;

    if (g_OverlayEnabled)
        DrawImGui(swapChain);

    if (loginGateActive)
        Sleep(33);

    PaceFrameForLowLatency60();

    return g_OriginalPresent(swapChain, loginGateActive ? 1 : EffectivePresentSyncInterval(syncInterval), flags);
}

HRESULT __stdcall ResizeBuffersHook(
    IDXGISwapChain* swapChain,
    UINT bufferCount,
    UINT width,
    UINT height,
    DXGI_FORMAT newFormat,
    UINT swapChainFlags)
{
    CleanupRenderTarget();

    return g_OriginalResizeBuffers(
        swapChain,
        bufferCount,
        width,
        height,
        newFormat,
        swapChainFlags);
}

HWND CreateDummyWindow()
{
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "BreakersRevivedDummyDX11Window";

    RegisterClassExA(&wc);

    return CreateWindowA(
        wc.lpszClassName,
        "BreakersRevivedDummyDX11Window",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        100,
        100,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr);
}

bool CreateDummySwapChain(IDXGISwapChain** outSwapChain)
{
    *outSwapChain = nullptr;

    HWND hwnd = CreateDummyWindow();

    if (!hwnd)
        return false;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 100;
    sd.BufferDesc.Height = 100;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel{};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &sd,
        outSwapChain,
        &device,
        &featureLevel,
        &context);

    SAFE_RELEASE(context);
    SAFE_RELEASE(device);

    DestroyWindow(hwnd);
    UnregisterClassA("BreakersRevivedDummyDX11Window", GetModuleHandleA(nullptr));

    return SUCCEEDED(hr) && *outSwapChain;
}

