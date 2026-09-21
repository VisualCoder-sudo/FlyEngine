#pragma once

#include "City.hpp"

namespace city {

// Selection/drag state used by the viewport node editor and the ImGui panel.
struct CityEditorState {
    City* activeCity = nullptr;
    int selectedNode = -1;
    int selectedEdge = -1;
    int hoveredNode = -1;

    // Viewport node dragging.
    bool draggingNode = false;
    int dragNode = -1;
    Vector2 dragStartNodePos{};
    bool dragMoved = false;

    // Keyboard nudge step (Shift scales it down for fine control).
    float nudgeStep = 1.0f;

    // ImGui panel.
    bool panelOpen = false;
    CityParams prevParams{}; // last snapshot of the active city's params
};

CityEditorState& GetCityEditorState();
void InitCityEditor();

void SetActiveCity(City* c);
bool IsCityEditorActive();

// Runs every interaction-manager update BEFORE object picking. Returns true
// when the viewport left-button down/press should be consumed by the editor
// (node grabbed, city selected, road click) so it never leaks into picking.
bool UpdateCityEditor(Engine& engine, Camera3D& camera);

// ImGui panel drawn by ui::DrawImGuiFrame.
void DrawCityEditorPanel();

} // namespace city