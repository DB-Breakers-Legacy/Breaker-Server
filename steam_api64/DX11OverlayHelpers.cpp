#include "DX11OverlayInternal.h"

bool ActiveCheckbox(const char* label, bool* value, const char* help)
{
    bool changed = ImGui::Checkbox(label, value);

    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.35f, 1.0f), "[ACTIVE]");

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", help ? help : "");

    return changed;
}

bool PlannedCheckbox(const char* label, bool* value, const char* reason)
{
    bool changed = ImGui::Checkbox(label, value);

    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.25f, 1.0f), "[PLANNED]");

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", reason ? reason : "");

    return changed;
}

void UpdateFps()
{
    if (g_Frequency.QuadPart == 0)
    {
        QueryPerformanceFrequency(&g_Frequency);
        QueryPerformanceCounter(&g_LastFpsTime);
        return;
    }

    g_FrameCounter++;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);

    double elapsed =
        static_cast<double>(now.QuadPart - g_LastFpsTime.QuadPart) /
        static_cast<double>(g_Frequency.QuadPart);

    if (elapsed >= 1.0)
    {
        g_CurrentFps = static_cast<int>(
            static_cast<double>(g_FrameCounter) / elapsed + 0.5);

        g_FpsHistory[g_FpsHistoryOffset] = static_cast<float>(g_CurrentFps);
        g_FpsHistoryOffset = (g_FpsHistoryOffset + 1) % ARRAYSIZE(g_FpsHistory);

        g_FrameCounter = 0;
        g_LastFpsTime = now;
    }
}

void UpdateHotkeys()
{
    bool f1Down = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;

    if (f1Down && !g_F1WasDown)
    {
        g_MenuOpen = !g_MenuOpen;

        if (!g_MenuOpen)
            g_SelectedTab = TAB_NONE;
    }

    g_F1WasDown = f1Down;
}

float AnimateToward(float current, float target, float speed)
{
    float delta = target - current;
    current += delta * speed;

    if (fabsf(delta) < 0.001f)
        current = target;

    return current;
}

void DrawTabButton(const char* label, int tab)
{
    bool selected = g_SelectedTab == tab;

    if (selected)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.38f, 0.68f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.48f, 0.82f, 1.00f));
    }

    if (ImGui::Button(label, ImVec2(86.0f, 22.0f)))
    {
        if (selected)
            g_SelectedTab = TAB_NONE;
        else
        {
            g_SelectedTab = tab;
            g_SelectedSubPage = 0;
        }
    }

    if (selected)
        ImGui::PopStyleColor(2);
}

void DrawLeftButton(const char* label, int page)
{
    bool selected = g_SelectedSubPage == page;
    float width = std::max(120.0f, ImGui::GetContentRegionAvail().x);

    if (selected)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.17f, 0.38f, 0.66f, 1.0f));

    if (ImGui::Button(label, ImVec2(width, 25.0f)))
        g_SelectedSubPage = page;

    if (selected)
        ImGui::PopStyleColor();
}

bool IniCheckbox(const char* label, int& value)
{
    bool checked = value != 0;

    if (ImGui::Checkbox(label, &checked))
    {
        value = checked ? 1 : 0;
        return true;
    }

    return false;
}

bool IniCheckbox(const char* label, int& value, const char* help)
{
    bool changed = IniCheckbox(label, value);
    HelpMarker(help);
    return changed;
}

void HelpMarker(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");

    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(420.0f);
        ImGui::TextUnformatted(text ? text : "");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
