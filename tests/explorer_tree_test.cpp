// Nested explorer groups and the hide/lock flags must survive a scene save/load, and the tree helpers must keep
// parent/children links consistent. Opens a window (it needs an Engine), so it is not registered with ctest:
// build/tests/explorer_tree_test
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/Backend/ScenePersistence.hpp"

#include <cstdio>
#include <memory>
#include <sstream>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static ScatteredObject* Make(Engine& engine, std::vector<ScatteredObject*>& objs, const char* name) {
    auto o = std::make_unique<ScatteredObject>(Vector3{0, 0, 0}, Vector3{1, 1, 1}, Color{10, 20, 30, 255}, ShapeType::Cube);
    o->SetName(name);
    ScatteredObject* raw = o.get();
    objs.push_back(raw);
    engine.AddEntity(std::move(o));
    return raw;
}

int main() {
    Engine engine(320, 200, "explorer_tree_test", 60);
    std::vector<ScatteredObject*> objs;
    std::vector<std::unique_ptr<ModelGroup>> models;
    ScatteredObject* a = Make(engine, objs, "A");
    ScatteredObject* b = Make(engine, objs, "B");
    ScatteredObject* c = Make(engine, objs, "C");

    // Outer { A, Inner { B } }, C loose. Inner is hidden, Outer is locked, C is hidden on its own.
    models.push_back(std::make_unique<ModelGroup>());
    models.push_back(std::make_unique<ModelGroup>());
    ModelGroup* outer = models[0].get();
    ModelGroup* inner = models[1].get();
    outer->name = "Outer";
    inner->name = "Inner";
    outer->AddChild(inner);
    outer->members.push_back(a); a->parentModel = outer;
    inner->members.push_back(b); b->parentModel = inner;
    inner->hidden = true;
    outer->locked = true;
    c->hidden = true;
    a->AddScript("A"); a->AddScript("A2"); a->AddScript("A3");
    CHECK(a->AllScripts().size() == 3 && a->script == "A");

    CHECK(inner->IsDescendantOf(outer) && !outer->IsDescendantOf(inner));
    CHECK(b->IsHiddenEffective() && !a->IsHiddenEffective());
    CHECK(b->IsLockedEffective() && a->IsLockedEffective() && !c->IsLockedEffective());
    std::vector<ScatteredObject*> all;
    outer->CollectObjects(all);
    CHECK(all.size() == 2);

    const std::string base = "";
    std::string bytes;
    CHECK(SnapshotSceneToMemory(objs, models, base, bytes));

    std::vector<ScatteredObject*> loadedObjs;
    std::vector<std::unique_ptr<ModelGroup>> loaded;
    std::istringstream in(bytes);
    CHECK(RestoreSceneFromMemory(in, engine, loadedObjs, loaded, base));
    CHECK(loadedObjs.size() == 3 && loaded.size() == 2);
    if (loadedObjs.size() == 3 && loaded.size() == 2) {
        ModelGroup* lo = loaded[0].get();
        ModelGroup* li = loaded[1].get();
        CHECK(lo->name == "Outer" && li->name == "Inner");
        CHECK(li->parent == lo && lo->parent == nullptr);
        CHECK(lo->children.size() == 1 && lo->children[0] == li);
        CHECK(li->hidden && !lo->hidden && lo->locked && !li->locked);
        CHECK(loadedObjs[0]->parentModel == lo && loadedObjs[1]->parentModel == li && loadedObjs[2]->parentModel == nullptr);
        CHECK(loadedObjs[2]->hidden && !loadedObjs[0]->hidden);
        CHECK(loadedObjs[0]->script == "A" && loadedObjs[0]->extraScripts.size() == 2 && loadedObjs[0]->extraScripts[1] == "A3");
    }

    // v28: a plugin's tag and source data ride on the group, quotes/newlines/backslashes included.
    const std::string payload = "v1\nrooms \"a\" \\ 3.5\nend";
    outer->pluginTag = "planster.building";
    outer->pluginData = payload;
    std::string taggedBytes;
    CHECK(SnapshotSceneToMemory(objs, models, base, taggedBytes));
    {
        std::vector<ScatteredObject*> o2;
        std::vector<std::unique_ptr<ModelGroup>> m2;
        std::istringstream in2(taggedBytes);
        CHECK(RestoreSceneFromMemory(in2, engine, o2, m2, base));
        CHECK(m2.size() == 2);
        if (m2.size() == 2) {
            CHECK(m2[0]->pluginTag == "planster.building" && m2[0]->pluginData == payload);
            CHECK(m2[1]->pluginTag.empty() && m2[1]->pluginData.empty());
        }
    }

    // A v27 scene (no plugin fields) still loads, with untagged groups.
    {
        std::vector<ScatteredObject*> o3;
        std::vector<std::unique_ptr<ModelGroup>> m3;
        std::istringstream in3("SIMPLE_ENGINE_BUILD 27\n0\n1\n\"G\" 0\n\n-1 1 0\n");
        CHECK(RestoreSceneFromMemory(in3, engine, o3, m3, base));
        CHECK(m3.size() == 1 && m3[0]->name == "G" && m3[0]->hidden && m3[0]->pluginTag.empty());
    }

    a->RemoveScript("A");
    CHECK(a->script == "A2" && a->extraScripts.size() == 1);

    // Detaching a child leaves no stale links.
    outer->RemoveChild(inner);
    CHECK(inner->parent == nullptr && outer->children.empty());

    std::printf(g_fail ? "explorer_tree_test: %d FAILED\n" : "explorer_tree_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
