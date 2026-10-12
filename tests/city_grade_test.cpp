// Raising a road node with the editor's smooth height: the node stays where it was put, its neighbours follow so no ramp
// is steeper than the city's Max road grade, and the surface is much gentler than a lone raised node. Also checks the
// setting survives save/load. Opens a window (the city meshes need GL); not registered with ctest: build/tests/city_grade_test
#include "raylib.h"
#include "raymath.h"
#include "Engine.hpp"
#include "Engine/Graphics.hpp"
#include "../include/CityGen/City.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>
#include <vector>

using namespace city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// Steepest up-facing road/pad triangle (rise over run, %).
static float SteepestSurface(const City& c) {
    std::vector<Vector3> v; std::vector<float> lay;
    c.DebugSurfaceTriangles(v); c.DebugSurfaceLayers(lay);
    float worst = 0.0f;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        if (lay[i / 3] > 0.13f) continue;
        const Vector3 n = Vector3CrossProduct(Vector3Subtract(v[i + 1], v[i]), Vector3Subtract(v[i + 2], v[i]));
        const float nl = Vector3Length(n);
        if (nl < 1e-6f || fabsf(n.y) / nl < 0.3f) continue;
        worst = std::max(worst, sqrtf(n.x * n.x + n.z * n.z) / fabsf(n.y));
    }
    return worst * 100.0f;
}

int main() {
    Engine engine(320, 240, "city_grade_test", 60);
    engine.SetPlayerBuild(true);
    auto owner = std::make_unique<City>();
    City& c = *owner;
    engine.AddEntity(std::move(owner));
    c.GetParams().gridX = 7; c.GetParams().gridZ = 7; c.GetParams().cars = 0;
    c.GenerateGrid({ 0.0f, 0.0f });
    CHECK(c.GetParams().maxGrade == 15.0f);

    int mid = 0;
    float best = 1e9f;
    for (int i = 0; i < (int)c.GetNodes().size(); i++) { const float d = Vector2Length(c.GetNodes()[(size_t)i].pos); if (d < best) { best = d; mid = i; } }

    // A lone raised node: neighbours stay at 0 and the ramps are steep.
    c.SetNodeHeight(mid, 3.0f);
    const float steepLone = SteepestSurface(c);
    // The editor's version: the neighbours follow.
    c.SetNodeHeightSmooth(mid, 3.0f);
    const float steepSmooth = SteepestSurface(c);
    std::printf("steepest surface after raising one node 3 m: lone %.0f %%, with followers %.0f %%\n", steepLone, steepSmooth);
    CHECK(c.GetNodes()[(size_t)mid].h == 3.0f);                     // the edited node is exactly where it was put
    CHECK(steepSmooth < steepLone * 0.6f);
    float lowest = 1e9f, highest = -1e9f;
    for (const RoadNode& n : c.GetNodes()) { lowest = std::min(lowest, n.h); highest = std::max(highest, n.h); }
    std::printf("node heights now range %.2f .. %.2f m\n", lowest, highest);
    CHECK(highest == 3.0f);
    CHECK(lowest > -0.001f);                                         // the hill is local: nothing goes below the old ground
    int raised = 0;
    for (const RoadNode& n : c.GetNodes()) if (n.h > 0.001f) raised++;
    std::printf("%d of %zu nodes raised\n", raised, c.GetNodes().size());
    CHECK(raised > 1 && raised < (int)c.GetNodes().size());          // the neighbours follow, the far side of the city stays at 0
    const float again = c.LimitRoadGrades(15.0f, mid);               // already within the limit: nothing moves, steepest <= 15 %
    std::printf("steepest ramp %.2f %%\n", again);
    CHECK(again <= 15.2f);
    CHECK(c.GetNodes()[(size_t)mid].h == 3.0f);

    // Max grade 0 = off: SetNodeHeightSmooth is then a plain height change.
    c.GetParams().maxGrade = 0.0f;
    c.SetNodeHeight(mid, 0.0f);
    for (int i = 0; i < (int)c.GetNodes().size(); i++) if (i != mid) c.SetNodeHeight(i, 0.0f);
    c.SetNodeHeightSmooth(mid, 8.0f);
    int moved = 0;
    for (int i = 0; i < (int)c.GetNodes().size(); i++) if (i != mid && c.GetNodes()[(size_t)i].h != 0.0f) moved++;
    CHECK(moved == 0);

    // Save / load keeps the setting.
    c.GetParams().maxGrade = 7.0f;
    std::stringstream ss;
    CHECK(c.WriteToStream(ss));
    City other;
    CHECK(other.ReadFromStream(ss));
    std::printf("max grade after load: %.1f %%\n", other.GetParams().maxGrade);
    CHECK(other.GetParams().maxGrade == 7.0f);
    CHECK(other.GetNodes().size() == c.GetNodes().size());

    std::printf(g_fail ? "city_grade_test: %d FAILED\n" : "city_grade_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
