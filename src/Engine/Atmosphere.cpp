#include "../../include/Engine/Atmosphere.hpp"
#include "../../include/Engine/Graphics.hpp"
#include "../../include/Engine/PostFX.hpp"
#include "raymath.h"
#include "rlgl.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace gfx {

namespace {

// Sizes of the tables; shaders/fly_atmosphere.glsl has the same numbers.
constexpr int kTransW = 256, kTransH = 64, kMsSize = 32, kSkyW = 256, kSkyH = 144;
constexpr float kPi = 3.14159265358979f;
// The sun's light above the air. With it a white surface facing the noon sun comes to about 1,
// the same as under a sky of one colour, so a scene keeps its brightness when the sky is switched.
constexpr float kSunLight = 3.4f;
// Nights are far brighter than real ones (a real moon is a millionth of the sun), so that a moonlit
// scene can be seen: the moon gives a few percent of the sun's light, and the eye adapts the rest.
constexpr float kMoonLight = 0.17f;
// Discs are drawn larger than life, as films and games do: the real ones are a few pixels wide.
constexpr float kSunAngularRadius = 0.0075f;        // 0.43 degrees (the real sun: 0.267)
constexpr float kMoonAngularRadius = 0.0115f;       // 0.66 degrees (the real moon: 0.26)
constexpr float kNightGlow = 0.007f;                // the night sky is never quite black
const Vector3 kNightFill = { 0.013f, 0.018f, 0.050f };   // nor are the shadows of a night scene

// The air, in the units the shaders use (per kilometre, kilometres).
struct Air {
    Vector3 rayleigh; float rayleighH;
    Vector3 mie; float mieH;
    Vector3 mieAbs; float g;
    Vector3 ozone; float rg;
    Vector3 ground; float rt;
};

Air MakeAir(const LightingSettings& L) {
    Air a{};
    const float density = std::clamp(L.airDensity, 0.0f, 4.0f);
    a.rayleigh = Vector3{ std::max(L.airColor[0], 0.0f), std::max(L.airColor[1], 0.0f), std::max(L.airColor[2], 0.0f) } * (0.0331f * density);
    a.rayleighH = 8.0f;
    const float haze = std::clamp(L.haze, 0.0f, 12.0f);
    a.mie = Vector3{ std::max(L.hazeColor[0], 0.0f), std::max(L.hazeColor[1], 0.0f), std::max(L.hazeColor[2], 0.0f) } * (0.003996f * haze);
    a.mieH = 1.2f;
    a.mieAbs = Vector3{ 0.00440f, 0.00440f, 0.00440f } * haze;
    a.g = 0.8f;
    a.ozone = Vector3{ 0.000650f, 0.001881f, 0.000085f } * std::clamp(L.ozone, 0.0f, 3.0f);
    a.rg = 6360.0f;
    a.rt = 6460.0f;
    a.ground = SrgbToLinear(Vector3{ L.groundColor[0], L.groundColor[1], L.groundColor[2] });
    return a;
}

struct Tables {
    bool valid = false;
    Air air{};
    std::vector<Vector3> trans, ms;
    Texture2D transTex{}, msTex{};
};
Tables tb;

Vector3 Exp3(Vector3 v) { return { std::exp(v.x), std::exp(v.y), std::exp(v.z) }; }
Vector3 Div3(Vector3 a, Vector3 b) { return { a.x / std::max(b.x, 1e-7f), a.y / std::max(b.y, 1e-7f), a.z / std::max(b.z, 1e-7f) }; }

float RaySphere(Vector3 ro, Vector3 rd, float radius) {
    const float b = Vector3DotProduct(ro, rd);
    const float c = Vector3DotProduct(ro, ro) - radius * radius;
    if (c > 0.0f && b > 0.0f) return -1.0f;
    const float disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    if (c <= 0.0f) return -b + std::sqrt(disc);
    return -b - std::sqrt(disc);
}

bool HitsGround(float r, float mu) { return mu < 0.0f && r * r * (1.0f - mu * mu) < tb.air.rg * tb.air.rg; }

// x, y in 0..1 run from the first texel's centre to the last one's, as in the shaders.
Vector3 Bilinear(const std::vector<Vector3>& t, int w, int h, float x, float y) {
    const float fx = std::clamp(x, 0.0f, 1.0f) * (float)(w - 1), fy = std::clamp(y, 0.0f, 1.0f) * (float)(h - 1);
    const int x0 = (int)fx, y0 = (int)fy, x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float tx = fx - (float)x0, ty = fy - (float)y0;
    const Vector3 a = Vector3Lerp(t[(size_t)(y0 * w + x0)], t[(size_t)(y0 * w + x1)], tx);
    const Vector3 b = Vector3Lerp(t[(size_t)(y1 * w + x0)], t[(size_t)(y1 * w + x1)], tx);
    return Vector3Lerp(a, b, ty);
}

Vector3 Transmittance(float r, float mu) {
    const Air& a = tb.air;
    if (HitsGround(r, mu)) return { 0.0f, 0.0f, 0.0f };
    const float H = std::sqrt(a.rt * a.rt - a.rg * a.rg);
    const float rho = std::sqrt(std::max(r * r - a.rg * a.rg, 0.0f));
    const float d = std::max(-r * mu + std::sqrt(std::max(r * r * (mu * mu - 1.0f) + a.rt * a.rt, 0.0f)), 0.0f);
    const float dMin = a.rt - r, dMax = rho + H;
    return Bilinear(tb.trans, kTransW, kTransH, (d - dMin) / std::max(dMax - dMin, 1e-6f), rho / H);
}

Vector3 MultiScatter(float h, float cosSun) {
    return Bilinear(tb.ms, kMsSize, kMsSize, cosSun * 0.5f + 0.5f, h / (tb.air.rt - tb.air.rg));
}

void Medium(float h, Vector3& scatR, Vector3& scatM, Vector3& ext) {
    const Air& a = tb.air;
    const float dR = std::exp(-h / a.rayleighH), dM = std::exp(-h / a.mieH);
    const float dO = std::max(0.0f, 1.0f - std::fabs(h - 25.0f) / 15.0f);
    scatR = a.rayleigh * dR;
    scatM = a.mie * dM;
    ext = scatR + scatM + a.mieAbs * dM + a.ozone * dO;
}

float PhaseRayleigh(float c) { return 3.0f / (16.0f * kPi) * (1.0f + c * c); }
float PhaseMie(float c, float g) {
    const float g2 = g * g;
    return 3.0f / (8.0f * kPi) * ((1.0f - g2) * (1.0f + c * c)) / ((2.0f + g2) * std::pow(std::max(1.0f + g2 - 2.0f * g * c, 1e-4f), 1.5f));
}

// AtScatter() of fly_atmosphere.glsl.
Vector3 Scatter(Vector3 ro, Vector3 rd, float tMax, int steps, Vector3 toSun, Vector3 sunLight, Vector3 toMoon, Vector3 moonLight, Vector3* transOut) {
    const float g = tb.air.g;
    const float cs = Vector3DotProduct(rd, toSun), cm = Vector3DotProduct(rd, toMoon);
    const float phRs = PhaseRayleigh(cs), phMs = PhaseMie(cs, g), phRm = PhaseRayleigh(cm), phMm = PhaseMie(cm, g);
    Vector3 L{}, trans{ 1.0f, 1.0f, 1.0f };
    const float inv = 1.0f / (float)steps;
    for (int i = 0; i < steps; i++) {
        const float a0 = (float)i * inv, a1 = (float)(i + 1) * inv;
        const float t0 = a0 * a0 * tMax, t1 = a1 * a1 * tMax, dt = t1 - t0;
        const Vector3 p = ro + rd * (t0 + (t1 - t0) * 0.4f);
        const float r = Vector3Length(p), h = std::max(r - tb.air.rg, 0.0f);
        const Vector3 up = p * (1.0f / r);
        Vector3 scatR, scatM, ext;
        Medium(h, scatR, scatM, ext);
        const Vector3 scat = scatR + scatM;
        const float muS = Vector3DotProduct(up, toSun), muM = Vector3DotProduct(up, toMoon);
        const Vector3 S = sunLight * (Transmittance(r, muS) * (scatR * phRs + scatM * phMs) + MultiScatter(h, muS) * scat)
                        + moonLight * (Transmittance(r, muM) * (scatR * phRm + scatM * phMm) + MultiScatter(h, muM) * scat);
        const Vector3 stepT = Exp3(ext * -dt);
        L = L + trans * Div3(S - S * stepT, ext);
        trans = trans * stepT;
    }
    if (transOut) *transOut = trans;
    return L;
}

// How much light gets from a height, in a direction, out to space.
void BuildTransmittance() {
    const Air& a = tb.air;
    tb.trans.assign((size_t)kTransW * kTransH, Vector3{ 1.0f, 1.0f, 1.0f });
    const float H = std::sqrt(a.rt * a.rt - a.rg * a.rg);
    const int steps = 40;
    for (int j = 0; j < kTransH; j++) {
        const float rho = H * (float)j / (float)(kTransH - 1);
        const float r = std::sqrt(rho * rho + a.rg * a.rg);
        const float dMin = a.rt - r, dMax = rho + H;
        for (int i = 0; i < kTransW; i++) {
            const float d = dMin + (float)i / (float)(kTransW - 1) * (dMax - dMin);
            const float mu = d < 1e-5f ? 1.0f : std::clamp((H * H - rho * rho - d * d) / (2.0f * r * d), -1.0f, 1.0f);
            Vector3 depth{};
            const float dt = d / (float)steps;
            for (int s = 0; s < steps; s++) {
                const float t = ((float)s + 0.5f) * dt;
                const float rr = std::sqrt(std::max(r * r + t * t + 2.0f * r * t * mu, 0.0f));
                Vector3 scatR, scatM, ext;
                Medium(std::max(rr - a.rg, 0.0f), scatR, scatM, ext);
                depth = depth + ext * dt;
            }
            tb.trans[(size_t)(j * kTransW + i)] = Exp3(depth * -1.0f);
        }
    }
}

// The light that has been scattered more than once, per unit of sunlight: second-order scattering
// summed over a sphere of directions, then carried to every further order as a geometric series
// (Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique", 2020).
void BuildMultiScatter() {
    const Air& a = tb.air;
    tb.ms.assign((size_t)kMsSize * kMsSize, Vector3{});
    const int dirs = 8, steps = 20;
    const float groundAlbedo = 0.3f;
    for (int j = 0; j < kMsSize; j++) {
        const float h = (float)j / (float)(kMsSize - 1) * (a.rt - a.rg);
        const float r = a.rg + std::clamp(h, 0.001f, a.rt - a.rg - 0.001f);
        const Vector3 ro = { 0.0f, r, 0.0f };
        for (int i = 0; i < kMsSize; i++) {
            const float cosSun = (float)i / (float)(kMsSize - 1) * 2.0f - 1.0f;
            const Vector3 toSun = { std::sqrt(std::max(1.0f - cosSun * cosSun, 0.0f)), cosSun, 0.0f };
            Vector3 lum{}, fms{};
            for (int u = 0; u < dirs; u++) {
                for (int v = 0; v < dirs; v++) {
                    const float cosT = 1.0f - 2.0f * ((float)u + 0.5f) / (float)dirs;
                    const float sinT = std::sqrt(std::max(1.0f - cosT * cosT, 0.0f));
                    const float phi = 2.0f * kPi * ((float)v + 0.5f) / (float)dirs;
                    const Vector3 rd = { sinT * std::cos(phi), cosT, sinT * std::sin(phi) };
                    const float tGround = RaySphere(ro, rd, a.rg), tTop = RaySphere(ro, rd, a.rt);
                    const float tMax = tGround > 0.0f ? tGround : tTop;
                    if (tMax <= 0.0f) continue;
                    Vector3 L{}, F{}, trans{ 1.0f, 1.0f, 1.0f };
                    const float dt = tMax / (float)steps;
                    for (int s = 0; s < steps; s++) {
                        const Vector3 p = ro + rd * (((float)s + 0.3f) * dt);
                        const float pr = Vector3Length(p);
                        Vector3 scatR, scatM, ext;
                        Medium(std::max(pr - a.rg, 0.0f), scatR, scatM, ext);
                        const Vector3 scat = scatR + scatM;
                        const Vector3 S = Transmittance(pr, Vector3DotProduct(p, toSun) / pr) * scat * (1.0f / (4.0f * kPi));
                        const Vector3 stepT = Exp3(ext * -dt);
                        L = L + trans * Div3(S - S * stepT, ext);
                        F = F + trans * Div3(scat - scat * stepT, ext);
                        trans = trans * stepT;
                    }
                    if (tGround > 0.0f) {       // sunlight bounced off the ground
                        const Vector3 gp = Vector3Normalize(ro + rd * tGround);
                        const float nl = std::max(Vector3DotProduct(gp, toSun), 0.0f);
                        L = L + trans * Transmittance(a.rg, Vector3DotProduct(gp, toSun)) * (nl * groundAlbedo / kPi);
                    }
                    lum = lum + L;
                    fms = fms + F;
                }
            }
            const float n = 1.0f / (float)(dirs * dirs);
            lum = lum * n;
            fms = fms * n;
            tb.ms[(size_t)(j * kMsSize + i)] = { lum.x / std::max(1.0f - fms.x, 0.05f), lum.y / std::max(1.0f - fms.y, 0.05f), lum.z / std::max(1.0f - fms.z, 0.05f) };
        }
    }
}

uint16_t FloatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int exp = (int)((x >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        const uint32_t m = (mant | 0x800000u) >> (1 - exp);
        return (uint16_t)(sign | ((m + 0x1000u) >> 13));
    }
    if (exp >= 31) return (uint16_t)(sign | 0x7BFFu);      // clamp to the largest half
    return (uint16_t)(sign | ((uint32_t)exp << 10) | ((mant + 0x1000u) >> 13));
}

Texture2D UploadTable(const std::vector<Vector3>& t, int w, int h, Texture2D old) {
    std::vector<uint16_t> px((size_t)w * h * 4);
    for (size_t i = 0; i < t.size(); i++) {
        px[i * 4] = FloatToHalf(t[i].x); px[i * 4 + 1] = FloatToHalf(t[i].y); px[i * 4 + 2] = FloatToHalf(t[i].z); px[i * 4 + 3] = FloatToHalf(1.0f);
    }
    if (old.id != 0) UnloadTexture(old);
    Image img{};
    img.data = px.data();
    img.width = w;
    img.height = h;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R16G16B16A16;
    Texture2D tex = LoadTextureFromImage(img);
    SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(tex, TEXTURE_WRAP_CLAMP);
    return tex;
}

void EnsureTables() {
    const Air air = MakeAir(Lighting());
    if (tb.valid && std::memcmp(&air, &tb.air, sizeof(Air)) == 0) return;
    tb.air = air;
    BuildTransmittance();
    BuildMultiScatter();
    tb.transTex = UploadTable(tb.trans, kTransW, kTransH, tb.transTex);
    tb.msTex = UploadTable(tb.ms, kMsSize, kMsSize, tb.msTex);
    tb.valid = true;
}

// --- The light the sky puts on the scene -----------------------------------------------------------------
float CameraAltitudeKm(const Camera3D& camera) { return std::clamp(std::fabs(camera.position.y) / 1000.0f, 0.001f, 90.0f); }

struct LightInputs {
    Air air;
    Vector3 toSun, toMoon, toKey;
    Vector3 sunLight, moonLight;
    float altitudeKm, ambient, overcast, keyStrength, keyIsMoon, night;
};
LightInputs lightInputs{};
SkyLight skyLight{};
bool skyLightValid = false;
float frameAltitudeKm = 0.002f;

Vector3 SunLightNow() {
    const LightingSettings& L = Lighting();
    Vector3 c = SrgbToLinear(Vector3{ L.sunColor[0], L.sunColor[1], L.sunColor[2] }) * (kSunLight * std::max(L.sunIntensity, 0.0f));
    // A pinned sun stays in the sky, so the clock dims it instead.
    if (!L.sunFollowsTime) c = c * (0.02f + 0.98f * DayAmountNow());
    return c;
}
Vector3 MoonLightNow() {
    const LightingSettings& L = Lighting();
    const float s = std::sin(kPi * std::clamp(L.moonPhase, 0.0f, 1.0f));
    return Vector3{ 0.62f, 0.76f, 1.0f } * (kMoonLight * std::clamp(L.moonLight, 0.0f, 4.0f) * (0.12f + 0.88f * s * s));
}

Vector3 SkyRadianceFrom(float altitudeKm, Vector3 rd, const LightInputs& in, int steps) {
    const float r = tb.air.rg + altitudeKm;
    const Vector3 ro = { 0.0f, r, 0.0f };
    const float tGround = RaySphere(ro, rd, tb.air.rg), tTop = RaySphere(ro, rd, tb.air.rt);
    const float tMax = tGround > 0.0f ? tGround : tTop;
    if (tMax <= 0.0f) return {};
    return Scatter(ro, rd, tMax, steps, in.toSun, in.sunLight, in.toMoon, in.moonLight, nullptr);
}

void ComputeSkyLight(const LightInputs& in) {
    SkyLight s{};
    const float r = tb.air.rg + in.altitudeKm;
    const Vector3 glow = Vector3{ 0.35f, 0.45f, 0.75f } * (kNightGlow * in.night);

    // Direct light: what is left of the sun (or the moon) after the air, on a surface facing it.
    const Vector3 keyLight = in.keyIsMoon > 0.5f ? in.moonLight : in.sunLight;
    s.sunRadiance = keyLight * Transmittance(r, in.toKey.y) * (in.keyStrength / kPi);

    // The sky's light on a surface facing up: the sky's radiance over the hemisphere, weighted by the
    // cosine. With cosine-distributed directions that is just their mean.
    Vector3 sum{};
    const int rings = 5, spokes = 12;
    for (int i = 0; i < rings; i++) {
        const float cosT = std::sqrt(1.0f - ((float)i + 0.5f) / (float)rings);
        const float sinT = std::sqrt(std::max(1.0f - cosT * cosT, 0.0f));
        for (int j = 0; j < spokes; j++) {
            const float phi = 2.0f * kPi * ((float)j + 0.5f * (float)(i & 1)) / (float)spokes;
            sum = sum + SkyRadianceFrom(in.altitudeKm, { sinT * std::cos(phi), cosT, sinT * std::sin(phi) }, in, 14);
        }
    }
    s.ambientSky = sum * (1.0f / (float)(rings * spokes)) + glow + kNightFill * in.night;

    // The sky well above the horizon (for reflections) and at it (what distance fades into).
    Vector3 high{}, low{};
    for (int j = 0; j < spokes; j++) {
        const float phi = 2.0f * kPi * (float)j / (float)spokes;
        high = high + SkyRadianceFrom(in.altitudeKm, { 0.819f * std::cos(phi), 0.574f, 0.819f * std::sin(phi) }, in, 14);
        low = low + SkyRadianceFrom(in.altitudeKm, { 0.9986f * std::cos(phi), 0.0523f, 0.9986f * std::sin(phi) }, in, 20);
    }
    s.skyRadiance = high * (1.0f / (float)spokes) + glow;
    s.horizon = low * (1.0f / (float)spokes) + glow;

    // Cloud cover (until the clouds themselves shade the scene): less sun, a greyer, flatter sky.
    const float over = in.overcast;
    auto grey = [&](Vector3 c, float k) {
        const float l = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
        return Vector3Lerp(c, Vector3{ l, l, l } * k, 0.7f * over);
    };
    s.sunRadiance = s.sunRadiance * std::pow(1.0f - 0.78f * over, 1.3f);
    s.ambientSky = grey(s.ambientSky, 1.25f);
    s.skyRadiance = grey(s.skyRadiance, 0.85f);
    s.horizon = grey(s.horizon, 0.85f);

    s.ambientSky = s.ambientSky * in.ambient;
    // Light off the ground, onto what faces down.
    s.ambientGround = tb.air.ground * (s.sunRadiance * std::max(in.toKey.y, 0.0f) + s.ambientSky) * 0.8f;
    skyLight = s;
}

// --- GPU --------------------------------------------------------------------------------------------------
struct AtmosphereState {
    bool initialized = false;
    Shader flatShader{};
    int flatSkyLoc = -1, flatExposureLoc = -1;

    Shader viewShader{};
    AtmosphereLocs viewAt{};
    int svSunLoc = -1, svSunLightLoc = -1, svMoonLoc = -1, svMoonLightLoc = -1;
    RenderTexture2D skyView{};

    Shader atmoShader{};
    AtmosphereLocs atmoAt{};
    int camProjLoc = -1, camRightLoc = -1, camUpLoc = -1, camFwdLoc = -1;
    int skSunLoc = -1, skSunDiscLoc = -1, skMoonLoc = -1, skMoonDiscLoc = -1, skGroundLoc = -1, skParamsLoc = -1, skStarsLoc = -1;
};
AtmosphereState as;

void SetVec4(Shader sh, int loc, Vector3 v, float w) {
    if (loc < 0) return;
    const Vector4 x = { v.x, v.y, v.z, w };
    SetShaderValue(sh, loc, &x, SHADER_UNIFORM_VEC4);
}

} // namespace

void InitAtmosphere() {
    if (as.initialized) return;
    as.flatShader = LoadShaderProgram("sky_flat");
    as.flatSkyLoc = GetShaderLocation(as.flatShader, "skyHdr");
    as.flatExposureLoc = GetShaderLocation(as.flatShader, "exposureTex");

    as.viewShader = LoadShaderProgram("sky_view");
    as.viewAt = FindAtmosphereLocs(as.viewShader);
    as.svSunLoc = GetShaderLocation(as.viewShader, "svSun");
    as.svSunLightLoc = GetShaderLocation(as.viewShader, "svSunLight");
    as.svMoonLoc = GetShaderLocation(as.viewShader, "svMoon");
    as.svMoonLightLoc = GetShaderLocation(as.viewShader, "svMoonLight");
    as.skyView = LoadRenderTextureEx(kSkyW, kSkyH, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16, false);
    SetTextureWrap(as.skyView.texture, TEXTURE_WRAP_REPEAT);     // round the compass; the shader keeps v off the edges

    as.atmoShader = LoadShaderProgram("sky_atmo");
    as.atmoAt = FindAtmosphereLocs(as.atmoShader);
    as.camProjLoc = GetShaderLocation(as.atmoShader, "camProj");
    as.camRightLoc = GetShaderLocation(as.atmoShader, "camRight");
    as.camUpLoc = GetShaderLocation(as.atmoShader, "camUp");
    as.camFwdLoc = GetShaderLocation(as.atmoShader, "camFwd");
    as.skSunLoc = GetShaderLocation(as.atmoShader, "skSun");
    as.skSunDiscLoc = GetShaderLocation(as.atmoShader, "skSunDisc");
    as.skMoonLoc = GetShaderLocation(as.atmoShader, "skMoon");
    as.skMoonDiscLoc = GetShaderLocation(as.atmoShader, "skMoonDisc");
    as.skGroundLoc = GetShaderLocation(as.atmoShader, "skGround");
    as.skParamsLoc = GetShaderLocation(as.atmoShader, "skParams");
    as.skStarsLoc = GetShaderLocation(as.atmoShader, "skStars");
    as.initialized = true;
}

void ShutdownAtmosphere() {
    if (!as.initialized) return;
    UnloadShader(as.flatShader);
    UnloadShader(as.viewShader);
    UnloadShader(as.atmoShader);
    if (as.skyView.id > 0) UnloadRenderTexture(as.skyView);
    if (tb.transTex.id != 0) UnloadTexture(tb.transTex);
    if (tb.msTex.id != 0) UnloadTexture(tb.msTex);
    as = AtmosphereState{};
    tb = Tables{};
    skyLightValid = false;
}

bool AtmosphereActive() { return Lighting().hasSky && Lighting().skyMode == 1; }

Vector3 AtmosphereSunLight() { return SunLightNow(); }
Vector3 AtmosphereMoonLight() { return MoonLightNow(); }

AtmosphereLocs FindAtmosphereLocs(Shader shader) {
    AtmosphereLocs l;
    l.rayleigh = GetShaderLocation(shader, "atRayleigh");
    l.mie = GetShaderLocation(shader, "atMie");
    l.mieAbs = GetShaderLocation(shader, "atMieAbs");
    l.ozone = GetShaderLocation(shader, "atOzone");
    l.planet = GetShaderLocation(shader, "atPlanet");
    l.transLut = GetShaderLocation(shader, "transLut");
    l.msLut = GetShaderLocation(shader, "msLut");
    l.skyViewLut = GetShaderLocation(shader, "skyViewLut");
    return l;
}

void SetAtmosphereUniforms(Shader shader, const AtmosphereLocs& l) {
    const Air& a = tb.air;
    SetVec4(shader, l.rayleigh, a.rayleigh, a.rayleighH);
    SetVec4(shader, l.mie, a.mie, a.mieH);
    SetVec4(shader, l.mieAbs, a.mieAbs, a.g);
    SetVec4(shader, l.ozone, a.ozone, 0.0f);
    SetVec4(shader, l.planet, { a.rg, a.rt, frameAltitudeKm }, 0.0f);
    if (l.transLut >= 0) SetShaderValueTexture(shader, l.transLut, tb.transTex);
    if (l.msLut >= 0) SetShaderValueTexture(shader, l.msLut, tb.msTex);
    if (l.skyViewLut >= 0) SetShaderValueTexture(shader, l.skyViewLut, as.skyView.texture);
}

const SkyLight& AtmosphereLight() {
    EnsureTables();
    LightInputs in{};
    in.air = tb.air;
    in.toSun = SunPosition();
    in.toMoon = MoonPosition();
    in.toKey = Vector3Negate(SunDirection());
    in.sunLight = SunLightNow();
    in.moonLight = MoonLightNow();
    in.altitudeKm = std::round(frameAltitudeKm * 20.0f) / 20.0f + 0.001f;      // 50 m steps
    in.ambient = std::max(Lighting().ambient, 0.0f);
    in.overcast = OvercastNow();
    in.keyStrength = KeyLightStrength();
    in.keyIsMoon = KeyLightIsMoon() ? 1.0f : 0.0f;
    in.night = 1.0f - DayAmountNow();
    if (!skyLightValid || std::memcmp(&in, &lightInputs, sizeof(LightInputs)) != 0) {
        lightInputs = in;
        ComputeSkyLight(in);
        skyLightValid = true;
    }
    return skyLight;
}

void UpdateAtmosphere(const Camera3D& camera) {
    if (!AtmosphereActive()) return;
    if (!as.initialized) InitAtmosphere();
    EnsureTables();
    frameAltitudeKm = CameraAltitudeKm(camera);

    SetAtmosphereUniforms(as.viewShader, as.viewAt);
    SetVec4(as.viewShader, as.svSunLoc, SunPosition(), 0.0f);
    SetVec4(as.viewShader, as.svSunLightLoc, SunLightNow(), 0.0f);
    SetVec4(as.viewShader, as.svMoonLoc, MoonPosition(), 0.0f);
    SetVec4(as.viewShader, as.svMoonLightLoc, MoonLightNow(), 0.0f);
    BeginTextureMode(as.skyView);
    DrawFullscreen(as.viewShader, -1);
    EndTextureMode();
}

ViewInfo MakeViewInfo(const Camera3D& camera, int width, int height) {
    ViewInfo v;
    const double aspect = (double)width / (double)std::max(height, 1);
    v.ortho = camera.projection == CAMERA_ORTHOGRAPHIC;
    Matrix p;
    if (v.ortho) {
        const double top = camera.fovy / 2.0, right = top * aspect;
        p = MatrixOrtho(-right, right, -top, top, rlGetCullDistanceNear(), rlGetCullDistanceFar());
        v.proj = { 1.0f / p.m0, 1.0f / p.m5, p.m12, p.m13 };
        v.pixelAngle = 0.0f;
    } else {
        p = MatrixPerspective(camera.fovy * DEG2RAD, aspect, rlGetCullDistanceNear(), rlGetCullDistanceFar());
        v.proj = { 1.0f / p.m0, 1.0f / p.m5, p.m8, p.m9 };
        v.pixelAngle = camera.fovy * DEG2RAD / (float)std::max(height, 1);
    }
    v.depth = { p.m10, p.m14, v.ortho ? 1.0f : 0.0f, (float)rlGetCullDistanceFar() };
    // The same axes MatrixLookAt() builds.
    v.fwd = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    v.right = Vector3Normalize(Vector3CrossProduct(v.fwd, camera.up));
    v.up = Vector3CrossProduct(v.right, v.fwd);
    v.pos = camera.position;
    v.invView = { v.right.x, v.up.x, -v.fwd.x, v.pos.x,
                  v.right.y, v.up.y, -v.fwd.y, v.pos.y,
                  v.right.z, v.up.z, -v.fwd.z, v.pos.z,
                  0.0f, 0.0f, 0.0f, 1.0f };
    return v;
}

AerialParams GetAerialParams() {
    AerialParams p{};
    const Vector3 toKey = Vector3Negate(SunDirection());
    p.lightDir = { toKey.x, toKey.y, toKey.z, 0.0f };
    if (!AtmosphereActive()) return p;
    EnsureTables();
    const Air& a = tb.air;
    const float r = a.rg + frameAltitudeKm;
    // Per kilometre -> per metre.
    p.airR = { a.rayleigh.x * 1e-3f, a.rayleigh.y * 1e-3f, a.rayleigh.z * 1e-3f, 1.0f / (a.rayleighH * 1000.0f) };
    p.airM = { a.mie.x * 1e-3f, a.mie.y * 1e-3f, a.mie.z * 1e-3f, 1.0f / (a.mieH * 1000.0f) };
    p.airExt = { a.mieAbs.x * 1e-3f, a.mieAbs.y * 1e-3f, a.mieAbs.z * 1e-3f, a.g };
    const Vector3 key = (KeyLightIsMoon() ? MoonLightNow() : SunLightNow()) * Transmittance(r, toKey.y)
                      * (KeyLightStrength() * std::pow(1.0f - 0.78f * OvercastNow(), 1.3f));
    p.light = { key.x, key.y, key.z, 0.0f };
    const Vector3 multi = SunLightNow() * MultiScatter(frameAltitudeKm, SunPosition().y) + MoonLightNow() * MultiScatter(frameAltitudeKm, MoonPosition().y);
    p.multi = { multi.x, multi.y, multi.z, 0.0f };
    return p;
}

void DrawSky(const Camera3D& camera) {
    if (!as.initialized) InitAtmosphere();
    if (!AtmosphereActive()) {
        // One colour: handed over as the light that the exposure and the tone curve
        // turn back into exactly that colour.
        const Vector3 hdr = DisplayColorToScene(CurrentSky());
        const Vector4 sky = { hdr.x, hdr.y, hdr.z, 1.0f };
        SetShaderValue(as.flatShader, as.flatSkyLoc, &sky, SHADER_UNIFORM_VEC4);
        SetShaderValueTexture(as.flatShader, as.flatExposureLoc, GetExposureTexture());
        DrawFullscreen(as.flatShader, -1);
        return;
    }

    const LightingSettings& L = Lighting();
    const Vector2 size = GetRenderTargetSize();
    const ViewInfo v = MakeViewInfo(camera, (int)size.x, (int)size.y);
    const SkyLight& light = AtmosphereLight();
    const float night = 1.0f - DayAmountNow();

    SetAtmosphereUniforms(as.atmoShader, as.atmoAt);
    SetShaderValue(as.atmoShader, as.camProjLoc, &v.proj, SHADER_UNIFORM_VEC4);
    SetVec4(as.atmoShader, as.camRightLoc, v.right, v.ortho ? 1.0f : 0.0f);
    SetVec4(as.atmoShader, as.camUpLoc, v.up, 0.0f);
    SetVec4(as.atmoShader, as.camFwdLoc, v.fwd, 0.0f);

    // The sun's disc: its light spread over the disc's (tiny) solid angle, capped so the float
    // target and the bloom are not asked to carry a value in the tens of thousands.
    const float sunRadius = kSunAngularRadius * std::clamp(L.sunSize, 0.2f, 12.0f);
    const float discGain = std::min(1.0f / (kPi * sunRadius * sunRadius), 420.0f);
    SetVec4(as.atmoShader, as.skSunLoc, SunPosition(), sunRadius);
    SetVec4(as.atmoShader, as.skSunDiscLoc, SunLightNow() * discGain, 0.0f);

    // The moon's disc is drawn far dimmer than its light would make it (a real one is burnt out
    // white in any exposure that shows the landscape), so its face stays readable.
    const float moonRadius = kMoonAngularRadius * std::clamp(L.moonSize, 0.2f, 12.0f);
    const Vector3 moonDisc = Vector3{ 0.95f, 0.93f, 0.86f } * ((0.16f + 0.42f * night) * std::clamp(L.moonLight, 0.0f, 4.0f));
    SetVec4(as.atmoShader, as.skMoonLoc, MoonPosition(), moonRadius);
    SetVec4(as.atmoShader, as.skMoonDiscLoc, moonDisc, std::clamp(L.moonPhase, 0.0f, 1.0f));

    // The land below the horizon, lit like the scene's own ground.
    const Vector3 toKey = Vector3Negate(SunDirection());
    SetVec4(as.atmoShader, as.skGroundLoc, tb.air.ground * (light.sunRadiance * std::max(toKey.y, 0.0f) + light.ambientSky), 0.0f);

    const float turn = Lighting().timeOfDay / 24.0f * 2.0f * kPi;
    const Vector4 params = { std::clamp(L.stars, 0.0f, 4.0f) * 0.55f * night * night, (float)GetTime(), OvercastNow(), v.pixelAngle };
    const Vector4 stars = { std::cos(turn), std::sin(turn), kNightGlow * night, 0.0f };
    SetShaderValue(as.atmoShader, as.skParamsLoc, &params, SHADER_UNIFORM_VEC4);
    SetShaderValue(as.atmoShader, as.skStarsLoc, &stars, SHADER_UNIFORM_VEC4);
    DrawFullscreen(as.atmoShader, -1);
}

} // namespace gfx
