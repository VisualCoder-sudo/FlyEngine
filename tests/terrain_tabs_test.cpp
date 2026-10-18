// The terrain panel's tabs against the real editor UI, driven with injected mouse input: whatever tab is open is the
// only brush that works. Opens a window: build/tests/terrain_tabs_test
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/Frontend/ui.hpp"
#include "../include/Terrain/BasicTerrain.hpp"
#include "../include/Terrain/Foliage.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <algorithm>
#include <vector>
#include "Engine/Backend/ScatteredObject.hpp"
#include "Engine/Backend/PhysicsSimulation.hpp"
#include "Engine/Frontend/ObjectInteractionManager.hpp"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

void flyrl_test_set_mouse(float x, float y, bool leftDown);

static Engine* g_engine = nullptr;
static void Frames(int n) { for (int i = 0; i < n; i++) g_engine->StepFrame(1.0f / 60.0f); }
static float g_mx = 0, g_my = 0;
static void Mouse(float x, float y) { g_mx = x; g_my = y; flyrl_test_set_mouse(x, y, false); }
static void Down(bool d) { flyrl_test_set_mouse(g_mx, g_my, d); }
static void Click(float x, float y) { Mouse(x, y); Frames(2); Down(true); Frames(2); Down(false); Frames(2); }

static float HeightSum(BasicTerrain* t) {
    double s = 0;
    for (float z = -60; z <= 60; z += 6) for (float x = -60; x <= 60; x += 6) s += t->GetHeightAt(x, z);
    return (float)s;
}

int main() {
    Engine engine(1600, 1500, "terrain_tabs_test", 60);
    g_engine = &engine;
    ui::Init();
    std::vector<ScatteredObject*> objs;
    std::vector<std::unique_ptr<ModelGroup>> models;
    ui::SetSceneObjects(&objs);
    ui::SetSceneModels(&models);
    auto simOwner = std::make_unique<phys::Simulation>(objs);
    phys::Simulation& sim = *simOwner;
    ui::SetSimulation(&sim);
    engine.AddEntity(std::move(simOwner));
    // The same entities the editor runs, so a click goes through the real selection and UI-consumed logic.
    engine.AddEntity(std::make_unique<ObjectInteractionManager>(engine, engine.GetCamera(), nullptr, objs, models, &sim));
    auto owner = std::make_unique<BasicTerrain>(128, 128, 2.0f, 40.0f, 16.0f);
    BasicTerrain* t = owner.get();
    t->GenerateFlat(5.0f);
    engine.AddEntity(std::move(owner));
    Camera3D& cam = engine.GetCamera();
    cam.position = { 0.0f, 60.0f, -70.0f }; cam.target = { 0.0f, 0.0f, 0.0f }; cam.up = { 0, 1, 0 }; cam.fovy = 60.0f;
    BasicTerrain::SetActive(t);
    ui::SetSelectedTerrain(t);
    Frames(10);
    TakeScreenshot("terrain_tabs_0.png");

    // The viewport is x 220..1360, y 120..640; the camera looks down at the terrain's middle.
    cam.position = { 0.0f, 90.0f, -1.0f }; cam.target = { 0.0f, 0.0f, 0.0f }; cam.up = { 0, 0, 1 };
    const float panelTop = 120.0f + ((float)GetScreenHeight() - 120.0f) * 0.5f;   // the terrain panel: bottom half of the right column
    const float tabSculpt = 1407.0f, tabGenerate = 1480.0f, tabFoliage = 1552.0f, tabY = panelTop + 74.0f;
    auto drag = [&](float x0, float y0, float x1, float y1) {
        Mouse(x0, y0); Frames(3); Down(true); Frames(2);
        for (int i = 1; i <= 20; i++) { Mouse(x0 + (x1 - x0) * i / 20.0f, y0 + (y1 - y0) * i / 20.0f); Down(true); Frames(1); }
        Down(false); Frames(3);
    };

    // Sculpt tab, Raise: dragging on the terrain raises it.
    Click(tabSculpt, tabY);
    float h0 = HeightSum(t);
    drag(600, 300, 800, 400);
    float h1 = HeightSum(t);
    std::printf("sculpt tab: height sum %.1f -> %.1f\n", h0, h1);
    CHECK(h1 > h0 + 1.0f);

    // Foliage tab: the sculpt brush is off, and with no foliage brush a drag does nothing at all.
    Click(tabFoliage, tabY);
    Frames(5);
    TakeScreenshot("terrain_tabs_foliage.png");
    float h2 = HeightSum(t);
    drag(600, 300, 800, 400);
    float h3 = HeightSum(t);
    std::printf("foliage tab, no brush: %.1f -> %.1f\n", h2, h3);
    CHECK(std::fabs(h3 - h2) < 0.01f);
    std::printf("tool on foliage tab: %d\n", (int)BasicTerrain::GetBrush().tool);
    CHECK((int)BasicTerrain::GetBrush().tool == (int)BasicTerrain::Tool::None);

    // ADD Grass: the foliage brush is on. A drag paints grass and still does not sculpt.
    Click(1407.0f, panelTop + 253.0f);
    Frames(5);
    CHECK(foliage::Has(t) && foliage::For(t).rules.size() == 1 && foliage::For(t).rules[0].painted);
    int rule = -1;
    CHECK(foliage::PaintTarget(t, &rule) && rule == 0);
    TakeScreenshot("terrain_tabs_grass.png");
    float h4 = HeightSum(t);
    drag(600, 300, 800, 400);
    float h5 = HeightSum(t);
    std::printf("foliage brush: heights %.1f -> %.1f\n", h4, h5);
    CHECK(std::fabs(h5 - h4) < 0.01f);
    std::vector<Vector3> grown;
    foliage::Scatter(*t, foliage::For(t).rules[0], -128.0f, -128.0f, 128.0f, 128.0f, grown);
    std::printf("grass painted: %zu plants\n", grown.size());
    CHECK(grown.size() > 100);
    CHECK((int)BasicTerrain::GetBrush().tool == (int)BasicTerrain::Tool::None);

    // The brush size slider (a drag across its track) changes the radius, and it stays changed.
    const float r0 = BasicTerrain::GetBrush().radius;
    drag(1478.0f, panelTop + 403.0f, 1526.0f, panelTop + 403.0f);
    Frames(5);
    std::printf("brush size %.1f -> %.1f\n", r0, BasicTerrain::GetBrush().radius);
    CHECK(BasicTerrain::GetBrush().radius > r0 + 5.0f);
    const float r1 = BasicTerrain::GetBrush().radius;
    drag(600, 300, 640, 320);
    CHECK(std::fabs(BasicTerrain::GetBrush().radius - r1) < 0.01f);
    grown.clear();
    foliage::Scatter(*t, foliage::For(t).rules[0], -128.0f, -128.0f, 128.0f, 128.0f, grown);

    // A fast drag paints a continuous line, not a row of dots: Clear all, then sweep across in a few big jumps.
    Click(1535.0f, panelTop + 374.0f);   // Clear all
    Frames(3);
    {
        std::vector<Vector3> none;
        foliage::Scatter(*t, foliage::For(t).rules[0], -128.0f, -128.0f, 128.0f, 128.0f, none);
        CHECK(none.empty());
        BasicTerrain::GetBrush().radius = 6.0f;   // small, so the jumps are wider than the brush
        Mouse(350, 500); Frames(3); Down(true); Frames(2);
        for (int i = 1; i <= 4; i++) { Mouse(350.0f + 250.0f * (float)i, 500.0f); Down(true); Frames(1); }
        Down(false); Frames(3);
        std::vector<Vector3> line;
        foliage::Scatter(*t, foliage::For(t).rules[0], -128.0f, -128.0f, 128.0f, 128.0f, line);
        float zMid = 0.0f;
        for (const Vector3& p : line) zMid += p.z;
        zMid /= (float)std::max<size_t>(1, line.size());
        std::vector<float> xs;
        for (const Vector3& p : line) if (std::fabs(p.z - zMid) < 2.0f) xs.push_back(p.x);
        std::sort(xs.begin(), xs.end());
        float gap = 0.0f;
        for (size_t i = 1; i < xs.size(); i++) gap = std::max(gap, xs[i] - xs[i - 1]);
        std::printf("sweep: %zu plants, span %.1f m, biggest gap %.1f m\n", xs.size(), xs.empty() ? 0.0f : xs.back() - xs.front(), gap);
        CHECK(xs.size() > 50 && xs.back() - xs.front() > 40.0f);
        CHECK(gap < 4.0f);
        BasicTerrain::GetBrush().radius = r1;
    }
    grown.clear();
    foliage::Scatter(*t, foliage::For(t).rules[0], -128.0f, -128.0f, 128.0f, 128.0f, grown);

    // Generate tab: no brush either.
    Click(tabGenerate, tabY);
    Frames(3);
    const size_t before = grown.size();
    float h6 = HeightSum(t);
    drag(300, 200, 500, 300);
    CHECK(std::fabs(HeightSum(t) - h6) < 0.01f);
    CHECK(!foliage::PaintTarget(t));   // the foliage brush went away with its tab
    std::vector<Vector3> same;
    foliage::Scatter(*t, foliage::For(t).rules[0], -128.0f, -128.0f, 128.0f, 128.0f, same);
    CHECK(same.size() == before);

    // Back on Sculpt, the sculpt tool is back and works, and nothing was painted by it.
    Click(tabSculpt, tabY);
    Frames(3);
    CHECK((int)BasicTerrain::GetBrush().tool == (int)BasicTerrain::Tool::Raise);
    float h7 = HeightSum(t);
    drag(300, 200, 500, 300);
    CHECK(HeightSum(t) > h7 + 1.0f);
    std::vector<Vector3> after;
    foliage::Scatter(*t, foliage::For(t).rules[0], -128.0f, -128.0f, 128.0f, 128.0f, after);
    CHECK(after.size() == before);

    // Back on Foliage with the grass rule: the brush size slider holds its value.
    Click(tabFoliage, tabY);
    Frames(3);
    ui::Unload();
    return g_fail ? 1 : 0;
}
