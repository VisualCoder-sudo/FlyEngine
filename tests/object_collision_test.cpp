// Collisions with scene objects in play: the player walks into a wall of every primitive shape and of an imported
// (OBJ) model at each collision accuracy and must stop in front of it; dynamic shapes dropped on the floor come to
// rest on it; raycasts hit the objects. Opens a window (it needs an Engine), so ctest needs a display
// (label needs_display): build/tests/object_collision_test
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/Backend/CharacterController.hpp"
#include "Engine/Backend/PhysicsSimulation.hpp"
#include "Engine/Backend/ScatteredObject.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

namespace {

constexpr float kDt = 1.0f / 60.0f;

struct World {
    Engine& engine;
    std::vector<ScatteredObject*> objs;
    phys::Simulation* sim = nullptr;
    CharacterController* cc = nullptr;
};

ScatteredObject* Add(World& w, const char* name, ShapeType shape, Vector3 pos, Vector3 size, bool anchored) {
    auto o = std::make_unique<ScatteredObject>(pos, size, GRAY, shape);
    o->SetName(name);
    o->anchored = anchored;
    ScatteredObject* raw = o.get();
    w.objs.push_back(raw);
    w.engine.AddEntity(std::move(o));
    return raw;
}

void Teleport(World& w, Vector3 p) {
    w.sim->SetBodyPosition(w.cc->GetPlayerBody(), p);
    *w.cc->GetPlayerBody()->GetPosPtr() = p;
}

Vector3 PlayerPos(World& w) { return *w.cc->GetPlayerBody()->GetPosPtr(); }

void Frame(World& w, bool walk) {
    if (walk) w.cc->SetMoveInput({ 0.0f, -1.0f });   // forward: +z with the camera's default heading
    w.sim->Update(kDt);
    w.cc->Update(kDt);
}

// An OBJ box (8 corners, 12 triangles) the size of a unit cube.
void WriteBoxObj(const char* path) {
    std::ofstream f(path);
    f << "v -0.5 -0.5 -0.5\nv 0.5 -0.5 -0.5\nv 0.5 0.5 -0.5\nv -0.5 0.5 -0.5\n"
         "v -0.5 -0.5 0.5\nv 0.5 -0.5 0.5\nv 0.5 0.5 0.5\nv -0.5 0.5 0.5\n"
         "vn 0 0 -1\nvn 0 0 1\nvn -1 0 0\nvn 1 0 0\nvn 0 -1 0\nvn 0 1 0\nvt 0 0\n"
         "f 1/1/1 3/1/1 2/1/1\nf 1/1/1 4/1/1 3/1/1\nf 5/1/2 6/1/2 7/1/2\nf 5/1/2 7/1/2 8/1/2\n"
         "f 1/1/3 5/1/3 8/1/3\nf 1/1/3 8/1/3 4/1/3\nf 2/1/4 3/1/4 7/1/4\nf 2/1/4 7/1/4 6/1/4\n"
         "f 1/1/5 2/1/5 6/1/5\nf 1/1/5 6/1/5 5/1/5\nf 4/1/6 8/1/6 7/1/6\nf 4/1/6 7/1/6 3/1/6\n";
}

} // namespace

int main() {
    Engine engine(320, 240, "object_collision_test", 60);
    World w{ engine };
    auto simOwner = std::make_unique<phys::Simulation>(w.objs);
    w.sim = simOwner.get();
    engine.AddEntity(std::move(simOwner));
    auto ccOwner = std::make_unique<CharacterController>(engine, &engine.GetCamera(), *w.sim);
    w.cc = ccOwner.get();
    engine.AddEntity(std::move(ccOwner));
    w.objs.push_back(w.cc->GetPlayerBody());

    WriteBoxObj("collision_box.obj");
    // The same box with a normal-mapped material: loading it generates tangents (the PBR import path).
    {
        std::ifstream in("collision_box.obj");
        std::stringstream body;
        body << in.rdbuf();
        std::ofstream("collision_bumpy.obj") << "mtllib collision_bumpy.mtl\nusemtl bumpy\n" << body.str();
        std::ofstream("collision_bumpy.mtl") << "newmtl bumpy\nKd 1 1 1\nbump collision_bumps.png\n";
        Image bumps = GenImageColor(16, 16, Color{ 128, 128, 255, 255 });
        ExportImage(bumps, "collision_bumps.png");
        UnloadImage(bumps);
    }
    Add(w, "Floor", ShapeType::Cube, { 0, -0.5f, 0 }, { 400, 1, 400 }, true);

    // One lane per obstacle, 12 m apart along x; each obstacle's near face is at z = 8. (A cylinder's origin is its
    // base, every other shape's its centre.)
    struct Lane { const char* name; ShapeType shape; const char* model; pcoll::CollisionAccuracy acc; Vector3 size; };
    const Lane lanes[] = {
        { "cube", ShapeType::Cube, nullptr, pcoll::CollisionAccuracy::Box, { 4, 4, 2 } },
        { "sphere", ShapeType::Sphere, nullptr, pcoll::CollisionAccuracy::Default, { 4, 4, 4 } },
        { "cylinder", ShapeType::Cylinder, nullptr, pcoll::CollisionAccuracy::Default, { 4, 4, 4 } },
        { "wedge", ShapeType::Wedge, nullptr, pcoll::CollisionAccuracy::Default, { 4, 4, 4 } },
        { "obj box", ShapeType::Cube, "collision_box.obj", pcoll::CollisionAccuracy::Box, { 4, 4, 2 } },
        { "obj hull", ShapeType::Cube, "collision_box.obj", pcoll::CollisionAccuracy::Hull, { 4, 4, 2 } },
        { "obj default", ShapeType::Cube, "collision_box.obj", pcoll::CollisionAccuracy::Default, { 4, 4, 2 } },
        { "obj precise", ShapeType::Cube, "collision_box.obj", pcoll::CollisionAccuracy::Precise, { 4, 4, 2 } },
        { "obj normalmap", ShapeType::Cube, "collision_bumpy.obj", pcoll::CollisionAccuracy::Precise, { 4, 4, 2 } },
    };
    const int laneCount = (int)(sizeof(lanes) / sizeof(lanes[0]));
    std::vector<ScatteredObject*> walls;
    for (int i = 0; i < laneCount; ++i) {
        const Lane& l = lanes[i];
        const float y = l.shape == ShapeType::Cylinder ? 0.0f : l.size.y * 0.5f;
        const Vector3 pos = { -36.0f + 12.0f * i, y, 8.0f + l.size.z * 0.5f };
        ScatteredObject* o = Add(w, l.name, l.shape, pos, l.size, true);
        if (l.model) {
            CHECK(o->SetModel(l.model));
            o->NormalizeModelToUnitBox();
        }
        o->SetCollisionAccuracy(l.acc);
        walls.push_back(o);
    }

    // Dynamic shapes dropped from 6 m, far from the lanes.
    const ShapeType dropShapes[] = { ShapeType::Cube, ShapeType::Sphere, ShapeType::Cylinder, ShapeType::Wedge };
    std::vector<ScatteredObject*> drops;
    for (int i = 0; i < 4; ++i) drops.push_back(Add(w, "Drop", dropShapes[i], { 80.0f + 6.0f * i, 6.0f, 40.0f }, { 1, 1, 1 }, false));
    ScatteredObject* dropModel = Add(w, "DropModel", ShapeType::Cube, { 110.0f, 6.0f, 40.0f }, { 1, 1, 1 }, false);
    CHECK(dropModel->SetModel("collision_box.obj"));
    dropModel->NormalizeModelToUnitBox();

    w.sim->StartPlay();
    w.cc->EnsurePhysicsBody();

    for (int i = 0; i < laneCount; ++i) {
        const float x = walls[i]->GetPosPtr()->x;
        Teleport(w, { x, 1.0f, 0.0f });
        for (int f = 0; f < 30; ++f) Frame(w, false);
        for (int f = 0; f < 240; ++f) Frame(w, true);
        const Vector3 p = PlayerPos(w);
        std::printf("%-12s player stops at z %.2f (wall face at 8), y %.2f\n", lanes[i].name, p.z, p.y);
        CHECK(p.z < 8.0f && p.z > 6.0f);        // pressed against the wall, not through it, not short of it
        CHECK(p.y > 0.5f && p.y < 1.5f);        // still on the floor (not climbed over or pushed down)
        // A ray straight down onto the obstacle's top hits it.
        const Vector3 top = { x, 20.0f, walls[i]->GetPosPtr()->z };
        const phys::RaycastHit hit = w.sim->RayCast(top, { 0, -1, 0 }, 40.0f);
        std::printf("%-12s ray from above hits y %.2f (top at %.2f)\n", lanes[i].name, hit.hit ? hit.point.y : -999.0f, lanes[i].size.y);
        CHECK(hit.hit && hit.point.y > lanes[i].size.y * 0.45f && hit.point.y < lanes[i].size.y + 0.05f);
    }

    for (int f = 0; f < 240; ++f) Frame(w, false);
    for (ScatteredObject* d : drops) {
        const float rest = d->GetShapeType() == ShapeType::Cylinder ? 0.0f : 0.5f;   // origin at the base / the centre
        std::printf("drop %d rests at y %.2f (expected %.2f)\n", (int)d->GetShapeType(), d->GetPosPtr()->y, rest);
        CHECK(std::fabs(d->GetPosPtr()->y - rest) < 0.1f);
    }
    std::printf("dropped model rests at y %.2f\n", dropModel->GetPosPtr()->y);
    CHECK(std::fabs(dropModel->GetPosPtr()->y - 0.5f) < 0.1f);

    if (g_fail == 0) std::printf("object_collision_test: all checks passed\n");
    else std::printf("object_collision_test: %d FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
