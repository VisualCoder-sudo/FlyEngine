// ui_preferences.cpp: the Preferences window, split out of ui.cpp.
#include "ui_internal.hpp"
#include "../../../include/Engine/AI/AI.hpp"
#include <map>
#include <set>
#include "../../../include/Engine/ExportManager.hpp"
#include "../../../include/Engine/Backend/VisualScript.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Terrain/Water/WaterBody.hpp"
#include "../../../include/Terrain/BasicTerrain.hpp"
#include "../../../include/Engine/Scripts/CommandConsole.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "../../../include/Graphics/Graphics.hpp"
#include "../../../include/Graphics/PostFX.hpp"
#include "../../../include/Engine/LoadingScreen.hpp"
#include "../include/Engine/Scripts/ScriptLauncher.hpp"
#include "../../../include/Engine/Scripts/NativeScriptHost.hpp"
#include "../../../include/Engine/Frontend/PluginHost.hpp"
#include "../../../include/Engine/Frontend/PluginStore.hpp"
#include "../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../../../include/Engine/Backend/TextureManager.hpp"
#include "../include/Engine/Frontend/ProjectManager.hpp"
#include "../../../include/Engine/Backend/ModelImport.hpp"
#include "../include/Terrain/Terrain.hpp"
#include "../../../include/Terrain/TerrainEditor.hpp"
#include "../../../include/Terrain/TerrainRegistry.hpp"
#include "../../../include/CityGen/CityEditor.hpp"
#include "raymath.h"
#include "imgui.h"
#include "rlimgui.h"
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <algorithm>

namespace ui {

bool g_showPreferences = false;
int  g_prefsCategory = 0; // 0=General 1=AI 2=Scripting 3=Plugins 4=Rendering 5=Appearance 6=Loading 7=Layout

// ---- Preferences panel ----
// Editor/workflow settings only (font, autosave cadence, external tools,
// panel layout, etc. as they get added later) - never game or scene data.
// That belongs in the Terrain/Object/World property panels instead.

static void DrawPrefsSidebarButton(const char* label, int index, bool enabled = true) {
    bool selected = (g_prefsCategory == index);
    ImVec4 selColor    = theme::AccentVec();
    ImVec4 plainColor  = ImVec4(0.55f, 0.55f, 0.58f, 1.0f);
    ImVec4 plainHover  = ImVec4(0.62f, 0.62f, 0.66f, 1.0f);
    ImVec4 bg = selected ? selColor : plainColor;

    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? selColor : plainHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, selColor);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::BeginDisabled(!enabled);
    if (ImGui::Button(label, ImVec2(-1.0f, 30.0f)) && enabled) {
        g_prefsCategory = index;
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
}

// True until the first row of a tab is drawn, so that row gets no separator above it. Each tab resets it.
static bool g_prefsFirstRow = true;

// A full-width row with a separator line above it: label on the left, a control on the right.
static void DrawPrefsSettingRowBegin(const char* label) {
    if (!g_prefsFirstRow) ImGui::Separator();
    g_prefsFirstRow = false;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 rowPos = ImGui::GetCursorScreenPos();
    float rowHeight = 30.0f;
    ImGui::SetCursorScreenPos(ImVec2(rowPos.x + 8.0f, rowPos.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextUnformatted(label);
    ImGui::SameLine(avail.x - 150.0f);
    ImGui::SetCursorScreenPos(ImVec2(rowPos.x + avail.x - 150.0f, rowPos.y + 3.0f));
}

static void DrawPreferencesGeneralTab() {
    g_prefsFirstRow = true;
    DrawPrefsSettingRowBegin("Language");
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.5f, 0.54f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.54f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.5f, 0.54f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::Button("English (US)", ImVec2(140.0f, 24.0f)); // only option for now
    ImGui::PopStyleColor(4);

    DrawPrefsSettingRowBegin("Tutorial");
    if (ImGui::Button("Restart tutorial", ImVec2(140.0f, 24.0f))) {
        project::SetTutorialDone(false);
        g_showPreferences = false;
        OpenTutorial();
    }
}

// Plugin Manager: every plugin found under <project>/plugins (local) and the user's global plugins folder (every project),
// whether or not it loaded. Turning one off unloads it (and drops its modes and menu items) without deleting anything; the
// choice is kept in that folder's disabled.txt. The project manager's File > Manage Plugins edits the same folders.
static void DrawPreferencesPluginsTab() {
    ScriptRuntime* rt = GetActiveRuntime();
    NativeScript::NativeScriptHost* host = rt ? rt->GetHost() : nullptr;
    if (!host) { ImGui::TextDisabled("No plugin host (an exported game's plugins are built in)."); return; }

    const auto& plugins = host->GetPlugins();
    int running = 0, failed = 0;
    for (const auto& p : plugins) { running += p->loaded; failed += (p->enabled && p->loadFailed); }
    ImGui::Indent(8.0f);
    ImGui::Text("%zu plugin%s  -  %d running, %d disabled, %d failed", plugins.size(), plugins.size() == 1 ? "" : "s", running,
                int(std::count_if(plugins.begin(), plugins.end(), [](const auto& p) { return !p->enabled; })), failed);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 480.0f);
    static std::string lastRescan;
    static bool lastFailed = false;
    static bool installGlobal = false;   // where Import puts a plugin: every project, or only this one
    if (ImGui::Button("Import plugin...", ImVec2(130.0f, 24.0f)))
        platform::BeginChooseFolderDialog(platform::DialogPurpose::ImportPluginFolder, "Choose a plugin folder", "");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy a plugin folder (the one with id.json and build/) into this project (or into the global plugins when Install for all projects is on)");
    ImGui::SameLine();
    if (ImGui::Button("Import library...", ImVec2(130.0f, 24.0f)))
        platform::BeginOpenFileDialog(platform::DialogPurpose::ImportPluginFile, "Choose a plugin library", "",
                                      { { "Plugin library", "*.so;*.dll" } });
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Install a single built .so / .dll as a plugin");
    ImGui::SameLine();
    bool rescan = ImGui::Button("Rescan", ImVec2(100.0f, 24.0f));
    ImGui::SameLine();
    if (ImGui::Button("Open folder", ImVec2(110.0f, 24.0f))) {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::path(installGlobal ? pluginstore::GlobalRoot() : pluginstore::LocalRoot(host->GetProjectPath()));
        std::filesystem::create_directories(dir, ec);
        platform::OpenWithDefaultApp(dir.string());
    }
    ImGui::Checkbox("Install for all projects", &installGlobal);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Import and Open folder use the global plugins folder, so every project loads the plugin.\nOff: only this project.");
    ImGui::Unindent(8.0f);
    ImGui::Spacing();

    std::string picked;
    if (platform::PollDialogResult({ platform::DialogPurpose::ImportPluginFolder, platform::DialogPurpose::ImportPluginFile }, picked) &&
        !picked.empty())
    {
        const auto res = host->ImportPlugin(picked, installGlobal);
        lastRescan = res.message;
        lastFailed = !res.ok;
    }
    if (rescan) {
        const int found = host->RescanPlugins();
        lastFailed = false;
        lastRescan = found ? "Found " + std::to_string(found) + " new plugin" + (found == 1 ? "" : "s") : "No new plugins found.";
    }
    if (!lastRescan.empty()) {
        ImGui::Indent(8.0f);
        if (lastFailed) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s", lastRescan.c_str());
        else ImGui::TextDisabled("%s", lastRescan.c_str());
        ImGui::Unindent(8.0f);
    }

    if (plugins.empty()) {
        ImGui::Indent(8.0f);
        ImGui::TextWrapped("No plugins yet. Build one into <project>/plugins/nat/<name>/build/ (or the global plugins folder) and press Rescan. "
                           "See the Native plugins section of the README.");
        ImGui::Unindent(8.0f);
        return;
    }

    // Toggling or reloading never adds or removes records, but Rescan does: it ran above, before the loop.
    std::string toggleName, reloadName, removeName, moveName;
    bool toggleTo = false;
    for (const auto& p : plugins) {
        ImGui::PushID(p->name.c_str());
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::Indent(8.0f);

        ImVec4 dot = !p->enabled ? ImVec4(0.5f, 0.5f, 0.54f, 1.0f)
                   : p->loadFailed ? ImVec4(0.90f, 0.30f, 0.30f, 1.0f)
                   : ImVec4(0.35f, 0.80f, 0.45f, 1.0f);
        const ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(c.x + 5.0f, c.y + ImGui::GetTextLineHeight() * 0.5f), 5.0f, ImGui::ColorConvertFloat4ToU32(dot));
        ImGui::Dummy(ImVec2(16.0f, 0.0f));
        ImGui::SameLine();
        ImGui::TextUnformatted(p->displayName.c_str());
        if (!p->version.empty()) { ImGui::SameLine(); ImGui::TextDisabled("v%s", p->version.c_str()); }
        if (!p->publisher.empty()) { ImGui::SameLine(); ImGui::TextDisabled("by %s", p->publisher.c_str()); }
        ImGui::SameLine();
        ImGui::TextColored(p->global ? ImVec4(0.35f, 0.75f, 0.85f, 1.0f) : ImVec4(0.6f, 0.62f, 0.68f, 1.0f), "[%s]", p->global ? "Global" : "Local");

        bool on = p->enabled;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 450.0f);
        if (ImGui::Checkbox("Enabled", &on)) { toggleName = p->name; toggleTo = on; }
        ImGui::SameLine();
        ImGui::BeginDisabled(!p->enabled);
        if (ImGui::Button("Reload", ImVec2(70.0f, 22.0f))) reloadName = p->name;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(p->global ? "Make Local" : "Make Global", ImVec2(110.0f, 22.0f))) moveName = p->name;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(p->global ? "Keep it only in this project" : "Load it in every project");
        ImGui::SameLine();
        if (ImGui::Button("Show", ImVec2(60.0f, 22.0f))) platform::RevealInFileManager(p->path);
        ImGui::SameLine();
        ImGui::BeginDisabled(p->dir.empty());
        if (ImGui::Button("Remove", ImVec2(70.0f, 22.0f))) ImGui::OpenPopup("Remove plugin?");
        ImGui::EndDisabled();
        if (ImGui::BeginPopupModal("Remove plugin?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text(p->global ? "Delete \"%s\" and its folder from your global plugins (every project loses it)?" : "Delete \"%s\" and its folder from this project?", p->displayName.c_str());
            ImGui::TextDisabled("%s", p->dir.c_str());
            ImGui::TextDisabled("This cannot be undone.");
            if (ImGui::Button("Delete", ImVec2(100.0f, 24.0f))) { removeName = p->name; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100.0f, 24.0f))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (!p->description.empty()) { ImGui::Indent(16.0f); ImGui::TextWrapped("%s", p->description.c_str()); ImGui::Unindent(16.0f); }

        ImGui::Indent(16.0f);
        std::string meta = p->type.empty() ? "native" : p->type;
        if (!p->category.empty()) meta += "  -  " + p->category;
        if (p->entry) {
            meta += "  -  API " + std::to_string(p->entry->api_version.major) + "." + std::to_string(p->entry->api_version.minor) +
                    "." + std::to_string(p->entry->api_version.patch);
        } else if (!p->declaredApi.empty()) {
            meta += "  -  API " + p->declaredApi;
        }
        if (!p->hasIdentity) meta += "  -  no id.json";
        ImGui::TextDisabled("%s", meta.c_str());

        if (p->loaded && p->entry) {
            std::string reg;
            for (const auto& m : pluginhost::Modes())
                if (m.owner == p->entry) reg += (reg.empty() ? "" : ", ") + m.label;
            if (!reg.empty()) ImGui::TextDisabled("Editor modes: %s", reg.c_str());
        }
        if (!p->enabled) ImGui::TextDisabled("Disabled - not loaded.");
        else if (p->loadFailed) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "Failed: %s", p->errorMessage.c_str());
        ImGui::Unindent(16.0f);

        ImGui::Unindent(8.0f);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::PopID();
    }

    if (!toggleName.empty()) host->SetPluginEnabled(toggleName, toggleTo);
    if (!reloadName.empty()) host->ReloadPlugin(reloadName);
    if (!moveName.empty()) {
        bool toGlobal = true;
        for (const auto& p : plugins) if (p->name == moveName) toGlobal = !p->global;
        const auto res = host->MovePlugin(moveName, toGlobal);
        lastRescan = res.ok ? res.message + (toGlobal ? " It now loads in every project." : " It now loads in this project only.") : res.message;
        lastFailed = !res.ok;
        return;   // the list changed under the loop above
    }
    if (!removeName.empty()) {
        const auto res = host->RemovePlugin(removeName);
        lastRescan = res.message;
        lastFailed = !res.ok;
    }

    ImGui::Separator();
    ImGui::Indent(8.0f);
    ImGui::TextDisabled("Plugins run inside the editor with full access; only enable ones you trust.");
    ImGui::Unindent(8.0f);
}

// Where the editor's panels sit: shown or closed, docked left or right, how wide. Dragging a panel's title or
// edge in the editor changes the same settings. Saved per machine (<config dir>/layout.cfg).
static void DrawPreferencesLayoutTab() {
    g_prefsFirstRow = true;
    LayoutState& l = Layout();
    bool changed = false;
    for (int i = 0; i < kPanelCount; ++i) {
        const PanelId id = static_cast<PanelId>(i);
        PanelState& p = l.panel[i];
        ImGui::PushID(i);
        DrawPrefsSettingRowBegin(PanelName(id));
        if (ImGui::Checkbox("##shown", &p.visible)) changed = true;
        if (id != PanelId::Bottom) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            int side = static_cast<int>(p.side);
            const char* sides[] = { "Left", "Right" };
            ImGui::BeginDisabled(!p.visible);
            if (ImGui::Combo("##side", &side, sides, 2) && side != static_cast<int>(p.side)) {
                SetPanelSide(id, static_cast<DockSide>(side));
                changed = true;
            }
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
    DrawPrefsSettingRowBegin("Left column width");
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderFloat("##leftw", &l.leftW, 160.0f, 600.0f, "%.0f px")) changed = true;
    DrawPrefsSettingRowBegin("Right column width");
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderFloat("##rightw", &l.rightW, 160.0f, 600.0f, "%.0f px")) changed = true;
    DrawPrefsSettingRowBegin("Bottom panel height");
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderFloat("##bottomh", &l.bottomH, 90.0f, 600.0f, "%.0f px")) changed = true;
    DrawPrefsSettingRowBegin("Lock layout (no dragging)");
    if (ImGui::Checkbox("##locked", &l.locked)) changed = true;
    DrawPrefsSettingRowBegin("Reset to default");
    if (ImGui::Button("Reset layout", ImVec2(140.0f, 24.0f))) ResetLayout();
    ImGui::Spacing();
    ImGui::Indent(8.0f);
    ImGui::TextDisabled("Drag a panel's title to move it, drag its edges to resize, right-click the title to close it.");
    ImGui::Unindent(8.0f);
    if (changed) SaveLayout();
}

// How much of the machine the picture may use (gfx::RenderQuality). Kept per machine, not per scene.
static void DrawPreferencesRenderingTab() {
    g_prefsFirstRow = true;
    gfx::RenderQuality& q = gfx::Quality();
    bool changed = false;
    const auto combo = [&](const char* label, const char* id, int* value, const char* const* items, int count) {
        DrawPrefsSettingRowBegin(label);
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::Combo(id, value, items, count)) changed = true;
    };

    {
        DrawPrefsSettingRowBegin("Quality");
        ImGui::SetNextItemWidth(140.0f);
        const char* tiers[] = { "Low", "Medium", "High", "Ultra", "Custom" };
        int tier = (q.tier >= 0 && q.tier <= 3) ? q.tier : 4;
        if (ImGui::Combo("##quality", &tier, tiers, 5) && tier <= 3) {
            gfx::SetQualityTier(tier);
            gfx::SaveQualitySettings();
        }
    }
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    DrawPrefsSettingRowBegin("Render scale");
    ImGui::SetNextItemWidth(140.0f);
    float scale = q.renderScale * 100.0f;
    if (ImGui::SliderFloat("##renderscale", &scale, 50.0f, 100.0f, "%.0f%%")) { q.renderScale = scale / 100.0f; changed = true; }

    const char* aa[] = { "Off", "FXAA", "Temporal" };
    combo("Anti-aliasing", "##aa", &q.antiAliasing, aa, 3);

    DrawPrefsSettingRowBegin("Bloom");
    if (ImGui::Checkbox("##bloom", &q.bloom)) changed = true;
    DrawPrefsSettingRowBegin("Ambient occlusion");
    if (ImGui::Checkbox("##ao", &q.ambientOcclusion)) changed = true;
    DrawPrefsSettingRowBegin("Sky reflections");
    if (ImGui::Checkbox("##envlight", &q.environmentLighting)) changed = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ambient light and reflections from the sky, baked into cube maps.\nOff: a flat sky and ground colour.");

    DrawPrefsSettingRowBegin("Hover outline");
    if (ImGui::Checkbox("##hoveroutline", &q.hoverOutline)) changed = true;
    if (q.hoverOutline) {
        DrawPrefsSettingRowBegin("Outline thickness");
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::SliderFloat("##outlinethickness", &q.outlineThickness, 0.5f, 12.0f, "%.1f px")) changed = true;
        DrawPrefsSettingRowBegin("Outline color");
        if (ImGui::ColorEdit3("##outlinecolor", q.outlineColor, ImGuiColorEditFlags_NoInputs)) changed = true;
    }

    const char* clouds[] = { "Low", "Medium", "High", "Ultra" };
    combo("Clouds", "##clouds", &q.clouds, clouds, 4);
    const char* fog[] = { "Plain", "Light shafts", "Fine light shafts" };
    combo("Air and fog", "##volumetrics", &q.volumetrics, fog, 3);

    DrawPrefsSettingRowBegin("Shadow distance steps");
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderInt("##cascades", &q.shadowCascades, 1, 4)) changed = true;

    if (changed) {
        q.tier = -1;        // no longer one of the presets
        gfx::SaveQualitySettings();
    }

    // The editor's frame-rate limit. Not part of the quality presets, so changing it leaves the tier alone.
    DrawPrefsSettingRowBegin("FPS cap");
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::DragInt("##fpscap", &q.fpsCap, 1.0f, 0, 1000, q.fpsCap == 0 ? "Unlimited" : "%d")) {
        q.fpsCap = std::clamp(q.fpsCap, 0, 1000);
        gfx::ApplyFpsCap();
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) gfx::SaveQualitySettings();
    ImGui::SameLine();
    ImGui::TextDisabled("0 = unlimited");
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::Indent(8.0f);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 16.0f);
    ImGui::TextDisabled("These are kept for this machine, not saved with a scene. What a scene looks like (its sky, clouds, fog and picture) is in the Explorer's Lighting section. Press F2 in the viewport to see what each pass costs.");
    ImGui::PopTextWrapPos();
    ImGui::Unindent(8.0f);
}

// The loading screens of the editor and the player (src/Engine/LoadingScreen.cpp): on or off, what they show, how
// they look. Kept per machine; the player's look can also be saved into the project.
static void DrawLoadingLookEditor(const char* id, loading::Look& l, bool& changed) {
    ImGui::PushID(id);
    const auto row = [&](const char* label, bool& v) {
        DrawPrefsSettingRowBegin(label);
        if (ImGui::Checkbox("##v", &v)) changed = true;
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + 2.0f));
    };
    ImGui::PushID("enabled"); row("Show a loading screen", l.enabled); ImGui::PopID();
    ImGui::BeginDisabled(!l.enabled);
    ImGui::PushID("percent"); row("Percentage", l.showPercent); ImGui::PopID();
    ImGui::PushID("bar"); row("Progress bar", l.showBar); ImGui::PopID();
    ImGui::PushID("stage"); row("What it is doing now", l.showStage); ImGui::PopID();
    ImGui::PushID("logs"); row("Log lines", l.showLogs); ImGui::PopID();
    if (l.showLogs) {
        DrawPrefsSettingRowBegin("Log lines shown");
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::SliderInt("##loglines", &l.logLines, 3, 16)) changed = true;
    }
    DrawPrefsSettingRowBegin("Title ({name} = the project)");
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::InputText("##title", l.title, sizeof l.title)) changed = true;
    float bg[3] = { l.background.r / 255.0f, l.background.g / 255.0f, l.background.b / 255.0f };
    float ac[3] = { l.accent.r / 255.0f, l.accent.g / 255.0f, l.accent.b / 255.0f };
    DrawPrefsSettingRowBegin("Background colour");
    if (ImGui::ColorEdit3("##bg", bg, ImGuiColorEditFlags_NoInputs)) {
        l.background = Color{ (unsigned char)(bg[0] * 255.0f + 0.5f), (unsigned char)(bg[1] * 255.0f + 0.5f), (unsigned char)(bg[2] * 255.0f + 0.5f), 255 };
        changed = true;
    }
    DrawPrefsSettingRowBegin("Accent colour (bar, percentage)");
    if (ImGui::ColorEdit3("##ac", ac, ImGuiColorEditFlags_NoInputs)) {
        l.accent = Color{ (unsigned char)(ac[0] * 255.0f + 0.5f), (unsigned char)(ac[1] * 255.0f + 0.5f), (unsigned char)(ac[2] * 255.0f + 0.5f), 255 };
        changed = true;
    }
    ImGui::EndDisabled();

    // A small picture of the result.
    {
        const ImVec2 size(360.0f, 150.0f);
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::Indent(8.0f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(size);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const auto col = [](Color c) { return IM_COL32(c.r, c.g, c.b, 255); };
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), col(l.background), 6.0f);
        if (!l.enabled) {
            dl->AddText(ImVec2(p.x + 12.0f, p.y + 12.0f), IM_COL32(150, 150, 150, 255), "(no loading screen)");
        } else {
            const bool light = (l.background.r * 0.3f + l.background.g * 0.59f + l.background.b * 0.11f) > 140.0f;
            const ImU32 ink = light ? IM_COL32(20, 22, 26, 255) : IM_COL32(235, 240, 245, 255);
            std::string title = l.title;
            if (const size_t at = title.find("{name}"); at != std::string::npos) title.replace(at, 6, "MyProject");
            float y = p.y + (l.showLogs ? 10.0f : 28.0f);
            const auto centred = [&](const std::string& s, ImU32 c) {
                dl->AddText(ImVec2(p.x + (size.x - ImGui::CalcTextSize(s.c_str()).x) * 0.5f, y), c, s.c_str());
                y += ImGui::GetTextLineHeight() + 6.0f;
            };
            centred(title, ink);
            if (l.showPercent) centred("64%", col(l.accent));
            if (l.showBar) {
                dl->AddRectFilled(ImVec2(p.x + 60.0f, y), ImVec2(p.x + size.x - 60.0f, y + 5.0f), IM_COL32(128, 128, 128, 60), 3.0f);
                dl->AddRectFilled(ImVec2(p.x + 60.0f, y), ImVec2(p.x + 60.0f + (size.x - 120.0f) * 0.64f, y + 5.0f), col(l.accent), 3.0f);
                y += 13.0f;
            }
            if (l.showStage) centred("Loading objects (256 / 400)", IM_COL32(150, 150, 150, 255));
            if (l.showLogs) {
                const float ly = std::max(y + 4.0f, p.y + size.y - 50.0f);
                dl->AddRectFilled(ImVec2(p.x + 20.0f, ly), ImVec2(p.x + size.x - 20.0f, p.y + size.y - 8.0f), IM_COL32(0, 0, 0, 70), 4.0f);
                dl->AddText(ImVec2(p.x + 28.0f, ly + 4.0f), IM_COL32(150, 150, 150, 255), "[terrain] loaded 1 terrain(s)");
                dl->AddText(ImVec2(p.x + 28.0f, ly + 20.0f), ink, "[city] loaded 1 city(ies)");
            }
        }
        ImGui::Unindent(8.0f);
    }
    ImGui::PopID();
}

// AI: off by default. Nothing is sent anywhere until this is enabled and a key is entered. The key lives in ai.cfg in the
// machine config folder (owner-only), never in a project, so it is not exported or committed.
static void DrawPreferencesAITab() {
    g_prefsFirstRow = true;
    ai::Settings& s = ai::GetSettings();
    if (!ai::Available()) { ImGui::TextDisabled("AI is not available in this build (built without libcurl)."); return; }
    bool changed = false;

    DrawPrefsSettingRowBegin("Enable AI assistant");
    if (ImGui::Checkbox("##aienable", &s.enabled)) {
        changed = true;
        if (!s.enabled) ai::ClearHistory();
        else SetPanelVisible(PanelId::AI, true);   // the panel appears under the Explorer
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::Indent(8.0f);
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - 8.0f);
    ImGui::TextDisabled("Off by default. When on, the messages you send and the Flyengine documentation are sent to the "
                        "provider you choose. Nothing is sent until you press Send in the AI Assistant window; testing the connection and "
                        "refreshing the model list send only your key.");
    ImGui::PopTextWrapPos();
    ImGui::Unindent(8.0f);

    ImGui::BeginDisabled(!s.enabled);
    DrawPrefsSettingRowBegin("Provider");
    if (ImTextureID icon = AIProviderIcon(ai::ProviderIconIndex(s.provider))) {
        ImGui::Image(icon, ImVec2(20.0f, 20.0f));
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(116.0f);
    if (ImGui::BeginCombo("##aiprovider", ai::ProviderShortName(s.provider))) {
        for (int i = 0; i < ai::kProviderCount; ++i) {
            const ai::ProviderId id = static_cast<ai::ProviderId>(i);
            if (ImTextureID icon = AIProviderIcon(ai::ProviderIconIndex(id))) { ImGui::Image(icon, ImVec2(16.0f, 16.0f)); ImGui::SameLine(); }
            if (ImGui::Selectable(ai::ProviderName(id), id == s.provider) && id != s.provider) {
                s.provider = id;
                ai::ResetTestStatus();
                ai::MaybeRefreshModels();
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    // Each provider keeps its own key and model, so switching back and forth loses nothing.
    DrawPrefsSettingRowBegin("API key");
    static char keyBuf[256] = "";
    static int keyBufProvider = -1;
    static bool showKey = false;
    if (keyBufProvider != ai::Index(s.provider)) {
        std::snprintf(keyBuf, sizeof keyBuf, "%s", ai::SelectedKey().c_str());
        keyBufProvider = ai::Index(s.provider);
    }
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputText("##aikey", keyBuf, sizeof keyBuf, showKey ? 0 : ImGuiInputTextFlags_Password);
    if (ImGui::IsItemDeactivatedAfterEdit()) { ai::SelectedKey() = keyBuf; changed = true; ai::TestConnection(); ai::RefreshModels(); }
    ImGui::Indent(8.0f);
    if (ImGui::SmallButton(showKey ? "Hide" : "Show")) showKey = !showKey;
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear key")) { keyBuf[0] = 0; ai::SelectedKey().clear(); changed = true; }
    if (ai::KeyFromEnvironment()) ImGui::TextDisabled("%s is set and is used instead of this field.", ai::ProviderEnvVar(s.provider));
    ImGui::Unindent(8.0f);

    DrawPrefsSettingRowBegin("Model");
    if (DrawAIModelCombo("##aimodel", 140.0f)) changed = true;
    DrawPrefsSettingRowBegin("Model list");
    ImGui::BeginDisabled(ai::EffectiveKey().empty() || ai::ModelsLoading());
    if (ImGui::Button("Refresh from provider", ImVec2(140.0f, 24.0f))) ai::RefreshModels();
    ImGui::EndDisabled();
    ImGui::Indent(8.0f);
    {
        std::string status = ai::ModelsStatus(s.provider);
        if (ai::ModelsLoading()) status = "Loading models...";
        else if (status.empty()) status = ai::Models(s.provider).empty() ? "Not fetched yet." : std::to_string(ai::Models(s.provider).size()) + " models (cached)";
        ImGui::TextDisabled("%s", status.c_str());
    }
    ImGui::Unindent(8.0f);
    DrawPrefsSettingRowBegin("Show non-chat models");
    if (ImGui::Checkbox("##aishowall", &s.showAllModels)) changed = true;
    // Any other model id the API accepts (a newer release, a dated snapshot) can be typed here.
    DrawPrefsSettingRowBegin("Custom model ID");
    static char modelBuf[128] = "";
    if (!ImGui::IsAnyItemActive() && ai::SelectedModel() != modelBuf) std::snprintf(modelBuf, sizeof modelBuf, "%s", ai::SelectedModel().c_str());
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputText("##aimodelid", modelBuf, sizeof modelBuf);
    if (ImGui::IsItemDeactivatedAfterEdit() && modelBuf[0]) { ai::SelectedModel() = modelBuf; changed = true; }

    DrawPrefsSettingRowBegin("Connection");
    ImGui::BeginDisabled(ai::EffectiveKey().empty() || ai::Busy());
    if (ImGui::Button("Test connection", ImVec2(140.0f, 24.0f))) ai::TestConnection();
    ImGui::EndDisabled();
    if (!ai::TestStatus().empty()) {
        ImGui::Indent(8.0f);
        const bool ok = ai::TestStatus() == "OK";
        if (ImTextureID icon = AIPanelIcon(3)) {
            ImVec4 tint = ai::TestStatus() == "Testing..." ? ImVec4(1, 1, 1, 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 6.0f)) : ImVec4(1, 1, 1, 1);
            ImGui::ImageWithBg(icon, ImVec2(16.0f, 16.0f), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tint);
            ImGui::SameLine();
        }
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - 8.0f);
        ImGui::TextColored(ok ? ImVec4(0.4f, 0.9f, 0.5f, 1.0f) : ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", ai::TestStatus().c_str());
        ImGui::PopTextWrapPos();
        ImGui::Unindent(8.0f);
    }
    ImGui::EndDisabled();

    if (changed) ai::Save();
}

static void DrawPreferencesLoadingTab() {
    g_prefsFirstRow = true;
    loading::Settings& s = loading::GetSettings();
    bool changed = false;
    static int which = 0;       // 0 = the editor's, 1 = the player's
    ImGui::Indent(8.0f);
    if (ImGui::RadioButton("Editor", which == 0)) which = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Player", which == 1)) which = 1;
    ImGui::Unindent(8.0f);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    DrawLoadingLookEditor(which == 0 ? "editor" : "player", which == 0 ? s.editor : s.player, changed);
    if (changed) loading::Save();

    if (which == 1) {
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::Indent(8.0f);
        const std::string folder = project::GetCurrentProject().path;
        if (!folder.empty()) {
            const bool has = loading::HasProjectOverride(folder);
            if (ImGui::Button("Use this look for this project only")) loading::SaveProjectOverride(folder);
            ImGui::SameLine();
            ImGui::BeginDisabled(!has);
            if (ImGui::Button("Remove the project's own look")) loading::RemoveProjectOverride(folder);
            ImGui::EndDisabled();
            ImGui::TextDisabled(has ? "This project has its own player loading screen (loading.cfg in its folder); it wins over these settings."
                                    : "The player uses these settings. Saving them for the project makes the game carry its own loading screen.");
        }
        ImGui::TextDisabled("Command line: --noloading, --loading, --loadinglogs");
        ImGui::Unindent(8.0f);
    }
}

void DrawImGuiPreferencesWindow() {
    if (!g_showPreferences) return;

    ImGuiIO& io = ImGui::GetIO();

    // Dim everything behind the panel so it reads as an actual dialog instead
    // of a rectangle floating over the viewport.
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0.0f, 0.0f), io.DisplaySize, IM_COL32(0, 0, 0, 140));

    ImVec2 winSize(std::min(1000.0f, io.DisplaySize.x - 80.0f), std::min(560.0f, io.DisplaySize.y - 80.0f));
    ImVec2 winPos((io.DisplaySize.x - winSize.x) * 0.5f, (io.DisplaySize.y - winSize.y) * 0.5f);

    ImGui::SetNextWindowSize(winSize, ImGuiCond_Always);
    ImGui::SetNextWindowPos(winPos, ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, theme::AccentVec(0.35f));
    if (!ImGui::Begin("##Preferences", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
        return;
    }

    // Header strip - its own filled background + bottom separator so it reads
    // as a title bar rather than a bare gap with a stray X in it.
    const float headerHeight = 40.0f;
    ImVec2 headerP0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(headerP0, ImVec2(headerP0.x + winSize.x, headerP0.y + headerHeight),
                       IM_COL32(20, 22, 27, 255), 8.0f, ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(headerP0.x, headerP0.y + headerHeight), ImVec2(headerP0.x + winSize.x, headerP0.y + headerHeight),
                IM_COL32(255, 255, 255, 20));

    ImGui::BeginChild("##PrefsHeader", ImVec2(0.0f, headerHeight), false);
    ImGui::SetCursorPos(ImVec2(14.0f, (headerHeight - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextUnformatted("Preferences");
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - headerHeight, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.88f, 1.0f));
    if (ImGui::Button("X", ImVec2(headerHeight, headerHeight))) {
        g_showPreferences = false;
    }
    ImGui::PopStyleColor(4);
    ImGui::EndChild();

    // Left sidebar
    ImGui::BeginChild("##PrefsSidebar", ImVec2(200.0f, 0.0f), false);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::Indent(10.0f);

    DrawPrefsSidebarButton("General", 0);
    DrawPrefsSidebarButton("AI", 1);
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    DrawPrefsSidebarButton("Scripting", 2);
    DrawPrefsSidebarButton("Plugins", 3);
    DrawPrefsSidebarButton("Rendering", 4);
    DrawPrefsSidebarButton("Loading", 6);
    DrawPrefsSidebarButton("Layout", 7);
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    DrawPrefsSidebarButton("Appearance", 5);

    ImGui::Unindent(10.0f);
    ImGui::PopStyleVar();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 0.0f);

    // Vertical divider between sidebar and content
    {
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(p0, ImVec2(p0.x, p0.y + ImGui::GetContentRegionAvail().y),
                                            IM_COL32(255, 255, 255, 20));
        ImGui::Dummy(ImVec2(1.0f, 0.0f));
        ImGui::SameLine(0.0f, 0.0f);
    }

    // Right content area
    ImGui::BeginChild("##PrefsContent", ImVec2(0.0f, 0.0f), false);
    ImGui::Dummy(ImVec2(0.0f, 16.0f));
    switch (g_prefsCategory) {
        case 0: DrawPreferencesGeneralTab(); break;
        case 1: DrawPreferencesAITab(); break;
        case 3: DrawPreferencesPluginsTab(); break;
        case 4: DrawPreferencesRenderingTab(); break;
        case 6: DrawPreferencesLoadingTab(); break;
        case 7: DrawPreferencesLayoutTab(); break;
        default: ImGui::TextDisabled("Nothing to configure here yet."); break;
    }
    ImGui::EndChild();

    ImGui::End();
    ImGui::PopStyleColor(); // border
    ImGui::PopStyleVar(3); // rounding, border size, outer padding
}

void ShowPreferences(bool show, int category) {
    g_showPreferences = show;
    if (show) g_prefsCategory = category;
}

} // namespace ui
