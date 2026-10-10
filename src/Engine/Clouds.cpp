#include "../../include/Engine/Clouds.hpp"
#include "../../include/Engine/Graphics.hpp"
#include "../../include/Engine/PostFX.hpp"
#include "raymath.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace gfx {

namespace {

constexpr int kShapeSize = 128;         // voxels along each side of the shape noise
constexpr int kDetailSize = 32;
constexpr int kWeatherSize = 256;
constexpr int kShadowSize = 256;
constexpr float kShadowArea = 9000.0f;  // metres of ground the cloud-shadow texture covers
constexpr float kShapeMetres = 5200.0f; // the shape noise repeats this often (before the Clouds item's size)
constexpr float kDetailMetres = 640.0f;
constexpr float kWeatherMetres = 36000.0f;
constexpr float kPlanetRadius = 6360000.0f;
constexpr float kExtinction = 0.028f;   // per metre, in the thick of a cloud

// --- Tiling noise --------------------------------------------------------------------------------------------
uint32_t Hash(uint32_t x, uint32_t y, uint32_t z, uint32_t seed) {
    uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ z * 0xcb1ab31fu ^ seed * 0x165667b1u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15; h *= 0x27d4eb2fu; h ^= h >> 16;
    return h;
}
float Hash01(uint32_t h) { return (float)(h & 0xffffffu) / 16777215.0f; }
int Wrap(int v, int period) { v %= period; return v < 0 ? v + period : v; }

// Worley noise that repeats every `cells` cells: 1 at a cell's point, falling to 0 a cell away.
float Worley(float x, float y, float z, int cells, uint32_t seed) {
    const float px = x * (float)cells, py = y * (float)cells, pz = z * (float)cells;
    const int ix = (int)std::floor(px), iy = (int)std::floor(py), iz = (int)std::floor(pz);
    const float fx = px - (float)ix, fy = py - (float)iy, fz = pz - (float)iz;
    float best = 1e9f;
    for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                const uint32_t cx = (uint32_t)Wrap(ix + dx, cells), cy = (uint32_t)Wrap(iy + dy, cells), cz = (uint32_t)Wrap(iz + dz, cells);
                const float ox = (float)dx + Hash01(Hash(cx, cy, cz, seed)) - fx;
                const float oy = (float)dy + Hash01(Hash(cx, cy, cz, seed + 1u)) - fy;
                const float oz = (float)dz + Hash01(Hash(cx, cy, cz, seed + 2u)) - fz;
                best = std::min(best, ox * ox + oy * oy + oz * oz);
            }
    return 1.0f - std::min(std::sqrt(best), 1.0f);
}
float WorleyFbm(float x, float y, float z, int cells, uint32_t seed) {
    return Worley(x, y, z, cells, seed) * 0.625f + Worley(x, y, z, cells * 2, seed + 7u) * 0.25f + Worley(x, y, z, cells * 4, seed + 13u) * 0.125f;
}

// Gradient noise that repeats every `period` cells, about -1..1.
float Perlin(float x, float y, float z, int period, uint32_t seed) {
    const float px = x * (float)period, py = y * (float)period, pz = z * (float)period;
    const int ix = (int)std::floor(px), iy = (int)std::floor(py), iz = (int)std::floor(pz);
    const float fx = px - (float)ix, fy = py - (float)iy, fz = pz - (float)iz;
    auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
    auto grad = [&](int cx, int cy, int cz, float dx, float dy, float dz) {
        const uint32_t h = Hash((uint32_t)Wrap(cx, period), (uint32_t)Wrap(cy, period), (uint32_t)Wrap(cz, period), seed);
        // One of twelve edge directions of a cube.
        switch (h % 12u) {
            case 0: return dx + dy; case 1: return -dx + dy; case 2: return dx - dy; case 3: return -dx - dy;
            case 4: return dx + dz; case 5: return -dx + dz; case 6: return dx - dz; case 7: return -dx - dz;
            case 8: return dy + dz; case 9: return -dy + dz; case 10: return dy - dz; default: return -dy - dz;
        }
    };
    const float u = fade(fx), v = fade(fy), w = fade(fz);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float x00 = lerp(grad(ix, iy, iz, fx, fy, fz), grad(ix + 1, iy, iz, fx - 1.0f, fy, fz), u);
    const float x10 = lerp(grad(ix, iy + 1, iz, fx, fy - 1.0f, fz), grad(ix + 1, iy + 1, iz, fx - 1.0f, fy - 1.0f, fz), u);
    const float x01 = lerp(grad(ix, iy, iz + 1, fx, fy, fz - 1.0f), grad(ix + 1, iy, iz + 1, fx - 1.0f, fy, fz - 1.0f), u);
    const float x11 = lerp(grad(ix, iy + 1, iz + 1, fx, fy - 1.0f, fz - 1.0f), grad(ix + 1, iy + 1, iz + 1, fx - 1.0f, fy - 1.0f, fz - 1.0f), u);
    return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}
float PerlinFbm(float x, float y, float z, int period, int octaves, uint32_t seed) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; i++) {
        sum += Perlin(x, y, z, period, seed + (uint32_t)i * 31u) * amp;
        norm += amp;
        amp *= 0.5f;
        period *= 2;
    }
    return sum / norm;
}
float Remap(float v, float lo, float hi, float newLo, float newHi) { return newLo + (v - lo) / (hi - lo) * (newHi - newLo); }
float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
uint8_t ToByte(float v) { return (uint8_t)std::lround(Clamp01(v) * 255.0f); }

// The billows: Perlin-Worley noise (Schneider, "The Real-time Volumetric Cloudscapes of Horizon Zero Dawn"),
// with its erosion by lower-frequency Worley noise already applied, so the shader reads one channel.
void BuildShapeSlice(std::vector<uint8_t>& out, int z) {
    const int n = kShapeSize;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            const float u = ((float)x + 0.5f) / (float)n, v = ((float)y + 0.5f) / (float)n, w = ((float)z + 0.5f) / (float)n;
            float perlin = PerlinFbm(u, v, w, 4, 6, 101u) * 0.5f + 0.5f;
            perlin = std::fabs((0.5f + 0.5f * perlin) * 2.0f - 1.0f);
            const float w1 = WorleyFbm(u, v, w, 4, 11u), w2 = WorleyFbm(u, v, w, 8, 23u), w3 = WorleyFbm(u, v, w, 16, 37u);
            const float pw = Remap(perlin, 0.0f, 1.0f, w1, 1.0f);
            const float erode = w1 * 0.625f + w2 * 0.25f + w3 * 0.125f;
            out[(size_t)((z * n + y) * n + x)] = ToByte(Remap(pw, erode - 1.0f, 1.0f, 0.0f, 1.0f));
        }
}

void BuildDetailSlice(std::vector<uint8_t>& out, int z) {
    const int n = kDetailSize;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            const float u = ((float)x + 0.5f) / (float)n, v = ((float)y + 0.5f) / (float)n, w = ((float)z + 0.5f) / (float)n;
            out[(size_t)((z * n + y) * n + x)] = ToByte(WorleyFbm(u, v, w, 2, 71u));
        }
}

// The weather map: r = where cloud forms (broad patches with ragged edges), g = how tall it grows there.
void BuildWeather(std::vector<uint8_t>& out) {
    const int n = kWeatherSize;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            const float u = ((float)x + 0.5f) / (float)n, v = ((float)y + 0.5f) / (float)n;
            const float broad = PerlinFbm(u, v, 0.37f, 3, 5, 301u) * 0.5f + 0.5f;
            const float ragged = Worley(u, v, 0.5f, 7, 311u);
            const float cover = Clamp01(Remap(broad * 0.72f + ragged * 0.28f, 0.22f, 0.78f, 0.0f, 1.0f));
            const float tall = Clamp01(PerlinFbm(u, v, 0.81f, 2, 4, 331u) * 0.9f + 0.5f);
            const size_t i = (size_t)(y * n + x) * 4;
            out[i] = ToByte(cover); out[i + 1] = ToByte(tall); out[i + 2] = 0; out[i + 3] = 255;
        }
}

struct CloudState {
    bool initialized = false;
    // Noise: made on worker threads the first time clouds are wanted, uploaded when done.
    std::thread worker;
    std::atomic<bool> building{ false }, built{ false };
    std::vector<uint8_t> shapeData, detailData, weatherData;
    Texture2D shapeTex{}, detailTex{}, weatherTex{};
    bool ready = false;

    Shader marchShader{};
    int camProjLoc = -1, camDepthLoc = -1, camInvViewLoc = -1, originLoc = -1, layerLoc = -1, shapeLoc = -1, windLoc = -1;
    int sunLoc = -1, sunLightLoc = -1, ambTopLoc = -1, ambBottomLoc = -1, paramsLoc = -1;
    int depthTexLoc = -1, weatherTexLoc = -1, shapeTexLoc = -1, detailTexLoc = -1;
    AtmosphereLocs marchAt{};
    RenderTexture2D target{};
    int targetW = 0, targetH = 0;

    Shader shadowShader{};
    int shOriginLoc = -1, shLayerLoc = -1, shShapeLoc = -1, shWindLoc = -1, shSunLoc = -1, shAreaLoc = -1;
    int shWeatherTexLoc = -1, shShapeTexLoc = -1, shDetailTexLoc = -1;
    RenderTexture2D shadow{};
    Vector4 shadowParams{};

    Vector2 windShape{}, windWeather{};     // how far the wind has carried the clouds (metres)

    // The worker writes into the vectors above: it must be done before they go.
    ~CloudState() { if (worker.joinable()) worker.join(); }
};
CloudState cs;

void SetVec4(Shader sh, int loc, const Vector4& v) {
    if (loc >= 0) SetShaderValue(sh, loc, &v, SHADER_UNIFORM_VEC4);
}

void StartNoiseBuild() {
    if (cs.building.exchange(true)) return;
    cs.worker = std::thread([] {
        cs.shapeData.assign((size_t)kShapeSize * kShapeSize * kShapeSize, 0);
        cs.detailData.assign((size_t)kDetailSize * kDetailSize * kDetailSize, 0);
        cs.weatherData.assign((size_t)kWeatherSize * kWeatherSize * 4, 0);
        const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
        std::atomic<int> next{ 0 };
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; t++)
            pool.emplace_back([&] {
                for (int z = next.fetch_add(1); z < kShapeSize; z = next.fetch_add(1)) BuildShapeSlice(cs.shapeData, z);
            });
        for (int z = 0; z < kDetailSize; z++) BuildDetailSlice(cs.detailData, z);
        BuildWeather(cs.weatherData);
        for (std::thread& t : pool) t.join();
        cs.built = true;
    });
}

// Uploads the noise once the worker has finished it.
void PollNoise() {
    if (cs.ready || !cs.built.load()) return;
    if (cs.worker.joinable()) cs.worker.join();
    cs.shapeTex = LoadTexture3D(cs.shapeData.data(), kShapeSize, kShapeSize, kShapeSize, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);
    cs.detailTex = LoadTexture3D(cs.detailData.data(), kDetailSize, kDetailSize, kDetailSize, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);
    Image img{};
    img.data = cs.weatherData.data();
    img.width = kWeatherSize;
    img.height = kWeatherSize;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    cs.weatherTex = LoadTextureFromImage(img);
    SetTextureFilter(cs.weatherTex, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(cs.weatherTex, TEXTURE_WRAP_REPEAT);
    std::vector<uint8_t>().swap(cs.shapeData);
    std::vector<uint8_t>().swap(cs.detailData);
    cs.ready = cs.shapeTex.id != 0 && cs.detailTex.id != 0 && cs.weatherTex.id != 0;
}

// The layer, shared by the march and the shadow pass.
struct Layer { Vector4 origin, layer, shape, wind, sun; };
Layer MakeLayer(Vector3 camPos) {
    const LightingSettings& L = Lighting();
    Layer l;
    const float size = std::clamp(L.cloudScale, 0.3f, 3.0f);
    const float thickness = std::clamp(L.cloudThickness, 200.0f, 6000.0f);
    // Rain cloud is thicker and darker underneath.
    const float density = kExtinction * std::clamp(L.cloudDensity, 0.2f, 3.0f) * (1.0f + 0.9f * RainNow());
    l.origin = { camPos.x, camPos.y, camPos.z, 1.0f / (2.0f * kPlanetRadius) };
    // Even a full sky of cloud keeps thinner places in it (a sheet of cloud is mottled, not one grey).
    l.layer = { std::clamp(L.cloudBase, 100.0f, 12000.0f), thickness, std::min(CloudCoverageNow(), 0.9f), density };
    l.shape = { 1.0f / (kShapeMetres * size), 1.0f / (kDetailMetres * size), 1.0f / (kWeatherMetres * size), 0.42f };
    l.wind = { cs.windShape.x, cs.windShape.y, cs.windWeather.x, cs.windWeather.y };
    const Vector3 toKey = Vector3Negate(SunDirection());
    l.sun = { toKey.x, toKey.y, toKey.z, 0.0f };
    return l;
}

} // namespace

void InitClouds() {
    if (cs.initialized) return;
    cs.marchShader = LoadShaderProgram("clouds_march");
    cs.camProjLoc = GetShaderLocation(cs.marchShader, "camProj");
    cs.camDepthLoc = GetShaderLocation(cs.marchShader, "camDepth");
    cs.camInvViewLoc = GetShaderLocation(cs.marchShader, "camInvView");
    cs.originLoc = GetShaderLocation(cs.marchShader, "clOrigin");
    cs.layerLoc = GetShaderLocation(cs.marchShader, "clLayer");
    cs.shapeLoc = GetShaderLocation(cs.marchShader, "clShape");
    cs.windLoc = GetShaderLocation(cs.marchShader, "clWind");
    cs.sunLoc = GetShaderLocation(cs.marchShader, "clSun");
    cs.sunLightLoc = GetShaderLocation(cs.marchShader, "clSunLight");
    cs.ambTopLoc = GetShaderLocation(cs.marchShader, "clAmbTop");
    cs.ambBottomLoc = GetShaderLocation(cs.marchShader, "clAmbBottom");
    cs.paramsLoc = GetShaderLocation(cs.marchShader, "clParams");
    cs.depthTexLoc = GetShaderLocation(cs.marchShader, "clDepthTex");
    cs.weatherTexLoc = GetShaderLocation(cs.marchShader, "weatherTex");
    cs.shapeTexLoc = GetShaderLocation(cs.marchShader, "shapeTex");
    cs.detailTexLoc = GetShaderLocation(cs.marchShader, "detailTex");
    cs.marchAt = FindAtmosphereLocs(cs.marchShader);

    cs.shadowShader = LoadShaderProgram("clouds_shadow");
    cs.shOriginLoc = GetShaderLocation(cs.shadowShader, "clOrigin");
    cs.shLayerLoc = GetShaderLocation(cs.shadowShader, "clLayer");
    cs.shShapeLoc = GetShaderLocation(cs.shadowShader, "clShape");
    cs.shWindLoc = GetShaderLocation(cs.shadowShader, "clWind");
    cs.shSunLoc = GetShaderLocation(cs.shadowShader, "clSun");
    cs.shAreaLoc = GetShaderLocation(cs.shadowShader, "csArea");
    cs.shWeatherTexLoc = GetShaderLocation(cs.shadowShader, "weatherTex");
    cs.shShapeTexLoc = GetShaderLocation(cs.shadowShader, "shapeTex");
    cs.shDetailTexLoc = GetShaderLocation(cs.shadowShader, "detailTex");
    cs.shadow = LoadRenderTextureEx(kShadowSize, kShadowSize, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE, false);
    cs.initialized = true;
}

void ShutdownClouds() {
    if (cs.worker.joinable()) cs.worker.join();
    if (!cs.initialized) return;
    UnloadShader(cs.marchShader);
    UnloadShader(cs.shadowShader);
    if (cs.target.id > 0) UnloadRenderTexture(cs.target);
    if (cs.shadow.id > 0) UnloadRenderTexture(cs.shadow);
    if (cs.shapeTex.id != 0) UnloadTexture(cs.shapeTex);
    if (cs.detailTex.id != 0) UnloadTexture(cs.detailTex);
    if (cs.weatherTex.id != 0) UnloadTexture(cs.weatherTex);
    cs.initialized = false;
    cs.ready = false;
    cs.built = false;
    cs.building = false;
    cs.shapeTex = cs.detailTex = cs.weatherTex = Texture2D{};
    cs.target = cs.shadow = RenderTexture2D{};
    cs.targetW = cs.targetH = 0;
    cs.shadowParams = {};
}

float CloudCoverageNow() {
    if (!AtmosphereActive()) return 0.0f;
    const LightingSettings& L = Lighting();
    const float base = L.hasClouds ? std::clamp(L.cloudCoverage, 0.0f, 1.0f) : 0.0f;
    return std::clamp(base + (1.0f - base) * OvercastNow(), 0.0f, 1.0f);
}

bool CloudsActive() {
    if (CloudCoverageNow() < 0.01f) return false;
    if (!cs.ready) {
        // Wanted for the first time: start making the noise, and draw clouds once it is there.
        if (!cs.initialized) InitClouds();
        StartNoiseBuild();
        PollNoise();
    }
    return cs.ready;
}

float CloudSunlightNow() {
    if (!CloudsActive()) return 1.0f;
    const float c = CloudCoverageNow();
    return 0.06f + 0.94f * std::pow(1.0f - c, 1.6f);
}

void UpdateClouds(float dt) {
    const LightingSettings& L = Lighting();
    const float a = L.windDirection * DEG2RAD;
    // The shader adds these to the position it samples at, so the clouds move the other way.
    const Vector2 step = { -std::sin(a) * L.windSpeed * dt, std::cos(a) * L.windSpeed * dt };
    cs.windShape = Vector2Add(cs.windShape, step);
    cs.windWeather = Vector2Add(cs.windWeather, Vector2Scale(step, 0.6f));     // the patches drift slower than the billows boil
    // Keep the offsets small (the noise repeats), so they stay precise on a long session.
    const float wrapShape = kShapeMetres * 8.0f * 3.0f, wrapWeather = kWeatherMetres * 3.0f;
    cs.windShape = { std::fmod(cs.windShape.x, wrapShape), std::fmod(cs.windShape.y, wrapShape) };
    cs.windWeather = { std::fmod(cs.windWeather.x, wrapWeather), std::fmod(cs.windWeather.y, wrapWeather) };
}

Texture2D RenderClouds(const ViewInfo& view, Texture2D sceneDepth, int sceneWidth, int sceneHeight, int frame) {
    if (!CloudsActive() || view.ortho) return Texture2D{};
    const int quality = std::clamp(Quality().clouds, 0, 3);
    const int div = quality == 0 ? 4 : 2;
    const int w = std::max(1, (sceneWidth + div - 1) / div), h = std::max(1, (sceneHeight + div - 1) / div);
    if (cs.target.id == 0 || cs.targetW != w || cs.targetH != h) {
        if (cs.target.id > 0) UnloadRenderTexture(cs.target);
        cs.target = LoadRenderTextureEx(w, h, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16, false);
        cs.targetW = w;
        cs.targetH = h;
    }
    static const int kSteps[4] = { 28, 44, 64, 96 }, kSunSteps[4] = { 3, 4, 6, 6 };

    const Layer l = MakeLayer(view.pos);
    const SkyLight& sky = AtmosphereLight();
    // The sun's (or the moon's) light on the clouds, the clear sky's light on their tops and the light
    // that comes up from the ground on their undersides.
    const Vector3 light = sky.keyLight;
    const Vector3 top = sky.clearSky;
    const Vector3 bottom = Vector3Add(Vector3Scale(sky.ambientGround, 0.8f), Vector3Scale(sky.clearSky, 0.25f));

    Shader sh = cs.marchShader;
    SetVec4(sh, cs.camProjLoc, view.proj);
    SetVec4(sh, cs.camDepthLoc, view.depth);
    SetShaderValueMatrix(sh, cs.camInvViewLoc, view.invView);
    SetVec4(sh, cs.originLoc, l.origin);
    SetVec4(sh, cs.layerLoc, l.layer);
    SetVec4(sh, cs.shapeLoc, l.shape);
    SetVec4(sh, cs.windLoc, l.wind);
    SetVec4(sh, cs.sunLoc, { l.sun.x, l.sun.y, l.sun.z, 1.0f / 42000.0f });
    SetVec4(sh, cs.sunLightLoc, { light.x, light.y, light.z, 0.0f });
    SetVec4(sh, cs.ambTopLoc, { top.x, top.y, top.z, 0.0f });
    SetVec4(sh, cs.ambBottomLoc, { bottom.x, bottom.y, bottom.z, 0.0f });
    SetVec4(sh, cs.paramsLoc, { (float)kSteps[quality], (float)kSunSteps[quality], (float)(frame % 8) * 5.3f, 70000.0f });
    SetAtmosphereUniforms(sh, cs.marchAt);
    SetShaderValueTexture(sh, cs.depthTexLoc, sceneDepth);
    SetShaderValueTexture(sh, cs.weatherTexLoc, cs.weatherTex);
    SetShaderValueTexture(sh, cs.shapeTexLoc, cs.shapeTex);
    SetShaderValueTexture(sh, cs.detailTexLoc, cs.detailTex);
    BeginTextureMode(cs.target);
    DrawFullscreen(sh, -1);
    EndTextureMode();
    return cs.target.texture;
}

void UpdateCloudShadow(const Camera3D& camera) {
    if (!CloudsActive()) { cs.shadowParams = {}; return; }
    const Layer l = MakeLayer(camera.position);
    // The square follows the camera in whole texels, so the shadows do not shimmer as it moves.
    const float texel = kShadowArea / (float)kShadowSize;
    const float cx = std::floor(camera.position.x / texel) * texel, cz = std::floor(camera.position.z / texel) * texel;
    Shader sh = cs.shadowShader;
    SetVec4(sh, cs.shOriginLoc, l.origin);
    SetVec4(sh, cs.shLayerLoc, l.layer);
    SetVec4(sh, cs.shShapeLoc, l.shape);
    SetVec4(sh, cs.shWindLoc, l.wind);
    SetVec4(sh, cs.shSunLoc, l.sun);
    SetVec4(sh, cs.shAreaLoc, { cx, cz, kShadowArea, 0.0f });
    SetShaderValueTexture(sh, cs.shWeatherTexLoc, cs.weatherTex);
    SetShaderValueTexture(sh, cs.shShapeTexLoc, cs.shapeTex);
    SetShaderValueTexture(sh, cs.shDetailTexLoc, cs.detailTex);
    BeginTextureMode(cs.shadow);
    DrawFullscreen(sh, -1);
    EndTextureMode();
    cs.shadowParams = { cx - kShadowArea * 0.5f, cz - kShadowArea * 0.5f, 1.0f / kShadowArea, l.layer.x };
}

Texture2D GetCloudShadowTexture() { return cs.shadowParams.w > 0.0f ? cs.shadow.texture : Texture2D{}; }
Vector4 GetCloudShadowParams() { return cs.shadowParams; }

} // namespace gfx
