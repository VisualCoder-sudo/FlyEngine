#pragma once

#include "box3d/box3d.h"

class BasicTerrain;

namespace terrain {

// Static Box3D height-field collider for a BasicTerrain. The simulation builds
// one per terrain when play starts (the physics world is rebuilt every play
// session, so the collider always matches the heightmap that was painted
// before pressing Play).
struct TerrainCollider {
    b3BodyId body{};
    b3HeightFieldData* heightField = nullptr;   // owned; must outlive the world
};

// Quantised Box3D height field from a row-major grid (countX columns along +x,
// countZ rows along +z, `cellSize` metres between samples). The grid's first
// sample is the local origin. Returns nullptr for grids smaller than 2x2 or
// non-finite input. The caller owns the result (b3DestroyHeightField).
b3HeightFieldData* BuildHeightField(const float* heights, int countX, int countZ, float cellSize);

// Creates the height field and a static body for `t`, aligned with the mesh
// BasicTerrain draws (including its world position). Returns false, leaving
// `out` untouched, if the terrain has no usable heightmap.
bool CreateTerrainCollider(b3WorldId world, const BasicTerrain& t, float friction, float restitution,
                           TerrainCollider& out);

// Frees the height field. Call only after the world that holds its shape has
// been destroyed (or the body removed).
void ReleaseTerrainCollider(TerrainCollider& c);

} // namespace terrain
