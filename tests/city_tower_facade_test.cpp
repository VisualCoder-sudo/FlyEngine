// Stepped towers (narrower as they rise) on raised ground: renders close-ups of a tall tower's ledges so the floor
// grid (windows, slab bands) can be checked against each tier (city_tower_facade_<n>.png). Opens a window, so it is not
// registered with ctest: build/tests/city_tower_facade_test
#include "raylib.h"
#include "raymath.h"
#include "Engine.hpp"
#include "Engine/Graphics.hpp"
#include "../include/CityGen/City.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
    Engine engine(900, 700, "city_tower_facade_test", 60);
    engine.SetPlayerBuild(true);
    gfx::SetTimeOfDay(12.0f);
    auto owner = std::make_unique<City>();
    City& c = *owner;
    engine.AddEntity(std::move(owner));
    c.GetParams().gridX = 6; c.GetParams().gridZ = 6; c.GetParams().cars = 0;
    c.GenerateGrid({ 0.0f, 0.0f });
    for (int i = 0; i < (int)c.GetNodes().size(); i++) {
        const Vector2 p = c.GetNodes()[(size_t)i].pos;
        c.SetNodeHeight(i, 6.0f + 4.0f * sinf(p.x * 0.045f) + 4.0f * cosf(p.y * 0.05f + p.x * 0.02f));
    }
    // A few lone towers on blocks of the hills (placed buildings keep their own footprint, the rest of the block stays low).
    for (size_t bi = 4; bi < c.GetBlocks().size() && c.GetPlacedBuildings().size() < 3; bi += 3) {
        const Block& blk = c.GetBlocks()[bi];
        if (blk.park || blk.inset.empty()) continue;
        Vector2 ctr = { 0.0f, 0.0f };
        for (const Vector2& v : blk.inset) { ctr.x += v.x; ctr.y += v.y; }
        ctr = Vector2Scale(ctr, 1.0f / (float)blk.inset.size());
        PlacedBuilding pb;
        pb.center = ctr; pb.sizeX = 14.0f; pb.sizeZ = 14.0f; pb.height = 44.0f; pb.shape = 4; pb.colorBucket = (int)bi;
        c.AddPlacedBuilding(pb);
    }
    std::vector<const Building*> towers;
    for (const Block& blk : c.GetBlocks()) for (const Building& b : blk.buildings) if (b.shape == 4 && b.placedIndex >= 0) towers.push_back(&b);
    std::printf("placed towers: %zu\n", towers.size());
    CHECK(!towers.empty());
    std::sort(towers.begin(), towers.end(), [](const Building* a, const Building* b) { return a->foundation > b->foundation; });
    int shot = 0;
    for (int k = 0; k < 2 && k < (int)towers.size(); k++) {
        const Building& b = *towers[(size_t)k];
        std::printf("tower %d: %.1f x %.1f x %.1f, foundation %.2f m\n", k, b.size.x, b.size.y, b.size.z, b.foundation);
        const float baseY = b.center.y - b.size.y * 0.5f;
        for (int a = 0; a < 3; a++) {
            const float ang = b.angleY + 0.8f + (float)a * 2.1f;
            Camera3D& cam = engine.GetCamera();
            const float dist = b.size.y * 1.3f + 22.0f;
            cam.position = { b.center.x + sinf(ang) * dist, baseY + b.size.y * 0.55f, b.center.z + cosf(ang) * dist };
            cam.target = { b.center.x, baseY + b.size.y * 0.45f, b.center.z };
            cam.up = { 0.0f, 1.0f, 0.0f };
            cam.fovy = 50.0f;
            for (int f = 0; f < 8; f++) engine.StepFrame(1.0f / 60.0f);
            TakeScreenshot(TextFormat("city_tower_facade_%d.png", shot++));
        }
    }
    std::printf(g_fail ? "city_tower_facade_test: %d FAILED\n" : "city_tower_facade_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
