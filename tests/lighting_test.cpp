// Lighting items (sun, ambient, sky, fog) change the render and survive a scene save/load. Renders the same view
// with different settings (lighting_<name>.png) and checks the images differ the way they should. Opens a window,
// so it is not registered with ctest: build/tests/lighting_test
#include "raylib.h"
#include "raymath.h"
#include "Engine.hpp"
#include "Engine/Graphics.hpp"
#include "Engine/Backend/ScenePersistence.hpp"
#include "../include/CityGen/City.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>
#include <vector>

using namespace city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

struct Stats { float r = 0, g = 0, b = 0, skyR = 0, skyG = 0, skyB = 0, farR = 0, farG = 0, farB = 0; };

static Stats Shoot(Engine& engine, const char* name) {
    for (int f = 0; f < 6; f++) engine.StepFrame(1.0f / 60.0f);
    char path[96];
    std::snprintf(path, sizeof path, "lighting_%s.png", name);
    TakeScreenshot(path);
    Image img = LoadImage(path);
    Stats s;
    long n = 0;
    for (int y = img.height / 2; y < img.height; y += 4)
        for (int x = 0; x < img.width; x += 4) { const Color c = GetImageColor(img, x, y); s.r += c.r; s.g += c.g; s.b += c.b; n++; }
    s.r /= (float)n; s.g /= (float)n; s.b /= (float)n;
    const Color sky = GetImageColor(img, img.width / 2, 4);       // top of the picture
    s.skyR = sky.r; s.skyG = sky.g; s.skyB = sky.b;
    const Color far = GetImageColor(img, img.width / 2, img.height / 2 - 10);   // near the horizon
    s.farR = far.r; s.farG = far.g; s.farB = far.b;
    UnloadImage(img);
    return s;
}

int main() {
    Engine engine(640, 360, "lighting_test", 60);
    engine.SetPlayerBuild(true);
    auto owner = std::make_unique<City>();
    City& c = *owner;
    engine.AddEntity(std::move(owner));
    c.GetParams().gridX = 8; c.GetParams().gridZ = 8;
    c.GenerateGrid({ 0.0f, 0.0f });
    gfx::ResetLighting();
    gfx::SetTimeOfDay(12.0f);
    Camera3D& cam = engine.GetCamera();
    cam.position = { -60.0f, 14.0f, 110.0f };
    cam.target = { 10.0f, 6.0f, -40.0f };
    cam.up = { 0.0f, 1.0f, 0.0f };
    cam.fovy = 60.0f;

    const Stats base = Shoot(engine, "default");

    gfx::Lighting().hasAmbient = true; gfx::Lighting().ambient = 0.2f;
    const Stats dark = Shoot(engine, "ambient_low");
    CHECK(dark.r + dark.g + dark.b < base.r + base.g + base.b - 12.0f);
    gfx::ResetLighting(); gfx::SetTimeOfDay(12.0f);

    gfx::Lighting().hasSun = true; gfx::Lighting().sunColor[0] = 1.0f; gfx::Lighting().sunColor[1] = 0.45f; gfx::Lighting().sunColor[2] = 0.15f;
    const Stats orange = Shoot(engine, "sun_orange");
    CHECK(orange.r - orange.b > base.r - base.b + 8.0f);              // warmer
    gfx::ResetLighting(); gfx::SetTimeOfDay(12.0f);

    gfx::Lighting().hasSun = true; gfx::Lighting().sunAzimuth = 250.0f; gfx::Lighting().sunElevation = 14.0f;
    Shoot(engine, "sun_low");
    gfx::ResetLighting(); gfx::SetTimeOfDay(12.0f);

    gfx::Lighting().hasSky = true; gfx::Lighting().skyColor[0] = 0.25f; gfx::Lighting().skyColor[1] = 0.55f; gfx::Lighting().skyColor[2] = 0.95f;
    const Stats blue = Shoot(engine, "sky_blue");
    CHECK(blue.skyB > blue.skyR + 80.0f);
    gfx::ResetLighting(); gfx::SetTimeOfDay(12.0f);

    gfx::Lighting().hasSky = true; gfx::Lighting().skyColor[0] = 0.25f; gfx::Lighting().skyColor[1] = 0.55f; gfx::Lighting().skyColor[2] = 0.95f;
    gfx::Lighting().hasFog = true; gfx::Lighting().fogDensity = 0.012f;
    const Stats fog = Shoot(engine, "fog");
    std::printf("horizon pixel: no fog (%.0f,%.0f,%.0f) -> fog (%.0f,%.0f,%.0f), sky (%.0f,%.0f,%.0f)\n", base.farR, base.farG, base.farB, fog.farR, fog.farG, fog.farB, fog.skyR, fog.skyG, fog.skyB);
    CHECK(fog.farB > base.farB + 15.0f);                               // distant geometry takes the (blue) sky colour
    gfx::ResetLighting(); gfx::SetTimeOfDay(12.0f);

    // Save / load: the items and their values come back.
    {
        gfx::LightingSettings& L = gfx::Lighting();
        L.hasSun = true; L.sunAzimuth = 123.0f; L.sunElevation = 33.0f; L.sunIntensity = 1.7f; L.sunColor[1] = 0.6f;
        L.hasFog = true; L.fogDensity = 0.013f; L.hasSky = true; L.skyColor[2] = 0.9f; L.hasAmbient = true; L.ambient = 0.55f; L.timeOfDay = 17.5f;
        std::vector<ScatteredObject*> objs; std::vector<std::unique_ptr<ModelGroup>> models;
        std::stringstream ss;
        CHECK(SaveSceneToStream(ss, objs, models, "", nullptr));
        gfx::ResetLighting();
        CHECK(gfx::Lighting().hasSun == false);
        std::vector<ScatteredObject*> objs2; std::vector<std::unique_ptr<ModelGroup>> models2;
        CHECK(LoadSceneFromStream(ss, engine, objs2, models2, "", nullptr, nullptr));
        const gfx::LightingSettings& R = gfx::Lighting();
        CHECK(R.hasSun && R.hasFog && R.hasSky && R.hasAmbient);
        CHECK(std::fabs(R.sunAzimuth - 123.0f) < 0.01f && std::fabs(R.sunElevation - 33.0f) < 0.01f && std::fabs(R.sunIntensity - 1.7f) < 0.01f);
        CHECK(std::fabs(R.fogDensity - 0.013f) < 1e-5f && std::fabs(R.ambient - 0.55f) < 1e-4f && std::fabs(R.timeOfDay - 17.5f) < 1e-3f);
        CHECK(std::fabs(R.skyColor[2] - 0.9f) < 1e-4f && std::fabs(R.sunColor[1] - 0.6f) < 1e-4f);
    }

    std::printf(g_fail ? "lighting_test: %d FAILED\n" : "lighting_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
