#include "../../include/Engine/PostFX.hpp"
#include "../../include/Engine/Atmosphere.hpp"
#include "../../include/Engine/Clouds.hpp"
#include "../../include/Engine/Graphics.hpp"
#include "../../include/Engine/Platform/Platform.hpp"
#include "raymath.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

    RenderTexture2D hdr{};              // the scene with air and fog laid over it
    RenderTexture2D fog{};              // half size: light added by air and fog, and what they let through
    std::vector<RenderTexture2D> bloom; // half size, then halved again and again
    RenderTexture2D ldr{};              // the finished picture before anti-aliasing
    ViewInfo view{};                    // the camera of this frame

    Shader exposureShader{};
    int expParamsLoc = -1, expRangeLoc = -1, expLumaLoc = -1, expPrevLoc = -1;
    Shader lumaShader{};
    int lumaSrcLoc = -1;
    Shader tonemapShader{};
    int tmParamsLoc = -1, tmGradeLoc = -1, tmTexelLoc = -1, tmSceneLoc = -1, tmBloomLoc = -1, tmExposureLoc = -1;
    Shader fogShader{};
    int fgCamProjLoc = -1, fgCamDepthLoc = -1, fgCamInvViewLoc = -1, fgAirRLoc = -1, fgAirMLoc = -1, fgAirExtLoc = -1;
    int fgLightLoc = -1, fgLightDirLoc = -1, fgMultiLoc = -1, fgFogLoc = -1, fgHazeLoc = -1, fgFogSunLoc = -1, fgFogAmbLoc = -1, fgParamsLoc = -1;
    int fgLightVPLoc = -1, fgDepthLoc = -1, fgShadowLoc = -1, fgExposureLoc = -1, fgCloudShadowLoc = -1, fgCloudShadowTexLoc = -1;
    Shader compositeShader{};
    int cpCamDepthLoc = -1, cpTexelLoc = -1, cpParamsLoc = -1, cpSceneLoc = -1, cpDepthLoc = -1, cpFogLoc = -1;
    int cpCloudTexelLoc = -1, cpCloudLoc = -1;
    Shader bloomDownShader{}, bloomUpShader{};
    int bdParamsLoc = -1, bdSrcLoc = -1, buParamsLoc = -1, buSrcLoc = -1;
    Shader fxaaShader{};
    int fxTexelLoc = -1, fxSrcLoc = -1;
    int frame = 0;
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

void Unload(RenderTexture2D& rt) {
    if (rt.id > 0) UnloadRenderTexture(rt);
    rt = {};
}

void UnloadTargets() {
    Unload(ps.scene);
    Unload(ps.hdr);
    Unload(ps.fog);
    Unload(ps.ldr);
    for (RenderTexture2D& b : ps.bloom) Unload(b);
    ps.bloom.clear();
    ps.width = ps.height = 0;
}

void EnsureTargets(int width, int height) {
    if (ps.scene.id > 0 && ps.width == width && ps.height == height) return;
    UnloadTargets();
    const int hdrFormat = PIXELFORMAT_UNCOMPRESSED_R16G16B16A16;
    ps.scene = LoadRenderTextureEx(width, height, hdrFormat, true);
    ps.hdr = LoadRenderTextureEx(width, height, hdrFormat, false);
    ps.fog = LoadRenderTextureEx((width + 1) / 2, (height + 1) / 2, hdrFormat, false);
    ps.ldr = LoadRenderTextureEx(width, height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, false);
    // Bloom: halve until the short side is a handful of pixels.
    int bw = width / 2, bh = height / 2;
    while (ps.bloom.size() < 7 && bw >= 8 && bh >= 8) {
        ps.bloom.push_back(LoadRenderTextureEx(bw, bh, hdrFormat, false));
        bw /= 2;
        bh /= 2;
    }
    ps.width = width;
    ps.height = height;
}

void SetVec4(Shader sh, int loc, const Vector4& v) {
    if (loc >= 0) SetShaderValue(sh, loc, &v, SHADER_UNIFORM_VEC4);
}

void Pass(RenderTexture2D target, Shader shader, int blend = -1) {
    BeginTextureMode(target);
    DrawFullscreen(shader, blend);
    EndTextureMode();
}

// Air and fog along every view ray, at half size (fs_fog).
void RenderFog() {
    const LightingSettings& L = Lighting();
    const ViewInfo& v = ps.view;
    const AerialParams air = GetAerialParams();
    const bool atmosphere = AtmosphereActive();
    const Texture2D shadow = GetShadowMapTexture();
    const int steps = quality.volumetrics >= 2 ? 32 : (quality.volumetrics == 1 ? 20 : 10);
    const bool shafts = quality.volumetrics >= 1 && shadow.id != 0;

    SetVec4(ps.fogShader, ps.fgCamProjLoc, v.proj);
    SetVec4(ps.fogShader, ps.fgCamDepthLoc, v.depth);
    SetShaderValueMatrix(ps.fogShader, ps.fgCamInvViewLoc, v.invView);
    SetVec4(ps.fogShader, ps.fgAirRLoc, air.airR);
    SetVec4(ps.fogShader, ps.fgAirMLoc, air.airM);
    SetVec4(ps.fogShader, ps.fgAirExtLoc, air.airExt);
    SetVec4(ps.fogShader, ps.fgLightLoc, { air.light.x, air.light.y, air.light.z, shafts ? 1.0f : 0.0f });
    SetVec4(ps.fogShader, ps.fgLightDirLoc, { air.lightDir.x, air.lightDir.y, air.lightDir.z, GetShadowRange() });
    SetVec4(ps.fogShader, ps.fgMultiLoc, air.multi);

    // The Fog item (and the haze of bad weather). Under an atmosphere it is lit by the sun and the sky;
    // under a sky of one colour it is that colour, so distance fades into the background.
    const float haze = WeatherHaze();
    const float density = FogDensity() - haze;
    const float height = std::max(L.fogHeight, 0.0f);
    SetVec4(ps.fogShader, ps.fgFogLoc, { density, height > 0.0f ? 1.0f / height : 0.0f, 0.0f, 0.6f });
    // Under an atmosphere the weather's haze hugs the ground (the clouds above are drawn as clouds); under a
    // sky of one colour it is plain distance fog, as it always was.
    SetVec4(ps.fogShader, ps.fgHazeLoc, { atmosphere ? haze * 0.5f : haze, atmosphere ? 1.0f / 320.0f : 0.0f, 0.0f, 0.0f });
    if (atmosphere) {
        const Vector3 sun = Vector3Scale(SunRadiance(), PI);
        const Vector3 amb = Vector3Scale(Vector3Add(AmbientSky(), AmbientGround()), 0.5f);
        SetVec4(ps.fogShader, ps.fgFogSunLoc, { sun.x, sun.y, sun.z, 0.0f });
        SetVec4(ps.fogShader, ps.fgFogAmbLoc, { amb.x, amb.y, amb.z, 0.0f });
    } else {
        const Vector3 sky = DisplayColorToScene(FogColorNow());
        SetVec4(ps.fogShader, ps.fgFogSunLoc, { 0.0f, 0.0f, 0.0f, 0.0f });
        SetVec4(ps.fogShader, ps.fgFogAmbLoc, { sky.x, sky.y, sky.z, 1.0f });
    }
    // With an atmosphere the fog is drawn over the sky too (out to twice the far plane); a flat sky is
    // already the fog's colour.
    SetVec4(ps.fogShader, ps.fgParamsLoc, { (float)steps, (float)(ps.frame % 8) * 3.7f, atmosphere && (density > 0.0f || haze > 0.0f) ? 1.0f : 0.0f, v.depth.w * 2.0f });
    SetShaderValueMatrix(ps.fogShader, ps.fgLightVPLoc, GetLightViewProj());
    SetShaderValueTexture(ps.fogShader, ps.fgDepthLoc, ps.scene.depth);
    SetShaderValueTexture(ps.fogShader, ps.fgShadowLoc, shadow);
    SetShaderValueTexture(ps.fogShader, ps.fgExposureLoc, GetExposureTexture());
    SetVec4(ps.fogShader, ps.fgCloudShadowLoc, GetCloudShadowParams());
    SetShaderValueTexture(ps.fogShader, ps.fgCloudShadowTexLoc, GetCloudShadowTexture());
    Pass(ps.fog, ps.fogShader);
}

void RenderBloom(Texture2D src) {
    const int levels = (int)ps.bloom.size();
    for (int i = 0; i < levels; i++) {
        const Texture2D in = i == 0 ? src : ps.bloom[(size_t)i - 1].texture;
        SetVec4(ps.bloomDownShader, ps.bdParamsLoc, { 1.0f / (float)in.width, 1.0f / (float)in.height, i == 0 ? 1.0f : 0.0f, 600.0f });
        SetShaderValueTexture(ps.bloomDownShader, ps.bdSrcLoc, in);
        Pass(ps.bloom[(size_t)i], ps.bloomDownShader);
    }
    for (int i = levels - 2; i >= 0; i--) {
        const Texture2D in = ps.bloom[(size_t)i + 1].texture;
        SetVec4(ps.bloomUpShader, ps.buParamsLoc, { 1.0f / (float)in.width, 1.0f / (float)in.height, 0.62f, 0.0f });
        SetShaderValueTexture(ps.bloomUpShader, ps.buSrcLoc, in);
        Pass(ps.bloom[(size_t)i], ps.bloomUpShader, BLEND_ALPHA);
    }
}

// The exposure of this frame, into a 1x1 target every pass can read.
void UpdateExposure() {
    const LightingSettings& L = Lighting();
    const int prev = ps.exposureIndex, cur = 1 - prev;
    const bool adapt = L.autoExposure && AtmosphereActive();
    const float dt = std::clamp(GetFrameTime(), 0.0f, 0.25f);
    const Vector4 params = { std::pow(2.0f, std::clamp(L.exposure, -8.0f, 8.0f)), adapt ? 1.0f : 0.0f,
                             1.0f - std::exp(-dt * 1.6f), ps.exposureReset ? 1.0f : 0.0f };
    const Vector4 range = { 0.5f, 3.0f, 0.31f, 0.6f };
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

namespace {
std::string QualityFile() {
    const std::string dir = platform::ConfigDir();
    return dir.empty() ? std::string() : (std::filesystem::u8path(dir) / "graphics.cfg").string();
}
}

void LoadQualitySettings() {
    const std::string path = QualityFile();
    if (path.empty()) return;
    std::ifstream in(path);
    if (!in) return;
    RenderQuality q = quality;
    std::string key;
    while (in >> key) {
        float v = 0.0f;
        if (!(in >> v)) break;
        if (key == "tier") q.tier = (int)v;
        else if (key == "renderScale") q.renderScale = std::clamp(v, 0.5f, 1.0f);
        else if (key == "antiAliasing") q.antiAliasing = std::clamp((int)v, 0, 2);
        else if (key == "bloom") q.bloom = v != 0.0f;
        else if (key == "ambientOcclusion") q.ambientOcclusion = v != 0.0f;
        else if (key == "clouds") q.clouds = std::clamp((int)v, 0, 3);
        else if (key == "volumetrics") q.volumetrics = std::clamp((int)v, 0, 2);
        else if (key == "shadowCascades") q.shadowCascades = std::clamp((int)v, 1, 4);
    }
    quality = q;
}

void SaveQualitySettings() {
    const std::string path = QualityFile();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    const RenderQuality& q = quality;
    out << "tier " << q.tier << "\nrenderScale " << q.renderScale << "\nantiAliasing " << q.antiAliasing
        << "\nbloom " << (q.bloom ? 1 : 0) << "\nambientOcclusion " << (q.ambientOcclusion ? 1 : 0)
        << "\nclouds " << q.clouds << "\nvolumetrics " << q.volumetrics << "\nshadowCascades " << q.shadowCascades << "\n";
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

    ps.fogShader = LoadShaderProgram("post_fog");
    ps.fgCamProjLoc = GetShaderLocation(ps.fogShader, "camProj");
    ps.fgCamDepthLoc = GetShaderLocation(ps.fogShader, "camDepth");
    ps.fgCamInvViewLoc = GetShaderLocation(ps.fogShader, "camInvView");
    ps.fgAirRLoc = GetShaderLocation(ps.fogShader, "fgAirR");
    ps.fgAirMLoc = GetShaderLocation(ps.fogShader, "fgAirM");
    ps.fgAirExtLoc = GetShaderLocation(ps.fogShader, "fgAirExt");
    ps.fgLightLoc = GetShaderLocation(ps.fogShader, "fgLight");
    ps.fgLightDirLoc = GetShaderLocation(ps.fogShader, "fgLightDir");
    ps.fgMultiLoc = GetShaderLocation(ps.fogShader, "fgMulti");
    ps.fgFogLoc = GetShaderLocation(ps.fogShader, "fgFog");
    ps.fgHazeLoc = GetShaderLocation(ps.fogShader, "fgHaze");
    ps.fgFogSunLoc = GetShaderLocation(ps.fogShader, "fgFogSun");
    ps.fgFogAmbLoc = GetShaderLocation(ps.fogShader, "fgFogAmb");
    ps.fgParamsLoc = GetShaderLocation(ps.fogShader, "fgParams");
    ps.fgLightVPLoc = GetShaderLocation(ps.fogShader, "lightVP");
    ps.fgDepthLoc = GetShaderLocation(ps.fogShader, "depthTex");
    ps.fgShadowLoc = GetShaderLocation(ps.fogShader, "shadowMap");
    ps.fgExposureLoc = GetShaderLocation(ps.fogShader, "exposureTex");
    ps.fgCloudShadowLoc = GetShaderLocation(ps.fogShader, "fgCloudShadow");
    ps.fgCloudShadowTexLoc = GetShaderLocation(ps.fogShader, "cloudShadowTex");

    ps.compositeShader = LoadShaderProgram("post_composite");
    ps.cpCamDepthLoc = GetShaderLocation(ps.compositeShader, "camDepth");
    ps.cpTexelLoc = GetShaderLocation(ps.compositeShader, "cpTexel");
    ps.cpParamsLoc = GetShaderLocation(ps.compositeShader, "cpParams");
    ps.cpSceneLoc = GetShaderLocation(ps.compositeShader, "sceneTex");
    ps.cpDepthLoc = GetShaderLocation(ps.compositeShader, "cpDepthTex");
    ps.cpFogLoc = GetShaderLocation(ps.compositeShader, "fogTex");
    ps.cpCloudTexelLoc = GetShaderLocation(ps.compositeShader, "cpCloudTexel");
    ps.cpCloudLoc = GetShaderLocation(ps.compositeShader, "cloudTex");

    ps.bloomDownShader = LoadShaderProgram("post_bloom_down");
    ps.bdParamsLoc = GetShaderLocation(ps.bloomDownShader, "bdParams");
    ps.bdSrcLoc = GetShaderLocation(ps.bloomDownShader, "srcTex");
    ps.bloomUpShader = LoadShaderProgram("post_bloom_up");
    ps.buParamsLoc = GetShaderLocation(ps.bloomUpShader, "buParams");
    ps.buSrcLoc = GetShaderLocation(ps.bloomUpShader, "srcTex");

    ps.fxaaShader = LoadShaderProgram("post_fxaa");
    ps.fxTexelLoc = GetShaderLocation(ps.fxaaShader, "fxTexel");
    ps.fxSrcLoc = GetShaderLocation(ps.fxaaShader, "srcTex");

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
    ShutdownClouds();
    ShutdownAtmosphere();
    UnloadTargets();
    for (RenderTexture2D& e : ps.exposure) if (e.id > 0) UnloadRenderTexture(e);
    if (ps.luma.id > 0) UnloadRenderTexture(ps.luma);
    for (Shader* s : { &ps.exposureShader, &ps.lumaShader, &ps.tonemapShader, &ps.fogShader, &ps.compositeShader,
                       &ps.bloomDownShader, &ps.bloomUpShader, &ps.fxaaShader })
        UnloadShader(*s);
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
    ps.view = MakeViewInfo(camera, w, h);
    ps.frame++;

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
    Texture2D src = ps.scene.texture;

    // Clouds, then air and fog, over the scene.
    const Texture2D clouds = RenderClouds(ps.view, ps.scene.depth, ps.width, ps.height, ps.frame);
    const bool fog = !ps.view.ortho && (AtmosphereActive() || FogDensity() > 0.0f);
    if (fog) RenderFog();
    if (fog || clouds.id != 0) {
        SetVec4(ps.compositeShader, ps.cpCamDepthLoc, ps.view.depth);
        SetVec4(ps.compositeShader, ps.cpTexelLoc, { 1.0f / (float)ps.width, 1.0f / (float)ps.height,
                                                    1.0f / (float)ps.fog.texture.width, 1.0f / (float)ps.fog.texture.height });
        SetVec4(ps.compositeShader, ps.cpCloudTexelLoc, { clouds.id != 0 ? 1.0f / (float)clouds.width : 1.0f, clouds.id != 0 ? 1.0f / (float)clouds.height : 1.0f, 0.0f, 0.0f });
        SetVec4(ps.compositeShader, ps.cpParamsLoc, { fog ? 1.0f : 0.0f, clouds.id != 0 ? 1.0f : 0.0f, 0.0f, 0.0f });
        SetShaderValueTexture(ps.compositeShader, ps.cpSceneLoc, src);
        SetShaderValueTexture(ps.compositeShader, ps.cpDepthLoc, ps.scene.depth);
        SetShaderValueTexture(ps.compositeShader, ps.cpFogLoc, ps.fog.texture);
        SetShaderValueTexture(ps.compositeShader, ps.cpCloudLoc, clouds);
        Pass(ps.hdr, ps.compositeShader);
        src = ps.hdr.texture;
    }

    // Bloom, and from one of its small steps the picture's brightness for the next frame's exposure.
    const bool bloom = quality.bloom && L.bloom > 0.001f && !ps.bloom.empty();
    if (bloom) RenderBloom(src);
    SetShaderValueTexture(ps.lumaShader, ps.lumaSrcLoc, bloom ? ps.bloom[std::min<size_t>(2, ps.bloom.size() - 1)].texture : src);
    Pass(ps.luma, ps.lumaShader);

    // Exposure, tone curve, grading. Straight to the screen target, or by way of the anti-aliasing pass.
    SetVec4(ps.tonemapShader, ps.tmParamsLoc, { (float)std::clamp(L.toneCurve, 0, 3), bloom ? Clamp01(L.bloom) * 0.11f : 0.0f,
                                               Clamp01(L.vignette), Clamp01(L.filmGrain) });
    SetVec4(ps.tonemapShader, ps.tmGradeLoc, { std::clamp(L.contrast, 0.0f, 2.0f), std::clamp(L.saturation, 0.0f, 2.0f),
                                              std::clamp(L.temperature, -1.0f, 1.0f), (float)GetTime() });
    SetVec4(ps.tonemapShader, ps.tmTexelLoc, { 1.0f / (float)ps.width, 1.0f / (float)ps.height, 0.0f, 0.0f });
    SetShaderValueTexture(ps.tonemapShader, ps.tmSceneLoc, src);
    if (bloom) SetShaderValueTexture(ps.tonemapShader, ps.tmBloomLoc, ps.bloom[0].texture);
    SetShaderValueTexture(ps.tonemapShader, ps.tmExposureLoc, GetExposureTexture());
    if (quality.antiAliasing == 1) {
        Pass(ps.ldr, ps.tonemapShader);
        SetVec4(ps.fxaaShader, ps.fxTexelLoc, { 1.0f / (float)ps.width, 1.0f / (float)ps.height, 0.0f, 0.0f });
        SetShaderValueTexture(ps.fxaaShader, ps.fxSrcLoc, ps.ldr.texture);
        DrawFullscreen(ps.fxaaShader, -1);
    } else {
        DrawFullscreen(ps.tonemapShader, -1);
    }
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
