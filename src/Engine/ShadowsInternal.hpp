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

} // namespace shadow

namespace detail {
// Defined in Graphics.cpp: hands the freshly rendered light matrix to the lit, road and instanced programs.
void UploadLightViewProj(const Matrix& lightViewProj);
}

} // namespace gfx
