#include "../../include/Engine/Graphics.hpp"
#include "../../include/Engine/TechnicalTools.hpp"
#include "raymath.h"
#include "rlgl.h"
#include "../../include/Engine/Frontend/ui.hpp"
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

int shadowMapResolution = 2048;
int shadowQuality = 52;
constexpr int SHADOW_TEXTURE_SLOT = 10;

int reflectionResolution = 768;
int reflectionQuality = 50;
constexpr int REFLECTION_TEXTURE_SLOT = 11;
bool reflectionEnabled = true;
bool inReflectionPass = false;
RenderTexture2D reflectionTarget{};
Matrix reflView = MatrixIdentity();
Matrix reflProj = MatrixIdentity();
Matrix reflViewProj = MatrixIdentity();

int QualityToResolution(int quality) {
    // Shadow acne fix (3): raise the texel budget for a given quality slider.
    // Base shifted 8 -> 9 (log2), so default 52 now yields 2048 (4x texels of
    // the old 1024) - the generated shadow map is sharper and each acne stripe
    // is a few texels narrower, cutting the visible flicker. Low 512 barely
    // differs; High reaches 8192 for crisp silhouettes from up high.
    quality = Clamp(quality, 5, 100);
    float t = (float)(quality - 5) / 95.0f;
    int log2 = 9 + (int)std::lroundf(t * 4.0f);
    return 1 << log2;
}

int ReflectionQualityToResolution(int quality) {
    quality = Clamp(quality, 5, 100);
    float t = (float)(quality - 5) / 95.0f;
    return 360 + (int)std::lroundf(t * 1080.0f); // 360..1440
}

const Vector3 kAmbient = { 0.35f, 0.35f, 0.35f };
gfx::LightingSettings lightingSettings;
Color engineClear = { 245, 245, 245, 255 };
float Smooth01(float a, float b, float x) { const float t = fminf(fmaxf((x - a) / (b - a), 0.0f), 1.0f); return t * t * (3.0f - 2.0f * t); }
// 1 in daylight, 0 at night, easing through dawn (5:00-7:30) and dusk (16:30-19:00).
float DayAmount() {
    const float h = fmodf(fmodf(lightingSettings.timeOfDay, 24.0f) + 24.0f, 24.0f);
    return Smooth01(5.0f, 7.5f, h) * (1.0f - Smooth01(16.5f, 19.0f, h));
}
// The sun's height above the horizon along its path (degrees): up at 6:00, the Sun item's height at noon, down at 18:00.
float RawSunElevation() {
    if (!lightingSettings.sunFollowsTime) return fminf(fmaxf(lightingSettings.sunElevation, 8.0f), 90.0f);
    const float h = fmodf(fmodf(lightingSettings.timeOfDay, 24.0f) + 24.0f, 24.0f);
    const float peak = fminf(fmaxf(lightingSettings.sunElevation, 8.0f), 90.0f);
    return peak * sinf((h - 6.0f) / 12.0f * PI);    // negative at night
}
// The compass direction the sun shines from (it moves 15 degrees an hour when it follows the time).
float SunAzimuthNow() {
    if (!lightingSettings.sunFollowsTime) return lightingSettings.sunAzimuth;
    const float h = fmodf(fmodf(lightingSettings.timeOfDay, 24.0f) + 24.0f, 24.0f);
    return lightingSettings.sunAzimuth + (h - 12.0f) * 15.0f;
}
// Unit vector the sunlight travels along. At night the light is moonlight, kept well above the horizon so the
// dim shadows stay short.
Vector3 SunDir() {
    float azd = SunAzimuthNow(), eld = RawSunElevation();
    if (lightingSettings.sunFollowsTime) {
        const float night = Smooth01(0.5f, 1.0f, 1.0f - DayAmount());
        eld = fmaxf(eld, 8.0f) * (1.0f - night) + 38.0f * night;
        azd = roundf(azd * 2.0f) * 0.5f;                              // half-degree steps: shadows are not redrawn for tiny moves
        eld = roundf(eld * 2.0f) * 0.5f;
    }
    const float az = azd * DEG2RAD;
    const float el = fminf(fmaxf(eld, 8.0f), 90.0f) * DEG2RAD;
    return Vector3Normalize({ -cosf(el) * sinf(az), -sinf(el), -cosf(el) * cosf(az) });
}
// Cloud cover and rain, 0..1 (rain brings its own cloud).
float OvercastAmount() {
    if (!lightingSettings.hasWeather) return 0.0f;
    return fminf(fmaxf(fmaxf(lightingSettings.overcast, lightingSettings.rain * 0.9f), 0.0f), 1.0f);
}
float RainAmount() { return lightingSettings.hasWeather ? fminf(fmaxf(lightingSettings.rain, 0.0f), 1.0f) : 0.0f; }
float WetAmount() { return lightingSettings.hasWeather ? fminf(fmaxf(fmaxf(lightingSettings.wetGround, lightingSettings.rain), 0.0f), 1.0f) : 0.0f; }
bool gridVisible = true;
bool wireframe = false;
bool inShadowPass = false;
// Shadow-map reuse state.
bool shadowReuseEnabled = true;
std::atomic<bool> shadowsDirty{ true };
bool shadowPassReused = false;
float shadowStableHalf = 0.0f; // hysteresis-held shadow box half-size (m)
bool shadowHaveRendered = false;
Vector3 shadowLastCenter{};
float shadowLastHalf = 0.0f;
int shadowLastRes = 0;
int shadowFramesSinceRender = 0;
int shadowInputHold = 0;
constexpr int kShadowMaxAge = 90;   // force a refresh at least this often (frames)
constexpr int kShadowInputHold = 3; // keep re-rendering this many frames after input
// The depth map is only reused after the view and the scene have been still this long. Reuse refreshes
// every kShadowMaxAge frames, which looks like shadows updating once a second (cars, pedestrians), so
// it must not start the moment the camera stops.
constexpr double kShadowStillSeconds = 30.0;
double shadowStillSince = 0.0;

// Any input that could edit the scene (or move the camera) this frame.
bool SceneInputActivity() {
    for (int b = 0; b < 7; b++)
        if (IsMouseButtonDown(b) || IsMouseButtonPressed(b) || IsMouseButtonReleased(b)) return true;
    if (GetMouseWheelMove() != 0.0f) return true;
    for (int k = 32; k <= 348; k++)
        if (IsKeyDown(k) || IsKeyPressed(k) || IsKeyReleased(k)) return true;
    return false;
}

// Persistent instance buffers.
bool instanceBuffersEnabled = true;

// Raise the camera near plane with altitude: depth precision grows ~1/near, so a
// higher camera can afford a much larger near plane and far-away layers stop
// z-fighting. rlSetClipPlanes()/rlGetCullDistanceFar() only exist from raylib
// 5.5 onward; on older raylib this is a no-op -- the road shader's bias still
// adapts on its own.
#if defined(RAYLIB_VERSION_MAJOR) && \
    (RAYLIB_VERSION_MAJOR > 5 || (RAYLIB_VERSION_MAJOR == 5 && RAYLIB_VERSION_MINOR >= 5))
void ApplyNearPlane(double nearP) {
    rlSetClipPlanes(nearP, rlGetCullDistanceFar());
}
#else
void ApplyNearPlane(double) {}
#endif

Camera3D shadowViewCamera{};   // last camera seen by UpdateLighting (the shadow frustum follows it)
bool haveShadowViewCamera = false;
constexpr float kShadowNear = 1.0f;
constexpr float kShadowFar = 430.0f;
constexpr float kShadowLightDist = 330.0f; // light eye distance from the frustum centre
constexpr float kShadowMinHalf = 40.0f;
constexpr float kShadowMaxHalf = 150.0f;

Mesh cityWedgeMesh{}; // unit corner wedge for angled parcels
Mesh citySlantMesh{}; // unit sheared slab for silhouette variety
Mesh cityGableMesh{}; // unit box with a pitched roof
Mesh cityTowerMesh{}; // unit stepped tower
Mesh cityShedMesh{};
Mesh cityBusMesh{}, cityBusGlassMesh{};
Mesh cityBenchMesh{}, cityHydrantMesh{}, cityBollardMesh{}, cityBusStopMesh{}, citySignMesh{};   // sidewalk furniture
Mesh cityLampMesh{}, cityTreeMesh{}, cityCarMesh{}, cityCarGlassMesh{}, cityWheelMesh{}, cityPersonMesh{}, citySignalMesh{}; // props and agents

Shader litShader{};
Texture2D defaultTexture{};
Model cubeModel{};
Model sphereModel{};
Model cylinderModel{};
Model wedgeModel{};
Model groundModel{};
Texture2D groundTexture{};
int roadWeatherLoc = -1, roadCamLoc = -1, cityWeatherLoc = -1;
int lightDirLoc = -1;
int ambientLoc = -1;
int lightVPLoc = -1;
int shadowMapLoc = -1;
int shadowsEnabledLoc = -1;
// Instanced-lit shader + material used for city buildings.
Shader cityInstancedShader{};
Material cityInstancedMaterial{};
bool cityInstancedReady = false;
int cityLightDirLoc = -1;
int cityAmbientLoc = -1;
int cityLightVPLoc = -1;
int cityNightLoc = -1;
int litSunLoc = -1, litFogLoc = -1, roadSunLoc = -1, roadFogLoc = -1, citySunLoc = -1, cityFogLoc = -1;
int cityLightCountLoc = -1, cityLightsLoc = -1;
int roadNightLoc = -1, roadLightCountLoc = -1, roadLightsLoc = -1;
float nightLightData[32 * 4] = {};
int nightLightCount = 0;
int cityShadowMapLoc = -1;
int cityShadowsEnabledLoc = -1;
int cityWaterSurfaceYLoc = -1;
// Road shader: lit fragment shader with a depth-biased vertex stage, used only
// for the flat road/pad/parks mesh so it wins the depth test against the
// near-coplanar ground plane at altitude.
Shader roadShader{};
int roadLightDirLoc = -1;
int roadAmbientLoc = -1;
int roadLightVPLoc = -1;
int roadShadowMapLoc = -1;
int roadShadowsEnabledLoc = -1;
int roadWaterSurfaceYLoc = -1;
// Underwater uniforms
int waterSurfaceYLoc = -1;
int waterAbsorptionLoc = -1;
int waterFogDensityLoc = -1;
int waterFogColorLoc = -1;
bool initialized = false;
bool shadowsEnabled = true;

RenderTexture2D shadowMap{};
Camera3D lightCamera{};
Matrix lightView = MatrixIdentity();
Matrix lightProj = MatrixIdentity();
Matrix lightViewProj = MatrixIdentity();

// The lit, road and instanced shaders live in shaders/lit.glsl and are compiled
// into the binary by sokol-shdc (programs "lit", "lit_road", "lit_instanced").

Texture2D GenerateGridTexture() {
    const int texSize = 400;
    const int lineSpacing = 10;

    Image img = GenImageColor(texSize, texSize, Color{ 32, 34, 40, 255 });
    const Color lineColor = { 56, 60, 70, 255 };
    for (int i = 0; i <= texSize; i += lineSpacing) {
        ImageDrawLine(&img, i, 0, i, texSize - 1, lineColor);
        ImageDrawLine(&img, 0, i, texSize - 1, i, lineColor);
    }

    Texture2D texture = LoadTextureFromImage(img);
    UnloadImage(img);

    GenTextureMipmaps(&texture);
    SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);

    return texture;
}

Mesh GenerateWedgeMesh() {
    Mesh mesh = { 0 };

    const int vertexCount = 18;
    const int indexCount = 24;

    float vertices[] = {
        -0.5f, -0.5f, -0.5f,
        -0.5f,  0.5f, -0.5f,
         0.5f, -0.5f, -0.5f,
        -0.5f, -0.5f,  0.5f,
         0.5f, -0.5f, 0.5f,
        -0.5f,  0.5f,  0.5f,
        -0.5f, -0.5f, -0.5f,
         0.5f, -0.5f, -0.5f,
         0.5f, -0.5f,  0.5f,
        -0.5f, -0.5f,  0.5f,
        -0.5f, -0.5f, -0.5f,
        -0.5f, -0.5f,  0.5f,
        -0.5f,  0.5f,  0.5f,
        -0.5f,  0.5f, -0.5f,
        0.5f, -0.5f, -0.5f,
        -0.5f,  0.5f, -0.5f,
        -0.5f,  0.5f, 0.5f,
        0.5f, -0.5f,  0.5f,
    };

    float texcoords[] = {
        0.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 0.0f,
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        0.0f, 0.0f,
        1.0f, 0.0f,
        1.0f, 1.0f,
        0.0f, 1.0f,
        0.0f, 0.0f,
        1.0f, 0.0f,
        1.0f, 1.0f,
        0.0f, 1.0f,
        0.0f, 0.0f,
        1.0f, 0.0f,
        1.0f, 1.0f,
        0.0f, 1.0f,
    };

    float normals[] = {
        0.0f,  0.0f, -1.0f,
         0.0f,  0.0f, -1.0f,
         0.0f,  0.0f, -1.0f,
        0.0f,  0.0f,  1.0f,
         0.0f,  0.0f,  1.0f,
         0.0f,  0.0f,  1.0f,
        0.0f, -1.0f,  0.0f,
         0.0f, -1.0f,  0.0f,
         0.0f, -1.0f,  0.0f,
         0.0f, -1.0f,  0.0f,
        -1.0f, 0.0f, 0.0f,
        -1.0f,  0.0f,  0.0f,
        -1.0f, 0.0f,  0.0f,
        -1.0f,  0.0f,  0.0f,
        0.7071f,  0.7071f, 0.0f,
         0.7071f,  0.7071f, 0.0f,
         0.7071f,  0.7071f, 0.0f,
         0.7071f,  0.7071f, 0.0f,
    };

    unsigned short indices[] = {
         0,  1,  2,
         3,  4,  5,
         6,  7,  8,  6,  8,  9,
        10, 11, 12, 10, 12, 13,
        14, 15, 16, 14, 16, 17,
    };

    mesh.vertexCount = vertexCount;
    mesh.triangleCount = indexCount / 3;

    mesh.vertices = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    memcpy(mesh.vertices, vertices, vertexCount * 3 * sizeof(float));

    mesh.texcoords = (float*)RL_MALLOC(vertexCount * 2 * sizeof(float));
    memcpy(mesh.texcoords, texcoords, vertexCount * 2 * sizeof(float));

    mesh.normals = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    memcpy(mesh.normals, normals, vertexCount * 3 * sizeof(float));

    mesh.indices = (unsigned short*)RL_MALLOC(indexCount * sizeof(unsigned short));
    memcpy(mesh.indices, indices, indexCount * sizeof(unsigned short));

    UploadMesh(&mesh, false);

    return mesh;
}

// Corner wedge: a right-isoceles triangular prism (unit cube footprint, apex at
// (0.5, 0.5)). Fills the triangular parcel where two angled streets meet.
Mesh GenerateCityWedgeMesh() {
    Mesh mesh = { 0 };

    const float invSqrt2 = 0.70710678f;
    const int vertexCount = 18;
    const int indexCount = 24;

    float vertices[vertexCount * 3] = {
        // bottom face (A,C,B) y=-0.5
         0.5f, -0.5f,  0.5f,    -0.5f, -0.5f,  0.5f,     0.5f, -0.5f, -0.5f,
        // top face (a,b,c) y=+0.5
         0.5f,  0.5f,  0.5f,     0.5f,  0.5f, -0.5f,    -0.5f,  0.5f,  0.5f,
        // +x leg (A,B,b,a)
         0.5f, -0.5f,  0.5f,     0.5f, -0.5f, -0.5f,
         0.5f,  0.5f, -0.5f,     0.5f,  0.5f,  0.5f,
        // +z leg (A,a,c,C)
         0.5f, -0.5f,  0.5f,     0.5f,  0.5f,  0.5f,
        -0.5f,  0.5f,  0.5f,    -0.5f, -0.5f,  0.5f,
        // hypotenuse (B,C,c,b)
         0.5f, -0.5f, -0.5f,    -0.5f, -0.5f,  0.5f,
        -0.5f,  0.5f,  0.5f,     0.5f,  0.5f, -0.5f,
    };
    float normals[vertexCount * 3] = {
        0.0f, -1.0f, 0.0f,   0.0f, -1.0f, 0.0f,   0.0f, -1.0f, 0.0f,
        0.0f,  1.0f, 0.0f,   0.0f,  1.0f, 0.0f,   0.0f,  1.0f, 0.0f,
        1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f,
        -invSqrt2, 0.0f, -invSqrt2,   -invSqrt2, 0.0f, -invSqrt2,
        -invSqrt2, 0.0f, -invSqrt2,   -invSqrt2, 0.0f, -invSqrt2,
    };
    float texcoords[vertexCount * 2] = {
        0.0f, 0.0f,   1.0f, 0.0f,   0.0f, 1.0f,
        0.0f, 0.0f,   1.0f, 0.0f,   0.0f, 1.0f,
        0.0f, 0.0f,   1.0f, 0.0f,   1.0f, 1.0f,   0.0f, 1.0f,
        0.0f, 0.0f,   1.0f, 0.0f,   1.0f, 1.0f,   0.0f, 1.0f,
        0.0f, 0.0f,   1.0f, 0.0f,   1.0f, 1.0f,   0.0f, 1.0f,
    };
    unsigned short indices[indexCount] = {
        0, 1, 2,
        3, 5, 4,
        6, 8, 7,  6, 9, 8,
        10, 12, 11,  10, 13, 12,
        14, 16, 15,  14, 17, 16,
    };

    mesh.vertexCount = vertexCount;
    mesh.triangleCount = indexCount / 3;
    mesh.vertices = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    memcpy(mesh.vertices, vertices, vertexCount * 3 * sizeof(float));
    mesh.texcoords = (float*)RL_MALLOC(vertexCount * 2 * sizeof(float));
    memcpy(mesh.texcoords, texcoords, vertexCount * 2 * sizeof(float));
    mesh.normals = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    memcpy(mesh.normals, normals, vertexCount * 3 * sizeof(float));
    mesh.colors = (unsigned char*)RL_MALLOC(vertexCount * 4 * sizeof(unsigned char));
    for (int i = 0; i < vertexCount * 4; i++) mesh.colors[i] = 255;
    mesh.indices = (unsigned short*)RL_MALLOC(indexCount * sizeof(unsigned short));
    memcpy(mesh.indices, indices, indexCount * sizeof(unsigned short));

    UploadMesh(&mesh, false);

    return mesh;
}

// Sheared slab: unit footprint with its top pushed +z by 0.35 (a parallelogram
// cross-section), giving buildings a slanted rather than boxy silhouette.
Mesh GenerateCitySlantMesh() {
    Mesh mesh = { 0 };

    const float sh = 0.35f;
    const int vertexCount = 24;
    const int indexCount = 36;

    float vertices[vertexCount * 3] = {
        // bottom (B0,B1,B2,B3)
        -0.5f, -0.5f, -0.5f,     0.5f, -0.5f, -0.5f,     0.5f, -0.5f,  0.5f,    -0.5f, -0.5f,  0.5f,
        // top (T3,T2,T1,T0)
        -0.5f,  0.5f,  0.5f + sh,  0.5f,  0.5f,  0.5f + sh,  0.5f,  0.5f, -0.5f + sh,   -0.5f,  0.5f, -0.5f + sh,
        // +x (B1,B2,T2,T1)
         0.5f, -0.5f, -0.5f,     0.5f, -0.5f,  0.5f,     0.5f,  0.5f,  0.5f + sh,    0.5f,  0.5f, -0.5f + sh,
        // -x (B3,B0,T0,T3)
        -0.5f, -0.5f,  0.5f,    -0.5f, -0.5f, -0.5f,    -0.5f,  0.5f, -0.5f + sh,   -0.5f,  0.5f,  0.5f + sh,
        // +z ramp (B2,T2,T3,B3)
         0.5f, -0.5f,  0.5f,     0.5f,  0.5f,  0.5f + sh,  -0.5f,  0.5f,  0.5f + sh,   -0.5f, -0.5f,  0.5f,
        // -z ramp (B0,T0,T1,B1)
        -0.5f, -0.5f, -0.5f,    -0.5f,  0.5f, -0.5f + sh,   0.5f,  0.5f, -0.5f + sh,    0.5f, -0.5f, -0.5f,
    };
    float normals[vertexCount * 3] = {
        0.0f, -1.0f, 0.0f,   0.0f, -1.0f, 0.0f,   0.0f, -1.0f, 0.0f,   0.0f, -1.0f, 0.0f,
        0.0f,  1.0f, 0.0f,   0.0f,  1.0f, 0.0f,   0.0f,  1.0f, 0.0f,   0.0f,  1.0f, 0.0f,
        1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,
        -1.0f, 0.0f, 0.0f,   -1.0f, 0.0f, 0.0f,   -1.0f, 0.0f, 0.0f,   -1.0f, 0.0f, 0.0f,
        0.0f, -sh, 1.0f,     0.0f, -sh, 1.0f,     0.0f, -sh, 1.0f,     0.0f, -sh, 1.0f,
        0.0f,  sh, -1.0f,    0.0f,  sh, -1.0f,    0.0f,  sh, -1.0f,    0.0f,  sh, -1.0f,
    };
    float texcoords[vertexCount * 2] = {
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
    };
    unsigned short indices[indexCount] = {
        0, 2, 1,  0, 3, 2,
        4, 6, 5,  4, 7, 6,
        8, 10, 9,  8, 11, 10,
        12, 14, 13,  12, 15, 14,
        16, 18, 17,  16, 19, 18,
        20, 22, 21,  20, 23, 22,
    };

    mesh.vertexCount = vertexCount;
    mesh.triangleCount = indexCount / 3;
    mesh.vertices = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    memcpy(mesh.vertices, vertices, vertexCount * 3 * sizeof(float));
    mesh.texcoords = (float*)RL_MALLOC(vertexCount * 2 * sizeof(float));
    memcpy(mesh.texcoords, texcoords, vertexCount * 2 * sizeof(float));
    mesh.normals = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    memcpy(mesh.normals, normals, vertexCount * 3 * sizeof(float));
    mesh.colors = (unsigned char*)RL_MALLOC(vertexCount * 4 * sizeof(unsigned char));
    for (int i = 0; i < vertexCount * 4; i++) mesh.colors[i] = 255;
    mesh.indices = (unsigned short*)RL_MALLOC(indexCount * sizeof(unsigned short));
    memcpy(mesh.indices, indices, indexCount * sizeof(unsigned short));

    UploadMesh(&mesh, false);

    return mesh;
}


// Flat-shaded unit mesh from a triangle soup (each triangle gets its own vertices and face normal;
// wound so the face normal points away from the shape's centre).
Mesh BuildFlatShapeMesh(const std::vector<Vector3>& tris, const std::vector<Color>* triColors = nullptr,
                        const std::vector<Vector3>* triInside = nullptr) {
    Mesh mesh = { 0 };
    const int vc = (int)tris.size();
    mesh.vertexCount = vc;
    mesh.triangleCount = vc / 3;
    mesh.vertices = (float*)RL_MALLOC(vc * 3 * sizeof(float));
    mesh.texcoords = (float*)RL_MALLOC(vc * 2 * sizeof(float));
    mesh.normals = (float*)RL_MALLOC(vc * 3 * sizeof(float));
    mesh.colors = (unsigned char*)RL_MALLOC(vc * 4);
    mesh.indices = (unsigned short*)RL_MALLOC(vc * sizeof(unsigned short));
    for (int t = 0; t + 2 < vc; t += 3) {
        Vector3 a = tris[(size_t)t], b = tris[(size_t)t + 1], c = tris[(size_t)t + 2];
        Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a));
        Vector3 cen = { (a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f, (a.z + b.z + c.z) / 3.0f };
        if (triInside && (size_t)(t / 3) < triInside->size()) cen = Vector3Subtract(cen, (*triInside)[(size_t)(t / 3)]);   // relative to the primitive's centre
        if (Vector3DotProduct(n, cen) < 0.0f) { std::swap(b, c); n = Vector3Negate(n); }
        n = Vector3Normalize(n);
        const Vector3 v[3] = { a, b, c };
        for (int k = 0; k < 3; k++) {
            const int i = t + k;
            mesh.vertices[i * 3] = v[k].x; mesh.vertices[i * 3 + 1] = v[k].y; mesh.vertices[i * 3 + 2] = v[k].z;
            mesh.normals[i * 3] = n.x; mesh.normals[i * 3 + 1] = n.y; mesh.normals[i * 3 + 2] = n.z;
            mesh.texcoords[i * 2] = 0.0f; mesh.texcoords[i * 2 + 1] = 0.0f;
            Color col = WHITE;
            if (triColors && (size_t)(t / 3) < triColors->size()) col = (*triColors)[(size_t)(t / 3)];
            mesh.colors[i * 4] = col.r; mesh.colors[i * 4 + 1] = col.g; mesh.colors[i * 4 + 2] = col.b; mesh.colors[i * 4 + 3] = col.a;
            mesh.indices[i] = (unsigned short)i;
        }
    }
    UploadMesh(&mesh, false);
    return mesh;
}

void AddBoxTris(std::vector<Vector3>& t, float x0, float y0, float z0, float x1, float y1, float z1, bool bottom) {
    const Vector3 p[8] = { {x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1},{x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1} };
    auto quad = [&](int a, int b, int c, int d) { t.push_back(p[a]); t.push_back(p[b]); t.push_back(p[c]); t.push_back(p[a]); t.push_back(p[c]); t.push_back(p[d]); };
    quad(4,5,6,7);                       // top
    quad(0,1,5,4); quad(1,2,6,5); quad(2,3,7,6); quad(3,0,4,7);   // sides
    if (bottom) quad(0,3,2,1);
}

// Box with a gabled roof: walls up to y=+0.5-ridge, ridge along local x at +0.5.
Mesh GenerateCityGableMesh() {
    std::vector<Vector3> t;
    const float eave = 0.12f;   // eave height as a fraction of the unit (the rest is roof)
    const float ey = 0.5f - eave;
    AddBoxTris(t, -0.5f, -0.5f, -0.5f, 0.5f, ey, 0.5f, true);
    // Roof prism: two slopes + two gable ends.
    const Vector3 a0 = { -0.5f, ey, -0.5f }, a1 = { 0.5f, ey, -0.5f }, b0 = { -0.5f, ey, 0.5f }, b1 = { 0.5f, ey, 0.5f };
    const Vector3 r0 = { -0.5f, 0.5f, 0.0f }, r1 = { 0.5f, 0.5f, 0.0f };
    t.insert(t.end(), { a0, a1, r1, a0, r1, r0 });   // -z slope
    t.insert(t.end(), { b1, b0, r0, b1, r0, r1 });   // +z slope
    t.insert(t.end(), { a0, r0, b0 });               // -x gable
    t.insert(t.end(), { a1, b1, r1 });               // +x gable
    return BuildFlatShapeMesh(t);
}

// Box with a single-slope roof: the roof drops from the full height at -z to 75% at +z.
Mesh GenerateCityShedMesh() {
    std::vector<Vector3> t;
    const float lo = 0.5f - 0.25f;
    const Vector3 p0 = { -0.5f, -0.5f, -0.5f }, p1 = { 0.5f, -0.5f, -0.5f }, p2 = { 0.5f, -0.5f, 0.5f }, p3 = { -0.5f, -0.5f, 0.5f };
    const Vector3 q0 = { -0.5f, 0.5f, -0.5f }, q1 = { 0.5f, 0.5f, -0.5f }, q2 = { 0.5f, lo, 0.5f }, q3 = { -0.5f, lo, 0.5f };
    t.insert(t.end(), { p0, p2, p1, p0, p3, p2 });          // bottom
    t.insert(t.end(), { q0, q3, q2, q0, q2, q1 });          // sloped roof
    t.insert(t.end(), { p0, p1, q1, p0, q1, q0 });          // -z wall
    t.insert(t.end(), { p3, q2, p2, p3, q3, q2 });          // +z wall
    t.insert(t.end(), { p0, q0, q3, p0, q3, p3 });          // -x wall
    t.insert(t.end(), { p1, p2, q2, p1, q2, q1 });          // +x wall
    return BuildFlatShapeMesh(t);
}

// Stepped tower: base (full footprint, lower 45%), shaft (80% wide, to 85%), crown (55% wide, to the top).
Mesh GenerateCityTowerMesh() {
    // Winding is decided per box against that box's own centre: against the whole shape's origin the
    // base's top face (y = -0.05, below the origin) came out inverted and the ledge had no roof.
    std::vector<Vector3> t, in;
    const auto box = [&](float x0, float y0, float z0, float x1, float y1, float z1, bool bottom) {
        const size_t before = t.size();
        AddBoxTris(t, x0, y0, z0, x1, y1, z1, bottom);
        in.insert(in.end(), (t.size() - before) / 3, Vector3{ (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f });
    };
    box(-0.5f, -0.5f, -0.5f, 0.5f, -0.05f, 0.5f, true);
    box(-0.4f, -0.05f, -0.4f, 0.4f, 0.35f, 0.4f, false);
    box(-0.27f, 0.35f, -0.27f, 0.27f, 0.5f, 0.27f, false);
    return BuildFlatShapeMesh(t, nullptr, &in);
}

Mesh BuildColoredShapeMesh(const std::vector<Vector3>& tris, const std::vector<Color>& triColors, const std::vector<Vector3>& inside) {
    return BuildFlatShapeMesh(tris, &triColors, &inside);
}

struct ShapeBuilder {
    std::vector<Vector3> t;
    std::vector<Color> c;
    std::vector<Vector3> in;   // per triangle: a point inside its primitive (for outward winding)
    void Box(float x0, float y0, float z0, float x1, float y1, float z1, Color col) {
        const size_t before = t.size();
        AddBoxTris(t, x0, y0, z0, x1, y1, z1, true);
        c.insert(c.end(), (t.size() - before) / 3, col);
        in.insert(in.end(), (t.size() - before) / 3, Vector3{ (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f });
    }
    // Pyramid-ish crown (octahedron-like) for trees: 4-sided bipyramid.
    void Crown(float cx, float cy, float cz, float r, float h, Color col) {
        const Vector3 top = { cx, cy + h, cz }, bot = { cx, cy, cz };
        const Vector3 q[4] = { { cx + r, cy + h * 0.45f, cz }, { cx, cy + h * 0.45f, cz + r }, { cx - r, cy + h * 0.45f, cz }, { cx, cy + h * 0.45f, cz - r } };
        for (int i = 0; i < 4; i++) {
            const Vector3 a = q[i], b = q[(i + 1) % 4];
            t.push_back(top); t.push_back(a); t.push_back(b); c.push_back(col); in.push_back({ cx, cy + h * 0.45f, cz });
            t.push_back(bot); t.push_back(b); t.push_back(a); c.push_back(col); in.push_back({ cx, cy + h * 0.45f, cz });
        }
    }
};

// Unit prop meshes. They are authored at real-world size (metres); the city instances them with a
// uniform scale of 1 so a streetlight is ~7 m tall. Origin: centre of the bottom face at y = -0.5
// would waste the unit cube convention, so props use y = 0 at the ground and the instance matrix
// translates them to the surface directly.
Mesh GenerateCityLampMesh() {
    ShapeBuilder b;
    b.Box(-0.07f, 0.0f, -0.07f, 0.07f, 5.6f, 0.07f, Color{ 70, 72, 78, 255 });       // pole
    b.Box(-0.07f, 5.5f, -0.07f, 0.07f, 5.64f, 1.1f, Color{ 70, 72, 78, 255 });       // arm toward +z
    b.Box(-0.22f, 5.38f, 0.8f, 0.22f, 5.52f, 1.4f, Color{ 250, 232, 170, 248 });      // lamp head (alpha 248 = a street lamp: switches on at dusk, see LampOn in lit.glsl)
    b.Box(-0.18f, 0.0f, -0.18f, 0.18f, 0.35f, 0.18f, Color{ 60, 62, 68, 255 });       // base
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

// Sidewalk furniture. Authored in metres, origin on the ground, +z toward the road.
// Bus: 11 m long along local +x (forward), 2.5 m wide. The body is white so the instance tint colours it (the
// line colour); windows and lights are a second, untinted mesh.
Mesh GenerateCityBusMesh() {
    ShapeBuilder b;
    b.Box(-5.5f, 0.4f, -1.25f, 5.5f, 3.0f, 1.25f, WHITE);                                // body
    b.Box(-5.5f, 3.0f, -1.2f, 5.5f, 3.08f, 1.2f, Color{ 215, 215, 220, 255 });          // roof cap (tinted a little)
    b.Box(-5.4f, 0.28f, -1.15f, 5.4f, 0.4f, 1.15f, Color{ 60, 62, 68, 255 });            // skirt
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCityBusGlassMesh() {
    ShapeBuilder b;
    const Color glass{ 34, 48, 62, 255 };
    for (float z : { -1.252f, 1.252f }) b.Box(-5.0f, 1.45f, z - 0.005f, 4.6f, 2.65f, z + 0.005f, glass);   // side windows
    b.Box(5.5f, 1.3f, -1.1f, 5.512f, 2.7f, 1.1f, glass);                                  // windscreen
    b.Box(-5.512f, 1.5f, -1.0f, -5.5f, 2.6f, 1.0f, glass);                                // rear window
    b.Box(5.49f, 1.0f, -1.1f, 5.516f, 1.15f, 1.1f, Color{ 255, 190, 80, 250 });          // destination sign (glows at night)
    for (float z : { -0.9f, 0.9f }) {
        b.Box(5.49f, 0.6f, z - 0.2f, 5.52f, 0.85f, z + 0.2f, Color{ 255, 244, 205, 250 });   // headlights
        b.Box(-5.52f, 0.6f, z - 0.2f, -5.49f, 0.85f, z + 0.2f, Color{ 230, 30, 25, 250 });   // tail lights
    }
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCityBenchMesh() {
    ShapeBuilder b;
    const Color wood{ 128, 92, 58, 255 }, iron{ 52, 54, 60, 255 };
    b.Box(-0.8f, 0.42f, -0.22f, 0.8f, 0.48f, 0.22f, wood);          // seat
    b.Box(-0.8f, 0.48f, -0.24f, 0.8f, 0.95f, -0.18f, wood);         // backrest (away from the road)
    for (float x : { -0.7f, 0.7f }) b.Box(x - 0.04f, 0.0f, -0.2f, x + 0.04f, 0.42f, 0.2f, iron);   // legs
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCityHydrantMesh() {
    ShapeBuilder b;
    const Color red{ 196, 38, 32, 255 }, cap{ 160, 28, 24, 255 };
    b.Box(-0.11f, 0.0f, -0.11f, 0.11f, 0.62f, 0.11f, red);          // body
    b.Box(-0.14f, 0.62f, -0.14f, 0.14f, 0.74f, 0.14f, cap);         // cap
    b.Box(-0.2f, 0.34f, -0.06f, 0.2f, 0.46f, 0.06f, red);           // side nozzles
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCityBollardMesh() {
    ShapeBuilder b;
    b.Box(-0.07f, 0.0f, -0.07f, 0.07f, 0.85f, 0.07f, Color{ 96, 98, 106, 255 });
    b.Box(-0.075f, 0.62f, -0.075f, 0.075f, 0.74f, 0.075f, Color{ 236, 200, 40, 255 });   // reflective band
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCityBusStopMesh() {
    ShapeBuilder b;
    const Color frame{ 58, 62, 72, 255 }, roof{ 40, 90, 150, 255 }, glass{ 120, 150, 170, 255 }, seat{ 120, 88, 56, 255 };
    for (float x : { -1.45f, 1.45f }) b.Box(x - 0.04f, 0.0f, -0.46f, x + 0.04f, 2.4f, -0.38f, frame);   // posts
    b.Box(-1.6f, 2.4f, -0.55f, 1.6f, 2.52f, 0.55f, roof);                                              // roof
    b.Box(-1.45f, 0.35f, -0.46f, 0.7f, 2.3f, -0.43f, glass);                                           // back glass
    b.Box(0.78f, 0.35f, -0.46f, 1.45f, 2.3f, -0.43f, Color{ 255, 238, 190, 250 });                     // advert panel (emissive at night)
    b.Box(-1.2f, 0.42f, -0.4f, 0.6f, 0.48f, -0.05f, seat);                                             // bench
    b.Box(1.9f, 0.0f, 0.2f, 1.96f, 2.5f, 0.26f, frame);                                                // stop sign pole
    b.Box(1.74f, 2.1f, 0.17f, 2.12f, 2.5f, 0.3f, Color{ 30, 110, 190, 255 });                          // stop sign
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCitySignMesh() {
    ShapeBuilder b;
    b.Box(-0.03f, 0.0f, -0.03f, 0.03f, 2.7f, 0.03f, Color{ 90, 92, 100, 255 });                        // pole
    b.Box(-0.5f, 2.4f, -0.05f, 0.5f, 2.66f, 0.05f, Color{ 24, 88, 52, 255 });                          // name plate
    b.Box(-0.46f, 2.43f, 0.05f, 0.46f, 2.63f, 0.052f, Color{ 238, 238, 232, 255 });                    // plate text strip
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCityTreeMesh() {
    ShapeBuilder b;
    b.Box(-0.14f, 0.0f, -0.14f, 0.14f, 2.0f, 0.14f, Color{ 96, 70, 46, 255 });
    b.Crown(0.0f, 1.6f, 0.0f, 1.5f, 3.4f, Color{ 62, 128, 66, 255 });
    b.Crown(0.0f, 2.8f, 0.0f, 1.1f, 2.6f, Color{ 78, 150, 78, 255 });
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

// Car: 4.2 m long along local +x, 1.8 m wide. Body and roof are white so the instance tint colours them;
// the windows and wheels are separate meshes (untinted) so black cars still have glass and tyres.
// Cabin rings (x range, half width, height) give a slanted, rounded windscreen and rear window.
struct CabinRing { float xr, xf, hw, y; };
constexpr CabinRing kCabin[3] = { { -1.30f, 1.20f, 0.80f, 0.95f }, { -1.04f, 0.74f, 0.75f, 1.21f }, { -0.60f, 0.22f, 0.64f, 1.44f } };

Mesh GenerateCityCarMesh() {
    ShapeBuilder b;
    b.Box(-2.1f, 0.35f, -0.9f, 2.1f, 0.95f, 0.9f, WHITE);                              // body
    // Roof cap (white, tinted).
    const CabinRing& r = kCabin[2];
    const Vector3 in{ -0.05f, 1.2f, 0.0f };
    const Vector3 q[4] = { { r.xr, r.y, -r.hw }, { r.xf, r.y, -r.hw }, { r.xf, r.y, r.hw }, { r.xr, r.y, r.hw } };
    for (int k : { 0, 1, 2, 0, 2, 3 }) { b.t.push_back(q[k]); b.c.push_back(WHITE); b.in.push_back(in); }
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCityCarGlassMesh() {
    ShapeBuilder b;
    const Color glass{ 38, 52, 66, 255 };
    const Vector3 in{ -0.05f, 1.2f, 0.0f };
    auto quad = [&](Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3) {
        for (const Vector3& v : { p0, p1, p2, p0, p2, p3 }) { b.t.push_back(v); b.c.push_back(glass); b.in.push_back(in); }
    };
    for (int i = 0; i < 2; i++) {
        const CabinRing &lo = kCabin[i], &hi = kCabin[i + 1];
        quad({ lo.xf, lo.y, -lo.hw }, { lo.xf, lo.y, lo.hw }, { hi.xf, hi.y, hi.hw }, { hi.xf, hi.y, -hi.hw });   // windscreen
        quad({ lo.xr, lo.y, -lo.hw }, { lo.xr, lo.y, lo.hw }, { hi.xr, hi.y, hi.hw }, { hi.xr, hi.y, -hi.hw });   // rear window
        quad({ lo.xr, lo.y, lo.hw }, { lo.xf, lo.y, lo.hw }, { hi.xf, hi.y, hi.hw }, { hi.xr, hi.y, hi.hw });     // +z side
        quad({ lo.xr, lo.y, -lo.hw }, { lo.xf, lo.y, -lo.hw }, { hi.xf, hi.y, -hi.hw }, { hi.xr, hi.y, -hi.hw }); // -z side
    }
    // Head and tail lights (alpha 250 = emissive at night). Untinted, so every car colour has them.
    const Color head{ 255, 244, 205, 250 }, tail{ 230, 30, 25, 250 };
    for (float z : { -0.62f, 0.62f }) {
        const size_t before = b.t.size();
        AddBoxTris(b.t, 2.08f, 0.55f, z - 0.14f, 2.14f, 0.70f, z + 0.14f, true);
        b.c.insert(b.c.end(), (b.t.size() - before) / 3, head);
        b.in.insert(b.in.end(), (b.t.size() - before) / 3, Vector3{ 2.11f, 0.62f, z });
        const size_t before2 = b.t.size();
        AddBoxTris(b.t, -2.14f, 0.55f, z - 0.14f, -2.08f, 0.70f, z + 0.14f, true);
        b.c.insert(b.c.end(), (b.t.size() - before2) / 3, tail);
        b.in.insert(b.in.end(), (b.t.size() - before2) / 3, Vector3{ -2.11f, 0.62f, z });
    }
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

// Wheel: round tyre (axis along z, radius 0.32 m) with a hub and a spoke so the roll is visible.
Mesh GenerateCityWheelMesh() {
    ShapeBuilder b;
    const int n = 20;
    const float R = 0.32f, W = 0.12f, H = 0.19f;
    const Color tyre{ 22, 22, 24, 255 }, hub{ 150, 154, 162, 255 };
    const Vector3 o{ 0, 0, 0 };
    auto tri = [&](Vector3 a, Vector3 bb, Vector3 c, Color col) { b.t.push_back(a); b.t.push_back(bb); b.t.push_back(c); b.c.push_back(col); b.in.push_back(o); };
    for (int i = 0; i < n; i++) {
        const float a0 = 2.0f * PI * (float)i / n, a1 = 2.0f * PI * (float)(i + 1) / n;
        const Vector3 p0{ cosf(a0) * R, sinf(a0) * R, 0 }, p1{ cosf(a1) * R, sinf(a1) * R, 0 };
        const Vector3 h0{ cosf(a0) * H, sinf(a0) * H, 0 }, h1{ cosf(a1) * H, sinf(a1) * H, 0 };
        for (float z : { -W, W }) {
            const Vector3 a{ p0.x, p0.y, z }, c{ p1.x, p1.y, z }, ha{ h0.x, h0.y, z }, hc{ h1.x, h1.y, z };
            tri(ha, hc, Vector3{ 0, 0, z }, hub);                         // hub disc
            tri(a, c, hc, tyre); tri(a, hc, ha, tyre);                      // tyre sidewall ring
        }
        const Vector3 t0{ p0.x, p0.y, -W }, t1{ p1.x, p1.y, -W }, t2{ p1.x, p1.y, W }, t3{ p0.x, p0.y, W };
        tri(t0, t1, t2, tyre); tri(t0, t2, t3, tyre);                      // tread
    }
    b.Box(-H, -0.035f, -W - 0.01f, H, 0.035f, W + 0.01f, Color{ 215, 218, 224, 255 });   // spoke across the hub
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

// Traffic-light pole with a dark three-lamp housing; the lit lamps are drawn separately.
// Local +z is the direction the lamps face (toward arriving traffic).
Mesh GenerateCitySignalMesh() {
    ShapeBuilder b;
    b.Box(-0.06f, 0.0f, -0.06f, 0.06f, 4.2f, 0.06f, Color{ 70, 72, 78, 255 });
    b.Box(-0.2f, 3.0f, -0.07f, 0.2f, 4.5f, 0.22f, Color{ 26, 26, 30, 255 });
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCityPersonMesh() {
    ShapeBuilder b;
    b.Box(-0.16f, 0.0f, -0.1f, 0.16f, 0.85f, 0.1f, Color{ 50, 60, 90, 255 });         // legs
    b.Box(-0.2f, 0.85f, -0.12f, 0.2f, 1.45f, 0.12f, WHITE);                            // torso (tinted)
    b.Box(-0.12f, 1.45f, -0.1f, 0.12f, 1.72f, 0.1f, Color{ 224, 188, 160, 255 });      // head
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}
}

namespace gfx {

Matrix ComposeTRS(Vector3 scale, Matrix rotation, Vector3 position) {
    const Matrix scaleMat = MatrixScale(scale.x, scale.y, scale.z);
    const Matrix transMat = MatrixTranslate(position.x, position.y, position.z);
    return MatrixMultiply(MatrixMultiply(scaleMat, rotation), transMat);
}

void Init() {
    if (initialized) return;

    // Shaders are compiled into the binary (shaders/lit.glsl via sokol-shdc).
    litShader = LoadShaderProgram("lit");
    lightDirLoc = GetShaderLocation(litShader, "lightDir");
    litSunLoc = GetShaderLocation(litShader, "sunColor");
    litFogLoc = GetShaderLocation(litShader, "fogParams");
    ambientLoc = GetShaderLocation(litShader, "ambient");
    lightVPLoc = GetShaderLocation(litShader, "lightVP");
    shadowMapLoc = GetShaderLocation(litShader, "shadowMap");
    shadowsEnabledLoc = GetShaderLocation(litShader, "shadowsEnabled");
    waterSurfaceYLoc = GetShaderLocation(litShader, "waterSurfaceY");
    waterAbsorptionLoc = GetShaderLocation(litShader, "waterAbsorption");
    waterFogDensityLoc = GetShaderLocation(litShader, "waterFogDensity");
    waterFogColorLoc = GetShaderLocation(litShader, "waterFogColor");

    int shadowSlot = SHADOW_TEXTURE_SLOT;
    SetShaderValue(litShader, shadowMapLoc, &shadowSlot, SHADER_UNIFORM_INT);

    // Road shader: same lighting as litShader but with a depth bias so the flat
    // road mesh always wins against the near-coplanar ground plane at altitude.
    roadShader = LoadShaderProgram("lit_road");
    // Loud diagnostic: this is the ONLY place a real-GPU shader failure can
    // silently hide (the load falls back to the default shader, losing the depth
    // bias -> coplanar roads re-fight the ground at altitude). The headless
    // harness stubs GetRoadShader() so it can never see this; the file + loud log
    // let the real application surface it.
    if (!IsShaderValid(roadShader)) {
        FILE* dzD = std::fopen("road_shader_status.txt", "w");
        if (dzD) {
            fputs("ROAD_SHADER_LINK_FAILED - road depth bias INACTIVE, expect z-fighting.\n", dzD);
            fclose(dzD);
        }
        TraceLog(LOG_ERROR, "ROAD SHADER LINK FAILED (id=0): depth bias inactive, "
                            "roads will z-fight the ground at altitude. Check road_shader_status.txt");
    } else {
        FILE* dzD = std::fopen("road_shader_status.txt", "w");
        if (dzD) {
            fputs("ROAD_SHADER_LINK_OK - depth bias active (constant world-space offset).\n", dzD);
            fclose(dzD);
        }
        TraceLog(LOG_INFO, "Road shader linked OK (id=%i): depth bias active.", roadShader.id);
    }
    // DrawMesh uploads matProjection to shaders whose projection loc is registered.
    if (roadShader.id != 0 && roadShader.locs)
        roadShader.locs[SHADER_LOC_MATRIX_PROJECTION] = GetShaderLocation(roadShader, "matProjection");
    roadLightDirLoc = GetShaderLocation(roadShader, "lightDir");
    roadSunLoc = GetShaderLocation(roadShader, "sunColor");
    roadFogLoc = GetShaderLocation(roadShader, "fogParams");
    roadAmbientLoc = GetShaderLocation(roadShader, "ambient");
    roadLightVPLoc = GetShaderLocation(roadShader, "lightVP");
    roadShadowMapLoc = GetShaderLocation(roadShader, "shadowMap");
    roadShadowsEnabledLoc = GetShaderLocation(roadShader, "shadowsEnabled");
    roadWaterSurfaceYLoc = GetShaderLocation(roadShader, "waterSurfaceY");
    roadNightLoc = GetShaderLocation(roadShader, "nightAmount");
    roadWeatherLoc = GetShaderLocation(roadShader, "weather");
    roadCamLoc = GetShaderLocation(roadShader, "camPos");
    roadLightCountLoc = GetShaderLocation(roadShader, "lightCount");
    roadLightsLoc = GetShaderLocation(roadShader, "nightLights");
    if (roadShadowMapLoc != -1) SetShaderValue(roadShader, roadShadowMapLoc, &shadowSlot, SHADER_UNIFORM_INT);

    // Instanced lit shader (city buildings). Own uniforms mirror litShader's.
    cityInstancedShader = LoadShaderProgram("lit_instanced");
    if (cityInstancedShader.id != 0) {
        cityLightDirLoc = GetShaderLocation(cityInstancedShader, "lightDir");
        citySunLoc = GetShaderLocation(cityInstancedShader, "sunColor");
        cityFogLoc = GetShaderLocation(cityInstancedShader, "fogParams");
        cityAmbientLoc = GetShaderLocation(cityInstancedShader, "ambient");
        cityLightVPLoc = GetShaderLocation(cityInstancedShader, "lightVP");
        cityShadowMapLoc = GetShaderLocation(cityInstancedShader, "shadowMap");
        cityShadowsEnabledLoc = GetShaderLocation(cityInstancedShader, "shadowsEnabled");
        cityWaterSurfaceYLoc = GetShaderLocation(cityInstancedShader, "waterSurfaceY");
        cityNightLoc = GetShaderLocation(cityInstancedShader, "nightAmount");
        cityLightCountLoc = GetShaderLocation(cityInstancedShader, "lightCount");
        cityLightsLoc = GetShaderLocation(cityInstancedShader, "nightLights");
        cityWeatherLoc = GetShaderLocation(cityInstancedShader, "weather");
        if (cityShadowMapLoc != -1) SetShaderValue(cityInstancedShader, cityShadowMapLoc, &shadowSlot, SHADER_UNIFORM_INT);
    }

    // Load the shared default texture BEFORE any material grabs it, otherwise
    // materials keep texture id 0 and the shaders sample an unbound unit (black).
    Image checker = GenImageChecked(64, 64, 8, 8, LIGHTGRAY, GRAY);
    defaultTexture = LoadTextureFromImage(checker);
    UnloadImage(checker);

    cityInstancedMaterial = LoadMaterialDefault();
    cityInstancedMaterial.shader = cityInstancedShader;
    cityInstancedMaterial.maps[MATERIAL_MAP_DIFFUSE].texture = defaultTexture;
    cityInstancedReady = cityInstancedShader.id != 0;

    cubeModel = LoadModelFromMesh(GenMeshCube(1.0f, 1.0f, 1.0f));
    sphereModel = LoadModelFromMesh(GenMeshSphere(0.5f, 16, 16));
    cylinderModel = LoadModelFromMesh(GenMeshCylinder(0.5f, 1.0f, 16));
    wedgeModel = LoadModelFromMesh(GenerateWedgeMesh());
    cityWedgeMesh = GenerateCityWedgeMesh();
    citySlantMesh = GenerateCitySlantMesh();
    cityGableMesh = GenerateCityGableMesh();
    cityTowerMesh = GenerateCityTowerMesh();
    cityShedMesh = GenerateCityShedMesh();
    cityLampMesh = GenerateCityLampMesh();
    cityTreeMesh = GenerateCityTreeMesh();
    cityCarMesh = GenerateCityCarMesh();
    cityCarGlassMesh = GenerateCityCarGlassMesh();
    cityWheelMesh = GenerateCityWheelMesh();
    cityPersonMesh = GenerateCityPersonMesh();
    citySignalMesh = GenerateCitySignalMesh();
    cityBenchMesh = GenerateCityBenchMesh();
    cityBusMesh = GenerateCityBusMesh();
    cityBusGlassMesh = GenerateCityBusGlassMesh();
    cityHydrantMesh = GenerateCityHydrantMesh();
    cityBollardMesh = GenerateCityBollardMesh();
    cityBusStopMesh = GenerateCityBusStopMesh();
    citySignMesh = GenerateCitySignMesh();

    Model* models[] = { &cubeModel, &sphereModel, &cylinderModel, &wedgeModel };
    for (Model* model : models) {
        model->materials[0].shader = litShader;
        model->materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = defaultTexture;
    }

    groundModel = LoadModelFromMesh(GenMeshPlane(40.0f, 40.0f, 1, 1));
    groundTexture = GenerateGridTexture();
    groundModel.materials[0].shader = litShader;
    groundModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = groundTexture;

    // Depth-only target: the lit shaders sample it through a comparison
    // sampler (hardware PCF), so no color attachment is needed.
    shadowMap = LoadRenderTextureDepth(shadowMapResolution, shadowMapResolution);

    reflectionTarget = LoadRenderTexture(reflectionResolution, reflectionResolution);
    SetTextureFilter(reflectionTarget.texture, TEXTURE_FILTER_BILINEAR);

    lightCamera.position = Vector3Scale(SunDir(), -40.0f);
    lightCamera.target = { 0.0f, 0.0f, 0.0f };
    lightCamera.up = { 0.0f, 1.0f, 0.0f };
    lightCamera.fovy = 45.0f;
    lightCamera.projection = CAMERA_ORTHOGRAPHIC;

    initialized = true;
}

void Shutdown() {
    if (!initialized) return;

    UnloadModel(cubeModel);
    UnloadModel(sphereModel);
    UnloadModel(cylinderModel);
    UnloadModel(wedgeModel);
    UnloadMesh(cityWedgeMesh);
    UnloadMesh(citySlantMesh);
    UnloadMesh(cityGableMesh);
    UnloadMesh(cityTowerMesh);
    UnloadMesh(cityShedMesh);
    UnloadMesh(cityLampMesh);
    UnloadMesh(cityTreeMesh);
    UnloadMesh(cityCarMesh);
    UnloadMesh(cityCarGlassMesh);
    UnloadMesh(cityWheelMesh);
    UnloadMesh(cityPersonMesh);
    UnloadMesh(citySignalMesh);
    UnloadMesh(cityBenchMesh);
    UnloadMesh(cityBusMesh);
    UnloadMesh(cityBusGlassMesh);
    UnloadMesh(cityHydrantMesh);
    UnloadMesh(cityBollardMesh);
    UnloadMesh(cityBusStopMesh);
    UnloadMesh(citySignMesh);
    UnloadModel(groundModel);
    UnloadTexture(defaultTexture);
    UnloadTexture(groundTexture);
    if (shadowMap.id > 0) UnloadRenderTexture(shadowMap);
    if (reflectionTarget.id > 0) UnloadRenderTexture(reflectionTarget);
    if (cityInstancedShader.id != 0) UnloadShader(cityInstancedShader);
    if (roadShader.id != 0) UnloadShader(roadShader);
    cityInstancedReady = false;
    cityInstancedMaterial = {};
    UnloadShader(litShader);

    initialized = false;
}

Shader& GetLitShader() { return litShader; }

Shader& GetRoadShader() { return roadShader; }

Model& GetShapeModel(ShapeType type) {
    switch (type) {
        case ShapeType::Sphere:   return sphereModel;
        case ShapeType::Cylinder: return cylinderModel;
        case ShapeType::Wedge:    return wedgeModel;
        default:                  return cubeModel;
    }
}

Texture2D GetDefaultTexture() { return defaultTexture; }

// Unit building meshes used by the city instancer. Shape 0 is a plain box.
Mesh GetCityShapeMesh(int shape) {
    switch (shape) {
        case 1: return cityWedgeMesh;
        case 2: return citySlantMesh;
        case 3: return cityGableMesh;
        case 4: return cityTowerMesh;
        case 5: return cityLampMesh;
        case 6: return cityTreeMesh;
        case 7: return cityCarMesh;
        case 8: return cityPersonMesh;
        case 9: return citySignalMesh;
        case 10: return cityShedMesh;
        case 11: return cityCarGlassMesh;
        case 12: return cityWheelMesh;
        case 13: return cityBenchMesh;
        case 14: return cityHydrantMesh;
        case 15: return cityBollardMesh;
        case 16: return cityBusStopMesh;
        case 17: return citySignMesh;
        case 18: return cityBusMesh;
        case 19: return cityBusGlassMesh;
        default: return cubeModel.meshes[0];
    }
}

void SetShadowsEnabled(bool enabled) { shadowsEnabled = enabled; shadowsDirty = true; }

bool IsShadowsEnabled() { return shadowsEnabled; }

void SetShadowQuality(int quality) {
    quality = Clamp(quality, 5, 100);
    if (quality == shadowQuality && shadowMap.id > 0) return;
    shadowQuality = quality;

    int resolution = QualityToResolution(quality);
    if (resolution == shadowMapResolution && shadowMap.id > 0) return;
    shadowMapResolution = resolution;
    shadowsDirty = true;

    if (shadowMap.id > 0) {
        UnloadRenderTexture(shadowMap);
        shadowMap = { 0 };
    }
    shadowMap = LoadRenderTextureDepth(shadowMapResolution, shadowMapResolution);
}

int GetShadowQuality() { return shadowQuality; }

void SetAmbientIntensity(float intensity) {
    lightingSettings.ambient = fmaxf(0.0f, fminf(intensity, 2.0f));
}

float GetAmbientIntensity() {
    return lightingSettings.ambient;
}

void SetGridVisible(bool visible) { gridVisible = visible; }

bool IsGridVisible() { return gridVisible; }

void SetWireframe(bool enabled) {
    wireframe = enabled;
    if (wireframe) rlEnableWireMode();
    else rlDisableWireMode();
}

bool IsWireframe() { return wireframe; }

void BeginShadowPass() {
    if (!shadowsEnabled || shadowMap.id == 0) return;

    inShadowPass = true;

    // The shadow frustum follows the viewer, but it must be STABLE while the
    // viewer only rotates: centring it on where the view ray hits the ground made
    // the box jump by hundreds of metres (and rescale) with tiny pitch changes at
    // street level, which showed up as shadows popping and glitching inside the
    // city. So:
    //   * size depends on camera HEIGHT only, with hysteresis (no per-frame rescale);
    //   * the centre sits ahead of the camera along its HORIZONTAL look direction
    //     (pitch-independent), blending to the ground-hit point only when the
    //     camera looks steeply down (top-down editing).
    Vector3 focus = { 0.0f, 0.0f, 0.0f };
    float half = shadowStableHalf > 0.0f ? shadowStableHalf : kShadowMinHalf;
    if (haveShadowViewCamera) {
        const Camera3D& cam = shadowViewCamera;
        const Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
        const float camH = fmaxf(cam.position.y, 0.0f);

        const float targetHalf = fminf(fmaxf(64.0f + 0.6f * camH, kShadowMinHalf), kShadowMaxHalf);
        if (shadowStableHalf <= 0.0f || fabsf(targetHalf - shadowStableHalf) > 12.0f)
            shadowStableHalf = ceilf(targetHalf / 8.0f) * 8.0f;
        half = shadowStableHalf;

        Vector3 fh = { fwd.x, 0.0f, fwd.z };
        const float fl = Vector3Length(fh);
        fh = fl > 1e-3f ? Vector3Scale(fh, 1.0f / fl) : Vector3{ 0.0f, 0.0f, -1.0f };
        const Vector3 ahead = { cam.position.x + fh.x * half * 0.35f, 0.0f,
                                cam.position.z + fh.z * half * 0.35f };
        Vector3 hit = ahead;
        if (cam.position.y > 0.0f && fwd.y < -0.05f) {
            const float tHit = fminf(-cam.position.y / fwd.y, 800.0f);
            hit = Vector3Add(cam.position, Vector3Scale(fwd, tHit));
            hit.y = 0.0f;
        }
        const float w = fminf(fmaxf((-fwd.y - 0.5f) / 0.3f, 0.0f), 1.0f);
        focus = Vector3Lerp(ahead, hit, w);
    }

    // Snap the frustum centre to whole shadow texels in light space (no shimmering
    // while the camera moves). Basis matches raylib's lookAt for up = (0,1,0).
    const Vector3 sunDir = SunDir();
    {   // a moved sun changes every shadow
        static Vector3 lastSun{};
        if (!Vector3Equals(lastSun, sunDir)) { lastSun = sunDir; shadowsDirty = true; }
    }
    const Vector3 lightRight = Vector3Normalize(Vector3CrossProduct(sunDir, { 0.0f, 1.0f, 0.0f }));
    const Vector3 lightUp = Vector3CrossProduct(lightRight, sunDir);
    const float texel = (2.0f * half) / (float)shadowMapResolution;
    const float cx = Vector3DotProduct(focus, lightRight);
    const float cy = Vector3DotProduct(focus, lightUp);
    const float sx = floorf(cx / texel) * texel;
    const float sy = floorf(cy / texel) * texel;
    focus = Vector3Add(focus, Vector3Add(Vector3Scale(lightRight, sx - cx), Vector3Scale(lightUp, sy - cy)));

    lightCamera.target = focus;
    lightCamera.position = Vector3Subtract(focus, Vector3Scale(sunDir, kShadowLightDist));
    lightCamera.up = { 0.0f, 1.0f, 0.0f };
    lightCamera.projection = CAMERA_ORTHOGRAPHIC;

    // Reuse the previous depth map when nothing that could change it happened.
    if (SceneInputActivity()) shadowInputHold = kShadowInputHold;
    const bool frustumSame = shadowHaveRendered && shadowLastRes == shadowMapResolution &&
                             shadowLastHalf == half && Vector3Equals(shadowLastCenter, focus);
    const double now = GetTime();
    if (shadowInputHold > 0 || !frustumSame || shadowsDirty.load()) shadowStillSince = now;
    shadowPassReused = shadowReuseEnabled && frustumSame && !shadowsDirty.load() &&
                       shadowInputHold == 0 && shadowFramesSinceRender < kShadowMaxAge &&
                       now - shadowStillSince >= kShadowStillSeconds &&
                       !ui::IsPlayActive();
    if (shadowInputHold > 0) shadowInputHold--;

    BeginTextureMode(shadowMap);
    if (shadowPassReused) {
        // Keep the depth map as is: entities still "draw" between Begin/EndShadowPass,
        // but an empty scissor rect discards every fragment.
        rlEnableScissorTest();
        rlScissor(0, 0, 0, 0);
        shadowFramesSinceRender++;
    } else {
        ClearBackground(WHITE);
        shadowsDirty = false;
        shadowHaveRendered = true;
        shadowLastCenter = focus;
        shadowLastHalf = half;
        shadowLastRes = shadowMapResolution;
        shadowFramesSinceRender = 0;
    }

    BeginMode3D(lightCamera);
    rlSetMatrixProjection(MatrixOrtho(-half, half, -half, half, kShadowNear, kShadowFar));

    lightView = rlGetMatrixModelview();
    lightProj = rlGetMatrixProjection();
}

void EndShadowPass() {
    if (!shadowsEnabled || shadowMap.id == 0) return;

    EndMode3D();
    EndTextureMode();
    if (shadowPassReused) rlDisableScissorTest();
    shadowPassReused = false;

    inShadowPass = false;
    lightViewProj = MatrixMultiply(lightView, lightProj);

    // The frustum moves every frame, so hand the fresh matrix to the receiving
    // shaders now instead of relying on UpdateLighting() running after this pass.
    if (litShader.id != 0 && lightVPLoc != -1) SetShaderValueMatrix(litShader, lightVPLoc, lightViewProj);
    if (roadShader.id != 0 && roadLightVPLoc != -1) SetShaderValueMatrix(roadShader, roadLightVPLoc, lightViewProj);
    if (cityInstancedReady && cityLightVPLoc != -1) SetShaderValueMatrix(cityInstancedShader, cityLightVPLoc, lightViewProj);
}

bool IsInShadowPass() {
    return inShadowPass;
}

void SetReflectionsEnabled(bool enabled) { reflectionEnabled = enabled; }

bool IsReflectionsEnabled() { return reflectionEnabled; }

void SetReflectionQuality(int quality) {
    quality = Clamp(quality, 5, 100);
    if (quality == reflectionQuality && reflectionTarget.id > 0) return;
    reflectionQuality = quality;

    int resolution = ReflectionQualityToResolution(quality);
    if (resolution == reflectionResolution && reflectionTarget.id > 0) return;
    reflectionResolution = resolution;

    if (reflectionTarget.id > 0) UnloadRenderTexture(reflectionTarget);
    reflectionTarget = LoadRenderTexture(reflectionResolution, reflectionResolution);
    SetTextureFilter(reflectionTarget.texture, TEXTURE_FILTER_BILINEAR);
}

int GetReflectionQuality() { return reflectionQuality; }

Camera3D BeginReflectionPass(float planeHeight, const Camera3D& worldCamera, Color clearColor) {
    if (!reflectionEnabled || reflectionTarget.id == 0) {
        inReflectionPass = false;
        return worldCamera;
    }

    inReflectionPass = true;

    // Mirror the camera about the horizontal plane y = planeHeight. Objects above
    // the water appear below the mirrored camera, producing a true planar mirror.
    Camera3D mirrored = worldCamera;
    float planeY = planeHeight;
    mirrored.position.y = 2.0f * planeY - mirrored.position.y;
    mirrored.target.y = 2.0f * planeY - mirrored.target.y;
    mirrored.up.y = -mirrored.up.y;

    // Guard against a degenerate basis when looking straight up/down (the mirrored
    // up vector becomes parallel to the view direction).
    Vector3 fwd = Vector3Normalize(Vector3Subtract(mirrored.target, mirrored.position));
    if (fabsf(Vector3DotProduct(fwd, mirrored.up)) > 0.999f) {
        mirrored.up = { 0.0f, 0.0f, 1.0f };
    }

    BeginTextureMode(reflectionTarget);
    ClearBackground(clearColor);

    return mirrored;
}

void EndReflectionPass() {
    if (!inReflectionPass || reflectionTarget.id == 0) return;

    reflView = rlGetMatrixModelview();
    reflProj = rlGetMatrixProjection();

    rlDrawRenderBatchActive();
    EndMode3D();
    EndTextureMode();

    inReflectionPass = false;
    reflViewProj = MatrixMultiply(reflView, reflProj);
}

bool IsInReflectionPass() {
    return inReflectionPass;
}

Matrix GetReflectionViewProj() { return reflViewProj; }

RenderTexture2D GetReflectionTarget() { return reflectionTarget; }

int GetReflectionTextureSlot() { return REFLECTION_TEXTURE_SLOT; }

namespace {
// Sun direction scaled by its intensity (the shaders use its length as the diffuse strength) and the
// ambient colour, both blended towards moonlight at night.
// Clouds dim and soften the sun (the shadows fade with it) and flatten the light.
float SunPower() { return lightingSettings.sunIntensity * (0.10f + 0.90f * DayAmount()) * (1.0f - 0.78f * OvercastAmount()); }
Vector3 SunVector() { return Vector3Scale(SunDir(), SunPower()); }
Vector3 EffectiveAmbient() {
    const Vector3 nightAmbient = { 0.16f, 0.19f, 0.31f };
    const Vector3 day = Vector3Scale(kAmbient, lightingSettings.ambient * (1.0f + 0.30f * OvercastAmount()));
    return Vector3Lerp(nightAmbient, day, DayAmount());
}
// The sun is white-yellow high up, orange near the horizon (sunrise and sunset), and whiter under cloud.
Vector3 SunTint() {
    Vector3 c = { lightingSettings.sunColor[0], lightingSettings.sunColor[1], lightingSettings.sunColor[2] };
    if (lightingSettings.sunFollowsTime) {
        const float warm = (1.0f - Smooth01(4.0f, 36.0f, RawSunElevation())) * (1.0f - OvercastAmount());
        c = Vector3Lerp(c, Vector3{ c.x * 1.0f, c.y * 0.56f, c.z * 0.26f }, warm);
    }
    return c;
}
}

float SunElevationNow() { return RawSunElevation(); }
float WetnessNow() { return WetAmount(); }
float RainNow() { return RainAmount(); }
float OvercastNow() { return OvercastAmount(); }
Vector3 SunDirection() { return SunDir(); }
Vector3 SunLightScale() { return Vector3Scale(SunTint(), SunPower()); }
Vector3 AmbientScale() {
    const Vector3 e = EffectiveAmbient();
    return { e.x / kAmbient.x, e.y / kAmbient.y, e.z / kAmbient.z };
}
float FogDensity() {
    // Cloud and rain add a little haze to whatever fog the Fog item sets.
    return (lightingSettings.hasFog ? lightingSettings.fogDensity : 0.0f) + 0.0014f * OvercastAmount() + 0.0045f * RainAmount();
}
void SetEngineClearColor(Color c) { engineClear = c; }
Color CurrentSky() {
    const Color day = lightingSettings.hasSky
        ? Color{ (unsigned char)lroundf(lightingSettings.skyColor[0] * 255.0f), (unsigned char)lroundf(lightingSettings.skyColor[1] * 255.0f),
                 (unsigned char)lroundf(lightingSettings.skyColor[2] * 255.0f), 255 }
        : engineClear;
    return SkyColor(day);
}
Color FogColorNow() { return CurrentSky(); }

void SetTimeOfDay(float hours) {
    const float h = fmodf(fmodf(hours, 24.0f) + 24.0f, 24.0f);
    lightingSettings.timeOfDay = h;
}
float GetTimeOfDay() { return lightingSettings.timeOfDay; }
LightingSettings& Lighting() { return lightingSettings; }
void ResetLighting() { lightingSettings = LightingSettings{}; }
void TickLighting(float dt) {
    if (lightingSettings.dayLengthMinutes > 0.0f)
        lightingSettings.timeOfDay = fmodf(lightingSettings.timeOfDay + dt * 24.0f / (lightingSettings.dayLengthMinutes * 60.0f), 24.0f);
}
// 0 = day, 1 = night. A dark storm in the daytime already switches some lights on.
float GetNightAmount() { return fmaxf(1.0f - DayAmount(), 0.40f * OvercastAmount()); }
float LampSwitchOn(float x, float z, float night) {
    auto fract1 = [](float v) { return v - floorf(v); };
    const float qx = floorf(x * 2.0f + 0.5f), qz = floorf(z * 2.0f + 0.5f);
    float p0 = fract1(qx * 0.1031f), p1 = fract1(qz * 0.1031f), p2 = p0;
    const float d = p0 * (p1 + 33.33f) + p1 * (p2 + 33.33f) + p2 * (p0 + 33.33f);
    p0 += d; p1 += d; p2 += d;
    const float h = fract1((p0 + p1) * p2);
    const float thr = 0.20f + (0.62f - 0.20f) * h;
    float on = Smooth01(thr, thr + 0.12f, night);
    if (fract1(h * 17.31f + 0.37f) < 0.10f) {
        const float k = fract1(floorf((float)GetTime() * 8.0f + h * 50.0f) * 0.61803f + h * 3.7f);
        if (k < 0.3f) on *= 0.2f;
    }
    return on;
}

namespace {
uint32_t RainHash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
}

// Rain: streaks that fall through a box around the camera. Each drop's place is a function of its index and the
// time only, wrapped around the camera, so the rain stays put in the world while you move through it.
void DrawWeather(const Camera3D& camera) {
    const float rain = RainAmount();
    if (rain < 0.02f || inShadowPass || inReflectionPass) return;
    const int count = (int)(300.0f + 2300.0f * rain);
    const float box = 44.0f, top = 14.0f, depth = 30.0f;
    const float t = (float)GetTime();
    const float speed = 15.0f + 6.0f * rain;
    const Color sky = CurrentSky();
    const Color col = { (unsigned char)(sky.r + (255 - sky.r) * 0.55f), (unsigned char)(sky.g + (255 - sky.g) * 0.55f),
                        (unsigned char)(sky.b + (255 - sky.b) * 0.55f), (unsigned char)(70.0f + 60.0f * rain) };
    const Vector3 slant = { 0.10f, -0.9f, 0.04f };
    for (int i = 0; i < count; i++) {
        const uint32_t h = RainHash((uint32_t)i * 2654435761U + 17U);
        const float hx = (float)(h & 0xffff) / 65535.0f * box;
        const float hz = (float)((h >> 16) & 0xffff) / 65535.0f * box;
        const float phase = (float)(RainHash(h) & 0xffff) / 65535.0f * depth;
        float dx = fmodf(hx - camera.position.x, box); if (dx < 0.0f) dx += box;
        float dz = fmodf(hz - camera.position.z, box); if (dz < 0.0f) dz += box;
        const float fall = fmodf(t * speed + phase, depth);
        const Vector3 a = { camera.position.x + dx - box * 0.5f, camera.position.y + top - fall, camera.position.z + dz - box * 0.5f };
        const Vector3 b = Vector3Add(a, Vector3Scale(slant, 0.9f + 0.5f * rain));
        DrawLine3D(a, b, col);
    }
}
void SetNightLights(const Vector4* lights, int count) {
    count = Clamp(count, 0, 32);
    nightLightCount = count;
    for (int i = 0; i < count; i++) {
        nightLightData[i * 4] = lights[i].x; nightLightData[i * 4 + 1] = lights[i].y;
        nightLightData[i * 4 + 2] = lights[i].z; nightLightData[i * 4 + 3] = lights[i].w;
    }
    const float night = GetNightAmount();
    const float c = (float)count;
    if (roadShader.id != 0 && roadLightsLoc != -1) {
        SetShaderValue(roadShader, roadNightLoc, &night, SHADER_UNIFORM_FLOAT);
        SetShaderValue(roadShader, roadLightCountLoc, &c, SHADER_UNIFORM_FLOAT);
        if (count > 0) SetShaderValueV(roadShader, roadLightsLoc, nightLightData, SHADER_UNIFORM_VEC4, count);
    }
    if (cityInstancedReady && cityLightsLoc != -1) {
        SetShaderValue(cityInstancedShader, cityLightCountLoc, &c, SHADER_UNIFORM_FLOAT);
        if (count > 0) SetShaderValueV(cityInstancedShader, cityLightsLoc, nightLightData, SHADER_UNIFORM_VEC4, count);
    }
}

namespace {
void UploadWeather(Shader& sh, int weatherLoc) {
    if (weatherLoc == -1) return;
    const Vector4 w = { WetAmount(), (float)GetTime(), 0.0f, 0.0f };
    SetShaderValue(sh, weatherLoc, &w, SHADER_UNIFORM_VEC4);
}
void UploadSunAndFog(Shader& sh, int sunLoc, int fogLoc) {
    if (sunLoc != -1) {
        const Vector4 sc = { lightingSettings.sunColor[0], lightingSettings.sunColor[1], lightingSettings.sunColor[2], 1.0f };
        SetShaderValue(sh, sunLoc, &sc, SHADER_UNIFORM_VEC4);
    }
    if (fogLoc != -1) {
        const Color f = FogColorNow();
        const Vector4 fp = { f.r / 255.0f, f.g / 255.0f, f.b / 255.0f, FogDensity() };
        SetShaderValue(sh, fogLoc, &fp, SHADER_UNIFORM_VEC4);
    }
}
}

Color SkyColor(Color dayColor) {
    const float d = DayAmount();
    const float over = OvercastAmount();
    // Cloud: the daytime colour goes grey and a little darker.
    const float grey = 0.58f * over;
    const float lum = (dayColor.r * 0.3f + dayColor.g * 0.59f + dayColor.b * 0.11f) * 0.86f;
    float r = dayColor.r + (lum - dayColor.r) * grey, g = dayColor.g + (lum - dayColor.g) * grey, b = dayColor.b + (lum - dayColor.b) * grey;
    const float dim = 1.0f - 0.28f * over;
    r *= dim; g *= dim; b *= dim;
    // Sunrise and sunset: the sky glows orange while the sun is near the horizon.
    if (lightingSettings.sunFollowsTime) {
        const float e = RawSunElevation();
        const float tw = Smooth01(-10.0f, 0.0f, e) * (1.0f - Smooth01(2.0f, 34.0f, e)) * (1.0f - 0.7f * over);
        r += (238.0f - r) * 0.6f * tw; g += (132.0f - g) * 0.6f * tw; b += (86.0f - b) * 0.6f * tw;
    }
    const Color night = { 10, 14, 28, 255 };
    auto mix = [&](unsigned char n, float dd) { return (unsigned char)lroundf(fminf(fmaxf((float)n + (dd - (float)n) * d, 0.0f), 255.0f)); };
    return Color{ mix(night.r, r), mix(night.g, g), mix(night.b, b), dayColor.a };
}
Vector3 WaterSky() {
    Vector3 base = { 0.65f, 0.78f, 0.88f };
    if (lightingSettings.hasSky) base = Vector3Lerp(base, Vector3{ lightingSettings.skyColor[0], lightingSettings.skyColor[1], lightingSettings.skyColor[2] }, 0.6f);
    const Color c = SkyColor(Color{ (unsigned char)lroundf(base.x * 255.0f), (unsigned char)lroundf(base.y * 255.0f), (unsigned char)lroundf(base.z * 255.0f), 255 });
    return { c.r / 255.0f, c.g / 255.0f, c.b / 255.0f };
}

void UpdateLighting(const Camera3D& camera) {
    shadowViewCamera = camera;
    haveShadowViewCamera = true;

    if (camera.projection == CAMERA_PERSPECTIVE) {
        const double h = fabs((double)camera.position.y);
        ApplyNearPlane(fmin(fmax(h * 0.03, 0.05), 2.0));
    }

    const Vector3 sun = SunVector();
    const Vector3 amb = EffectiveAmbient();
    SetShaderValue(litShader, lightDirLoc, &sun, SHADER_UNIFORM_VEC3);
    SetShaderValue(litShader, ambientLoc, &amb, SHADER_UNIFORM_VEC3);
    UploadSunAndFog(litShader, litSunLoc, litFogLoc);
    SetShaderValueMatrix(litShader, lightVPLoc, lightViewProj);

    float enabled = shadowsEnabled ? 1.0f : 0.0f;
    SetShaderValue(litShader, shadowsEnabledLoc, &enabled, SHADER_UNIFORM_FLOAT);

    if (roadShader.id != 0) {
        SetShaderValue(roadShader, roadLightDirLoc, &sun, SHADER_UNIFORM_VEC3);
        SetShaderValue(roadShader, roadAmbientLoc, &amb, SHADER_UNIFORM_VEC3);
        UploadSunAndFog(roadShader, roadSunLoc, roadFogLoc);
        UploadWeather(roadShader, roadWeatherLoc);
        if (roadCamLoc != -1) { const Vector4 cp = { camera.position.x, camera.position.y, camera.position.z, 1.0f }; SetShaderValue(roadShader, roadCamLoc, &cp, SHADER_UNIFORM_VEC4); }
        SetShaderValueMatrix(roadShader, roadLightVPLoc, lightViewProj);
        SetShaderValue(roadShader, roadShadowsEnabledLoc, &enabled, SHADER_UNIFORM_FLOAT);
    }

    if (shadowsEnabled && shadowMap.depth.id > 0) {
        rlActiveTextureSlot(SHADOW_TEXTURE_SLOT);
        rlEnableTexture(shadowMap.depth.id);
    }
}

void SetupInstancedLighting() {
    if (!cityInstancedReady) return;
    const Vector3 sun = SunVector();
    const Vector3 amb = EffectiveAmbient();
    SetShaderValue(cityInstancedShader, cityLightDirLoc, &sun, SHADER_UNIFORM_VEC3);
    SetShaderValue(cityInstancedShader, cityAmbientLoc, &amb, SHADER_UNIFORM_VEC3);
    UploadSunAndFog(cityInstancedShader, citySunLoc, cityFogLoc);
    UploadWeather(cityInstancedShader, cityWeatherLoc);
    if (cityNightLoc != -1) {
        const float night = GetNightAmount();
        SetShaderValue(cityInstancedShader, cityNightLoc, &night, SHADER_UNIFORM_FLOAT);
    }
    SetShaderValueMatrix(cityInstancedShader, cityLightVPLoc, lightViewProj);

    float enabled = shadowsEnabled ? 1.0f : 0.0f;
    SetShaderValue(cityInstancedShader, cityShadowsEnabledLoc, &enabled, SHADER_UNIFORM_FLOAT);

    if (shadowsEnabled && shadowMap.depth.id > 0) {
        rlActiveTextureSlot(SHADOW_TEXTURE_SLOT);
        rlEnableTexture(shadowMap.depth.id);
    }
}

void DrawCityInstances(Mesh mesh, const std::vector<Matrix>& transforms, int start, int count, Color tint) {
    if (!cityInstancedReady || count <= 0) return;
    if (start < 0 || (size_t)(start + count) > transforms.size()) return;
    if (!inShadowPass) SetupInstancedLighting();
    cityInstancedMaterial.maps[MATERIAL_MAP_DIFFUSE].color = tint;
    DrawMeshInstanced(mesh, cityInstancedMaterial, transforms.data() + start, count);
    IncrementDrawCallCount(1);
    AddMeshCount(1);
    AddTriangleCount(mesh.triangleCount * count);
    NoteMaterialUsed(cityInstancedMaterial.shader.id);
    if (cityInstancedMaterial.maps) {
        for (int m = 0; m < kMaterialMapCount; ++m) {
            NoteTextureBound(cityInstancedMaterial.maps[m].texture.id);
        }
    }
}

unsigned int CreateInstanceBuffer(const std::vector<Matrix>& transforms) {
    if (transforms.empty()) return 0;
    // Column-major float16 (m0,m1,m2,...), NOT the Matrix struct's memory order.
    std::vector<float16> data(transforms.size());
    for (size_t i = 0; i < transforms.size(); i++) data[i] = MatrixToFloatV(transforms[i]);
    return rlLoadVertexBuffer(data.data(), (int)(data.size() * sizeof(float16)), false);
}

void DestroyInstanceBuffer(unsigned int id) {
    if (id != 0) rlUnloadVertexBuffer(id);
}

void SetInstanceBuffersEnabled(bool enabled) { instanceBuffersEnabled = enabled; }
bool GetInstanceBuffersEnabled() { return instanceBuffersEnabled; }

bool InstanceBuffersActive() {
    return instanceBuffersEnabled && cityInstancedReady && cityInstancedShader.locs != nullptr &&
           cityInstancedShader.locs[SHADER_LOC_VERTEX_INSTANCETRANSFORM] != -1;
}

// Same as DrawCityInstances(), but the per-instance transforms come from a
// persistent buffer instead of being uploaded on every call.
void DrawCityInstancesBuffered(Mesh mesh, unsigned int instanceVbo, int count, Color tint) {
    if (!InstanceBuffersActive() || count <= 0 || instanceVbo == 0 || mesh.vaoId == 0) return;
    if (!inShadowPass) SetupInstancedLighting();
    cityInstancedMaterial.maps[MATERIAL_MAP_DIFFUSE].color = tint;
    DrawMeshInstancedBuffer(mesh, cityInstancedMaterial, instanceVbo, count);
    IncrementDrawCallCount(1);
    AddMeshCount(1);
    AddTriangleCount(mesh.triangleCount * count);
    const Texture2D& tex = cityInstancedMaterial.maps[MATERIAL_MAP_DIFFUSE].texture;
    if (tex.id > 0) {
        NoteTextureBound(tex.id);
        NoteMaterialUsed(cityInstancedShader.id);
    }
}

void SetShadowReuseEnabled(bool enabled) { shadowReuseEnabled = enabled; shadowsDirty = true; }
bool GetShadowReuseEnabled() { return shadowReuseEnabled; }
void MarkShadowsDirty() { shadowsDirty = true; }
bool IsShadowPassReused() { return inShadowPass && shadowPassReused; }

void SetUnderwaterParams(float waterY, Vector3, float, Vector3) {
    if (waterSurfaceYLoc != -1) SetShaderValue(litShader, waterSurfaceYLoc, &waterY, SHADER_UNIFORM_FLOAT);
    if (cityWaterSurfaceYLoc != -1 && cityInstancedShader.id != 0)
        SetShaderValue(cityInstancedShader, cityWaterSurfaceYLoc, &waterY, SHADER_UNIFORM_FLOAT);
    if (roadWaterSurfaceYLoc != -1 && roadShader.id != 0)
        SetShaderValue(roadShader, roadWaterSurfaceYLoc, &waterY, SHADER_UNIFORM_FLOAT);
}

void DrawGround() {
    if (!gridVisible) return;
    groundModel.transform = MatrixIdentity();
    DrawModel(groundModel, Vector3Zero(), 1.0f, WHITE);
}

} // namespace

// Per-frame render statistics. Reset once per frame by ResetFrameStats() and
// read by the player debug overlay.
static int g_drawCallCount = 0;
static int g_triangleCount = 0;
static int g_meshCount = 0;

// Frustum culling statistics (for debug overlay)
static int g_culledEntityCount = 0;
static int g_renderedEntityCount = 0;

// Global frustum culling toggle (for A/B comparison)
static bool g_frustumCullingEnabled = true;

// Last frame's per-pass wall times, published by Engine::Draw(). Not reset by
// ResetFrameStats(): these describe the frame that just finished, so zeroing
// them there would blank the overlay for the frame currently being built.
static gfx::FrameTimings g_frameTimings;

namespace {

// Fixed-capacity distinct-id set for the texture/material counters. A linear
// scan is fine at this size and keeps the frame allocation-free, which matters
// because this runs inside the render loop we are trying to measure. The cap is
// generous for a debug overlay; past it we record that we saturated instead of
// growing, so a wildly-textured frame degrades to an undercount rather than to
// a stutter.
constexpr int kDistinctIdCap = 1024;

struct DistinctIdSet {
    int ids[kDistinctIdCap];
    int count = 0;
    bool saturated = false;

    // Returns true if id was already present. Ids <= 0 are not resources and
    // are reported as already-seen so callers never have to guard.
    bool Add(int id) {
        if (id <= 0) return true;
        for (int i = 0; i < count; ++i) {
            if (ids[i] == id) return true;
        }
        if (count >= kDistinctIdCap) { saturated = true; return false; }
        ids[count++] = id;
        return false;
    }

    void Reset() { count = 0; saturated = false; }
};

DistinctIdSet g_textures;
DistinctIdSet g_materials;

} // namespace

int gfx::GetDrawCallCount() { return g_drawCallCount; }
void gfx::IncrementDrawCallCount(int count) { g_drawCallCount += count; }

int gfx::GetTriangleCount() { return g_triangleCount; }
void gfx::AddTriangleCount(int count) { g_triangleCount += count; }

int gfx::GetMeshCount() { return g_meshCount; }
void gfx::AddMeshCount(int count) { g_meshCount += count; }

int gfx::GetCulledEntityCount() { return g_culledEntityCount; }
int gfx::GetRenderedEntityCount() { return g_renderedEntityCount; }
void gfx::IncrementCulledEntityCount(int count) { g_culledEntityCount += count; }
void gfx::IncrementRenderedEntityCount(int count) { g_renderedEntityCount += count; }

int gfx::GetTextureCount() { return g_textures.count; }
int gfx::GetMaterialCount() { return g_materials.count; }
bool gfx::GetStatsSaturated() { return g_textures.saturated || g_materials.saturated; }

void gfx::NoteTextureBound(int id) { g_textures.Add(id); }
void gfx::NoteMaterialUsed(int id) { g_materials.Add(id); }

void gfx::ResetFrameStats() {
    g_drawCallCount = 0;
    g_triangleCount = 0;
    g_meshCount = 0;
    g_culledEntityCount = 0;
    g_renderedEntityCount = 0;
    g_textures.Reset();
    g_materials.Reset();
}

bool gfx::GetFrustumCullingEnabled() { return g_frustumCullingEnabled; }
void gfx::SetFrustumCullingEnabled(bool enabled) { g_frustumCullingEnabled = enabled; }

void gfx::SetFrameTimings(const FrameTimings& timings) { g_frameTimings = timings; }
gfx::FrameTimings gfx::GetFrameTimings() { return g_frameTimings; }