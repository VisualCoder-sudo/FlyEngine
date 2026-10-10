// The atmosphere sky, the sun and the moon, and the picture passes (exposure, tone curve, bloom). Renders the same
// city through a day (sky_<name>.png) and checks the pictures the way a person would: blue overhead at noon, red
// towards a setting sun, dark with stars at night. Opens a window, so it is not registered with ctest:
// build/tests/sky_test
#include "raylib.h"
#include "raymath.h"
#include "Engine.hpp"
#include "Engine/Atmosphere.hpp"
#include "Engine/Clouds.hpp"
#include "Engine/Graphics.hpp"
#include "Engine/PostFX.hpp"
#include "Engine/Backend/ScenePersistence.hpp"
#include "../include/CityGen/City.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

struct Shot {
    float lum = 0;                      // mean of the whole picture, 0..255
    Vector3 top{}, mid{}, low{};        // mean colour of the top eighth, of a band above the middle, and of a band just above the horizon of a level view
    float bright = 0;                   // share of pixels that are nearly white
    std::string path;
};

static Shot Shoot(Engine& engine, const char* name, int frames = 10) {
    for (int f = 0; f < frames; f++) engine.StepFrame(1.0f / 60.0f);
    Shot s;
    s.path = std::string("sky_") + name + ".png";
    TakeScreenshot(s.path.c_str());
    Image img = LoadImage(s.path.c_str());
    double sum = 0; long n = 0, nTop = 0, nMid = 0, nLow = 0, nBright = 0;
    for (int y = 0; y < img.height; y += 2)
        for (int x = 0; x < img.width; x += 2) {
            const Color c = GetImageColor(img, x, y);
            sum += (c.r + c.g + c.b) / 3.0; n++;
            if (c.r > 245 && c.g > 240 && c.b > 225) nBright++;
            if (y < img.height / 8) { s.top.x += c.r; s.top.y += c.g; s.top.z += c.b; nTop++; }
            if (y > img.height * 3 / 10 && y < img.height * 4 / 10) { s.mid.x += c.r; s.mid.y += c.g; s.mid.z += c.b; nMid++; }
            if (y > img.height * 50 / 100 && y < img.height * 58 / 100 && x > img.width / 4 && x < img.width * 3 / 4) { s.low.x += c.r; s.low.y += c.g; s.low.z += c.b; nLow++; }
        }
    s.lum = (float)(sum / (double)n);
    s.top = Vector3Scale(s.top, 1.0f / (float)nTop);
    s.mid = Vector3Scale(s.mid, 1.0f / (float)nMid);
    s.low = Vector3Scale(s.low, 1.0f / (float)nLow);
    s.bright = (float)nBright / (float)n;
    UnloadImage(img);
    std::printf("%-22s lum %5.1f  top (%3.0f,%3.0f,%3.0f)  mid (%3.0f,%3.0f,%3.0f)  bright %.4f\n", name, s.lum, s.top.x, s.top.y, s.top.z, s.mid.x, s.mid.y, s.mid.z, s.bright);
    return s;
}

// Looks from the city towards a point `distance` away in the compass direction the sun (or anything) is in.
static void LookTowards(Camera3D& cam, Vector3 dir, float pitchUp) {
    Vector3 flat = Vector3Normalize({ dir.x, 0.0f, dir.z });
    cam.position = { -30.0f, 34.0f, 60.0f };
    cam.target = Vector3Add(cam.position, Vector3{ flat.x * 100.0f, pitchUp * 100.0f, flat.z * 100.0f });
}

int main() {
    Engine engine(1280, 720, "sky_test", 60);
    engine.SetPlayerBuild(true);
    auto owner = std::make_unique<City>();
    City& c = *owner;
    engine.AddEntity(std::move(owner));
    c.GetParams().gridX = 8; c.GetParams().gridZ = 8;
    c.GetParams().cars = 0;
    c.GenerateGrid({ 0.0f, 0.0f });
    city::SetTrafficRunning(false);
    gfx::ResetLighting();
    Camera3D& cam = engine.GetCamera();
    cam.position = { -60.0f, 14.0f, 110.0f };
    cam.target = { 10.0f, 16.0f, -40.0f };
    cam.up = { 0.0f, 1.0f, 0.0f };
    cam.fovy = 60.0f;

    // ---- Tone curves: each is undone by its inverse (this is what keeps a flat sky the colour it was given). The
    // filmic curves cannot produce the most saturated colours at all; those come back as the nearest they can.
    for (int curve = 0; curve <= 3; curve++) {
        for (const Vector3 col : { Vector3{ 0.5f, 0.5f, 0.5f }, Vector3{ 0.96f, 0.96f, 0.96f }, Vector3{ 0.2f, 0.3f, 0.6f }, Vector3{ 0.6f, 0.4f, 0.25f }, Vector3{ 0.01f, 0.02f, 0.03f } }) {
            const Vector3 back = gfx::TonemapApply(curve, gfx::TonemapInverse(curve, col));
            const float err = Vector3Distance(back, col);
            if (err > 0.004f) std::printf("curve %d: (%.3f,%.3f,%.3f) came back as (%.3f,%.3f,%.3f)\n", curve, col.x, col.y, col.z, back.x, back.y, back.z);
            CHECK(err < 0.004f);
        }
        const Vector3 vivid = { 0.06f, 0.26f, 0.89f };
        const Vector3 back = gfx::TonemapApply(curve, gfx::TonemapInverse(curve, vivid));
        CHECK(Vector3Distance(back, vivid) < (curve <= 1 ? 0.004f : 0.2f));
    }

    // ---- A sky of one colour reaches the screen as that colour (to within what the tone curve can show).
    gfx::Lighting().hasSky = true; gfx::Lighting().skyColor[0] = 0.25f; gfx::Lighting().skyColor[1] = 0.55f; gfx::Lighting().skyColor[2] = 0.95f;
    gfx::SetTimeOfDay(12.0f);
    for (int curve = 0; curve <= 3; curve++) {
        gfx::Lighting().toneCurve = curve;
        const Shot s = Shoot(engine, curve == 0 ? "flat_none" : curve == 1 ? "flat_neutral" : curve == 2 ? "flat_aces" : "flat_agx", 4);
        CHECK(std::fabs(s.top.y - 140.0f) < 2.5f && std::fabs(s.top.z - 242.0f) < 2.5f);
        if (curve <= 1) CHECK(std::fabs(s.top.x - 64.0f) < 2.5f);
    }
    gfx::Lighting().toneCurve = 1;
    gfx::Lighting().hasSky = true; gfx::Lighting().skyColor[0] = 0.5f; gfx::Lighting().skyColor[1] = 0.6f; gfx::Lighting().skyColor[2] = 0.7f;
    for (int curve = 2; curve <= 3; curve++) {
        gfx::Lighting().toneCurve = curve;
        const Shot s = Shoot(engine, curve == 2 ? "flat_soft_aces" : "flat_soft_agx", 4);
        CHECK(std::fabs(s.top.x - 128.0f) < 2.5f && std::fabs(s.top.y - 153.0f) < 2.5f && std::fabs(s.top.z - 179.0f) < 2.5f);
    }
    gfx::ResetLighting();

    // ---- The atmosphere through a day.
    gfx::LightingSettings& L = gfx::Lighting();
    L.hasSky = true; L.skyMode = 1;
    CHECK(gfx::AtmosphereActive());

    gfx::SetTimeOfDay(12.0f);
    const Shot noon = Shoot(engine, "noon", 30);
    CHECK(noon.top.z > noon.top.x + 40.0f);                       // blue overhead
    CHECK(noon.mid.x + noon.mid.y + noon.mid.z > noon.top.x + noon.top.y + noon.top.z);   // paler towards the horizon
    {
        const Vector3 sun = gfx::SunRadiance(), sky = gfx::AmbientSky();
        std::printf("noon: sun light (%.2f,%.2f,%.2f)  sky light (%.3f,%.3f,%.3f)\n", sun.x, sun.y, sun.z, sky.x, sky.y, sky.z);
        CHECK(sun.x > 0.6f && sun.x < 1.4f);                      // a white surface in the noon sun is about 1
        CHECK(sky.z > sky.x * 1.2f);                              // the sky's light is blue
    }

    LookTowards(cam, gfx::SunPosition(), 0.45f);
    const Shot noonSun = Shoot(engine, "noon_sun", 30);

    gfx::SetTimeOfDay(17.75f);                                    // the sun is a few degrees up
    LookTowards(cam, gfx::SunPosition(), 0.12f);
    const Shot sunset = Shoot(engine, "sunset", 40);
    std::printf("sunset: just above the horizon (%.0f,%.0f,%.0f)\n", sunset.low.x, sunset.low.y, sunset.low.z);
    CHECK(sunset.low.x > sunset.low.z + 25.0f);                   // red and orange towards the sun
    CHECK(sunset.top.z > sunset.top.x);                           // still blue overhead
    {
        const Vector3 sun = gfx::SunRadiance();
        std::printf("sunset: sun light (%.2f,%.2f,%.2f)\n", sun.x, sun.y, sun.z);
        CHECK(sun.x > sun.z * 2.0f);                              // and the light itself is warm
    }
    LookTowards(cam, Vector3Negate(gfx::SunPosition()), 0.12f);
    Shoot(engine, "sunset_away", 30);

    gfx::SetTimeOfDay(18.4f);                                     // just after sunset
    LookTowards(cam, gfx::SunPosition(), 0.12f);
    const Shot dusk = Shoot(engine, "dusk", 40);
    CHECK(dusk.lum < noon.lum);

    gfx::SetTimeOfDay(23.5f);
    LookTowards(cam, gfx::MoonPosition(), 0.55f);
    const Shot night = Shoot(engine, "night_moon", 60);
    CHECK(night.lum < noon.lum * 0.6f);
    cam.position = { -60.0f, 14.0f, 110.0f };
    cam.target = { 10.0f, 60.0f, -40.0f };
    Shoot(engine, "night_stars", 30);

    gfx::SetTimeOfDay(6.3f);
    LookTowards(cam, gfx::SunPosition(), 0.10f);
    Shoot(engine, "sunrise", 40);

    // ---- From high up: the curve of the planet's haze and the ground beyond the far plane.
    gfx::SetTimeOfDay(15.0f);
    cam.position = { 0.0f, 3000.0f, 800.0f };
    cam.target = { 0.0f, 2600.0f, -2000.0f };
    Shoot(engine, "high", 30);

    // ---- Clouds.
    {
        L.hasClouds = true; L.cloudCoverage = 0.5f;
        gfx::SetTimeOfDay(13.0f);
        cam.position = { -60.0f, 14.0f, 110.0f };
        cam.target = { 10.0f, 50.0f, -40.0f };
        // The noise is made on other threads the first time clouds are wanted.
        const double start = GetTime();
        while (!gfx::CloudsActive() && GetTime() - start < 30.0) engine.StepFrame(1.0f / 60.0f);
        std::printf("cloud noise ready after %.2f s\n", GetTime() - start);
        CHECK(gfx::CloudsActive());
        const Shot clouds = Shoot(engine, "clouds_noon", 30);
        CHECK(clouds.bright + 0.0f >= 0.0f);
        // Clouds are white and grey where the clear sky was blue: the top of the picture is less blue than it was.
        CHECK(clouds.top.z - clouds.top.x < noon.top.z - noon.top.x - 6.0f);

        cam.position = { -60.0f, 6.0f, 110.0f };
        cam.target = { 10.0f, 8.0f, -40.0f };
        Shoot(engine, "clouds_street", 20);

        gfx::SetTimeOfDay(17.6f);
        LookTowards(cam, gfx::SunPosition(), 0.22f);
        Shoot(engine, "clouds_sunset", 40);

        gfx::SetTimeOfDay(13.0f);
        L.cloudCoverage = 0.2f;
        cam.position = { -60.0f, 14.0f, 110.0f };
        cam.target = { 10.0f, 40.0f, -40.0f };
        Shoot(engine, "clouds_few", 30);

        L.cloudCoverage = 0.5f;
        L.hasWeather = true; L.overcast = 1.0f;
        const Shot overcast = Shoot(engine, "clouds_overcast", 40);
        CHECK(overcast.lum < clouds.lum);
        {
            const Vector3 skyLight = gfx::AmbientSky();
            CHECK(std::fabs(skyLight.z - skyLight.x) < 0.5f * skyLight.z);    // grey light under cloud
        }
        L.rain = 0.8f;
        Shoot(engine, "clouds_rain", 30);
        L.hasWeather = false; L.overcast = 0.0f; L.rain = 0.0f;

        // Above the clouds, looking down on them and across them.
        cam.position = { 0.0f, 4200.0f, 800.0f };
        cam.target = { 0.0f, 3300.0f, -3000.0f };
        Shoot(engine, "clouds_above", 30);

        gfx::SetTimeOfDay(23.5f);
        cam.position = { -60.0f, 14.0f, 110.0f };
        cam.target = { 10.0f, 60.0f, -40.0f };
        Shoot(engine, "clouds_night", 50);
        L.hasClouds = false;
    }

    // ---- Other skies: the same air with different numbers.
    cam.position = { -60.0f, 14.0f, 110.0f };
    cam.target = { 10.0f, 30.0f, -40.0f };
    gfx::SetTimeOfDay(14.0f);
    L.airColor[0] = 1.0f; L.airColor[1] = 0.55f; L.airColor[2] = 0.25f; L.haze = 5.0f; L.hazeColor[0] = 1.0f; L.hazeColor[1] = 0.7f; L.hazeColor[2] = 0.45f;
    const Shot dusty = Shoot(engine, "dusty", 30);
    CHECK(dusty.top.x > dusty.top.z);                             // a butterscotch sky
    L.airColor[0] = 0.25f; L.airColor[1] = 1.0f; L.airColor[2] = 0.45f; L.haze = 1.0f; L.hazeColor[0] = L.hazeColor[1] = L.hazeColor[2] = 1.0f;
    const Shot green = Shoot(engine, "green", 30);
    CHECK(green.top.y > green.top.x && green.top.y > green.top.z);

    std::printf(g_fail ? "sky_test: %d FAILED\n" : "sky_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
