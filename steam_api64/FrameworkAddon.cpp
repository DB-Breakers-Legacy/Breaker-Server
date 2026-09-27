#include "StdInc.h"
#include "FrameworkAddon.h"
#include "DX11OverlayState.h"
#include "Logger.h"
#include "imgui.h"

#include <filesystem>
#include <string>

namespace
{
    using GetApiVersionFn = int(__cdecl*)();
    using GetDisplayNameFn = const char*(__cdecl*)();
    using BindImGuiContextFn = void(__cdecl*)(ImGuiContext*);
    using BindClientRenderStateFn = void(__cdecl*)(ID3D11Device*, ID3D11DeviceContext*, HWND, int);
    using DrawGuiFn = void(__cdecl*)();
    using DrawTabFn = void(__cdecl*)(int);
    using ShutdownFn = void(__cdecl*)();

    HMODULE g_Module = nullptr;
    GetApiVersionFn g_GetApiVersion = nullptr;
    GetDisplayNameFn g_GetDisplayName = nullptr;
    BindImGuiContextFn g_BindImGuiContext = nullptr;
    BindClientRenderStateFn g_BindClientRenderState = nullptr;
    DrawGuiFn g_DrawGui = nullptr;
    DrawTabFn g_DrawTab = nullptr;
    ShutdownFn g_Shutdown = nullptr;
    bool g_LoadAttempted = false;
    std::string g_Status = "Not loaded";
    std::string g_Name = "BreakersFramework.dll";
    std::string g_Path;

    std::string ExeFolder()
    {
        char path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path().string();
    }

    FARPROC RequiredProc(const char* name)
    {
        FARPROC proc = GetProcAddress(g_Module, name);
        if (!proc)
            Logger::Error(std::string("BreakersFramework.dll missing export ") + name);
        return proc;
    }

    std::filesystem::path FrameworkPath()
    {
        return std::filesystem::path(ExeFolder()) / "BreakersRevived" / "Tools" / "BreakersFramework.dll";
    }
}

namespace FrameworkAddon
{
    bool IsAvailable()
    {
        return IsLoaded() || std::filesystem::exists(FrameworkPath());
    }

    bool IsLoaded()
    {
        return g_Module && g_DrawGui;
    }

    const char* Name()
    {
        return g_Name.c_str();
    }

    const char* StatusText()
    {
        return g_Status.c_str();
    }

    const char* PathText()
    {
        if (g_Path.empty())
            g_Path = FrameworkPath().string();

        return g_Path.c_str();
    }

    void EnsureLoaded()
    {
        Load();
    }

    bool Load()
    {
        if (IsLoaded())
            return true;

        const std::filesystem::path frameworkPath = FrameworkPath();
        g_Path = frameworkPath.string();

        if (!std::filesystem::exists(frameworkPath))
        {
            g_LoadAttempted = true;
            g_Status = "Optional add-on not installed.";
            return false;
        }

        g_LoadAttempted = true;

        g_Module = LoadLibraryA(frameworkPath.string().c_str());
        if (!g_Module)
        {
            g_Status = "BreakersFramework.dll is present, but Windows could not load it.";
            return false;
        }

        g_GetApiVersion = reinterpret_cast<GetApiVersionFn>(RequiredProc("Framework_GetApiVersion"));
        g_GetDisplayName = reinterpret_cast<GetDisplayNameFn>(RequiredProc("Framework_GetDisplayName"));
        g_BindImGuiContext = reinterpret_cast<BindImGuiContextFn>(RequiredProc("Framework_BindImGuiContext"));
        g_BindClientRenderState = reinterpret_cast<BindClientRenderStateFn>(RequiredProc("Framework_BindClientRenderState"));
        g_DrawGui = reinterpret_cast<DrawGuiFn>(RequiredProc("Framework_OnClientGui"));
        g_DrawTab = reinterpret_cast<DrawTabFn>(GetProcAddress(g_Module, "Framework_OnClientTab"));
        g_Shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(g_Module, "Framework_OnClientShutdown"));

        if (!g_GetApiVersion || !g_BindImGuiContext || !g_BindClientRenderState || !g_DrawGui)
        {
            g_Status = "BreakersFramework.dll loaded, but its API is incomplete.";
            FreeLibrary(g_Module);
            g_Module = nullptr;
            g_DrawGui = nullptr;
            Logger::Error(g_Status);
            return false;
        }

        const int apiVersion = g_GetApiVersion();
        if (apiVersion != 1)
        {
            g_Status = "BreakersFramework.dll API version mismatch.";
            FreeLibrary(g_Module);
            g_Module = nullptr;
            g_DrawGui = nullptr;
            Logger::Error(g_Status);
            return false;
        }

        if (g_GetDisplayName)
        {
            const char* name = g_GetDisplayName();
            if (name && name[0])
                g_Name = name;
        }

        g_Status = "Loaded";
        Logger::Info("Loaded framework add-on from " + frameworkPath.string());
        return true;
    }

    void DrawClientPanel()
    {
        ImGui::Text("Framework add-on: %s", IsLoaded() ? Name() : (IsAvailable() ? "available" : "not installed"));
        ImGui::TextWrapped("%s", StatusText());
        ImGui::TextWrapped("Path: %s", PathText());

        if (!IsLoaded() && IsAvailable())
        {
            if (ImGui::Button("Load Framework Add-on", ImVec2(190.0f, 24.0f)))
                Load();
        }

        if (!IsLoaded())
        {
            ImGui::Separator();
            ImGui::TextWrapped("BreakersFramework.dll is optional. The base client and online features work without it.");
            return;
        }

        ImGui::Separator();
        g_BindImGuiContext(ImGui::GetCurrentContext());
        g_BindClientRenderState(g_Device, g_Context, g_GameWindow, g_CurrentFps);
        g_DrawGui();
    }

    void DrawTabPanel(int tab)
    {
        EnsureLoaded();

        if (!IsLoaded())
        {
            ImGui::TextWrapped("%s", StatusText());
            return;
        }

        g_BindImGuiContext(ImGui::GetCurrentContext());
        g_BindClientRenderState(g_Device, g_Context, g_GameWindow, g_CurrentFps);

        if (g_DrawTab)
            g_DrawTab(tab);
        else
            g_DrawGui();
    }

    void Shutdown()
    {
        if (g_Shutdown)
            g_Shutdown();

        if (g_Module)
            FreeLibrary(g_Module);

        g_Module = nullptr;
        g_GetApiVersion = nullptr;
        g_GetDisplayName = nullptr;
        g_BindImGuiContext = nullptr;
        g_BindClientRenderState = nullptr;
        g_DrawGui = nullptr;
        g_DrawTab = nullptr;
        g_Shutdown = nullptr;
        g_LoadAttempted = false;
        g_Status = "Not loaded";
        g_Name = "BreakersFramework.dll";
        g_Path.clear();
    }
}
