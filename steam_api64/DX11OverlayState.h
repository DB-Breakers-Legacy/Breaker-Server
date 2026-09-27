#pragma once
#include "StdInc.h"
#include "imgui.h"
#include <unordered_set>


#include <d3d11.h>
#include <dxgi.h>

typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);
typedef HRESULT(__stdcall* ResizeBuffersFn)(
    IDXGISwapChain* swapChain,
    UINT bufferCount,
    UINT width,
    UINT height,
    DXGI_FORMAT newFormat,
    UINT swapChainFlags);

enum DX11OverlayTab
{
    TAB_NONE     = -1,
    TAB_CLIENT   = 0,
    TAB_GRAPHICS = 1,
    TAB_CONTROLS = 2,
    TAB_GAME     = 3,
    TAB_ONLINE   = 4,
    TAB_CAMERA   = 5,
    TAB_ASSETS   = 6,
    TAB_MODS     = 7,
    TAB_PATCHES  = 8,
    TAB_DEBUG    = 9
};

extern PresentFn g_OriginalPresent;
extern ResizeBuffersFn g_OriginalResizeBuffers;

extern bool g_HookReady;
extern bool g_PresentSeen;
extern bool g_ImGuiReady;
extern bool g_MenuOpen;
extern bool g_F1WasDown;

extern HWND g_GameWindow;
extern WNDPROC g_OriginalWndProc;

extern std::unordered_set<int> g_SelectedAssetIndexes;
extern int g_LastClickedAssetIndex;

extern ID3D11Device* g_Device;
extern ID3D11DeviceContext* g_Context;
extern ID3D11RenderTargetView* g_RenderTargetView;

extern LARGE_INTEGER g_Frequency;
extern LARGE_INTEGER g_LastFpsTime;
extern int g_FrameCounter;
extern int g_CurrentFps;
extern float g_FpsHistory[180];
extern int g_FpsHistoryOffset;

extern int g_SelectedTab;
extern int g_SelectedSubPage;
extern float g_MenuAnimation;

extern bool g_OverlayEnabled;
extern bool g_ShowStatusBar;
extern bool g_StatusShowName;
extern bool g_StatusShowBuild;
extern bool g_StatusShowFps;
extern bool g_StatusShowHotkey;
extern bool g_DrawMouseCursor;
extern bool g_BlockGameInputWhenOpen;

extern bool g_GraphicsWindowedFullscreen;
extern bool g_GraphicsShowFps;
extern bool g_GraphicsVsync;
extern bool g_GraphicsKeepOverlayAboveUi;
extern bool g_GraphicsFixResizeFlicker;
extern bool g_GraphicsBorderlessFix;

extern bool g_ControlsControllerEnabled;
extern bool g_ControlsCaptureMouse;
extern bool g_ControlsBlockInput;
extern bool g_ControlsKeyboardNav;

extern bool g_NetworkBlockTraffic;
extern bool g_NetworkCustomServer;
extern bool g_NetworkLogSockets;
extern bool g_NetworkLogDns;
extern bool g_NetworkLanOnly;

extern bool g_SteamStorageLocal;
extern bool g_SteamStatsEmulated;
extern bool g_SteamFriendsEmulated;
extern bool g_SteamMatchmakingEmulated;
extern bool g_SteamLogInterfaces;

extern bool g_ModsEnabled;
extern bool g_ModsLoadDll;
extern bool g_ModsVerboseLog;

extern bool g_GameSkipIntro;
extern bool g_GameAutoSkipCutscenes;
extern bool g_GameUnlockAllTemporary;
extern bool g_GameUnlockAllPersistent;
extern bool g_GameSaveBackupEnabled;
extern bool g_GameUseCustomSaveFolder;

extern bool g_CameraFreeCam;
extern bool g_CameraHideHud;
extern bool g_CameraAllowDuringCutscenes;
extern bool g_CameraCopyQueued;
extern float g_CameraFov;
extern float g_CameraSpeed;
extern float g_CameraBoost;
extern float g_CameraSmooth;
extern float g_CameraPos[3];
extern float g_CameraRot[3];

extern bool g_PatchBorderlessFullscreen;
extern bool g_PatchNativeUltrawide;
extern bool g_PatchMultiMonitor;
extern bool g_PatchDisableIntro;
extern bool g_PatchAutoSkipCutscenes;
extern bool g_PatchUnlockAllTemp;
extern bool g_PatchUnlockAllPermanent;
extern bool g_PatchHighRefresh;
extern bool g_PatchBackgroundPauseFix;
extern bool g_PatchSkipOnlineChecks;

extern bool g_DebugOverlayEnabled;
extern bool g_DebugShowOnlyFps;
extern bool g_DebugShowBuildDate;
extern bool g_DebugShowF1Hint;
extern bool g_DebugDrawMouseCursor;
extern bool g_DebugBlockInput;

extern bool g_DebugVerboseLog;
extern bool g_DebugFpsGraph;
extern bool g_DebugFrameTimeGraph;
extern bool g_DebugShowMinAvgMax;
extern bool g_DebugShowPresentCount;
extern bool g_DebugShowResizeCount;
extern bool g_DebugShowImGuiStats;

extern bool g_DebugShowRenderHandles;
extern bool g_DebugShowSwapchainInfo;
extern bool g_DebugShowWindowState;
extern bool g_DebugShowRefreshRate;

extern bool g_DebugLogSteamInterfaces;
extern bool g_DebugLogCallbacks;
extern bool g_DebugLogCallResults;
extern bool g_DebugShowSteamId;
extern bool g_DebugShowLobbyState;

extern bool g_DebugLogDns;
extern bool g_DebugLogSocketConnects;
extern bool g_DebugShowLastBlockedAddress;
extern bool g_DebugShowNetworkHookStatus;
extern bool g_DebugForceBlockTraffic;
extern bool g_DebugLanOnly;

extern bool g_DebugLogStorageReads;
extern bool g_DebugLogStorageWrites;
extern bool g_DebugShowStorageFolder;
extern bool g_DebugShowLastSaveFile;

extern bool g_DebugShowModsFolder;
extern bool g_DebugLogModErrors;
extern bool g_DebugShowLoadedModCount;

extern char g_AssetSearch[128];
extern int g_AssetTypeFilter;
extern int g_SelectedAssetIndex;
extern int g_AssetPreviewTab;
extern bool g_AssetOverridesOnly;
extern bool g_AssetDumpedOnly;
extern bool g_AssetAutoPreview;
extern bool g_AssetAutoDumpBeforePreview;
extern bool g_AssetFullPathLabels;
extern bool g_AssetStartedAsyncScan;

extern char g_LuaSearch[128];
extern char g_LuaConsoleInput[1024];
extern int g_SelectedLuaScript;
extern std::string g_LuaScriptText;
