// Sidewalk furniture: every prop type gets placed in a generated city, and a close-up screenshot of one of
// each is written to the working directory (city_prop_<name>.png) for a visual check. Opens a window, so it is
// not registered with ctest: build/tests/city_props_test
#include "raylib.h"
#include "raymath.h"
#include "Engine.hpp"
#include "../include/CityGen/City.hpp"

#include <cmath>
#include <cstdio>
#include <memory>

using namespace city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
    Engine engine(640, 360, "city_props_test", 60);
    engine.SetPlayerBuild(true);   // no editor UI
    auto cityOwner = std::make_unique<City>();
    City& c = *cityOwner;
    c.GetParams().gridX = 10; c.GetParams().gridZ = 10;
    c.GetParams().furniture = true;
    engine.AddEntity(std::move(cityOwner));
    c.GenerateGrid({ 0.0f, 0.0f });

    struct P { int shape; const char* name; };
    const P props[] = { { kPropBench, "bench" }, { kPropHydrant, "hydrant" }, { kPropBollard, "bollard" },
                        { kPropBusStop, "busstop" }, { kPropSign, "sign" }, { kPropLamp, "lamp" } };
    for (const P& p : props) {
        const int n = c.CountInstances(p.shape);
        std::printf("%-8s %d placed\n", p.name, n);
        CHECK(n > 0);
        Vector3 pos; float yaw;
        if (!c.FindInstance(p.shape, n / 2, pos, yaw)) continue;
        const Vector3 toRoad = { sinf(yaw), 0.0f, cosf(yaw) };            // local +z points toward the road
        Camera3D& cam = engine.GetCamera();
        const float d = p.shape == kPropBusStop ? 6.5f : 3.2f;
        cam.position = { pos.x + toRoad.x * d + toRoad.z * 0.8f, pos.y + 1.5f, pos.z + toRoad.z * d - toRoad.x * 0.8f };
        cam.target = { pos.x, pos.y + (p.shape == kPropBusStop ? 1.2f : 0.7f), pos.z };
        cam.up = { 0.0f, 1.0f, 0.0f };
        cam.fovy = 55.0f;
        cam.projection = CAMERA_PERSPECTIVE;
        for (int f = 0; f < 8; f++) engine.StepFrame(1.0f / 60.0f);
        char path[128];
        std::snprintf(path, sizeof path, "city_prop_%s.png", p.name);
        TakeScreenshot(path);
    }

    std::printf(g_fail ? "city_props_test: %d FAILED\n" : "city_props_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
