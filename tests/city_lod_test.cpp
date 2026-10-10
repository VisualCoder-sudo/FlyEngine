// Big cities: how long generation takes, what a frame costs (shadow / opaque passes, draw calls, triangles) from an
// overview, a rooftop and a street, with traffic. Prints the numbers and renders pictures (city_lod_<name>.png). Opens a
// window, so it is not registered with ctest: build/tests/city_lod_test [grid size, default 70]
#include "raylib.h"
#include "raymath.h"
#include "Engine.hpp"
#include "Engine/Clouds.hpp"
#include "Engine/Graphics.hpp"
#include "Engine/PostFX.hpp"
#include "../include/CityGen/City.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <vector>

using namespace city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

struct View { const char* name; Vector3 pos, target; };

static double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Average frame time (ms) over n frames, plus the last frame's pass timings and counters.
static double Measure(Engine& engine, const View& v, int n, gfx::FrameTimings* ft, int* draws, int* tris) {
    Camera3D& cam = engine.GetCamera();
    cam.position = v.pos; cam.target = v.target; cam.up = { 0.0f, 1.0f, 0.0f }; cam.fovy = 55.0f;
    for (int f = 0; f < 6; f++) engine.StepFrame(1.0f / 60.0f);       // warm up (builds the shadow map, uploads buffers)
    const double t0 = NowMs();
    for (int f = 0; f < n; f++) engine.StepFrame(1.0f / 60.0f);
    const double ms = (NowMs() - t0) / n;
    if (ft) *ft = gfx::GetFrameTimings();
    if (draws) *draws = gfx::GetDrawCallCount();
    if (tris) *tris = gfx::GetTriangleCount();
    return ms;
}

int main(int argc, char** argv) {
    const int grid = argc > 1 ? std::max(10, atoi(argv[1])) : 70;
    Engine engine(1000, 640, "city_lod_test", 1000);
    engine.SetPlayerBuild(true);
    gfx::SetTimeOfDay(12.0f);
    // What the picture costs: city_lod_test <grid> <tier 0..3> [sky]. "sky" turns on the atmosphere sky with clouds
    // (without it the scene has the plain sky of one colour, and the tier only decides anti-aliasing, occlusion and cascades).
    if (argc > 2) gfx::SetQualityTier(atoi(argv[2]));
    if (argc > 3) {
        gfx::Lighting().hasSky = true; gfx::Lighting().skyMode = 1;
        gfx::Lighting().hasClouds = true;
        const double w0 = NowMs();
        while (!gfx::CloudsActive() && NowMs() - w0 < 30000.0) engine.StepFrame(1.0f / 60.0f);
    }
    std::printf("quality tier %d, %s\n", gfx::Quality().tier, argc > 3 ? "atmosphere sky with clouds" : "sky of one colour");
    gfx::SetShadowReuseEnabled(false);       // measure the worst case: shadows redrawn every frame
    auto owner = std::make_unique<City>();
    City& c = *owner;
    engine.AddEntity(std::move(owner));
    c.GetParams().gridX = grid; c.GetParams().gridZ = grid;
    c.GetParams().cars = 0;
    const double g0 = NowMs();
    c.GenerateGrid({ 0.0f, 0.0f });
    const double queuedMs = NowMs() - g0;
    int waitFrames = 0;
    while (c.IsRebuilding() && waitFrames < 20000) { engine.StepFrame(1.0f / 60.0f); waitFrames++; }   // big cities rebuild on a worker thread
    const double genMs = NowMs() - g0;
    std::printf("generation: queued in %.0f ms, finished after %.0f ms (%d frames pumped)\n", queuedMs, genMs, waitFrames);
    std::printf("grid %dx%d: %zu nodes, %zu roads, %zu blocks; generated in %.0f ms\n", grid, grid, c.GetNodes().size(), c.GetEdges().size(), c.GetBlocks().size(), genMs);
    size_t buildings = 0;
    for (const Block& b : c.GetBlocks()) buildings += b.buildings.size();
    std::printf("buildings: %zu\n", buildings);

    const float half = (float)grid * 40.0f * 0.5f;
    const View views[] = {
        { "overview", { 0.0f, half * 1.1f, half * 1.0f }, { 0.0f, 0.0f, 0.0f } },
        { "roof", { 0.0f, 120.0f, 0.0f }, { 160.0f, 10.0f, 160.0f } },
        { "street", { 10.0f, 2.2f, 10.0f }, { 120.0f, 6.0f, 60.0f } },
    };
    std::vector<double> frameMs;
    for (const View& v : views) {
        gfx::FrameTimings ft; int draws = 0, tris = 0;
        const double ms = Measure(engine, v, 20, &ft, &draws, &tris);
        std::printf("%-9s %7.2f ms/frame   (shadow %.2f, sky %.2f, opaque %.2f, post %.2f)  %5d draws  %8d triangles\n", v.name, ms, ft.shadowMs, ft.skyMs, ft.opaqueMs, ft.postMs, draws, tris);
        frameMs.push_back(ms);
        TakeScreenshot(TextFormat("city_lod_%s.png", v.name));
    }

    // The same views with the distance LOD off: the LOD must cut the draw calls a lot, and the buildings must all still be there.
    {
        c.GetParams().lodDistance = 0.0f;
        double offMs = 0.0, onMs = 0.0;
        int offDraws = 0, onDraws = 0;
        for (const View& v : views) {
            int d0 = 0, d1 = 0;
            offMs += Measure(engine, v, 12, nullptr, &d0, nullptr);
            offDraws += d0;
            (void)d1;
        }
        c.GetParams().lodDistance = 450.0f;
        for (const View& v : views) {
            int d1 = 0;
            onMs += Measure(engine, v, 12, nullptr, &d1, nullptr);
            onDraws += d1;
        }
        std::printf("distance LOD off -> on: %d -> %d draw calls over the three views, %.1f -> %.1f ms per frame (summed)\n", offDraws, onDraws, offMs, onMs);
        CHECK(onDraws < offDraws * 0.45);
        CHECK(onMs < offMs);
    }

    // The setting survives save/load (a small city: the roundtrip of a big one takes a while).
    {
        City small;
        small.GetParams().gridX = 3; small.GetParams().gridZ = 3; small.GetParams().lodDistance = 300.0f;
        small.GenerateGrid({ 0.0f, 0.0f });
        std::stringstream ss;
        CHECK(small.WriteToStream(ss));
        City other;
        CHECK(other.ReadFromStream(ss));
        CHECK(other.GetParams().lodDistance == 300.0f);
    }

    // With traffic and people.
    c.GetParams().cars = 600; c.GetParams().pedestrians = 600;
    SetTrafficRunning(true);
    const double s0 = NowMs();
    for (int i = 0; i < 100; i++) c.TrafficStepForTest(0.05f);
    const double stepMs = (NowMs() - s0) / 100.0;
    const City::TrafficStats ts = c.GetTrafficStats();
    std::printf("traffic: %d cars (%d detailed), %d people; simulation step %.2f ms\n", ts.cars, ts.nearCars, ts.peds, stepMs);
    for (const View& v : views) {
        gfx::FrameTimings ft; int draws = 0, tris = 0;
        const double ms = Measure(engine, v, 20, &ft, &draws, &tris);
        std::printf("%-9s %7.2f ms/frame with traffic  %5d draws  %8d triangles\n", v.name, ms, draws, tris);
    }
    SetTrafficRunning(false);

    std::printf(g_fail ? "city_lod_test: %d FAILED\n" : "city_lod_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
