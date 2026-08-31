#pragma once
#include "raylib.h"
#include "ScatteredObject.hpp"

// Shared rendering resources: one lit/textured shader plus one unit-sized
// mesh per shape type, reused by every ScatteredObject instead of each
// object owning its own copy. Call Init() once after the window exists,
// Shutdown() once before it closes.
namespace gfx {

void Init();
void Shutdown();

Shader& GetLitShader();
Model& GetShapeModel(ShapeType type); // fits in a 1x1x1 box; scale it via the model's transform
Texture2D GetDefaultTexture();        // procedural checker texture, used when an object has none of its own

void UpdateLighting(const Camera3D& camera); // call once per frame before drawing
void SetUnderwaterParams(float waterY, Vector3 absorption, float fogDensity, Vector3 fogColor); // call per ocean per frame

// Directional shadow mapping. Wrap entity drawing in BeginShadowPass()/EndShadowPass()
// to render occluders from the light's point of view, then draw the scene normally
// with UpdateLighting() already called (the lit shader samples the generated shadow map).
void SetShadowsEnabled(bool enabled);
bool IsShadowsEnabled();
void BeginShadowPass();
void EndShadowPass();
bool IsInShadowPass(); // true while BeginShadowPass()/EndShadowPass() is active

// Shadow map quality on a 5..100 scale (5 = lowest, 100 = highest).
// Higher values re-create the shadow map at a higher resolution.
void SetShadowQuality(int quality);
int GetShadowQuality();

// Ambient lighting intensity (0..2 scales the default ambient term).
void SetAmbientIntensity(float intensity);
float GetAmbientIntensity();

// Toggles the shadow-receiving ground plane.
void SetGridVisible(bool visible);
bool IsGridVisible();

// Toggles wireframe rendering for the whole 3D pass.
void SetWireframe(bool enabled);
bool IsWireframe();

void DrawGround(); // shadow-receiving ground plane (replaces DrawGrid)

} // namespace gfx