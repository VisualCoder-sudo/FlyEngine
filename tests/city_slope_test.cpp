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
#include <vector>

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
    // Irregular (organic) street layout on uneven ground: every node gets its own height, roads meet at odd angles
    // and the blocks are irregular polygons with buildings. Close oblique views.
    auto owner = std::make_unique<City>();
    City& c = *owner;
    engine.AddEntity(std::move(owner));
    c.GetParams().gridX = 5; c.GetParams().gridZ = 5;
    c.GetParams().organic = true;
    c.GetParams().organicStrength = 0.6f;
    c.GetParams().cars = 0;
    c.GenerateGrid({ 0.0f, 0.0f });
    for (int i = 0; i < (int)c.GetNodes().size(); i++) {
        const Vector2 p = c.GetNodes()[(size_t)i].pos;
        c.SetNodeHeight(i, 5.0f + 4.0f * sinf(p.x * 0.045f) + 4.0f * cosf(p.y * 0.05f + p.x * 0.02f));   // rolling ground, 0..13 m
    }
    std::printf("organic hills: %zu nodes, %zu blocks\n", c.GetNodes().size(), c.GetBlocks().size());
    const Vector3 views[4][2] = {
        { { -30.0f, 45.0f, 50.0f }, { 0.0f, 6.0f, 0.0f } },
        { { 40.0f, 40.0f, -35.0f }, { 0.0f, 6.0f, 10.0f } },
        { { 0.0f, 70.0f, 0.1f }, { 0.0f, 0.0f, 0.0f } },
        { { 60.0f, 50.0f, 60.0f }, { 10.0f, 6.0f, 10.0f } } };
    for (int v = 0; v < 4; v++) Shot(engine, views[v][0], views[v][1], TextFormat("city_slope_angled_%d.png", v));

    // Close-ups of the foot of the building with the most buried foundation: windows must start above the plinth, not be
    // cut diagonally by the sloping ground.
    {
        const Building* best = nullptr;
        for (const Block& blk : c.GetBlocks()) for (const Building& b : blk.buildings) if (!best || fabsf(b.foundation - 1.8f) < fabsf(best->foundation - 1.8f)) best = &b;   // a typical sloped foot, not the steepest outlier
        CHECK(best != nullptr);
        if (best) {
            std::printf("sample foundation: %.2f m (building %.1f x %.1f x %.1f)\n", best->foundation, best->size.x, best->size.y, best->size.z);
            const float baseY = best->center.y - best->size.y * 0.5f + best->foundation;
            for (int k = 0; k < 3; k++) {
                const float ang = best->angleY + (float)k * 2.1f;
                Shot(engine, { best->center.x + sinf(ang) * 16.0f, baseY + 3.0f, best->center.z + cosf(ang) * 16.0f }, { best->center.x, baseY + 1.0f, best->center.z }, TextFormat("city_slope_foot_%d.png", k));
            }
        }
    }

    std::printf(g_fail ? "city_slope_test: %d FAILED\n" : "city_slope_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
