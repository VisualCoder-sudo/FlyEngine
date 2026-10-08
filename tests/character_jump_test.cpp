// Player jump: after landing from any height, ONE jump press must lift the player a full jump
// (v^2 / 2g), never "a tiny bit and stuck". Drives the real CharacterController against a static roof.
// Needs a display (the engine opens a window); not registered with ctest: build/tests/character_jump_test
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/Backend/CharacterController.hpp"
#include "Engine/Backend/PhysicsSimulation.hpp"
#include "Engine/Backend/ScatteredObject.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
    Engine engine(320, 240, "character_jump_test", 60);
    std::vector<ScatteredObject*> objs;
    auto simOwner = std::make_unique<phys::Simulation>(objs);
    phys::Simulation& sim = *simOwner;
    engine.AddEntity(std::move(simOwner));

    // The roof: a big static slab whose top face is at y = 0.
    auto roof = std::make_unique<ScatteredObject>(Vector3{ 0.0f, -0.5f, 0.0f }, Vector3{ 60.0f, 1.0f, 60.0f }, GRAY, ShapeType::Cube);
    roof->anchored = true;
    objs.push_back(roof.get());
    engine.AddEntity(std::move(roof));

    auto ccOwner = std::make_unique<CharacterController>(engine, &engine.GetCamera(), sim);
    CharacterController& cc = *ccOwner;
    engine.AddEntity(std::move(ccOwner));
    objs.push_back(cc.GetPlayerBody());

    sim.StartPlay();
    cc.EnsurePhysicsBody();

    const float dt = 1.0f / 60.0f;
    const float expected = cc.jumpForce * cc.jumpForce / (2.0f * -cc.gravity);   // apex height of one jump
    std::printf("expected apex rise %.2f m\n", expected);

    for (float dropFrom : { 1.5f, 3.0f, 6.0f, 12.0f, 25.0f, 50.0f }) {
        sim.SetBodyPosition(cc.GetPlayerBody(), { 0.0f, 0.9f + dropFrom, 0.0f });
        *cc.GetPlayerBody()->GetPosPtr() = { 0.0f, 0.9f + dropFrom, 0.0f };
        // Fall and settle.
        for (int i = 0; i < 400; i++) { sim.Update(dt); cc.Update(dt); }
        const float standY = cc.GetPlayerBody()->GetPosPtr()->y;

        // One jump press, then track the highest point reached.
        cc.Jump();
        float top = standY;
        for (int i = 0; i < 120; i++) {
            sim.Update(dt); cc.Update(dt);
            top = std::fmax(top, cc.GetPlayerBody()->GetPosPtr()->y);
        }
        const float rise = top - standY;
        std::printf("drop %5.1f m: stand centre y=%.3f  first jump rose %.2f m (%s)\n", dropFrom, standY, rise,
                    rise > expected * 0.8f ? "ok" : "STUCK");
        CHECK(rise > expected * 0.8f);
    }

    std::printf(g_fail ? "character_jump_test: %d FAILED\n" : "character_jump_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
