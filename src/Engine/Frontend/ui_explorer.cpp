// ui_explorer.cpp: the Explorer panel and its right-click menu, split out of ui.cpp.
#include "ui_internal.hpp"
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
#include "../../../include/Engine/Graphics.hpp"
#include "../../../include/Engine/PostFX.hpp"
#include "../../../include/Engine/LoadingScreen.hpp"
#include "../include/Engine/Scripts/ScriptLauncher.hpp"
#include "../../../include/Engine/Scripts/NativeScriptHost.hpp"
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
#include "ui_internal.hpp"

namespace ui {

// Deletes a script for real: its graph, its Explorer entry and its .cpp, and tells the host so the still compiled
// class doesn't bring it back. `index` is its StandaloneScripts slot, or -1 to look it up by key.
void DeleteScriptEverywhere(const std::string& key, int index) {
    vscript::RemoveByName(key);
    ScriptRuntime* rt = GetActiveRuntime();
    if (!rt) return;
    auto& scripts = rt->StandaloneScripts();
    if (index < 0)
        for (int i = 0; i < static_cast<int>(scripts.size()); ++i)
            if (ScriptKey(scripts[i]) == key) { index = i; break; }
    if (index >= 0 && index < static_cast<int>(scripts.size())) rt->RemoveStandalone(index);
    g_scriptSel.erase(key);
    if (scriptLauncher::DeleteScriptFile(key) && rt->GetHost()) rt->GetHost()->ForgetScriptClass(key);
}

// True when the script menu was opened on one of several selected scripts: the menu then acts on all of them.
static bool ScriptMenuIsMulti() {
    ScriptRuntime* rt = GetActiveRuntime();
    if (!rt || g_scriptSel.size() < 2) return false;
    auto& scripts = rt->StandaloneScripts();
    return g_explorerMenuScript >= 0 && g_explorerMenuScript < static_cast<int>(scripts.size()) &&
           g_scriptSel.count(ScriptKey(scripts[g_explorerMenuScript])) > 0;
}

// Graphs made in the Visual Script tab get a script entry of their own, so the Explorer lists them too.
static void SyncVisualScriptEntries() {
    ScriptRuntime* rt = GetActiveRuntime();
    if (!rt) return;
    auto& scripts = rt->StandaloneScripts();
    for (int i = 0; i < vscript::Count(); ++i) {
        const std::string key = vscript::NameAt(i);
        bool found = false;
        for (const auto& s : scripts) if (ScriptKey(s) == key) { found = true; break; }
        if (!found && g_sceneObjects)
            for (auto* o : *g_sceneObjects) if (o && o->script == key) { found = true; break; }
        if (found) continue;
        ScriptRuntime::StandaloneScript s;
        s.name = key;
        s.typeName = key;
        s.runOnPlay = true;
        scripts.push_back(std::move(s));
    }
}

const char* ExplorerMenuItemLabel(int i) {
    switch (g_explorerMenuMode) {
        case ExplorerMenuMode::Object: {
            bool singleSel = g_explorerMenuObject
                && g_selection.size() == 1
                && std::find(g_selection.begin(), g_selection.end(), g_explorerMenuObject) != g_selection.end();
            bool has = singleSel && !g_explorerMenuObject->script.empty();
            bool canGroup = g_selection.size() >= 2;
            bool inModel = g_explorerMenuObject && g_explorerMenuObject->parentModel;
            int cursor = 0;
            if (singleSel && i == cursor++) return has ? "Edit Script" : "Add Script";
            if (has && i == cursor++) return "Remove Script";
            if (has && i == cursor++) return "Choose Editor...";
            if (has && i == cursor++) return "Open in Visual Script";
            if (canGroup && i == cursor++) return "Group";
            if (inModel && i == cursor++) return "Ungroup";
            return nullptr;
        }
        case ExplorerMenuMode::Model: {
            if (i == 0) return "Ungroup";
            if (i == 1) return "Delete";
            return nullptr;
        }
        case ExplorerMenuMode::Empty:
            return i == 0 ? "Insert Script" : nullptr;
        case ExplorerMenuMode::Script:
            if (ScriptMenuIsMulti()) return i == 0 ? "Open in Visual Script" : (i == 1 ? "Delete Selected" : nullptr);
            return i == 0 ? "Rename" : (i == 1 ? "Delete" : (i == 2 ? "Choose Editor..." : (i == 3 ? "Open in Visual Script" : nullptr)));
    }
    return nullptr;
}

static void PerformExplorerMenuAction(int i) {
    // Object/group/model actions work without a script runtime; only the
    // script-list actions need one, and they check below.
    switch (g_explorerMenuMode) {
        case ExplorerMenuMode::Object: {
            if (!g_explorerMenuObject) return;
            bool singleSel = g_selection.size() == 1
                && std::find(g_selection.begin(), g_selection.end(), g_explorerMenuObject) != g_selection.end();
            bool has = singleSel && !g_explorerMenuObject->script.empty();
            bool canGroup = g_selection.size() >= 2;
            bool inModel = g_explorerMenuObject->parentModel != nullptr;
            int cursor = 0;
            if (singleSel && i == cursor++) {
                scriptLauncher::EditObjectScript(g_explorerMenuObject);
                return;
            }
            if (has && i == cursor++) {
                if (ScriptRuntime* rt = GetActiveRuntime()) rt->StopObjectScript(g_explorerMenuObject);
                g_explorerMenuObject->script.clear();
                g_explorerMenuObject->runOnPlay = false;
                return;
            }
            if (has && i == cursor++) {
                scriptLauncher::ChooseEditorForObjectScript(g_explorerMenuObject);
                return;
            }
            if (has && i == cursor++) {
                vscript::OpenByName(scriptLauncher::MakeClassName(g_explorerMenuObject->script));
                SelectBottomTab(2);
                return;
            }
            if (canGroup && i == cursor++) {
                GroupSelectedObjects();
                return;
            }
            if (inModel && i == cursor++) {
                UngroupModelContaining(g_explorerMenuObject);
                return;
            }
            break;
        }
        case ExplorerMenuMode::Model: {
            if (!g_explorerMenuObject || !g_explorerMenuObject->parentModel) return;
            if (i == 0) {
                UngroupModelContaining(g_explorerMenuObject);
            } else if (i == 1) {
                ModelGroup* model = g_explorerMenuObject->parentModel;
                std::vector<ScatteredObject*> targets = model->members;
                UngroupModelContaining(g_explorerMenuObject);
                RequestDelete(targets);
                SetSelection({}, nullptr);
            }
            break;
        }
        case ExplorerMenuMode::Empty: {
            if (ScriptRuntime* rt = GetActiveRuntime()) {
                auto& scripts = rt->StandaloneScripts();
                ScriptRuntime::StandaloneScript s;
                s.name = "Script";
                s.typeName = "";
                s.runOnPlay = true;
                scripts.push_back(std::move(s));
                auto idx = static_cast<int>(scripts.size()) - 1;
                std::string className;
                scriptLauncher::EditStandaloneScript(scripts[idx].name, scripts[idx].typeName, &className);
                if (!className.empty() && scripts[idx].typeName.empty())
                    scripts[idx].typeName = className;
            }
            break;
        }
        case ExplorerMenuMode::Script: {
            if (ScriptRuntime* rt = GetActiveRuntime()) {
                auto& scripts = rt->StandaloneScripts();
                if (g_explorerMenuScript < 0 || g_explorerMenuScript >= static_cast<int>(scripts.size())) return;
                if (ScriptMenuIsMulti()) {
                    const std::set<std::string> keys = g_scriptSel;
                    if (i == 0) {
                        for (const auto& k : keys) vscript::OpenByName(k);
                        SelectBottomTab(2);
                    } else if (i == 1) {
                        for (int k = static_cast<int>(scripts.size()) - 1; k >= 0; --k)
                            if (keys.count(ScriptKey(scripts[k]))) {
                                DeleteScriptEverywhere(ScriptKey(scripts[k]), k);
                            }
                        g_scriptSel.clear();
                    }
                    return;
                }
                if (i == 0) {
                    g_scriptRenameIndex = g_explorerMenuScript;
                    snprintf(g_scriptNameBuffer, sizeof(g_scriptNameBuffer), "%s", scripts[g_explorerMenuScript].name.c_str());
                    g_scriptNameBuffer[sizeof(g_scriptNameBuffer) - 1] = '\0';
                    g_cursorPos = static_cast<int>(strlen(g_scriptNameBuffer));
                    g_selStart = -1;
                } else if (i == 1) {
                    DeleteScriptEverywhere(ScriptKey(scripts[g_explorerMenuScript]), g_explorerMenuScript);
                } else if (i == 2) {
                    auto& s = scripts[g_explorerMenuScript];
                    scriptLauncher::ChooseEditorForStandaloneScript(s.name, s.typeName);
                } else if (i == 3) {
                    vscript::OpenByName(ScriptKey(scripts[g_explorerMenuScript]));
                    SelectBottomTab(2);
                }
            }
            break;
        }
    }
}

static void OpenExplorerMenu(Vector2 pos, ExplorerMenuMode mode, ScatteredObject* obj, int scriptIndex) {
    CloseContextMenu();
    g_explorerMenuOpen = true;
    g_explorerMenuPos = pos;
    g_explorerMenuMode = mode;
    g_explorerMenuObject = obj;
    g_explorerMenuScript = scriptIndex;
    g_explorerMenuOpenTime = GetTime();
}

// ImGui popup version of the explorer's right-click menu. Runs in the ImGui
// frame (unlike the raylib 2D pass, which is always painted *under* every
// ImGui window), so it can never end up hidden behind the Explorer panel.
void DrawImGuiExplorerMenu() {
    // g_explorerMenuOpenTime is refreshed on every OpenExplorerMenu() call,
    // including retargeting an already-open menu to a new row - use it (not
    // IsPopupOpen) to detect "new request", so right-clicking a different
    // row while the menu is open moves it there in one click instead of
    // needing a close-then-reopen.
    static double s_lastHandledOpenTime = -1.0;
    if (g_explorerMenuOpen && g_explorerMenuOpenTime != s_lastHandledOpenTime) {
        s_lastHandledOpenTime = g_explorerMenuOpenTime;
        ImGui::SetNextWindowPos(ImVec2(g_explorerMenuPos.x, g_explorerMenuPos.y));
        ImGui::OpenPopup("##ExplorerCtxMenu");
    }
    if (ImGui::BeginPopup("##ExplorerCtxMenu")) {
        int n = ExplorerMenuItemCount();
        for (int i = 0; i < n; ++i) {
            const char* label = ExplorerMenuItemLabel(i);
            if (!label) continue;
            if (ImGui::MenuItem(label)) {
                PerformExplorerMenuAction(i);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    // Mirror ImGui's own popup lifetime back into the shared flag so other
    // click handlers (which check !g_explorerMenuOpen) stay correctly gated
    // while the popup is visible, and un-gate once it closes for any reason
    // (item picked, Escape, click outside).
    g_explorerMenuOpen = ImGui::IsPopupOpen("##ExplorerCtxMenu");
}

// ---------------------------------------------------------------------------
// 2. Left Panel (Explorer Window)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 2. Left Panel (Explorer Window, Dear ImGui)
// Reuses the existing rename/selection/drag-drop state (g_renameObject,
// g_explorerDrag*, OpenExplorerMenu, PerformExplorerDrop, ...) - only the
// input/rendering is now driven by ImGui widgets instead of raylib hit tests.
// Called once per frame from DrawImGuiFrame().
// ---------------------------------------------------------------------------
static bool ExplorerRenameActive() {
    return g_renameObject || g_renameModel || g_renameTerrain || g_renameLegacyTerrain || g_scriptRenameIndex >= 0;
}

// Draws one row as an ImGui Selectable and returns true if it was clicked
// (left button, on this frame).
static bool ExplorerRow(const char* label, bool selected, bool* outRightClicked) {
    ImGuiSelectableFlags flags = ImGuiSelectableFlags_AllowDoubleClick;
    bool clicked = ImGui::Selectable(label, selected, flags, ImVec2(0.0f, 20.0f));
    if (outRightClicked) {
        *outRightClicked = ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    }
    return clicked;
}

// Same as ExplorerRow, but draws a fixed-size icon (from assets/EditorIcons/)
// before the text instead of a leading unicode glyph.
static bool ExplorerRowIcon(RowIconType icon, const char* label, bool selected, bool* outRightClicked) {
    ImVec2 rowStart = ImGui::GetCursorPos();
    ImGuiSelectableFlags flags = ImGuiSelectableFlags_AllowDoubleClick;
    bool clicked = ImGui::Selectable("##row", selected, flags, ImVec2(0.0f, 20.0f));
    if (outRightClicked) {
        *outRightClicked = ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    }
    ImGui::SetCursorPos(ImVec2(rowStart.x + 4.0f, rowStart.y + 2.0f));
    ImTextureID tex = RowTex(icon);
    if (tex) {
        ImGui::Image(tex, ImVec2(14.0f, 14.0f));
        ImGui::SameLine(0.0f, 6.0f);
    } else {
        ImGui::Dummy(ImVec2(14.0f, 14.0f));
        ImGui::SameLine(0.0f, 6.0f);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    return clicked;
}

// Small helper: draw+commit/cancel an inline rename InputText, focusing it
// the first frame it appears. Returns true once Enter/blur commits the value.
static bool ExplorerRenameField(const char* imguiId, char* buf, size_t bufSize, bool justOpened) {
    ImGui::SetNextItemWidth(-1.0f);
    if (justOpened) ImGui::SetKeyboardFocusHere();
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
    bool committed = ImGui::InputText(imguiId, buf, bufSize, flags);
    bool cancel = ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    bool blurred = ImGui::IsItemDeactivatedAfterEdit() && !committed;
    if (committed || blurred) return true;
    if (cancel) { *buf = '\0'; return false; } // caller checks g_render* pointers, not the buffer, to detect cancel
    return false;
}

void DrawImGuiExplorer() {
    Rectangle panelRec = GetExplorerPanelBounds();

    ImGui::SetNextWindowPos(ImVec2(panelRec.x, panelRec.y));
    ImGui::SetNextWindowSize(ImVec2(panelRec.width, panelRec.height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##Explorer", nullptr, flags);

    ImGui::TextColored(theme::AccentVec(), "EXPLORER");
    ImGui::Separator();

    bool rightClickHandled = false;
    bool rowClickedThisFrame = false;
    ImVec2 mouseImgui = ImGui::GetMousePos();
    Vector2 mouse = { mouseImgui.x, mouseImgui.y };

    ImGui::BeginChild("##ExplorerContent", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextDisabled("SCENE");

    if (g_sceneObjects != nullptr) {
        // F2 to rename the current selection, matching the old raylib shortcut.
        if (!ExplorerRenameActive() && IsKeyPressed(KEY_F2)) {
            if (g_selectedTerrain) {
                g_renameTerrain = g_selectedTerrain;
                snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", g_selectedTerrain->GetName().c_str());
            } else if (terrain::GetTerrainEditorState().selectedTerrainLegacy) {
                terrain::Terrain* legacy = terrain::GetTerrainEditorState().selectedTerrainLegacy;
                g_renameLegacyTerrain = legacy;
                snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", legacy->GetName().c_str());
            } else if (g_primarySelection) {
                ModelGroup* pm = g_primarySelection->parentModel;
                bool modelAllSelected = pm && !pm->members.empty();
                if (pm) {
                    for (auto* m : pm->members) {
                        if (!m || !m->isSelected) { modelAllSelected = false; break; }
                    }
                }
                if (modelAllSelected) {
                    g_renameModel = pm;
                    snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", pm->name.c_str());
                } else {
                    g_renameObject = g_primarySelection;
                    snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", g_primarySelection->GetName().c_str());
                }
            }
            g_nameBuffer[sizeof(g_nameBuffer) - 1] = '\0';
        }

        std::vector<ExplorerEntry> entries = BuildExplorerEntries();

        for (const auto& entry : entries) {
            const void* stableId = entry.isHeader ? static_cast<const void*>(entry.model)
                : entry.water ? static_cast<const void*>(entry.water)
                : entry.basicTerrain ? static_cast<const void*>(entry.basicTerrain)
                : entry.legacyTerrain ? static_cast<const void*>(entry.legacyTerrain)
                : static_cast<const void*>(entry.obj);
            ImGui::PushID(stableId);

            // --- Model header row ---
            if (entry.isHeader) {
                ModelGroup* model = entry.model;
                if (!model) { ImGui::PopID(); continue; }

                bool modelSelected = !model->members.empty();
                for (auto* m : model->members) {
                    if (!m || !m->isSelected) { modelSelected = false; break; }
                }

                if (model == g_renameModel) {
                    static ModelGroup* s_lastRenameModel = nullptr;
                    bool justOpened = (s_lastRenameModel != model);
                    s_lastRenameModel = model;
                    if (ExplorerRenameField("##rename", g_nameBuffer, sizeof(g_nameBuffer), justOpened)) {
                        model->name = g_nameBuffer;
                        g_renameModel = nullptr;
                    } else if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()) {
                        g_renameModel = nullptr; // ESC / blur-without-edit cancels
                    }
                } else {
                    char label[128];
                    snprintf(label, sizeof(label), "%s (%d)", model->name.c_str(), static_cast<int>(model->members.size()));
                    bool rightClicked = false;
                    bool clicked = ExplorerRowIcon(ROWICON_OBJECT, label, modelSelected, &rightClicked);

                    if (g_explorerDragActive && ImGui::IsItemHovered()) {
                        g_explorerDropTarget = model;
                        g_explorerDropValid = true;
                    }

                    if (clicked && !ExplorerRenameActive() && !g_explorerMenuOpen) {
                        bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
                        bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                        if (ctrl) {
                            std::vector<ScatteredObject*> next = g_selection;
                            if (modelSelected) {
                                next.erase(std::remove_if(next.begin(), next.end(),
                                    [&](ScatteredObject* o) {
                                        return std::find(model->members.begin(), model->members.end(), o) != model->members.end();
                                    }), next.end());
                            } else {
                                for (auto* m : model->members) {
                                    if (m && std::find(next.begin(), next.end(), m) == next.end()) next.push_back(m);
                                }
                            }
                            g_explorerAnchorIndex = entry.objRowIndex;
                            SetSelection(next, KeepPrimary(next, g_primarySelection));
                        } else if (shift && g_explorerAnchorIndex >= 0) {
                            int a = g_explorerAnchorIndex, b = entry.objRowIndex;
                            if (a < 0) a = b;
                            if (a > b) std::swap(a, b);
                            std::vector<ScatteredObject*> sel;
                            for (const auto& e : entries) {
                                if (!e.isHeader && e.obj && e.objRowIndex >= a && e.objRowIndex <= b) sel.push_back(e.obj);
                            }
                            ScatteredObject* primary = model->members.empty() ? nullptr : model->members.front();
                            SetSelection(sel, KeepPrimary(sel, primary));
                        } else {
                            SetSelection(model->members, model->members.empty() ? nullptr : model->members.front());
                            g_explorerAnchorIndex = entry.objRowIndex;
                            double now = GetTime();
                            if (g_lastExplorerModelClick == model && now - g_lastExplorerClickTime < 0.35) {
                                g_renameModel = model;
                                snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", model->name.c_str());
                                g_nameBuffer[sizeof(g_nameBuffer) - 1] = '\0';
                                g_lastExplorerModelClick = nullptr;
                            } else {
                                g_lastExplorerModelClick = model;
                                g_lastExplorerClickTime = now;
                            }
                        }
                        rowClickedThisFrame = true;
                    } else if (rightClicked && !ExplorerRenameActive()) {
                        if (!modelSelected) {
                            SetSelection(model->members, model->members.empty() ? nullptr : model->members.front());
                        }
                        rightClickHandled = true;
                        if (!model->members.empty()) OpenExplorerMenu(mouse, ExplorerMenuMode::Model, model->members.front(), -1);
                        rowClickedThisFrame = true;
                    }

                    if (ImGui::IsItemActivated() && !ExplorerRenameActive() && !g_explorerMenuOpen && !model->members.empty()) {
                        g_explorerDragPending = true;
                        g_explorerDragObjects = model->members;
                        g_explorerDragStart = mouse;
                    }
                }
                ImGui::PopID();
                continue;
            }

            // --- Water body row ---
            if (entry.water) {
                WaterBody* water = entry.water;
                if (!water) { ImGui::PopID(); continue; }
                char label[128];
                snprintf(label, sizeof(label), "%s", water->GetName().c_str());
                bool rightClicked = false;
                bool clicked = ExplorerRowIcon(ROWICON_WATER, label, water->isSelected, &rightClicked);
                if (clicked && !ExplorerRenameActive() && !g_explorerMenuOpen) {
                    g_explorerAnchorIndex = entry.objRowIndex;
                    SetSelectedWater(g_selectedWater == water ? nullptr : water);
                    rowClickedThisFrame = true;
                } else if (rightClicked && !ExplorerRenameActive()) {
                    SetSelectedWater(water);
                    rightClickHandled = true;
                    rowClickedThisFrame = true;
                }
                ImGui::PopID();
                continue;
            }

            // --- BasicTerrain row ---
            if (entry.basicTerrain) {
                BasicTerrain* terr = entry.basicTerrain;
                if (!terr) { ImGui::PopID(); continue; }
                bool isRenaming = (g_renameTerrain == terr);
                bool isSel = terr->isSelected || terr == BasicTerrain::GetActive();

                if (isRenaming) {
                    static BasicTerrain* s_lastRenameTerrain = nullptr;
                    bool justOpened = (s_lastRenameTerrain != terr);
                    s_lastRenameTerrain = terr;
                    if (ExplorerRenameField("##rename", g_nameBuffer, sizeof(g_nameBuffer), justOpened)) {
                        terr->SetName(g_nameBuffer);
                        g_renameTerrain = nullptr;
                    } else if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()) {
                        g_renameTerrain = nullptr;
                    }
                } else {
                    char label[128];
                    snprintf(label, sizeof(label), "%s", terr->GetName().c_str());
                    bool rightClicked = false;
                    bool clicked = ExplorerRowIcon(ROWICON_TERRAIN, label, isSel, &rightClicked);
                    if (clicked && !g_explorerMenuOpen) {
                        SetSelectedTerrain(g_selectedTerrain == terr ? nullptr : terr);
                        rowClickedThisFrame = true;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            g_renameTerrain = terr;
                            snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", terr->GetName().c_str());
                            g_nameBuffer[sizeof(g_nameBuffer) - 1] = '\0';
                        }
                    } else if (rightClicked) {
                        SetSelectedTerrain(terr);
                        rightClickHandled = true;
                        rowClickedThisFrame = true;
                    }
                }
                ImGui::PopID();
                continue;
            }

            // --- Legacy chunked terrain::Terrain row ---
            if (entry.legacyTerrain) {
                terrain::Terrain* legacy = entry.legacyTerrain;
                if (!legacy) { ImGui::PopID(); continue; }
                bool isSel = terrain::GetTerrainEditorState().selectedTerrainLegacy == legacy;
                bool isRenaming = (g_renameLegacyTerrain == legacy);

                if (isRenaming) {
                    static terrain::Terrain* s_lastRenameLegacy = nullptr;
                    bool justOpened = (s_lastRenameLegacy != legacy);
                    s_lastRenameLegacy = legacy;
                    if (ExplorerRenameField("##rename", g_nameBuffer, sizeof(g_nameBuffer), justOpened)) {
                        std::string newName = g_nameBuffer;
                        if (newName.empty()) newName = "Terrain";
                        bool taken = false;
                        for (auto* t : terrain::GetTerrainRegistry().GetTerrains()) {
                            if (t && t != legacy && t->GetName() == newName) { taken = true; break; }
                        }
                        if (taken) LogAlways("[terrain] name '%s' is already in use - rename blocked", newName.c_str());
                        else legacy->SetName(newName);
                        g_renameLegacyTerrain = nullptr;
                    } else if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()) {
                        g_renameLegacyTerrain = nullptr;
                    }
                } else {
                    char label[128];
                    snprintf(label, sizeof(label), "%s", legacy->GetName().c_str());
                    bool rightClicked = false;
                    bool clicked = ExplorerRowIcon(ROWICON_TERRAIN, label, isSel, &rightClicked);
                    if (clicked && !g_explorerMenuOpen) {
                        terrain::HandleTerrainSelection(legacy, !isSel);
                        rowClickedThisFrame = true;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            g_renameLegacyTerrain = legacy;
                            snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", legacy->GetName().c_str());
                            g_nameBuffer[sizeof(g_nameBuffer) - 1] = '\0';
                        }
                    } else if (rightClicked) {
                        terrain::HandleTerrainSelection(legacy, true);
                        rightClickHandled = true;
                        rowClickedThisFrame = true;
                    }
                }
                ImGui::PopID();
                continue;
            }

            // --- Object / model member row ---
            ScatteredObject* obj = entry.obj;
            if (!obj) { ImGui::PopID(); continue; }
            bool isMember = obj->parentModel != nullptr;

            if (g_renameObject == obj) {
                if (isMember) ImGui::Indent(14.0f);
                static ScatteredObject* s_lastRenameObj = nullptr;
                bool justOpened = (s_lastRenameObj != obj);
                s_lastRenameObj = obj;
                if (ExplorerRenameField("##rename", g_nameBuffer, sizeof(g_nameBuffer), justOpened)) {
                    obj->SetName(g_nameBuffer);
                    g_renameObject = nullptr;
                } else if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()) {
                    g_renameObject = nullptr;
                }
                if (isMember) ImGui::Unindent(14.0f);
            } else {
                if (isMember) ImGui::Indent(14.0f);
                char label[128];
                snprintf(label, sizeof(label), "%s", obj->GetName().c_str());
                bool rightClicked = false;
                bool clicked = ExplorerRowIcon(ROWICON_OBJECT, label, obj->isSelected, &rightClicked);

                if (g_explorerDragActive && ImGui::IsItemHovered()) {
                    g_explorerDropTarget = obj->parentModel;
                    g_explorerDropValid = true;
                }

                std::vector<ScatteredObject*> pressSelection;
                if (clicked && !ExplorerRenameActive() && !g_explorerMenuOpen) {
                    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
                    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                    if (shift && g_explorerAnchorIndex >= 0) {
                        int a = g_explorerAnchorIndex, b = entry.objRowIndex;
                        if (a > b) std::swap(a, b);
                        std::vector<ScatteredObject*> sel;
                        for (const auto& e : entries) {
                            if (!e.isHeader && e.obj && e.objRowIndex >= a && e.objRowIndex <= b) sel.push_back(e.obj);
                        }
                        SetSelection(sel, obj);
                        pressSelection = std::move(sel);
                    } else if (ctrl) {
                        bool wasSelected = obj->isSelected;
                        std::vector<ScatteredObject*> next;
                        for (auto* o : g_selection) if (o != obj) next.push_back(o);
                        if (!wasSelected) next.push_back(obj);
                        g_explorerAnchorIndex = entry.objRowIndex;
                        SetSelection(next, KeepPrimary(next, g_primarySelection));
                        pressSelection = std::move(next);
                    } else {
                        if (obj->isSelected && g_selection.size() > 1) {
                            pressSelection = g_selection;
                            SetSelection(pressSelection, g_primarySelection);
                        } else {
                            pressSelection = { obj };
                            SetSelection({ obj }, obj);
                        }
                        g_explorerAnchorIndex = entry.objRowIndex;

                        double now = GetTime();
                        if (g_lastExplorerClick == obj && now - g_lastExplorerClickTime < 0.35) {
                            g_renameObject = obj;
                            snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", obj->GetName().c_str());
                            g_nameBuffer[sizeof(g_nameBuffer) - 1] = '\0';
                            g_lastExplorerClick = nullptr;
                        } else {
                            g_lastExplorerClick = obj;
                            g_lastExplorerClickTime = now;
                        }
                    }
                    rowClickedThisFrame = true;
                } else if (rightClicked && !ExplorerRenameActive()) {
                    if (!obj->isSelected) {
                        SetSelection({ obj }, obj);
                        g_explorerAnchorIndex = entry.objRowIndex;
                    }
                    rightClickHandled = true;
                    OpenExplorerMenu(mouse, ExplorerMenuMode::Object, obj, -1);
                    rowClickedThisFrame = true;
                }

                if (!pressSelection.empty()) {
                    g_explorerDragPending = true;
                    g_explorerDragObjects = std::move(pressSelection);
                    g_explorerDragStart = mouse;
                }
                if (isMember) ImGui::Unindent(14.0f);
            }
            ImGui::PopID();
        }

        // Clicking blank space in the SCENE section clears the selection.
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !ExplorerRenameActive() && !g_explorerMenuOpen && !rowClickedThisFrame && !ImGui::IsAnyItemHovered()) {
            SetSelection({}, nullptr);
            g_explorerAnchorIndex = -1;
            g_lastExplorerClick = nullptr;
        }

        // Resolve drop target: hovering blank SCENE space (not over a row)
        // means "back to the workspace root".
        if (g_explorerDragActive && !g_explorerDropValid) {
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
                g_explorerDropTarget = nullptr;
                g_explorerDropValid = true;
            }
        }

        // Drag state machine: press-and-move past a threshold starts the
        // drag; release over a valid target reparents the dragged objects.
        if (g_explorerDragPending || g_explorerDragActive) {
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                if (g_explorerDragActive && g_explorerDropValid) {
                    PerformExplorerDrop(g_explorerDragObjects, g_explorerDropTarget);
                }
                g_explorerDragPending = false;
                g_explorerDragActive = false;
                g_explorerDragObjects.clear();
                g_explorerDropTarget = nullptr;
                g_explorerDropValid = false;
            } else if (!g_explorerDragActive && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                if (ExplorerRenameActive() || g_explorerMenuOpen) {
                    g_explorerDragPending = false;
                    g_explorerDragObjects.clear();
                } else {
                    Vector2 delta = Vector2Subtract(mouse, g_explorerDragStart);
                    if (fabsf(delta.x) + fabsf(delta.y) > 6.0f) {
                        g_explorerDragActive = true;
                        g_explorerDragPending = false;
                        g_explorerDropValid = false;
                    }
                }
            }
        }
    }

    // --- Lighting: scene-wide settings. Time is built in; the "+" menu inserts Sun, Ambient, Sky and Fog. Selecting a row
    // opens its settings in the properties area; right-click an inserted item to remove it (it goes back to its default). ---
    {
        gfx::LightingSettings& L = gfx::Lighting();
        if (!g_selection.empty() || g_selectedWater || g_selectedTerrain) g_lightingSel = 0;   // selecting anything else leaves it
        ImGui::Spacing();
        ImGui::TextDisabled("LIGHTING");
        ImGui::SameLine();
        if (ImGui::SmallButton("+##insertlighting")) ImGui::OpenPopup("##InsertLighting");
        if (ImGui::BeginPopup("##InsertLighting")) {
            ImGui::TextDisabled("Insert");
            ImGui::Separator();
            bool any = false;
            if (!L.hasSun && ImGui::MenuItem("Sun")) { L.hasSun = true; g_lightingSel = 2; any = true; }
            if (!L.hasAmbient && ImGui::MenuItem("Ambient")) { L.hasAmbient = true; g_lightingSel = 3; any = true; }
            // A new Sky is the atmosphere (it can be switched to one colour in its settings).
            if (!L.hasSky && ImGui::MenuItem("Sky")) { L.hasSky = true; L.skyMode = 1; g_lightingSel = 4; any = true; }
            // Under an atmosphere fog lies near the ground, so the sky stays visible above it.
            if (!L.hasFog && ImGui::MenuItem("Fog")) { L.hasFog = true; L.fogDensity = std::max(L.fogDensity, 0.004f); if (L.hasSky && L.skyMode == 1) L.fogHeight = 120.0f; g_lightingSel = 5; any = true; }
            if (!L.hasWeather && ImGui::MenuItem("Weather")) { L.hasWeather = true; L.overcast = 0.6f; L.rain = 0.5f; L.wetGround = 0.0f; g_lightingSel = 6; any = true; }
            // Clouds live in the atmosphere sky, so inserting them brings it along.
            if (!L.hasClouds && ImGui::MenuItem("Clouds")) { L.hasClouds = true; L.hasSky = true; L.skyMode = 1; g_lightingSel = 7; any = true; }
            if (!L.hasPicture && ImGui::MenuItem("Picture")) { L.hasPicture = true; g_lightingSel = 8; any = true; }
            if (any) {
                SetSelection({}, nullptr);
                if (g_selectedWater) { g_selectedWater->isSelected = false; g_selectedWater = nullptr; }
                if (g_selectedTerrain) { g_selectedTerrain->isSelected = false; g_selectedTerrain = nullptr; }
                BasicTerrain::SetActive(nullptr);
                terrain::GetTerrainEditorState().selectedTerrainLegacy = nullptr;
            }
            if (L.hasSun && L.hasAmbient && L.hasSky && L.hasFog && L.hasWeather && L.hasClouds && L.hasPicture) ImGui::TextDisabled("Everything is inserted.");
            ImGui::EndPopup();
        }
        struct Row { int id; const char* name; bool present; bool removable; };
        const Row rows[] = { { 1, "Time", true, false }, { 2, "Sun", L.hasSun, true }, { 3, "Ambient", L.hasAmbient, true },
                             { 4, "Sky", L.hasSky, true }, { 7, "Clouds", L.hasClouds, true }, { 5, "Fog", L.hasFog, true },
                             { 6, "Weather", L.hasWeather, true }, { 8, "Picture", L.hasPicture, true } };
        static int s_removeId = 0;
        for (const Row& r : rows) {
            if (!r.present) continue;
            ImGui::PushID(100 + r.id);
            bool rightClicked = false;
            const bool clicked = ExplorerRowIcon(ROWICON_OBJECT, r.name, g_lightingSel == r.id, &rightClicked);
            if (clicked && !ExplorerRenameActive() && !g_explorerMenuOpen) {
                SetSelection({}, nullptr);
                if (g_selectedWater) { g_selectedWater->isSelected = false; g_selectedWater = nullptr; }
                if (g_selectedTerrain) { g_selectedTerrain->isSelected = false; g_selectedTerrain = nullptr; }
                BasicTerrain::SetActive(nullptr);
                terrain::GetTerrainEditorState().selectedTerrainLegacy = nullptr;
                g_lightingSel = r.id;
            }
            if (rightClicked) {
                rightClickHandled = true;
                if (r.removable) { s_removeId = r.id; ImGui::OpenPopup("##LightingRowMenu"); }
            }
            ImGui::PopID();
        }
        if (ImGui::BeginPopup("##LightingRowMenu")) {
            if (ImGui::MenuItem("Remove (back to default)")) {
                const gfx::LightingSettings def;
                switch (s_removeId) {
                    case 2: L.hasSun = false; L.sunFollowsTime = def.sunFollowsTime; L.sunAzimuth = def.sunAzimuth; L.sunElevation = def.sunElevation; L.sunIntensity = def.sunIntensity;
                            for (int i = 0; i < 3; i++) L.sunColor[i] = def.sunColor[i]; break;
                    case 3: L.hasAmbient = false; L.ambient = def.ambient; break;
                    case 4: L.hasSky = false; L.skyMode = def.skyMode; for (int i = 0; i < 3; i++) { L.skyColor[i] = def.skyColor[i]; L.airColor[i] = def.airColor[i]; L.hazeColor[i] = def.hazeColor[i]; L.groundColor[i] = def.groundColor[i]; }
                            L.airDensity = def.airDensity; L.haze = def.haze; L.ozone = def.ozone; L.sunSize = def.sunSize; L.moonSize = def.moonSize;
                            L.moonPhase = def.moonPhase; L.moonLight = def.moonLight; L.stars = def.stars;
                            L.hasClouds = false; break;     // no atmosphere, no clouds in it
                    case 5: L.hasFog = false; L.fogDensity = 0.0f; L.fogHeight = def.fogHeight; break;
                    case 6: L.hasWeather = false; L.overcast = 0.0f; L.rain = 0.0f; L.wetGround = 0.0f; break;
                    case 7: L.hasClouds = false; L.cloudCoverage = def.cloudCoverage; L.cloudDensity = def.cloudDensity; L.cloudBase = def.cloudBase;
                            L.cloudThickness = def.cloudThickness; L.cloudScale = def.cloudScale; L.windSpeed = def.windSpeed; L.windDirection = def.windDirection; break;
                    case 8: L.hasPicture = false; L.toneCurve = def.toneCurve; L.exposure = def.exposure; L.autoExposure = def.autoExposure; L.bloom = def.bloom;
                            L.vignette = def.vignette; L.contrast = def.contrast; L.saturation = def.saturation; L.temperature = def.temperature; L.filmGrain = def.filmGrain; break;
                    default: break;
                }
                if (g_lightingSel == s_removeId) g_lightingSel = 1;
            }
            ImGui::EndPopup();
        }
    }

    SyncVisualScriptEntries();
    if (ScriptRuntime* rt = GetActiveRuntime()) {
        auto& scripts = rt->StandaloneScripts();
        ImGui::Spacing();
        ImGui::TextDisabled("SCRIPTS");

        for (int i = 0; i < static_cast<int>(scripts.size()); ++i) {
            ImGui::PushID(i);
            if (g_scriptRenameIndex == i) {
                static int s_lastRenameScriptIdx = -1;
                bool justOpened = (s_lastRenameScriptIdx != i);
                s_lastRenameScriptIdx = i;
                if (ExplorerRenameField("##scriptrename", g_scriptNameBuffer, sizeof(g_scriptNameBuffer), justOpened)) {
                    auto& s = scripts[i];
                    std::string oldClass = scriptLauncher::MakeClassName(s.typeName.empty() ? s.name : s.typeName);
                    s.name = g_scriptNameBuffer;
                    if (s.typeName == oldClass)
                        s.typeName = scriptLauncher::MakeClassName(s.name);
                    vscript::RenameKey(oldClass, ScriptKey(s));
                    g_scriptRenameIndex = -1;
                } else if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()) {
                    g_scriptRenameIndex = -1;
                }
            } else {
                char label[128];
                snprintf(label, sizeof(label), "%s", scripts[i].name.c_str());
                bool rightClicked = false;
                const std::string key = ScriptKey(scripts[i]);
                bool clicked = ExplorerRowIcon(ROWICON_SCRIPT, label, g_scriptSel.count(key) > 0, &rightClicked);
                if (rightClicked && !g_scriptSel.count(key)) { g_scriptSel = { key }; g_scriptSelAnchor = i; }
                if (clicked && !g_explorerMenuOpen) {
                    const ImGuiIO& io = ImGui::GetIO();
                    if (io.KeyCtrl) {                       // Ctrl: add or remove this one
                        if (!g_scriptSel.erase(key)) g_scriptSel.insert(key);
                        g_scriptSelAnchor = i;
                    } else if (io.KeyShift && g_scriptSelAnchor >= 0) {   // Shift: everything from the anchor to here
                        g_scriptSel.clear();
                        const int lo = std::min(g_scriptSelAnchor, i), hi = std::max(g_scriptSelAnchor, i);
                        for (int k = lo; k <= hi && k < static_cast<int>(scripts.size()); ++k) g_scriptSel.insert(ScriptKey(scripts[k]));
                    } else {                                // plain click: just this one, and edit it
                        g_scriptSel = { key };
                        g_scriptSelAnchor = i;
                        if (i >= 0 && i < static_cast<int>(scripts.size())) {
                            std::string className;
                            scriptLauncher::EditStandaloneScript(scripts[i].name, scripts[i].typeName, &className);
                            if (!className.empty()) {
                                scripts[i].typeName = className;
                                scripts[i].runOnPlay = true;
                            }
                        }
                    }
                } else if (rightClicked) {
                    rightClickHandled = true;
                    OpenExplorerMenu(mouse, ExplorerMenuMode::Script, nullptr, i);
                }
            }
            ImGui::PopID();
        }
    }

    bool contentHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    ImGui::EndChild();

    if (g_sceneObjects && contentHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !rightClickHandled) {
        OpenExplorerMenu(mouse, ExplorerMenuMode::Empty, nullptr, -1);
    }

    ImGui::End();
}

} // namespace ui
