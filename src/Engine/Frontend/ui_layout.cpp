// ui_layout.cpp: where the editor's panels sit. Explorer, Properties and Terrain dock to the left or right
// column (stacked in order), the bottom panel (Assets / Output / Visual Script) sits above the command console.
// Everything is user-adjustable: drag a panel's title to move it to the other side or reorder it, drag the edges
// to resize, right-click the title to close it, or use Preferences > Layout. Saved to <config dir>/layout.cfg.
#include "ui_internal.hpp"
#include "../../../include/Engine/AI/AI.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine/Scripts/CommandConsole.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ui {

namespace {

constexpr float kTopBarH   = 120.0f;
constexpr float kMinColW   = 160.0f;
constexpr float kMinPanelH = 90.0f;
constexpr float kMinBottomH = 90.0f;
constexpr float kGrab      = 4.0f;    // half-thickness of a resize edge's hit area
constexpr float kDragStart = 5.0f;    // pixels the mouse must travel before a title press becomes a move

ImU32 AccentU32(float a) { return ImGui::ColorConvertFloat4ToU32(theme::AccentVec(a)); }

const char* const kPanelNames[kPanelCount] = { "Explorer", "Properties", "Terrain", "Assets / Output", "AI Assistant" };

LayoutState s_layout;
bool s_loaded = false;

void ResetDefaults(LayoutState& l) {
    l = LayoutState{};
    l.panel[(int)PanelId::Explorer]   = { true, DockSide::Left,  0, 1.0f };
    l.panel[(int)PanelId::Properties] = { true, DockSide::Right, 0, 1.0f };
    l.panel[(int)PanelId::Terrain]    = { true, DockSide::Right, 1, 1.0f };
    l.panel[(int)PanelId::Bottom]     = { true, DockSide::Left,  0, 1.0f };
    l.panel[(int)PanelId::AI]         = { true, DockSide::Left,  1, 1.0f };   // under the Explorer
}

std::string LayoutPath() {
    const std::string dir = platform::ConfigDir();
    return dir.empty() ? std::string() : dir + "/layout.cfg";
}

void EnsureLoaded() {
    if (s_loaded) return;
    s_loaded = true;
    ResetDefaults(s_layout);
    const std::string path = LayoutPath();
    if (path.empty()) return;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        const float f = static_cast<float>(std::atof(val.c_str()));
        if (key == "leftW") s_layout.leftW = f;
        else if (key == "rightW") s_layout.rightW = f;
        else if (key == "bottomH") s_layout.bottomH = f;
        else if (key == "locked") s_layout.locked = f != 0.0f;
        else {
            for (int i = 0; i < kPanelCount; ++i) {
                char tag[32];
                snprintf(tag, sizeof(tag), "panel%d", i);
                if (key != tag) continue;
                int vis = 1, side = 0, order = 0;
                float weight = 1.0f;
                if (sscanf(val.c_str(), "%d,%d,%d,%f", &vis, &side, &order, &weight) == 4) {
                    PanelState& p = s_layout.panel[i];
                    p.visible = vis != 0;
                    p.side = side == 1 ? DockSide::Right : DockSide::Left;
                    p.order = order;
                    p.weight = std::clamp(weight, 0.1f, 10.0f);
                }
            }
        }
    }
}

// Shown right now. The AI panel also needs the feature switched on in Preferences > AI, so it takes no room while off.
bool PanelShown(int i) {
    if (!s_layout.panel[i].visible) return false;
    return i != (int)PanelId::AI || (ai::GetSettings().enabled && ai::Available());
}

// Visible panels docked to `side`, top to bottom (the bottom panel is not in a column).
int ColumnPanels(DockSide side, PanelId out[kPanelCount]) {
    int n = 0;
    for (int i = 0; i < kPanelCount; ++i) {
        if (i == (int)PanelId::Bottom) continue;
        if (PanelShown(i) && s_layout.panel[i].side == side) out[n++] = static_cast<PanelId>(i);
    }
    std::stable_sort(out, out + n, [](PanelId a, PanelId b) {
        return s_layout.panel[(int)a].order < s_layout.panel[(int)b].order;
    });
    return n;
}

float MaxColW() { return std::max(kMinColW, static_cast<float>(GetScreenWidth()) * 0.4f); }

void ColumnWidths(float& left, float& right) {
    EnsureLoaded();
    PanelId tmp[kPanelCount];
    left  = ColumnPanels(DockSide::Left, tmp)  > 0 ? std::clamp(s_layout.leftW,  kMinColW, MaxColW()) : 0.0f;
    right = ColumnPanels(DockSide::Right, tmp) > 0 ? std::clamp(s_layout.rightW, kMinColW, MaxColW()) : 0.0f;
}

struct Computed {
    Rectangle rect[kPanelCount] = {};
    float leftW = 0.0f, rightW = 0.0f;
};

Computed Compute() {
    Computed c;
    ColumnWidths(c.leftW, c.rightW);
    const float sw = static_cast<float>(GetScreenWidth());
    const float colH = static_cast<float>(GetScreenHeight()) - kTopBarH;
    for (DockSide side : { DockSide::Left, DockSide::Right }) {
        PanelId ids[kPanelCount];
        const int n = ColumnPanels(side, ids);
        float total = 0.0f;
        for (int i = 0; i < n; ++i) total += s_layout.panel[(int)ids[i]].weight;
        const float w = side == DockSide::Left ? c.leftW : c.rightW;
        const float x = side == DockSide::Left ? 0.0f : sw - w;
        float y = kTopBarH;
        for (int i = 0; i < n; ++i) {
            const float h = (i == n - 1) ? kTopBarH + colH - y : std::floor(colH * s_layout.panel[(int)ids[i]].weight / total);
            c.rect[(int)ids[i]] = { x, y, w, h };
            y += h;
        }
    }
    if (s_layout.panel[(int)PanelId::Bottom].visible) {
        const float barY = console::GetBounds().y;
        const float maxH = std::max(kMinBottomH, barY - kTopBarH - 60.0f);
        const float h = std::clamp(s_layout.bottomH, kMinBottomH, maxH);
        c.rect[(int)PanelId::Bottom] = { c.leftW, barY - h, sw - c.leftW - c.rightW, h };
    }
    return c;
}

// --- interaction ---------------------------------------------------------------------------------------------
enum class Drag { None, Column, Split, Bottom, Move };

struct Edge {
    Rectangle hit;
    Drag kind;
    DockSide side;
    int index;      // Split: position of the panel above the edge within its column
    bool horizontal; // the edge runs left-right, so the cursor is the vertical-resize one
};

Drag s_drag = Drag::None;
DockSide s_dragSide = DockSide::Left;
int s_dragIndex = 0;
PanelId s_movePanel = PanelId::Explorer;
bool s_movePending = false;   // title pressed, not yet moved far enough
Vector2 s_pressPos = {};
bool s_openMenu = false;
PanelId s_menuPanel = PanelId::Explorer;

// Drop target while moving a panel.
struct DropTarget { DockSide side; int index; };
DropTarget s_drop = { DockSide::Left, 0 };

Rectangle TitleHandle(PanelId id, const Rectangle& r) {
    switch (id) {
    case PanelId::Explorer: return { r.x, r.y, std::min(r.width, 96.0f), 26.0f };
    case PanelId::Properties: return { r.x, r.y, r.width, kPanelTitleH - 2.0f };
    case PanelId::Terrain: return { r.x, r.y, r.width, kPanelTitleH - 2.0f };
    case PanelId::AI: return { r.x, r.y, r.width, kPanelTitleH - 2.0f };
    default: return { 0, 0, 0, 0 };
    }
}

DropTarget FindDrop(const Computed& c, Vector2 mouse, PanelId moving) {
    DropTarget t;
    t.side = mouse.x < static_cast<float>(GetScreenWidth()) * 0.5f ? DockSide::Left : DockSide::Right;
    PanelId ids[kPanelCount];
    const int n = ColumnPanels(t.side, ids);
    t.index = 0;   // counts only the panels other than the moving one
    for (int i = 0; i < n; ++i) {
        if (ids[i] == moving) continue;
        const Rectangle& r = c.rect[(int)ids[i]];
        if (mouse.y > r.y + r.height * 0.5f) ++t.index;
    }
    return t;
}

void ApplyMove(PanelId id, DropTarget t) {
    PanelState& p = s_layout.panel[(int)id];
    p.side = t.side;
    p.visible = true;
    PanelId ids[kPanelCount];
    const int n = ColumnPanels(t.side, ids);
    std::vector<PanelId> order;
    for (int i = 0; i < n; ++i) if (ids[i] != id) order.push_back(ids[i]);
    order.insert(order.begin() + std::min<size_t>(t.index, order.size()), id);
    for (size_t i = 0; i < order.size(); ++i) s_layout.panel[(int)order[i]].order = static_cast<int>(i);
    p.weight = 1.0f;
}

std::vector<Edge> Edges(const Computed& c) {
    std::vector<Edge> e;
    const float sw = static_cast<float>(GetScreenWidth());
    const float sh = static_cast<float>(GetScreenHeight());
    if (c.leftW > 0.0f)  e.push_back({ { c.leftW - kGrab, kTopBarH, kGrab * 2, sh - kTopBarH }, Drag::Column, DockSide::Left, 0, false });
    if (c.rightW > 0.0f) e.push_back({ { sw - c.rightW - kGrab, kTopBarH, kGrab * 2, sh - kTopBarH }, Drag::Column, DockSide::Right, 0, false });
    for (DockSide side : { DockSide::Left, DockSide::Right }) {
        PanelId ids[kPanelCount];
        const int n = ColumnPanels(side, ids);
        for (int i = 0; i + 1 < n; ++i) {
            const Rectangle& r = c.rect[(int)ids[i]];
            e.push_back({ { r.x, r.y + r.height - kGrab, r.width, kGrab * 2 }, Drag::Split, side, i, true });
        }
    }
    const Rectangle& b = c.rect[(int)PanelId::Bottom];
    if (b.width > 0.0f) e.push_back({ { b.x, b.y - kGrab, b.width, kGrab * 2 }, Drag::Bottom, DockSide::Left, 0, true });
    return e;
}

bool ImGuiBlocking() {
    if (!ImGui::GetCurrentContext()) return false;
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

void ApplyResize(const Computed& c, Vector2 mouse) {
    const float sw = static_cast<float>(GetScreenWidth());
    if (s_drag == Drag::Column) {
        if (s_dragSide == DockSide::Left) s_layout.leftW = std::clamp(mouse.x, kMinColW, MaxColW());
        else s_layout.rightW = std::clamp(sw - mouse.x, kMinColW, MaxColW());
    } else if (s_drag == Drag::Bottom) {
        const float barY = console::GetBounds().y;
        s_layout.bottomH = std::clamp(barY - mouse.y, kMinBottomH, std::max(kMinBottomH, barY - kTopBarH - 60.0f));
    } else if (s_drag == Drag::Split) {
        PanelId ids[kPanelCount];
        const int n = ColumnPanels(s_dragSide, ids);
        if (s_dragIndex + 1 >= n) return;
        PanelState& a = s_layout.panel[(int)ids[s_dragIndex]];
        PanelState& b = s_layout.panel[(int)ids[s_dragIndex + 1]];
        const Rectangle& ra = c.rect[(int)ids[s_dragIndex]];
        const Rectangle& rb = c.rect[(int)ids[s_dragIndex + 1]];
        const float pair = ra.height + rb.height;
        const float newA = std::clamp(mouse.y - ra.y, kMinPanelH, std::max(kMinPanelH, pair - kMinPanelH));
        const float pairWeight = a.weight + b.weight;
        a.weight = pairWeight * newA / pair;
        b.weight = pairWeight - a.weight;
    }
}

} // namespace

LayoutState& Layout() {
    EnsureLoaded();
    return s_layout;
}

const char* PanelName(PanelId id) { return kPanelNames[(int)id]; }

void ResetLayout() {
    EnsureLoaded();
    ResetDefaults(s_layout);
    SaveLayout();
}

void SaveLayout() {
    EnsureLoaded();
    const std::string path = LayoutPath();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    out << "leftW=" << s_layout.leftW << "\nrightW=" << s_layout.rightW << "\nbottomH=" << s_layout.bottomH
        << "\nlocked=" << (s_layout.locked ? 1 : 0) << "\n";
    for (int i = 0; i < kPanelCount; ++i) {
        const PanelState& p = s_layout.panel[i];
        out << "panel" << i << '=' << (p.visible ? 1 : 0) << ',' << (p.side == DockSide::Right ? 1 : 0) << ','
            << p.order << ',' << p.weight << '\n';
    }
}

Rectangle GetPanelBounds(PanelId id) {
    EnsureLoaded();
    if (!PanelShown((int)id)) return Rectangle{ 0, 0, 0, 0 };
    return Compute().rect[(int)id];
}

bool IsPanelVisible(PanelId id) { EnsureLoaded(); return s_layout.panel[(int)id].visible; }

void SetPanelVisible(PanelId id, bool visible) {
    EnsureLoaded();
    if (s_layout.panel[(int)id].visible == visible) return;
    s_layout.panel[(int)id].visible = visible;
    SaveLayout();
}

void GetColumnWidths(float& left, float& right) { ColumnWidths(left, right); }

DockSide GetPanelSide(PanelId id) { EnsureLoaded(); return s_layout.panel[(int)id].side; }

void SetPanelSide(PanelId id, DockSide side) {
    EnsureLoaded();
    if (s_layout.panel[(int)id].side == side) return;
    PanelId ids[kPanelCount];
    ApplyMove(id, { side, ColumnPanels(side, ids) });
    SaveLayout();
}

void UpdatePanelLayout() {
    EnsureLoaded();
    const Vector2 mouse = GetMousePosition();

    if (s_drag != Drag::None || s_movePending) {
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            if (s_drag == Drag::Move) {
                ApplyMove(s_movePanel, s_drop);
                SaveLayout();
            } else if (s_drag != Drag::None) {
                SaveLayout();
            }
            s_drag = Drag::None;
            s_movePending = false;
            return;
        }
        const Computed c = Compute();
        if (s_movePending) {
            const float dx = mouse.x - s_pressPos.x, dy = mouse.y - s_pressPos.y;
            if (dx * dx + dy * dy >= kDragStart * kDragStart) { s_drag = Drag::Move; s_movePending = false; }
        }
        if (s_drag == Drag::Move) {
            s_drop = FindDrop(c, mouse, s_movePanel);
            MarkHand();
        } else if (s_drag != Drag::None) {
            ApplyResize(c, mouse);
            MarkResize(s_drag == Drag::Bottom || s_drag == Drag::Split);
        }
        g_clickConsumedThisFrame = true;
        return;
    }

    if (g_showPreferences || ImGuiBlocking() || g_playActive) return;
    if (mouse.y < kTopBarH) return;

    const Computed c = Compute();

    // Resize edges first: they sit on panel borders, so they win over the title handles.
    if (!s_layout.locked) {
        for (const Edge& e : Edges(c)) {
            if (!CheckCollisionPointRec(mouse, e.hit)) continue;
            MarkResize(e.horizontal);
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                s_drag = e.kind;
                s_dragSide = e.side;
                s_dragIndex = e.index;
                g_clickConsumedThisFrame = true;
            }
            return;
        }
    }

    for (int i = 0; i < kPanelCount; ++i) {
        const PanelId id = static_cast<PanelId>(i);
        if (i == (int)PanelId::Bottom || !PanelShown(i)) continue;
        if (!CheckCollisionPointRec(mouse, TitleHandle(id, c.rect[i]))) continue;
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            s_openMenu = true;
            s_menuPanel = id;
        } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !s_layout.locked) {
            s_movePending = true;
            s_movePanel = id;
            s_pressPos = mouse;
        }
        return;
    }

    // The bottom panel has no title strip of its own (its tab bar is the title), so right-click on the empty
    // tab-bar area offers the same menu.
    const Rectangle& b = c.rect[(int)PanelId::Bottom];
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && b.width > 0.0f &&
        CheckCollisionPointRec(mouse, { b.x, b.y, b.width, 24.0f }) && !ImGui::GetIO().WantTextInput &&
        !ImGui::IsAnyItemHovered()) {
        s_openMenu = true;
        s_menuPanel = PanelId::Bottom;
    }
}

void DrawPanelLayoutOverlay() {
    EnsureLoaded();
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    if (s_drag == Drag::Move) {
        const Computed c = Compute();
        const float sw = static_cast<float>(GetScreenWidth());
        const float sh = static_cast<float>(GetScreenHeight());
        const bool left = s_drop.side == DockSide::Left;
        const float w = left ? (c.leftW > 0 ? c.leftW : s_layout.leftW) : (c.rightW > 0 ? c.rightW : s_layout.rightW);
        const float x = left ? 0.0f : sw - w;
        dl->AddRectFilled(ImVec2(x, kTopBarH), ImVec2(x + w, sh), AccentU32(0.12f));
        dl->AddRect(ImVec2(x, kTopBarH), ImVec2(x + w, sh), AccentU32(0.8f), 0.0f, 0, 2.0f);
        // Insertion line between the panels already in that column.
        PanelId ids[kPanelCount];
        const int n = ColumnPanels(s_drop.side, ids);
        float lineY = kTopBarH + 2.0f;
        int seen = 0;
        for (int i = 0; i < n; ++i) {
            if (ids[i] == s_movePanel) continue;
            ++seen;
            if (seen <= s_drop.index) lineY = c.rect[(int)ids[i]].y + c.rect[(int)ids[i]].height;
        }
        dl->AddRectFilled(ImVec2(x + 2, lineY - 2), ImVec2(x + w - 2, lineY + 2), AccentU32(1.0f));
        const Vector2 m = GetMousePosition();
        const char* name = kPanelNames[(int)s_movePanel];
        const ImVec2 ts = ImGui::CalcTextSize(name);
        dl->AddRectFilled(ImVec2(m.x + 12, m.y + 8), ImVec2(m.x + 24 + ts.x, m.y + 16 + ts.y), IM_COL32(25, 27, 32, 230), 4.0f);
        dl->AddText(ImVec2(m.x + 18, m.y + 12), IM_COL32(255, 255, 255, 255), name);
    }

    if (s_openMenu) {
        ImGui::OpenPopup("##PanelHeaderMenu");
        s_openMenu = false;
    }
    if (ImGui::BeginPopup("##PanelHeaderMenu")) {
        ImGui::TextDisabled("%s", kPanelNames[(int)s_menuPanel]);
        ImGui::Separator();
        if (ImGui::MenuItem("Close panel")) { SetPanelVisible(s_menuPanel, false); }
        if (s_menuPanel != PanelId::Bottom) {
            const DockSide cur = GetPanelSide(s_menuPanel);
            if (ImGui::MenuItem("Dock left", nullptr, cur == DockSide::Left)) SetPanelSide(s_menuPanel, DockSide::Left);
            if (ImGui::MenuItem("Dock right", nullptr, cur == DockSide::Right)) SetPanelSide(s_menuPanel, DockSide::Right);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) ResetLayout();
        if (ImGui::MenuItem("Layout preferences...")) { ShowPreferences(true, 7); }
        ImGui::EndPopup();
    }
}

} // namespace ui
