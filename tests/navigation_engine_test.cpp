// Navigation against the real engine and physics: the API (FlyNative_Nav*) bakes the level from ray casts into Box3D, so a
// crate on the floor is an obstacle, the floor under it is not walkable, and things that move are not part of the map.
// Needs a display (the engine opens a window); not registered with ctest: build/tests/navigation_engine_test
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/Backend/PhysicsSimulation.hpp"
#include "Engine/Backend/ScatteredObject.hpp"
#include "Engine/Scripts/ScriptRuntime.hpp"
#include "Engine/Scripts/FlyScriptApi.hpp"
#include "Engine/Navigation.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

namespace {

ScatteredObject* AddBox(Engine& engine, std::vector<ScatteredObject*>& objs, const char* name, Vector3 centre, Vector3 size, bool anchored) {
    auto o = std::make_unique<ScatteredObject>(centre, size, GRAY, ShapeType::Cube);
    o->SetName(name);
    o->anchored = anchored;
    ScatteredObject* raw = o.get();
    objs.push_back(raw);
    engine.AddEntity(std::move(o));
    return raw;
}

// True if the straight segments between the points stay out of the box (floor plan only).
bool AvoidsBox(const float* pts, int n, float x0, float z0, float x1, float z1) {
    for (int i = 0; i + 1 < n; ++i)
        for (int k = 0; k <= 200; ++k) {
            const float t = k / 200.0f;
            const float x = pts[i * 3] + (pts[(i + 1) * 3] - pts[i * 3]) * t, z = pts[i * 3 + 2] + (pts[(i + 1) * 3 + 2] - pts[i * 3 + 2]) * t;
            if (x > x0 && x < x1 && z > z0 && z < z1) return false;
        }
    return true;
}

} // namespace

int main() {
    Engine engine(320, 240, "navigation_engine_test", 60);
    std::vector<ScatteredObject*> objs;
    std::vector<std::unique_ptr<ModelGroup>> models;
    auto simOwner = std::make_unique<phys::Simulation>(objs);
    phys::Simulation& sim = *simOwner;
    engine.AddEntity(std::move(simOwner));
    auto rtOwner = std::make_unique<ScriptRuntime>();
    ScriptRuntime& rt = *rtOwner;
    engine.AddEntity(std::move(rtOwner));
    rt.BindWorld(objs, models, engine.GetCamera(), sim, engine);
    FlyNative_BindRuntime(&rt);

    AddBox(engine, objs, "Floor", { 0, -0.5f, 0 }, { 170, 1, 170 }, true);               // top at y = 0
    AddBox(engine, objs, "Crate", { 0, 1.0f, 0 }, { 4, 2, 4 }, true);                   // 4 x 4 m, 2 m high, in the middle
    AddBox(engine, objs, "Wall", { 10, 1.5f, -6 }, { 0.3f, 3, 12 }, true);              // x = 10, z from -12 to 0
    AddBox(engine, objs, "Curb", { -10, 0.15f, 0 }, { 1, 0.3f, 40 }, true);             // a 0.3 m step along x = -10
    AddBox(engine, objs, "Ball", { 8, 6.0f, 8 }, { 2, 2, 2 }, false);                  // loose: falls, must not be baked
    sim.StartPlay();
    rt.SetSimPlaying(true);
    for (int i = 0; i < 5; i++) sim.Update(1.0f / 60.0f);

    // Nothing is baked until asked, and bad settings fail.
    CHECK(!FlyNative_NavReady());
    float from[3] = { -15, 0, -15 }, to[3] = { 15, 0, 15 }, pts[96];
    CHECK(FlyNative_NavFindPath(from, to, pts, 32, 0) == 0);
    const float bad[13] = { 0, 0, 0, 0, 0, 0, 0.5f, 0.4f, 1.8f, 0.5f, 45, 0, 0 };
    CHECK(!FlyNative_NavBuild(bad) && !FlyNative_NavReady());

    const float settings[13] = { -20, -2, -20, 20, 10, 20, 0.5f, 0.4f, 1.8f, 0.5f, 45, 0, 0 };
    const double t0 = GetTime();
    CHECK(FlyNative_NavBuild(settings) && FlyNative_NavReady());
    std::printf("bake took %.0f ms\n", (GetTime() - t0) * 1000.0);
    std::printf("baked %zu cells\n", nav::World().mesh.CellCount());

    // The floor is walkable; the crate is not, and neither is the floor inside it. Its top is a surface of its own.
    float p[3] = { -15, 0, -15 };
    CHECK(FlyNative_NavIsWalkable(p));
    p[0] = 0; p[2] = 0;
    CHECK(!FlyNative_NavIsWalkable(p));            // floor level inside the crate
    p[1] = 2.0f;
    CHECK(FlyNative_NavIsWalkable(p));             // the crate's top
    // The loose ball is not part of the map: the floor under where it hangs is open ground and so is its top.
    p[0] = 8; p[1] = 0; p[2] = 8;
    CHECK(FlyNative_NavIsWalkable(p));
    p[1] = 1.0f;
    CHECK(!FlyNative_NavIsWalkable(p) || nav::World().mesh.IsWalkable({ 8, 0, 8 }));
    // Nothing leads up onto the crate.
    float top[3] = { 0, 2, 0 };
    CHECK(FlyNative_NavFindPath(from, top, pts, 32, 0) == 0);

    // Around the crate and past the wall's end.
    int n = FlyNative_NavFindPath(from, to, pts, 32, 0);
    CHECK(n >= 3);
    CHECK(AvoidsBox(pts, n, -2, -2, 2, 2));
    CHECK(AvoidsBox(pts, n, 9.85f, -12, 10.15f, 0));
    for (int i = 0; i < n; ++i) { CHECK(std::fabs(pts[i * 3 + 1]) < 0.1f); }
    // Straight across the low curb.
    float a[3] = { -15, 0, 12 }, b[3] = { -5, 0, 12 };
    n = FlyNative_NavFindPath(a, b, pts, 32, 0);
    CHECK(n == 2);
    // Through the wall's gap (it ends at z = 0): the far side of the wall is reachable only by going past the end.
    float behind[3] = { 14, 0, -8 }, front[3] = { 6, 0, -8 };
    n = FlyNative_NavFindPath(front, behind, pts, 32, 0);
    CHECK(n >= 3 && AvoidsBox(pts, n, 9.85f, -12, 10.15f, 0));
    // A goal a little way off the ground (just inside the crate's edge) is moved to the nearest walkable ground; one buried
    // deep inside the crate has no ground near it.
    float edge[3] = { 1.5f, 0, 0 }, buried[3] = { 0, 0, 0 };
    CHECK(FlyNative_NavFindPath(from, edge, pts, 32, 0) != 0);
    CHECK(FlyNative_NavFindPath(from, buried, pts, 32, 0) == 0);

    // Random points are on the map; nearest snaps onto it.
    float c[3] = { 5, 0, 5 }, r[3];
    for (int i = 0; i < 10; ++i) { CHECK(FlyNative_NavRandomPoint(c, 6.0f, r) && FlyNative_NavIsWalkable(r)); }
    float high[3] = { 5, 1.0f, 5 }, snapped[3];
    CHECK(FlyNative_NavNearest(high, snapped) && std::fabs(snapped[1]) < 0.01f);

    // A bigger level: 160 x 160 m at 0.5 m is 100k columns. Only to see the bake time.
    {
        const float big[13] = { -80, -2, -80, 80, 10, 80, 0.5f, 0.4f, 1.8f, 0.5f, 45, 0, 0 };
        const double t1 = GetTime();
        CHECK(FlyNative_NavBuild(big));
        std::printf("bake of %zu columns took %.0f ms (%zu cells)\n", (size_t)(320 * 320), (GetTime() - t1) * 1000.0, nav::World().mesh.CellCount());
        const double t2 = GetTime();
        float far[3] = { 75, 0, 75 }, near[3] = { -75, 0, -75 };
        int paths = 0;
        for (int i = 0; i < 20; ++i) paths += FlyNative_NavFindPath(near, far, pts, 32, 0) > 0;
        std::printf("20 long searches took %.1f ms (%d found)\n", (GetTime() - t2) * 1000.0, paths);
        CHECK(paths == 20);
    }

    // Clearing, and a play restart (ResetWorld), forgets the map.
    FlyNative_NavClear();
    CHECK(!FlyNative_NavReady() && FlyNative_NavFindPath(from, to, pts, 32, 0) == 0);

    std::printf(g_fail ? "navigation_engine_test: %d FAILED\n" : "navigation_engine_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
