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

Vector2 RayGroundPoint(const Ray& ray, float planeY = 0.0f) {
    if (fabsf(ray.direction.y) < 1e-4f) return { 1e9f, 1e9f };
    float t = (planeY - ray.position.y) / ray.direction.y;
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
    s.selectedBlockId = 0;
    s.selectedBuildingSlot = -1;
    s.draggingNode = false;
    s.dragNode = -1;
    if (c) s.prevParams = c->GetParams();
    else s.prevParams = CityParams{};
}

bool IsCityEditorActive() {
    return GetCityEditorState().activeCity != nullptr;
}

// Press R to toggle the road draw tool (see CityEditorState::roadActive).

bool UpdateCityEditor(Engine& engine, Camera3D& camera) {

    (void)engine;
    auto& s = GetCityEditorState();
    // Closing the panel (X) drops any active tool so clicks don't keep drawing roads etc.
    if (!s.panelOpen && s.tool != CityTool::Select) {
        s.tool = CityTool::Select;
        s.roadActive = s.roadDragging = s.roadHasHandle = false;
        s.painting = false;
        s.hoveredBlock = -1;
        s.insertHasPreview = false;
        s.elevDragging = false;
    }
    if (IsKeyPressed(KEY_R) && s.panelOpen && !ui::IsEditingText()) {
        s.tool = s.tool == CityTool::DrawRoad ? CityTool::Select : CityTool::DrawRoad;
        s.roadActive = s.roadDragging = false;
        s.painting = false;
        g_clickConsumed = true;
        return true;
    }
    g_clickConsumed = false;
    if (ui::IsPlayActive()) return false;
    // Panel closed: the city is read-only (no picking, dragging, nudging or tools).
    if (!s.panelOpen) {
        s.draggingNode = false;
        s.elevDragging = false;
        s.hoveredNode = -1;
        return false;
    }

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

    // --- Block paint tool: click or drag across blocks to set their land use ---
    s.hoveredBlock = -1;
    if (s.tool == CityTool::PaintBlock) {
        if (!ui::IsMouseOverUI()) {
            Ray pr = GetMouseRay(GetMousePosition(), camera);
            const Vector2 hit = RayGroundPoint(pr);
            if (IsValidPoint(hit)) s.hoveredBlock = city->PickBlockAt(hit);
        }
        if (s.painting && !lDown) s.painting = false;
        if ((lPressed && !ui::IsMouseOverUI()) || (s.painting && lDown)) {
            if (lPressed) { s.painting = true; s.paintUndoPushed = false; }
            if (s.hoveredBlock >= 0) {
                const uint64_t id = city->GetBlocks()[(size_t)s.hoveredBlock].id;
                if (city->GetBlockKind(id) != s.paintKind) {
                    if (!s.paintUndoPushed) { s.paintUndoPushed = true; NotifyCityEdit(); } // one undo step per stroke
                    city->SetBlockKind(id, s.paintKind);
                }
            }
            return true;
        }
        if (lReleased) return true;
        return false;
    }

    // --- Transit tool ---
    if (s.tool == CityTool::Transit) {
        const bool over = ui::IsMouseOverUI();
        Ray tr = GetMouseRay(GetMousePosition(), camera);
        const Vector2 hit = RayGroundPoint(tr);
        if (s.transitStop >= (int)city->GetBusStops().size()) s.transitStop = -1;
        if (s.transitLine >= (int)city->GetBusLines().size()) s.transitLine = -1;
        if (lPressed && !over) {
            const int pick = city->PickBusStop(tr);
            const bool toLine = s.transitAddToLine && s.transitLine >= 0;
            if (pick >= 0) {
                s.transitStop = pick;
                if (toLine) { NotifyCityEdit(); city->AddStopToLine(s.transitLine, pick); }
            } else if (IsValidPoint(hit)) {
                Vector2 pos, heading;
                if (city->SnapBusStop(hit, pos, heading)) {
                    NotifyCityEdit();
                    const int idx = city->AddBusStop(pos, heading);
                    s.transitStop = idx;
                    if (idx >= 0 && toLine) city->AddStopToLine(s.transitLine, idx);
                }
            }
            return true;
        }
        if (IsKeyPressed(KEY_DELETE) && s.transitStop >= 0 && !ui::IsEditingText()) {
            NotifyCityEdit();
            city->RemoveBusStop(s.transitStop);
            s.transitStop = -1;
            return true;
        }
        return lReleased;
    }

    // --- District tool ---
    if (s.tool == CityTool::Districts) {
        const bool over = ui::IsMouseOverUI();
        Ray dr = GetMouseRay(GetMousePosition(), camera);
        const Vector2 hit = RayGroundPoint(dr);
        const bool valid = IsValidPoint(hit);
        if (s.districtSel >= (int)city->GetDistricts().size()) s.districtSel = -1;
        if (s.districtPaintMode) {
            if (!over && valid) s.hoveredBlock = city->PickBlockAt(hit);
            if (s.painting && !lDown) s.painting = false;
            if ((lPressed && !over) || (s.painting && lDown)) {
                if (lPressed) { s.painting = true; s.paintUndoPushed = false; }
                if (s.hoveredBlock >= 0 && s.districtSel >= 0) {
                    const uint64_t id = city->GetBlocks()[(size_t)s.hoveredBlock].id;
                    if (city->GetBlockDistrict(id) != s.districtSel) {
                        if (!s.paintUndoPushed) { s.paintUndoPushed = true; NotifyCityEdit(); }
                        city->PaintBlockDistrict(id, s.districtSel);
                    }
                }
                return true;
            }
            return lReleased;
        }
        if (s.districtDragging) {
            if (lDown && s.districtSel >= 0 && (size_t)s.districtSel < city->GetDistricts().size()) {
                // Drag on the horizontal plane through the marker's top so it follows the cursor 1:1.
                const float top = city->DistrictMarkerHeight(city->GetDistricts()[(size_t)s.districtSel]);
                const Vector2 onPlane = RayGroundPoint(dr, top);
                if (IsValidPoint(onPlane)) {
                    const Vector2 target = Vector2Add(onPlane, s.districtDragOffset);
                    if (Vector2Distance(target, s.districtDragLast) > 0.75f) {
                        s.districtDragLast = target;
                        city->MoveDistrict(s.districtSel, target);
                    }
                }
            }
            if (!lDown) s.districtDragging = false;
            return true;
        }
        if (lPressed && !over) {
            // Grab the marker pillar under the cursor, else place a new district on the ground.
            const int pick = city->PickDistrict(dr);
            if (pick >= 0) {
                const auto& ds = city->GetDistricts();
                const Vector2 onPlane = RayGroundPoint(dr, city->DistrictMarkerHeight(ds[(size_t)pick]));
                s.districtSel = pick;
                s.districtDragging = true;
                s.districtDragOffset = IsValidPoint(onPlane) ? Vector2Subtract(ds[(size_t)pick].pos, onPlane) : Vector2{ 0.0f, 0.0f };
                s.districtDragLast = ds[(size_t)pick].pos;
                NotifyCityEdit();
            } else if (valid) {
                NotifyCityEdit();
                s.districtSel = city->AddDistrict(City::MakeDistrict((DistrictKind)s.districtNewKind, hit));
            }
            return true;
        }
        return lReleased;
    }

    // --- Road draw tool ---
    s.roadPreview.clear();
    s.roadSnapValid = false;
    if (s.tool == CityTool::DrawRoad) {
        const bool over = ui::IsMouseOverUI();
        if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            s.roadActive = s.roadDragging = s.roadHasHandle = false;
        }
        if (!ui::IsEditingText()) {
            const float step = (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? 0.1f : 0.5f;
            if (IsKeyPressed(KEY_PAGE_UP)) s.roadHeight += step;
            if (IsKeyPressed(KEY_PAGE_DOWN)) s.roadHeight -= step;
            if (IsKeyPressed(KEY_HOME)) s.roadHeight = 0.0f;
            s.roadHeight = Clamp(s.roadHeight, -200.0f, 500.0f);
        }
        Ray rr = GetMouseRay(GetMousePosition(), camera);
        const Vector2 hit = over ? Vector2{ 1e9f, 1e9f } : RayGroundPoint(rr, s.roadHeight);
        const bool valid = IsValidPoint(hit);
        const bool erase = s.roadErase || IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

        if (erase) {
            s.roadActive = s.roadDragging = false;
            if (lPressed && !over) {
                const int e = city->PickRoad(rr, roadW);
                if (e >= 0) {
                    NotifyCityEdit();
                    city->DeleteRoadChain(e);
                    s.selectedNode = s.selectedEdge = -1;
                }
                return true;
            }
            return false;
        }

        City::RoadSnap snap;
        if (valid) {
            snap = city->SnapRoadPoint(hit);
            s.roadSnapPos = snap.pos;
            s.roadSnapValid = true;
            s.roadEndH = snap.node >= 0 ? city->GetNodes()[(size_t)snap.node].h
                       : snap.edge >= 0 ? city->EdgeProfileY(snap.edge, 0.5f) : s.roadHeight;
        }
        const float spacing = roadW * 1.5f;

        if (s.roadDragging) {
            if (lDown) {
                if (valid && Vector2Distance(hit, s.roadStart) > 2.0f) { s.roadHandle = hit; s.roadHasHandle = true; }
                else s.roadHasHandle = false;
            } else {
                s.roadDragging = false;
            }
            return true;
        }
        if (!s.roadActive) {
            if (lPressed && !over && valid) {
                s.roadActive = true;
                s.roadStart = snap.pos;
                s.roadStartH = snap.node >= 0 ? city->GetNodes()[(size_t)snap.node].h
                             : snap.edge >= 0 ? city->EdgeProfileY(snap.edge, 0.5f) : s.roadHeight;
                s.roadHasHandle = false;
                s.roadDragging = true;
                return true;
            }
            return false;
        }
        if (valid) {
            const Vector2 end = snap.pos;
            const Vector2 c = s.roadHasHandle ? s.roadHandle : Vector2Scale(Vector2Add(s.roadStart, end), 0.5f);
            s.roadPreview = citygeom::SampleQuadBezier(s.roadStart, c, end, spacing);
            s.roadPreviewOk = Vector2Distance(s.roadStart, end) >= roadW;
            if (lPressed && !over && s.roadPreviewOk) {
                NotifyCityEdit();
                const int endNode = city->AddRoadPath(s.roadPreview, s.roadStartH, s.roadHeight);
                // Chain: the end is the next start, continuing the curve smoothly.
                s.roadHandle = s.roadHasHandle ? Vector2Add(end, Vector2Subtract(end, c)) : end;
                s.roadStart = end;
                if (endNode >= 0 && (size_t)endNode < city->GetNodes().size()) s.roadStartH = city->GetNodes()[(size_t)endNode].h;
                return true;
            }
        }
        if (lPressed && !over) return true;
        return false;
    }

    // --- Road elevate tool: drag a node up/down to set its height ---
    if (s.tool == CityTool::ElevateRoad) {
        const bool over = ui::IsMouseOverUI();
        Ray er = GetMouseRay(GetMousePosition(), camera);
        if (s.elevDragging) {
            if (lDown && s.elevNode >= 0 && (size_t)s.elevNode < city->GetNodes().size()) {
                const bool fine = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                const bool snap = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
                float h = s.elevStartH + (s.elevMouseY - GetMousePosition().y) * (fine ? 0.01f : 0.06f);
                if (snap) h = roundf(h * 2.0f) * 0.5f;
                h = Clamp(h, -200.0f, 500.0f);
                if (fabsf(h - city->GetNodes()[(size_t)s.elevNode].h) > 1e-3f) {
                    if (!s.elevMoved) { s.elevMoved = true; NotifyCityEdit(); }
                    city->GetNodes()[(size_t)s.elevNode].h = h;
                    city->RebuildAfterNodeMove(s.elevNode);
                }
            } else {
                s.elevDragging = false;
                s.elevNode = -1;
            }
            return true;
        }
        s.hoveredNode = over ? -1 : city->PickNode(er, nodeR);
        if (lPressed && !over) {
            if (s.hoveredNode >= 0) {
                s.selectedNode = s.hoveredNode;
                s.selectedEdge = -1;
                s.elevDragging = true;
                s.elevNode = s.hoveredNode;
                s.elevStartH = city->GetNodes()[(size_t)s.hoveredNode].h;
                s.elevMouseY = GetMousePosition().y;
                s.elevMoved = false;
                return true;
            }
            const int edge = city->PickRoad(er, roadW);
            if (edge >= 0) {
                s.selectedEdge = edge;
                s.selectedNode = -1;
                return true;
            }
            return true;
        }
        return false;
    }

    // --- Building insert tool ---
    s.insertHasPreview = false;
    if (s.tool == CityTool::InsertBuilding) {
        const bool over = ui::IsMouseOverUI();
        if (!ui::IsEditingText()) {
            if (IsKeyPressed(KEY_Q)) s.insertAngle -= 15.0f * DEG2RAD;
            if (IsKeyPressed(KEY_E)) s.insertAngle += 15.0f * DEG2RAD;
        }
        Ray ir = GetMouseRay(GetMousePosition(), camera);
        const Vector2 hit = over ? Vector2{ 1e9f, 1e9f } : RayGroundPoint(ir);
        if (IsValidPoint(hit)) {
            PlacedBuilding pb;
            pb.sizeX = s.insertW; pb.sizeZ = s.insertD; pb.height = s.insertH;
            pb.colorBucket = (int)city->GetPlacedBuildings().size() * 5 + 3;
            pb.free = s.insertNoCollision;
            pb.shape = s.insertShape;
            const bool snapOn = s.insertSnap != (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT));
            if (!snapOn || !city->SnapBuildingToRoad(hit, pb)) { pb.center = hit; pb.angleY = s.insertAngle; }
            s.insertPreview = pb;
            s.insertHasPreview = true;
            s.insertPreviewOk = pb.free || city->CanPlaceBuilding(pb);
            if (lPressed && s.insertPreviewOk) {
                NotifyCityEdit();
                city->AddPlacedBuilding(pb);
                return true;
            }
        }
        if (lPressed && !over) return true;
        return false;
    }

    // --- Active node drag ---
    if (s.draggingNode) {
        g_clickConsumed = true;
        if (lDown) {
            Ray ray = GetMouseRay(GetMousePosition(), camera);
            Vector2 pos = RayGroundPoint(ray, city->GetNodes()[(size_t)s.dragNode].h);
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
                s.dragBlocked = false;
                if (city->MoveWouldCross(s.dragNode, pos)) {
                    s.dragBlocked = true;
                    // Slide as far toward the cursor as roads allow instead of crossing one.
                    float lo = 0.0f, hi = 1.0f;
                    for (int k = 0; k < 8; k++) {
                        const float mid = 0.5f * (lo + hi);
                        if (city->MoveWouldCross(s.dragNode, Vector2Lerp(cur, pos, mid))) hi = mid; else lo = mid;
                    }
                    pos = Vector2Lerp(cur, pos, lo);
                }
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
            s.selectedBlockId = 0; s.selectedBuildingSlot = -1;
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
            s.selectedBlockId = 0; s.selectedBuildingSlot = -1;
            g_clickConsumed = true;
            return true;
        }

        {
            int blockIdx = -1, slot = -1;
            if (city->PickBuilding(ray, blockIdx, slot)) {
                s.selectedBlockId = city->GetBlocks()[(size_t)blockIdx].id;
                s.selectedBuildingSlot = slot;
                s.selectedNode = -1;
                s.selectedEdge = -1;
                g_clickConsumed = true;
                return true;
            }
        }

        Vector2 hit = RayGroundPoint(ray);
        if (IsValidPoint(hit) && city->ContainsPoint(hit)) {
            s.selectedNode = -1;
            s.selectedEdge = -1;
            s.selectedBlockId = 0; s.selectedBuildingSlot = -1;
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
            const Vector2 target = Vector2Add(city->NodePos(s.selectedNode), d);
            if (!city->MoveWouldCross(s.selectedNode, target)) {
                NotifyCityEdit();
                city->MoveNode(s.selectedNode, target);
            }
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

    // --- Tools ---
    ImGui::SeparatorText("Tools");
    {
        int tool = (int)s.tool;
        ImGui::RadioButton("Select", &tool, (int)CityTool::Select); ImGui::SameLine();
        ImGui::RadioButton("Paint block", &tool, (int)CityTool::PaintBlock); ImGui::SameLine();
        ImGui::RadioButton("Draw road (R)", &tool, (int)CityTool::DrawRoad);
        ImGui::RadioButton("Insert building", &tool, (int)CityTool::InsertBuilding); ImGui::SameLine();
        ImGui::RadioButton("Elevate road", &tool, (int)CityTool::ElevateRoad); ImGui::SameLine();
        ImGui::RadioButton("Districts", &tool, (int)CityTool::Districts);
        ImGui::RadioButton("Transit", &tool, (int)CityTool::Transit);
        if (tool != (int)s.tool) {
            s.tool = (CityTool)tool;
            s.painting = false; s.draggingNode = false; s.elevDragging = false;
            s.roadActive = s.roadDragging = s.roadHasHandle = false;
        }
        if (s.tool == CityTool::InsertBuilding) {
            ImGui::Checkbox("Snap to roads (hold Alt to invert)", &s.insertSnap);
            ImGui::Checkbox("NoCollision (place anywhere)", &s.insertNoCollision);
            {
                static const char* kShapes[] = { "Flat roof", "Gable roof", "Shed roof", "Stepped tower" };
                static const int kShapeId[] = { 0, 3, 10, 4 };
                int si = 0;
                for (int k = 0; k < 4; k++) if (kShapeId[k] == s.insertShape) si = k;
                if (ImGui::Combo("Shape", &si, kShapes, 4)) s.insertShape = kShapeId[si];
            }
            ImGui::DragFloat("Width", &s.insertW, 0.25f, 2.0f, 60.0f);
            ImGui::DragFloat("Depth", &s.insertD, 0.25f, 2.0f, 60.0f);
            {
                int fl = std::max(1, (int)lroundf(s.insertH / kFloorHeight));
                if (ImGui::DragInt("Floors", &fl, 0.2f, 1, 120)) s.insertH = (float)fl * kFloorHeight;
                ImGui::SameLine(); ImGui::TextDisabled("%.1f m", (float)fl * kFloorHeight);
            }
            ImGui::TextDisabled("Click a block to plop. Q/E rotate when not snapped.\nNeighbouring buildings shrink or vanish to make room.\nSelect a placed building to move/delete it.");
        }
        if (s.tool == CityTool::DrawRoad) {
            ImGui::Checkbox("Delete roads (or hold Ctrl)", &s.roadErase);
            ImGui::DragFloat("New road height", &s.roadHeight, 0.1f, -200.0f, 500.0f, "%.1f m");
            ImGui::TextDisabled("PgUp/PgDn raise/lower (Shift = fine), Home resets.\nSnapping to a node keeps that node's height; the road ramps between its ends.");
            ImGui::TextDisabled("Click to start (drag to bend), click to place the end.\nRoads chain; Esc / right-click stops. Ends snap to roads.");
        }
        if (s.tool == CityTool::ElevateRoad) {
            ImGui::TextDisabled("Drag a node up/down to set its height (Shift = fine, Ctrl = snap to 0.5 m).\nClick a road to set grade, smooth it or turn it into a bridge.");
        }
        if (s.tool == CityTool::Transit) {
            const auto& stops = city->GetBusStops();
            const auto& lines = city->GetBusLines();
            ImGui::TextDisabled("Auto-generate lines between hubs, or build them by hand:\nclick a road to place a stop on that kerb, then click stops in the order a line visits them.");
            ImGui::DragInt("Lines", &s.transitAutoLines, 0.2f, 1, 12);
            ImGui::DragInt("Stops per line", &s.transitAutoStops, 0.2f, 2, 24);
            if (ImGui::Button("Auto-generate transit")) { NotifyCityEdit(); city->AutoTransit(s.transitAutoLines, s.transitAutoStops); s.transitLine = lines.empty() ? -1 : 0; s.transitStop = -1; }
            ImGui::SameLine();
            if (ImGui::Button("Clear all")) { NotifyCityEdit(); city->ClearTransit(); s.transitLine = s.transitStop = -1; }
            ImGui::Text("%zu stops, %zu lines", stops.size(), lines.size());
            {
                const auto& ts = city->GetTrafficStats();
                if (ts.buses > 0) ImGui::TextDisabled("%d buses running, %d stop visits (Play mode)", ts.buses, ts.busStopsServed);
            }
            ImGui::SeparatorText("Lines");
            int delLine = -1;
            for (int i = 0; i < (int)lines.size(); i++) {
                ImGui::PushID(1000 + i);
                const Color lc = lines[(size_t)i].color;
                ImGui::ColorButton("##c", ImVec4(lc.r / 255.0f, lc.g / 255.0f, lc.b / 255.0f, 1.0f), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(14, 14));
                ImGui::SameLine();
                char lbl[96];
                snprintf(lbl, sizeof lbl, "%s (%zu stops, %d buses)##l", lines[(size_t)i].name.c_str(), lines[(size_t)i].stops.size(), lines[(size_t)i].buses);
                if (ImGui::Selectable(lbl, s.transitLine == i)) s.transitLine = i;
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) delLine = i;
                ImGui::PopID();
            }
            if (delLine >= 0) { NotifyCityEdit(); city->RemoveBusLine(delLine); s.transitLine = -1; }
            if (ImGui::Button("New line")) {
                static const Color kPal[8] = { { 40, 110, 200, 255 }, { 210, 70, 60, 255 }, { 50, 160, 90, 255 }, { 230, 170, 40, 255 },
                                               { 150, 80, 170, 255 }, { 40, 170, 180, 255 }, { 220, 110, 50, 255 }, { 120, 130, 140, 255 } };
                BusLine nl;
                nl.name = "Line " + std::to_string(lines.size() + 1);
                nl.color = kPal[lines.size() % 8];
                NotifyCityEdit();
                s.transitLine = city->AddBusLine(nl);
            }
            if (s.transitLine >= 0 && (size_t)s.transitLine < lines.size()) {
                BusLine l = lines[(size_t)s.transitLine];
                bool ch = false;
                char nm[64]; snprintf(nm, sizeof nm, "%s", l.name.c_str());
                if (ImGui::InputText("Line name", nm, sizeof nm)) { l.name = nm; ch = true; }
                float col[3] = { l.color.r / 255.0f, l.color.g / 255.0f, l.color.b / 255.0f };
                if (ImGui::ColorEdit3("Colour", col, ImGuiColorEditFlags_NoInputs)) { l.color = Color{ (unsigned char)(col[0] * 255.0f), (unsigned char)(col[1] * 255.0f), (unsigned char)(col[2] * 255.0f), 255 }; ch = true; }
                ch |= ImGui::DragInt("Buses", &l.buses, 0.1f, 0, 20);
                ImGui::Checkbox("Clicking stops adds them to this line", &s.transitAddToLine);
                int rem = -1;
                for (int k = 0; k < (int)l.stops.size(); k++) {
                    ImGui::PushID(2000 + k);
                    const int si = l.stops[(size_t)k];
                    ImGui::Text("%d. %s", k + 1, (size_t)si < stops.size() ? stops[(size_t)si].name.c_str() : "?");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) rem = k;
                    ImGui::PopID();
                }
                if (rem >= 0) { NotifyCityEdit(); city->RemoveStopFromLine(s.transitLine, rem); }
                else if (ch) { NotifyCityEdit(); city->UpdateBusLine(s.transitLine, l); }
                if (l.stops.size() < 2) ImGui::TextDisabled("A line needs at least two stops before buses run.");
            }
            if (s.transitStop >= 0 && (size_t)s.transitStop < stops.size()) {
                ImGui::SeparatorText("Selected stop");
                ImGui::Text("%s%s", stops[(size_t)s.transitStop].name.c_str(), stops[(size_t)s.transitStop].edge < 0 ? "  (not on a road)" : "");
                if (ImGui::Button("Remove stop (Del)")) { NotifyCityEdit(); city->RemoveBusStop(s.transitStop); s.transitStop = -1; }
            }
        }
        if (s.tool == CityTool::Districts) {
            static const char* kKinds[] = { "Downtown", "Suburb", "Industrial" };
            static const char* kDStyles[] = { "City style", "Modern", "Brick", "Industrial", "Suburban" };
            ImGui::Checkbox("Paint blocks into selected district", &s.districtPaintMode);
            ImGui::Combo("New district kind", &s.districtNewKind, kKinds, 3);
            ImGui::DragFloat("Auto downtown height x", &s.districtAutoPeak, 0.05f, 0.3f, 10.0f);
            if (ImGui::Button("Auto: downtown in the middle")) { NotifyCityEdit(); city->AutoDistricts(s.districtAutoPeak); s.districtSel = 0; }
            ImGui::SameLine();
            if (ImGui::Button("Clear all")) {
                NotifyCityEdit();
                for (int i = (int)city->GetDistricts().size() - 1; i >= 0; i--) city->RemoveDistrict(i);
                s.districtSel = -1;
            }
            int del = -1;
            const auto& ds = city->GetDistricts();
            for (int i = 0; i < (int)ds.size(); i++) {
                ImGui::PushID(i);
                char lbl[96];
                snprintf(lbl, sizeof lbl, "%s (%s)##d", ds[(size_t)i].name.empty() ? "District" : ds[(size_t)i].name.c_str(), kKinds[std::clamp(ds[(size_t)i].kind, 0, 2)]);
                if (ImGui::Selectable(lbl, s.districtSel == i)) s.districtSel = i;
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) del = i;
                ImGui::PopID();
            }
            if (del >= 0) { NotifyCityEdit(); city->RemoveDistrict(del); s.districtSel = -1; }
            if (s.districtSel >= 0 && (size_t)s.districtSel < city->GetDistricts().size()) {
                District d = city->GetDistricts()[(size_t)s.districtSel];
                bool ch = false;
                char nm[64]; snprintf(nm, sizeof nm, "%s", d.name.c_str());
                if (ImGui::InputText("Name", nm, sizeof nm)) { d.name = nm; ch = true; }
                ch |= ImGui::Combo("Kind", &d.kind, kKinds, 3);
                ch |= ImGui::DragFloat("Radius", &d.radius, 1.0f, 10.0f, 2000.0f, "%.0f m");
                ch |= ImGui::DragFloat("Height at centre x", &d.peak, 0.02f, 0.15f, 10.0f);
                int st = d.style + 1;
                if (ImGui::Combo("Style", &st, kDStyles, 5)) { d.style = st - 1; ch = true; }
                if (ch) { NotifyCityEdit(); city->UpdateDistrict(s.districtSel, d); }
            }
            ImGui::TextDisabled("Click ground: new district. Click a marker: select + drag.\nHeight fades smoothly from the centre to the radius; districts can overlap.");
        }
        if (s.tool == CityTool::PaintBlock) {
            int k = (int)s.paintKind;
            ImGui::RadioButton("Park", &k, (int)BlockKind::Park); ImGui::SameLine();
            ImGui::RadioButton("Buildings", &k, (int)BlockKind::Buildings); ImGui::SameLine();
            ImGui::RadioButton("Concrete", &k, (int)BlockKind::Concrete);
            ImGui::RadioButton("Auto (procedural)", &k, (int)BlockKind::Auto);
            s.paintKind = (BlockKind)k;
            ImGui::TextDisabled("Click or drag over blocks in the viewport.");
        }
    }

    {
        bool coll = city->GetCollisionEnabled();
        if (ImGui::Checkbox("Building collision (Play mode)", &coll)) {
            NotifyCityEdit();
            city->SetCollisionEnabled(coll);
        }
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
    {
        static const char* kStyles[] = { "Modern", "Brick", "Industrial", "Suburban" };
        ImGui::Combo("Building style", &p.style, kStyles, 4);
        ImGui::SliderFloat("Roof / shape variety", &p.shapeVariety, 0.0f, 1.0f);
        ImGui::SliderFloat("Short buildings", &p.shortChance, 0.0f, 1.0f);
        ImGui::SliderFloat("Footprint variety", &p.footprintVariety, 0.0f, 0.6f);
        ImGui::Checkbox("Street furniture (lights, trees)", &p.furniture);
        ImGui::DragInt("Cars (Play mode)", &p.cars, 0.5f, 0, 500);
        ImGui::DragInt("Pedestrians (Play mode)", &p.pedestrians, 0.5f, 0, 1000);
        ImGui::Checkbox("Cars drive to destinations", &p.routedTraffic);
        ImGui::SameLine(); ImGui::TextDisabled("(downtown attracts more)");
        ImGui::Checkbox("Rush hours (car count follows the hour)", &p.rushHours);
        ImGui::DragFloat("Traffic detail distance", &p.trafficDetailDistance, 1.0f, 0.0f, 2000.0f, p.trafficDetailDistance > 0.0f ? "%.0f m" : "always detailed");
        ImGui::Checkbox("Drive on the left", &p.leftHandTraffic);
        if (ImGui::TreeNode("Car colours")) {
            ImGui::Checkbox("All (equal chance, ignore percentages)", &p.carColorAll);
            float total = 0.0f;
            for (const CarColor& c : p.carColors) total += std::max(c.weight, 0.0f);
            int del = -1;
            for (size_t i = 0; i < p.carColors.size(); i++) {
                CarColor& c = p.carColors[i];
                ImGui::PushID((int)i);
                float col[3] = { c.color.r / 255.0f, c.color.g / 255.0f, c.color.b / 255.0f };
                ImGui::BeginDisabled(c.other);
                if (ImGui::ColorEdit3("##c", col, ImGuiColorEditFlags_NoInputs))
                    c.color = Color{ (unsigned char)(col[0] * 255.0f + 0.5f), (unsigned char)(col[1] * 255.0f + 0.5f), (unsigned char)(col[2] * 255.0f + 0.5f), 255 };
                ImGui::EndDisabled();
                ImGui::SameLine();
                char nm[32]; std::snprintf(nm, sizeof nm, "%s", c.name.c_str());
                ImGui::SetNextItemWidth(90.0f);
                if (ImGui::InputText("##n", nm, sizeof nm)) c.name = nm;
                ImGui::SameLine();
                ImGui::BeginDisabled(p.carColorAll);
                ImGui::SetNextItemWidth(70.0f);
                ImGui::DragFloat("##w", &c.weight, 0.5f, 0.0f, 1000.0f, "%.0f");
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (p.carColorAll) ImGui::TextDisabled("equal");
                else ImGui::Text("%.0f%%", total > 0.0f ? 100.0f * std::max(c.weight, 0.0f) / total : 0.0f);
                ImGui::SameLine();
                if (c.other) { ImGui::TextDisabled("(random vivid)"); ImGui::SameLine(); }
                if (ImGui::SmallButton("x")) del = (int)i;
                ImGui::PopID();
            }
            if (del >= 0) p.carColors.erase(p.carColors.begin() + del);
            if (ImGui::Button("Add colour")) p.carColors.push_back(CarColor{ "Custom", Color{ 60, 90, 200, 255 }, 5.0f, false });
            ImGui::SameLine();
            if (ImGui::Button("Reset colours")) { p.carColors = CityParams{}.carColors; p.carColorAll = false; }
            ImGui::TreePop();
        }
    }
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
        if (edited && city->MoveWouldCross(s.selectedNode, v)) {
            edited = false; // would cross another road
        }
        if (edited) {
            if (!s.dragMoved) { s.dragMoved = true; NotifyCityEdit(); }
            city->MoveNode(s.selectedNode, v, true); // incremental rebuild
        }
        {
            float hh = city->GetNodes()[(size_t)s.selectedNode].h;
            if (ImGui::DragFloat("Height", &hh, 0.1f, -200.0f, 500.0f, "%.2f m")) {
                if (!s.dragMoved) { s.dragMoved = true; NotifyCityEdit(); }
                city->SetNodeHeight(s.selectedNode, hh);
            }
        }
        if (city->GetNodes()[(size_t)s.selectedNode].junction) {
            static const char* kJ[] = { "Plain", "Crosswalks", "Traffic light", "Stop signs", "Roundabout" };
            int jk = city->GetNodes()[(size_t)s.selectedNode].jkind;
            if (ImGui::Combo("Junction", &jk, kJ, 5)) {
                NotifyCityEdit();
                city->SetNodeJunctionKind(s.selectedNode, jk);
            }
        }
        if (city->GetNodes()[(size_t)s.selectedNode].junction &&
            city->GetNodes()[(size_t)s.selectedNode].jkind == (int)JunctionKind::Roundabout) {
            const RoadNode& rn = city->GetNodes()[(size_t)s.selectedNode];
            float isl = rn.rbIsland, rg = rn.rbRing;
            bool sp = rn.rbSplitters, co = rn.rbConcrete, ch = false;
            ImGui::SeparatorText("Roundabout");
            ch |= ImGui::DragFloat("Island radius", &isl, 0.1f, 0.0f, 30.0f, isl > 0.0f ? "%.1f m" : "auto");
            ch |= ImGui::DragFloat("Roadway width", &rg, 0.1f, 0.0f, 20.0f, rg > 0.0f ? "%.1f m" : "auto");
            ch |= ImGui::Checkbox("Splitter islands", &sp);
            ch |= ImGui::Checkbox("Concrete island (no grass)", &co);
            ImGui::TextDisabled("0 = automatic size from the connected roads.\nBuildings keep clear of the ring.");
            if (ch) { NotifyCityEdit(); city->SetRoundaboutProps(s.selectedNode, isl, rg, sp, co); }
        }
        if (city->GetNodes()[(size_t)s.selectedNode].junction &&
            city->GetNodes()[(size_t)s.selectedNode].jkind == (int)JunctionKind::TrafficLight) {
            RoadNode& nd = city->GetNodes()[(size_t)s.selectedNode];
            ImGui::SeparatorText("Traffic signal");
            auto act = [&]() { if (ImGui::IsItemActivated()) NotifyCityEdit(); };
            ImGui::DragFloat("Amber (s)", &nd.yellow, 0.1f, 0.5f, 6.0f, "%.1f"); act();
            ImGui::DragFloat("All-red (s)", &nd.allRed, 0.1f, 0.0f, 6.0f, "%.1f"); act();
            ImGui::DragFloat("Offset (s)", &nd.sigOffset, 0.2f, 0.0f, 300.0f, "%.1f"); act();
            if (nd.phases.empty()) {
                ImGui::TextDisabled("Default cycle: east/west roads, then north/south.");
                if (ImGui::Button("Customise phases")) {
                    NotifyCityEdit();
                    nd.phases = { SignalPhase{ 9.0f, (uint8_t)((1 << 0) | (1 << 1) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 7)) },
                                  SignalPhase{ 9.0f, (uint8_t)((1 << 2) | (1 << 6)) } };
                }
            } else {
                static const char* kSec[8] = { "E", "NE", "N", "NW", "W", "SW", "S", "SE" };
                ImGui::TextDisabled("Tick the roads (by compass direction from the\njunction) that get green in each phase.");
                int removeAt = -1;
                for (size_t pi = 0; pi < nd.phases.size(); pi++) {
                    ImGui::PushID((int)pi);
                    SignalPhase& ph = nd.phases[pi];
                    ImGui::Text("Phase %zu", pi + 1);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove") && nd.phases.size() > 1) removeAt = (int)pi;
                    ImGui::DragFloat("Green (s)", &ph.duration, 0.2f, 1.0f, 120.0f, "%.1f"); act();
                    for (int b = 0; b < 8; b++) {
                        bool on = (ph.mask >> b) & 1;
                        if (b & 3) ImGui::SameLine();
                        ImGui::PushID(b);
                        if (ImGui::Checkbox(kSec[b], &on)) {
                            NotifyCityEdit();
                            if (on) ph.mask |= (uint8_t)(1u << b); else ph.mask &= (uint8_t)~(1u << b);
                        }
                        ImGui::PopID();
                    }
                    ImGui::PopID();
                }
                if (removeAt >= 0) { NotifyCityEdit(); nd.phases.erase(nd.phases.begin() + removeAt); }
                if (ImGui::Button("Add phase")) { NotifyCityEdit(); nd.phases.push_back(SignalPhase{ 9.0f, 0 }); }
                ImGui::SameLine();
                if (ImGui::Button("Reset to default")) { NotifyCityEdit(); nd.phases.clear(); }
            }
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

        {
            RoadEdge pr = city->GetEdges()[(size_t)s.selectedEdge];
            bool ed = false;
            static const char* kTypes[] = { "Street", "Avenue", "Highway", "Dirt path", "Pedestrian street" };
            int ty = pr.type;
            if (ImGui::Combo("Road type", &ty, kTypes, 5)) {
                pr.type = ty;
                // Presets: lanes / lane width / sidewalk / curb / markings.
                switch (ty) {
                    case 0: pr.lanes = 0; pr.width = 0.0f; pr.sidewalk = -1.0f; pr.curbH = 0.12f; pr.markings = true; break;
                    case 1: pr.lanes = 4; pr.width = 0.0f; pr.sidewalk = 3.0f; pr.curbH = 0.15f; pr.markings = true; break;
                    case 2: pr.lanes = 6; pr.width = 3.6f; pr.sidewalk = 1.0f; pr.curbH = 0.0f; pr.markings = true; break;
                    case 3: pr.lanes = 1; pr.width = 2.6f; pr.sidewalk = 0.3f; pr.curbH = 0.0f; pr.markings = false; break;
                    default: pr.lanes = 2; pr.width = 2.0f; pr.sidewalk = 1.0f; pr.curbH = 0.04f; pr.markings = false; break;
                }
                ed = true;
            }
            float lw = pr.width;
            if (ImGui::DragFloat("Lane width", &lw, 0.05f, 0.0f, 12.0f, lw > 0.0f ? "%.2f m" : "global")) { pr.width = lw; ed = true; }
            float sw = pr.sidewalk;
            if (ImGui::DragFloat("Sidewalk", &sw, 0.05f, -1.0f, 12.0f, sw >= 0.0f ? "%.2f m" : "global")) { pr.sidewalk = sw; ed = true; }
            ed |= ImGui::DragFloat("Curb height", &pr.curbH, 0.01f, 0.0f, 1.0f, "%.2f m");
            ed |= ImGui::Checkbox("Lane markings", &pr.markings);
            ed |= ImGui::DragFloat("Banking", &pr.bank, 0.1f, -20.0f, 20.0f, "%.1f deg");
            ed |= ImGui::Checkbox("Bridge", &pr.bridge);
            static const char* kOW[] = { "Two-way", "One-way (A to B)", "One-way (B to A)" };
            ed |= ImGui::Combo("Direction", &pr.oneWay, kOW, 3);
            ed |= ImGui::DragFloat("Speed limit (m/s)", &pr.speedLimit, 0.25f, 0.0f, 60.0f, pr.speedLimit > 0.0f ? "%.1f" : "default");
            if (ed) { NotifyCityEdit(); city->SetEdgeProps(s.selectedEdge, pr); }
        }
        {
            // Turn lanes: which manoeuvres each lane (counted from the centre line) may make at each road end.
            const RoadEdge& te = city->GetEdges()[(size_t)s.selectedEdge];
            uint16_t ta = te.turnA, tb = te.turnB;
            bool changed = false;
            const int lanesHere = te.lanes > 0 ? te.lanes : city->GetParams().lanes;
            const int perDir = std::min(te.oneWay ? lanesHere : std::max(lanesHere / 2, 1), 4);
            if (ImGui::TreeNode("Turn lanes")) {
                ImGui::TextDisabled("Per lane, centre line outward: L = left, S = straight, R = right.\nNone ticked = any manoeuvre.");
                for (int endSel = 0; endSel < 2; endSel++) {
                    uint16_t& m = endSel == 0 ? ta : tb;
                    ImGui::Text("Arriving at node %d", endSel == 0 ? te.a : te.b);
                    for (int l = 0; l < perDir; l++) {
                        ImGui::PushID(endSel * 8 + l);
                        ImGui::Text("Lane %d", l + 1);
                        for (int b = 0; b < 3; b++) {
                            static const char* kL[3] = { "L", "S", "R" };
                            bool on = (m >> (4 * l + b)) & 1;
                            ImGui::SameLine();
                            ImGui::PushID(b);
                            if (ImGui::Checkbox(kL[b], &on)) {
                                if (on) m |= (uint16_t)(1u << (4 * l + b)); else m &= (uint16_t)~(1u << (4 * l + b));
                                changed = true;
                            }
                            ImGui::PopID();
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::TreePop();
            }
            if (changed) { NotifyCityEdit(); city->SetEdgeTurnLanes(s.selectedEdge, ta, tb, te.speedLimit); }
        }
        {
            const RoadEdge& se = city->GetEdges()[(size_t)s.selectedEdge];
            ImGui::Text("Ends: %.1f m -> %.1f m", city->GetNodes()[(size_t)se.a].h, city->GetNodes()[(size_t)se.b].h);
            ImGui::DragFloat("Grade", &s.gradePercent, 0.1f, -30.0f, 30.0f, "%.1f %%");
            if (ImGui::Button("Apply grade along road")) {
                NotifyCityEdit();
                city->SetRoadGrade(s.selectedEdge, se.a, s.gradePercent);
            }
            ImGui::SameLine();
            if (ImGui::Button("Smooth heights")) {
                NotifyCityEdit();
                city->SmoothRoadHeights(s.selectedEdge);
            }
        }
        if (ImGui::Button("Split road here")) {
            Vector2 mid = city->EdgeMidpoint(s.selectedEdge);
            NotifyCityEdit();
            int n = city->InsertNodeOnEdge(s.selectedEdge, mid);
            s.selectedEdge = -1;
            if (n >= 0) s.selectedNode = n;
        }
    }

    // --- Building editing ---
    if (s.selectedBlockId != 0 && s.selectedBuildingSlot >= 0) {
        const auto& blocks = city->GetBlocks();
        int bi = -1;
        for (int i = 0; i < (int)blocks.size(); i++) if (blocks[(size_t)i].id == s.selectedBlockId) { bi = i; break; }
        const bool valid = bi >= 0 && (size_t)s.selectedBuildingSlot < blocks[(size_t)bi].buildings.size();
        ImGui::SeparatorText("Building##panel");
        const Building* sel = valid ? &blocks[(size_t)bi].buildings[(size_t)s.selectedBuildingSlot] : nullptr;
        if (sel && sel->placedIndex >= 0 && (size_t)sel->placedIndex < city->GetPlacedBuildings().size()) {
            const Building& b = *sel;
            // User-placed building: edits go to the placed list (they survive regeneration).
            PlacedBuilding pb = city->GetPlacedBuildings()[(size_t)b.placedIndex];
            ImGui::Text("Placed building %d", b.placedIndex);
            bool edited = false;
            edited |= ImGui::DragFloat2("Position", &pb.center.x, 0.1f);
            float deg = pb.angleY * RAD2DEG;
            if (ImGui::DragFloat("Rotation", &deg, 0.5f, -360.0f, 360.0f)) { pb.angleY = deg * DEG2RAD; edited = true; }
            edited |= ImGui::DragFloat("Width", &pb.sizeX, 0.1f, 2.0f, 60.0f);
            edited |= ImGui::DragFloat("Depth", &pb.sizeZ, 0.1f, 2.0f, 60.0f);
            {
                int fl = std::max(1, (int)lroundf(pb.height / kFloorHeight));
                if (ImGui::DragInt("Floors", &fl, 0.2f, 1, 120)) { pb.height = (float)fl * kFloorHeight; edited = true; }
            }
            if (ImGui::Checkbox("NoCollision", &pb.free)) edited = true;
            {
                static const char* kShapes[] = { "Flat roof", "Gable roof", "Shed roof", "Stepped tower" };
                static const int kShapeId[] = { 0, 3, 10, 4 };
                int si = 0;
                for (int k = 0; k < 4; k++) if (kShapeId[k] == pb.shape) si = k;
                if (ImGui::Combo("Shape", &si, kShapes, 4)) { pb.shape = kShapeId[si]; edited = true; }
            }
            if (edited && (pb.free || city->CanPlaceBuilding(pb, b.placedIndex))) {
                if (!s.dragMoved) { s.dragMoved = true; NotifyCityEdit(); }
                city->UpdatePlacedBuilding(b.placedIndex, pb);
            }
            if (ImGui::Button("Delete building")) {
                NotifyCityEdit();
                city->RemovePlacedBuilding(b.placedIndex);
                s.selectedBlockId = 0; s.selectedBuildingSlot = -1;
            }
        } else if (valid) {
            const Building& b = blocks[(size_t)bi].buildings[(size_t)s.selectedBuildingSlot];
            ImGui::Text("Block %llu, building %d", (unsigned long long)s.selectedBlockId, s.selectedBuildingSlot);
            const bool hasOverride = city->HasBuildingOverride(s.selectedBlockId, s.selectedBuildingSlot);
            int floorsUi = b.floors;
            float height = (float)floorsUi * kFloorHeight;
            if (ImGui::DragInt("Floors", &floorsUi, 0.2f, 1, 120)) {
                height = (float)floorsUi * kFloorHeight;
                NotifyCityEdit();
                city->SetBuildingHeightOverride(s.selectedBlockId, s.selectedBuildingSlot, height);
            }
            const Vector2 prevPos = { b.center.x, b.center.z };
            Vector2 pos = prevPos;
            if (ImGui::DragFloat2("Position", &pos.x, 0.1f)) {
                NotifyCityEdit();
                // b.center already has any existing offset baked in (that's what
                // laid it out here), so the new offset is the old one plus however
                // far this drag just moved it.
                const Vector2 existing = city->GetBuildingOffsetOverride(s.selectedBlockId, s.selectedBuildingSlot);
                const Vector2 delta = Vector2Subtract(pos, prevPos);
                city->SetBuildingOffsetOverride(s.selectedBlockId, s.selectedBuildingSlot,
                                                Vector2Add(existing, delta));
            }
            ImGui::TextDisabled(hasOverride ? "Custom height/position" : "Using the procedural default");
            if (hasOverride && ImGui::Button("Reset to default")) {
                NotifyCityEdit();
                city->ClearBuildingOverride(s.selectedBlockId, s.selectedBuildingSlot);
            }
        } else {
            ImGui::TextDisabled("This building no longer exists (its block changed shape).");
        }
    } else {
        ImGui::SeparatorText("Building##panel");
        ImGui::TextDisabled("Click a building in the viewport to select it.");
    }

    // --- Stats ---
    int buildingCount = 0;
    for (const auto& b : city->GetBlocks()) buildingCount += (int)b.buildings.size();
    ImGui::SeparatorText("Stats");
    ImGui::Text("Nodes: %zu   Roads: %zu   Blocks: %zu", city->GetNodes().size(),
                city->GetEdges().size(), city->GetBlocks().size());
    ImGui::Text("Buildings: %d", buildingCount);
    if (city->IsRebuilding()) ImGui::TextDisabled("Rebuilding in background...");
    {
        const auto& ts = city->GetTrafficStats();
        if (ts.cars + ts.peds > 0)
            ImGui::Text("Traffic: %d cars (%d near, %d far), %d walkers\navg %.1f m/s, %d stopped, min gap %.1f m, step %.2f ms\nwalker max %.1f m/s | crashes: %d | jumps: %d",
                        ts.cars, ts.nearCars, ts.farCars, ts.peds, ts.avgSpeed, ts.stopped, ts.minGap, ts.stepMs, ts.maxWalkerSpeed, ts.overlaps, ts.jumps);
    }

    ImGui::SeparatorText("Diagnostics");
    {
        ImGui::Checkbox("Show geometry problems", &s.showProblems);
        if (s.showProblems) {
            if (!city->GetCollisionEnabled()) ImGui::TextDisabled("Needs \"Building collision\" on (it builds the surface data).");
            else {
                const auto& gp = city->GetGeometryProblemsCached();
                ImGui::Text("steep: %zu   thin: %zu   steps/cracks: %zu", gp.steep.size(), gp.thin.size(), gp.steps.size());
                ImGui::Text("pad drawn over road: %zu", gp.padOverRoad.size());
                ImGui::TextWrapped("Red outline = steep triangle, orange = thin spike, red vertical line = a step or crack between two surfaces, magenta cross = a pad drawn over the road asphalt. Move the camera over the broken area.");
            }
        }
    }

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