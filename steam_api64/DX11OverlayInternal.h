#pragma once

#include "StdInc.h"

#include "DX11Overlay.h"
#include "DX11OverlayState.h"

#include "Logger.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11.h>
#include <dxgi.h>
#include <algorithm>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Helpers
bool ActiveCheckbox(const char* label, bool* value, const char* help);
bool PlannedCheckbox(const char* label, bool* value, const char* reason);
void UpdateFps();
void UpdateHotkeys();
float AnimateToward(float current, float target, float speed);
void DrawTabButton(const char* label, int tab);
void DrawLeftButton(const char* label, int page);
bool IniCheckbox(const char* label, int& value);
bool IniCheckbox(const char* label, int& value, const char* help);
void HelpMarker(const char* text);

// Core
void CleanupRenderTarget();
bool CreateRenderTarget(IDXGISwapChain* swapChain);
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool HookGameWindow();
void ApplyStyle();
bool InitImGui(IDXGISwapChain* swapChain);
void DrawAlwaysStatusBar();
void DrawDropdownPanel();
void DrawAnimatedTopMenu();
void DrawImGui(IDXGISwapChain* swapChain);
void ShutdownImGui();
HRESULT __stdcall PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);
HRESULT __stdcall ResizeBuffersHook(
    IDXGISwapChain* swapChain,
    UINT bufferCount,
    UINT width,
    UINT height,
    DXGI_FORMAT newFormat,
    UINT swapChainFlags);
HWND CreateDummyWindow();
bool CreateDummySwapChain(IDXGISwapChain** outSwapChain);

// Pages
void DrawClientPage();
void DrawOnlinePage();
