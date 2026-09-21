#pragma once

#include "TerrainTypes.hpp"
#include "raylib.h"

namespace terrain {

// ============================================================================
// Mesh Generation
// ============================================================================

// Generate terrain mesh from heightmap data
Mesh GenerateTerrainMesh(const float* heightmap, int resolution, float scale, float minHeight, float maxHeight);

// Generate LOD mesh by downsampling heightmap
Mesh GenerateTerrainMeshLOD(const float* heightmap, int resolution, int lod, float scale, float minHeight, float maxHeight);

// Apply geomorphing for smooth LOD transitions
void ApplyGeomorphing(Mesh& mesh, const float* heightmap, int resolution, int lod, float morphFactor);

// Update mesh vertex heights and normals (for sculpting)
void UpdateMeshHeights(Mesh& mesh, const float* heightmap, int resolution, float scale, float minHeight);

// Update mesh splatmap UV coordinates
void UpdateMeshSplatmap(Mesh& mesh, const uint8_t* splatmap, int resolution);

// Compute bounding box from mesh
BoundingBox ComputeMeshBounds(const Mesh& mesh, float minHeight, float maxHeight);

} // namespace terrain