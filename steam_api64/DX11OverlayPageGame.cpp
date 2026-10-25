#include "DX11OverlayInternal.h"

namespace
{
    char g_AchievementSearch[128]{};
    char g_StatName[128]{};
    int g_StatIntValue = 0;
    float g_StatFloatValue = 0.0f;
    bool g_ShowUnlockedAchievements = true;
    bool g_ShowLockedAchievements = true;

    bool AchievementMatchesFilter(const SteamStatsLocal::AchievementInfo& achievement)
    {
        if (!g_ShowUnlockedAchievements && achievement.Achieved)
            return false;

        if (!g_ShowLockedAchievements && !achievement.Achieved)
            return false;

        if (g_AchievementSearch[0] == '\0')
            return true;

        return achievement.Name.find(g_AchievementSearch) != std::string::npos ||
            achievement.DisplayName.find(g_AchievementSearch) != std::string::npos ||
            achievement.Description.find(g_AchievementSearch) != std::string::npos;
    }

    void DrawAchievementsPage()
    {
        std::vector<SteamStatsLocal::AchievementInfo> achievements = SteamStatsLocal::GetAchievementSnapshot();

        uint32_t unlocked = 0;
        for (const auto& achievement : achievements)
        {
            if (achievement.Achieved)
                ++unlocked;
        }

        const uint32_t total = static_cast<uint32_t>(achievements.size());
        const float progress = total > 0 ? static_cast<float>(unlocked) / static_cast<float>(total) : 0.0f;

        ImGui::Text("Game > Achievements");
        ImGui::Separator();

        ImGui::Text("Unlocked: %u / %u", unlocked, total);
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 18.0f));

        ImGui::Separator();

        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputText("Search", g_AchievementSearch, ARRAYSIZE(g_AchievementSearch));
        ImGui::SameLine();
        ImGui::Checkbox("Unlocked", &g_ShowUnlockedAchievements);
        ImGui::SameLine();
        ImGui::Checkbox("Locked", &g_ShowLockedAchievements);

        if (ImGui::Button("Unlock All", ImVec2(120.0f, 24.0f)))
        {
            SteamStatsLocal::UnlockAllAchievements();
            SteamStatsLocal::Save();
        }

        ImGui::SameLine();

        if (ImGui::Button("Lock All", ImVec2(120.0f, 24.0f)))
        {
            SteamStatsLocal::ClearAllAchievements();
            SteamStatsLocal::Save();
        }

        ImGui::SameLine();

        if (ImGui::Button("Save Stats", ImVec2(120.0f, 24.0f)))
            SteamStatsLocal::Save();

        ImGui::Separator();

        if (ImGui::BeginChild("##AchievementsList", ImVec2(0.0f, 0.0f), true))
        {
            if (ImGui::BeginTable(
                "##AchievementsTable",
                4,
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_Resizable |
                ImGuiTableFlags_ScrollY))
            {
                ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                ImGui::TableSetupColumn("Description");
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableHeadersRow();

                for (const auto& achievement : achievements)
                {
                    if (!AchievementMatchesFilter(achievement))
                        continue;

                    ImGui::PushID(achievement.Name.c_str());

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);

                    if (achievement.Achieved)
                        ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.0f), "Unlocked");
                    else
                        ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "Locked");

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s", achievement.DisplayName.c_str());
                    ImGui::TextDisabled("%s", achievement.Name.c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextWrapped("%s", achievement.Description.c_str());

                    ImGui::TableSetColumnIndex(3);
                    if (achievement.Achieved)
                    {
                        if (ImGui::Button("Lock", ImVec2(90.0f, 22.0f)))
                        {
                            SteamStatsLocal::ClearAchievement(achievement.Name.c_str());
                            SteamStatsLocal::Save();
                        }
                    }
                    else
                    {
                        if (ImGui::Button("Unlock", ImVec2(90.0f, 22.0f)))
                        {
                            SteamStatsLocal::SetAchievement(achievement.Name.c_str());
                            SteamStatsLocal::Save();
                        }
                    }

                    ImGui::PopID();
                }

                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
    }

    void DrawStatsPage()
    {
        std::vector<SteamStatsLocal::StatInfo> stats = SteamStatsLocal::GetStatSnapshot();

        ImGui::Text("Game > Stats");
        ImGui::Separator();
        ImGui::Text("Local stat rows: %d", static_cast<int>(stats.size()));

        ImGui::Separator();

        ImGui::SetNextItemWidth(240.0f);
        ImGui::InputText("Stat name", g_StatName, ARRAYSIZE(g_StatName));

        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputInt("Int value", &g_StatIntValue);
        ImGui::SameLine();
        if (ImGui::Button("Set Int", ImVec2(110.0f, 24.0f)) && g_StatName[0] != '\0')
        {
            SteamStatsLocal::SetInt(g_StatName, g_StatIntValue);
            SteamStatsLocal::Save();
        }

        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("Float value", &g_StatFloatValue);
        ImGui::SameLine();
        if (ImGui::Button("Set Float", ImVec2(110.0f, 24.0f)) && g_StatName[0] != '\0')
        {
            SteamStatsLocal::SetFloat(g_StatName, g_StatFloatValue);
            SteamStatsLocal::Save();
        }

        if (ImGui::Button("Save Stats", ImVec2(120.0f, 24.0f)))
            SteamStatsLocal::Save();

        ImGui::SameLine();

        if (ImGui::Button("Clear Stats", ImVec2(120.0f, 24.0f)))
            SteamStatsLocal::ClearStats();

        ImGui::Separator();

        if (ImGui::BeginChild("##StatsList", ImVec2(0.0f, 0.0f), true))
        {
            if (ImGui::BeginTable(
                "##StatsTable",
                3,
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_Resizable |
                ImGuiTableFlags_ScrollY))
            {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 160.0f);
                ImGui::TableHeadersRow();

                for (const auto& stat : stats)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextWrapped("%s", stat.Name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", stat.Type.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%s", stat.Value.c_str());
                }

                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
    }
}

void DrawGamePage()
{
    if (g_SelectedSubPage == 0)
    {
        ImGui::Text("Game > General");
        ImGui::Separator();

        ImGui::TextWrapped("General game tools live here. Patch-style toggles were moved to Patches so options are not duplicated.");

        ImGui::Separator();

        ImGui::Checkbox("Enable save backup helper", &g_GameSaveBackupEnabled);
        HelpMarker("Keeps backup/restore tools enabled for the local save system.");

        ImGui::Checkbox("Use custom save folder", &g_GameUseCustomSaveFolder);
        HelpMarker("Planned hook for redirecting SteamStorageLocal to a custom folder.");

        ImGui::Separator();

        if (ImGui::Button("Backup Save", ImVec2(150, 24)))
            Logger::Info("Game page: backup save requested");

        ImGui::SameLine();

        if (ImGui::Button("Restore Save", ImVec2(150, 24)))
            Logger::Info("Game page: restore save requested");

        ImGui::SameLine();

        if (ImGui::Button("Open Save Folder", ImVec2(170, 24)))
            Logger::Info("Game page: open save folder requested");

        ImGui::Separator();

        ImGui::Text("Useful Status");
        ImGui::Text("Remote storage: local");
        ImGui::Text("Stats: local/emulated");
        ImGui::Text("Game mode: offline-safe");
    }
    else if (g_SelectedSubPage == 1)
    {
        ImGui::Text("Game > Saves");
        ImGui::Separator();

        ActiveCheckbox("Enable save backup helper", &g_GameSaveBackupEnabled, "UI toggle for save backup/restore workflow.");
        PlannedCheckbox("Use custom save folder", &g_GameUseCustomSaveFolder, "Needs SteamStorageLocal path override exposed.");

        if (ImGui::Button("Backup Save", ImVec2(150, 24)))
            Logger::Info("Game page: backup save requested");

        ImGui::SameLine();

        if (ImGui::Button("Restore Save", ImVec2(150, 24)))
            Logger::Info("Game page: restore save requested");

        ImGui::SameLine();

        if (ImGui::Button("Validate Save", ImVec2(150, 24)))
            Logger::Info("Game page: validate save requested");

        ImGui::Separator();
        ImGui::TextWrapped("Unlocks, intro skip, cutscene skip, borderless, ultrawide, and similar patch features are only in Patches.");
    }
    else if (g_SelectedSubPage == 2)
    {
        DrawAchievementsPage();
    }
    else if (g_SelectedSubPage == 3)
    {
        DrawStatsPage();
    }
    else
    {
        ImGui::Text("Game > Information");
        ImGui::Separator();

        ImGui::Text("Process: DRAGON BALL THE BREAKERS.exe");
        ImGui::Text("Renderer: D3D11");
        ImGui::Text("Project: Breakers Revived");
        ImGui::Text("Build: %s %s", __DATE__, __TIME__);

        ImGui::Separator();

        ImGui::Text("Game folder:");
        ImGui::TextWrapped("%s", AssetBrowser::GetGameFolder().c_str());

        ImGui::Text("Dump folder:");
        ImGui::TextWrapped("%s", AssetBrowser::GetDumpFolder().c_str());

        ImGui::Text("Mods folder:");
        ImGui::TextWrapped("BreakersRevived\\mods");

        ImGui::Separator();

        ImGui::Text("Asset preload:");
        ImGui::Text("%s", AssetPreloadManager::IsPreloading() ? "loading" : "idle/complete");
    }
}
