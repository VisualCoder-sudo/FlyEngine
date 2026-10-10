#pragma once
#include "raylib.h"
#include "Backend/ScatteredObject.hpp"

#include <string>
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

// Ambient lighting intensity (0..2 scales the default ambient term).
void SetAmbientIntensity(float intensity);

// Scene-wide lighting settings (the Lighting section of the Explorer). Saved with the scene.
struct LightingSettings {
    float timeOfDay = 12.0f;          // hours, 0..24 (12 = noon)
    float dayLengthMinutes = 0.0f;    // real minutes per 24 h; 0 = the time stays where it is set

    // Items inserted from the Explorer's Lighting "+" menu (Time is built in). A removed item goes back to its default.
    bool hasSun = false, hasAmbient = false, hasSky = false, hasFog = false;
    float sunAzimuth = 53.13f;        // degrees, the compass direction the sun shines FROM
    float sunElevation = 63.43f;      // degrees above the horizon (the old fixed sun)
    float sunIntensity = 1.0f;        // 0..3
    float sunColor[3] = { 1.0f, 1.0f, 1.0f };
    float ambient = 1.0f;             // 0..2: scales the ambient light
    float skyColor[3] = { 0.14f, 0.15f, 0.17f };   // the daytime sky (the editor's own clear colour is used until a Sky item is inserted)
    float fogDensity = 0.0f;          // per metre; the fog takes the sky colour
    float fogHeight = 0.0f;           // metres over which the fog thins out with height; 0 = the same at every height
    bool sunFollowsTime = true;       // the sun rises in the east, peaks at sunElevation at noon and sets in the west
    bool hasWeather = false;          // the Weather item
    float overcast = 0.0f;            // 0..1 cloud cover: dimmer, softer sun, grey sky, a little haze
    float rain = 0.0f;                // 0..1 rain: falling rain, wet ground, more cloud and haze
    float wetGround = 0.0f;           // 0..1 wet roads without rain (rain wets them too)

    // The Sky item can be one colour (skyColor, above) or a physical atmosphere: air that scatters the sun's light,
    // with a sun, a moon and stars in it (shaders/sky.glsl, src/Engine/Atmosphere.cpp).
    int skyMode = 0;                  // 0 = one colour, 1 = atmosphere
    float airColor[3] = { 0.175f, 0.410f, 1.0f };   // what the air scatters most: Earth's is blue
    float airDensity = 1.0f;          // 0..4: thin air is dark overhead, thick air is pale with deep red sunsets
    float haze = 2.0f;                // 0..12: dust and moisture, the white glow near the horizon and round the sun (1 = very clear air)
    float hazeColor[3] = { 1.0f, 1.0f, 1.0f };
    float ozone = 1.0f;               // 0..3: keeps the zenith blue at dusk
    float groundColor[3] = { 0.40f, 0.42f, 0.35f }; // the land below the horizon, seen from high up (it also colours the light bounced up off the ground)
    float sunSize = 1.0f;             // 1 = a little larger than the real sun's half a degree, as films and games show it
    float moonSize = 1.0f;
    float moonPhase = 0.5f;           // 0 = new, 0.5 = full, 1 = new again
    float moonLight = 1.0f;           // 0..4: how bright moonlit nights are
    float stars = 1.0f;               // 0..4

    // The Clouds item: a layer of volumetric cloud in the atmosphere sky (shaders/clouds.glsl). The Weather item's
    // cloud cover and rain add cloud to it (or bring some of their own when there is no Clouds item).
    bool hasClouds = false;
    float cloudCoverage = 0.45f;      // 0..1: how much of the sky is cloud
    float cloudDensity = 1.0f;        // 0.2..3: thin and bright, or thick and dark underneath
    float cloudBase = 1300.0f;        // metres: the height of the cloud base
    float cloudThickness = 1700.0f;   // metres: how tall the tallest clouds grow
    float cloudScale = 1.0f;          // 0.3..3: the size of the clouds
    float windSpeed = 14.0f;          // metres a second
    float windDirection = 70.0f;      // degrees, the compass direction the wind blows towards

    // The Picture item: how the lit scene becomes the image on screen (shaders/post.glsl).
    bool hasPicture = false;
    int toneCurve = 1;                // 0 = none (clip), 1 = neutral (colours stay as given), 2 = ACES filmic, 3 = AgX
    float exposure = 0.0f;            // stops, -4..4
    bool autoExposure = true;         // the eye adapts to the brightness of the view (only with an atmosphere sky)
    float bloom = 0.4f;               // 0..1: glow around the sun, lamps, lit windows
    float vignette = 0.0f;            // 0..1: darker corners
    float contrast = 1.0f;            // 0.5..1.5
    float saturation = 1.0f;          // 0..2
    float temperature = 0.0f;         // -1 cool .. 1 warm
    float filmGrain = 0.0f;           // 0..1
};
LightingSettings& Lighting();
void ResetLighting();                 // back to noon, no running cycle (a new scene starts like this)
void TickLighting(float dt);          // advances the time of day when a day length is set

// Time of day in hours (0..24, 12 = noon, the default and the unchanged look). Dims the sun and ambient
// light, darkens the sky and drives the emissive window/street light/car light glow of the city shaders.
void SetTimeOfDay(float hours);
float GetTimeOfDay();
float GetNightAmount();
float LampSwitchOn(float x, float z, float night);   // 0..1: is the street lamp at this spot on? Same rule as the shader (lamps come on one by one at dusk; a few flicker)
// Up to 32 point lights for the night glow on roads, buildings and props: (x, y, z, radius) each.
void SetNightLights(const Vector4* lights, int count);          // 0 = full day, 1 = full night
Color SkyColor(Color dayColor);  // the clear colour at the current time of day
void SetEngineClearColor(Color c);   // the engine's own background colour (used as the sky until a Sky item is inserted)
Color CurrentSky();              // the sky colour now: the Sky item (or the engine colour) darkened for the time of day

// What the scene's lighting currently is, for shaders that do not go through the lit shader (terrain, water).
// The scene is lit in linear light: a white surface in full noon sun comes to about 1.
Vector3 SunDirection();          // unit vector the sunlight travels along
Vector3 SunRadiance();           // linear light on a surface that faces the sun (colour x intensity x day/night x cloud)
Vector3 SunRadianceShaded();     // the same with the clouds' shadow averaged in, for shaders that do not read the cloud-shadow texture themselves
Vector3 AmbientSky();            // linear light from the sky on a surface that faces up
Vector3 AmbientGround();         // linear light bounced off the ground on a surface that faces down
Vector3 SkyRadiance();           // the sky's own linear light, for reflections (water, puddles)
Vector3 SunLightScale();         // the sun's colour and strength relative to the plain noon sun (about 1,1,1 at noon)
Vector3 AmbientScale();          // the sky's light relative to the default noon ambient
float FogDensity();              // per metre: the Fog item's fog plus the weather's haze
float WeatherHaze();             // per metre: the part of it that bad weather adds
Color FogColorNow();
float GetAmbientIntensity();
Vector3 WaterSky();              // the sky's linear light the water shader reflects when the reflection is off or far (day, dusk, night, cloud)
float SunElevationNow();         // degrees above the horizon of the sun along its path (negative = below, at night)
Vector3 SunPosition();           // unit vector towards the sun in the sky (it points down when the sun is below the horizon)
Vector3 MoonPosition();          // unit vector towards the moon
bool KeyLightIsMoon();           // the scene is lit, and its shadows cast, by the moon now (SunDirection() is then the moon's)
float KeyLightStrength();        // 0..1: how much of that light arrives (it fades out round sunrise and sunset)
float DayAmountNow();            // 1 in daylight, 0 at night
float WetnessNow();              // 0..1: how wet the ground is (rain or the Weather item's wet ground)
float RainNow();                 // 0..1 rain amount
float OvercastNow();             // 0..1 cloud cover (rain adds to it)
void DrawWeather(const Camera3D& camera);   // falling rain around the camera (call inside the 3D pass)

// Toggles the shadow-receiving ground plane.
void SetGridVisible(bool visible);
bool IsGridVisible();

// Toggles wireframe rendering for the whole 3D pass.
void SetWireframe(bool enabled);
bool IsWireframe();

void DrawGround(); // shadow-receiving ground plane (replaces DrawGrid)
// Things that can reach below the ground plane (a city on hills dips under y = 0) report their lowest point so the
// plane moves down out of their way instead of covering them. Each owner reports every frame it draws.
void SetGroundLimit(const void* owner, float lowestY);
void ClearGroundLimit(const void* owner);

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