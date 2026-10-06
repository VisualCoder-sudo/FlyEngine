#pragma once

#include "Terrain.hpp"
#include "TerrainTypes.hpp"

namespace terrain {

// ============================================================================
// Brush Operations
// ============================================================================

// Apply a brush to the terrain at world position
// dt scales Raise/Lower (units per second) so painting is frame-rate independent; one-shot stamps pass dt = 1.
void ApplyBrush(Terrain& terrain, const TerrainBrush& brush, Vector2 center, float dt = 1.0f);

// Create a ramp between two points
void RampTerrain(Terrain& terrain, Vector2 start, Vector2 end, float startHeight, float endHeight);

// Paint a material layer
void PaintLayer(Terrain& terrain, Vector2 center, float radius, float strength, int layerIndex, bool erase = false);

// Apply erosion to a world-space rectangular region (for Generate tab)
void ApplyErosionToRegion(Terrain& terrain, float minX, float minZ, float maxX, float maxZ, const TerrainBrush& brush);

// ============================================================================
// Undo/Redo (methods on Terrain class)
// ============================================================================
// bool Terrain::Undo();
// bool Terrain::Redo();
// void Terrain::ClearUndoRedo();

} // namespace terrain