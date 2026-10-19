// ui_explorer.cpp: the Explorer panel and its right-click menu, split out of ui.cpp.
#include "ui_internal.hpp"
#include "../../../include/Engine/Frontend/PluginHost.hpp"
#include <map>
#include <set>
#include "../../../include/Engine/ExportManager.hpp"
#include "../../../include/Engine/Backend/VisualScript.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Backend/ScenePersistence.hpp"
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
#include <cctype>
#include <functional>
#include <fstream>
#include <unordered_set>
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

static bool ExplorerRenameActive() {
    return g_renameObject || g_renameModel || g_renameTerrain || g_renameLegacyTerrain || g_scriptRenameIndex >= 0;
}

// Small helper: draw+commit/cancel an inline rename InputText, focusing it
// the first frame it appears. Returns true once Enter/blur commits the value.
static bool ExplorerRenameField(const char* imguiId, char* buf, size_t bufSize, bool justOpened) {
    if (justOpened) ImGui::SetKeyboardFocusHere();
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
    bool committed = ImGui::InputText(imguiId, buf, bufSize, flags);
    bool cancel = ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    bool blurred = ImGui::IsItemDeactivatedAfterEdit() && !committed;
    if (committed || blurred) return true;
    if (cancel) { *buf = '\0'; return false; } // caller checks g_render* pointers, not the buffer, to detect cancel
    return false;
}

// ---------------------------------------------------------------------------
// Context menu: a list of {label, action} built when the menu opens.
// ---------------------------------------------------------------------------
namespace {

struct MenuItem {
    std::string label;
    std::function<void()> action;
    std::vector<MenuItem> sub;   // non-empty: this entry opens a submenu
    bool undo = true;            // false when the action does not change the scene, or records its own undo step
};
std::vector<MenuItem> s_menuItems;

// Group currently highlighted in the tree. Only needed for groups that hold no objects (nothing to select);
// every other group is highlighted from the selection itself.
ModelGroup* s_selGroup = nullptr;

// A selection made by clicking a group row must not unfold that group.
bool s_skipReveal = false;

bool GroupExists(const ModelGroup* g) {
    if (!g || !g_sceneModels) return false;
    for (const auto& m : *g_sceneModels) if (m.get() == g) return true;
    return false;
}

std::vector<ScatteredObject*> GroupObjects(const ModelGroup* g) {
    std::vector<ScatteredObject*> out;
    if (g) g->CollectObjects(out);
    return out;
}

bool AllSelected(const std::vector<ScatteredObject*>& objs) {
    if (objs.empty()) return false;
    for (auto* o : objs) if (!o || !o->isSelected) return false;
    return true;
}

// The outermost group whose objects are all selected and that contains `obj`, or nullptr.
ModelGroup* FullySelectedGroupOf(ScatteredObject* obj) {
    ModelGroup* best = nullptr;
    for (ModelGroup* g = obj ? obj->parentModel : nullptr; g; g = g->parent)
        if (AllSelected(GroupObjects(g))) best = g;
    return best;
}

void BeginRename(ModelGroup* g) {
    g_renameModel = g;
    SetNameBuffer(g->name);
    g_nameBuffer[sizeof(g_nameBuffer) - 1] = '\0';
}

void BeginRename(ScatteredObject* o) {
    g_renameObject = o;
    SetNameBuffer(o->GetName());
    g_nameBuffer[sizeof(g_nameBuffer) - 1] = '\0';
}

// --- Fold state --------------------------------------------------------------
// Everything is open unless its key is in s_closed. Group keys are the "/"-joined name path so the state
// survives a restart and a scene reload. Saved to <config dir>/explorer_folds.txt.
std::unordered_set<std::string> s_closed;
bool s_foldLoaded = false;

std::string FoldFilePath() {
    const std::string dir = platform::ConfigDir();
    return dir.empty() ? std::string() : dir + "/explorer_folds.txt";
}

void LoadFolds() {
    if (s_foldLoaded) return;
    s_foldLoaded = true;
    const std::string path = FoldFilePath();
    if (path.empty()) return;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) if (!line.empty()) s_closed.insert(line);
}

void SaveFolds() {
    const std::string path = FoldFilePath();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    for (const auto& k : s_closed) out << k << '\n';
}

// One path segment: the name with '\\', '/', '~', '\n' and '\r' escaped (so the separator and the one-key-per-line
// file stay unambiguous), plus "~n" for the nth later sibling that has the same name. Names without those
// characters, and the first of a set of same-named siblings, keep the key older versions saved.
std::string GroupKeySegment(const ModelGroup* g) {
    std::string seg;
    for (char c : g->name) {
        switch (c) {
            case '\\': seg += "\\\\"; break;
            case '/':  seg += "\\/"; break;
            case '~':  seg += "\\~"; break;
            case '\n': seg += "\\n"; break;
            case '\r': seg += "\\r"; break;
            default:   seg += c;
        }
    }
    int rank = 0;
    if (g->parent) {
        for (const ModelGroup* sib : g->parent->children) {
            if (sib == g) break;
            if (sib && sib->name == g->name) ++rank;
        }
    } else if (g_sceneModels) {
        for (const auto& m : *g_sceneModels) {
            if (m.get() == g) break;
            if (m && !m->parent && m->name == g->name) ++rank;
        }
    }
    if (rank > 0) seg += "~" + std::to_string(rank);
    return seg;
}

std::string GroupKey(const ModelGroup* g) {
    std::string key = GroupKeySegment(g);
    for (const ModelGroup* p = g->parent; p; p = p->parent) key = GroupKeySegment(p) + "/" + key;
    return key;
}

bool IsOpen(const std::string& key) { return s_closed.count(key) == 0; }

void SetOpen(const std::string& key, bool open) {
    if (open ? s_closed.erase(key) > 0 : s_closed.insert(key).second) SaveFolds();
}

void ExpandAll() {
    if (s_closed.empty()) return;
    s_closed.clear();
    SaveFolds();
}

void CollapseAll() {
    for (const char* k : { "#Objects", "#Water", "#Terrain", "#Lighting", "#Scripts" }) s_closed.insert(k);
    if (g_sceneModels) for (const auto& m : *g_sceneModels) if (m) s_closed.insert(GroupKey(m.get()));
    SaveFolds();
}

// --- Search ------------------------------------------------------------------
char s_filter[64] = "";

bool FilterActive() { return s_filter[0] != '\0'; }

bool NameMatches(const std::string& name) {
    if (!FilterActive()) return true;
    auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::string hay(name), needle(s_filter);
    std::transform(hay.begin(), hay.end(), hay.begin(), lower);
    std::transform(needle.begin(), needle.end(), needle.begin(), lower);
    return hay.find(needle) != std::string::npos;
}

bool GroupMatches(const ModelGroup* g) {
    if (!FilterActive()) return true;
    if (NameMatches(g->name)) return true;
    for (const auto* m : g->members) if (m && NameMatches(m->GetName())) return true;
    for (const auto* c : g->children) if (c && GroupMatches(c)) return true;
    return false;
}

// --- Reveal: a selection made elsewhere (3D view) opens the path to its row and scrolls to it ----------------
const void* s_scrollTarget = nullptr;

// Call just before drawing a row, with the cursor still at its start.
void ScrollToIfTarget(const void* id) {
    if (!id || id != s_scrollTarget) return;
    s_scrollTarget = nullptr;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (!ImGui::IsRectVisible(p, ImVec2(p.x + 1.0f, p.y + 20.0f))) ImGui::SetScrollHereY(0.5f);
}

// Opens every fold above `obj` and queues a scroll to it. Runs once per change of the primary selection.
void RevealObject(ScatteredObject* obj) {
    if (!obj) return;
    bool changed = false;
    auto open = [&](const std::string& key) { if (s_closed.erase(key) > 0) changed = true; };
    open("#Objects");
    for (ModelGroup* g = obj->parentModel; g; g = g->parent) open(GroupKey(g));
    if (changed) SaveFolds();
    s_scrollTarget = obj;
}

// --- Row drawing -------------------------------------------------------------
struct RowDesc {
    const void* id = nullptr;
    int idInt = 0;                       // used when id is null
    RowIconType icon = ROWICON_OBJECT;
    ImTextureID customIcon = 0;          // overrides `icon` (plugin group kinds)
    bool showIcon = true;
    const char* label = "";
    bool selected = false;
    int depth = 0;
    bool hasArrow = false;
    bool open = false;
    bool dimmed = false;
    bool header = false;                 // category row: muted caps look
    bool renaming = false;
    char* renameBuf = nullptr;
    size_t renameBufSize = 0;
    bool* hidden = nullptr;              // eye toggle
    bool* locked = nullptr;              // lock toggle
    bool hiddenInherited = false;        // shown dim when a parent group is hidden
    bool lockedInherited = false;
    ScatteredObject* obj = nullptr;      // set for tree rows that take part in drag and drop
    ModelGroup* group = nullptr;
    bool dropToRoot = false;             // dropping on this row moves things out of every group
};

struct RowResult {
    bool clicked = false;
    bool doubleClicked = false;
    bool rightClicked = false;
    bool arrowClicked = false;
    bool renameCommit = false;
    bool renameCancel = false;
};

// Drag and drop between explorer rows. The payload carries nothing: the dragged things live here.
std::vector<ScatteredObject*> s_dragObjects;
ModelGroup* s_dragGroup = nullptr;
const void* s_lastRenameId = nullptr;

enum class DropZone { Before, Into, After };

void DrawEye(ImDrawList* dl, ImVec2 c, bool off, ImU32 col) {
    if (!off) {
        dl->AddEllipse(c, ImVec2(6.0f, 3.5f), col, 0.0f, 0, 1.3f);
        dl->AddCircleFilled(c, 1.8f, col);
    } else {
        dl->AddLine(ImVec2(c.x - 6.0f, c.y + 0.5f), ImVec2(c.x + 6.0f, c.y + 0.5f), col, 1.3f);
        dl->AddLine(ImVec2(c.x - 4.0f, c.y + 1.0f), ImVec2(c.x - 5.0f, c.y + 3.5f), col, 1.1f);
        dl->AddLine(ImVec2(c.x, c.y + 1.5f), ImVec2(c.x, c.y + 4.0f), col, 1.1f);
        dl->AddLine(ImVec2(c.x + 4.0f, c.y + 1.0f), ImVec2(c.x + 5.0f, c.y + 3.5f), col, 1.1f);
    }
}

void DrawLock(ImDrawList* dl, ImVec2 c, bool on, ImU32 col) {
    dl->AddRectFilled(ImVec2(c.x - 4.0f, c.y - 1.0f), ImVec2(c.x + 4.0f, c.y + 5.0f), col, 1.0f);
    const float cx = on ? c.x : c.x + 3.0f;
    dl->PathArcTo(ImVec2(cx, c.y - 1.0f), 2.8f, 3.1415927f, 6.2831853f, 8);
    dl->PathStroke(col, 0, 1.3f);
    if (!on) dl->AddLine(ImVec2(c.x + 5.8f, c.y - 1.0f), ImVec2(c.x + 5.8f, c.y - 0.2f), col, 1.3f);
    else {
        dl->AddLine(ImVec2(c.x - 2.8f, c.y - 1.0f), ImVec2(c.x - 2.8f, c.y), col, 1.3f);
        dl->AddLine(ImVec2(c.x + 2.8f, c.y - 1.0f), ImVec2(c.x + 2.8f, c.y), col, 1.3f);
    }
}

// Drop handling for one row. Called right after the row's Selectable, while it is still the last item.
void HandleRowDrop(const RowDesc& d, ImVec2 rmin, ImVec2 rmax) {
    if (!ImGui::BeginDragDropTarget()) return;
    const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("FLY_EXPLORER",
        ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
    if (pl) {
        const float t = (ImGui::GetMousePos().y - rmin.y) / std::max(1.0f, rmax.y - rmin.y);
        DropZone zone = t < 0.25f ? DropZone::Before : (t > 0.75f ? DropZone::After : DropZone::Into);
        if (d.dropToRoot) zone = DropZone::Into;
        // Only groups can hold things. Dropping in the middle of an object row places the drag after it.
        if (zone == DropZone::Into && !d.group && !d.dropToRoot) zone = DropZone::After;

        // Where would the drag end up: the receiving group (nullptr = root) and the sibling it is placed next to.
        ModelGroup* newParent = nullptr;
        ScatteredObject* refObj = nullptr;
        ModelGroup* refGroup = nullptr;
        if (d.dropToRoot) {
            newParent = nullptr;
        } else if (zone == DropZone::Into) {
            newParent = d.group;
        } else if (d.group) {
            newParent = d.group->parent;
            refGroup = d.group;
        } else if (d.obj) {
            newParent = d.obj->parentModel;
            refObj = d.obj;
        }

        bool valid = true;
        if (s_dragGroup) {
            valid = !(newParent == s_dragGroup || (newParent && newParent->IsDescendantOf(s_dragGroup)));
            if (refGroup == s_dragGroup) valid = false;
        } else {
            if (s_dragObjects.empty()) valid = false;
            for (auto* o : s_dragObjects) if (o == refObj) valid = false;
        }

        if (valid) {
            const ImU32 col = theme::AccentU32();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (zone == DropZone::Into) {
                dl->AddRectFilled(rmin, rmax, theme::AccentU32(50));
                dl->AddRect(rmin, rmax, col, 2.0f, 0, 1.5f);
            } else {
                const float y = zone == DropZone::Before ? rmin.y - 2.0f : rmax.y + 2.0f;
                dl->AddLine(ImVec2(rmin.x, y), ImVec2(rmax.x, y), col, 2.0f);
            }
        }

        if (pl->IsDelivery() && valid) {
            const bool after = zone == DropZone::After;
            PushUndo();
            if (s_dragGroup) {
                // Dropping beside an object: the group joins that object's parent group.
                MoveGroupTo(s_dragGroup, newParent, refGroup, after);
            } else {
                MoveObjectsToGroup(s_dragObjects, newParent, refObj, after);
            }
            s_dragObjects.clear();
            s_dragGroup = nullptr;
        }
    }
    ImGui::EndDragDropTarget();
}

// One explorer row: fold arrow, icon, label (or an inline rename field), eye/lock toggles, drag source and
// drop target. Every row type goes through here so they all look and behave the same.
RowResult DrawRow(const RowDesc& d) {
    RowResult r;
    constexpr float kHeight = 20.0f, kIndent = 14.0f, kArrow = 14.0f;
    if (d.id) ImGui::PushID(d.id); else ImGui::PushID(d.idInt);

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemAllowOverlap();
    const bool selClicked = ImGui::Selectable("##row", d.selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, kHeight));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
    const ImVec2 after = ImGui::GetCursorScreenPos();

    const float x0 = p.x + 4.0f + static_cast<float>(d.depth) * kIndent;
    if (selClicked) {
        const float mx = ImGui::GetMousePos().x;
        if (d.hasArrow && mx >= x0 - 2.0f && mx <= x0 + kArrow) r.arrowClicked = true;
        else r.clicked = true;
    }
    r.doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !r.arrowClicked;
    r.rightClicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);

    // Drag source / drop target (tree rows only).
    if ((d.obj || d.group) && !d.renaming && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        s_dragObjects.clear();
        s_dragGroup = nullptr;
        if (d.group) {
            s_dragGroup = d.group;
            ImGui::Text("%s", d.group->name.c_str());
        } else {
            if (d.obj->isSelected && g_selection.size() > 1) s_dragObjects = g_selection;
            else s_dragObjects = { d.obj };
            if (s_dragObjects.size() > 1) ImGui::Text("%d objects", static_cast<int>(s_dragObjects.size()));
            else ImGui::Text("%s", d.obj->GetName().c_str());
        }
        const int dummy = 1;
        ImGui::SetDragDropPayload("FLY_EXPLORER", &dummy, sizeof(dummy));
        ImGui::EndDragDropSource();
    }
    if (d.obj || d.group || d.dropToRoot) HandleRowDrop(d, rmin, rmax);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 textVec = d.header ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImVec4 tv = textVec;
    if (d.dimmed) tv.w *= 0.45f;
    const ImU32 textCol = ImGui::GetColorU32(tv);
    const float midY = (rmin.y + rmax.y) * 0.5f;

    float x = x0;
    if (d.hasArrow) {
        const ImU32 ac = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        if (d.open)
            dl->AddTriangleFilled(ImVec2(x + 2.0f, midY - 2.0f), ImVec2(x + 10.0f, midY - 2.0f), ImVec2(x + 6.0f, midY + 3.0f), ac);
        else
            dl->AddTriangleFilled(ImVec2(x + 3.0f, midY - 4.0f), ImVec2(x + 3.0f, midY + 4.0f), ImVec2(x + 8.0f, midY), ac);
    }
    x += kArrow + 2.0f;
    if (d.showIcon) {
        ImTextureID tex = d.customIcon ? d.customIcon : RowTex(d.icon);
        if (tex) dl->AddImage(tex, ImVec2(x, midY - 7.0f), ImVec2(x + 14.0f, midY + 7.0f), ImVec2(0, 0), ImVec2(1, 1),
                              d.dimmed ? IM_COL32(255, 255, 255, 110) : IM_COL32_WHITE);
        x += 20.0f;
    }

    if (d.renaming) {
        ImGui::SetCursorScreenPos(ImVec2(x, p.y));
        ImGui::SetNextItemWidth(std::max(40.0f, rmax.x - x - 4.0f));
        const bool justOpened = (s_lastRenameId != (d.id ? d.id : reinterpret_cast<const void*>(static_cast<intptr_t>(d.idInt) + 1)));
        s_lastRenameId = d.id ? d.id : reinterpret_cast<const void*>(static_cast<intptr_t>(d.idInt) + 1);
        r.renameCommit = ExplorerRenameField("##rename", d.renameBuf, d.renameBufSize, justOpened);
        r.renameCancel = !r.renameCommit && ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetCursorScreenPos(after);
    } else {
        const ImVec2 ts = ImGui::CalcTextSize(d.label);
        dl->AddText(ImVec2(x, midY - ts.y * 0.5f), textCol, d.label);
    }

    // Right edge: eye + lock on tree rows.
    const bool rowHot = ImGui::IsMouseHoveringRect(rmin, rmax) && !d.renaming;
    float rx = rmax.x - 14.0f;
    auto toggle = [&](const char* btnId, bool* flag, bool inherited, bool isEye) {
        const bool shown = rowHot || *flag || inherited;
        if (!shown) { rx -= 20.0f; return; }
        ImGui::SetCursorScreenPos(ImVec2(rx - 9.0f, rmin.y + 1.0f));
        if (ImGui::InvisibleButton(btnId, ImVec2(18.0f, kHeight - 2.0f))) { PushUndo(); *flag = !*flag; }
        const bool on = *flag;
        ImVec4 cv = ImGui::GetStyleColorVec4(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        if (!on && !inherited) cv.w *= 0.7f;
        const ImU32 col = ImGui::GetColorU32(isEye ? (on ? ImVec4(0.95f, 0.45f, 0.40f, 1.0f) : cv)
                                                   : (on ? ImVec4(0.95f, 0.75f, 0.35f, 1.0f) : cv));
        if (isEye) DrawEye(dl, ImVec2(rx, midY), on || inherited, col);
        else DrawLock(dl, ImVec2(rx, midY - 1.5f), on || inherited, col);
        ImGui::SetCursorScreenPos(after);
        rx -= 20.0f;
    };
    if (d.locked) toggle("##lock", d.locked, d.lockedInherited, false);
    if (d.hidden) toggle("##hide", d.hidden, d.hiddenInherited, true);

    ImGui::SetCursorScreenPos(after);
    ImGui::PopID();
    return r;
}

// Category row (Objects, Water, ...): an arrow, a caps label. Returns whether it is open; `rightClicked` reports a right-click on the header.
bool DrawCategory(const char* label, const char* key, bool* rightClicked = nullptr, bool dropToRoot = false) {
    RowDesc d;
    d.id = key;
    d.showIcon = false;
    d.label = label;
    d.header = true;
    d.hasArrow = true;
    d.open = IsOpen(key);
    d.dropToRoot = dropToRoot;
    RowResult r = DrawRow(d);
    if (rightClicked) *rightClicked = r.rightClicked;
    if (r.clicked || r.arrowClicked) {
        SetOpen(key, !d.open);
        return !d.open;
    }
    return d.open;
}

// --- Context menu builders ---------------------------------------------------
void AddMenu(const char* label, std::function<void()> fn, bool undo = true) { s_menuItems.push_back({ label, std::move(fn), {}, undo }); }

template <class F> void ForEachSelected(F&& f) {
    for (auto* o : std::vector<ScatteredObject*>(g_selection)) if (o) f(o);
}

MenuItem MakeInsertMenu(ScatteredObject* scriptTarget);

void BuildObjectMenu(ScatteredObject* obj) {
    const bool singleSel = obj && g_selection.size() == 1 &&
        std::find(g_selection.begin(), g_selection.end(), obj) != g_selection.end();
    // Insert > Script attaches another script to the object right-clicked on.
    s_menuItems.push_back(MakeInsertMenu(singleSel ? obj : nullptr));
    if (g_selection.size() >= 2) AddMenu("Group", [] { GroupSelectedObjects(); });
    AddMenu("Duplicate", [] { RequestDuplicate(); }, false);
    if (obj && obj->parentModel) AddMenu("Ungroup", [obj] { UngroupModelContaining(obj); });
    const bool hide = !obj->hidden;
    AddMenu(hide ? "Hide" : "Show", [hide] { ForEachSelected([hide](ScatteredObject* o) { o->hidden = hide; }); });
    const bool lock = !obj->locked;
    AddMenu(lock ? "Lock" : "Unlock", [lock] { ForEachSelected([lock](ScatteredObject* o) { o->locked = lock; }); });
}

// Menu for one script row under an object.
void BuildObjectScriptMenu(ScatteredObject* obj, const std::string& name) {
    AddMenu("Edit Script", [obj, name] { scriptLauncher::EditObjectScriptNamed(obj, name); }, false);
    AddMenu("Remove Script", [obj, name] {
        if (ScriptRuntime* rt = GetActiveRuntime()) rt->StopObjectScript(obj);
        obj->RemoveScript(name);
        if (obj->script.empty()) obj->runOnPlay = false;
    });
    AddMenu("Choose Editor...", [name] { scriptLauncher::ChooseEditorForObjectScriptNamed(name); }, false);
    AddMenu("Open in Visual Script", [name] {
        vscript::OpenByName(scriptLauncher::MakeClassName(name));
        SelectBottomTab(2);
    }, false);
    MenuItem insert = MakeInsertMenu(obj);
    s_menuItems.insert(s_menuItems.begin(), std::move(insert));
}

// Writes the group (nested groups included) to assets/Prefabs/<name>.flyprefab; a name already taken gets _2, _3...
static void SaveGroupAsPrefab(const ModelGroup* model) {
    const std::string& projectDir = ::project::GetCurrentProject().path;
    if (projectDir.empty()) { LogAlways("Open a project before saving a prefab"); return; }
    std::string base;
    for (unsigned char c : model->name) base += (std::isalnum(c) || c == '-' || c == '_') ? (char)c : '_';
    if (base.empty()) base = "Prefab";
    const std::filesystem::path dir = std::filesystem::path(projectDir) / "assets" / "Prefabs";
    std::filesystem::path file = dir / (base + ".flyprefab");
    std::error_code ec;
    for (int n = 2; std::filesystem::exists(file, ec); ++n) file = dir / (base + "_" + std::to_string(n) + ".flyprefab");
    if (SavePrefabToFile(*model, file.generic_string(), projectDir))
        LogAlways("Saved prefab assets/Prefabs/%s", file.filename().string().c_str());
    else
        LogAlways("Could not save the prefab (is the group empty?)");
}

void BuildModelMenu(ModelGroup* model) {
    AddMenu("Rename", [model] { BeginRename(model); }, false);
    AddMenu("Duplicate", [] { RequestDuplicate(); }, false);
    // A plugin-owned group: the plugin's items, and no way to take it apart or unlock it from here.
    if (const pluginhost::GroupKind* kind = pluginhost::FindGroupKind(model->pluginTag)) {
        for (const auto& item : kind->items) AddMenu(item.label.c_str(), [run = item.run, model] { run(model); }, false);
        AddMenu(model->hidden ? "Show" : "Hide", [model] { model->hidden = !model->hidden; });
        AddMenu("Delete", [model] {
            std::vector<ScatteredObject*> targets = DissolveModelTree(model);
            if (s_selGroup == model) s_selGroup = nullptr;
            RequestDelete(targets, true);   // the menu's undo step (taken before the groups dissolved) covers it
            SetSelection({}, nullptr);
        });
        return;
    }
    AddMenu("New Group Inside", [model] {
        ModelGroup* g = CreateEmptyGroup(model);
        if (!g) return;
        SetOpen(GroupKey(model), true);
        BeginRename(g);
    });
    AddMenu("Save as Prefab", [model] { SaveGroupAsPrefab(model); }, false);
    AddMenu("Ungroup", [model] { UngroupModel(model); if (s_selGroup == model) s_selGroup = nullptr; });
    AddMenu(model->hidden ? "Show" : "Hide", [model] { model->hidden = !model->hidden; });
    AddMenu(model->locked ? "Unlock" : "Lock", [model] { model->locked = !model->locked; });
    AddMenu("Delete", [model] {
        std::vector<ScatteredObject*> targets = DissolveModelTree(model);
        if (s_selGroup == model) s_selGroup = nullptr;
        RequestDelete(targets, true);   // the menu's undo step (taken before the groups dissolved) covers it
        SetSelection({}, nullptr);
    });
}

void InsertScript() {
    ScriptRuntime* rt = GetActiveRuntime();
    if (!rt) return;
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

// "Insert" submenu: everything that can be added to the scene. Shapes and water/terrain reuse the viewport spawn
// actions, placed in front of the camera.
MenuItem MakeInsertMenu(ScatteredObject* scriptTarget) {
    MenuItem insert{ "Insert", nullptr, {} };
    auto spawn = [&](const char* label, ui::MenuAction a) {
        insert.sub.push_back({ label, [a] { ui::RequestMenuAction(a, true); }, {}, false });
    };
    spawn("Cube", ui::MenuAction::SpawnCube);
    spawn("Sphere", ui::MenuAction::SpawnSphere);
    spawn("Cylinder", ui::MenuAction::SpawnCylinder);
    spawn("Wedge", ui::MenuAction::SpawnWedge);
    insert.sub.push_back({ "Group", [] { if (ModelGroup* g = CreateEmptyGroup(nullptr)) BeginRename(g); }, {} });
    spawn("Water Body", ui::MenuAction::SpawnWater);
    spawn("Basic Terrain", ui::MenuAction::SpawnBasicTerrain);
    spawn("Import Terrain", ui::MenuAction::ImportTerrain);
    // With an object to attach to, "Script" adds to that object; otherwise it makes a standalone script.
    if (scriptTarget) insert.sub.push_back({ "Script", [scriptTarget] { scriptLauncher::AddObjectScript(scriptTarget); }, {} });
    else insert.sub.push_back({ "Script", [] { InsertScript(); }, {} });
    return insert;
}

void BuildEmptyMenu() {
    s_menuItems.push_back(MakeInsertMenu(nullptr));
    AddMenu("Expand All", [] { ExpandAll(); }, false);
    AddMenu("Collapse All", [] { CollapseAll(); }, false);
}

void BuildScriptMenu(int scriptIndex) {
    ScriptRuntime* rt = GetActiveRuntime();
    if (!rt) return;
    auto& scripts = rt->StandaloneScripts();
    if (scriptIndex < 0 || scriptIndex >= static_cast<int>(scripts.size())) return;
    if (ScriptMenuIsMulti()) {
        const std::set<std::string> keys = g_scriptSel;
        AddMenu("Open in Visual Script", [keys] {
            for (const auto& k : keys) vscript::OpenByName(k);
            SelectBottomTab(2);
        }, false);
        AddMenu("Delete Selected", [keys] {
            ScriptRuntime* r = GetActiveRuntime();
            if (!r) return;
            auto& list = r->StandaloneScripts();
            for (int k = static_cast<int>(list.size()) - 1; k >= 0; --k)
                if (keys.count(ScriptKey(list[k]))) DeleteScriptEverywhere(ScriptKey(list[k]), k);
            g_scriptSel.clear();
        });
        return;
    }
    AddMenu("Rename", [scriptIndex] {
        ScriptRuntime* r = GetActiveRuntime();
        if (!r || scriptIndex >= static_cast<int>(r->StandaloneScripts().size())) return;
        g_scriptRenameIndex = scriptIndex;
        snprintf(g_scriptNameBuffer, sizeof(g_scriptNameBuffer), "%s", r->StandaloneScripts()[scriptIndex].name.c_str());
        g_scriptNameBuffer[sizeof(g_scriptNameBuffer) - 1] = '\0';
        g_cursorPos = static_cast<int>(strlen(g_scriptNameBuffer));
        g_selStart = -1;
    }, false);
    AddMenu("Delete", [scriptIndex] {
        ScriptRuntime* r = GetActiveRuntime();
        if (!r || scriptIndex >= static_cast<int>(r->StandaloneScripts().size())) return;
        DeleteScriptEverywhere(ScriptKey(r->StandaloneScripts()[scriptIndex]), scriptIndex);
    });
    AddMenu("Choose Editor...", [scriptIndex] {
        ScriptRuntime* r = GetActiveRuntime();
        if (!r || scriptIndex >= static_cast<int>(r->StandaloneScripts().size())) return;
        auto& s = r->StandaloneScripts()[scriptIndex];
        scriptLauncher::ChooseEditorForStandaloneScript(s.name, s.typeName);
    }, false);
    AddMenu("Open in Visual Script", [scriptIndex] {
        ScriptRuntime* r = GetActiveRuntime();
        if (!r || scriptIndex >= static_cast<int>(r->StandaloneScripts().size())) return;
        vscript::OpenByName(ScriptKey(r->StandaloneScripts()[scriptIndex]));
        SelectBottomTab(2);
    }, false);
}

void OpenExplorerMenu(Vector2 pos, ExplorerMenuMode mode, ScatteredObject* obj, int scriptIndex, ModelGroup* model = nullptr,
                      const std::string& objScript = std::string()) {
    CloseContextMenu();
    g_explorerMenuOpen = true;
    g_explorerMenuPos = pos;
    g_explorerMenuMode = mode;
    g_explorerMenuObject = obj;
    g_explorerMenuScript = scriptIndex;
    g_explorerMenuOpenTime = GetTime();
    s_menuItems.clear();
    switch (mode) {
        case ExplorerMenuMode::Object:
            if (obj && !objScript.empty()) BuildObjectScriptMenu(obj, objScript);
            else if (obj) BuildObjectMenu(obj);
            break;
        case ExplorerMenuMode::Model:  if (model) BuildModelMenu(model); break;
        case ExplorerMenuMode::Empty:  BuildEmptyMenu(); break;
        case ExplorerMenuMode::Script: BuildScriptMenu(scriptIndex); break;
    }
}

} // namespace

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
        std::function<void()> chosen;
        bool chosenUndo = false;
        for (const auto& item : s_menuItems) {
            if (!item.sub.empty()) {
                if (ImGui::BeginMenu(item.label.c_str())) {
                    for (const auto& sub : item.sub)
                        if (ImGui::MenuItem(sub.label.c_str())) { chosen = sub.action; chosenUndo = sub.undo; }
                    ImGui::EndMenu();
                }
            } else if (ImGui::MenuItem(item.label.c_str())) {
                chosen = item.action;
                chosenUndo = item.undo;
            }
            if (chosen) break;
        }
        if (chosen) {
            // The action may open another menu and rebuild the list, so it runs after the loop.
            ImGui::CloseCurrentPopup();
            // Scene-changing items record an undo step first.
            if (chosenUndo) PushUndo();
            chosen();
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
// Explorer window (Dear ImGui). Called once per frame from DrawImGuiFrame().
// Layout: search box, then collapsible categories (Objects, Water, Terrain, Lighting, Scripts). Objects holds
// the nested group tree; every row is drawn by DrawRow.
// ---------------------------------------------------------------------------
namespace {

// Per-frame state shared by the tree drawing helpers.
struct TreeCtx {
    bool rightClickHandled = false;
    bool rowClickedThisFrame = false;
    Vector2 mouse = { 0.0f, 0.0f };
    std::vector<ScatteredObject*> order;      // object rows in the order drawn this frame
};
TreeCtx s_ctx;
std::vector<ScatteredObject*> s_orderPrev;    // last frame's order, for shift-range selection

int IndexInOrder(const ScatteredObject* o) {
    for (size_t i = 0; i < s_orderPrev.size(); ++i) if (s_orderPrev[i] == o) return static_cast<int>(i);
    return -1;
}

void SelectRange(int a, int b, ScatteredObject* primary) {
    if (a > b) std::swap(a, b);
    std::vector<ScatteredObject*> sel;
    for (int i = std::max(a, 0); i <= b && i < static_cast<int>(s_orderPrev.size()); ++i) sel.push_back(s_orderPrev[i]);
    SetSelection(sel, KeepPrimary(sel, primary));
}

void HandleObjectRow(ScatteredObject* obj, const RowResult& r) {
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    const int myIdx = IndexInOrder(obj);
    if (r.clicked && !ExplorerRenameActive() && !g_explorerMenuOpen) {
        s_selGroup = nullptr;
        if (shift && g_explorerAnchorIndex >= 0) {
            SelectRange(g_explorerAnchorIndex, myIdx, obj);
        } else if (ctrl) {
            std::vector<ScatteredObject*> next;
            for (auto* o : g_selection) if (o != obj) next.push_back(o);
            if (!obj->isSelected) next.push_back(obj);
            g_explorerAnchorIndex = myIdx;
            SetSelection(next, KeepPrimary(next, g_primarySelection));
        } else {
            // Pressing a row of an existing multi-selection keeps it, so the whole selection can be dragged.
            if (!(obj->isSelected && g_selection.size() > 1)) SetSelection({ obj }, obj);
            g_explorerAnchorIndex = myIdx;
        }
        s_ctx.rowClickedThisFrame = true;
    }
    if (r.doubleClicked && !ExplorerRenameActive() && !g_explorerMenuOpen) {
        SetSelection({ obj }, obj);
        BeginRename(obj);
    } else if (r.rightClicked && !ExplorerRenameActive()) {
        if (!obj->isSelected) {
            SetSelection({ obj }, obj);
            g_explorerAnchorIndex = myIdx;
        }
        s_ctx.rightClickHandled = true;
        OpenExplorerMenu(s_ctx.mouse, ExplorerMenuMode::Object, obj, -1);
        s_ctx.rowClickedThisFrame = true;
    }
}

void HandleGroupRow(ModelGroup* g, const std::vector<ScatteredObject*>& all, bool selected, const RowResult& r) {
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    ScatteredObject* first = all.empty() ? nullptr : all.front();
    if (r.clicked && !ExplorerRenameActive() && !g_explorerMenuOpen) {
        s_selGroup = g;
        s_skipReveal = true;
        if (ctrl) {
            std::vector<ScatteredObject*> next = g_selection;
            if (selected) {
                next.erase(std::remove_if(next.begin(), next.end(),
                    [&](ScatteredObject* o) { return std::find(all.begin(), all.end(), o) != all.end(); }), next.end());
            } else {
                for (auto* m : all) if (std::find(next.begin(), next.end(), m) == next.end()) next.push_back(m);
            }
            g_explorerAnchorIndex = IndexInOrder(first);
            SetSelection(next, KeepPrimary(next, g_primarySelection));
        } else if (shift && g_explorerAnchorIndex >= 0 && first) {
            // Below the anchor the range runs to the group's last object, so the whole group is included.
            const int fi = IndexInOrder(first);
            const int li = IndexInOrder(all.back());
            SelectRange(g_explorerAnchorIndex, g_explorerAnchorIndex <= fi ? std::max(fi, li) : fi, first);
        } else {
            SetSelection(all, first);
            g_explorerAnchorIndex = IndexInOrder(first);
        }
        s_ctx.rowClickedThisFrame = true;
    }
    if (r.doubleClicked && !ExplorerRenameActive() && !g_explorerMenuOpen) {
        BeginRename(g);
    } else if (r.rightClicked && !ExplorerRenameActive()) {
        if (!selected) {
            SetSelection(all, first);
            s_selGroup = g;
            s_skipReveal = true;
        }
        s_ctx.rightClickHandled = true;
        OpenExplorerMenu(s_ctx.mouse, ExplorerMenuMode::Model, first, -1, g);
        s_ctx.rowClickedThisFrame = true;
    }
}

// Objects whose script child is folded away. Runtime only; everything starts open.
std::unordered_set<const ScatteredObject*> s_scriptClosed;

// The script attached to an object, shown as a child row under it.
void DrawObjectScriptRow(ScatteredObject* obj, const std::string& name, int idx, int depth) {
    ImGui::PushID(obj);
    RowDesc d;
    d.idInt = 1 + idx;
    d.icon = ROWICON_SCRIPT;
    d.label = name.c_str();
    d.selected = false;
    d.depth = depth;
    d.dimmed = obj->IsHiddenEffective();
    RowResult r = DrawRow(d);
    ImGui::PopID();
    if (r.clicked && !ExplorerRenameActive() && !g_explorerMenuOpen) {
        s_selGroup = nullptr;
        SetSelection({ obj }, obj);
        s_ctx.rowClickedThisFrame = true;
    }
    if (r.doubleClicked && !ExplorerRenameActive() && !g_explorerMenuOpen) {
        SetSelection({ obj }, obj);
        scriptLauncher::EditObjectScriptNamed(obj, name);
    } else if (r.rightClicked && !ExplorerRenameActive()) {
        SetSelection({ obj }, obj);
        s_ctx.rightClickHandled = true;
        s_ctx.rowClickedThisFrame = true;
        OpenExplorerMenu(s_ctx.mouse, ExplorerMenuMode::Object, obj, -1, nullptr, name);
    }
}

void DrawObjectRow(ScatteredObject* obj, int depth) {
    s_ctx.order.push_back(obj);
    const std::string label = obj->GetName();
    const bool hasScript = !obj->script.empty();
    const bool scriptOpen = hasScript && s_scriptClosed.count(obj) == 0;
    RowDesc d;
    d.id = obj;
    d.icon = ROWICON_OBJECT;
    d.label = label.c_str();
    d.selected = obj->isSelected;
    d.depth = depth;
    d.hasArrow = hasScript;
    d.open = scriptOpen;
    d.hidden = &obj->hidden;
    d.locked = &obj->locked;
    d.hiddenInherited = !obj->hidden && obj->parentModel && obj->parentModel->IsHiddenEffective();
    d.lockedInherited = !obj->locked && obj->parentModel && obj->parentModel->IsLockedEffective();
    d.dimmed = obj->IsHiddenEffective();
    d.obj = obj;
    d.renaming = (g_renameObject == obj);
    d.renameBuf = g_nameBuffer;
    d.renameBufSize = sizeof(g_nameBuffer);
    ScrollToIfTarget(obj);
    RowResult r = DrawRow(d);
    if (d.renaming) {
        if (r.renameCommit) { if (obj->GetName() != g_nameBuffer) PushUndo(); obj->SetName(g_nameBuffer); g_renameObject = nullptr; }
        else if (r.renameCancel) g_renameObject = nullptr;
    } else {
        if (r.arrowClicked) {
            if (scriptOpen) s_scriptClosed.insert(obj); else s_scriptClosed.erase(obj);
        }
        HandleObjectRow(obj, r);
    }
    if (hasScript && (scriptOpen || FilterActive())) {
        const std::vector<std::string> names = obj->AllScripts();
        for (size_t i = 0; i < names.size(); ++i) DrawObjectScriptRow(obj, names[i], static_cast<int>(i), depth + 1);
    }
}

void DrawGroupNode(ModelGroup* g, int depth, bool ancestorMatched) {
    if (!g) return;
    const bool selfMatch = ancestorMatched || NameMatches(g->name);
    if (FilterActive() && !selfMatch && !GroupMatches(g)) return;

    const std::vector<ScatteredObject*> all = GroupObjects(g);
    const bool selected = !all.empty() ? AllSelected(all) : (s_selGroup == g && g_selection.empty());
    const std::string key = GroupKey(g);
    const bool hasContent = !g->children.empty() || !g->members.empty();
    const bool open = FilterActive() ? true : IsOpen(key);

    char label[160];
    snprintf(label, sizeof(label), "%s (%d)", g->name.c_str(), static_cast<int>(all.size()));
    RowDesc d;
    d.id = g;
    d.icon = ROWICON_GROUP;
    if (const pluginhost::GroupKind* kind = pluginhost::FindGroupKind(g->pluginTag)) d.customIcon = PluginIconTex(kind->iconPath);
    d.label = label;
    d.selected = selected;
    d.depth = depth;
    d.hasArrow = hasContent;
    d.open = open;
    d.hidden = &g->hidden;
    d.locked = &g->locked;
    d.hiddenInherited = !g->hidden && g->parent && g->parent->IsHiddenEffective();
    d.lockedInherited = !g->locked && g->parent && g->parent->IsLockedEffective();
    d.dimmed = g->IsHiddenEffective();
    d.group = g;
    d.renaming = (g_renameModel == g);
    d.renameBuf = g_nameBuffer;
    d.renameBufSize = sizeof(g_nameBuffer);
    ScrollToIfTarget(g);
    RowResult r = DrawRow(d);

    if (d.renaming) {
        if (r.renameCommit) { if (g->name != g_nameBuffer) PushUndo(); g->name = g_nameBuffer; g_renameModel = nullptr; }
        else if (r.renameCancel) g_renameModel = nullptr;
    } else {
        if (r.arrowClicked && !FilterActive()) SetOpen(key, !open);
        HandleGroupRow(g, all, selected, r);
    }

    if (!open) {
        // Folded rows still count for Shift-range selection.
        for (auto* o : all) s_ctx.order.push_back(o);
        return;
    }
    // Copy: an action in this frame (drop, delete) must not invalidate what we iterate.
    const std::vector<ModelGroup*> kids = g->children;
    const std::vector<ScatteredObject*> mem = g->members;
    for (auto* c : kids) DrawGroupNode(c, depth + 1, selfMatch && NameMatches(g->name) ? true : ancestorMatched);
    for (auto* m : mem) {
        if (!m) continue;
        if (FilterActive() && !selfMatch && !NameMatches(m->GetName())) continue;
        DrawObjectRow(m, depth + 1);
    }
}

} // namespace

void SuppressExplorerReveal() { s_skipReveal = true; }

void DrawImGuiExplorer() {
    Rectangle panelRec = GetExplorerPanelBounds();
    LoadFolds();

    ImGui::SetNextWindowPos(ImVec2(panelRec.x, panelRec.y));
    ImGui::SetNextWindowSize(ImVec2(panelRec.width, panelRec.height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##Explorer", nullptr, flags);
    ImGui::TextColored(theme::AccentVec(), "EXPLORER");
    ImGui::SameLine();
    {
        // Right-aligned fold buttons.
        const float bw = ImGui::CalcTextSize("Collapse").x + ImGui::CalcTextSize("Expand").x + 4.0f * ImGui::GetStyle().FramePadding.x + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - bw));
        if (ImGui::SmallButton("Expand")) ExpandAll();
        ImGui::SameLine();
        if (ImGui::SmallButton("Collapse")) CollapseAll();
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##explorerfilter", "Search...", s_filter, sizeof(s_filter));
    ImGui::Separator();

    s_ctx = TreeCtx{};
    ImVec2 mouseImgui = ImGui::GetMousePos();
    s_ctx.mouse = { mouseImgui.x, mouseImgui.y };
    if (!ExplorerRenameActive()) s_lastRenameId = nullptr;
    if (s_selGroup && !GroupExists(s_selGroup)) s_selGroup = nullptr;
    if (g_renameModel && !GroupExists(g_renameModel)) g_renameModel = nullptr;

    // The selection changed (a click in the 3D view, Ctrl+A, undo...): show it here.
    {
        static const void* s_seenPrimary = nullptr;
        static const void* s_seenWater = nullptr;
        static const void* s_seenTerrain = nullptr;
        const void* terrainNow = g_selectedTerrain ? static_cast<const void*>(g_selectedTerrain)
            : static_cast<const void*>(terrain::GetTerrainEditorState().selectedTerrainLegacy);
        if (g_primarySelection != s_seenPrimary) {
            s_seenPrimary = g_primarySelection;
            if (g_primarySelection && !s_skipReveal) RevealObject(g_primarySelection);
        } else if (g_selectedWater != s_seenWater && g_selectedWater) {
            s_seenWater = g_selectedWater;
            SetOpen("#Water", true);
            s_scrollTarget = g_selectedWater;
        } else if (terrainNow != s_seenTerrain && terrainNow) {
            s_seenTerrain = terrainNow;
            SetOpen("#Terrain", true);
            s_scrollTarget = terrainNow;
        }
        s_skipReveal = false;
        s_seenWater = g_selectedWater;
        s_seenTerrain = terrainNow;
    }

    ImGui::BeginChild("##ExplorerContent", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NoSavedSettings);

    if (g_sceneObjects != nullptr) {
        // F2 to rename the current selection, matching the old raylib shortcut.
        if (!ExplorerRenameActive() && !ImGui::GetIO().WantTextInput && IsKeyPressed(KEY_F2)) {
            if (g_selectedTerrain) {
                g_renameTerrain = g_selectedTerrain;
                SetNameBuffer(g_selectedTerrain->GetName());
            } else if (terrain::GetTerrainEditorState().selectedTerrainLegacy) {
                terrain::Terrain* legacy = terrain::GetTerrainEditorState().selectedTerrainLegacy;
                g_renameLegacyTerrain = legacy;
                SetNameBuffer(legacy->GetName());
            } else if (s_selGroup && g_selection.empty()) {
                BeginRename(s_selGroup);
            } else if (g_primarySelection) {
                if (ModelGroup* pm = FullySelectedGroupOf(g_primarySelection)) BeginRename(pm);
                else BeginRename(g_primarySelection);
            }
            g_nameBuffer[sizeof(g_nameBuffer) - 1] = '\0';
        }

        // --- Objects: the nested group tree, then loose objects ---
        bool objectsOpen = DrawCategory("OBJECTS", "#Objects", nullptr, true);
        if (objectsOpen || FilterActive()) {
            std::vector<ModelGroup*> roots;
            if (g_sceneModels) for (auto& m : *g_sceneModels) if (m && !m->parent) roots.push_back(m.get());
            for (auto* g : roots) DrawGroupNode(g, 1, false);
            const std::vector<ScatteredObject*> loose(g_sceneObjects->begin(), g_sceneObjects->end());
            for (auto* o : loose) {
                if (!o || o->parentModel) continue;
                if (FilterActive() && !NameMatches(o->GetName())) continue;
                DrawObjectRow(o, 1);
            }
        }

        // --- Water ---
        const auto& waters = WaterBody::GetInstances();
        bool anyWater = false;
        for (auto* w : waters) if (w) { anyWater = true; break; }
        if (anyWater && DrawCategory("WATER", "#Water")) {
            for (auto* water : waters) {
                if (!water) continue;
                if (FilterActive() && !NameMatches(water->GetName())) continue;
                char label[128];
                snprintf(label, sizeof(label), "%s", water->GetName().c_str());
                RowDesc d;
                d.id = water;
                d.icon = ROWICON_WATER;
                d.label = label;
                d.selected = water->isSelected;
                d.depth = 1;
                ScrollToIfTarget(water);
                RowResult r = DrawRow(d);
                if (r.clicked && !ExplorerRenameActive() && !g_explorerMenuOpen) {
                    g_explorerAnchorIndex = -1;
                    s_selGroup = nullptr;
                    SetSelectedWater(g_selectedWater == water ? nullptr : water);
                    s_ctx.rowClickedThisFrame = true;
                } else if (r.rightClicked && !ExplorerRenameActive()) {
                    SetSelectedWater(water);
                    s_ctx.rightClickHandled = true;
                    s_ctx.rowClickedThisFrame = true;
                }
            }
        }

        // --- Terrain (BasicTerrain and the legacy chunked terrain) ---
        const auto& basicTerrains = BasicTerrain::GetInstances();
        const auto& legacyTerrains = terrain::GetTerrainRegistry().GetTerrains();
        bool anyTerrain = false;
        for (auto* t : basicTerrains) if (t) { anyTerrain = true; break; }
        for (auto* t : legacyTerrains) if (t) { anyTerrain = true; break; }
        if (anyTerrain && DrawCategory("TERRAIN", "#Terrain")) {
            for (auto* terr : basicTerrains) {
                if (!terr) continue;
                if (FilterActive() && !NameMatches(terr->GetName())) continue;
                char label[128];
                snprintf(label, sizeof(label), "%s", terr->GetName().c_str());
                RowDesc d;
                d.id = terr;
                d.icon = ROWICON_TERRAIN;
                d.label = label;
                d.selected = terr->isSelected || terr == BasicTerrain::GetActive();
                d.depth = 1;
                ScrollToIfTarget(terr);
                d.renaming = (g_renameTerrain == terr);
                d.renameBuf = g_nameBuffer;
                d.renameBufSize = sizeof(g_nameBuffer);
                RowResult r = DrawRow(d);
                if (d.renaming) {
                    if (r.renameCommit) { terr->SetName(g_nameBuffer); g_renameTerrain = nullptr; }
                    else if (r.renameCancel) g_renameTerrain = nullptr;
                } else if (r.clicked && !g_explorerMenuOpen) {
                    g_explorerAnchorIndex = -1;
                    s_selGroup = nullptr;
                    SetSelectedTerrain(g_selectedTerrain == terr ? nullptr : terr);
                    s_ctx.rowClickedThisFrame = true;
                } else if (r.rightClicked) {
                    SetSelectedTerrain(terr);
                    s_ctx.rightClickHandled = true;
                    s_ctx.rowClickedThisFrame = true;
                }
                if (r.doubleClicked && !d.renaming) {
                    g_renameTerrain = terr;
                    SetNameBuffer(terr->GetName());
                    g_nameBuffer[sizeof(g_nameBuffer) - 1] = '\0';
                }
            }
            for (auto* legacy : legacyTerrains) {
                if (!legacy) continue;
                if (FilterActive() && !NameMatches(legacy->GetName())) continue;
                const bool isSel = terrain::GetTerrainEditorState().selectedTerrainLegacy == legacy;
                char label[128];
                snprintf(label, sizeof(label), "%s", legacy->GetName().c_str());
                RowDesc d;
                d.id = legacy;
                d.icon = ROWICON_TERRAIN;
                d.label = label;
                d.selected = isSel;
                d.depth = 1;
                ScrollToIfTarget(legacy);
                d.renaming = (g_renameLegacyTerrain == legacy);
                d.renameBuf = g_nameBuffer;
                d.renameBufSize = sizeof(g_nameBuffer);
                RowResult r = DrawRow(d);
                if (d.renaming) {
                    if (r.renameCommit) {
                        std::string newName = g_nameBuffer;
                        if (newName.empty()) newName = "Terrain";
                        bool taken = false;
                        for (auto* t : terrain::GetTerrainRegistry().GetTerrains()) {
                            if (t && t != legacy && t->GetName() == newName) { taken = true; break; }
                        }
                        if (taken) LogAlways("[terrain] name '%s' is already in use - rename blocked", newName.c_str());
                        else legacy->SetName(newName);
                        g_renameLegacyTerrain = nullptr;
                    } else if (r.renameCancel) {
                        g_renameLegacyTerrain = nullptr;
                    }
                } else if (r.clicked && !g_explorerMenuOpen) {
                    g_explorerAnchorIndex = -1;
                    s_selGroup = nullptr;
                    terrain::HandleTerrainSelection(legacy, !isSel);
                    s_ctx.rowClickedThisFrame = true;
                } else if (r.rightClicked) {
                    terrain::HandleTerrainSelection(legacy, true);
                    s_ctx.rightClickHandled = true;
                    s_ctx.rowClickedThisFrame = true;
                }
                if (r.doubleClicked && !d.renaming) {
                    g_renameLegacyTerrain = legacy;
                    SetNameBuffer(legacy->GetName());
                    g_nameBuffer[sizeof(g_nameBuffer) - 1] = '\0';
                }
            }
        }
    }

    // --- Lighting: scene-wide settings. Time is built in; right-clicking the LIGHTING header (or Time) opens an Insert menu for Sun, Ambient, Sky, Fog and so on. Selecting a row
    // opens its settings in the properties area; right-click an inserted item to remove it (it goes back to its default). ---
    {
        gfx::LightingSettings& L = gfx::Lighting();
        if (!g_selection.empty() || g_selectedWater || g_selectedTerrain) g_lightingSel = 0;   // selecting anything else leaves it
        bool headerRightClicked = false;
        const bool lightingOpen = DrawCategory("LIGHTING", "#Lighting", &headerRightClicked);
        if (headerRightClicked) { s_ctx.rightClickHandled = true; ImGui::OpenPopup("##InsertLighting"); }
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
            if (!r.present || !lightingOpen) continue;
            RowDesc rd;
            rd.idInt = 100 + r.id;
            rd.icon = ROWICON_OBJECT;
            rd.label = r.name;
            rd.selected = g_lightingSel == r.id;
            rd.depth = 1;
            const RowResult rr = DrawRow(rd);
            const bool rightClicked = rr.rightClicked;
            if (rr.clicked && !ExplorerRenameActive() && !g_explorerMenuOpen) {
                s_selGroup = nullptr;
                SetSelection({}, nullptr);
                if (g_selectedWater) { g_selectedWater->isSelected = false; g_selectedWater = nullptr; }
                if (g_selectedTerrain) { g_selectedTerrain->isSelected = false; g_selectedTerrain = nullptr; }
                BasicTerrain::SetActive(nullptr);
                terrain::GetTerrainEditorState().selectedTerrainLegacy = nullptr;
                g_lightingSel = r.id;
            }
            if (rightClicked) {
                s_ctx.rightClickHandled = true;
                if (r.removable) { s_removeId = r.id; ImGui::OpenPopup("##LightingRowMenu"); }
                else ImGui::OpenPopup("##InsertLighting");
            }
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
        if (DrawCategory("SCRIPTS", "#Scripts")) {
            for (int i = 0; i < static_cast<int>(scripts.size()); ++i) {
                if (FilterActive() && !NameMatches(scripts[i].name)) continue;
                const std::string key = ScriptKey(scripts[i]);
                char label[128];
                snprintf(label, sizeof(label), "%s", scripts[i].name.c_str());
                RowDesc d;
                d.idInt = 1000 + i;
                d.icon = ROWICON_SCRIPT;
                d.label = label;
                d.selected = g_scriptSel.count(key) > 0;
                d.depth = 1;
                d.renaming = (g_scriptRenameIndex == i);
                d.renameBuf = g_scriptNameBuffer;
                d.renameBufSize = sizeof(g_scriptNameBuffer);
                RowResult r = DrawRow(d);
                if (d.renaming) {
                    if (r.renameCommit) {
                        auto& s = scripts[i];
                        if (s.name != g_scriptNameBuffer) PushUndo();
                        std::string oldClass = scriptLauncher::MakeClassName(s.typeName.empty() ? s.name : s.typeName);
                        s.name = g_scriptNameBuffer;
                        if (s.typeName == oldClass)
                            s.typeName = scriptLauncher::MakeClassName(s.name);
                        vscript::RenameKey(oldClass, ScriptKey(s));
                        g_scriptRenameIndex = -1;
                    } else if (r.renameCancel) {
                        g_scriptRenameIndex = -1;
                    }
                    continue;
                }
                if (r.rightClicked && !g_scriptSel.count(key)) { g_scriptSel = { key }; g_scriptSelAnchor = i; }
                if (r.clicked && !g_explorerMenuOpen) {
                    const ImGuiIO& io = ImGui::GetIO();
                    if (io.KeyCtrl) {                       // Ctrl: add or remove this one
                        if (!g_scriptSel.erase(key)) g_scriptSel.insert(key);
                        g_scriptSelAnchor = i;
                    } else if (io.KeyShift && g_scriptSelAnchor >= 0) {   // Shift: everything from the anchor to here
                        g_scriptSel.clear();
                        const int lo = std::min(g_scriptSelAnchor, i), hi = std::max(g_scriptSelAnchor, i);
                        for (int k = lo; k <= hi && k < static_cast<int>(scripts.size()); ++k) g_scriptSel.insert(ScriptKey(scripts[k]));
                    } else {                                // plain click: just select it
                        g_scriptSel = { key };
                        g_scriptSelAnchor = i;
                    }
                    s_ctx.rowClickedThisFrame = true;
                } else if (r.doubleClicked && !g_explorerMenuOpen) {   // double click opens it in the editor
                    std::string className;
                    scriptLauncher::EditStandaloneScript(scripts[i].name, scripts[i].typeName, &className);
                    if (!className.empty()) {
                        scripts[i].typeName = className;
                        scripts[i].runOnPlay = true;
                    }
                } else if (r.rightClicked) {
                    s_ctx.rightClickHandled = true;
                    OpenExplorerMenu(s_ctx.mouse, ExplorerMenuMode::Script, nullptr, i);
                }
            }
        }
    }

    // Blank space below the rows: a click clears the selection, a drop moves things out of every group.
    {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("##explorerblank", ImVec2(std::max(avail.x, 1.0f), std::max(avail.y, 24.0f)));
        const ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
        if (ImGui::BeginDragDropTarget()) {
            const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("FLY_EXPLORER",
                ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
            if (pl) {
                ImGui::GetWindowDrawList()->AddRect(ImVec2(bmin.x, bmin.y), ImVec2(bmax.x, bmin.y + 1.0f), theme::AccentU32(), 0.0f, 0, 2.0f);
                if (pl->IsDelivery()) {
                    PushUndo();
                    if (s_dragGroup) MoveGroupTo(s_dragGroup, nullptr, nullptr, false);
                    else MoveObjectsToGroup(s_dragObjects, nullptr, nullptr, false);
                    s_dragObjects.clear();
                    s_dragGroup = nullptr;
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ExplorerRenameActive() && !g_explorerMenuOpen) {
            SetSelection({}, nullptr);
            s_selGroup = nullptr;
            g_explorerAnchorIndex = -1;
        }
    }

    s_orderPrev = std::move(s_ctx.order);

    bool contentHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    ImGui::EndChild();

    if (g_sceneObjects && contentHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !s_ctx.rightClickHandled) {
        OpenExplorerMenu(s_ctx.mouse, ExplorerMenuMode::Empty, nullptr, -1);
    }

    ImGui::End();
}

} // namespace ui
