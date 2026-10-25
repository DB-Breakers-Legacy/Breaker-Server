#include "StdInc.h"
#include "WindowedFullscreen.h"
#include "Logger.h"

static HWND g_GameWindow = nullptr;
static WNDPROC g_OriginalWndProc = nullptr;
static bool g_Applied = false;
static DWORD g_BackgroundKeepAliveArmTick = 0;
static bool g_BackgroundKeepAliveLogged = false;

extern bool g_PatchBackgroundPauseFix;

static bool BackgroundKeepAliveReady()
{
    if (!g_PatchBackgroundPauseFix || g_BackgroundKeepAliveArmTick == 0)
        return false;

    const DWORD elapsed = GetTickCount() - g_BackgroundKeepAliveArmTick;
    const bool ready = elapsed >= 5000;

    if (ready && !g_BackgroundKeepAliveLogged)
    {
        g_BackgroundKeepAliveLogged = true;
        Logger::Info("Background keep-alive armed without controller focus sharing");
    }

    return ready;
}

static LRESULT CALLBACK GameWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ACTIVATEAPP:
        if (wParam == FALSE && BackgroundKeepAliveReady())
            return CallWindowProcA(g_OriginalWndProc, hwnd, msg, TRUE, lParam);

        return CallWindowProcA(g_OriginalWndProc, hwnd, msg, wParam, lParam);

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && BackgroundKeepAliveReady())
            return CallWindowProcA(g_OriginalWndProc, hwnd, msg, MAKEWPARAM(WA_ACTIVE, HIWORD(wParam)), lParam);

        return CallWindowProcA(g_OriginalWndProc, hwnd, msg, wParam, lParam);

    case WM_KILLFOCUS:
        if (BackgroundKeepAliveReady())
            return 0;

        return CallWindowProcA(g_OriginalWndProc, hwnd, msg, wParam, lParam);

    case WM_SETFOCUS:
        return CallWindowProcA(g_OriginalWndProc, hwnd, msg, wParam, lParam);

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE)
            return 0;
        break;
    }

    return CallWindowProcA(g_OriginalWndProc, hwnd, msg, wParam, lParam);
}

static BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM)
{
    DWORD windowProcessId = 0;
    GetWindowThreadProcessId(hwnd, &windowProcessId);

    if (windowProcessId != GetCurrentProcessId())
        return TRUE;

    if (!IsWindowVisible(hwnd))
        return TRUE;

    char title[256]{};
    GetWindowTextA(hwnd, title, sizeof(title));

    if (strlen(title) <= 0)
        return TRUE;

    if (strstr(title, "Breakers Revival Offline Debug Console"))
        return TRUE;

    if (strstr(title, "DRAGON BALL") ||
        strstr(title, "THE BREAKERS") ||
        strstr(title, "Breakers"))
    {
        g_GameWindow = hwnd;
        return FALSE;
    }

    g_GameWindow = hwnd;
    return FALSE;
}

static HWND FindGameWindow()
{
    g_GameWindow = nullptr;
    EnumWindows(EnumWindowsCallback, 0);
    return g_GameWindow;
}

static void HookGameWindowProc(HWND hwnd)
{
    if (!hwnd || g_OriginalWndProc)
        return;

    g_OriginalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GameWindowProc)));

    if (g_OriginalWndProc)
        Logger::Info("Game window procedure hooked for borderless fullscreen");
    else
        Logger::Error("Failed to hook game window procedure");
}

static bool IsBorderlessFullscreenApplied(HWND hwnd, const RECT& rect)
{
    if (!hwnd)
        return false;

    const LONG_PTR style = GetWindowLongPtrA(hwnd, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);

    if ((style & (WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU)) != 0)
        return false;

    if ((exStyle & (WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE)) != 0)
        return false;

    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect))
        return false;

    return windowRect.left == rect.left &&
        windowRect.top == rect.top &&
        windowRect.right == rect.right &&
        windowRect.bottom == rect.bottom;
}

static void ApplyBorderlessFullscreen(HWND hwnd)
{
    if (!hwnd)
        return;

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(MONITORINFO);

    if (!GetMonitorInfoA(monitor, &monitorInfo))
        return;

    const RECT& rect = monitorInfo.rcMonitor;

    if (g_Applied && IsBorderlessFullscreenApplied(hwnd, rect))
        return;

    LONG_PTR style = GetWindowLongPtrA(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);

    style &= ~WS_CAPTION;
    style &= ~WS_THICKFRAME;
    style &= ~WS_MINIMIZEBOX;
    style &= ~WS_MAXIMIZEBOX;
    style &= ~WS_SYSMENU;

    exStyle &= ~WS_EX_DLGMODALFRAME;
    exStyle &= ~WS_EX_CLIENTEDGE;
    exStyle &= ~WS_EX_STATICEDGE;
    exStyle &= ~WS_EX_WINDOWEDGE;

    SetWindowLongPtrA(hwnd, GWL_STYLE, style);
    SetWindowLongPtrA(hwnd, GWL_EXSTYLE, exStyle);

    ShowWindow(hwnd, SW_RESTORE);

    SetWindowPos(
        hwnd,
        HWND_TOP,
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        SWP_NOOWNERZORDER |
        SWP_FRAMECHANGED |
        SWP_SHOWWINDOW);

    HookGameWindowProc(hwnd);

    if (!g_Applied)
    {
        g_BackgroundKeepAliveArmTick = GetTickCount();
        g_BackgroundKeepAliveLogged = false;
        Logger::Info("Windowed fullscreen applied to game window");
        Logger::Info("Background keep-alive will arm after startup audio grace");
    }
    else
    {
        Logger::Info("Windowed fullscreen corrected after game window changed");
    }

    g_Applied = true;
}

static DWORD WINAPI WindowedFullscreenThread(LPVOID)
{
    Logger::Info("Windowed fullscreen thread started");

    for (int i = 0; i < 120; i++)
    {
        HWND hwnd = FindGameWindow();

        if (hwnd)
        {
            ApplyBorderlessFullscreen(hwnd);
            Sleep(1000);
            continue;
        }

        Sleep(500);
    }

    if (!g_Applied)
        Logger::Error("Failed to find game window for windowed fullscreen");
    return 0;
}

namespace WindowedFullscreen
{
    void Init()
    {
        CreateThread(
            nullptr,
            0,
            WindowedFullscreenThread,
            nullptr,
            0,
            nullptr);
    }
}
