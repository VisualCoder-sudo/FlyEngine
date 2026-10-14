#include "../../../include/Engine/Backend/VisualScript.hpp"
#include "imgui.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace vscript {

namespace {

constexpr float kHeaderH = 24.0f;
constexpr float kPinR = 6.0f;

ImVec2 g_pan(20.0f, 20.0f);
int g_selected = 0;
int g_dragFrom = 0;            // node whose exec-out pin is being dragged
int g_dragPin = 0;             // ...and which of its output pins
int g_dragData = 0;            // value node whose output is being dragged
int g_wireMenuToPin = -1;      // >= 0: the wire menu is for a value wire into that input pin
ImVec2 g_menuCanvasPos(0, 0);
char g_search[64] = "";
std::string g_browsePath;      // folder shown in the target picker, e.g. "Workspace/Group1"
int g_pickerNode = 0;

ImU32 HeaderColor(NodeType t) {
    switch (t) {
        case NodeType::Start: return IM_COL32(40, 130, 60, 255);
        case NodeType::Change: return IM_COL32(40, 95, 170, 255);
        case NodeType::End: return IM_COL32(170, 50, 50, 255);
        case NodeType::OnUpdate: case NodeType::OnStop: return IM_COL32(40, 130, 60, 255);
        case NodeType::Branch: case NodeType::Sequence: case NodeType::Delay:
        case NodeType::Repeat: case NodeType::DoOnce: return IM_COL32(150, 110, 30, 255);
        case NodeType::Print: return IM_COL32(110, 70, 160, 255);
        case NodeType::SetVariable: return IM_COL32(40, 130, 140, 255);
        default: if (IsPure(t)) return IM_COL32(40, 110, 130, 255); break;
    }
    return IM_COL32(80, 80, 80, 255);
}

std::string TargetLabel(const Node& n) { return n.target.empty() ? std::string("Game...") : "Game/" + n.target; }

// The Target button grows with its text, and the node grows with the button.
float TargetButtonW(const Node& n) {
    return std::max(180.0f, ImGui::CalcTextSize(TargetLabel(n).c_str()).x + 24.0f);
}

constexpr float kRowH = 26.0f;

int BodyRows(NodeType t) {
    switch (t) {
        case NodeType::Change: return 3;
        case NodeType::SetVariable: return 2;
        case NodeType::Print: case NodeType::Delay: case NodeType::Branch:
        case NodeType::Sequence: case NodeType::Repeat:
        case NodeType::GetVariable: case NodeType::Not: return 1;
        case NodeType::GetProperty: case NodeType::And: case NodeType::Or: case NodeType::Add: case NodeType::Sub:
        case NodeType::Mul: case NodeType::Div: case NodeType::Random: return 2;
        case NodeType::Compare: case NodeType::Lerp: return 3;
        default: return 0;
    }
}

// Body row that holds data input pin `pin`, or -1.
int DataPinRow(const Node& n, int pin) {
    if (pin < 0 || pin >= DataInputCount(n.type)) return -1;
    switch (n.type) {
        case NodeType::SetVariable: return 1;
        case NodeType::Change: return 2;
        case NodeType::Compare: return pin == 0 ? 0 : 2;
        default: return pin;
    }
}

bool IsWired(const Graph& g, int id, int pin) {
    for (const DataLink& d : g.dataLinks) if (d.to == id && d.toPin == pin) return true;
    return false;
}

// Branching nodes list their output pins in rows under the body; everything else has one pin in the header.
bool MultiOut(const Node& n) { return OutputCount(n.type, &n) > 1; }

float NodeWidth(const Node& n) {
    if (n.type == NodeType::Change) return 10.0f + 60.0f + TargetButtonW(n) + 14.0f;
    return BodyRows(n.type) ? 190.0f : 130.0f;
}

float NodeHeight(const Node& n) {
    const int rows = BodyRows(n.type) + (MultiOut(n) ? OutputCount(n.type, &n) : 0);
    if (!rows) return kHeaderH + 12.0f;
    return kHeaderH + 8.0f + rows * kRowH + 8.0f;
}

bool IsBoolProp(const std::string& p) {
    return p == "Anchored" || p == "CanCollide" || p == "GlobalShadows" || p == "Grid" || p == "Wireframe";
}
bool IsVecProp(const std::string& p) { return p == "Position" || p == "Size" || p == "Rotation"; }

ImVec2 InData(const Node& n, ImVec2 o, int pin) {
    return ImVec2(o.x + n.x, o.y + n.y + kHeaderH + 6.0f + DataPinRow(n, pin) * kRowH + 11.0f);
}

void DrawDataPin(ImDrawList* dl, ImVec2 c, bool connected) {
    const ImU32 col = IM_COL32(90, 200, 230, 255);
    if (connected) dl->AddCircleFilled(c, 5.5f, col);
    else dl->AddCircle(c, 5.5f, col, 0, 1.8f);
}

ImVec2 InPin(const Node& n, ImVec2 o) { return ImVec2(o.x + n.x, o.y + n.y + kHeaderH * 0.5f); }
ImVec2 OutPin(const Node& n, ImVec2 o, int pin = 0) {
    const float y = MultiOut(n) ? kHeaderH + 8.0f + (BodyRows(n.type) + pin) * kRowH + kRowH * 0.5f : kHeaderH * 0.5f;
    return ImVec2(o.x + n.x + NodeWidth(n), o.y + n.y + y);
}

void DrawWire(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
    const float d = std::max(40.0f, std::abs(b.x - a.x) * 0.5f);
    dl->AddBezierCubic(a, ImVec2(a.x + d, a.y), ImVec2(b.x - d, b.y), b, col, 2.5f);
}

// Distance from `m` to the wire a->b (same curve DrawWire draws), sampled along it.
float WireDistance(ImVec2 a, ImVec2 b, ImVec2 m) {
    const float d = std::max(40.0f, std::abs(b.x - a.x) * 0.5f);
    const ImVec2 c1(a.x + d, a.y), c2(b.x - d, b.y);
    float best = FLT_MAX;
    for (int i = 0; i <= 32; ++i) {
        const float t = i / 32.0f, u = 1.0f - t;
        const float x = u*u*u*a.x + 3*u*u*t*c1.x + 3*u*t*t*c2.x + t*t*t*b.x;
        const float y = u*u*u*a.y + 3*u*u*t*c1.y + 3*u*t*t*c2.y + t*t*t*b.y;
        best = std::min(best, std::hypot(x - m.x, y - m.y));
    }
    return best;
}

int g_wireMenuFrom = 0, g_wireMenuTo = 0, g_wireMenuPin = 0;

void DrawExecPin(ImDrawList* dl, ImVec2 c, bool connected) {
    const ImU32 col = IM_COL32(245, 245, 245, 255);
    ImVec2 p[3] = { ImVec2(c.x - 5, c.y - 6), ImVec2(c.x + 6, c.y), ImVec2(c.x - 5, c.y + 6) };
    if (connected) dl->AddConvexPolyFilled(p, 3, col);
    else dl->AddPolyline(p, 3, col, ImDrawFlags_Closed, 1.8f);
}

std::string FormatVec(const float v[3]) {
    char b[96];
    std::snprintf(b, sizeof(b), "(%g, %g, %g)", v[0], v[1], v[2]);
    return b;
}

void DrawValueWidget(Node& n) {
    ImGui::SetNextItemWidth(170.0f);
    if (n.prop == "Color") {
        int r = 255, g = 0, b = 0;
        std::sscanf(n.value.c_str(), " ( %d , %d , %d", &r, &g, &b);
        float c[3] = { r / 255.0f, g / 255.0f, b / 255.0f };
        if (ImGui::ColorEdit3("##val", c, ImGuiColorEditFlags_NoInputs)) {
            char s[64];
            std::snprintf(s, sizeof(s), "(%d, %d, %d)", int(c[0] * 255 + 0.5f), int(c[1] * 255 + 0.5f), int(c[2] * 255 + 0.5f));
            n.value = s;
        }
    } else if (IsVecProp(n.prop)) {
        float v[3] = { 0, 0, 0 };
        std::sscanf(n.value.c_str(), " ( %f , %f , %f", &v[0], &v[1], &v[2]);
        if (ImGui::DragFloat3("##val", v, 0.05f)) n.value = FormatVec(v);
    } else if (IsBoolProp(n.prop)) {
        bool b = n.value == "true";
        if (ImGui::Checkbox("##val", &b)) n.value = b ? "true" : "false";
    } else {
        float f = static_cast<float>(std::atof(n.value.c_str()));
        if (ImGui::DragFloat("##val", &f, 0.05f)) {
            char s[32];
            std::snprintf(s, sizeof(s), "%g", f);
            n.value = s;
        }
    }
}

// Places the next popup under `anchor` (a button's rect), or above it when there is more room there, and caps its
// height to the room so a long list scrolls instead of running off the screen.
void PlaceListPopup(ImVec2 anchorMin, ImVec2 anchorMax) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float below = vp->Pos.y + vp->Size.y - anchorMax.y - 8.0f;
    const float above = anchorMin.y - vp->Pos.y - 8.0f;
    if (below >= std::min(above, 260.0f)) {
        ImGui::SetNextWindowPos(ImVec2(anchorMin.x, anchorMax.y), ImGuiCond_Always, ImVec2(0, 0));
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, std::max(80.0f, below)));
    } else {
        ImGui::SetNextWindowPos(ImVec2(anchorMin.x, anchorMin.y), ImGuiCond_Always, ImVec2(0, 1));
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, std::max(80.0f, above)));
    }
}

// File-explorer style picker, rooted at "Game".
void DrawTargetPopup(Node& n, const SceneListing& scene) {
    PlaceListPopup(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (!ImGui::BeginPopup("##vsTarget")) return;
    ImGui::TextDisabled("Game%s%s", g_browsePath.empty() ? "" : " / ", g_browsePath.c_str());
    ImGui::Separator();
    if (!g_browsePath.empty() && ImGui::Selectable(".. (up)", false, ImGuiSelectableFlags_DontClosePopups)) {
        const size_t s = g_browsePath.find_last_of('/');
        g_browsePath = s == std::string::npos ? "" : g_browsePath.substr(0, s);
    }
    auto pick = [&](const std::string& path) { n.target = path; ImGui::CloseCurrentPopup(); };
    if (g_browsePath.empty()) {
        if (ImGui::Selectable("[folder] Workspace", false, ImGuiSelectableFlags_DontClosePopups)) g_browsePath = "Workspace";
        for (const auto& g : WorldGroups())
            if (ImGui::Selectable(g.c_str())) { pick(g); n.prop = WorldProperties(g).front(); n.value = IsBoolProp(n.prop) ? "true" : "0"; }
    } else if (g_browsePath == "Workspace") {
        // Several objects can share a name, so every row gets an ID of its own.
        for (size_t i = 0; i < scene.groups.size(); ++i) {
            const auto& g = scene.groups[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(("[folder] " + g.name).c_str(), false, ImGuiSelectableFlags_DontClosePopups)) g_browsePath = "Workspace/" + g.name;
            ImGui::PopID();
        }
        for (size_t i = 0; i < scene.loose.size(); ++i) {
            ImGui::PushID(static_cast<int>(1000 + i));
            if (ImGui::Selectable(scene.loose[i].c_str())) pick("Workspace/" + scene.loose[i]);
            ImGui::PopID();
        }
    } else {
        const std::string gname = g_browsePath.substr(g_browsePath.find('/') + 1);
        for (const auto& g : scene.groups) {
            if (g.name != gname) continue;
            for (size_t i = 0; i < g.members.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(g.members[i].c_str())) pick(g_browsePath + "/" + g.members[i]);
                ImGui::PopID();
            }
        }
    }
    ImGui::EndPopup();
}

// One labelled text field in a node body row.
void StrRow(Node& n, ImVec2 o, int row, const char* label, std::string& s, const char* id, bool wired = false) {
    const float x = o.x + n.x + 10.0f, y = o.y + n.y + kHeaderH + 6.0f + row * kRowH;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SetCursorScreenPos(ImVec2(x + 60.0f, y));
    if (wired) { ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("(wired)"); return; }
    ImGui::SetNextItemWidth(NodeWidth(n) - 80.0f);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", s.c_str());
    if (ImGui::InputText(id, buf, sizeof(buf))) s = buf;
}

void NumRow(Node& n, ImVec2 o, int row, const char* label, std::string& s, bool integer, bool wired = false) {
    const float x = o.x + n.x + 10.0f, y = o.y + n.y + kHeaderH + 6.0f + row * kRowH;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SetCursorScreenPos(ImVec2(x + 60.0f, y));
    if (wired) { ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("(wired)"); return; }
    ImGui::SetNextItemWidth(NodeWidth(n) - 80.0f);
    char b[32];
    if (integer) {
        int v = std::atoi(s.c_str());
        if (ImGui::DragInt("##num", &v, 0.1f, 0, 1000)) { std::snprintf(b, sizeof(b), "%d", v); s = b; }
    } else {
        float v = static_cast<float>(std::atof(s.c_str()));
        if (ImGui::DragFloat("##num", &v, 0.05f, 0.0f, 3600.0f)) { std::snprintf(b, sizeof(b), "%g", v); s = b; }
    }
}

void ComboRow(Node& n, ImVec2 o, int row, const char* label, std::string& s, const std::vector<std::string>& items) {
    const float x = o.x + n.x + 10.0f, y = o.y + n.y + kHeaderH + 6.0f + row * kRowH;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SetCursorScreenPos(ImVec2(x + 60.0f, y));
    ImGui::SetNextItemWidth(NodeWidth(n) - 80.0f);
    if (ImGui::BeginCombo("##combo", s.c_str())) {
        for (const auto& it : items) if (ImGui::Selectable(it.c_str(), it == s)) s = it;
        ImGui::EndCombo();
    }
}

void DrawNodeBody(Node& n, ImVec2 o, const SceneListing& scene) {
    const Graph& g = Active();
    auto wired = [&](int pin) { return IsWired(g, n.id, pin); };
    static const std::vector<std::string> kOps = { "==", "!=", "<", ">", "<=", ">=" };
    ImGui::PushID(n.id);
    switch (n.type) {
        case NodeType::Print: StrRow(n, o, 0, "Text", n.value, "##txt", wired(0)); break;
        case NodeType::Delay: NumRow(n, o, 0, "Seconds", n.value, false, wired(0)); break;
        case NodeType::Repeat: NumRow(n, o, 0, "Times", n.value, true); break;
        case NodeType::Sequence: NumRow(n, o, 0, "Outputs", n.value, true); break;
        case NodeType::Branch: StrRow(n, o, 0, wired(0) ? "Condition" : "Variable", n.target, "##var", wired(0)); break;
        case NodeType::SetVariable:
            StrRow(n, o, 0, "Name", n.target, "##var");
            StrRow(n, o, 1, "Value", n.value, "##val", wired(0));
            break;
        case NodeType::GetVariable: StrRow(n, o, 0, "Name", n.target, "##var"); break;
        case NodeType::GetProperty:
            StrRow(n, o, 0, "Object", n.target, "##obj");
            ComboRow(n, o, 1, "Property", n.prop, ObjectProperties());
            break;
        case NodeType::Compare:
            StrRow(n, o, 0, "A", n.target, "##a", wired(0));
            ComboRow(n, o, 1, "Op", n.prop, kOps);
            StrRow(n, o, 2, "B", n.value, "##b", wired(1));
            break;
        case NodeType::Not: StrRow(n, o, 0, "In", n.target, "##a", wired(0)); break;
        case NodeType::Lerp:
            StrRow(n, o, 0, "A", n.target, "##a", wired(0));
            StrRow(n, o, 1, "B", n.value, "##b", wired(1));
            StrRow(n, o, 2, "T", n.prop, "##t", wired(2));
            break;
        default:
            if (IsPure(n.type)) {
                StrRow(n, o, 0, DataInputLabel(n.type, 0), n.target, "##a", wired(0));
                StrRow(n, o, 1, DataInputLabel(n.type, 1), n.value, "##b", wired(1));
            }
            break;
    }
    ImGui::PopID();
    if (n.type != NodeType::Change) return;
    const float x = o.x + n.x + 10.0f;
    float y = o.y + n.y + kHeaderH + 6.0f;
    const bool world = std::find(WorldGroups().begin(), WorldGroups().end(), n.target) != WorldGroups().end();

    ImGui::PushID(n.id);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Target");
    ImGui::SameLine(70.0f);
    ImGui::SetCursorScreenPos(ImVec2(x + 60.0f, y));
    const std::string label = TargetLabel(n);
    if (ImGui::Button((label + "##t").c_str(), ImVec2(TargetButtonW(n), 0))) {
        g_browsePath.clear();
        g_pickerNode = n.id;
        ImGui::OpenPopup("##vsTarget");
    }
    if (g_pickerNode == n.id) DrawTargetPopup(n, scene);

    y += 26.0f;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Property");
    ImGui::SetCursorScreenPos(ImVec2(x + 60.0f, y));
    ImGui::SetNextItemWidth(TargetButtonW(n));
    const std::vector<std::string> props = world ? WorldProperties(n.target) : ObjectProperties();
    if (ImGui::BeginCombo("##p", n.prop.c_str())) {
        for (const auto& p : props)
            if (ImGui::Selectable(p.c_str(), p == n.prop)) {
                n.prop = p;
                n.value = IsBoolProp(p) ? "true" : IsVecProp(p) ? "(0, 0, 0)" : p == "Color" ? "(255, 0, 0)" : "0";
            }
        ImGui::EndCombo();
    }

    y += 26.0f;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(n.prop == "Color" ? "Colour" : "Value");
    ImGui::SetCursorScreenPos(ImVec2(x + 60.0f, y));
    if (IsWired(g, n.id, 0)) { ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("(wired)"); }
    else DrawValueWidget(n);
    ImGui::PopID();
}

} // namespace

namespace {
char g_nameBuf[64] = "";

// Shown when the scene has no visual script yet.
void DrawEmptyState(const SceneListing& scene) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float w = 220.0f;
    ImGui::SetCursorPos(ImVec2(std::max(0.0f, (avail.x - w) * 0.5f), std::max(0.0f, avail.y * 0.5f - 50.0f)));
    ImGui::BeginGroup();
    ImGui::TextDisabled("No visual scripts yet");
    if (ImGui::Button("Create new script", ImVec2(w, 30))) NewScript();
    if (ImGui::Button("Open existing", ImVec2(w, 30))) ImGui::OpenPopup("##vsExisting");
    PlaceListPopup(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::BeginPopup("##vsExisting")) {
        if (scene.existingScripts.empty()) ImGui::TextDisabled("No scripts in this scene");
        for (size_t i = 0; i < scene.existingScripts.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(scene.existingScripts[i].c_str())) OpenByName(scene.existingScripts[i]);
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::EndGroup();
}

// Small button at the canvas' top-left: lists the scripts, switches between them.
void DrawScriptManager(const SceneListing& scene) {
    ImGui::SetCursorPos(ImVec2(8, 8));
    if (ImGui::Button("Scripts \xe2\x96\xbe")) ImGui::OpenPopup("##vsManager");
    const ImVec2 btnMin = ImGui::GetItemRectMin(), btnMax = ImGui::GetItemRectMax();
    ImGui::SameLine();
    ImGui::TextUnformatted(Count() ? NameAt(CurrentIndex()).c_str() : "");
    PlaceListPopup(btnMin, btnMax);
    if (ImGui::BeginPopup("##vsManager")) {
        // The few most recently used scripts; the rest are under "All scripts".
        constexpr int kRecent = 5;
        const std::vector<int> order = RecentOrder();
        for (int k = 0; k < std::min<int>(kRecent, static_cast<int>(order.size())); ++k) {
            const int i = order[k];
            ImGui::PushID(i);
            if (ImGui::Selectable(NameAt(i).c_str(), i == CurrentIndex())) Select(i);
            ImGui::PopID();
        }
        if (static_cast<int>(order.size()) > kRecent && ImGui::BeginMenu("All scripts")) {
            for (int i = 0; i < Count(); ++i) {
                ImGui::PushID(i);
                if (ImGui::MenuItem(NameAt(i).c_str(), nullptr, i == CurrentIndex())) Select(i);
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("+ New script")) NewScript();
        if (Count()) {
            if (ImGui::BeginMenu("Rename current")) {
                if (ImGui::IsWindowAppearing()) std::snprintf(g_nameBuf, sizeof(g_nameBuf), "%s", NameAt(CurrentIndex()).c_str());
                if (ImGui::InputText("##rn", g_nameBuf, sizeof(g_nameBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                    RenameCurrent(g_nameBuf);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Delete current")) {
                const std::string key = NameAt(CurrentIndex());
                if (scene.deleteScript) scene.deleteScript(key); else RemoveCurrent();
            }
        }
        ImGui::EndPopup();
    }
}
}

void DrawEditor(const SceneListing& scene) {
    if (Count() == 0) {
        ImGui::BeginChild("##vsEmpty", ImVec2(0, 0), false);
        DrawEmptyState(scene);
        ImGui::EndChild();
        return;
    }
    Graph& g = Active();
    ImGui::BeginChild("##vsCanvas", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cp = ImGui::GetCursorScreenPos();
    const ImVec2 cs = ImGui::GetContentRegionAvail();
    const ImVec2 o(cp.x + g_pan.x, cp.y + g_pan.y);

    dl->AddRectFilled(cp, ImVec2(cp.x + cs.x, cp.y + cs.y), IM_COL32(28, 28, 32, 255));
    for (float x = std::fmod(g_pan.x, 32.0f); x < cs.x; x += 32.0f)
        dl->AddLine(ImVec2(cp.x + x, cp.y), ImVec2(cp.x + x, cp.y + cs.y), IM_COL32(255, 255, 255, 12));
    for (float y = std::fmod(g_pan.y, 32.0f); y < cs.y; y += 32.0f)
        dl->AddLine(ImVec2(cp.x, cp.y + y), ImVec2(cp.x + cs.x, cp.y + y), IM_COL32(255, 255, 255, 12));
    dl->PushClipRect(cp, ImVec2(cp.x + cs.x, cp.y + cs.y), true);

    // Background: pan, context menu.
    ImGui::SetCursorScreenPos(cp);
    ImGui::SetNextItemAllowOverlap();   // nodes and their widgets sit on top of the background
    ImGui::InvisibleButton("##vsBg", cs, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
    const bool bgHovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        g_pan.x += ImGui::GetIO().MouseDelta.x;
        g_pan.y += ImGui::GetIO().MouseDelta.y;
    }
    if (bgHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) g_selected = 0;
    // Wire under the mouse (if any): lit up, and right-click offers to delete it.
    int hoverFrom = 0, hoverTo = 0, hoverPin = 0, hoverDataPin = -1;
    if (bgHovered) {
        float best = 9.0f;
        for (const Link& l : g.links) {
            const Node* a = Find(l.from); const Node* b = Find(l.to);
            if (!a || !b) continue;
            const float dist = WireDistance(OutPin(*a, o, l.fromPin), InPin(*b, o), ImGui::GetMousePos());
            if (dist < best) { best = dist; hoverFrom = l.from; hoverTo = l.to; hoverPin = l.fromPin; }
        }
    }
    if (bgHovered && !hoverFrom) {   // value wires count only when no exec wire is under the mouse
        float best = 9.0f;
        for (const DataLink& l : g.dataLinks) {
            const Node* a = Find(l.from); const Node* b = Find(l.to);
            if (!a || !b || DataPinRow(*b, l.toPin) < 0) continue;
            const float dist = WireDistance(OutPin(*a, o), InData(*b, o, l.toPin), ImGui::GetMousePos());
            if (dist < best) { best = dist; hoverFrom = l.from; hoverTo = l.to; hoverDataPin = l.toPin; }
        }
    }
    if (bgHovered && hoverFrom && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        g_wireMenuFrom = hoverFrom; g_wireMenuTo = hoverTo; g_wireMenuPin = hoverPin; g_wireMenuToPin = hoverDataPin;
        ImGui::OpenPopup("##vsWire");
    } else if (bgHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        const ImVec2 m = ImGui::GetMousePos();
        g_menuCanvasPos = ImVec2(m.x - o.x, m.y - o.y);
        g_search[0] = '\0';
        ImGui::OpenPopup("##vsInsert");
    }

    // Wires.
    for (const Link& l : g.links) {
        const Node* a = Find(l.from); const Node* b = Find(l.to);
        const bool lit = l.from == hoverFrom && l.to == hoverTo && l.fromPin == hoverPin;
        if (a && b) DrawWire(dl, OutPin(*a, o, l.fromPin), InPin(*b, o), lit ? IM_COL32(255, 200, 60, 255) : IM_COL32(245, 245, 245, 230));
    }

    for (const DataLink& l : g.dataLinks) {
        const Node* a = Find(l.from); const Node* b = Find(l.to);
        if (!a || !b || DataPinRow(*b, l.toPin) < 0) continue;
        const bool lit = hoverDataPin == l.toPin && l.from == hoverFrom && l.to == hoverTo;
        DrawWire(dl, OutPin(*a, o), InData(*b, o, l.toPin), lit ? IM_COL32(255, 200, 60, 255) : IM_COL32(90, 200, 230, 230));
    }

    // Nodes.
    int toDelete = 0;
    int dropDataNode = 0, dropDataPin = -1;
    const ImVec2 mouse = ImGui::GetMousePos();
    int dropTarget = 0;
    for (Node& n : g.nodes) {
        const ImVec2 p0(o.x + n.x, o.y + n.y);
        const float w = NodeWidth(n), h = NodeHeight(n);
        const ImVec2 p1(p0.x + w, p0.y + h);
        dl->AddRectFilled(ImVec2(p0.x + 3, p0.y + 4), ImVec2(p1.x + 3, p1.y + 4), IM_COL32(0, 0, 0, 90), 6.0f);
        dl->AddRectFilled(p0, p1, IM_COL32(44, 44, 50, 245), 6.0f);
        dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + kHeaderH), HeaderColor(n.type), 6.0f, ImDrawFlags_RoundCornersTop);
        dl->AddRect(p0, p1, n.id == g_selected ? IM_COL32(255, 200, 60, 255) : IM_COL32(90, 90, 100, 255), 6.0f, 0, n.id == g_selected ? 2.0f : 1.0f);
        dl->AddText(ImVec2(p0.x + 24, p0.y + 4), IM_COL32_WHITE, TypeName(n.type));

        // Header: drag to move, right-click for node menu.
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 16, p0.y));
        ImGui::PushID(n.id);
        ImGui::InvisibleButton("##hdr", ImVec2(w - 32, kHeaderH));
        if (ImGui::IsItemActivated()) g_selected = n.id;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            n.x += ImGui::GetIO().MouseDelta.x;
            n.y += ImGui::GetIO().MouseDelta.y;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) { g_selected = n.id; ImGui::OpenPopup("##vsNode"); }
        if (ImGui::BeginPopup("##vsNode")) {
            if (ImGui::MenuItem("Delete node")) toDelete = n.id;
            if (ImGui::MenuItem("Break links"))
            {
                g.links.erase(std::remove_if(g.links.begin(), g.links.end(), [&](const Link& l) { return l.from == n.id || l.to == n.id; }), g.links.end());
                g.dataLinks.erase(std::remove_if(g.dataLinks.begin(), g.dataLinks.end(), [&](const DataLink& l) { return l.from == n.id || l.to == n.id; }), g.dataLinks.end());
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();

        // Pins.
        bool inLinked = false;
        for (const Link& l : g.links) if (l.to == n.id) inLinked = true;
        const bool hasInput = !IsPure(n.type) && n.type != NodeType::Start && n.type != NodeType::OnUpdate && n.type != NodeType::OnStop;
        if (hasInput) {
            DrawExecPin(dl, InPin(n, o), inLinked);
            const ImVec2 c = InPin(n, o);
            if (g_dragFrom && std::abs(mouse.x - c.x) < 16 && std::abs(mouse.y - c.y) < 14) dropTarget = n.id;
        }
        for (int pin = 0; pin < DataInputCount(n.type); ++pin) {
            if (DataPinRow(n, pin) < 0) continue;
            const ImVec2 c = InData(n, o, pin);
            DrawDataPin(dl, c, IsWired(g, n.id, pin));
            if (g_dragData && std::abs(mouse.x - c.x) < 16 && std::abs(mouse.y - c.y) < 14) { dropDataNode = n.id; dropDataPin = pin; }
        }
        if (IsPure(n.type)) {   // single value output on the header's right edge
            const ImVec2 c = OutPin(n, o);
            bool linked = false;
            for (const DataLink& d : g.dataLinks) if (d.from == n.id) linked = true;
            DrawDataPin(dl, c, linked);
            ImGui::SetCursorScreenPos(ImVec2(c.x - 10, c.y - 10));
            ImGui::PushID(n.id + 100000);
            ImGui::InvisibleButton("##dout", ImVec2(20, 20));
            if (ImGui::IsItemActivated()) g_dragData = n.id;
            ImGui::PopID();
        }
        for (int pin = 0; pin < OutputCount(n.type, &n); ++pin) {
            bool pinLinked = false;
            for (const Link& l : g.links) if (l.from == n.id && l.fromPin == pin) pinLinked = true;
            const ImVec2 c = OutPin(n, o, pin);
            DrawExecPin(dl, c, pinLinked);
            if (MultiOut(n)) {
                const char* label = OutputLabel(n, pin);
                dl->AddText(ImVec2(c.x - 16 - ImGui::CalcTextSize(label).x, c.y - 7), IM_COL32(220, 220, 220, 255), label);
            }
            ImGui::SetCursorScreenPos(ImVec2(c.x - 10, c.y - 10));
            ImGui::PushID(n.id + 100000 + pin * 1000000);
            ImGui::InvisibleButton("##out", ImVec2(20, 20));
            if (ImGui::IsItemActivated()) { g_dragFrom = n.id; g_dragPin = pin; }
            ImGui::PopID();
        }
        DrawNodeBody(n, o, scene);
    }

    if (g_dragFrom) {
        if (const Node* a = Find(g_dragFrom)) DrawWire(dl, OutPin(*a, o, g_dragPin), mouse, IM_COL32(255, 200, 60, 255));
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (dropTarget) Connect(g_dragFrom, dropTarget, g_dragPin);
            g_dragFrom = 0;
        }
    }
    if (g_dragData) {
        if (const Node* a = Find(g_dragData)) DrawWire(dl, OutPin(*a, o), mouse, IM_COL32(90, 200, 230, 255));
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (dropDataNode) ConnectData(g_dragData, dropDataNode, dropDataPin);
            g_dragData = 0;
        }
    }
    if (toDelete) { RemoveNode(toDelete); if (g_selected == toDelete) g_selected = 0; }
    if (g_selected && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        RemoveNode(g_selected);
        g_selected = 0;
    }
    dl->PopClipRect();

    if (ImGui::BeginPopup("##vsWire")) {
        if (ImGui::MenuItem("Delete link")) {
            if (g_wireMenuToPin >= 0)
                g.dataLinks.erase(std::remove_if(g.dataLinks.begin(), g.dataLinks.end(),
                    [&](const DataLink& l) { return l.from == g_wireMenuFrom && l.to == g_wireMenuTo && l.toPin == g_wireMenuToPin; }), g.dataLinks.end());
            else
                g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
                    [&](const Link& l) { return l.from == g_wireMenuFrom && l.to == g_wireMenuTo && l.fromPin == g_wireMenuPin; }), g.links.end());
        }
        ImGui::EndPopup();
    }

    // Insert menu: Blender style, a search box plus a submenu.
    if (ImGui::BeginPopup("##vsInsert")) {
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##search", "Search...", g_search, sizeof(g_search));
        ImGui::Separator();
        struct Category { const char* name; std::vector<NodeType> types; };
        const std::vector<Category> categories = {
            { "Events", { NodeType::Start, NodeType::OnUpdate, NodeType::OnStop } },
            { "Flow", { NodeType::Branch, NodeType::Sequence, NodeType::Delay, NodeType::Repeat, NodeType::DoOnce, NodeType::End } },
            { "Actions", { NodeType::Change, NodeType::Print } },
            { "Variables", { NodeType::SetVariable, NodeType::GetVariable, NodeType::GetProperty } },
            { "Math", { NodeType::Add, NodeType::Sub, NodeType::Mul, NodeType::Div, NodeType::Random, NodeType::Lerp } },
            { "Logic", { NodeType::Compare, NodeType::And, NodeType::Or, NodeType::Not } },
        };
        auto matches = [](const char* name) {
            if (!g_search[0]) return true;
            std::string a = name, b = g_search;
            std::transform(a.begin(), a.end(), a.begin(), ::tolower);
            std::transform(b.begin(), b.end(), b.begin(), ::tolower);
            return a.find(b) != std::string::npos;
        };
        auto add = [&](NodeType t) { AddNode(t, g_menuCanvasPos.x, g_menuCanvasPos.y); ImGui::CloseCurrentPopup(); };
        if (g_search[0]) {
            for (const auto& c : categories)
                for (NodeType t : c.types) if (matches(TypeName(t)) && ImGui::MenuItem(TypeName(t))) add(t);
        } else if (ImGui::BeginMenu("Insert Node")) {
            for (const auto& c : categories) {
                if (!ImGui::BeginMenu(c.name)) continue;
                for (NodeType t : c.types) if (ImGui::MenuItem(TypeName(t))) add(t);
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    DrawScriptManager(scene);
    ImGui::EndChild();
}

} // namespace vscript
