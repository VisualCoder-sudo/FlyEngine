#include "ui_internal.hpp"
#include "../../../include/Engine/Frontend/PluginHost.hpp"
#include "../../../include/Engine/ExportManager.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../include/Engine/Frontend/ProjectManager.hpp"
#include "../../../include/Terrain/TerrainEditor.hpp"
#include "../../../include/CityGen/CityEditor.hpp"
#include "Engine/U8Path.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace ui {

// ---------------------------------------------------------------------------
// 1. Top Bar
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 1. Top Bar (Dear ImGui) - File menu, Import, tool buttons, Play/Stop.
// Called once per frame from DrawImGuiFrame(), between ImGui::NewFrame()
// (UpdateInput()) and ImGui::Render().
// ---------------------------------------------------------------------------
static ImTextureID ToolTex(int i) {
    return g_toolIcons[i].id != 0
        ? static_cast<ImTextureID>(GetTextureImGuiId(g_toolIcons[i]))
        : static_cast<ImTextureID>(0);
}

// Launch the standalone player with the current project
static void LaunchPlayer() {
    std::string playerExe = platform::ExecutablePath();
    std::filesystem::path exePath = U8Path(playerExe);
    
    // Try to find FlyPlayer in the same directory
    std::filesystem::path playerPath = exePath.parent_path() / "FlyPlayer";
    {
        // Knows the platform's file name too (FlyPlayer.exe on Windows).
        const std::string found = exporter::FindPlayerExecutable();
        if (!found.empty()) playerPath = U8Path(found);
    }
    if (!std::filesystem::exists(playerPath)) {
        // Try case-insensitive search
        for (auto& entry : std::filesystem::directory_iterator(exePath.parent_path())) {
            std::string name = entry.path().filename().string();
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower == "flyplayer") {
                playerPath = entry.path();
                break;
            }
        }
    }
    
    // Fallback to case variants
    if (!std::filesystem::exists(playerPath)) {
        playerPath = exePath.parent_path() / "flyplayer";
    }
    if (!std::filesystem::exists(playerPath)) {
        playerPath = exePath.parent_path() / "FLYPLAYER";
    }
    if (!std::filesystem::exists(playerPath)) {
        playerPath = exePath.parent_path() / "Flyplayer";
    }
    
    // Get current project path
    const auto& proj = project::GetCurrentProject();
    if (proj.path.empty()) {
        return;
    }
    
    // Save current scene to temp file (doesn't overwrite main .flyproj)
    std::string tempProjectFile = "";
    
    // Get legacy terrain from terrain editor state
    terrain::Terrain* currentTerrain = nullptr;
    auto& terrainEditorState = terrain::GetTerrainEditorState();
    currentTerrain = terrainEditorState.selectedTerrainLegacy;
    
    if (g_sceneObjects && g_sceneModels) {
        tempProjectFile = project::SaveProjectFileTemp(proj.path, *g_sceneObjects, *g_sceneModels, currentTerrain);
        if (!tempProjectFile.empty()) {
            std::printf("[Play in Player] Saved temp scene to: %s\n", tempProjectFile.c_str());
        }
    }
    
    // Final fallback to editor executable
    if (!std::filesystem::exists(playerPath)) {
        playerPath = U8Path(platform::ExecutablePath());
    }
    
    // Use temp file if created, otherwise use main project
    std::string launchPath = tempProjectFile.empty() ? proj.path : tempProjectFile;
    std::vector<std::string> args = {"-play", launchPath};
    platform::LaunchDetached(playerPath.string(), args);
}

void DrawImGuiTopBar() {
    Rectangle barRec = GetTopBarBounds();

    ImGui::SetNextWindowPos(ImVec2(barRec.x, barRec.y));
    ImGui::SetNextWindowSize(ImVec2(barRec.width, barRec.height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(25 / 255.0f, 27 / 255.0f, 32 / 255.0f, 1.0f)); // theme::BG_TOP_BAR
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
    ImGui::Begin("##TopBar", nullptr, flags);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::BeginDisabled(g_playActive);

    // File button (toggles a popup below it)
    if (ImGui::Button("File", ImVec2(56.0f, 30.0f))) {
        ImGui::OpenPopup("##FileMenu");
    }
    ImGui::SameLine();

    // Import button
    if (ToolTex(4)) {
        ImGui::Image(ToolTex(4), ImVec2(16.0f, 16.0f));
        ImGui::SameLine(0.0f, 6.0f);
    }
    if (ImGui::Button("Import", ImVec2(0.0f, 30.0f))) {
        g_importRequested = true;
    }

    ImGui::SameLine();
    if (ImGui::Button("City", ImVec2(0.0f, 30.0f))) {
        ImGui::OpenPopup("##CityMenu");
    }
    ImVec2 cityBtnMin = ImGui::GetItemRectMin();

    // Mode switcher: only when a plugin offers a mode. Leaving a mode (or Play) restores the fly camera.
    ImVec2 modeBtnMin = cityBtnMin;
    const bool haveModes = !pluginhost::Modes().empty();
    if (haveModes) {
        const std::string& activeId = pluginhost::ActiveModeId();
        std::string modeLabel = "Mode: Normal";
        for (const auto& m : pluginhost::Modes()) if (m.id == activeId) modeLabel = "Mode: " + m.label;
        const bool inMode = !activeId.empty();
        ImGui::SameLine();
        if (inMode) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(theme::AccentU32(200)));
        if (ImGui::Button((modeLabel + "  v").c_str(), ImVec2(0.0f, 30.0f))) ImGui::OpenPopup("##ModeMenu");
        if (inMode) ImGui::PopStyleColor();
        modeBtnMin = ImGui::GetItemRectMin();
    }

    // Plugin menus: one button per distinct menu name.
    std::vector<std::string> pluginMenus;
    std::vector<ImVec2> pluginMenuPos;
    for (const auto& item : pluginhost::TopMenuItems()) {
        if (std::find(pluginMenus.begin(), pluginMenus.end(), item.menu) != pluginMenus.end()) continue;
        pluginMenus.push_back(item.menu);
        ImGui::SameLine();
        if (ImGui::Button((item.menu + "##pluginmenu").c_str(), ImVec2(0.0f, 30.0f))) ImGui::OpenPopup(("##PluginMenu_" + item.menu).c_str());
        pluginMenuPos.push_back(ImGui::GetItemRectMin());
    }

    ImGui::EndDisabled();

    for (size_t i = 0; i < pluginMenus.size(); ++i) {
        ImGui::SetNextWindowPos(ImVec2(pluginMenuPos[i].x, barRec.y + 38.0f));
        if (!ImGui::BeginPopup(("##PluginMenu_" + pluginMenus[i]).c_str())) continue;
        // Copy: a click may unload the plugin that owns the item.
        const std::vector<pluginhost::TopMenuItem> items = pluginhost::TopMenuItems();
        for (const auto& item : items) {
            if (item.menu != pluginMenus[i]) continue;
            if (ImGui::MenuItem(item.label.c_str())) { item.run(); ImGui::CloseCurrentPopup(); break; }
        }
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(ImVec2(modeBtnMin.x, barRec.y + 38.0f));
    if (haveModes && ImGui::BeginPopup("##ModeMenu")) {
        const std::string activeId = pluginhost::ActiveModeId();
        if (ImGui::MenuItem("Normal", nullptr, activeId.empty())) { pluginhost::RequestMode(""); ImGui::CloseCurrentPopup(); }
        ImGui::Separator();
        for (const auto& m : pluginhost::Modes()) {
            if (ImGui::MenuItem(m.label.c_str(), nullptr, m.id == activeId)) {
                pluginhost::RequestMode(m.id);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // City menu popup, anchored under the City button
    ImGui::SetNextWindowPos(ImVec2(cityBtnMin.x, barRec.y + 38.0f));
    if (ImGui::BeginPopup("##CityMenu")) {
        if (ImGui::MenuItem("New regular city")) { g_pendingFileAction = MenuAction::SpawnCity; ImGui::CloseCurrentPopup(); }
        if (ImGui::MenuItem("New organic city")) { g_pendingFileAction = MenuAction::SpawnCityOrganic; ImGui::CloseCurrentPopup(); }
        ImGui::Separator();
        bool panelOpen = city::GetCityEditorState().panelOpen;
        if (ImGui::MenuItem("City panel", nullptr, &panelOpen)) {
            city::GetCityEditorState().panelOpen = panelOpen;
        }
        ImGui::EndPopup();
    }

    // File menu popup, anchored under the File button
    ImGui::SetNextWindowPos(ImVec2(barRec.x + 4.0f, barRec.y + 38.0f));
    if (ImGui::BeginPopup("##FileMenu")) {
        if (ImGui::MenuItem("Save")) { g_pendingFileAction = MenuAction::Save; ImGui::CloseCurrentPopup(); }
        if (ImGui::MenuItem("Save as")) { g_pendingFileAction = MenuAction::SaveAs; ImGui::CloseCurrentPopup(); }
        if (ImGui::MenuItem("Open scene")) { g_pendingFileAction = MenuAction::OpenScene; ImGui::CloseCurrentPopup(); }
        ImGui::Separator();
        if (ImGui::MenuItem("Export...")) { OpenExportDialog(); ImGui::CloseCurrentPopup(); }
        ImGui::Separator();
        if (ImGui::MenuItem("Tutorial")) { OpenTutorial(); ImGui::CloseCurrentPopup(); }
        if (ImGui::MenuItem("Preferences")) { g_showPreferences = true; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // Transform tool buttons (row 2)
    ImGui::SetCursorPos(ImVec2(75.0f, 45.0f));
    constexpr const char* toolNames[] = { "Select", "Move", "Scale", "Rotate" };
    constexpr TransformTool toolEnums[] = { TransformTool::Select, TransformTool::Move, TransformTool::Scale, TransformTool::Rotate };
    for (int i = 0; i < 4; ++i) {
        bool selected = (g_activeTool == toolEnums[i]);

        // One pill per tool: rounded icon tile on the left, label on the right.
        constexpr float pillH = 34.0f;
        constexpr float tile = 26.0f;
        constexpr float padX = 4.0f;
        constexpr float gap = 8.0f;
        const float textW = ImGui::CalcTextSize(toolNames[i]).x;
        const ImVec2 pillSize(padX + tile + gap + textW + 12.0f, pillH);

        ImGui::PushID(i);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##tool", pillSize)) g_activeTool = toolEnums[i];
        const bool hov = ImGui::IsItemHovered();
        if (hov) MarkHand();
        ImGui::PopID();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p1(p0.x + pillSize.x, p0.y + pillSize.y);
        ImU32 bg = selected ? theme::AccentU32(180)
                 : hov      ? IM_COL32(63, 67, 78, 255)
                            : IM_COL32(48, 51, 60, 255);
        dl->AddRectFilled(p0, p1, bg, 10.0f);

        const ImVec2 t0(p0.x + padX, p0.y + (pillH - tile) * 0.5f);
        const ImVec2 t1(t0.x + tile, t0.y + tile);
        dl->AddRectFilled(t0, t1, selected ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 22), 8.0f);
        if (ImTextureID tex = ToolTex(i)) {
            constexpr float icon = 16.0f;
            const float o = (tile - icon) * 0.5f;
            dl->AddImage(tex, ImVec2(t0.x + o, t0.y + o), ImVec2(t0.x + o + icon, t0.y + o + icon));
        }
        dl->AddText(ImVec2(t1.x + gap, p0.y + (pillH - ImGui::GetTextLineHeight()) * 0.5f),
                    IM_COL32(232, 232, 238, 255), toolNames[i]);

        if (i < 3) ImGui::SameLine(0.0f, 10.0f);
    }

    // Tools pill: opens the snapping popup (G toggles Snap, H toggles To Object, Ctrl inverts Snap while dragging)
    ImGui::SameLine(0.0f, 18.0f);
    {
        constexpr float pillH = 34.0f;
        const ImVec2 pillSize(ImGui::CalcTextSize("Tools").x + 44.0f, pillH);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##toolsBtn", pillSize)) ImGui::OpenPopup("##ToolsPopup");
        const bool hov = ImGui::IsItemHovered();
        if (hov) MarkHand();
        const bool open = ImGui::IsPopupOpen("##ToolsPopup");
        const bool active = g_snap.enabled || g_snap.toObject;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p1(p0.x + pillSize.x, p0.y + pillSize.y);
        ImU32 bg = open ? theme::AccentU32(180)
                 : hov  ? IM_COL32(63, 67, 78, 255)
                        : IM_COL32(48, 51, 60, 255);
        dl->AddRectFilled(p0, p1, bg, 10.0f);
        const float ty = p0.y + (pillH - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText(ImVec2(p0.x + 14.0f, ty), IM_COL32(232, 232, 238, 255), "Tools");
        // chevron
        const float cx = p1.x - 16.0f, cy = p0.y + pillH * 0.5f;
        dl->AddTriangleFilled(ImVec2(cx - 4.0f, cy - 2.0f), ImVec2(cx + 4.0f, cy - 2.0f), ImVec2(cx, cy + 3.0f),
                              IM_COL32(232, 232, 238, 255));
        if (active) dl->AddCircleFilled(ImVec2(p1.x - 5.0f, p0.y + 5.0f), 3.5f, theme::AccentU32(255)); // something is on

        ImGui::SetNextWindowPos(ImVec2(p0.x, p1.y + 6.0f));
        if (ImGui::BeginPopup("##ToolsPopup")) {
            ImGui::TextDisabled("SNAPPING");
            ImGui::Separator();
            ImGui::Checkbox("Snap to grid", &g_snap.enabled);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("G. Hold Ctrl while dragging to invert");
            ImGui::BeginDisabled(!g_snap.enabled);
            ImGui::Indent(8.0f);
            ImGui::SetNextItemWidth(90.0f);
            ImGui::DragFloat("Move", &g_snap.move, 0.05f, 0.01f, 1000.0f, "%.2f m");
            ImGui::SetNextItemWidth(90.0f);
            ImGui::DragFloat("Rotate", &g_snap.rotate, 0.5f, 0.1f, 180.0f, "%.1f deg");
            ImGui::SetNextItemWidth(90.0f);
            ImGui::DragFloat("Scale", &g_snap.scale, 0.01f, 0.01f, 10.0f, "%.2f x");
            ImGui::Unindent(8.0f);
            ImGui::EndDisabled();
            ImGui::Spacing();
            ImGui::Checkbox("Snap to object", &g_snap.toObject);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("H. Move: slide flush against nearby objects");
            ImGui::EndPopup();
        }
    }

    // Play in Player button (launches standalone player), right-aligned next to Play
    {
        ImGui::SetCursorPos(ImVec2(barRec.width - 210.0f, 15.0f));
        if (ImGui::Button("Play in Player", ImVec2(115.0f, 30.0f))) {
            LaunchPlayer();
        }
        ImGui::SameLine(0.0f, 12.0f);
    }

    // Play/Stop button, right-aligned
    {
        const char* playLabel = g_playActive ? "Stop" : "Play";
        ImVec4 playCol = g_playActive
            ? ImVec4(200 / 255.0f, 40 / 255.0f, 40 / 255.0f, 1.0f)   // theme::DANGER (approx)
            : ImVec4(0 / 255.0f, 170 / 255.0f, 90 / 255.0f, 1.0f);   // theme::SUCCESS (approx)
        ImGui::PushStyleColor(ImGuiCol_Button, playCol);
        ImGui::SetCursorPos(ImVec2(barRec.width - 85.0f, 15.0f));
        if (ImGui::Button(playLabel, ImVec2(70.0f, 30.0f))) {
            g_playTogglePending = true;
        }
        ImGui::PopStyleColor();
    }

    ImGui::End();
}

} // namespace ui
