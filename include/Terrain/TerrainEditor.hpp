#pragma once

#include "BasicTerrain.hpp"
#include "TerrainTypes.hpp"
#include "raylib.h"
#include <vector>

namespace phys { class Simulation; }
class Engine;
class CameraController;

// Forward declare terrain namespace types
namespace terrain {
    class Terrain;
}

// Terrain tool mode (separate from TransformTool)
enum class TerrainEditorMode {
    None,
    Sculpt,
    Paint,
    Select
};

// Terrain editor state
struct TerrainEditorState {
    enum class Mode { None, Sculpt, Paint, Select } mode = Mode::None;
    class BasicTerrain* selectedTerrain = nullptr;
    class terrain::Terrain* selectedTerrainLegacy = nullptr;
    terrain::TerrainBrush brush;
    int paintLayerIndex = 0;
    bool paintErase = false;
    
    // Brush preview
    bool showBrushPreview = true;
    Vector2 brushWorldPos = {0, 0};
    bool brushValid = false;
    
    // Generate tab region box (world-space bounds)
    float regionMinX = -500.0f;
    float regionMaxX = 500.0f;
    float regionMinZ = -500.0f;
    float regionMaxZ = 500.0f;
    
    // Import dialog
    bool showImportDialog = false;
    struct HeightmapImportSettings {
        float worldWidth = 1000.0f;
        float worldDepth = 1000.0f;
        float maxHeight = 200.0f;
        float minHeight = 0.0f;
        bool flipY = false;
        int targetChunkSize = 256;
        int targetResolution = 65;
        bool generateMips = true;
    } importSettings;
    char importPath[512] = {0};
    
    // New terrain dialog
    bool showNewTerrainDialog = false;
    float newTerrainWidth = 1000.0f;
    float newTerrainDepth = 1000.0f;
    int newTerrainChunkSize = 256;
    int newTerrainResolution = 65;
    float newTerrainMaxHeight = 200.0f;
};

namespace terrain {

// Initialize terrain editor
void InitTerrainEditor();

// Shutdown terrain editor
void ShutdownTerrainEditor();

// Update terrain editor (called each frame)
void UpdateTerrainEditor(class Engine& engine, class CameraController* cameraCtrl, class phys::Simulation* physicsSim);

// Draw terrain editor UI (called in UI pass)
void DrawTerrainEditorUI();

// Get editor state
struct TerrainEditorState& GetTerrainEditorState();

// Check if terrain editor is active
bool IsTerrainEditorActive();

// Set terrain editor mode (pass as int, caller should cast from ui::TransformTool)
void SetTerrainEditorMode(int mode);

// Get current terrain tool
BasicTerrain::Tool GetCurrentTerrainTool();

// Set current terrain tool
void SetCurrentTerrainTool(BasicTerrain::Tool tool);

// Get brush settings
BasicTerrain::Brush& GetTerrainBrush();

// Request new terrain creation
void RequestNewTerrain();

// Request heightmap import
void RequestHeightmapImport();

// Handle terrain selection (called by interaction manager)
void HandleTerrainSelection(class BasicTerrain* terrain, bool selected);
void HandleTerrainSelection(class terrain::Terrain* terrain, bool selected);

// Draw brush preview in viewport
void DrawTerrainBrushPreview(const Camera3D& camera);

// Request heightmap import
void RequestHeightmapImport();

} // namespace terrain