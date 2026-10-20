#pragma once
#include "raylib.h"
#include "../Engine/Backend/ScatteredObject.hpp"
#include "Lighting.hpp"

#include <string>
#include <vector>

// Shared rendering resources: one lit/textured shader plus one unit-sized
// mesh per shape type, reused by every ScatteredObject instead of each
// object owning its own copy. Call Init() once after the window exists,
// Shutdown() once before it closes.
namespace gfx {

void Init();
void Shutdown();

// Reflection target lifetime; Init()/Shutdown() call these (src/Engine/Reflections.cpp).
void InitReflections();
void ShutdownReflections();

// Model matrix from a scale, an already-computed rotation and a translation:
// MatrixMultiply(MatrixMultiply(MatrixScale(s...), rot), MatrixTranslate(p...)).
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

// Loose shape batching: between BeginShapeBatch() and EndShapeBatch(), QueueShape() collects opaque
// shapes and EndShapeBatch() draws each (shape, texture) group in one instanced call. QueueShape()
// returns false when batching is off or unavailable; the caller then draws the shape itself.
// EndShapeBatch() must run inside the same pass (and before any state that changes it) as the queueing.
void BeginShapeBatch();
bool QueueShape(ShapeType shape, Texture2D texture, const Matrix& transform, Color tint);
void EndShapeBatch();
// Defers a hover outline until EndShapeBatch() has drawn the batched shapes. False when not batching.
bool QueueShapeOutline(ShapeType shape, const Matrix& transform);

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

// Shadow cascades: the map above is a box of ground round the camera; with more than one cascade
// (Preferences > Rendering) further, wider boxes carry shadows out into the distance. Draw the
// casters once per cascade:
//     for (int c = 0; c < ShadowCascadeCount(); c++) {
//         if (!BeginShadowCascade(c)) continue;     // this cascade keeps its map this frame
//         ...draw...
//         EndShadowCascade();
//     }
// Cascade 0 is BeginShadowPass()/EndShadowPass(); the further ones are redrawn every few frames.
int ShadowCascadeCount();
bool BeginShadowCascade(int index);
void EndShadowCascade();

// The sun's shadow map, for passes outside the lit shaders (the shafts of light in fog).
Texture2D GetShadowMapTexture();     // depth; read through a comparison sampler. id 0 when shadows are off
Matrix GetLightViewProj();
float GetShadowRange();              // how far from the camera (m) the map reaches

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

// Up to 32 point lights for the night glow on roads, buildings and props: (x, y, z, radius) each.
void SetNightLights(const Vector4* lights, int count);          // 0 = full day, 1 = full night

// Light nodes (SpotLight / PointLight / SurfaceLight): at most 32 lights, uploaded to the lit shaders by UpdateLighting.
struct NodeLight {
    Vector3 pos{};          // world position
    float range = 16.0f;    // metres
    Vector3 dir{};          // unit axis it points along (spot, surface)
    int kind = 0;           // 0 point, 1 spot, 2 surface
    Vector3 color{};        // linear colour times brightness
    int falloff = 1;        // 0 linear, 1 inverse square
    float cosOuter = 0.0f;  // cone edge
    float cosInner = 0.0f;  // cone where the light is at full strength
    Vector3 u{}, v{};       // surface: half-extent of the emitter along its two sides (world)
    bool castShadows = false;
    int shadowRes = 1024;   // shadow map size for this light (a point light's six faces each get this)
    const void* host = nullptr;   // the object carrying the light: it does not shadow its own light
};
constexpr int kMaxNodeLights = 32;
void SetNodeLights(const NodeLight* lights, int count);   // call before the shadow passes and UpdateLighting each frame

// Shadows of light nodes: every shadow-casting light gets one tile (a point light six) of one shared depth atlas.
// SetNodeLights() hands the tiles out; the engine then draws the scene once into each:
//     for (int t = 0; t < NodeShadowTileCount(); t++) {
//         if (!BeginNodeShadowTile(t)) continue;
//         ... draw the casters (the same entity loop as a shadow cascade) ...
//         EndNodeShadowTile();
//     }
int NodeShadowTileCount();
bool BeginNodeShadowTile(int index);
void EndNodeShadowTile();
const void* NodeShadowSkipHost();   // during a tile pass: the object that must not cast (the light's own host)

// Decal / Texture child nodes: an image projected onto one face of an object's mesh. ScatteredObject::Draw queues
// them; the engine draws the queue (DrawNodeSurfaces) right after the opaque objects, so they sit on the surface.
struct NodeSurface {
    const Model* model = nullptr;       // the mesh(es) to draw again
    Matrix transform{};                 // the object's model matrix
    Vector3 size{};                     // the object's size in metres
    Vector3 faceN{}, faceU{}, faceV{};  // the face, in the object's local axes
    bool tiled = false;                 // true: a repeating Texture; false: one Decal over the whole face
    Vector2 metresPerTile{ 4.0f, 4.0f };
    Vector2 offset{};                   // in tiles
    Color tint = WHITE;                 // a = opacity
    Texture2D texture{};                // id 0: the checker texture
};
void QueueNodeSurface(const NodeSurface& surface);
void DrawNodeSurfaces();                // draws and clears the queue
void DrawWeather(const Camera3D& camera);   // falling rain around the camera (call inside the 3D pass)

// Toggles the shadow-receiving ground plane.
void SetGridVisible(bool visible);
bool IsGridVisible();

// Toggles wireframe rendering for the whole 3D pass.
void SetWireframe(bool enabled);
bool IsWireframe();

// Hover outline (shaders/outline.glsl): draws the model again as a flat-colour rim `thickness` pixels wide around
// its silhouette. `transform` replaces model.transform. Uses the colour and thickness from Quality().
void DrawModelOutline(const Model& model, const Matrix& transform);

// A progress message shown on screen while something slow loads in the background (a big city building), e.g.
// "City attempting to load [14%]" with a thin progress bar. Each owner sets its message every frame it is loading and
// clears it when done. Engine::Draw draws them in the 2D pass, centred in `area` (the whole window in the player).
void SetLoadingStatus(const void* owner, const std::string& text, float fraction);
void ClearLoadingStatus(const void* owner);
void DrawLoadingStatus(Rectangle area);
int LoadingStatusCount();                       // how many messages are showing (tests)
std::string LoadingStatusText(int index);

// Planar water reflections. BeginReflectionPass() mirrors the view camera about a
// horizontal plane (water height) and returns the mirrored camera. Wrap the opaque
// scene render in BeginMode3D(mirrored)/EndMode3D(), then call EndReflectionPass().
// Water shaders sample GetReflectionTarget().texture using GetReflectionViewProj().
void SetReflectionsEnabled(bool enabled);
bool IsReflectionsEnabled();
Camera3D BeginReflectionPass(float planeHeight, const Camera3D& worldCamera, Color clearColor);
// Call right after BeginMode3D(mirrored): clips geometry below the water plane.
void ClipReflectionPassToWater();
void EndReflectionPass();
bool IsInReflectionPass();
Matrix GetReflectionViewProj();        // view-projection of the mirrored camera
RenderTexture2D GetReflectionTarget(); // color image rendered during the reflection pass
int GetReflectionTextureSlot();
// Hands a water shader the sun's shadow cascades (sunDir, shadowVP, shadowMap*), so water in shade gets no sun glints.
void UploadWaterShadows(Shader& shader);
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
// Adds every mesh of `model` to this frame's triangle count (call after drawing it).
void AddModelTriangles(const Model& model);

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
    double skyMs = 0.0;            // sky, atmosphere tables, cloud and fog passes
    double opaqueMs = 0.0;
    double transparentMs = 0.0;
    double postMs = 0.0;           // everything between the lit scene and the picture
    double twoDMs = 0.0;
};
void SetFrameTimings(const FrameTimings& timings);
FrameTimings GetFrameTimings();

// Global frustum culling toggle (for A/B comparison)
bool GetFrustumCullingEnabled();
void SetFrustumCullingEnabled(bool enabled);

} // namespace gfx