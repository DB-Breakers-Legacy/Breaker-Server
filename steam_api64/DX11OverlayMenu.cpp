#include "DX11OverlayInternal.h"
#include "FrameworkAddon.h"

namespace
{
    const char* kOverlayTitle = "Breakers Revived";

    bool ShouldShowFrameworkTabs()
    {
        return FrameworkAddon::IsLoaded() || FrameworkAddon::IsAvailable();
    }

    bool IsFrameworkTabValue(int tab)
    {
        return tab == TAB_GRAPHICS ||
            tab == TAB_CONTROLS ||
            tab == TAB_CAMERA ||
            tab == TAB_ASSETS ||
            tab == TAB_MODS ||
            tab == TAB_PATCHES;
    }

    bool IsFrameworkTopTab(int tab)
    {
        return ShouldShowFrameworkTabs() && IsFrameworkTabValue(tab);
    }
}

void DrawDropdownPanel()
{
    if (!g_MenuOpen || g_SelectedTab < 0)
        return;

    if (IsFrameworkTabValue(g_SelectedTab) && !ShouldShowFrameworkTabs())
    {
        g_SelectedTab = TAB_CLIENT;
        g_SelectedSubPage = 0;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();

    float x = 95.0f + static_cast<float>(g_SelectedTab) * 88.0f;
    float y = 31.0f * g_MenuAnimation;
    float panelWidth = std::min(viewport->Size.x - x - 12.0f, 1280.0f);
    float panelHeight = viewport->Size.y - y - 28.0f;

    if (g_SelectedTab == TAB_CLIENT || g_SelectedTab == TAB_ONLINE || IsFrameworkTopTab(g_SelectedTab))
    {
        x = 12.0f;
        panelWidth = viewport->Size.x - 24.0f;
    }

    if (panelWidth < 760.0f)
    {
        x = 12.0f;
        panelWidth = viewport->Size.x - 24.0f;
    }

    if (panelHeight < 360.0f)
        panelHeight = std::max(260.0f, viewport->Size.y - y - 20.0f);

    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelWidth, panelHeight), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.90f);

    ImGui::Begin(
        "##BreakersRevivalDropdown",
        nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoCollapse);

    const bool frameworkTab = IsFrameworkTopTab(g_SelectedTab);

    if (!frameworkTab)
    {
        float leftWidth = panelWidth < 900.0f ? 185.0f : 220.0f;
        ImGui::BeginChild("##BreakersRevivalLeftList", ImVec2(leftWidth, 0.0f), true);
        ImGui::Text("Options");
        ImGui::Separator();

        switch (g_SelectedTab)
        {
        case TAB_CLIENT:
            DrawLeftButton("Overview", 0);
            DrawLeftButton("Connection", 1);
            break;

        case TAB_ONLINE:
            DrawLeftButton("Overview", 0);
            DrawLeftButton("Diagnostics", 1);
            DrawLeftButton("Settings", 2);
            DrawLeftButton("Friends", 3);
            DrawLeftButton("Invites", 4);
            DrawLeftButton("Profile", 5);
            break;
        }

        ImGui::EndChild();
        ImGui::SameLine();
    }

    ImGui::BeginChild("##BreakersRevivalPageArea", ImVec2(0.0f, 0.0f), true);

    switch (g_SelectedTab)
    {
    case TAB_CLIENT: DrawClientPage(); break;
    case TAB_ONLINE: DrawOnlinePage(); break;
    case TAB_GRAPHICS:
    case TAB_CONTROLS:
    case TAB_CAMERA:
    case TAB_ASSETS:
    case TAB_MODS:
    case TAB_PATCHES:
        FrameworkAddon::DrawTabPanel(g_SelectedTab);
        break;
    }

    ImGui::EndChild();
    ImGui::End();
}

void DrawAnimatedTopMenu()
{
    float target = g_MenuOpen ? 1.0f : 0.0f;
    float speed = ImGui::GetIO().DeltaTime * 14.0f;
    g_MenuAnimation = AnimateToward(g_MenuAnimation, target, speed);

    if (IsFrameworkTabValue(g_SelectedTab) && !ShouldShowFrameworkTabs())
    {
        g_SelectedTab = TAB_CLIENT;
        g_SelectedSubPage = 0;
    }

    if (g_MenuAnimation < 0.01f && !g_MenuOpen)
    {
        g_MenuAnimation = 0.0f;
        g_SelectedTab = TAB_NONE;
        return;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();

    const float barHeight = 30.0f;
    const float y = -barHeight + (barHeight * g_MenuAnimation);

    ImGui::SetNextWindowPos(ImVec2(0.0f, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, barHeight), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    ImGui::Begin(
        "##BreakersRevivalAnimatedTopMenuBar",
        nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse);

    ImGui::SetCursorPos(ImVec2(8.0f, 5.0f));

    ImGui::Text("%s", kOverlayTitle);
    ImGui::SameLine(95.0f);

    DrawTabButton("Client", TAB_CLIENT); ImGui::SameLine();
    DrawTabButton("LAN", TAB_ONLINE); ImGui::SameLine();

    if (ShouldShowFrameworkTabs())
    {
        DrawTabButton("Graphics", TAB_GRAPHICS); ImGui::SameLine();
        DrawTabButton("Controls", TAB_CONTROLS); ImGui::SameLine();
        DrawTabButton("Camera", TAB_CAMERA); ImGui::SameLine();
        DrawTabButton("Assets", TAB_ASSETS); ImGui::SameLine();
        DrawTabButton("Mods", TAB_MODS); ImGui::SameLine();
        DrawTabButton("Patches", TAB_PATCHES); ImGui::SameLine();
    }

    float rightTextX = viewport->Size.x - 120.0f;

    if (rightTextX > ImGui::GetCursorPosX() + 20.0f)
        ImGui::SameLine(rightTextX);

    ImGui::Text("FPS %d", g_CurrentFps);

    ImGui::End();

    DrawDropdownPanel();
}