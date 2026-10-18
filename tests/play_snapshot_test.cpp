// The Play/undo snapshot must put back every property the scene file stores, and report what differs. Opens a window
// (it needs an Engine), so it is not registered with ctest: build/tests/play_snapshot_test
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/Backend/ScenePersistence.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static ScatteredObject* Make(Engine& engine, std::vector<ScatteredObject*>& objs, const char* name, Vector3 pos) {
    auto o = std::make_unique<ScatteredObject>(pos, Vector3{1, 2, 3}, Color{10, 20, 30, 255}, ShapeType::Cube);
    o->SetName(name);
    ScatteredObject* raw = o.get();
    objs.push_back(raw);
    engine.AddEntity(std::move(o));
    return raw;
}

int main() {
    Engine engine(320, 200, "play_snapshot_test", 60);
    std::vector<ScatteredObject*> objs;
    std::vector<std::unique_ptr<ModelGroup>> models;
    ScatteredObject* a = Make(engine, objs, "A", {1, 2, 3});
    ScatteredObject* b = Make(engine, objs, "B", {4, 5, 6});
    a->waterInteraction.floats = false; a->waterInteraction.foam = false; a->waterInteraction.splash = true;
    b->SetTransparency(0.25f); b->anchored = true; b->canCollide = false; b->SetMass(7.0f);

    const std::string base = "";
    std::string bytes;
    CHECK(SnapshotSceneToMemory(objs, models, base, bytes));
    const auto before = SnapshotObjectRecords(objs, base);
    CHECK(before.size() == 2);
    CHECK(DiffObjectRecords(before, SnapshotObjectRecords(objs, base)).empty());

    // Play changes things, including the water interaction flags the old snapshot never stored.
    a->waterInteraction.floats = true; a->waterInteraction.foam = true; a->waterInteraction.splash = false;
    *a->GetPosPtr() = {9, 9, 9};
    *a->GetColorPtr() = Color{200, 0, 0, 255};
    b->SetTransparency(1.0f); b->anchored = false; b->canCollide = true;
    Make(engine, objs, "Made", {0, 0, 0});

    const auto changed = DiffObjectRecords(before, SnapshotObjectRecords(objs, base));
    CHECK(changed.size() == 3);   // A and B changed, Made is new

    // Restore, the way ObjectInteractionManager does: load the snapshot, drop what was there before.
    const size_t previous = objs.size();
    std::istringstream in(bytes);
    CHECK(RestoreSceneFromMemory(in, engine, objs, models, base));
    for (size_t i = 0; i < previous; ++i) engine.RemoveEntity(objs[i]);
    objs.erase(objs.begin(), objs.begin() + previous);

    CHECK(objs.size() == 2);
    const auto after = SnapshotObjectRecords(objs, base);
    const auto stuck = DiffObjectRecords(before, after);
    for (const auto& s : stuck) std::printf("still differs: %s\n", s.c_str());
    CHECK(stuck.empty());
    CHECK(!objs[0]->waterInteraction.floats && !objs[0]->waterInteraction.foam && objs[0]->waterInteraction.splash);
    CHECK(std::fabs(objs[1]->GetTransparency() - 0.25f) < 1e-6f && objs[1]->anchored && !objs[1]->canCollide);

    std::printf(g_fail ? "play_snapshot_test: %d FAILED\n" : "play_snapshot_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
