#pragma once
#include "raylib.h"
#include "Backend/ScatteredObject.hpp"

#include <vector>

// Shared rendering resources: one lit/textured shader plus one unit-sized
// mesh per shape type, reused by every ScatteredObject instead of each
// object owning its own copy. Call Init() once after the window exists,
// Shutdown() once before it closes.
namespace gfx {

void Init();
void Shutdown();

Shader& GetLitShader();
Shader& GetRoadShader(); // lit shader with a baked depth bias (roads win against the ground plane)
Model& GetShapeModel(ShapeType type); // fits in a 1x1x1 box; scale it via the model's transform
Texture2D GetDefaultTexture();        // procedural checker texture, used when an object has none of its own

// Instanced city buildings: draws `count` unit building meshes from the
// transform buffer at `start` tinted `tint`, using the instanced variant of
// the lit shader (instanceTransform supplies the model matrix).
void DrawCityInstances(Mesh mesh, const std::vector<Matrix>& transforms, int start, int count, Color tint);
Mesh GetCityShapeMesh(int shape); // unit building mesh: 0=box,1=wedge(corner),2=slant

// Persistent GPU instance buffers for the city: a tile uploads its building
// transforms ONCE (CreateInstanceBuffer) and every frame just draws from that
// buffer, instead of raylib re-uploading every visible transform on each pass.
// The tint rides in each transform exactly like DrawCityInstances().
unsigned int CreateInstanceBuffer(const std::vector<Matrix>& transforms); // 0 on failure
void DestroyInstanceBuffer(unsigned int id);
bool InstanceBuffersActive();          // enabled AND supported by this GL/shader setup
void SetInstanceBuffersEnabled(bool enabled);
bool GetInstanceBuffersEnabled();
void DrawCityInstancesBuffered(Mesh mesh, unsigned int instanceVbo, int count, Color tint);

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

// Shadow-map reuse. While the scene is idle (no input, not playing, camera and
// light frustum unchanged, nothing called MarkShadowsDirty) the previous depth
// map is kept: BeginShadowPass()/EndShadowPass() then draw nothing into it, and
// IsShadowPassReused() is true so heavy casters (the city) can skip submitting.
void SetShadowReuseEnabled(bool enabled);
bool GetShadowReuseEnabled();
void MarkShadowsDirty();   // something that casts shadows changed: re-render next pass
bool IsShadowPassReused(); // true inside a shadow pass whose depth map is being kept

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

// Planar water reflections. BeginReflectionPass() mirrors the view camera about a
// horizontal plane (water height) and returns the mirrored camera. Wrap the opaque
// scene render in BeginMode3D(mirrored)/EndMode3D(), then call EndReflectionPass().
// Water shaders sample GetReflectionTarget().texture using GetReflectionViewProj().
void SetReflectionsEnabled(bool enabled);
bool IsReflectionsEnabled();
Camera3D BeginReflectionPass(float planeHeight, const Camera3D& worldCamera, Color clearColor);
void EndReflectionPass();
bool IsInReflectionPass();
Matrix GetReflectionViewProj();        // view-projection of the mirrored camera
RenderTexture2D GetReflectionTarget(); // color image rendered during the reflection pass
int GetReflectionTextureSlot();
void SetReflectionQuality(int quality); // 5..100 (recreates the target at a higher resolution)
int GetReflectionQuality();

} // namespace gfx