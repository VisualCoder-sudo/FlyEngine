// `Flyengine --testscene [frames]`: renders a scene that exercises the paths the
// water test does not -- BasicTerrain with every sculpt/paint tool (so the
// splatmap texture and the terrain mesh are rebuilt repeatedly), a generated
// city (roads, instanced buildings, shadows), and a mix of lit shapes. Meant to
// be run from a Debug build, where sokol's validation layer aborts on any
// invalid GPU usage; it also saves screenshots to scene_*.png for a visual check.
#include "../include/Engine.hpp"
#include "../include/Engine/Graphics.hpp"
#include "../include/Engine/Frontend/ui.hpp"
#include "../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../include/Engine/Backend/ScatteredObject.hpp"
#include "../include/Terrain/BasicTerrain.hpp"
#include "../include/CityGen/City.hpp"
#include "../include/CityGen/CityGeometry.hpp"
#include "raylib.h"
#include "raymath.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace scenetest {

namespace {
struct ScopedUI {
    ScopedUI() { ui::Init(); }
    ~ScopedUI() noexcept { ui::Unload(); }
};
std::vector<ScatteredObject*> g_objects;
std::vector<std::unique_ptr<ModelGroup>> g_models;
}

int Run(int argc, char** argv) {
    int frames = argc > 2 ? std::atoi(argv[2]) : 360;
    const bool cityCloseup = argc > 3 && std::string(argv[3]) == "city";
    if (frames < 120) frames = 120;
    std::printf("[scene] %d frames\n", frames);

    Engine engine(1280, 720, "Flyengine Scene Test");
    engine.SetClearColor(Color{ 90, 120, 160, 255 });
    const ScopedUI uiScope;
    ui::SetSceneObjects(&g_objects);
    ui::SetSceneModels(&g_models);
    auto sim = std::make_unique<phys::Simulation>(g_objects);
    ui::SetSimulation(sim.get());
    engine.AddEntity(std::move(sim));

    // Terrain: painted and sculpted every frame below.
    auto terrainOwner = std::make_unique<BasicTerrain>(128, 128, 2.0f, 40.0f, 16.0f);
    BasicTerrain* terrain = terrainOwner.get();
    terrain->GenerateFlat(0.0f);
    engine.AddEntity(std::move(terrainOwner));

    // City next to it.
    auto cityOwner = std::make_unique<city::City>();
    city::City* cityPtr = cityOwner.get();
    const bool parkMode = argc > 3 && std::string(argv[3]) == "park";
    if (parkMode) {
        // Force parks, and make the node layout uneven so the park outlines are irregular.
        cityPtr->GetParams().parkThreshold = 100.0f;
        cityPtr->GetParams().parkInset = 3.0f;
    }
    cityPtr->GenerateGrid(Vector2{ 260.0f, 0.0f });
    if (parkMode) {
        auto& nodes = cityPtr->GetNodes();
        const bool noJitter = std::getenv("FLY_NOJITTER") != nullptr;   // perfect grid
        for (size_t i = 0; i < nodes.size() && !noJitter; ++i) {
            const float jx = std::sin((float)i * 12.9898f) * 9.0f, jz = std::cos((float)i * 78.233f) * 9.0f;
            nodes[i].pos.x += jx;
            nodes[i].pos.y += jz;
        }
        // Merge cells: deleting interior nodes leaves oversized, irregular blocks (parks).
        const int mid = (int)cityPtr->GetNodes().size() / 2;
        cityPtr->DeleteNode(mid + 1);
        cityPtr->DeleteNode(mid - 2);
        if (noJitter) { cityPtr->DeleteNode(mid + 2); cityPtr->DeleteNode(mid - 1); cityPtr->DeleteNode(mid + 8); }
        cityPtr->RebuildAll();
    }
    engine.AddEntity(std::move(cityOwner));

    // A few lit shapes of every kind.
    const ShapeType shapes[] = { ShapeType::Cube, ShapeType::Sphere, ShapeType::Cylinder, ShapeType::Wedge };
    for (int i = 0; i < 12; ++i) {
        auto obj = std::make_unique<ScatteredObject>(
            Vector3{ -40.0f + 7.0f * (float)i, 3.0f, 60.0f }, Vector3{ 4, 4, 4 },
            Color{ (unsigned char)(60 + i * 15), 140, (unsigned char)(220 - i * 12), 255 }, shapes[i % 4]);
        g_objects.push_back(obj.get());
        engine.AddEntity(std::move(obj));
    }

    if (parkMode) {
        // "Incremental rebuild is identical to a full rebuild": move each node by several
        // offsets with the incremental path, then rebuild the same graph from scratch
        // and compare the generated geometry.
        const Vector2 deltas[] = { { 2.0f, 1.0f }, { -6.0f, 4.0f }, { 14.0f, -9.0f }, { -20.0f, -18.0f } };
        int mismatches = 0, checks = 0;
        for (int ni = 0; ni < (int)cityPtr->GetNodes().size(); ++ni) {
            const Vector2 home = cityPtr->NodePos(ni);
            for (const Vector2& d : deltas) {
                cityPtr->MoveNode(ni, Vector2{ home.x + d.x, home.y + d.y }, true);   // incremental
                const auto inc = cityPtr->DebugGeometryHashes();
                cityPtr->RebuildAll();                                               // from scratch
                const auto full = cityPtr->DebugGeometryHashes();
                ++checks;
                if (inc.road != full.road || inc.buildings != full.buildings ||
                    inc.roadTris != full.roadTris || inc.instances != full.instances) {
                    if (mismatches < 10)
                        std::printf("[park] node %d moved (%.0f,%.0f): incremental differs (roadTris %zu vs %zu, instances %zu vs %zu, roads %s, buildings %s)\n",
                                    ni, d.x, d.y, inc.roadTris, full.roadTris, inc.instances, full.instances,
                                    inc.road == full.road ? "same" : "DIFFERENT", inc.buildings == full.buildings ? "same" : "DIFFERENT");
                    ++mismatches;
                }
            }
            cityPtr->MoveNode(ni, home, false);
            cityPtr->RebuildAll();
        }
        std::printf("[park] %d of %d moves differ between incremental and full rebuild\n", mismatches, checks);
    }
    if (parkMode) {
        // Real outlines come out of the road graph. After many random drags, every block's
        // pad (and park) must be fully covered by its triangulation; a missing ear leaves a hole.
        auto coverage = [](const std::vector<Vector2>& poly, float& sum, bool& cw) {
            std::vector<int> tris;
            citygeom::TriangulateSimple(poly, tris);
            sum = 0.0f; cw = false;
            for (size_t t = 0; t + 2 < tris.size(); t += 3) {
                const Vector2 &a = poly[(size_t)tris[t]], &b = poly[(size_t)tris[t + 1]], &c = poly[(size_t)tris[t + 2]];
                const float ar = 0.5f * ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
                sum += ar;
                if (ar < -1e-3f) cw = true;
            }
        };
        unsigned seed = 4242;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.0f; };
        int bad = 0, blocksChecked = 0, shown = 0, selfCrossing = 0;
        for (int iter = 0; iter < 400; ++iter) {
            const int ni = (int)(rnd() * (float)cityPtr->GetNodes().size());
            const Vector2 p = cityPtr->NodePos(ni);
            cityPtr->MoveNode(ni, Vector2{ p.x + (rnd() - 0.5f) * 50.0f, p.y + (rnd() - 0.5f) * 50.0f }, true);
            for (const auto& b : cityPtr->GetBlocks()) {
                std::vector<Vector2> poly;
                for (int idx : b.nodes) poly.push_back(cityPtr->NodePos(idx));
                for (int which = 0; which < 2; ++which) {
                    const std::vector<Vector2>& pg = which == 0 ? poly : b.parkPoly;
                    if (pg.size() < 3 || (which == 1 && !b.park)) continue;
                    if (!citygeom::IsSimplePolygonForTest(pg)) { ++selfCrossing; continue; }   // dragged across a road
                    ++blocksChecked;
                    float sum; bool cw;
                    coverage(pg, sum, cw);
                    const float want = citygeom::PolygonArea(pg);
                    if (cw || std::fabs(sum - want) > 0.01f * std::fabs(want) + 0.05f) {
                        ++bad;
                        if (shown++ < 4) {
                            std::printf("[park] iter %d: %s of block %llu: triangles cover %.1f of %.1f m^2%s, %zu vertices:", iter,
                                        which == 0 ? "pad" : "park grass", (unsigned long long)b.id, sum, want, cw ? " (clockwise tri)" : "", pg.size());
                            for (const auto& v : pg) std::printf(" (%.2f,%.2f)", v.x, v.y);
                            std::printf("\n");
                        }
                    }
                }
            }
        }
        // Park grass sanity: the inset outline must stay inside the block and keep roughly
        // (area - perimeter*inset) of it; a star-shaped or pinched grass outline is wrong.
        {
            int weird = 0, parks = 0, shownW = 0;
            for (int iter = 0; iter < 600; ++iter) {
                const int ni = (int)(rnd() * (float)cityPtr->GetNodes().size());
                const Vector2 p = cityPtr->NodePos(ni);
                cityPtr->MoveNode(ni, Vector2{ p.x + (rnd() - 0.5f) * 30.0f, p.y + (rnd() - 0.5f) * 30.0f }, true);
                for (const auto& b : cityPtr->GetBlocks()) {
                    if (!b.park || b.parkPoly.size() < 3) continue;
                    std::vector<Vector2> poly;
                    for (int idx : b.nodes) poly.push_back(cityPtr->NodePos(idx));
                    if (!citygeom::IsSimplePolygonForTest(poly)) continue;
                    ++parks;
                    float per = 0.0f;
                    for (size_t i = 0; i < poly.size(); ++i) {
                        const Vector2 d{ poly[(i + 1) % poly.size()].x - poly[i].x, poly[(i + 1) % poly.size()].y - poly[i].y };
                        per += std::sqrt(d.x * d.x + d.y * d.y);
                    }
                    const float a0 = std::fabs(citygeom::PolygonArea(poly)), a1 = std::fabs(citygeom::PolygonArea(b.parkPoly));
                    const float expect = a0 - per * cityPtr->GetParams().parkInset;
                    bool outside = false;
                    for (const auto& v : b.parkPoly) if (!citygeom::PointInPolygon(v, poly)) outside = true;
                    const bool sameAsOutline = b.parkPoly.size() == poly.size() && std::fabs(a1 - a0) < 1e-3f;
                    if (outside || a1 < expect * 0.8f || (a1 > a0 * 1.001f)) {
                        ++weird;
                        if (shownW++ < 3) {
                            std::printf("[park] weird grass (block %llu): block area %.0f, grass area %.0f, expected ~%.0f%s%s; block:",
                                        (unsigned long long)b.id, a0, a1, expect, outside ? ", grass vertex outside block" : "", sameAsOutline ? ", grass == block outline" : "");
                            for (const auto& v : poly) std::printf(" (%.2f,%.2f)", v.x, v.y);
                            std::printf("\n[park]   grass:");
                            for (const auto& v : b.parkPoly) std::printf(" (%.2f,%.2f)", v.x, v.y);
                            std::printf("\n");
                        }
                    }
                }
            }
            std::printf("[park] park grass outlines: %d of %d look wrong\n", weird, parks);
        }
        // A real drag is a long sequence of incremental rebuilds. After every step the live
        // (incrementally updated) city must match a copy rebuilt from scratch.
        {
            unsigned seed2 = 99;
            auto rnd2 = [&]() { seed2 = seed2 * 1664525u + 1013904223u; return (float)(seed2 >> 8) / 16777216.0f; };
            int diverged = 0, steps = 0, shownD = 0;
            for (int drag = 0; drag < 25; ++drag) {
                const int ni = (int)(rnd2() * (float)cityPtr->GetNodes().size());
                const Vector2 start = cityPtr->NodePos(ni);
                const Vector2 target{ start.x + (rnd2() - 0.5f) * 70.0f, start.y + (rnd2() - 0.5f) * 70.0f };
                for (int k = 1; k <= 12; ++k) {                       // 12 mouse-move steps
                    const float t = (float)k / 12.0f;
                    cityPtr->MoveNode(ni, Vector2{ start.x + (target.x - start.x) * t, start.y + (target.y - start.y) * t }, true);
                    std::stringstream ss;
                    cityPtr->WriteToStream(ss);
                    city::City fresh;
                    fresh.ReadFromStream(ss);
                    fresh.RebuildAll();
                    const auto a = cityPtr->DebugGeometryHashes(), b = fresh.DebugGeometryHashes();
                    ++steps;
                    if (a.road != b.road || a.buildings != b.buildings || a.roadTris != b.roadTris || a.instances != b.instances) {
                        ++diverged;
                        if (shownD++ < 5)
                            std::printf("[park] drag %d step %d (node %d): live city differs from a fresh rebuild: roadTris %zu vs %zu, instances %zu vs %zu\n",
                                        drag, k, ni, a.roadTris, b.roadTris, a.instances, b.instances);
                    }
                }
            }
            std::printf("[park] drag sequences: %d of %d steps differ from a fresh rebuild\n", diverged, steps);
        }
        std::printf("[park] triangulation coverage: %d of %d valid outlines incomplete (%d self-crossing outlines skipped)\n", bad, blocksChecked, selfCrossing);
    }
    gfx::SetShadowsEnabled(!(argc > 4 && std::string(argv[4]) == "noshadow"));
    ui::SetPlayActive(false);

    const BasicTerrain::Tool tools[] = {
        BasicTerrain::Tool::Raise, BasicTerrain::Tool::Paint, BasicTerrain::Tool::Smooth,
        BasicTerrain::Tool::Lower, BasicTerrain::Tool::Paint, BasicTerrain::Tool::Noise,
        BasicTerrain::Tool::Flatten, BasicTerrain::Tool::Erode, BasicTerrain::Tool::Ramp,
    };

    for (int f = 0; f < frames; ++f) {
        // Brush the terrain: a different tool every 20 frames, painting layers in turn.
        BasicTerrain::Brush brush;
        brush.tool = tools[(f / 20) % 9];
        brush.paintLayer = (f / 7) % 4;
        brush.paintErase = ((f / 50) % 4) == 3;
        brush.radius = 12.0f + 10.0f * std::sin(f * 0.05f);
        brush.strength = 40.0f;
        terrain->ApplyBrush(brush, Vector2{ 40.0f * std::cos(f * 0.04f), 40.0f * std::sin(f * 0.04f) }, 1.0f / 60.0f);

        // Orbit across terrain, city and shapes.
        const float ang = f * 0.012f;
        Camera3D& cam = engine.GetCamera();
        if (parkMode) {
            // Look straight down at the largest park.
            Vector2 c{ 260.0f, 0.0f };
            float best = 0.0f;
            for (const auto& b : cityPtr->GetBlocks()) {
                if (!b.park || b.area <= best) continue;
                best = b.area;
                c = { 0, 0 };
                for (int idx : b.nodes) { c.x += cityPtr->NodePos(idx).x; c.y += cityPtr->NodePos(idx).y; }
                c.x /= (float)b.nodes.size(); c.y /= (float)b.nodes.size();
            }
            if (f == 0) std::printf("[scene] largest park area=%.0f at (%.1f, %.1f)\n", best, c.x, c.y);
            cam.position = Vector3{ c.x, 95.0f, c.y + 22.0f };
            cam.target = Vector3{ c.x, 0.0f, c.y - 8.0f };
        } else if (argc > 3 && std::string(argv[3]) == "terrain") {
            cam.position = Vector3{ 70.0f, 70.0f, 70.0f };
            cam.target = Vector3{ 0.0f, 0.0f, 0.0f };
        } else if (argc > 3 && std::string(argv[3]) == "citynear") {
            cam.position = Vector3{ 262.0f, 14.0f, 40.0f };
            cam.target = Vector3{ 258.0f, 8.0f, 10.0f };
        } else if (cityCloseup) {
            cam.position = Vector3{ 330.0f, 45.0f, 70.0f };
            cam.target = Vector3{ 260.0f, 8.0f, 10.0f };
        } else {
            cam.position = Vector3{ 130.0f + std::cos(ang) * 190.0f, 90.0f + 25.0f * std::sin(ang * 2.0f), 30.0f + std::sin(ang) * 190.0f };
            cam.target = Vector3{ 110.0f, 0.0f, 20.0f };
        }
        cam.up = Vector3{ 0, 1, 0 };
        cam.fovy = 55.0f;
        cam.projection = CAMERA_PERSPECTIVE;

        if (parkMode && f == 60) {
            // Emulate a node drag release: the incremental rebuild path.
            const int ni = (int)cityPtr->GetNodes().size() / 2;
            Vector2 p = cityPtr->NodePos(ni);
            cityPtr->MoveNode(ni, Vector2{ p.x + 0.5f, p.y }, false);
            cityPtr->RebuildAfterNodeMove(ni);
        }
        // "sky": the atmosphere sky with clouds, and a walk through the panels that set it (each
        // Lighting item's settings, then Preferences > Rendering), so that they are all drawn once
        // under the validation layer. Pictures: sky_panel_<n>.png.
        const bool skyMode = argc > 3 && std::string(argv[3]) == "sky";
        if (skyMode) {
            gfx::LightingSettings& L = gfx::Lighting();
            L.hasSky = true; L.skyMode = 1; L.hasClouds = true; L.hasFog = true; L.fogDensity = 0.002f; L.fogHeight = 120.0f;
            L.hasWeather = true; L.hasPicture = true; L.hasSun = true; L.hasAmbient = true; L.timeOfDay = 16.5f;
            const int step = f / 12;            // a new panel every 12 frames
            if (step >= 1 && step <= 8) { ui::ShowPreferences(false, 0); ui::SelectLightingItem(step); }
            else if (step == 9) { ui::SelectLightingItem(0); ui::ShowPreferences(true, 4); }
            else if (step == 10) ui::ShowPreferences(false, 0);
            // The bottom panel: the Output tab, back to Assets, and then a new line while Assets is showing (the red dot).
            else if (step == 11) ui::SelectBottomTab(1);
            else if (step == 12) ui::SelectBottomTab(0);
            else if (step == 13 && f % 12 == 0) ui::LogAlways("[test] something happened while the Assets tab was open");
        }
        engine.StepFrame(1.0f / 60.0f);
        if (skyMode && f % 12 == 10 && f / 12 >= 1 && f / 12 <= 14) {
            char name[64];
            std::snprintf(name, sizeof(name), "sky_panel_%d.png", f / 12);
            TakeScreenshot(name);
        }
        if (parkMode && (f == 50 || f == 110)) {
            char name[64];
            std::snprintf(name, sizeof(name), "park_%03d.png", f);
            TakeScreenshot(name);
        }

        if (f == frames / 4 || f == frames / 2 || f == frames - 1) {
            char name[64];
            std::snprintf(name, sizeof(name), "scene_%03d.png", f);
            TakeScreenshot(name);
        }
    }
    std::printf("[scene] done\n");
    return 0;
}

} // namespace scenetest
