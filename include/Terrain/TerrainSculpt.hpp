#pragma once

#include "Terrain.hpp"
#include "TerrainTypes.hpp"

namespace terrain {

// ============================================================================
// Brush Operations
// ============================================================================

// Apply a brush to the terrain at world position
void ApplyBrush(Terrain& terrain, const TerrainBrush& brush, Vector2 center);

// Create a ramp between two points
void RampTerrain(Terrain& terrain, Vector2 start, Vector2 end, float startHeight, float endHeight);

// Paint a material layer
void PaintLayer(Terrain& terrain, Vector2 center, float radius, float strength, int layerIndex, bool erase = false);

// ============================================================================
// Undo/Redo (methods on Terrain class)
// ============================================================================
// bool Terrain::Undo();
// bool Terrain::Redo();
// void Terrain::ClearUndoRedo();

} // namespace terrain