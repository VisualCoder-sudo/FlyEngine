// Sloped roads on regular and irregular (non-right-angle) street layouts: renders top-down and oblique shots of
// each (city_slope_<name>.png) so the pads next to sloped roads can be inspected. Opens a window, so it is not
// registered with ctest: build/tests/city_slope_test
#include "raylib.h"
#include "raymath.h"
#include "Engine.hpp"
#include "Engine/Graphics.hpp"
#include "../include/CityGen/City.hpp"

#include <cmath>
#include <cstdio>
#include <memory>

using namespace city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void Shot(Engine& engine, Vector3 pos, Vector3 target, const char* path) {
    Camera3D& cam = engine.GetCamera();
    cam.position = pos;
    cam.target = target;
    cam.up = { 0.0f, 0.0f, -1.0f };
    if (fabsf(pos.x - target.x) + fabsf(pos.z - target.z) > 1.0f) cam.up = { 0.0f, 1.0f, 0.0f };
    cam.fovy = 55.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    for (int f = 0; f < 8; f++) engine.StepFrame(1.0f / 60.0f);
    TakeScreenshot(path);
}

int main() {
    Engine engine(900, 700, "city_slope_test", 60);
    engine.SetPlayerBuild(true);
    gfx::SetTimeOfDay(12.0f);
    struct Case { const char* name; bool organic; float strength; };
    const Case cases[] = { { "regular", false, 0.0f }, { "organic", true, 0.45f }, { "veryorganic", true, 0.8f } };
    for (const Case& cs : cases) {
        auto owner = std::make_unique<City>();
        City& c = *owner;
        City* raw = owner.get();
        engine.AddEntity(std::move(owner));
        c.GetParams().gridX = 5; c.GetParams().gridZ = 5;
        c.GetParams().organic = cs.organic;
        c.GetParams().organicStrength = cs.strength;
        c.GetParams().cars = 0;
        c.GenerateGrid({ 0.0f, 0.0f });
        // Sloped roads that cross the blocks at angles other than 90 degrees: a diagonal ramp and a shallower one.
        auto line = [&](Vector2 p0, Vector2 p1, float h0, float h1) {
            std::vector<Vector2> pts;
            for (int k = 0; k <= 16; k++) pts.push_back({ p0.x + (p1.x - p0.x) * (float)k / 16.0f, p0.y + (p1.y - p0.y) * (float)k / 16.0f });
            c.AddRoadPath(pts, h0, h1);
        };
        line({ -95.0f, -95.0f }, { 95.0f, 95.0f }, 0.0f, 9.0f);
        line({ -100.0f, 40.0f }, { 100.0f, -10.0f }, 6.0f, 0.0f);
        Shot(engine, { 0.0f, 260.0f, 0.1f }, { 0.0f, 0.0f, 0.0f }, TextFormat("city_slope_%s_top.png", cs.name));
        Shot(engine, { 0.0f, 150.0f, 170.0f }, { 0.0f, 0.0f, 0.0f }, TextFormat("city_slope_%s_oblique.png", cs.name));
        std::printf("%s: %zu nodes, %zu blocks\n", cs.name, c.GetNodes().size(), c.GetBlocks().size());
        engine.RemoveEntity(raw);
    }
    std::printf(g_fail ? "city_slope_test: %d FAILED\n" : "city_slope_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
