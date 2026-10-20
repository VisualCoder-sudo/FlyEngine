#pragma once
#include "raylib.h"

// Shadow state shared between Shadows.cpp and the lit-shader uploads in Graphics.cpp.
namespace gfx {
namespace shadow {

constexpr int kMaxCascades = 4;
constexpr int kTextureSlot = 10;   // texture unit of the sun's shadow map in the lit shaders

void Init();       // after the window exists
void Shutdown();
void SetViewCamera(const Camera3D& camera);   // the shadow frustum follows this camera
void BindMap();                               // puts the first cascade's depth map on kTextureSlot
// The cascades' light view-projections for the lit shaders (column-major, one after another); returns how many are live.
int CascadeData(float out[16 * kMaxCascades]);
Texture2D FarMapDepth(int farIndex);          // depth map of far cascade 1 + farIndex

// Light-node shadow atlas (Shadows.cpp). Plan() hands out tiles to the shadow-casting lights (count of them in
// `casters`, in the order given); it fills firstTile[i] (-1 = no shadow) and texelAngle[i] (negative = a cube of
// six tiles). Data() returns the tile matrices (16 floats each) and rects (cx, cy, sx, sy in tile-adjusted NDC).
constexpr int kMaxNodeShadowTiles = 16;
struct NodeShadowRequest { Vector3 pos, dir; int kind; float outerAngleDeg; float range; int res; const void* host; };
void PlanNodeShadows(const NodeShadowRequest* requests, const bool* wantShadow, int count, int maxCasters,
                     float* firstTile, float* texelAngle);
int NodeShadowData(float* viewProj, float* rects);   // returns the number of tiles
Texture2D NodeShadowAtlas();                         // depth atlas (id 0 when there is none)

} // namespace shadow

namespace detail {
// Defined in Graphics.cpp: hands the freshly rendered light matrix to the lit, road and instanced programs.
void UploadLightViewProj(const Matrix& lightViewProj);
}

} // namespace gfx
