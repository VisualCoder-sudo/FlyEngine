#include "../../include/Engine/Lighting.hpp"
#include "../../include/Engine/Atmosphere.hpp"
#include "../../include/Engine/Clouds.hpp"
#include "../../include/Engine/PostFX.hpp"
#include "raymath.h"
#include <algorithm>
#include <cmath>

namespace {

const Vector3 kAmbient = { 0.35f, 0.35f, 0.35f };
gfx::LightingSettings lightingSettings;
Color engineClear = { 245, 245, 245, 255 };
float Smooth01(float a, float b, float x) { const float t = fminf(fmaxf((x - a) / (b - a), 0.0f), 1.0f); return t * t * (3.0f - 2.0f * t); }
float HourNow() { return fmodf(fmodf(lightingSettings.timeOfDay, 24.0f) + 24.0f, 24.0f); }
// The sun's height above the horizon along its path (degrees): up at 6:00, the Sun item's height at noon, down at
// 18:00, and below the horizon (negative) at night. A pinned sun stays at the Sun item's height.
float RawSunElevation() {
    if (!lightingSettings.sunFollowsTime) return fminf(fmaxf(lightingSettings.sunElevation, 2.0f), 90.0f);
    const float peak = fminf(fmaxf(lightingSettings.sunElevation, 8.0f), 90.0f);
    return peak * sinf((HourNow() - 6.0f) / 12.0f * PI);
}
// The compass direction the sun shines from (it moves 15 degrees an hour when it follows the time).
float SunAzimuthNow() {
    if (!lightingSettings.sunFollowsTime) return lightingSettings.sunAzimuth;
    return lightingSettings.sunAzimuth + (HourNow() - 12.0f) * 15.0f;
}
// The moon is opposite the sun: it rises as the sun sets, is highest at midnight and sets at sunrise.
float MoonElevation() { return 52.0f * sinf((HourNow() - 18.0f) / 12.0f * PI); }
float MoonAzimuth() { return SunAzimuthNow() + 180.0f; }
// 1 in daylight, 0 at night. With a sun that follows the time it is the sun's height that decides (dusk lasts until
// the sun is well below the horizon); a pinned sun eases through dawn (5:00-7:30) and dusk (16:30-19:00) by the clock.
float DayAmount() {
    if (lightingSettings.sunFollowsTime) return Smooth01(-9.0f, 5.0f, RawSunElevation());
    const float h = HourNow();
    return Smooth01(5.0f, 7.5f, h) * (1.0f - Smooth01(16.5f, 19.0f, h));
}
Vector3 DirFromAngles(float azimuthDeg, float elevationDeg) {
    const float az = azimuthDeg * DEG2RAD, el = elevationDeg * DEG2RAD;
    return Vector3Normalize({ -cosf(el) * sinf(az), -sinf(el), -cosf(el) * cosf(az) });
}
// The light that shades the scene and casts its shadows: the sun by day, the moon at night. Between the two (the
// sun a few degrees under the horizon) neither shines, so the direction can change hands unseen.
struct KeyLight {
    Vector3 dir;        // unit vector the light travels along
    float strength;     // 0..1: how much of the sun's (or the moon's) full light arrives
    bool moon;
};
KeyLight Key() {
    KeyLight k{};
    if (!lightingSettings.sunFollowsTime) {
        k.dir = DirFromAngles(lightingSettings.sunAzimuth, RawSunElevation());
        k.strength = 1.0f;      // the clock dims a pinned sun (see FlatSunRadiance)
        return k;
    }
    const float sunEl = RawSunElevation();
    float azd, eld;
    if (sunEl > -4.0f) {
        azd = SunAzimuthNow();
        eld = fmaxf(sunEl, 2.0f);                   // a sun on the horizon still lights from just above it
        k.strength = Smooth01(-2.5f, 6.0f, sunEl);
    } else {
        const float moonEl = MoonElevation();
        azd = MoonAzimuth();
        eld = fmaxf(moonEl, 6.0f);
        k.strength = Smooth01(-4.0f, -9.0f, sunEl) * Smooth01(0.0f, 8.0f, moonEl);
        k.moon = true;
    }
    azd = roundf(azd * 2.0f) * 0.5f;                // half-degree steps: shadows are not redrawn for tiny moves
    eld = roundf(eld * 2.0f) * 0.5f;
    k.dir = DirFromAngles(azd, fminf(eld, 90.0f));
    return k;
}
Vector3 SunDir() { return Key().dir; }
// Cloud cover and rain, 0..1 (rain brings its own cloud).
float OvercastAmount() {
    if (!lightingSettings.hasWeather) return 0.0f;
    return fminf(fmaxf(fmaxf(lightingSettings.overcast, lightingSettings.rain * 0.9f), 0.0f), 1.0f);
}
float RainAmount() { return lightingSettings.hasWeather ? fminf(fmaxf(lightingSettings.rain, 0.0f), 1.0f) : 0.0f; }
float WetAmount() { return lightingSettings.hasWeather ? fminf(fmaxf(fmaxf(lightingSettings.wetGround, lightingSettings.rain), 0.0f), 1.0f) : 0.0f; }

}

namespace gfx {

void SetAmbientIntensity(float intensity) {
    lightingSettings.ambient = fmaxf(0.0f, fminf(intensity, 2.0f));
}

float GetAmbientIntensity() {
    return lightingSettings.ambient;
}

namespace {
// Sun direction scaled by its intensity (the shaders use its length as the diffuse strength) and the
// ambient colour, both blended towards moonlight at night.
// Clouds dim and soften the sun (the shadows fade with it) and flatten the light.
// The scene is lit in linear light. These are the values for a sky of one colour (no atmosphere): a white surface
// facing the noon sun comes to 1 (0.9 from the sun, 0.1 from the sky), and the night is lit by a dim blue moon.
const Vector3 kMoonRadiance = { 0.034f, 0.040f, 0.065f };
const Vector3 kNightAmbient = { 0.018f, 0.026f, 0.076f };
Vector3 SunTint();
Vector3 FlatSunRadiance() {
    const float over = OvercastAmount();
    const Vector3 sun = Vector3Scale(SrgbToLinear(SunTint()), 0.9f * powf(1.0f - 0.78f * over, 1.3f) * (1.0f - 0.25f * RainAmount()));
    const Vector3 moon = Vector3Scale(kMoonRadiance, 1.0f - 0.6f * over);
    const KeyLight k = Key();
    Vector3 r;
    if (!lightingSettings.sunFollowsTime) r = Vector3Lerp(moon, sun, DayAmount());   // a pinned sun turns into moonlight by the clock
    else r = Vector3Scale(k.moon ? moon : sun, k.strength);
    return Vector3Scale(r, lightingSettings.sunIntensity);
}
Vector3 FlatAmbientSky() {
    const float a = fmaxf(lightingSettings.ambient, 0.0f);
    // Cloud spreads the light about (more of it reaches the shade); rain cloud is thick enough to dim it again.
    const float day = powf(kAmbient.x * a * (1.0f + 0.30f * OvercastAmount()), 2.2f) * (1.0f - 0.25f * RainAmount());
    return Vector3Lerp(Vector3Scale(kNightAmbient, powf(fminf(a, 1.5f), 2.2f)), Vector3{ day, day, day }, DayAmount());
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
Vector3 SunPosition() { return Vector3Negate(DirFromAngles(SunAzimuthNow(), RawSunElevation())); }
Vector3 MoonPosition() { return Vector3Negate(DirFromAngles(MoonAzimuth(), MoonElevation())); }
bool KeyLightIsMoon() { return Key().moon; }
float KeyLightStrength() { return Key().strength; }
float DayAmountNow() { return DayAmount(); }
// With an atmosphere the light comes out of it (src/Engine/Atmosphere.cpp); under a sky of one colour it is the
// plain sun and ambient above.
Vector3 SunRadiance() { return AtmosphereActive() ? AtmosphereLight().sunRadiance : FlatSunRadiance(); }
Vector3 SunRadianceShaded() { return Vector3Scale(SunRadiance(), CloudSunlightNow()); }
Vector3 AmbientSky() { return AtmosphereActive() ? AtmosphereLight().ambientSky : FlatAmbientSky(); }
Vector3 AmbientGround() { return AtmosphereActive() ? AtmosphereLight().ambientGround : Vector3Scale(FlatAmbientSky(), 0.72f); }
Vector3 SkyRadiance() { return AtmosphereActive() ? AtmosphereLight().skyRadiance : SrgbToLinear(CurrentSky()); }
Vector3 SunLightScale() { return Vector3Scale(SunRadiance(), 1.0f / 0.9f); }
Vector3 AmbientScale() {
    const float noon = powf(kAmbient.x, 2.2f);
    return Vector3Scale(AmbientSky(), 1.0f / noon);
}
// Cloud and rain add a little haze to whatever fog the Fog item sets.
float WeatherHaze() { return 0.0014f * OvercastAmount() + 0.0045f * RainAmount(); }
float FogDensity() { return (lightingSettings.hasFog ? lightingSettings.fogDensity : 0.0f) + WeatherHaze(); }
void SetEngineClearColor(Color c) { engineClear = c; }
Color CurrentSky() {
    if (AtmosphereActive()) {
        // About the colour the horizon has on screen (for what still wants one colour: rain streaks, the clear colour).
        const Vector3 c = LinearToSrgb(TonemapApply(std::clamp(lightingSettings.toneCurve, 0, 3), AtmosphereLight().horizon));
        return Color{ (unsigned char)lroundf(c.x * 255.0f), (unsigned char)lroundf(c.y * 255.0f), (unsigned char)lroundf(c.z * 255.0f), 255 };
    }
    auto byte01 = [](float v) { return (unsigned char)lroundf((std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f) * 255.0f); };
    const Color day = lightingSettings.hasSky
        ? Color{ byte01(lightingSettings.skyColor[0]), byte01(lightingSettings.skyColor[1]), byte01(lightingSettings.skyColor[2]), 255 }
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
LightingSettings NewSceneLighting() {
    LightingSettings L;
    L.hasSky = true; L.skyMode = 1;
    L.hasClouds = true; L.cloudCoverage = 0.35f;
    L.hasWeather = true;                // clear: all zero, but the item is there to turn up
    L.hasPicture = true;
    L.timeOfDay = 15.0f;                // afternoon light shows the sky and shadows well
    return L;
}
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
    if (AtmosphereActive()) return AtmosphereLight().skyRadiance;
    Vector3 base = { 0.65f, 0.78f, 0.88f };
    if (lightingSettings.hasSky) base = Vector3Lerp(base, Vector3{ lightingSettings.skyColor[0], lightingSettings.skyColor[1], lightingSettings.skyColor[2] }, 0.6f);
    return SrgbToLinear(SkyColor(Color{ (unsigned char)lroundf(base.x * 255.0f), (unsigned char)lroundf(base.y * 255.0f), (unsigned char)lroundf(base.z * 255.0f), 255 }));
}

} // namespace gfx
