// Terrain integration: the city snaps to a hilly BasicTerrain (node heights from the ground, with a grade limit),
// and the terrain can be reshaped under the city so it never pokes through roads or pads. Renders before/after
// pictures. Opens a window, so it is not registered with ctest: build/tests/city_terrain_test
#include "raylib.h"
#include "raymath.h"
#include "Engine.hpp"
#include "Engine/Graphics.hpp"
#include "../include/CityGen/City.hpp"
#include "../include/Terrain/BasicTerrain.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// Terrain surface above the city surface: counts sample points of the city's road/pad triangles where it does.
static int Pokes(const City& c, const BasicTerrain& t, float* worst) {
    std::vector<Vector3> v; std::vector<float> lay;
    c.DebugSurfaceTriangles(v); c.DebugSurfaceLayers(lay);
    int pokes = 0, samples = 0;
    *worst = 0.0f;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        if (lay[i / 3] > 0.13f || lay[i / 3] < 0.0f) continue;
        const Vector3 &a = v[i], &b = v[i + 1], &d = v[i + 2];
        const Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(d, a));
        if (Vector3Length(n) < 1e-6f || fabsf(n.y) / Vector3Length(n) < 0.5f) continue;
        for (int k = 0; k < 4; k++) {
            const float w[4][3] = { { 0.34f, 0.33f, 0.33f }, { 0.7f, 0.15f, 0.15f }, { 0.15f, 0.7f, 0.15f }, { 0.15f, 0.15f, 0.7f } };
            const float x = a.x * w[k][0] + b.x * w[k][1] + d.x * w[k][2], z = a.z * w[k][0] + b.z * w[k][1] + d.z * w[k][2];
            const float y = a.y * w[k][0] + b.y * w[k][1] + d.y * w[k][2];
            const float ty = t.position.y + t.GetHeightAt(x, z);
            samples++;
            if (ty > y + 0.02f) { pokes++; *worst = std::max(*worst, ty - y); }
        }
    }
    return pokes;
}

int main() {
    Engine engine(900, 560, "city_terrain_test", 60);
    engine.SetPlayerBuild(true);
    gfx::SetTimeOfDay(12.0f);
    auto terrainOwner = std::make_unique<BasicTerrain>(64, 64, 8.0f, 100.0f);
    BasicTerrain& terrain = *terrainOwner;
    engine.AddEntity(std::move(terrainOwner));
    terrain.GenerateFlat(0.0f);
    {   // rolling hills, 0..22 m
        float* h = terrain.GetHeightData();
        for (int z = 0; z < 64; z++) for (int x = 0; x < 64; x++)
            h[z * 64 + x] = 11.0f + 7.0f * sinf((float)x * 0.22f) + 4.0f * cosf((float)z * 0.27f + (float)x * 0.08f);
        terrain.MarkAllDirty();
    }
    auto cityOwner = std::make_unique<City>();
    City& c = *cityOwner;
    engine.AddEntity(std::move(cityOwner));
    c.GetParams().gridX = 6; c.GetParams().gridZ = 6;
    c.GetParams().cars = 0;
    c.GenerateGrid({ 0.0f, 0.0f });

    Camera3D& cam = engine.GetCamera();
    cam.position = { -140.0f, 120.0f, 200.0f };
    cam.target = { 0.0f, 12.0f, 0.0f };
    cam.up = { 0.0f, 1.0f, 0.0f };
    cam.fovy = 55.0f;
    auto shot = [&](const char* name) { for (int f = 0; f < 6; f++) engine.StepFrame(1.0f / 60.0f); TakeScreenshot(name); };

    // Before: a flat city (height 0) buried in the terrain.
    shot("city_terrain_0_flat.png");

    // Snap: every node takes the terrain height (+ offset).
    const int snapped = c.SnapToTerrain(terrain, 0.3f, 0.0f);
    std::printf("snapped %d of %zu nodes\n", snapped, c.GetNodes().size());
    CHECK(snapped == (int)c.GetNodes().size());
    float worstOff = 0.0f;
    for (const RoadNode& n : c.GetNodes()) worstOff = std::max(worstOff, fabsf(n.h - (terrain.position.y + terrain.GetHeightAt(n.pos.x, n.pos.y) + 0.3f)));
    std::printf("largest node height error against the terrain: %.4f m\n", worstOff);
    CHECK(worstOff < 0.01f);
    float worst = 0.0f;
    const int pokesSnapped = Pokes(c, terrain, &worst);
    std::printf("terrain above the roads after snapping only: %d sample points (worst %.2f m)\n", pokesSnapped, worst);
    shot("city_terrain_1_snapped.png");

    // Grade limit: relax heights to at most 8 %.
    {
        const float steepest = c.LimitRoadGrades(8.0f);
        std::printf("steepest road grade after the 8 %% limit: %.1f %%\n", steepest);
        CHECK(steepest < 9.0f);
        c.SnapToTerrain(terrain, 0.3f, 0.0f);      // back to the raw snap for the shaping test
    }

    // Shape the terrain to the city.
    std::vector<float> before(terrain.GetHeightData(), terrain.GetHeightData() + 64 * 64);
    const int changed = c.ShapeTerrainToCity(terrain, 0.3f, 16.0f);
    float worstAfter = 0.0f;
    const int pokesAfter = Pokes(c, terrain, &worstAfter);
    std::printf("terrain shaped: %d vertices changed; terrain above the roads: %d sample points (worst %.2f m)\n", changed, pokesAfter, worstAfter);
    CHECK(changed > 50);
    CHECK(pokesAfter == 0);
    CHECK(pokesSnapped > 0);               // the test terrain does poke through before shaping
    // Far from the city nothing changed.
    CHECK(terrain.GetHeightData()[0] == before[0]);
    CHECK(terrain.GetHeightData()[64 * 64 - 1] == before[64 * 64 - 1]);
    shot("city_terrain_2_shaped.png");

    std::printf(g_fail ? "city_terrain_test: %d FAILED\n" : "city_terrain_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
