#include "DX11OverlayInternal.h"
#include <unordered_set>

PresentFn g_OriginalPresent = nullptr;
ResizeBuffersFn g_OriginalResizeBuffers = nullptr;

bool g_HookReady = false;
bool g_PresentSeen = false;
bool g_ImGuiReady = false;
bool g_MenuOpen = false;
bool g_F1WasDown = false;

HWND g_GameWindow = nullptr;
WNDPROC g_OriginalWndProc = nullptr;

ID3D11Device* g_Device = nullptr;
ID3D11DeviceContext* g_Context = nullptr;
ID3D11RenderTargetView* g_RenderTargetView = nullptr;

LARGE_INTEGER g_Frequency{};
LARGE_INTEGER g_LastFpsTime{};
int g_FrameCounter = 0;
int g_CurrentFps = 0;
float g_FpsHistory[180]{};
int g_FpsHistoryOffset = 0;

int g_SelectedTab = TAB_NONE;
int g_SelectedSubPage = 0;
float g_MenuAnimation = 0.0f;

bool g_OverlayEnabled = true;
bool g_ShowStatusBar = true;
bool g_StatusShowName = true;
bool g_StatusShowBuild = true;
bool g_StatusShowFps = true;
bool g_StatusShowHotkey = true;
bool g_DrawMouseCursor = true;
bool g_BlockGameInputWhenOpen = true;

std::unordered_set<int> g_SelectedAssetIndexes;
int g_LastClickedAssetIndex = -1;

bool g_GraphicsWindowedFullscreen = true;
bool g_GraphicsShowFps = true;
bool g_GraphicsVsync = false;
bool g_GraphicsKeepOverlayAboveUi = true;
bool g_GraphicsFixResizeFlicker = true;
bool g_GraphicsBorderlessFix = true;

bool g_ControlsControllerEnabled = true;
bool g_ControlsCaptureMouse = true;
bool g_ControlsBlockInput = true;
bool g_ControlsKeyboardNav = true;

bool g_NetworkBlockTraffic = true;
bool g_NetworkCustomServer = false;
bool g_NetworkLogSockets = true;
bool g_NetworkLogDns = true;
bool g_NetworkLanOnly = false;

bool g_SteamStorageLocal = true;
bool g_SteamStatsEmulated = true;
bool g_SteamFriendsEmulated = true;
bool g_SteamMatchmakingEmulated = true;
bool g_SteamLogInterfaces = false;

bool g_ModsEnabled = true;
bool g_ModsLoadDll = true;
bool g_ModsVerboseLog = true;

bool g_GameSkipIntro = false;
bool g_GameAutoSkipCutscenes = false;
bool g_GameUnlockAllTemporary = false;
bool g_GameUnlockAllPersistent = false;
bool g_GameSaveBackupEnabled = true;
bool g_GameUseCustomSaveFolder = false;

bool g_CameraFreeCam = false;
bool g_CameraHideHud = false;
bool g_CameraAllowDuringCutscenes = false;
bool g_CameraCopyQueued = false;
float g_CameraFov = 60.0f;
float g_CameraSpeed = 1.0f;
float g_CameraBoost = 4.0f;
float g_CameraSmooth = 0.25f;
float g_CameraPos[3] = { 0.0f, 0.0f, 0.0f };
float g_CameraRot[3] = { 0.0f, 0.0f, 0.0f };

bool g_PatchBorderlessFullscreen = true;
bool g_PatchNativeUltrawide = false;
bool g_PatchMultiMonitor = false;
bool g_PatchDisableIntro = false;
bool g_PatchAutoSkipCutscenes = false;
bool g_PatchUnlockAllTemp = false;
bool g_PatchUnlockAllPermanent = false;
bool g_PatchHighRefresh = false;
bool g_PatchBackgroundPauseFix = true;
bool g_PatchSkipOnlineChecks = true;

bool g_DebugOverlayEnabled = true;
bool g_DebugShowOnlyFps = false;
bool g_DebugShowBuildDate = true;
bool g_DebugShowF1Hint = true;
bool g_DebugDrawMouseCursor = true;
bool g_DebugBlockInput = true;

bool g_DebugVerboseLog = false;
bool g_DebugFpsGraph = true;
bool g_DebugFrameTimeGraph = true;
bool g_DebugShowMinAvgMax = true;
bool g_DebugShowPresentCount = true;
bool g_DebugShowResizeCount = true;
bool g_DebugShowImGuiStats = true;

bool g_DebugShowRenderHandles = true;
bool g_DebugShowSwapchainInfo = true;
bool g_DebugShowWindowState = true;
bool g_DebugShowRefreshRate = true;

bool g_DebugLogSteamInterfaces = false;
bool g_DebugLogCallbacks = false;
bool g_DebugLogCallResults = false;
bool g_DebugShowSteamId = true;
bool g_DebugShowLobbyState = true;

bool g_DebugLogDns = true;
bool g_DebugLogSocketConnects = true;
bool g_DebugShowLastBlockedAddress = true;
bool g_DebugShowNetworkHookStatus = true;
bool g_DebugForceBlockTraffic = true;
bool g_DebugLanOnly = false;

bool g_DebugLogStorageReads = true;
bool g_DebugLogStorageWrites = true;
bool g_DebugShowStorageFolder = true;
bool g_DebugShowLastSaveFile = true;

bool g_DebugShowModsFolder = true;
bool g_DebugLogModErrors = true;
bool g_DebugShowLoadedModCount = true;

char g_AssetSearch[128]{};
int g_AssetTypeFilter = 0;
int g_SelectedAssetIndex = -1;
int g_AssetPreviewTab = 0;
bool g_AssetOverridesOnly = false;
bool g_AssetDumpedOnly = false;
bool g_AssetAutoPreview = true;
bool g_AssetAutoDumpBeforePreview = false;
bool g_AssetFullPathLabels = true;
bool g_AssetStartedAsyncScan = false;

char g_LuaSearch[128]{};
char g_LuaConsoleInput[1024]{};
int g_SelectedLuaScript = -1;




extern std::unordered_set<int> g_SelectedAssetIndexes;



std::string g_LuaScriptText;
