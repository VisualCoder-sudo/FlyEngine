// Foliage (rule-based plants on a BasicTerrain) and particles/decals. Checks the placement rules, the save format and
// the pools, and writes screenshots (foliage_*.png, particles_*.png) to the working directory for a visual check.
// Opens a window, so it is not registered with ctest: build/tests/foliage_test
#include "raylib.h"
#include "raymath.h"
#include "Engine.hpp"
#include "Engine/Graphics.hpp"
#include "Engine/Particles.hpp"
#include "../include/Terrain/BasicTerrain.hpp"
#include "../include/Terrain/Foliage.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void Look(Engine& engine, Vector3 pos, Vector3 target, float fov = 60.0f) {
    Camera3D& cam = engine.GetCamera();
    cam.position = pos; cam.target = target; cam.up = { 0, 1, 0 }; cam.fovy = fov; cam.projection = CAMERA_PERSPECTIVE;
}

int main() {
    Engine engine(960, 540, "foliage_test", 60);
    engine.SetPlayerBuild(true);

    auto owner = std::make_unique<BasicTerrain>(128, 128, 2.0f, 40.0f, 16.0f);
    BasicTerrain* t = owner.get();
    t->GenerateFlat(0.0f);
    t->genBoxPos = { 0.0f, 12.0f, 0.0f };
    t->genBoxSize = { 256.0f, 30.0f, 256.0f };
    t->genHills = true;
    t->genTextures = true;
    t->GenerateTerrainInBox();
    engine.AddEntity(std::move(owner));

    // ---- rules ----
    foliage::Settings& set = foliage::For(t);
    for (foliage::Kind k : { foliage::Kind::Grass, foliage::Kind::Flowers, foliage::Kind::Bush, foliage::Kind::Tree, foliage::Kind::Pine, foliage::Kind::Rock })
        set.rules.push_back(foliage::DefaultRule(k));
    for (foliage::Rule& r : set.rules) r.layer = -1;   // the automatic painting depends on how many layer textures loaded
    CHECK(foliage::Has(t));

    // Placement: deterministic, on the ground, inside the terrain, never steeper than the rule allows.
    {
        foliage::Rule r = foliage::DefaultRule(foliage::Kind::Tree);
        r.layer = -1; r.density = 20.0f; r.maxSlope = 20.0f;
        std::vector<Vector3> a, b;
        foliage::Scatter(*t, r, -60.0f, -60.0f, 60.0f, 60.0f, a);
        foliage::Scatter(*t, r, -60.0f, -60.0f, 60.0f, 60.0f, b);
        std::printf("trees placed: %zu\n", a.size());
        CHECK(a.size() > 500 && a.size() == b.size());
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); i++) same = a[i].x == b[i].x && a[i].z == b[i].z;
        CHECK(same);
        const float cosMax = std::cos(r.maxSlope * DEG2RAD);
        int steep = 0, floating = 0;
        for (const Vector3& p : a) {
            if (t->GetNormalAt(p.x, p.z).y < cosMax - 1e-4f) steep++;
            if (std::fabs(p.y - (t->position.y + t->GetHeightAt(p.x, p.z))) > 1e-3f) floating++;
        }
        CHECK(steep == 0);
        CHECK(floating == 0);
        const float expected = r.density / 100.0f * 120.0f * 120.0f;
        CHECK((float)a.size() < expected * 1.05f);   // the slope test only removes plants

        // Fewer when painted weight is demanded than when it is not.
        foliage::Rule onLayer = r; onLayer.layer = 3; onLayer.minWeight = 0.9f;
        std::vector<Vector3> c;
        foliage::Scatter(*t, onLayer, -60.0f, -60.0f, 60.0f, 60.0f, c);
        CHECK(c.size() < a.size());

        // Altitude limits.
        foliage::Rule high = r; high.minAltitude = t->position.y + 15.0f;
        std::vector<Vector3> d;
        foliage::Scatter(*t, high, -60.0f, -60.0f, 60.0f, 60.0f, d);
        for (const Vector3& p : d) CHECK(p.y >= high.minAltitude - 1e-3f);
    }

    // Level of detail: every kind gets cheaper with each level, and the cheapest is a small fraction of the full mesh.
    for (int k = 0; k < (int)foliage::Kind::Count; k++) {
        const foliage::Kind kind = (foliage::Kind)k;
        const int t0 = foliage::MeshTriangles(kind, 0), t1 = foliage::MeshTriangles(kind, 1), t2 = foliage::MeshTriangles(kind, 2);
        std::printf("%s triangles: %d / %d / %d\n", foliage::KindName(kind), t0, t1, t2);
        CHECK(t0 > 0 && t1 > 0 && t2 > 0);
        CHECK(t1 <= t0 && t2 <= t1);
        CHECK(t2 * 2 <= t0);
    }
    set.rules[0].lod1 = 0.1f; set.rules[0].lod2 = 0.3f;

    // Save format round trip.
    {
        std::stringstream ss;
        CHECK(foliage::Write(ss));
        const foliage::Settings before = set;
        set.rules.clear();
        set.wind = 0.0f;
        CHECK(foliage::Read(ss) == 1);
        const foliage::Settings& after = foliage::For(t);
        CHECK(after.rules.size() == before.rules.size());
        CHECK(std::fabs(after.wind - before.wind) < 1e-4f);
        for (size_t i = 0; i < after.rules.size() && i < before.rules.size(); i++) {
            CHECK(after.rules[i].kind == before.rules[i].kind);
            CHECK(std::fabs(after.rules[i].density - before.rules[i].density) < 1e-3f);
            CHECK(after.rules[i].tintB.g == before.rules[i].tintB.g);
            CHECK(after.rules[i].layer == before.rules[i].layer);
            CHECK(std::fabs(after.rules[i].lod1 - before.rules[i].lod1) < 1e-4f && std::fabs(after.rules[i].lod2 - before.rules[i].lod2) < 1e-4f);
        }
    }

    // ---- pictures ----
    {
        const float h = t->position.y + t->GetHeightAt(0.0f, 0.0f);
        Look(engine, { 0.0f, h + 2.2f, 0.0f }, { 6.0f, h + 1.2f, 30.0f });
        for (int f = 0; f < 40; f++) engine.StepFrame(1.0f / 60.0f);
        std::printf("plants placed %d, drawn %d\n", foliage::InstanceCount(), foliage::DrawnCount());
        CHECK(foliage::InstanceCount() > 1000);
        CHECK(foliage::DrawnCount() > 500);
        TakeScreenshot("foliage_ground.png");

        Look(engine, { -90.0f, h + 45.0f, -90.0f }, { 20.0f, h, 20.0f }, 65.0f);
        for (int f = 0; f < 60; f++) engine.StepFrame(1.0f / 60.0f);
        TakeScreenshot("foliage_overview.png");

        // A close look at each kind, standing in a row at the camera's feet.
        Look(engine, { 0.0f, h + 1.4f, 0.0f }, { 0.0f, h + 0.9f, 8.0f }, 50.0f);
        for (int f = 0; f < 10; f++) engine.StepFrame(1.0f / 60.0f);
        TakeScreenshot("foliage_closeup.png");
    }

    // ---- the foliage brush and the distance fade ----
    {
        foliage::Rule painted = foliage::DefaultRule(foliage::Kind::Flowers);
        painted.painted = true; painted.density = 40.0f; painted.seed = 11;
        set.rules.push_back(painted);
        const int idx = (int)set.rules.size() - 1;
        std::vector<Vector3> none;
        foliage::Scatter(*t, set.rules[idx], -60.0f, -60.0f, 60.0f, 60.0f, none);
        CHECK(none.empty());   // nothing painted, nothing grows
        foliage::SetPaintTarget(t, idx);
        CHECK(foliage::PaintTarget(t));
        for (int i = 0; i < 30; i++) foliage::Paint(t, 10.0f, 25.0f, 8.0f, 0.13f, 0.5f, false);
        std::vector<Vector3> grown;
        foliage::Scatter(*t, set.rules[idx], -60.0f, -60.0f, 60.0f, 60.0f, grown);
        CHECK(grown.size() > 20);
        bool inside = true;
        for (const Vector3& p : grown) inside = inside && std::hypot(p.x - 10.0f, p.z - 25.0f) < 9.5f;
        CHECK(inside);
        // Erasing takes them away again.
        for (int i = 0; i < 60; i++) foliage::Paint(t, 10.0f, 25.0f, 12.0f, 0.13f, 1.0f, true);
        std::vector<Vector3> gone;
        foliage::Scatter(*t, set.rules[idx], -60.0f, -60.0f, 60.0f, 60.0f, gone);
        CHECK(gone.empty());
        for (int i = 0; i < 30; i++) foliage::Paint(t, 10.0f, 25.0f, 8.0f, 0.13f, 0.5f, false);

        // The mask survives a save.
        std::stringstream ss;
        CHECK(foliage::Write(ss));
        set.rules.clear();
        CHECK(foliage::Read(ss) == 1);
        const foliage::Settings& back = foliage::For(t);
        CHECK(back.rules.size() == 7 && back.rules[6].painted && back.rules[6].mask);
        std::vector<Vector3> again;
        foliage::Scatter(*t, back.rules[6], -60.0f, -60.0f, 60.0f, 60.0f, again);
        CHECK(again.size() == grown.size());
        foliage::SetPaintTarget(nullptr, -1);

        // Far away, plants have thinned out; up close they all show.
        const float h = t->position.y + t->GetHeightAt(0.0f, 0.0f);
        foliage::For(t).rules[6].enabled = false;
        for (size_t i = 1; i < foliage::For(t).rules.size() - 1; i++) foliage::For(t).rules[i].enabled = false;
        foliage::For(t).rules[0].fadeStart = 0.1f;
        foliage::For(t).rules[0].viewDistance = 90.0f;
        foliage::For(t).rules[0].density = 300.0f;
        Look(engine, { 0.0f, h + 3.0f, 0.0f }, { 0.0f, h + 0.5f, 60.0f }, 60.0f);
        for (int f = 0; f < 60; f++) engine.StepFrame(1.0f / 60.0f);
        TakeScreenshot("foliage_fade.png");
        const int drawnFar = foliage::DrawnCount();
        foliage::For(t).distance = 0.4f;   // draw distance knob
        for (int f = 0; f < 10; f++) engine.StepFrame(1.0f / 60.0f);
        std::printf("fade: drawn %d, at 0.4x distance %d\n", drawnFar, foliage::DrawnCount());
        CHECK(foliage::DrawnCount() < drawnFar);
        foliage::For(t).distance = 1.0f;
        foliage::For(t).rules[0] = foliage::DefaultRule(foliage::Kind::Grass);
        foliage::For(t).rules[0].layer = -1;
        for (foliage::Rule& r : foliage::For(t).rules) r.enabled = true;
        foliage::For(t).rules.pop_back();   // the painted rule
    }

    // ---- particles and decals ----
    {
        fx::Clear();
        CHECK(fx::ParticleCount() == 0 && fx::DecalCount() == 0 && fx::EmitterCount() == 0);
        foliage::For(t).enabled = false;   // a clear view of the effects
        const float h = t->position.y + t->GetHeightAt(0.0f, 4.0f);   // the ground where the effects are
        const int fire = fx::StartEmitter(fx::Preset::Fire, { 0.0f, h + 0.1f, 4.0f }, { 0, 1, 0 }, 1.2f);
        const int smoke = fx::StartEmitter(fx::Preset::Smoke, { 0.0f, h + 1.7f, 4.0f }, { 0, 1, 0 }, 1.4f);
        CHECK(fire > 0 && smoke > 0 && fire != smoke);
        fx::Emit(fx::Preset::Sparks, { -1.8f, h + 0.5f, 4.0f }, { 0, 1, 0 }, 40, 1.0f);
        fx::Emit(fx::Preset::Dust, { 1.8f, h + 0.1f, 4.0f }, { 0, 1, 0 }, 30, 1.0f);
        fx::Decal({ 0.0f, h + 0.02f, 3.0f }, { 0, 1, 0 }, 1.6f, { 20, 18, 15, 200 }, 30.0f);
        Look(engine, { 0.0f, h + 2.6f, -2.0f }, { 0.0f, h + 1.0f, 4.0f }, 60.0f);
        for (int f = 0; f < 45; f++) engine.StepFrame(1.0f / 60.0f);
        std::printf("particles %d decals %d emitters %d\n", fx::ParticleCount(), fx::DecalCount(), fx::EmitterCount());
        CHECK(fx::ParticleCount() > 50);
        CHECK(fx::DecalCount() == 1);
        CHECK(fx::EmitterCount() == 2);
        TakeScreenshot("particles_fire.png");
        fx::StopEmitter(fire);
        CHECK(fx::EmitterCount() == 1);

        // Everything dies off by itself.
        fx::Clear();
        fx::Emit(fx::Preset::Splash, { 0.0f, h + 1.0f, 3.0f }, { 0, 1, 0 }, 25, 1.0f);
        CHECK(fx::ParticleCount() == 25);
        for (int f = 0; f < 150; f++) engine.StepFrame(1.0f / 60.0f);
        CHECK(fx::ParticleCount() == 0);

        // Decals fade out; the ring replaces the oldest when full.
        fx::Decal({ 0, 0, 0 }, { 0, 1, 0 }, 1.0f, WHITE, 0.5f);
        for (int f = 0; f < 60; f++) engine.StepFrame(1.0f / 60.0f);
        CHECK(fx::DecalCount() == 0);
        for (int i = 0; i < 400; i++) fx::Decal({ (float)i, 0, 0 }, { 0, 1, 0 }, 1.0f, WHITE, 60.0f);
        CHECK(fx::DecalCount() == 256);

        // Hard hits kick up dust by themselves; soft ones don't.
        fx::Clear();
        fx::OnImpact({ 0, 0, 0 }, { 0, 1, 0 }, 1.0f);
        CHECK(fx::ParticleCount() == 0);
        fx::OnImpact({ 0, 0, 0 }, { 0, 1, 0 }, 9.0f);
        CHECK(fx::ParticleCount() > 5);
        fx::SetAutoImpacts(false);
        fx::Clear();
        fx::OnImpact({ 0, 0, 0 }, { 0, 1, 0 }, 9.0f);
        CHECK(fx::ParticleCount() == 0);
        fx::SetAutoImpacts(true);

        fx::Preset p;
        CHECK(fx::PresetFromName("fIrE", p) && p == fx::Preset::Fire);
        CHECK(!fx::PresetFromName("Lava", p));
    }

    // Sculpting re-places plants once the ground settles.
    {
        foliage::For(t).enabled = true;
        const int before = foliage::InstanceCount();
        BasicTerrain::Brush br; br.tool = BasicTerrain::Tool::Raise; br.radius = 30.0f; br.strength = 40.0f;
        const uint32_t rev = t->GetRevision();
        t->ApplyBrush(br, { 10.0f, 10.0f }, 1.0f);
        CHECK(t->GetRevision() != rev);
        for (int f = 0; f < 60; f++) engine.StepFrame(1.0f / 60.0f);
        CHECK(foliage::InstanceCount() > 0);
        (void)before;
    }

    std::printf(g_fail ? "foliage_test: %d FAILED\n" : "foliage_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
