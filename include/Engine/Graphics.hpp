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

// Builds a raylib model matrix from a scale, an already-computed rotation, and
// a translation. Equivalent to
//     MatrixMultiply(MatrixMultiply(MatrixScale(s...), rot), MatrixTranslate(p...))
// which was open-coded at four sites in ScatteredObject.cpp -- including the
// selection-outline pass, which needed a separate slightly-enlarged scale and
// so could not be folded into a naive find-and-replace.
//
// Rotation is a Matrix rather than Euler angles because the callers already
// have one in hand (a render-rotation override, or MatrixRotateXYZ), and
// recomputing it here would change the call order of the rotations.
Matrix ComposeTRS(Vector3 scale, Matrix rotation, Vector3 position);

Shader& GetLitShader();
Shader& GetRoadShader(); // lit shader with a baked depth bias (roads win against the ground plane)
Model& GetShapeModel(ShapeType type); // fits in a 1x1x1 box; scale it via the model's transform
Texture2D GetDefaultTexture();        // procedural checker texture, used when an object has none of its own

// Instanced city buildings: draws `count` unit building meshes from the
// transform buffer at `start` tinted `tint`, using the instanced variant of
// the lit shader (instanceTransform supplies the model matrix).
void DrawCityInstances(Mesh mesh, const std::vector<Matrix>& transforms, int start, int count, Color tint);
Mesh GetCityShapeMesh(int shape); // unit building mesh: 0=box,1=wedge(corner),2=slant,3=gable,4=stepped tower

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

// raylib's Material.maps is a bare pointer with no length field, and the
// MAX_MATERIAL_MAPS constant that older raylib exported no longer exists -- the
// current header only mentions it in a comment. Deriving the length from the
// last enumerator keeps it correct if the enum ever grows, instead of baking in
// a literal that would quietly under- or over-read.
constexpr int kMaterialMapCount = MATERIAL_MAP_BRDF + 1;

// Per-frame render statistics. The player debug overlay reads these, so they
// exist to answer "what did this frame cost" rather than to be exhaustive
// instrumentation.
//
// Coverage is partial by nature: the counters are fed from the engine's own
// draw sites (the city instance batches, and the per-entity Draw() loop), not
// from inside raylib. Anything drawn by raylib on our behalf without going
// through those sites is not counted. Draw calls in particular were already
// approximate -- the per-entity loop charges one call per entity regardless of
// how many meshes that entity actually submits -- and the mesh count inherits
// that same approximation. Treat these as a relative signal between frames,
// not as GPU-truth.
int GetDrawCallCount();
int GetTriangleCount();

// Feed the draw-call, mesh and triangle counters. Call these at the same place
// you issue a draw.
void IncrementDrawCallCount(int count = 1);
void AddMeshCount(int count = 1);
void AddTriangleCount(int count);

// Meshes submitted this frame.
int GetMeshCount();

// Distinct textures and materials touched this frame. "Distinct" matters: a
// city tile drawn from 300 instances touches one texture 300 times, and
// reporting 300 tells you nothing. Ids are deduplicated with a fixed-capacity
// set, so a frame that touches more than the cap undercounts rather than
// allocating; GetStatsSaturated() reports when that happened so a clamped
// number is visible instead of silently wrong.
int GetTextureCount();
int GetMaterialCount();
bool GetStatsSaturated();

// Feed the texture/material counters. Ids at or below 0 are ignored, so callers
// can pass an unset Texture2D.id or a 0 shader id without guarding.
void NoteTextureBound(int id);
void NoteMaterialUsed(int id);

// Frustum culling statistics (for debug overlay)
int GetCulledEntityCount();
int GetRenderedEntityCount();
void IncrementCulledEntityCount(int count = 1);
void IncrementRenderedEntityCount(int count = 1);

// Call once at the top of the frame, before any drawing.
void ResetFrameStats();

// Per-pass wall-clock cost of the last completed frame, in milliseconds.
// Engine::Draw() publishes these because it already calls GetTime() around
// every pass to fill in Engine::lastShadowMs and friends; without a copy on
// this side the numbers were computed and then only reachable from the stress
// harness, which is the one program that is not trying to find out why the
// editor feels slow.
struct FrameTimings {
    double shadowMs = 0.0;
    double reflectionMs = 0.0;
    double opaqueMs = 0.0;
    double transparentMs = 0.0;
    double twoDMs = 0.0;
};
void SetFrameTimings(const FrameTimings& timings);
FrameTimings GetFrameTimings();

// Global frustum culling toggle (for A/B comparison)
bool GetFrustumCullingEnabled();
void SetFrustumCullingEnabled(bool enabled);

} // namespace gfx