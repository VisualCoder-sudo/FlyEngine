#include "../../include/Terrain/TerrainEditor.hpp"
#include "../../include/Terrain/BasicTerrain.hpp"
#include "../../include/Terrain/Terrain.hpp"
#include "../../include/Engine.hpp"
#include "../../include/Engine/Backend/CameraController.hpp"
#include "../../include/Engine/Frontend/ui.hpp"
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
    
    // Handle chunked terrain (legacy)
    if (state.selectedTerrainLegacy) {
        BasicTerrain::SetEditorActive(false);
        terrain::Terrain* terrain = state.selectedTerrainLegacy;
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
        
        // Handle input for chunked terrain
        if (state.mode == TerrainEditorState::Mode::Sculpt) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !ui::IsMouseOverUI()) {
                float dt = GetFrameTime();
                // Paint tool uses PaintLayer for proper splatmap handling
                if (state.brush.tool == terrain::TerrainTool::Paint) {
                    terrain->PaintLayer(state.brushWorldPos, state.brush.radius, state.brush.strength * dt * 2.0f, 
                                        state.brush.paintLayer, state.brush.paintErase);
                } else {
                    terrain->ApplyBrush(state.brush, state.brushWorldPos, dt);
                }
            }
        }
        return;
    }
    
    // Handle BasicTerrain
    if (!state.selectedTerrain) {
        BasicTerrain::SetEditorActive(false);
        return;
    }
    
    BasicTerrain* terrain = state.selectedTerrain;
    Camera3D& camera = engine.GetCamera();
    
    // Mark editor as active for this terrain so BasicTerrain skips its own input handling
    BasicTerrain::SetEditorActive(state.mode == TerrainEditorState::Mode::Sculpt);
    
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
    
    // Handle input for BasicTerrain directly (bypassing BasicTerrain::Update's input handling)
    if (state.mode == TerrainEditorState::Mode::Sculpt) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !ui::IsMouseOverUI()) {
            float dt = GetFrameTime();
            // Convert terrain::TerrainBrush to BasicTerrain::Brush
            BasicTerrain::Brush bBrush;
            bBrush.tool = static_cast<BasicTerrain::Tool>(state.brush.tool);
            bBrush.shape = static_cast<BasicTerrain::Shape>(state.brush.shape);
            bBrush.radius = state.brush.radius;
            bBrush.strength = state.brush.strength;
            bBrush.hardness = state.brush.hardness;
            bBrush.targetHeight = state.brush.targetHeight;
            bBrush.paintLayer = state.brush.paintLayer;
            bBrush.paintErase = state.brush.paintErase;
            bBrush.noiseSeed = state.brush.noiseSeed;
            bBrush.noiseScale = state.brush.noiseScale;
            
            // Paint tool uses ApplyPaintBrush for proper splatmap handling
            if (state.brush.tool == terrain::TerrainTool::Paint) {
                terrain->ApplyPaintBrush(bBrush, state.brushWorldPos, dt);
            } else {
                terrain->ApplyBrush(bBrush, state.brushWorldPos, dt);
            }
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
    return BasicTerrain::GetBrush().tool;
}

void SetCurrentTerrainTool(BasicTerrain::Tool tool) {
    BasicTerrain::GetBrush().tool = tool;
    g_terrainEditorState.brush.tool = static_cast<terrain::TerrainTool>(tool);
}

BasicTerrain::Brush& GetTerrainBrush() {
    return BasicTerrain::GetBrush();
}

void RequestNewTerrain() {
    // Not implemented
}

void RequestHeightmapImport() {
    // Not implemented
}

void HandleTerrainSelection(BasicTerrain* terrain, bool selected) {
    auto& state = g_terrainEditorState;
    if (selected) {
        state.selectedTerrain = terrain;
        state.mode = TerrainEditorState::Mode::Sculpt;
        // Sync brush from BasicTerrain's shared brush
        state.brush.tool = static_cast<terrain::TerrainTool>(BasicTerrain::GetBrush().tool);
        state.brush.shape = static_cast<terrain::TerrainBrush::Shape>(BasicTerrain::GetBrush().shape);
        state.brush.radius = BasicTerrain::GetBrush().radius;
        state.brush.strength = BasicTerrain::GetBrush().strength;
        state.brush.hardness = BasicTerrain::GetBrush().hardness;
        state.brush.targetHeight = BasicTerrain::GetBrush().targetHeight;
        state.brush.paintLayer = BasicTerrain::GetBrush().paintLayer;
        state.brush.paintErase = BasicTerrain::GetBrush().paintErase;
        state.brush.noiseSeed = BasicTerrain::GetBrush().noiseSeed;
        state.brush.noiseScale = BasicTerrain::GetBrush().noiseScale;
    } else if (state.selectedTerrain == terrain) {
        state.selectedTerrain = nullptr;
    }
}

void HandleTerrainSelection(class terrain::Terrain* terrain, bool selected) {
    auto& state = g_terrainEditorState;
    if (selected) {
        state.selectedTerrainLegacy = terrain;
        state.mode = TerrainEditorState::Mode::Sculpt;
        // Brush is already in terrain::TerrainBrush format, no conversion needed
    } else if (state.selectedTerrainLegacy == terrain) {
        state.selectedTerrainLegacy = nullptr;
    }
}

void DrawTerrainBrushPreview(const Camera3D& camera) {
    auto& state = g_terrainEditorState;
    if (!state.brushValid || !state.showBrushPreview) return;

    Terrain* terrain = state.selectedTerrainLegacy;
    if (!terrain) return;

    if (state.mode != TerrainEditorState::Mode::Sculpt) return;

    // Get brush settings
    const TerrainBrush& brush = state.brush;
    Vector2 center = state.brushWorldPos;
    float radius = brush.radius;

    // Draw brush outline that follows the terrain surface (shape-aware)
    Color ring = Color{ 60, 140, 255, 220 };

    if (brush.shape == TerrainBrush::Shape::Square) {
        const int steps = 48;
        Vector3 prev{}; 
        bool has = false;
        for (int i = 0; i <= steps; i++) {
            float s = (float)i / steps * 4.0f;
            int side = (int)s; 
            float f = s - side;
            float u, v;
            switch (side) {
                case 0: u = -1.0f + f * 2.0f; v = -1.0f; break;
                case 1: u = 1.0f;             v = -1.0f + f * 2.0f; break;
                case 2: u = 1.0f - f * 2.0f;  v = 1.0f; break;
                default: u = -1.0f;           v = 1.0f - f * 2.0f; break;
            }
            float px = center.x + u * radius;
            float pz = center.y + v * radius;
            Vector3 p = { px, terrain->GetHeightAt(px, pz) + 0.4f, pz };
            if (has) DrawLine3D(prev, p, ring);
            prev = p; 
            has = true;
        }
    } else {
        const int seg = 40;
        Vector3 prev{};
        for (int i = 0; i <= seg; i++) {
            float ang = (float)i / seg * 2.0f * PI;
            float px = center.x + cosf(ang) * radius;
            float pz = center.y + sinf(ang) * radius;
            Vector3 p = { px, terrain->GetHeightAt(px, pz) + 0.4f, pz };
            if (i > 0) DrawLine3D(prev, p, ring);
            prev = p;
        }
    }
}

} // namespace terrain