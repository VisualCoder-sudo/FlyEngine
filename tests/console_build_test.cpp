// The console's building commands (what the AI's Build mode runs): spawn, delete, clone, rename, against the real engine in
// edit mode (not playing). Needs a display; not registered with ctest: build/tests/console_build_test
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/Backend/PhysicsSimulation.hpp"
#include "Terrain/Water/WaterBody.hpp"
#include "Terrain/TerrainRegistry.hpp"
#include "CityGen/City.hpp"
#include "Engine/Backend/ScatteredObject.hpp"
#include "Engine/Scripts/ScriptRuntime.hpp"
#include "Engine/Scripts/FlyScriptApi.hpp"
#include "Engine/Scripts/CommandConsole.hpp"
#include "Engine/Frontend/ui.hpp"
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static bool Run(const std::string& line, std::string* out = nullptr) {
    std::string text;
    const bool ok = console::RunLine(line, &text);
    if (out) *out = text;
    if (!ok) std::printf("  (%s -> %s)\n", line.c_str(), text.c_str());
    return ok;
}

int main() {
    Engine engine(320, 240, "console_build_test", 60);
    std::vector<ScatteredObject*> objs;
    std::vector<std::unique_ptr<ModelGroup>> models;
    auto simOwner = std::make_unique<phys::Simulation>(objs);
    phys::Simulation& sim = *simOwner;
    engine.AddEntity(std::move(simOwner));
    auto rtOwner = std::make_unique<ScriptRuntime>();
    ScriptRuntime& rt = *rtOwner;
    engine.AddEntity(std::move(rtOwner));
    rt.BindWorld(objs, models, engine.GetCamera(), sim, engine);
    FlyNative_BindRuntime(&rt);

    std::string out;
    CHECK(Run("spawn Cube Wall Position=(0,1,0) Size=(10,2,1) Color=(200,60,60)", &out));
    CHECK(out == "Created Cube 'Wall'");
    const unsigned long long wall = FlyNative_FindObject("Wall");
    CHECK(wall != 0 && objs.size() == 1);
    float x, y, z;
    FlyNative_GetPosition(wall, &x, &y, &z); CHECK(x == 0 && y == 1 && z == 0);
    FlyNative_GetSize(wall, &x, &y, &z); CHECK(x == 10 && y == 2 && z == 1);
    unsigned char r, g, b;
    FlyNative_GetColor(wall, &r, &g, &b); CHECK(r == 200 && g == 60 && b == 60);
    CHECK(FlyNative_GetAnchored(wall) != 0);                               // scenery by default

    CHECK(Run("spawn Sphere Ball Anchored=false Position=(0, 5, 0) Mass=2"));
    CHECK(FlyNative_GetAnchored(FlyNative_FindObject("Ball")) == 0);
    CHECK(Run("spawn Cube Spaced Size = (2, 2, 2) Transparency = 0.5"));   // spaces around = and inside the tuple
    FlyNative_GetSize(FlyNative_FindObject("Spaced"), &x, &y, &z); CHECK(x == 2 && y == 2 && z == 2);

    // Names: unique, generated when missing, quoted when they have a space.
    CHECK(!Run("spawn Cube Wall", &out) && out.find("already exists") != std::string::npos);
    CHECK(Run("spawn cube", &out) && out == "Created Cube 'Cube'");        // shape names are case-insensitive
    CHECK(Run("spawn Cube", &out) && out == "Created Cube 'Cube2'");
    CHECK(Run("spawn Cylinder \"My Box\""));
    CHECK(FlyNative_FindObject("My Box") != 0);

    // All or nothing: a bad property creates nothing.
    const size_t before = objs.size();
    CHECK(!Run("spawn Cube Bad Position=(x)", &out) && objs.size() == before && FlyNative_FindObject("Bad") == 0);
    CHECK(!Run("spawn Cube Bad2 Weight=3", &out) && out.find("Unknown property") != std::string::npos && FlyNative_FindObject("Bad2") == 0);
    CHECK(!Run("spawn Pyramid", &out) && out.find("Unknown shape") != std::string::npos);
    CHECK(!Run("spawn Cube Bad3 Size", &out) && FlyNative_FindObject("Bad3") == 0);
    CHECK(!Run("spawn Cube Bad.Name", &out));                              // a dot would read as a property path
    CHECK(objs.size() == before);

    // clone / rename / delete.
    CHECK(Run("clone Wall", &out) && out.find("'Wall2'") != std::string::npos && FlyNative_FindObject("Wall2") != 0);
    CHECK(Run("clone Wall Wall3") && FlyNative_FindObject("Wall3") != 0);
    FlyNative_GetSize(FlyNative_FindObject("Wall3"), &x, &y, &z); CHECK(x == 10);   // a copy keeps the look
    CHECK(!Run("clone Nope"));
    CHECK(Run("rename Wall2 Fence") && FlyNative_FindObject("Fence") != 0 && FlyNative_FindObject("Wall2") == 0);
    CHECK(!Run("rename Fence Wall", &out) && out.find("already exists") != std::string::npos);
    CHECK(!Run("rename Missing X"));

    const size_t count = objs.size();
    CHECK(Run("delete Fence") && FlyNative_FindObject("Fence") == 0 && objs.size() == count - 1);
    CHECK(Run("delete \"My Box\"") && FlyNative_FindObject("My Box") == 0);
    CHECK(!Run("delete Fence", &out) && out.find("No object named") != std::string::npos);

    // A selected object that is deleted is dropped from the selection (nothing may point at freed memory).
    auto* ball = reinterpret_cast<ScatteredObject*>(static_cast<uintptr_t>(FlyNative_FindObject("Ball")));
    ui::SetSelection({ ball }, ball);
    CHECK(Run("delete Ball"));
    CHECK(ui::GetSelection().empty());

    // The normal property syntax still works on what was built.
    CHECK(Run("Wall.Color = (10, 20, 30)"));
    CHECK(Run("Wall.Position", &out) && out.find("(0, 1, 0)") != std::string::npos);

    // The example in docs/guide/ConsoleCommands.md (which the AI is given) really runs.
    CHECK(Run("spawn Cube Floor Position=(0,-0.5,0) Size=(30,1,30) Color=(90,140,90)"));
    CHECK(Run("spawn Cube Wall1 Position=(0,2,-15) Size=(30,4,1) Color=(200,200,200)"));
    CHECK(Run("spawn Sphere Ball Position=(0,6,0) Size=(1,1,1) Color=(220,40,40) Anchored=false"));
    CHECK(FlyNative_GetAnchored(FlyNative_FindObject("Floor")) != 0 && FlyNative_GetAnchored(FlyNative_FindObject("Ball")) == 0);

    // Water bodies and terrain.
    const size_t waters = WaterBody::GetInstances().size();
    const size_t terrains = terrain::GetTerrainRegistry().Count();
    CHECK(Run("water Lake Position=(0,0,20) Size=(60,1,40) Color=(20,90,200,150) Height=-0.5", &out) && out == "Created water body 'Lake'");
    CHECK(WaterBody::GetInstances().size() == waters + 1);
    CHECK(Run("water", &out) && out == "Created water body 'Water'");
    CHECK(!Run("water Lake", &out) && out.find("already exists") != std::string::npos);
    CHECK(!Run("water Bad Wobble=3", &out) && out.find("Unknown water property") != std::string::npos);
    CHECK(!Run("water Bad Color=(300,0,0)", &out) && out.find("Color") != std::string::npos);
    CHECK(WaterBody::GetInstances().size() == waters + 2);                 // a bad line makes nothing
    CHECK(Run("terrain Hills Size=(400,400) Hills=15 Seed=7", &out) && out == "Created terrain 'Hills'");
    CHECK(Run("terrain", &out) && out == "Created terrain 'Terrain'");
    CHECK(!Run("terrain Bad Size=(10,10)", &out) && out.find("Size") != std::string::npos);
    CHECK(!Run("terrain Hills", &out) && out.find("already exists") != std::string::npos);
    CHECK(terrain::GetTerrainRegistry().Count() == terrains + 2);
    CHECK(!Run("spawn Cube Lake", &out) && out.find("already exists") != std::string::npos);   // names are shared
    CHECK(Run("rename Lake Pond") && terrain::GetTerrainRegistry().Count() == terrains + 2);
    CHECK(Run("delete Pond", &out) && out == "Deleted water 'Pond'");
    CHECK(Run("delete Water") && Run("delete Hills", &out) && out == "Deleted terrain 'Hills'");
    CHECK(Run("delete Terrain"));
    CHECK(WaterBody::GetInstances().size() == waters && terrain::GetTerrainRegistry().Count() == terrains);
    CHECK(!Run("delete Pond", &out) && out.find("No object named") != std::string::npos);

    // Nodes, unions, cities.
    CHECK(Run("spawn Cube Lamp Size=(0.3,0.3,0.3) Position=(0,4,0) Transparency=1 CanCollide=false"));
    auto* lamp = reinterpret_cast<ScatteredObject*>(static_cast<uintptr_t>(FlyNative_FindObject("Lamp")));
    CHECK(Run("node Lamp PointLight Color=(255,200,120) Brightness=3 Range=20 Name=Glow", &out) && out.find("Added") == 0);
    CHECK(lamp->nodes.size() == 1 && lamp->nodes[0].name == "Glow" && lamp->nodes[0].range == 20.0f && lamp->nodes[0].IsLight());
    CHECK(Run("node Lamp Decal Texture=Textures/a.png Face=Top Transparency=0.5") && lamp->nodes.size() == 2);
    CHECK(!Run("node Lamp Decal Texture=../secret.png", &out) && lamp->nodes.size() == 2);
    CHECK(!Run("node Lamp Candle", &out) && out.find("Unknown node type") != std::string::npos);
    CHECK(!Run("node Lamp PointLight Wobble=1", &out) && lamp->nodes.size() == 2);
    CHECK(!Run("node Nope PointLight") && !Run("delnode Lamp Missing"));
    CHECK(Run("delnode Lamp Glow") && lamp->nodes.size() == 1);

    CHECK(Run("spawn Cube PartA Position=(0,1,0) Size=(2,2,2)") && Run("spawn Cube PartB Position=(1,1,0) Size=(2,2,2)"));
    CHECK(Run("union Block PartA PartB", &out) && out == "Created union 'Block'");
    CHECK(FlyNative_FindObject("Block") != 0 && FlyNative_FindObject("PartA") == 0);
    CHECK(Run("separate Block", &out) && out.find("2 parts") != std::string::npos && FlyNative_FindObject("PartA") != 0);
    CHECK(!Run("union X PartA", &out));
    // A cutter (Negate) is carved out; UseGlobalColor paints the union one colour.
    CHECK(Run("spawn Cube Slab Position=(0,1,5) Size=(4,4,1)") && Run("spawn Cylinder Cutter Position=(0,1,5) Size=(2,2,2) Negate=true"));
    CHECK(Run("Cutter.Negate", &out) && out.find("true") != std::string::npos);
    CHECK(Run("union Doorway Slab Cutter") && Run("Doorway.UseGlobalColor = true"));
    CHECK(Run("Doorway.UseGlobalColor", &out) && out == "true");
    CHECK(!Run("Wall.UseGlobalColor = true", &out));                         // only a union has it

    const size_t cities = city::GetCityRegistry().Count();
    CHECK(Run("city Town Grid=(3,3) Seed=5 Position=(200,200)", &out) && out == "Created city 'Town'");
    CHECK(!Run("city Town", &out) && !Run("city Bad Grid=(1,1)", &out));
    CHECK(city::GetCityRegistry().Count() == cities + 1);
    CHECK(Run("rename Town Metro") && Run("delete Metro", &out) && out == "Deleted city 'Metro'");
    CHECK(city::GetCityRegistry().Count() == cities);
    CHECK(!Run("prefab ../x.flyprefab") && !Run("prefab Missing.flyprefab"));

    std::printf(g_fail ? "console_build_test: %d failure(s)\n" : "console_build_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
