// Parks and plazas: painted park blocks get paths, a fountain (big parks), benches, lamps and trees; plaza blocks are paved
// with a fountain, a ring of benches, lamps and corner trees. Checks the props exist (flat and on hills), that plaza kinds
// survive save/load, and renders pictures (city_park_<name>.png). Opens a window, so it is not registered with ctest:
// build/tests/city_park_test
#include "raylib.h"
#include "raymath.h"
#include "Engine.hpp"
#include "Engine/Graphics.hpp"
#include "../include/CityGen/City.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>
#include <vector>

using namespace city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void Shot(Engine& engine, Vector3 pos, Vector3 target, const char* path) {
    Camera3D& cam = engine.GetCamera();
    cam.position = pos; cam.target = target; cam.up = { 0.0f, 1.0f, 0.0f }; cam.fovy = 55.0f;
    if (fabsf(pos.x - target.x) + fabsf(pos.z - target.z) < 1.0f) cam.up = { 0.0f, 0.0f, -1.0f };
    for (int f = 0; f < 8; f++) engine.StepFrame(1.0f / 60.0f);
    TakeScreenshot(path);
}

static Vector2 BlockMid(const City& c, const Block& b) {
    Vector2 m{ 0.0f, 0.0f };
    for (int n : b.nodes) { m.x += c.GetNodes()[(size_t)n].pos.x; m.y += c.GetNodes()[(size_t)n].pos.y; }
    return Vector2Scale(m, 1.0f / (float)b.nodes.size());
}

int main() {
    Engine engine(900, 640, "city_park_test", 60);
    engine.SetPlayerBuild(true);
    gfx::SetTimeOfDay(12.0f);
    auto owner = std::make_unique<City>();
    City& c = *owner;
    engine.AddEntity(std::move(owner));
    c.GetParams().gridX = 6; c.GetParams().gridZ = 6; c.GetParams().cars = 0;
    c.GenerateGrid({ 0.0f, 0.0f });

    const int fountains0 = c.CountInstances(kPropFountain), benches0 = c.CountInstances(kPropBench), trees0 = c.CountInstances(kPropTree);
    // Paint three blocks near the middle: a park, a plaza and another park.
    std::vector<uint64_t> ids;
    {
        std::vector<std::pair<float, uint64_t>> byDist;
        for (const Block& b : c.GetBlocks()) byDist.push_back({ Vector2Length(BlockMid(c, b)), b.id });
        std::sort(byDist.begin(), byDist.end());
        for (size_t i = 0; i < 3 && i < byDist.size(); i++) ids.push_back(byDist[i].second);
    }
    CHECK(ids.size() == 3);
    c.SetBlockKind(ids[0], BlockKind::Park);
    c.SetBlockKind(ids[1], BlockKind::Plaza);
    c.SetBlockKind(ids[2], BlockKind::Park);
    const int fountains1 = c.CountInstances(kPropFountain), benches1 = c.CountInstances(kPropBench), trees1 = c.CountInstances(kPropTree);
    std::printf("fountains %d -> %d, benches %d -> %d, trees %d -> %d, lamps %d\n", fountains0, fountains1, benches0, benches1, trees0, trees1, c.CountInstances(kPropLamp));
    CHECK(fountains1 > fountains0);
    CHECK(benches1 > benches0 + 4);
    CHECK(trees1 > trees0 + 4);
    int plazas = 0, parks = 0;
    for (const Block& b : c.GetBlocks()) { if (b.plaza) plazas++; else if (b.park) parks++; }
    std::printf("park blocks %d, plaza blocks %d\n", parks, plazas);
    CHECK(plazas == 1);

    // Pictures: from above and at a slant over each painted block.
    for (size_t i = 0; i < ids.size(); i++) {
        for (const Block& b : c.GetBlocks()) if (b.id == ids[i]) {
            const Vector2 m = BlockMid(c, b);
            Shot(engine, { m.x, 90.0f, m.y + 0.01f }, { m.x, 0.0f, m.y }, TextFormat("city_park_top_%d.png", (int)i));
            Shot(engine, { m.x + 30.0f, 14.0f, m.y + 30.0f }, { m.x, 1.5f, m.y }, TextFormat("city_park_view_%d.png", (int)i));
        }
    }

    // Save / load: the plaza is still a plaza.
    {
        std::stringstream ss;
        CHECK(c.WriteToStream(ss));
        City other;
        CHECK(other.ReadFromStream(ss));
        CHECK(other.GetBlockKind(ids[1]) == BlockKind::Plaza);
        CHECK(other.GetBlockKind(ids[0]) == BlockKind::Park);
    }

    // On hills too.
    for (int i = 0; i < (int)c.GetNodes().size(); i++) {
        const Vector2 p = c.GetNodes()[(size_t)i].pos;
        c.SetNodeHeight(i, 4.0f + 3.0f * sinf(p.x * 0.05f) + 3.0f * cosf(p.y * 0.06f));
    }
    CHECK(c.CountInstances(kPropFountain) == fountains1);
    for (const Block& b : c.GetBlocks()) if (b.id == ids[1]) {
        const Vector2 m = BlockMid(c, b);
        Shot(engine, { m.x + 26.0f, 18.0f, m.y + 26.0f }, { m.x, 5.0f, m.y }, "city_park_hills_plaza.png");
    }
    for (const Block& b : c.GetBlocks()) if (b.id == ids[0]) {
        const Vector2 m = BlockMid(c, b);
        Shot(engine, { m.x - 30.0f, 20.0f, m.y + 30.0f }, { m.x, 5.0f, m.y }, "city_park_hills_park.png");
    }

    // People visit the parks: a crowd walks around the painted blocks for a while; some walk in, sit and walk back.
    {
        c.GetParams().pedestrians = 120;
        SetTrafficRunning(true);
        int maxIn = 0;
        for (int i = 0; i < 6000; i++) {
            c.TrafficStepForTest(0.1f);
            if (i % 20 == 0) maxIn = std::max(maxIn, c.GetTrafficStats().inParks);
        }
        const City::TrafficStats ts = c.GetTrafficStats();
        std::printf("pedestrians: %d walking, %d park visits so far, %d in parks now (most at once %d), %d trips\n", ts.peds, ts.parkVisits, ts.inParks, maxIn, ts.pedTrips);
        CHECK(ts.parkVisits >= 5);
        CHECK(maxIn >= 1);
        // A picture with someone in a park.
        for (int i = 0; i < 4000 && c.GetTrafficStats().inParks == 0; i++) c.TrafficStepForTest(0.1f);
        Vector3 pp; float yaw;
        int idx = 0;
        bool shot = false;
        while (c.FindWalkingPed(idx++, pp, yaw)) {
            bool inPark = false;
            for (const Block& b : c.GetBlocks()) if (b.park && !b.parkPoly.empty()) {
                Vector2 m = BlockMid(c, b);
                if (Vector2Distance({ pp.x, pp.z }, m) < 12.0f) inPark = true;
            }
            if (inPark) { Shot(engine, { pp.x + 9.0f, pp.y + 5.0f, pp.z + 9.0f }, { pp.x, pp.y + 1.0f, pp.z }, "city_park_visitor.png"); shot = true; break; }
        }
        std::printf("visitor picture: %s\n", shot ? "taken" : "nobody in a park right now");
        SetTrafficRunning(false);
        c.GetParams().pedestrians = 0;
    }

    std::printf(g_fail ? "city_park_test: %d FAILED\n" : "city_park_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
