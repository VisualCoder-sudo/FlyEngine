// Writes the sample projects in assets/templates (the New Project templates "Coin Dash", "Physics
// Playground", "Campfire Valley", "Island" and "Night City") into the directory given as argument 1 (default: that directory under the
// source tree): each project's .flyproj and, for Campfire Valley, its terrain and foliage (into deps/). The deps/Scripts
// folders, data.json and thumbnail.png are ordinary files kept in the tree and are left alone. Also loads every project back and checks it.
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
#include "../include/Terrain/Water/WaterBody.hpp"
#include "../include/CityGen/City.hpp"

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
    std::string script;    // FLY_SCRIPT class that runs on Play; empty = a scenery template without a script
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
    out.close();
    // Everything the scene saved next to the .flyproj (terrain, foliage) belongs in deps/, which a template copies into
    // the new project's root.
    std::vector<fs::path> extras;
    for (const fs::directory_entry& e : fs::directory_iterator(dir)) {
        const std::string name = e.path().filename().string();
        if (e.is_regular_file() && e.path().extension() != ".flyproj" && name != "data.json" && name != "thumbnail.png") extras.push_back(e.path());
    }
    for (const fs::path& f : extras) {
        fs::create_directories(dir / "deps");
        fs::rename(f, dir / "deps" / f.filename());
    }
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
    const Sample island{ "Island", "Island", "" };
    const Sample night{ "NightCity", "Night City", "" };

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
    // ---- Island: a hilly island in an ocean, evening light ----
    {
        gfx::Lighting() = gfx::NewSceneLighting();
        gfx::Lighting().timeOfDay = 17.3f;
        gfx::Lighting().cloudCoverage = 0.3f;
        gfx::Lighting().haze = 2.6f;
        runtime.StandaloneScripts() = {};

        auto owner = std::make_unique<BasicTerrain>(384, 384, 3.0f, 60.0f, 48.0f);
        BasicTerrain* t = owner.get();
        t->SetName("Island");
        t->GenerateFlat(0.0f);
        t->genBoxPos = { 0.0f, 6.0f, 0.0f };
        t->genBoxSize = { 576.0f, 34.0f, 576.0f };
        t->genSeed = 77;
        t->genHills = true;
        t->genMountains = false;
        t->genTextures = true;
        t->GenerateTerrainInBox();
        // The generated hills are only a relief (their height depends on the generator, and can come out below 0), so
        // rescale them to 8..32 m over the island and pull the edges down to the sea floor with a radial mask.
        const int w = t->GetWidth(), d = t->GetDepth();
        float* h = t->GetHeightData();
        const auto radius = [&](int x, int z) {
            const float nx = (x - (w - 1) * 0.5f) * 3.0f / 288.0f, nz = (z - (d - 1) * 0.5f) * 3.0f / 288.0f;   // 1 = 288 m out
            return std::sqrt(nx * nx + nz * nz);
        };
        float lo = 1e9f, hi = -1e9f;
        for (int z = 0; z < d; z++) for (int x = 0; x < w; x++) if (radius(x, z) < 0.6f) { lo = std::min(lo, h[z * w + x]); hi = std::max(hi, h[z * w + x]); }
        CHECK(hi - lo > 1.0f);
        for (int z = 0; z < d; z++) {
            for (int x = 0; x < w; x++) {
                float m = std::min(1.0f, std::max(0.0f, (0.85f - radius(x, z)) / 0.5f));
                m = m * m * (3.0f - 2.0f * m);
                const float rel = (h[z * w + x] - lo) / std::max(hi - lo, 0.001f);
                h[z * w + x] = m * (8.0f + 24.0f * std::min(1.0f, std::max(0.0f, rel)));
            }
        }
        t->position.y = -7.0f;   // the sea (y = 0) then reaches the shore where the ground is 7 m up

        // Paint it: sand on the beach, grass on the land, rocky grass on steep and high ground. The paint brush and this
        // generator run without the terrain textures loaded, so the layers are named by their slot: the files in
        // assets/Textures/TerrainTextures load in alphabetical order (Dirt, LeafyGrass, RockyGrass, Sand).
        const int grass = 1, rocky = 2, sand = 3;
        {
            const int sw = t->GetSplatWidth(), sd = t->GetSplatDepth();
            const float sx = w * 3.0f / sw, sz = d * 3.0f / sd;
            uint8_t* splat = t->GetSplatData();
            for (int z = 0; z < sd; z++) {
                for (int x = 0; x < sw; x++) {
                    const float wx = (x - sw * 0.5f) * sx, wz = (z - sd * 0.5f) * sz;
                    const float alt = t->position.y + t->GetHeightAt(wx, wz);
                    const float slope = 1.0f - t->GetNormalAt(wx, wz).y;
                    float wgt[3] = { std::min(1.0f, std::max(0.0f, (3.0f - alt) / 2.0f)),                        // sand
                                     std::min(1.0f, std::max(0.0f, (alt - 2.0f) / 2.0f)),                        // grass
                                     std::min(1.0f, std::max(slope * 3.0f - 0.35f, (alt - 24.0f) / 8.0f)) };     // rocky
                    wgt[1] *= 1.0f - wgt[2];
                    wgt[0] *= 1.0f - wgt[2];
                    const float sum = std::max(0.001f, wgt[0] + wgt[1] + wgt[2]);
                    uint8_t* px = splat + (static_cast<size_t>(z) * sw + x) * 4;
                    px[0] = px[1] = px[2] = px[3] = 0;
                    int total = 0;
                    const int idx[3] = { sand, grass, rocky };
                    for (int k = 0; k < 3; k++) { px[idx[k]] = static_cast<uint8_t>(wgt[k] / sum * 255.0f); total += px[idx[k]]; }
                    px[grass] = static_cast<uint8_t>(px[grass] + (255 - total));   // channels sum to exactly 255
                }
            }
        }
        t->MarkAllDirty();
        engine.AddEntity(std::move(owner));

        auto sea = std::make_unique<WaterBody>(Vector3{ 0.0f, 0.0f, 0.0f }, Vector3{ 1152.0f, 10.0f, 1152.0f }, 0.0f);
        sea->SetName("Sea");
        WaterBody* seaPtr = sea.get();
        engine.AddEntity(std::move(sea));

        foliage::Settings& set = foliage::For(t);
        set.wind = 0.6f;
        for (foliage::Kind kind : { foliage::Kind::Grass, foliage::Kind::Flowers, foliage::Kind::Bush, foliage::Kind::Tree, foliage::Kind::Rock }) {
            foliage::Rule rule = foliage::DefaultRule(kind);
            rule.minAltitude = 1.5f;                  // nothing grows on the beach or under water
            if (kind != foliage::Kind::Rock) rule.layer = grass;
            if (kind == foliage::Kind::Tree) rule.density = 0.5f;
            if (kind == foliage::Kind::Rock) rule.density = 0.5f;
            set.rules.push_back(rule);
        }
        WriteProject(root / island.folder, island, {});
        engine.RemoveEntity(seaPtr);
        engine.RemoveEntity(t);
        foliage::Forget(t);
    }

    // ---- Night City: a lit, wet city with traffic, pedestrians and buses ----
    {
        gfx::Lighting() = gfx::NewSceneLighting();
        gfx::Lighting().timeOfDay = 22.0f;
        gfx::Lighting().stars = 1.5f;
        gfx::Lighting().moonLight = 1.5f;
        gfx::Lighting().bloom = 0.6f;
        gfx::Lighting().wetGround = 0.7f;
        runtime.StandaloneScripts() = {};

        auto owner = std::make_unique<city::City>();
        city::City* c = owner.get();
        city::GetCityRegistry().Register(c);   // what the editor does for a new city: only registered cities are saved
        engine.AddEntity(std::move(owner));
        c->SetName("Night City");
        c->GetParams().gridX = 7;
        c->GetParams().gridZ = 7;
        c->GetParams().cellSize = 42.0f;
        c->GetParams().seed = 2077;
        c->GetParams().style = static_cast<int>(city::BuildingStyle::Modern);
        c->GetParams().avgHeight = 20.0f;
        c->GetParams().shapeVariety = 0.6f;
        c->GetParams().furniture = true;
        c->GetParams().cars = 30;
        c->GetParams().pedestrians = 25;
        c->GenerateGrid({ 0.0f, 0.0f });
        c->AutoDistricts(3.5f);
        c->AutoTransit(2, 5);
        c->RebuildAll();
        WriteProject(root / night.folder, night, {});
        city::GetCityRegistry().Unregister(c);
        engine.RemoveEntity(c);
    }
    SetActiveRuntime(nullptr);

    // ---- load every project back the way the editor does ----
    for (const Sample* s : { &coin, &physics, &valley, &island, &night }) {
        ScriptRuntime rt;
        SetActiveRuntime(&rt);
        std::vector<ScatteredObject*> objects;
        std::vector<std::unique_ptr<ModelGroup>> models;
        project::Info info;
        // A template is not a project until deps/ is copied next to its .flyproj, so load a scratch copy.
        const fs::path scratch = fs::temp_directory_path() / ("make_sample_projects_" + s->folder);
        fs::remove_all(scratch);
        fs::create_directories(scratch);
        if (fs::exists(root / s->folder / "deps")) fs::copy(root / s->folder / "deps", scratch, fs::copy_options::recursive);
        fs::copy_file(root / s->folder / (s->folder + ".flyproj"), scratch / (s->folder + ".flyproj"));
        const fs::path file = scratch / (s->folder + ".flyproj");
        const bool ok = project::OpenProjectFileFromPath(file.string(), engine, objects, models, info);
        CHECK(ok);
        CHECK(info.name == s->title);
        CHECK(rt.StandaloneScripts().size() == (s->script.empty() ? 0u : 1u));
        if (!rt.StandaloneScripts().empty()) {
            CHECK(rt.StandaloneScripts()[0].typeName == s->script);
            CHECK(rt.StandaloneScripts()[0].runOnPlay);
        }
        if (!s->script.empty()) CHECK(fs::exists(root / s->folder / "deps" / "Scripts" / (s->script + ".cpp")));
        if (s == &island) {
            CHECK(BasicTerrain::GetInstances().size() == 1);
            CHECK(WaterBody::GetInstances().size() == 1);
            for (BasicTerrain* t : std::vector<BasicTerrain*>(BasicTerrain::GetInstances())) { foliage::Forget(t); engine.RemoveEntity(t); }
            for (WaterBody* wb : std::vector<WaterBody*>(WaterBody::GetInstances())) engine.RemoveEntity(wb);
        } else if (s == &night) {
            CHECK(city::GetCityRegistry().Count() == 1);
            for (city::City* c : std::vector<city::City*>(city::GetCityRegistry().GetCities())) { city::GetCityRegistry().Unregister(c); engine.RemoveEntity(c); }
        } else if (s == &valley) {
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
