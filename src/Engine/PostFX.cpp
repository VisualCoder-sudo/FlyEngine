#include "../../include/Engine/PostFX.hpp"
#include "../../include/Engine/Atmosphere.hpp"
#include "../../include/Engine/Graphics.hpp"
#include "raymath.h"

#include <algorithm>
#include <cmath>

namespace gfx {

namespace {

constexpr int kLumaSize = 64;

struct PostState {
    bool initialized = false;
    bool sceneActive = false;
    int width = 0, height = 0;          // size of the scene target

    RenderTexture2D scene{};            // float colour + depth: the lit scene
    RenderTexture2D exposure[2]{};      // 1x1: this frame's exposure and the previous one
    int exposureIndex = 0;
    bool exposureReset = true;
    RenderTexture2D luma{};             // log luminance of the last frame, for eye adaptation

    Shader exposureShader{};
    int expParamsLoc = -1, expRangeLoc = -1, expLumaLoc = -1, expPrevLoc = -1;
    Shader lumaShader{};
    int lumaSrcLoc = -1;
    Shader tonemapShader{};
    int tmParamsLoc = -1, tmGradeLoc = -1, tmTexelLoc = -1, tmSceneLoc = -1, tmBloomLoc = -1, tmExposureLoc = -1;
};
PostState ps;
RenderQuality quality;

float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float Max3(Vector3 c) { return std::max(c.x, std::max(c.y, c.z)); }

// --- Tone curves, mirrored from shaders/post.glsl ---------------------------------------------------------
constexpr float kNeutralKnee = 0.76f;
constexpr float kNeutralDesaturation = 0.15f;

Vector3 CurveNeutral(Vector3 c) {
    const float peak = Max3(c);
    if (peak < kNeutralKnee) return c;
    const float d = 1.0f - kNeutralKnee;
    const float newPeak = 1.0f - d * d / (peak + d - kNeutralKnee);
    c = Vector3Scale(c, newPeak / peak);
    const float g = 1.0f - 1.0f / (kNeutralDesaturation * (peak - newPeak) + 1.0f);
    return Vector3Lerp(c, { newPeak, newPeak, newPeak }, g);
}

Vector3 CurveNeutralInverse(Vector3 o) {
    const float newPeak = Max3(o);
    if (newPeak < kNeutralKnee) return o;
    const float d = 1.0f - kNeutralKnee;
    const float peak = d * d / std::max(1.0f - newPeak, 1e-4f) - d + kNeutralKnee;
    const float g = 1.0f - 1.0f / (kNeutralDesaturation * (peak - newPeak) + 1.0f);
    // o = mix(c, newPeak, g)  ->  c = (o - g * newPeak) / (1 - g)
    Vector3 c = { (o.x - g * newPeak) / (1.0f - g), (o.y - g * newPeak) / (1.0f - g), (o.z - g * newPeak) / (1.0f - g) };
    c = { std::max(c.x, 0.0f), std::max(c.y, 0.0f), std::max(c.z, 0.0f) };
    return Vector3Scale(c, peak / newPeak);
}

// m holds the matrix by rows.
Vector3 Mul3(const float m[9], Vector3 v) {
    return { m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z };
}

Vector3 CurveACES(Vector3 c) {
    static const float in[9] = { 0.59719f, 0.35458f, 0.04823f, 0.07600f, 0.90834f, 0.01566f, 0.02840f, 0.13383f, 0.83777f };
    static const float out[9] = { 1.60475f, -0.53108f, -0.07367f, -0.10208f, 1.10813f, -0.00605f, -0.00327f, -0.07276f, 1.07602f };
    const Vector3 v = Mul3(in, Vector3Scale(c, 1.7f));
    auto fit = [](float x) { return (x * (x + 0.0245786f) - 0.000090537f) / (x * (0.983729f * x + 0.4329510f) + 0.238081f); };
    const Vector3 r = Mul3(out, { fit(v.x), fit(v.y), fit(v.z) });
    return { Clamp01(r.x), Clamp01(r.y), Clamp01(r.z) };
}

Vector3 CurveAgX(Vector3 c) {
    static const float in[9] = { 0.842479062253094f, 0.0784335999999992f, 0.0792237451477643f,
                                 0.0423282422610123f, 0.878468636469772f, 0.0791661274605434f,
                                 0.0423756549057051f, 0.0784336f, 0.879142973793104f };
    static const float out[9] = { 1.19687900512017f, -0.0980208811401368f, -0.0990297440797205f,
                                  -0.0528968517574562f, 1.15190312990417f, -0.0989611768448433f,
                                  -0.0529716355144438f, -0.0980434501171241f, 1.15107367264116f };
    const float minEv = -12.47393f, maxEv = 4.026069f;
    Vector3 v = Mul3(in, { std::max(c.x, 0.0f), std::max(c.y, 0.0f), std::max(c.z, 0.0f) });
    auto shape = [&](float x) {
        x = std::clamp(std::log2(std::max(x, 1e-10f)), minEv, maxEv);
        x = (x - minEv) / (maxEv - minEv);
        const float x2 = x * x, x4 = x2 * x2;
        return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
    };
    v = Mul3(out, { shape(v.x), shape(v.y), shape(v.z) });
    return { std::pow(Clamp01(v.x), 2.2f), std::pow(Clamp01(v.y), 2.2f), std::pow(Clamp01(v.z), 2.2f) };
}

float SrgbToLinear1(float c) {
    c = std::max(c, 0.0f);
    return c < 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float LinearToSrgb1(float c) {
    c = std::max(c, 0.0f);
    return c < 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

void UnloadTargets() {
    if (ps.scene.id > 0) UnloadRenderTexture(ps.scene);
    ps.scene = {};
    ps.width = ps.height = 0;
}

void EnsureTargets(int width, int height) {
    if (ps.scene.id > 0 && ps.width == width && ps.height == height) return;
    UnloadTargets();
    ps.scene = LoadRenderTextureEx(width, height, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16, true);
    ps.width = width;
    ps.height = height;
}

// The exposure of this frame, into a 1x1 target every pass can read.
void UpdateExposure() {
    const LightingSettings& L = Lighting();
    const int prev = ps.exposureIndex, cur = 1 - prev;
    const bool adapt = L.autoExposure && AtmosphereActive();
    const float dt = std::clamp(GetFrameTime(), 0.0f, 0.25f);
    const Vector4 params = { std::pow(2.0f, std::clamp(L.exposure, -8.0f, 8.0f)), adapt ? 1.0f : 0.0f,
                             1.0f - std::exp(-dt * 1.6f), ps.exposureReset ? 1.0f : 0.0f };
    const Vector4 range = { 0.35f, 10.0f, 0.26f, 0.0f };
    SetShaderValue(ps.exposureShader, ps.expParamsLoc, &params, SHADER_UNIFORM_VEC4);
    SetShaderValue(ps.exposureShader, ps.expRangeLoc, &range, SHADER_UNIFORM_VEC4);
    SetShaderValueTexture(ps.exposureShader, ps.expLumaLoc, ps.luma.texture);
    SetShaderValueTexture(ps.exposureShader, ps.expPrevLoc, ps.exposure[prev].texture);
    BeginTextureMode(ps.exposure[cur]);
    DrawFullscreen(ps.exposureShader, -1);
    EndTextureMode();
    ps.exposureIndex = cur;
    ps.exposureReset = false;
}

} // namespace

RenderQuality& Quality() { return quality; }

void SetQualityTier(int tier) {
    tier = std::clamp(tier, 0, 3);
    RenderQuality q;
    q.tier = tier;
    q.renderScale = 1.0f;
    q.antiAliasing = tier == 0 ? 0 : (tier >= 3 ? 2 : 1);
    q.bloom = true;
    q.ambientOcclusion = tier >= 1;
    q.clouds = tier;
    q.volumetrics = tier == 0 ? 0 : (tier == 1 ? 1 : 2);
    q.shadowCascades = tier == 0 ? 1 : (tier == 1 ? 2 : (tier == 2 ? 3 : 4));
    quality = q;
}

void InitPostFX() {
    if (ps.initialized) return;
    ps.exposureShader = LoadShaderProgram("post_exposure");
    ps.expParamsLoc = GetShaderLocation(ps.exposureShader, "expParams");
    ps.expRangeLoc = GetShaderLocation(ps.exposureShader, "expRange");
    ps.expLumaLoc = GetShaderLocation(ps.exposureShader, "lumaTex");
    ps.expPrevLoc = GetShaderLocation(ps.exposureShader, "prevExposureTex");
    ps.lumaShader = LoadShaderProgram("post_luma");
    ps.lumaSrcLoc = GetShaderLocation(ps.lumaShader, "srcTex");
    ps.tonemapShader = LoadShaderProgram("post_tonemap");
    ps.tmParamsLoc = GetShaderLocation(ps.tonemapShader, "tmParams");
    ps.tmGradeLoc = GetShaderLocation(ps.tonemapShader, "tmGrade");
    ps.tmTexelLoc = GetShaderLocation(ps.tonemapShader, "tmTexel");
    ps.tmSceneLoc = GetShaderLocation(ps.tonemapShader, "sceneTex");
    ps.tmBloomLoc = GetShaderLocation(ps.tonemapShader, "bloomTex");
    ps.tmExposureLoc = GetShaderLocation(ps.tonemapShader, "exposureTex");

    for (RenderTexture2D& e : ps.exposure) {
        e = LoadRenderTextureEx(1, 1, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16, false);
        SetTextureFilter(e.texture, TEXTURE_FILTER_POINT);
    }
    ps.luma = LoadRenderTextureEx(kLumaSize, kLumaSize, PIXELFORMAT_UNCOMPRESSED_R16, false);
    ps.exposureReset = true;
    InitAtmosphere();
    ps.initialized = true;
}

void ShutdownPostFX() {
    if (!ps.initialized) return;
    ShutdownAtmosphere();
    UnloadTargets();
    for (RenderTexture2D& e : ps.exposure) if (e.id > 0) UnloadRenderTexture(e);
    if (ps.luma.id > 0) UnloadRenderTexture(ps.luma);
    UnloadShader(ps.exposureShader);
    UnloadShader(ps.lumaShader);
    UnloadShader(ps.tonemapShader);
    ps = PostState{};
}

Texture2D GetExposureTexture() { return ps.exposure[ps.exposureIndex].texture; }
bool IsSceneActive() { return ps.sceneActive; }

void BeginScene(const Camera3D& camera) {
    if (!ps.initialized) InitPostFX();
    const float scale = std::clamp(quality.renderScale, 0.25f, 1.0f);
    const int w = std::max(1, (int)std::lround((float)GetScreenWidth() * scale));
    const int h = std::max(1, (int)std::lround((float)GetScreenHeight() * scale));
    EnsureTargets(w, h);
    UpdateExposure();

    BeginTextureMode(ps.scene);
    ClearBackground(BLACK);
    DrawSky(camera);
    ps.sceneActive = true;
}

void EndScene() {
    if (!ps.sceneActive) return;
    EndTextureMode();
    ps.sceneActive = false;
    const LightingSettings& L = Lighting();
    const Texture2D src = ps.scene.texture;

    // The picture's brightness, for the next frame's exposure.
    SetShaderValueTexture(ps.lumaShader, ps.lumaSrcLoc, src);
    BeginTextureMode(ps.luma);
    DrawFullscreen(ps.lumaShader, -1);
    EndTextureMode();

    // Exposure, tone curve, grading: into the screen target.
    const Vector4 params = { (float)std::clamp(L.toneCurve, 0, 3), 0.0f, Clamp01(L.vignette), Clamp01(L.filmGrain) };
    const Vector4 grade = { std::clamp(L.contrast, 0.0f, 2.0f), std::clamp(L.saturation, 0.0f, 2.0f),
                            std::clamp(L.temperature, -1.0f, 1.0f), (float)GetTime() };
    const Vector4 texel = { 1.0f / (float)ps.width, 1.0f / (float)ps.height, 0.0f, 0.0f };
    SetShaderValue(ps.tonemapShader, ps.tmParamsLoc, &params, SHADER_UNIFORM_VEC4);
    SetShaderValue(ps.tonemapShader, ps.tmGradeLoc, &grade, SHADER_UNIFORM_VEC4);
    SetShaderValue(ps.tonemapShader, ps.tmTexelLoc, &texel, SHADER_UNIFORM_VEC4);
    SetShaderValueTexture(ps.tonemapShader, ps.tmSceneLoc, src);
    SetShaderValueTexture(ps.tonemapShader, ps.tmExposureLoc, GetExposureTexture());
    DrawFullscreen(ps.tonemapShader, -1);
}

Vector3 TonemapApply(int curve, Vector3 c) {
    c = { std::max(c.x, 0.0f), std::max(c.y, 0.0f), std::max(c.z, 0.0f) };
    Vector3 r = c;
    if (curve == 1) r = CurveNeutral(c);
    else if (curve == 2) r = CurveACES(c);
    else if (curve == 3) r = CurveAgX(c);
    return { Clamp01(r.x), Clamp01(r.y), Clamp01(r.z) };
}

Vector3 TonemapInverse(int curve, Vector3 d) {
    d = { std::clamp(d.x, 0.0f, 0.995f), std::clamp(d.y, 0.0f, 0.995f), std::clamp(d.z, 0.0f, 0.995f) };
    if (curve <= 0) return d;
    if (curve == 1) return CurveNeutralInverse(d);
    // The filmic curves mix the channels, so they are inverted by iteration: scale each channel by how
    // far the curve's result is from the wanted one. A colour the curve cannot produce (the filmic
    // curves never reach a pure primary) ends up as the nearest one it can.
    Vector3 x = { std::max(d.x, 1e-4f), std::max(d.y, 1e-4f), std::max(d.z, 1e-4f) };
    const float eps = 1e-5f;
    for (int i = 0; i < 96; i++) {
        const Vector3 t = TonemapApply(curve, x);
        x.x = std::clamp(x.x * std::clamp((d.x + eps) / (t.x + eps), 0.5f, 2.0f), 1e-6f, 64.0f);
        x.y = std::clamp(x.y * std::clamp((d.y + eps) / (t.y + eps), 0.5f, 2.0f), 1e-6f, 64.0f);
        x.z = std::clamp(x.z * std::clamp((d.z + eps) / (t.z + eps), 0.5f, 2.0f), 1e-6f, 64.0f);
    }
    return x;
}

Vector3 SrgbToLinear(Vector3 c) { return { SrgbToLinear1(c.x), SrgbToLinear1(c.y), SrgbToLinear1(c.z) }; }
Vector3 LinearToSrgb(Vector3 c) { return { LinearToSrgb1(c.x), LinearToSrgb1(c.y), LinearToSrgb1(c.z) }; }
Vector3 SrgbToLinear(Color c) { return SrgbToLinear(Vector3{ c.r / 255.0f, c.g / 255.0f, c.b / 255.0f }); }

Vector3 DisplayColorToScene(Color c) {
    return TonemapInverse(std::clamp(Lighting().toneCurve, 0, 3), SrgbToLinear(c));
}

} // namespace gfx
