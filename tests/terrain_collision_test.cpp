// Terrain collision: the height-field collider must match the terrain mesh exactly (position, scale,
// orientation), the player must stand on / walk over / slide off it, and dynamic bodies must rest on it.
// Drives the real CharacterController and Simulation against BasicTerrain.
// Needs a display (the engine opens a window); not registered with ctest: build/tests/terrain_collision_test
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/Backend/CharacterController.hpp"
#include "Engine/Backend/PhysicsSimulation.hpp"
#include "Engine/Backend/ScatteredObject.hpp"
#include "Terrain/BasicTerrain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_NEAR(a, b, tol) do { const float va_ = (a), vb_ = (b); if (!(std::fabs(va_ - vb_) <= (tol))) { \
    std::printf("FAIL %s:%d %s = %.4f, expected %.4f (+-%.3f)\n", __FILE__, __LINE__, #a, va_, vb_, (float)(tol)); g_fail++; } } while (0)

namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr float kCapsuleHalf = 0.9f;
constexpr float kPi = 3.14159265f;

struct World {
    Engine& engine;
    std::vector<ScatteredObject*> objs;
    phys::Simulation* sim = nullptr;
    CharacterController* cc = nullptr;
};

// Each helper builds a terrain whose heights come from f(localX, localZ), in metres from the terrain's
// centre. The test owns the terrain, so BasicTerrain::GetInstances() sees exactly the scenario's terrains.
template <class F>
std::unique_ptr<BasicTerrain> MakeTerrain(int cells, float scale, Vector3 pos, F f) {
    auto t = std::make_unique<BasicTerrain>(cells, cells, scale, 400.0f, 8.0f);
    t->position = pos;
    float* h = t->GetHeightData();
    for (int z = 0; z < cells; ++z)
        for (int x = 0; x < cells; ++x)
            h[(size_t)z * cells + x] = f((x - cells * 0.5f) * scale, (z - cells * 0.5f) * scale);
    return t;
}

void Teleport(World& w, Vector3 p) {
    w.sim->SetBodyPosition(w.cc->GetPlayerBody(), p);
    *w.cc->GetPlayerBody()->GetPosPtr() = p;
}

Vector3 PlayerPos(World& w) { return *w.cc->GetPlayerBody()->GetPosPtr(); }

// Advances one frame; walking along +z when `walk` (yaw is 0 and nothing moves the mouse).
void Frame(World& w, bool walk) {
    if (walk) w.cc->SetMoveInput({ 0.0f, -1.0f });
    w.sim->Update(kDt);
    w.cc->Update(kDt);
}

void Settle(World& w, int frames = 240) { for (int i = 0; i < frames; ++i) Frame(w, false); }

float Rand01(unsigned& s) { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFF) / 65535.0f; }

// Height of the triangle Box3D builds over a cell: the cell is split along the (x+1,z)-(x,z+1) diagonal,
// the same split BasicTerrain's mesh uses.
float TriangleHeight(const BasicTerrain& t, float wx, float wz) {
    const int w = t.GetWidth(), d = t.GetDepth();
    const float s = t.GetScale();
    const float gx = (wx - t.position.x) / s + w * 0.5f;
    const float gz = (wz - t.position.z) / s + d * 0.5f;
    const int ix = std::clamp((int)std::floor(gx), 0, w - 2), iz = std::clamp((int)std::floor(gz), 0, d - 2);
    const float u = gx - ix, v = gz - iz;
    const float* h = t.GetHeightData();
    const float h11 = h[(size_t)iz * w + ix], h12 = h[(size_t)iz * w + ix + 1];
    const float h21 = h[(size_t)(iz + 1) * w + ix], h22 = h[(size_t)(iz + 1) * w + ix + 1];
    const float y = (u + v <= 1.0f) ? h11 + u * (h12 - h11) + v * (h21 - h11)
                                    : h22 + (1.0f - u) * (h21 - h22) + (1.0f - v) * (h12 - h22);
    return t.position.y + y;
}

} // namespace

int main() {
    Engine engine(320, 240, "terrain_collision_test", 60);
    World w{ engine };
    auto simOwner = std::make_unique<phys::Simulation>(w.objs);
    w.sim = simOwner.get();
    engine.AddEntity(std::move(simOwner));
    auto ccOwner = std::make_unique<CharacterController>(engine, &engine.GetCamera(), *w.sim);
    w.cc = ccOwner.get();
    engine.AddEntity(std::move(ccOwner));
    w.objs.push_back(w.cc->GetPlayerBody());

    // ---- 1. The collider matches the mesh: node by node, and across cells -------------------------
    {
        std::printf("-- alignment\n");
        // Offset, off-centre terrain with uneven heights, so a swapped axis, a wrong origin, a wrong
        // scale or the opposite cell diagonal all show up.
        unsigned seed = 7;
        auto t = MakeTerrain(48, 2.5f, { 30.0f, 5.0f, -20.0f }, [&](float, float) { return Rand01(seed) * 12.0f; });
        w.sim->StartPlay();
        Settle(w, 4);   // queries see shapes once the world has stepped
        const int n = t->GetWidth();
        const float* h = t->GetHeightData();
        int nodeMiss = 0, cellMiss = 0;
        for (int iz = 1; iz < n - 1; iz += 3) {
            for (int ix = 1; ix < n - 1; ix += 3) {
                const float wx = t->position.x + (ix - n * 0.5f) * t->GetScale();
                const float wz = t->position.z + (iz - n * 0.5f) * t->GetScale();
                const auto hit = w.sim->RayCast({ wx, 200.0f, wz }, { 0, -1, 0 }, 400.0f);
                if (!hit.hit || std::fabs(hit.point.y - (t->position.y + h[(size_t)iz * n + ix])) > 0.01f) nodeMiss++;
            }
        }
        unsigned s2 = 99;
        for (int i = 0; i < 400; ++i) {
            const float wx = t->position.x + (Rand01(s2) - 0.5f) * (n - 3) * t->GetScale();
            const float wz = t->position.z + (Rand01(s2) - 0.5f) * (n - 3) * t->GetScale();
            const auto hit = w.sim->RayCast({ wx, 200.0f, wz }, { 0, -1, 0 }, 400.0f);
            if (!hit.hit || std::fabs(hit.point.y - TriangleHeight(*t, wx, wz)) > 0.01f) cellMiss++;
        }
        std::printf("node mismatches %d, in-cell mismatches %d\n", nodeMiss, cellMiss);
        CHECK(nodeMiss == 0);
        CHECK(cellMiss == 0);
        // Outside the terrain there is only the safety floor.
        const auto out = w.sim->RayCast({ 400.0f, 200.0f, 400.0f }, { 0, -1, 0 }, 400.0f);
        CHECK(out.hit);
        CHECK_NEAR(out.point.y, 0.0f, 0.01f);
    }

    // ---- 2. Standing on terrain that is below, and well above, the old y = 0 floor ---------------
    for (float level : { -10.0f, 0.0f, 40.0f }) {
        std::printf("-- stand at terrain level %.0f\n", level);
        auto t = MakeTerrain(64, 4.0f, { 0.0f, level, 0.0f }, [](float, float) { return 0.0f; });
        w.sim->StartPlay();
        w.cc->EnsurePhysicsBody();
        Teleport(w, { 0.0f, level + 6.0f, 0.0f });
        Settle(w);
        CHECK_NEAR(PlayerPos(w).y, level + kCapsuleHalf, 0.03f);
        CHECK(w.cc->IsGrounded());
        CHECK_NEAR(engine.GetCamera().position.y, level + 1.6f, 0.05f);
    }

    // ---- 3. Spawning inside a hill lifts the player onto it ------------------------------------------
    {
        std::printf("-- spawn on a hill\n");
        auto t = MakeTerrain(64, 4.0f, { 0.0f, 0.0f, 0.0f }, [](float x, float z) {
            return 25.0f * std::exp(-(x * x + z * z) / (2.0f * 20.0f * 20.0f)); });
        w.sim->StartPlay();
        w.cc->EnsurePhysicsBody();
        Settle(w);
        const float surface = TriangleHeight(*t, PlayerPos(w).x, PlayerPos(w).z);
        std::printf("player y %.2f, surface %.2f\n", PlayerPos(w).y, surface);
        CHECK(PlayerPos(w).y > surface);
        CHECK(PlayerPos(w).y < surface + kCapsuleHalf + 1.0f);
    }

    // ---- 4. Walking up a 30 degree slope and over its crest, then down the far side ---------------------
    {
        std::printf("-- hill walk\n");
        const float slope = std::tan(30.0f * kPi / 180.0f);
        // A ridge along x: rises from z = 0 to a crest at z = 20, falls back to the ground at z = 40.
        auto t = MakeTerrain(200, 1.0f, { 0.0f, 0.0f, 0.0f }, [&](float, float z) {
            if (z < 0.0f) return 0.0f;
            if (z < 20.0f) return z * slope;
            if (z < 40.0f) return (40.0f - z) * slope;
            return 0.0f; });
        w.sim->StartPlay();
        w.cc->EnsurePhysicsBody();
        Teleport(w, { 0.0f, 2.0f, -10.0f });
        Settle(w);
        int airborne = 0;
        float worstClearance = 0.0f, worstZ = 0.0f;
        for (int i = 0; i < 60 * 14; ++i) {
            Frame(w, true);
            const Vector3 p = PlayerPos(w);
            if (!w.cc->IsGrounded()) airborne++;
            if (p.z > 1.0f && p.z < 39.0f) {   // on a slope: how far the capsule sits from the surface
                const float surf = TriangleHeight(*t, p.x, p.z);
                const float off = std::fabs(p.y - (surf + 0.5f + 0.4f / std::cos(30.0f * kPi / 180.0f)));
                if (off > worstClearance) { worstClearance = off; worstZ = p.z; }
                if (std::getenv("DBG") && p.z > 17.0f && p.z < 30.0f && i % 4 == 0)
                    std::printf("  z %.3f y %.3f surf %.3f off %.3f grounded %d\n", p.z, p.y, surf, p.y - (surf + 0.5f + 0.4f / std::cos(30.0f * kPi / 180.0f)), (int)w.cc->IsGrounded());
            }
        }
        const Vector3 p = PlayerPos(w);
        std::printf("end z %.1f y %.2f, airborne frames %d, worst deviation from the surface %.3f m (at z %.1f)\n",
                    p.z, p.y, airborne, worstClearance, worstZ);
        CHECK(p.z > 45.0f);                 // made it over the ridge
        CHECK_NEAR(p.y, kCapsuleHalf, 0.05f);
        CHECK(airborne <= 4);               // stayed on the ground over the crest and down the slope
        CHECK(worstClearance < 0.12f);      // never sank into, or floated above, the slope
    }

    // ---- 5. A slope too steep to stand on stops the player and slides them back -------------------
    {
        std::printf("-- steep slope\n");
        const float slope = std::tan(65.0f * kPi / 180.0f);
        auto t = MakeTerrain(200, 1.0f, { 0.0f, 0.0f, 0.0f }, [&](float, float z) {
            return z < 0.0f ? 0.0f : std::min(z * slope, 40.0f); });
        w.sim->StartPlay();
        w.cc->EnsurePhysicsBody();
        Teleport(w, { 0.0f, 2.0f, -10.0f });
        Settle(w);
        float maxY = 0.0f;
        for (int i = 0; i < 60 * 5; ++i) { Frame(w, true); maxY = std::max(maxY, PlayerPos(w).y); }
        std::printf("highest y reached %.2f, end z %.2f\n", maxY, PlayerPos(w).z);
        CHECK(maxY < 2.5f);                 // never climbed the 65 degree face
        CHECK(PlayerPos(w).z < 3.0f);
    }

    // ---- 6. Steps and walls on flat terrain ------------------------------------------------------------------
    for (float height : { 0.3f, 1.0f }) {
        std::printf("-- obstacle %.1f m tall\n", height);
        auto t = MakeTerrain(64, 4.0f, { 0.0f, 0.0f, 0.0f }, [](float, float) { return 0.0f; });
        auto block = std::make_unique<ScatteredObject>(Vector3{ 0.0f, height * 0.5f, 5.0f }, Vector3{ 4.0f, height, 1.0f }, GRAY, ShapeType::Cube);
        block->anchored = true;
        w.objs.push_back(block.get());
        ScatteredObject* blockPtr = block.get();
        engine.AddEntity(std::move(block));
        w.sim->StartPlay();
        w.cc->EnsurePhysicsBody();
        Teleport(w, { 0.0f, 2.0f, 0.0f });
        Settle(w);
        float maxZ = -100.0f, maxY = 0.0f;
        for (int i = 0; i < 60 * 3; ++i) {
            Frame(w, true); maxZ = std::max(maxZ, PlayerPos(w).z); maxY = std::max(maxY, PlayerPos(w).y);
            if (std::getenv("DBG") && i < 90 && i % 3 == 0) std::printf("  f%d z %.3f y %.3f grounded %d\n", i, PlayerPos(w).z, PlayerPos(w).y, (int)w.cc->IsGrounded());
        }
        std::printf("max z %.2f, max y %.2f\n", maxZ, maxY);
        if (height < 0.4f) {
            CHECK(maxZ > 8.0f);                       // stepped up and across
            CHECK(maxY > height + kCapsuleHalf - 0.05f);
        } else {
            CHECK(maxZ < 5.0f - 0.5f);                // stopped in front of the wall
            CHECK(maxZ > 5.0f - 0.5f - 0.5f - 0.1f);  // ... and actually reached it
            CHECK(maxY < kCapsuleHalf + 0.1f);
        }
        w.objs.erase(std::find(w.objs.begin(), w.objs.end(), blockPtr));
        *blockPtr->GetPosPtr() = { 5000.0f, -5000.0f, 5000.0f };
    }

    // ---- 7. Dynamic bodies come to rest on the terrain --------------------------------------------------------------
    {
        std::printf("-- dynamic bodies\n");
        // Raised, off-centre terrain: flat in the middle (box and ball land there), a 10 degree ramp towards +x
        // (a box on it must hold, friction 0.4 > tan 10), and nothing under the origin so a missing collider
        // would let everything fall to the y = 0 floor instead.
        const float ramp = std::tan(10.0f * kPi / 180.0f);
        auto t = MakeTerrain(64, 4.0f, { 10.0f, 5.0f, -10.0f }, [&](float x, float) {
            return 2.0f + (x > 40.0f ? (x - 40.0f) * ramp : 0.0f); });
        w.sim->SetRestitution(0.0f);   // no bounce, so "at rest" is reached quickly
        auto addBody = [&](Vector3 pos, ShapeType shape) {
            auto o = std::make_unique<ScatteredObject>(pos, Vector3{ 1.0f, 1.0f, 1.0f }, RED, shape);
            o->anchored = false;
            ScatteredObject* ptr = o.get();
            w.objs.push_back(ptr);
            engine.AddEntity(std::move(o));
            return ptr;
        };
        ScatteredObject* box = addBody({ 10.0f, 30.0f, -10.0f }, ShapeType::Cube);
        ScatteredObject* ball = addBody({ 0.0f, 30.0f, -10.0f }, ShapeType::Sphere);
        ScatteredObject* rampBox = addBody({ 10.0f + 70.0f, 5.0f + 2.0f + 30.0f * ramp + 4.0f, -10.0f }, ShapeType::Cube);
        w.sim->StartPlay();
        w.cc->EnsurePhysicsBody();
        Teleport(w, { -400.0f, 2.0f, 400.0f });
        for (int i = 0; i < 60 * 6; ++i) Frame(w, false);
        const float flatTop = 5.0f + 2.0f;
        std::printf("box y %.3f, ball y %.3f (surface %.3f)\n", box->GetPosPtr()->y, ball->GetPosPtr()->y, flatTop);
        CHECK_NEAR(box->GetPosPtr()->y, flatTop + 0.5f, 0.05f);
        CHECK_NEAR(ball->GetPosPtr()->y, flatTop + 0.5f, 0.05f);
        const Vector3 r0 = *rampBox->GetPosPtr();
        for (int i = 0; i < 60 * 4; ++i) Frame(w, false);
        const Vector3 r1 = *rampBox->GetPosPtr();
        const float surf = TriangleHeight(*t, r1.x, r1.z);
        std::printf("ramp box: (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f), surface %.3f\n", r0.x, r0.y, r0.z, r1.x, r1.y, r1.z, surf);
        CHECK(std::fabs(r1.x - r0.x) < 0.1f);      // held by friction, not sliding down
        CHECK(r1.y > surf + 0.45f && r1.y < surf + 0.75f);
        for (ScatteredObject* o : { box, ball, rampBox }) {
            w.objs.erase(std::find(w.objs.begin(), w.objs.end(), o));
            *o->GetPosPtr() = { 5000.0f, -5000.0f, 5000.0f };
        }
        w.sim->SetRestitution(0.7f);
    }

    std::printf(g_fail ? "terrain_collision_test: %d FAILED\n" : "terrain_collision_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
