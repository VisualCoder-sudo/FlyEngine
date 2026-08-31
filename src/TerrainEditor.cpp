#include "TerrainEditor.hpp"
#include "BasicTerrain.hpp"
#include "Engine.hpp"
#include "CameraController.hpp"
#include "ui.hpp"
#include "raylib.h"
#include "raymath.h"
#include <algorithm>
#include <string>
#include <vector>

namespace terrain {

static TerrainEditorState g_terrainEditorState;
static bool g_terrainEditorInitialized = false;

void InitTerrainEditor() {
    if (g_terrainEditorInitialized) return;
    g_terrainEditorInitialized = true;
}

void ShutdownTerrainEditor() {
    // Not implemented
}

TerrainEditorState& GetTerrainEditorState() {
    return g_terrainEditorState;
}

void UpdateTerrainEditor(Engine& engine, CameraController* cameraCtrl, phys::Simulation* physicsSim) {
    if (!g_terrainEditorInitialized) return;
    
    auto& state = g_terrainEditorState;
    if (!state.selectedTerrain) return;
    
    BasicTerrain* terrain = state.selectedTerrain;
    Camera3D& camera = engine.GetCamera();
    
    // Update brush preview position
    Ray ray = GetMouseRay(GetMousePosition(), camera);
    float distance = 0;
    Vector3 hitPoint, hitNormal;
    if (terrain->Raycast(ray, &distance, &hitPoint, &hitNormal)) {
        state.brushWorldPos = { hitPoint.x, hitPoint.z };
        state.brushValid = true;
    } else {
        state.brushValid = false;
    }
    
    // Handle input
    if (state.mode == TerrainEditorState::Mode::Sculpt) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !ui::IsMouseOverUI()) {
            terrain->ApplyBrush(state.brush, state.brushWorldPos);
        }
    }
}

void DrawTerrainEditorUI() {
    // Minimal implementation
}

bool IsTerrainEditorActive() {
    return true;
}

void SetTerrainEditorMode(int mode) {
    // Not implemented
}

BasicTerrain::Tool GetCurrentTerrainTool() {
    return BasicTerrain::Tool::Raise;
}

void SetCurrentTerrainTool(BasicTerrain::Tool tool) {
    // Not implemented
}

BasicTerrain::Brush& GetTerrainBrush() {
    static BasicTerrain::Brush brush;
    return brush;
}

void RequestNewTerrain() {
    // Not implemented
}

void RequestHeightmapImport() {
    // Not implemented
}

void HandleTerrainSelection(BasicTerrain* terrain, bool selected) {
    // Not implemented
}

void HandleTerrainSelection(class terrain::Terrain* terrain, bool selected) {
    // Not implemented
}

void DrawTerrainBrushPreview(const Camera3D& camera) {
    // Not implemented
}

} // namespace terrain