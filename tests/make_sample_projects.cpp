// Writes the sample projects in ScriptingSDK/samples/projects (the New Project templates "Coin Dash", "Physics
// Playground" and "Campfire Valley") into the directory given as argument 1 (default: that directory under the
// source tree): each project's .flyproj and, for Campfire Valley, its terrain and foliage. The Scripts/ folders are
// ordinary source files kept in the tree and are left alone. Also loads every project back and checks it.
// Opens a window; run it by hand after changing a sample: build/tests/make_sample_projects [output dir]
#include "raylib.h"
#include "raymath.h"
#include "Engine.hpp"
#include "Engine/Graphics.hpp"
#include "Engine/Lighting.hpp"
#include "Engine/Backend/ScenePersistence.hpp"
#include "Engine/Frontend/ProjectManager.hpp"
#include "Engine/Scripts/ScriptRuntime.hpp"
#include "../include/Terrain/BasicTerrain.hpp"
#include "../include/Terrain/Foliage.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

namespace {

struct Sample {
    std::string folder;    // directory name, no spaces
    std::string title;     // template name in the New Project dialog
    std::string script;    // FLY_SCRIPT class that runs on Play
};

std::unique_ptr<ScatteredObject> Block(Vector3 pos, Vector3 size, Color color, const char* name, ShapeType shape = ShapeType::Cube) {
    auto o = std::make_unique<ScatteredObject>(pos, size, color, shape);
    o->SetName(name);
    o->anchored = true;
    return o;
}

void WriteProject(const fs::path& dir, const Sample& s, const std::vector<ScatteredObject*>& objects) {
    fs::create_directories(dir);
    std::ofstream out(dir / (s.folder + ".flyproj"), std::ios::trunc);
    out << "FLYENGINE_PROJECT 1\n"
        << "Name = \"" << s.title << "\"\n"
        << "Template = \"" << s.title << "\"\n"
        << "Created = 0\nModified = 0\n---SCENE---\n";
    const std::vector<std::unique_ptr<ModelGroup>> models;
    CHECK(SaveSceneToStream(out, objects, models, dir.string()));
}

// Is any plant of `rule` within `radius` of (x, z)?
bool PlantsNear(const BasicTerrain& t, const foliage::Rule& rule, float x, float z, float radius) {
    std::vector<Vector3> p;
    foliage::Scatter(t, rule, x - radius, z - radius, x + radius, z + radius, p);
    for (const Vector3& v : p) if (std::hypot(v.x - x, v.z - z) < radius) return true;
    return false;
}

} // namespace

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::path(SAMPLES_DIR);
    Engine engine(640, 360, "make_sample_projects", 60);
    engine.SetPlayerBuild(true);

    ScriptRuntime runtime;
    SetActiveRuntime(&runtime);

    const Sample coin{ "CoinDash", "Coin Dash", "CoinDash" };
    const Sample physics{ "PhysicsPlayground", "Physics Playground", "PhysicsPlayground" };
    const Sample valley{ "CampfireValley", "Campfire Valley", "CampfireValley" };

    // ---- Coin Dash: a ground and a start pad; the script builds the course ----
    {
        gfx::Lighting() = gfx::NewSceneLighting();
        gfx::Lighting().timeOfDay = 11.0f;
        runtime.StandaloneScripts() = { { coin.title, coin.script, true } };
        auto ground = Block({ 0, -0.5f, 0 }, { 90, 1, 90 }, { 96, 160, 90, 255 }, "Ground");
        auto pad = Block({ 0, 0.1f, 0 }, { 6, 0.2f, 6 }, { 235, 235, 240, 255 }, "Start Pad");
        WriteProject(root / coin.folder, coin, { ground.get(), pad.get() });
    }

    // ---- Physics Playground: a big floor ----
    {
        gfx::Lighting() = gfx::NewSceneLighting();
        gfx::Lighting().timeOfDay = 14.0f;
        runtime.StandaloneScripts() = { { physics.title, physics.script, true } };
        auto floor = Block({ 0, -0.5f, 8 }, { 90, 1, 70 }, { 150, 150, 155, 255 }, "Floor");
        WriteProject(root / physics.folder, physics, { floor.get() });
    }

    // ---- Campfire Valley: terrain with foliage ----
    {
        gfx::Lighting() = gfx::NewSceneLighting();
        gfx::Lighting().timeOfDay = 18.0f;
        runtime.StandaloneScripts() = { { valley.title, valley.script, true } };

        auto owner = std::make_unique<BasicTerrain>(128, 128, 3.0f, 60.0f, 24.0f);
        BasicTerrain* t = owner.get();
        t->SetName("Valley");
        t->GenerateFlat(0.0f);
        t->genBoxPos = { 0.0f, 10.0f, 0.0f };
        t->genBoxSize = { 384.0f, 26.0f, 384.0f };
        t->genSeed = 2024;
        t->genHills = true;
        t->genMountains = false;
        t->genTextures = true;
        t->GenerateTerrainInBox();
        t->position.y = -t->GetHeightAt(0.0f, 0.0f);   // the ground under the start is at y = 0
        t->MarkAllDirty();
        engine.AddEntity(std::move(owner));

        // Plants, with a seed per kind chosen so that no tree, bush or rock stands where the player starts or in the camp.
        foliage::Settings& set = foliage::For(t);
        set.wind = 0.7f;
        const float campX = 7.0f, campZ = 9.0f;
        for (foliage::Kind kind : { foliage::Kind::Grass, foliage::Kind::Flowers, foliage::Kind::Bush, foliage::Kind::Tree,
                                    foliage::Kind::Pine, foliage::Kind::Rock }) {
            foliage::Rule rule = foliage::DefaultRule(kind);
            if (kind == foliage::Kind::Tree || kind == foliage::Kind::Pine) rule.density = 0.35f;
            if (kind == foliage::Kind::Bush) rule.density = 1.0f;
            if (kind == foliage::Kind::Rock) rule.density = 0.4f;
            if (kind == foliage::Kind::Bush || kind == foliage::Kind::Tree || kind == foliage::Kind::Pine || kind == foliage::Kind::Rock) {
                int seed = 1;
                for (; seed < 400; seed++) {
                    rule.seed = seed;
                    if (!PlantsNear(*t, rule, 0.0f, 0.0f, 6.0f) && !PlantsNear(*t, rule, campX, campZ, 6.5f)) break;
                }
                CHECK(seed < 400);
            }
            set.rules.push_back(rule);
        }
        WriteProject(root / valley.folder, valley, {});
        engine.RemoveEntity(t);
        foliage::Forget(t);
    }
    SetActiveRuntime(nullptr);

    // ---- load every project back the way the editor does ----
    for (const Sample* s : { &coin, &physics, &valley }) {
        ScriptRuntime rt;
        SetActiveRuntime(&rt);
        std::vector<ScatteredObject*> objects;
        std::vector<std::unique_ptr<ModelGroup>> models;
        project::Info info;
        const fs::path file = root / s->folder / (s->folder + ".flyproj");
        const bool ok = project::OpenProjectFileFromPath(file.string(), engine, objects, models, info);
        CHECK(ok);
        CHECK(info.name == s->title);
        CHECK(rt.StandaloneScripts().size() == 1);
        if (!rt.StandaloneScripts().empty()) {
            CHECK(rt.StandaloneScripts()[0].typeName == s->script);
            CHECK(rt.StandaloneScripts()[0].runOnPlay);
        }
        CHECK(fs::exists(root / s->folder / "Scripts" / (s->script + ".cpp")));
        if (s == &valley) {
            CHECK(BasicTerrain::GetInstances().size() == 1);
            if (!BasicTerrain::GetInstances().empty()) {
                BasicTerrain* t = BasicTerrain::GetInstances()[0];
                CHECK(foliage::Has(t));
                CHECK(foliage::For(t).rules.size() == 6);
                CHECK(std::fabs(t->GetHeightAt(0.0f, 0.0f) + t->position.y) < 0.01f);
                std::printf("valley: terrain %dx%d, %zu foliage rules, ground at start %.2f\n", t->GetWidth(), t->GetDepth(),
                            foliage::For(t).rules.size(), t->position.y + t->GetHeightAt(0.0f, 0.0f));
            }
            for (BasicTerrain* t : std::vector<BasicTerrain*>(BasicTerrain::GetInstances())) { foliage::Forget(t); engine.RemoveEntity(t); }
        } else {
            CHECK(objects.size() == 2 || objects.size() == 1);
        }
        for (ScatteredObject* o : objects) engine.RemoveEntity(o);
        SetActiveRuntime(nullptr);
        std::printf("%-18s loaded: %zu object(s), %zu script(s)\n", s->title.c_str(), objects.size(), rt.StandaloneScripts().size());
    }

    std::printf(g_fail ? "make_sample_projects: %d FAILED\n" : "make_sample_projects: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
