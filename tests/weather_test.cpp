// Sun path (the sun follows the time of day), weather (cloud, rain, wet ground), street lamps that switch on one by
// one at dusk, and water that follows the scene lighting. Renders pictures (weather_<name>.png) and checks them.
// Opens a window, so it is not registered with ctest: build/tests/weather_test
#include "raylib.h"
#include "raymath.h"
#include "Engine.hpp"
#include "Engine/Graphics.hpp"
#include "../include/CityGen/City.hpp"
#include "../include/Terrain/Water/WaterBody.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

struct Shot { float lum = 0, r = 0, g = 0, b = 0, skyR = 0, skyG = 0, skyB = 0, warmPx = 0; std::string path; };

static Shot Shoot(Engine& engine, const char* name, int frames = 8) {
    for (int f = 0; f < frames; f++) engine.StepFrame(1.0f / 60.0f);
    Shot s;
    s.path = std::string("weather_") + name + ".png";
    TakeScreenshot(s.path.c_str());
    Image img = LoadImage(s.path.c_str());
    long n = 0, warm = 0;
    for (int y = 0; y < img.height; y += 3)
        for (int x = 0; x < img.width; x += 3) {
            const Color c = GetImageColor(img, x, y);
            s.r += c.r; s.g += c.g; s.b += c.b; n++;
            if (c.r > 220 && c.g > 180 && c.b < 190 && c.r > c.b + 50) warm++;
        }
    s.r /= (float)n; s.g /= (float)n; s.b /= (float)n; s.lum = (s.r + s.g + s.b) / 3.0f;
    s.warmPx = (float)warm / (float)n;
    const Color sky = GetImageColor(img, img.width / 2, 3);
    s.skyR = sky.r; s.skyG = sky.g; s.skyB = sky.b;
    UnloadImage(img);
    return s;
}

static float ImageDiff(const std::string& a, const std::string& b) {
    Image ia = LoadImage(a.c_str()), ib = LoadImage(b.c_str());
    double sum = 0; long n = 0;
    for (int y = 0; y < ia.height && y < ib.height; y += 3)
        for (int x = 0; x < ia.width && x < ib.width; x += 3) {
            const Color p = GetImageColor(ia, x, y), q = GetImageColor(ib, x, y);
            sum += std::abs(p.r - q.r) + std::abs(p.g - q.g) + std::abs(p.b - q.b); n++;
        }
    UnloadImage(ia); UnloadImage(ib);
    return (float)(sum / (3.0 * (double)n));
}

int main() {
    Engine engine(640, 360, "weather_test", 60);
    engine.SetPlayerBuild(true);
    auto owner = std::make_unique<City>();
    City& c = *owner;
    engine.AddEntity(std::move(owner));
    c.GetParams().gridX = 8; c.GetParams().gridZ = 8;
    c.GetParams().cars = 0;
    c.GenerateGrid({ 0.0f, 0.0f });
    city::SetTrafficRunning(false);
    gfx::ResetLighting();
    gfx::SetTimeOfDay(12.0f);
    Camera3D& cam = engine.GetCamera();
    cam.position = { -60.0f, 14.0f, 110.0f };
    cam.target = { 10.0f, 6.0f, -40.0f };
    cam.up = { 0.0f, 1.0f, 0.0f };
    cam.fovy = 60.0f;

    // ---- Sun path -------------------------------------------------------------------------------------------
    {
        gfx::SetTimeOfDay(12.0f);
        const Vector3 noon = gfx::SunDirection();
        std::printf("noon sun direction (%.3f, %.3f, %.3f)\n", noon.x, noon.y, noon.z);
        CHECK(std::fabs(noon.y + std::sin(63.43f * DEG2RAD)) < 0.02f);          // the old fixed sun: unchanged look at noon
        float prevEl = -1.0f;
        for (float h : { 6.5f, 8.0f, 10.0f, 12.0f }) {
            gfx::SetTimeOfDay(h);
            const float el = gfx::SunElevationNow();
            CHECK(el > prevEl);
            prevEl = el;
        }
        for (float h : { 12.0f, 14.0f, 16.0f, 17.5f }) {
            gfx::SetTimeOfDay(h);
            const float el = gfx::SunElevationNow();
            CHECK(el < prevEl + 0.01f);
            prevEl = el;
        }
        gfx::SetTimeOfDay(9.0f);  const Vector3 am = gfx::SunDirection();
        gfx::SetTimeOfDay(15.0f); const Vector3 pm = gfx::SunDirection();
        std::printf("9:00 sun dir (%.2f, %.2f, %.2f)   15:00 (%.2f, %.2f, %.2f)\n", am.x, am.y, am.z, pm.x, pm.y, pm.z);
        CHECK(Vector3Distance(am, pm) > 0.5f);                                    // it crossed the sky
        gfx::SetTimeOfDay(7.0f);
        const Vector3 lowSun = gfx::SunLightScale();
        gfx::SetTimeOfDay(12.0f);
        const Vector3 highSun = gfx::SunLightScale();
        std::printf("sun colour scale 7:00 (%.2f,%.2f,%.2f)  noon (%.2f,%.2f,%.2f)\n", lowSun.x, lowSun.y, lowSun.z, highSun.x, highSun.y, highSun.z);
        CHECK(lowSun.x > lowSun.z * 1.7f);                                        // orange near the horizon
        CHECK(highSun.x < highSun.z * 1.15f);                                     // white at noon
        // Pinned sun: the Sun item's own direction at any time.
        gfx::Lighting().hasSun = true; gfx::Lighting().sunFollowsTime = false;
        gfx::SetTimeOfDay(9.0f);  const Vector3 p1 = gfx::SunDirection();
        gfx::SetTimeOfDay(15.0f); const Vector3 p2 = gfx::SunDirection();
        CHECK(Vector3Distance(p1, p2) < 0.001f);
        gfx::ResetLighting();

        gfx::SetTimeOfDay(9.0f);
        const Shot morning = Shoot(engine, "sun_9");
        gfx::SetTimeOfDay(15.0f);
        const Shot afternoon = Shoot(engine, "sun_15");
        gfx::SetTimeOfDay(7.0f);
        const Shot sunrise = Shoot(engine, "sun_7");
        std::printf("pictures: 9:00 vs 15:00 differ by %.1f\n", ImageDiff(morning.path, afternoon.path));
        CHECK(ImageDiff(morning.path, afternoon.path) > 4.0f);                    // shadows and light moved
        CHECK(sunrise.r - sunrise.b > morning.r - morning.b + 3.0f);              // warmer at sunrise
        gfx::SetTimeOfDay(12.0f);
    }

    // ---- Weather ----------------------------------------------------------------------------------------------
    {
        gfx::Lighting().hasSky = true; gfx::Lighting().skyColor[0] = 0.25f; gfx::Lighting().skyColor[1] = 0.55f; gfx::Lighting().skyColor[2] = 0.95f;
        const Shot clear = Shoot(engine, "clear");
        gfx::Lighting().hasWeather = true; gfx::Lighting().overcast = 0.9f;
        const Shot cloudy = Shoot(engine, "overcast");
        gfx::Lighting().hasWeather = true; gfx::Lighting().overcast = 0.0f; gfx::Lighting().wetGround = 1.0f;
        const Shot wet = Shoot(engine, "wet");
        gfx::Lighting().overcast = 0.9f; gfx::Lighting().rain = 1.0f; gfx::Lighting().wetGround = 0.0f;
        const Shot storm = Shoot(engine, "storm");
        std::printf("brightness clear %.0f  overcast %.0f  wet %.0f  storm %.0f; sky blue-red clear %.0f  overcast %.0f  storm %.0f\n",
                    clear.lum, cloudy.lum, wet.lum, storm.lum, clear.skyB - clear.skyR, cloudy.skyB - cloudy.skyR, storm.skyB - storm.skyR);
        CHECK(cloudy.lum < clear.lum - 8.0f);
        CHECK(cloudy.skyB - cloudy.skyR < (clear.skyB - clear.skyR) * 0.7f);       // the sky goes grey
        CHECK(wet.lum < clear.lum - 2.0f);                                          // wet roads are darker
        CHECK(std::fabs(wet.skyB - clear.skyB) < 3.0f);                             // wet ground alone leaves the sky alone
        CHECK(storm.lum < cloudy.lum + 1.0f);
        CHECK(gfx::RainNow() > 0.99f && gfx::WetnessNow() > 0.99f);
        // Rain streaks are drawn: the same overcast scene without rain looks different.
        gfx::Lighting().rain = 0.0f; gfx::Lighting().wetGround = 1.0f;
        const Shot noRain = Shoot(engine, "storm_norain");
        CHECK(ImageDiff(noRain.path, storm.path) > 1.0f);
        gfx::ResetLighting();
    }

    // ---- Street lamps switch on one by one ------------------------------------------------------------------
    {
        auto fractionOn = [](float night) {
            int on = 0, n = 0;
            for (int ix = 0; ix < 40; ix++) for (int iz = 0; iz < 40; iz++) { n++; if (gfx::LampSwitchOn(ix * 11.3f, iz * 9.7f, night) > 0.5f) on++; }
            return (float)on / (float)n;
        };
        const float f0 = fractionOn(0.1f), f1 = fractionOn(0.3f), f2 = fractionOn(0.45f), f3 = fractionOn(0.6f), f4 = fractionOn(1.0f);
        std::printf("lamps on: night 0.1 -> %.2f, 0.3 -> %.2f, 0.45 -> %.2f, 0.6 -> %.2f, 1.0 -> %.2f\n", f0, f1, f2, f3, f4);
        CHECK(f0 < 0.02f);
        CHECK(f1 > 0.05f && f1 < 0.55f);
        CHECK(f2 > f1 && f3 > f2);
        CHECK(f4 > 0.8f && f4 < 1.0f);                 // a few faulty ones are off at any moment
        // Street-level pictures at dusk and at night.
        cam.position = { -30.0f, 3.0f, 60.0f };
        cam.target = { -30.0f, 4.0f, -60.0f };
        gfx::SetTimeOfDay(17.9f);
        const Shot dusk = Shoot(engine, "lamps_dusk");
        gfx::SetTimeOfDay(22.0f);
        const Shot night = Shoot(engine, "lamps_night");
        std::printf("warm bright pixels: dusk %.4f, night %.4f\n", dusk.warmPx, night.warmPx);
        CHECK(night.warmPx > 0.0005f);
        CHECK(night.warmPx > dusk.warmPx);
        gfx::SetTimeOfDay(12.0f);
        cam.position = { -60.0f, 14.0f, 110.0f };
        cam.target = { 10.0f, 6.0f, -40.0f };
    }

    // ---- Pedestrians put up umbrellas in the rain ---------------------------------------------------------------
    {
        c.GetParams().pedestrians = 120;
        SetTrafficRunning(true);
        for (int i = 0; i < 600; i++) c.TrafficStepForTest(0.05f);
        Vector3 pp; float pyaw = 0.0f;
        CHECK(c.FindWalkingPed(5, pp, pyaw));
        cam.position = { pp.x + 9.0f, pp.y + 3.0f, pp.z + 9.0f };      // a few metres from a pedestrian, looking down at the street
        cam.target = { pp.x, pp.y + 1.2f, pp.z };
        cam.fovy = 55.0f;
        gfx::SetTimeOfDay(12.0f);
        const Shot dry = Shoot(engine, "umbrellas_dry");
        gfx::Lighting().hasWeather = true; gfx::Lighting().overcast = 0.7f; gfx::Lighting().rain = 0.8f;
        const Shot rainy = Shoot(engine, "umbrellas_rain");
        std::printf("umbrellas: dry vs rain picture difference %.1f\n", ImageDiff(dry.path, rainy.path));
        gfx::ResetLighting();
        SetTrafficRunning(false);
        c.GetParams().pedestrians = 0;
        cam.fovy = 60.0f;
    }

    // ---- Shop signs on the downtown towers --------------------------------------------------------------------
    {
        c.AutoDistricts(3.0f);          // a downtown in the middle: tall buildings
        cam.position = { 0.0f, 120.0f, 160.0f };
        cam.target = { 0.0f, 10.0f, 0.0f };
        gfx::SetTimeOfDay(12.0f);
        Shoot(engine, "downtown_overview");
        cam.position = { -20.0f, 10.0f, 70.0f };
        cam.target = { -10.0f, 14.0f, -20.0f };
        gfx::SetTimeOfDay(22.0f);
        const Shot s = Shoot(engine, "signs_night", 12);
        Image img = LoadImage(s.path.c_str());
        long neon = 0, n = 0;
        for (int y = 0; y < img.height; y += 2) for (int x = 0; x < img.width; x += 2) {
            const Color px = GetImageColor(img, x, y);
            n++;
            if ((px.r > 200 && px.b > 150 && px.g < 120) || (px.b > 200 && px.g > 170 && px.r < 110) || (px.r > 220 && px.g < 100 && px.b < 110)) neon++;
        }
        UnloadImage(img);
        std::printf("neon sign pixels at night: %.4f of the picture\n", (float)neon / (float)n);
        CHECK(neon > 30);
        gfx::SetTimeOfDay(12.0f);
        cam.position = { -60.0f, 14.0f, 110.0f };
        cam.target = { 10.0f, 6.0f, -40.0f };
    }

    // ---- Water follows the scene lighting ---------------------------------------------------------------------
    {
        engine.AddEntity(std::make_unique<WaterBody>(Vector3{ -150.0f, 0.0f, -150.0f }, Vector3{ 300.0f, 10.0f, 300.0f }, 0.0f));
        cam.position = { -150.0f, 35.0f, -60.0f };
        cam.target = { -150.0f, 0.0f, -150.0f };
        gfx::SetTimeOfDay(12.0f);
        const Shot day = Shoot(engine, "water_day", 12);
        gfx::SetTimeOfDay(23.0f);
        const Shot night = Shoot(engine, "water_night", 12);
        gfx::SetTimeOfDay(12.0f);
        gfx::Lighting().hasWeather = true; gfx::Lighting().overcast = 1.0f; gfx::Lighting().rain = 1.0f;
        const Shot storm = Shoot(engine, "water_storm", 12);
        std::printf("water view brightness: day %.0f, night %.0f, storm %.0f\n", day.lum, night.lum, storm.lum);
        CHECK(night.lum < day.lum * 0.6f);
        CHECK(storm.lum < day.lum - 6.0f);
        gfx::ResetLighting();
    }

    std::printf(g_fail ? "weather_test: %d FAILED\n" : "weather_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
