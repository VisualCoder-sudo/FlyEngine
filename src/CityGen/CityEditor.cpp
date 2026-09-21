#include "../../include/CityGen/CityEditor.hpp"
#include "../../include/CityGen/City.hpp"
#include "../../include/CityGen/CityGeometry.hpp"
#include "../../include/Engine/Frontend/ui.hpp"
#include "../../include/Engine/Graphics.hpp"
#include "raymath.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace city {

namespace {

bool g_clickConsumed = false;

Vector2 RayGroundPoint(const Ray& ray) {
    if (fabsf(ray.direction.y) < 1e-4f) return { 1e9f, 1e9f };
    float t = -ray.position.y / ray.direction.y;
    if (t < 0.0f) return { 1e9f, 1e9f };
    return { ray.position.x + ray.direction.x * t, ray.position.z + ray.direction.z * t };
}

bool IsValidPoint(const Vector2& p) {
    return fabsf(p.x) < 1e8f && fabsf(p.y) < 1e8f;
}

Vector2 CityCenter(const City* c) {
    Vector2 sum{};
    const auto& nodes = c->GetNodes();
    if (nodes.empty()) return sum;
    for (const auto& n : nodes) sum = Vector2Add(sum, n.pos);
    return Vector2Scale(sum, 1.0f / (float)nodes.size());
}

} // namespace

CityEditorState& GetCityEditorState() {
    static CityEditorState s;
    return s;
}

void InitCityEditor() {
    GetCityEditorState() = CityEditorState{};
}

void SetActiveCity(City* c) {
    auto& s = GetCityEditorState();
    s.activeCity = c;
    s.selectedNode = -1;
    s.selectedEdge = -1;
    s.draggingNode = false;
    s.dragNode = -1;
    if (c) s.prevParams = c->GetParams();
    else s.prevParams = CityParams{};
}

bool IsCityEditorActive() {
    return GetCityEditorState().activeCity != nullptr;
}

// ---- Road tool (runtime): press R to toggle "place road node" mode. ----
// While the tool is on, left-clicking a parcel interior in the viewport adds a
// new RoadNode there (City::AddNode) and auto-routes a RoadEdge to the nearest
// existing node or edge so the fresh road connects to whatever is already there.
static bool g_placeRoadNode = false;
static Vector2 g_prevRoadNode{ 0.0f, 0.0f };
static bool g_havePrevRoadNode = false; // unused; reserved for chaining

bool UpdateCityEditor(Engine& engine, Camera3D& camera) {

    (void)engine;
    auto& s = GetCityEditorState();
    if (IsKeyPressed(KEY_R)) { g_placeRoadNode = !g_placeRoadNode; g_clickConsumed = true; return true; }
    g_clickConsumed = false;
    if (ui::IsPlayActive()) return false;

    City* city = s.activeCity;
    if (!city || !city->alive) {
        if (city && !city->alive) SetActiveCity(nullptr);
        return false;
    }

    const float roadW = city->GetParams().RoadWidth();
    const float nodeR = std::max(roadW * 0.35f, 0.5f);

    // Hover highlight (visual only; twice the pick radius so it feels softer).
    s.hoveredNode = -1;
    if (!ui::IsMouseOverUI()) {
        Ray hoverRay = GetMouseRay(GetMousePosition(), camera);
        s.hoveredNode = city->PickNode(hoverRay, nodeR * 2.0f);
    }

    const bool lPressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    const bool lDown = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    const bool lReleased = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);

    // --- Active node drag ---
    if (s.draggingNode) {
        g_clickConsumed = true;
        if (lDown) {
            Ray ray = GetMouseRay(GetMousePosition(), camera);
            Vector2 pos = RayGroundPoint(ray);
            if (IsValidPoint(pos)) {
                // Keep nodes at least one road width apart; closer than that the
                // junction plates/strips of neighbouring nodes overlap and glitch.
                const float minSep = city->GetParams().RoadWidth();
                const auto& ns = city->GetNodes();
                for (int k = 0; k < (int)ns.size(); k++) {
                    if (k == s.dragNode) continue;
                    Vector2 dv = Vector2Subtract(pos, ns[(size_t)k].pos);
                    const float dl = Vector2Length(dv);
                    if (dl < minSep)
                        pos = Vector2Add(ns[(size_t)k].pos,
                                         dl > 1e-4f ? Vector2Scale(dv, minSep / dl) : Vector2{ minSep, 0.0f });
                }
                Vector2 cur = city->NodePos(s.dragNode);
                if (Vector2Distance(pos, cur) > 0.02f) {
                    // First actual move: capture the pre-edit state on the undo stack.
                    if (!s.dragMoved) {
                        s.dragMoved = true;
                        NotifyCityEdit();
                    }
                    // Incremental rebuild: only the roads, blocks and buildings that
                    // depend on this node are regenerated (identical to a full
                    // rebuild, ~constant cost regardless of city size), so it can
                    // run on every mouse move.
                    city->MoveNode(s.dragNode, pos, true);
                }
            }
        }
        if (lReleased || !lDown) {
            s.draggingNode = false;
            s.dragNode = -1;
        }
        return g_clickConsumed;
    }

    // --- Viewport click handling ---
    if (lPressed && !ui::IsMouseOverUI()) {
        Ray ray = GetMouseRay(GetMousePosition(), camera);

        int node = city->PickNode(ray, nodeR);
        if (node >= 0) {
            s.selectedNode = node;
            s.selectedEdge = -1;
            s.draggingNode = true;
            s.dragNode = node;
            s.dragMoved = false;
            g_clickConsumed = true;
            return true;
        }

        int edge = city->PickRoad(ray, roadW);
        if (edge >= 0) {
            s.selectedEdge = edge;
            s.selectedNode = -1;
            g_clickConsumed = true;
            return true;
        }

        if (g_placeRoadNode) {
            const Vector2 hit = RayGroundPoint(ray);
            if (IsValidPoint(hit)) {
                NotifyCityEdit();
                const int n = city->AddNodeConnected(hit);
                s.selectedNode = n; s.selectedEdge = -1;
                g_clickConsumed = true;
                return true;
            }
        }
        Vector2 hit = RayGroundPoint(ray);
        if (IsValidPoint(hit) && city->ContainsPoint(hit)) {
            s.selectedNode = -1;
            s.selectedEdge = -1;
            g_clickConsumed = true;
            return true;
        }
    }

    // --- Keyboard nudging of the selected node (arrow keys) ---
    if (s.selectedNode >= 0 && !ui::IsEditingText() && !ui::IsMouseOverUI()) {
        Vector2 d{};
        if (IsKeyPressed(KEY_LEFT))  d = Vector2{ -s.nudgeStep, 0.0f };
        if (IsKeyPressed(KEY_RIGHT)) d = Vector2{ s.nudgeStep, 0.0f };
        if (IsKeyPressed(KEY_UP))    d = Vector2{ 0.0f, s.nudgeStep };
        if (IsKeyPressed(KEY_DOWN))  d = Vector2{ 0.0f, -s.nudgeStep };
        if (d.x != 0.0f || d.y != 0.0f) {
            if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))
                d = Vector2Scale(d, 0.25f);
            NotifyCityEdit();
            city->MoveNode(s.selectedNode, Vector2Add(city->NodePos(s.selectedNode), d));
        }
    }

    return g_clickConsumed;
}

// ---------------------------------------------------------------------------
// ImGui panel
// ---------------------------------------------------------------------------
void DrawCityEditorPanel() {
    auto& s = GetCityEditorState();
    if (!s.panelOpen) {
        if (s.activeCity) s.prevParams = s.activeCity->GetParams();
        return;
    }

    ImGui::SetNextWindowPos(ImVec2((float)GetScreenWidth() - 400.0f, 150.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330.0f, 0.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("City Editor", &s.panelOpen)) {
        ImGui::End();
        return;
    }

    auto& reg = GetCityRegistry();
    if (s.activeCity && !s.activeCity->alive) SetActiveCity(nullptr);

    // --- City list ---
    ImGui::SeparatorText("Cities");
    if (reg.Count() > 0) {
        // Iterate in reverse so Unregister (which erases from the same vector)
        // never invalidates the next index/iterator.
        for (int i = (int)reg.GetCities().size() - 1; i >= 0; --i) {
            City* c = reg.GetCities()[(size_t)i];
            if (!c) continue;
            bool sel = (s.activeCity == c);
            ImGui::PushID((const void*)c);
            // The name selectable must leave room for the X button: a full-width
            // Selectable owns the hit area under the button, so clicks on X used
            // to just select the city instead of deleting it.
            const ImGuiStyle& st = ImGui::GetStyle();
            const float btnW = ImGui::CalcTextSize("X").x + st.FramePadding.x * 2.0f;
            const float nameW = std::max(ImGui::GetContentRegionAvail().x - btnW - st.ItemSpacing.x, 1.0f);
            if (ImGui::Selectable(c->GetName().c_str(), sel, 0, ImVec2(nameW, 0.0f))) {
                if (!sel) {
                    ui::LogAlways("[city] Selected %s", c->GetName().c_str());
                    SetActiveCity(c);
                }
            }
            ImGui::SameLine();
            const bool del = ImGui::SmallButton("X");
            ImGui::PopID();
            if (del) {
                NotifyCityEdit();
                c->alive = false;   // engine sweeps dead entities at end of Update
                reg.Unregister(c);
                SetActiveCity(nullptr);
                break;              // registry changed; leave the loop
            }
        }
    } else {
        ImGui::TextDisabled("No cities yet (top bar > City)");
    }

    City* city = s.activeCity;
    if (!city || !city->alive) {
        SetActiveCity(nullptr);
        ImGui::End();
        return;
    }

    ImGui::SeparatorText(city->GetName().c_str());

    char nameBuf[256];
    snprintf(nameBuf, sizeof(nameBuf), "%s", city->GetName().c_str());
    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
        city->SetName(nameBuf);
    }

    CityParams& p = city->GetParams();

    // --- Layout params ---
    // Apply is driven by a prevParams comparison at the bottom of the panel,
    // so the widgets only need to write into City::params directly.
    ImGui::PushID("layout");
    ImGui::DragInt("Grid X", &p.gridX, 1.0f, 2, 999);
    ImGui::DragInt("Grid Z", &p.gridZ, 1.0f, 2, 999);
    ImGui::DragFloat("Cell size", &p.cellSize, 0.5f, 5.0f, 120.0f);
    ImGui::Checkbox("Organic", &p.organic);
    ImGui::BeginDisabled(!p.organic);
    ImGui::DragFloat("Organic warp", &p.organicStrength, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Organic scale", &p.organicScale, 0.5f, 8.0f, 200.0f);
    ImGui::DragInt("Noise octaves", &p.noiseOctaves, 0.1f, 1, 6);
    ImGui::EndDisabled();
    ImGui::PopID();

    // --- Style params ---
    ImGui::PushID("style");
    ImGui::DragInt("Road lanes", &p.lanes, 0.1f, 1, 8);
    ImGui::DragFloat("Lane width", &p.laneWidth, 0.1f, 2.0f, 6.0f);
    ImGui::DragFloat("Sidewalk", &p.sidewalk, 0.1f, 0.0f, 5.0f);
    ImGui::DragFloat("Corner radius", &p.cornerRadius, 0.1f, 0.0f, 10.0f);
    ImGui::DragFloat("Avg height", &p.avgHeight, 0.5f, 2.0f, 160.0f);
    ImGui::DragFloat("Height variance", &p.heightVariance, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Building size", &p.buildingSize, 0.25f, 3.0f, 30.0f);
    ImGui::DragFloat("Building gap", &p.buildingGap, 0.1f, 0.0f, 8.0f);
    ImGui::DragFloat("Park threshold", &p.parkThreshold, 50.0f, 0.0f, 20000.0f);
    ImGui::DragFloat("Park inset", &p.parkInset, 0.25f, 0.0f, 12.0f);
    ImGui::DragInt("Seed", &p.seed, 1.0f, 1, 9999999);
    ImGui::PopID();

    ImGui::TextDisabled("Drag values to apply live; one undo step per widget.");

    if (ImGui::Button("Reroll with a new random seed")) {
        p.seed = ((int)(GetTime() * 997.0f)) % 90000 + 1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults")) {
        CityParams fresh;
        p = fresh;
    }

    // --- Node editing ---
    if (city && s.selectedNode >= 0 && (size_t)s.selectedNode < city->GetNodes().size()) {
        ImGui::SeparatorText("Road Node##panel");
        Vector2 v = city->NodePos(s.selectedNode);
        ImGui::Text("Node %d  %s", s.selectedNode,
                    city->GetNodes()[s.selectedNode].boundary ? "boundary" : "interior");
        bool edited = false;
        edited |= ImGui::DragFloat("Node X", &v.x, 0.1f);
        edited |= ImGui::DragFloat("Node Z", &v.y, 0.1f);
        if (edited) {
            if (!s.dragMoved) { s.dragMoved = true; NotifyCityEdit(); }
            city->MoveNode(s.selectedNode, v, true); // incremental rebuild
        }
        if (ImGui::Button("Delete node")) {
            NotifyCityEdit();
            city->DeleteNode(s.selectedNode);
            s.selectedNode = -1;
            s.selectedEdge = -1;
        }
        ImGui::TextDisabled("Click a node in the viewport to drag it; arrows nudge");
    } else {
        ImGui::SeparatorText("Road Node##panel");
        ImGui::TextDisabled("Click a road node in the viewport to select it.");
    }

    // --- Edge editing ---
    if (s.selectedEdge >= 0 && (size_t)s.selectedEdge < city->GetEdges().size()) {
        ImGui::SeparatorText("Road Edge##panel");
        ImGui::Text("Edge %d (nodes %d-%d)", s.selectedEdge,
                    city->GetEdges()[s.selectedEdge].a, city->GetEdges()[s.selectedEdge].b);
        int lanes = city->GetEdges()[s.selectedEdge].lanes;
        if (ImGui::DragInt("Lane override", &lanes, 0.1f, 0, 8)) {
            city->SetEdgeLaneOverride(s.selectedEdge, lanes);
        }
        ImGui::TextDisabled("0 = follow the global road lanes");
        if (ImGui::Button("Split road here")) {
            Vector2 mid = city->EdgeMidpoint(s.selectedEdge);
            NotifyCityEdit();
            int n = city->InsertNodeOnEdge(s.selectedEdge, mid);
            s.selectedEdge = -1;
            if (n >= 0) s.selectedNode = n;
        }
    }

    // --- Stats ---
    int buildingCount = 0;
    for (const auto& b : city->GetBlocks()) buildingCount += (int)b.buildings.size();
    ImGui::SeparatorText("Stats");
    ImGui::Text("Nodes: %zu   Roads: %zu   Blocks: %zu", city->GetNodes().size(),
                city->GetEdges().size(), city->GetBlocks().size());
    ImGui::Text("Buildings: %d", buildingCount);
    if (city->IsRebuilding()) ImGui::TextDisabled("Rebuilding in background...");

    ImGui::SeparatorText("Performance");
    {
        bool persistent = gfx::GetInstanceBuffersEnabled();
        if (ImGui::Checkbox("GPU instance buffers", &persistent)) gfx::SetInstanceBuffersEnabled(persistent);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Upload building transforms once and draw from GPU buffers.\nTurn off if buildings render wrong on your GPU.");
        bool reuse = gfx::GetShadowReuseEnabled();
        if (ImGui::Checkbox("Reuse shadow map when idle", &reuse)) gfx::SetShadowReuseEnabled(reuse);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Skip re-rendering shadows while nothing changes (no input, not playing).\nTurn off if shadows ever look stale.");
    }

    // --- Live apply of param edits (throttled to once per frame) ---
    CityParams applied = city->GetParams();
    bool layoutChanged = (s.prevParams.gridX != applied.gridX || s.prevParams.gridZ != applied.gridZ ||
                          s.prevParams.cellSize != applied.cellSize || s.prevParams.organic != applied.organic ||
                          s.prevParams.organicStrength != applied.organicStrength ||
                          s.prevParams.organicScale != applied.organicScale ||
                          s.prevParams.noiseOctaves != applied.noiseOctaves ||
                          s.prevParams.seed != applied.seed);
    bool styleChanged = !layoutChanged && s.prevParams != applied;

    static bool wasItemActive = false;
    const bool nowItemActive = ImGui::IsAnyItemActive();
    const bool editJustEnded = wasItemActive && !nowItemActive;
    wasItemActive = nowItemActive;

    if (layoutChanged) {
        Vector2 center = CityCenter(city);
        city->GenerateGrid(center);
        s.prevParams = city->GetParams();
        s.selectedNode = -1;
        s.selectedEdge = -1;
    } else if (styleChanged) {
        city->RebuildAll();
        s.prevParams = city->GetParams();
    }

    if ((layoutChanged || styleChanged) && editJustEnded) {
        NotifyCityEdit();
    }
    if (editJustEnded) {
        s.dragMoved = false;
    }

    ImGui::End();
}

} // namespace city