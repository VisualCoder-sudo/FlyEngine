// A group saved as a prefab spawns again with its objects, nested groups and flags, pivot moved to the target.
// Opens a window (it needs an Engine), so it is not registered with ctest: build/tests/prefab_test
#include "test_unique.hpp"
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/Backend/ScenePersistence.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static ScatteredObject* Make(Engine& engine, std::vector<ScatteredObject*>& objs, const char* name, Vector3 pos) {
    auto o = std::make_unique<ScatteredObject>(pos, Vector3{2, 2, 2}, Color{10, 20, 30, 255}, ShapeType::Cube);
    o->SetName(name);
    ScatteredObject* raw = o.get();
    objs.push_back(raw);
    engine.AddEntity(std::move(o));
    return raw;
}

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
    Engine engine(320, 200, "prefab_test", 60);
    std::vector<ScatteredObject*> objs;
    std::vector<std::unique_ptr<ModelGroup>> models;
    ScatteredObject* a = Make(engine, objs, "A", {10, 1, 10});
    ScatteredObject* b = Make(engine, objs, "B", {14, 1, 10});
    ScatteredObject* c = Make(engine, objs, "C", {12, 4, 10});
    ScatteredObject* outside = Make(engine, objs, "Outside", {0, 0, 0});
    (void)outside;
    b->locked = true;

    auto root = std::make_unique<ModelGroup>();
    root->name = "House";
    auto inner = std::make_unique<ModelGroup>();
    inner->name = "Roof";
    root->members = { a, b };
    inner->members = { c };
    for (ScatteredObject* o : root->members) o->parentModel = root.get();
    c->parentModel = inner.get();
    root->AddChild(inner.get());
    ModelGroup* rootPtr = root.get();
    models.push_back(std::move(root));
    models.push_back(std::move(inner));

    const std::string path = (std::filesystem::temp_directory_path() / TestUnique("prefab_test") / "House.flyprefab").string();
    CHECK(SavePrefabToFile(*rootPtr, path, ""));
    CHECK(models.size() == 2 && rootPtr->children.size() == 1);   // saving leaves the scene alone

    std::vector<ScatteredObject*> made;
    ModelGroup* madeRoot = nullptr;
    CHECK(SpawnPrefabFromFile(engine, objs, models, path, "", {100, 5, 200}, &made, &madeRoot));
    CHECK(made.size() == 3);
    CHECK(models.size() == 4 && madeRoot == models[2].get());
    CHECK(madeRoot && madeRoot->name == "House" && madeRoot->parent == nullptr && madeRoot->members.size() == 2);
    CHECK(madeRoot && madeRoot->children.size() == 1 && madeRoot->children[0]->name == "Roof" && madeRoot->children[0]->members.size() == 1);
    CHECK(made.size() == 3 && made[1]->locked && !made[0]->locked);

    // Pivot: middle of the footprint (x 9..15, z 9..11 -> 12, 10), lowest point (y 0). A sat at (10,1,10).
    CHECK(made.size() == 3 && Near(made[0]->GetPosPtr()->x, 98.0f) && Near(made[0]->GetPosPtr()->y, 6.0f) && Near(made[0]->GetPosPtr()->z, 200.0f));
    CHECK(made.size() == 3 && Near(made[2]->GetPosPtr()->x, 100.0f) && Near(made[2]->GetPosPtr()->y, 9.0f));
    CHECK(objs.size() == 7 && Near(a->GetPosPtr()->x, 10.0f));   // the original is untouched

    CHECK(!SpawnPrefabFromFile(engine, objs, models, path + ".missing", "", {0, 0, 0}));
    std::filesystem::remove_all(std::filesystem::path(path).parent_path());

    std::printf(g_fail ? "prefab_test: %d FAILED\n" : "prefab_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
