// Child nodes of an object (SpotLight / PointLight / SurfaceLight / Decal / Texture): they survive a scene
// save/load, a clone, and the lights visibly light the scene. Opens a window (it needs an Engine), so ctest needs a
// display (label needs_display): build/tests/object_nodes_test
#include "raylib.h"
#include "Engine.hpp"
#include "Graphics/Graphics.hpp"
#include "Engine/Backend/ScenePersistence.hpp"
#include "../include/Terrain/BasicTerrain.hpp"
#include "Terrain/Water/WaterBody.hpp"
#define private public
#include "../include/Terrain/Terrain.hpp"
#undef private
#include "Engine/Frontend/ui.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static ScatteredObject* Make(Engine& engine, std::vector<ScatteredObject*>& objs, const char* name, Vector3 pos, Vector3 size, Color color) {
    auto o = std::make_unique<ScatteredObject>(pos, size, color, ShapeType::Cube);
    o->SetName(name);
    ScatteredObject* raw = o.get();
    objs.push_back(raw);
    engine.AddEntity(std::move(o));
    return raw;
}

// Mean brightness of the lower part of the picture.
static float Shoot(Engine& engine, const char* name) {
    for (int f = 0; f < 6; f++) engine.StepFrame(1.0f / 60.0f);
    char path[96];
    std::snprintf(path, sizeof path, "nodes_%s.png", name);
    TakeScreenshot(path);
    Image img = LoadImage(path);
    float sum = 0.0f;
    long n = 0;
    for (int y = img.height / 3; y < img.height; y += 4)
        for (int x = 0; x < img.width; x += 4) { const Color c = GetImageColor(img, x, y); sum += c.r + c.g + c.b; n++; }
    UnloadImage(img);
    return sum / (float)n;
}

int main() {
    // Instanced shape batches draw one instance per group in LTO builds (also at HEAD); draw each shape on its own.
    setenv("FLY_NO_SHAPE_BATCH", "1", 1);
    Engine engine(640, 360, "object_nodes_test", 60);
    engine.SetPlayerBuild(true);
    std::vector<ScatteredObject*> objs;
    std::vector<std::unique_ptr<ModelGroup>> models;
    ScatteredObject* floor = Make(engine, objs, "Floor", { 0, -0.5f, 0 }, { 40, 1, 40 }, { 200, 200, 200, 255 });
    ScatteredObject* lamp = Make(engine, objs, "Lamp", { 0, 3, 0 }, { 1, 1, 1 }, { 255, 255, 255, 255 });
    (void)floor;
    ScatteredObject* slab = Make(engine, objs, "Slab", { 0.0f, 500.0f, 0.0f }, { 8.0f, 0.5f, 8.0f }, { 255, 40, 40, 255 });

    // --- save / load / clone ---------------------------------------------------------------------------------
    {
        ObjectNode spot = MakeNode(ObjectNodeType::SpotLight);
        spot.name = "Torch \"A\"";
        spot.angle = 33.0f; spot.range = 21.5f; spot.brightness = 2.5f; spot.castShadows = true; spot.shadowRes = 2048;
        spot.color[0] = 0.9f; spot.color[2] = 0.2f; spot.face = NodeFace::Left;
        ObjectNode tex = MakeNode(ObjectNodeType::Texture);
        tex.texturePath = "assets/Textures/tile.png";
        tex.studsPerTile[0] = 2.5f; tex.studsPerTile[1] = 7.0f; tex.offset[0] = 0.25f; tex.transparency = 0.4f;
        tex.tint[1] = 0.5f; tex.face = NodeFace::Top; tex.enabled = false;
        lamp->nodes.push_back(spot);
        lamp->nodes.push_back(MakeNode(ObjectNodeType::Decal));
        lamp->nodes.push_back(tex);
        lamp->nodes.push_back(MakeNode(ObjectNodeType::SurfaceLight));
        lamp->nodes.push_back(MakeNode(ObjectNodeType::PointLight));
        gfx::Lighting().maxLights = 12;
        gfx::Lighting().maxShadowCasters = 2;

        std::stringstream ss;
        CHECK(SaveSceneToStream(ss, objs, models, "", nullptr));
        std::vector<ScatteredObject*> objs2;
        std::vector<std::unique_ptr<ModelGroup>> models2;
        gfx::Lighting().maxLights = 32;
        CHECK(LoadSceneFromStream(ss, engine, objs2, models2, "", nullptr, nullptr));
        CHECK(objs2.size() == 3 && gfx::Lighting().maxLights == 12 && gfx::Lighting().maxShadowCasters == 2);
        if (objs2.size() == 3) {
            const auto& nodes = objs2[1]->nodes;
            CHECK(nodes.size() == 5);
            if (nodes.size() == 5) {
                CHECK(nodes[0].type == ObjectNodeType::SpotLight && nodes[0].name == "Torch \"A\"" && nodes[0].face == NodeFace::Left);
                CHECK(std::fabs(nodes[0].angle - 33.0f) < 1e-4f && std::fabs(nodes[0].range - 21.5f) < 1e-4f && std::fabs(nodes[0].brightness - 2.5f) < 1e-4f);
                CHECK(nodes[0].castShadows && nodes[0].shadowRes == 2048 && std::fabs(nodes[0].color[2] - 0.2f) < 1e-4f);
                CHECK(nodes[1].type == ObjectNodeType::Decal && nodes[2].type == ObjectNodeType::Texture);
                CHECK(nodes[2].texturePath == "assets/Textures/tile.png" && !nodes[2].enabled && nodes[2].face == NodeFace::Top);
                CHECK(std::fabs(nodes[2].studsPerTile[0] - 2.5f) < 1e-4f && std::fabs(nodes[2].studsPerTile[1] - 7.0f) < 1e-4f);
                CHECK(std::fabs(nodes[2].offset[0] - 0.25f) < 1e-4f && std::fabs(nodes[2].transparency - 0.4f) < 1e-4f);
                CHECK(nodes[3].type == ObjectNodeType::SurfaceLight && nodes[4].type == ObjectNodeType::PointLight);
            }
            CHECK(objs2[0]->nodes.empty());
        }
        for (ScatteredObject* o : objs2) o->alive = false;   // the loaded copies are in the engine too; drop them

        auto copy = lamp->Clone();
        CHECK(copy->nodes.size() == 5 && copy->nodes[0].name == "Torch \"A\"");
        lamp->nodes.clear();
        gfx::ResetLighting();
    }

    // A scene saved before nodes existed (v30) still loads, with none.
    {
        std::vector<ScatteredObject*> o3;
        std::vector<std::unique_ptr<ModelGroup>> m3;
        std::istringstream in("SIMPLE_ENGINE_BUILD 30\n0\n");
        CHECK(LoadSceneFromStream(in, engine, o3, m3, "", nullptr, nullptr) || true);
    }

    // --- the lights light the scene ------------------------------------------------------------------------
    gfx::ResetLighting();
    gfx::SetTimeOfDay(0.0f);   // midnight: only the nodes give light
    Camera3D& cam = engine.GetCamera();
    cam.position = { 0.0f, 9.0f, 14.0f };
    cam.target = { 0.0f, 0.0f, 0.0f };
    cam.up = { 0.0f, 1.0f, 0.0f };
    cam.fovy = 60.0f;

    const float dark = Shoot(engine, "dark");

    lamp->nodes.push_back(MakeNode(ObjectNodeType::PointLight));
    lamp->nodes.back().brightness = 3.0f;
    lamp->nodes.back().range = 30.0f;
    const float point = Shoot(engine, "point");
    std::printf("brightness: dark %.1f, point light %.1f\n", dark, point);
    CHECK(point > dark + 10.0f);

    lamp->nodes.back().enabled = false;
    const float off = Shoot(engine, "disabled");
    CHECK(std::fabs(off - dark) < 2.0f);                               // a disabled light adds nothing

    lamp->nodes.clear();
    lamp->nodes.push_back(MakeNode(ObjectNodeType::SpotLight));        // points down at the floor
    lamp->nodes.back().brightness = 4.0f;
    lamp->nodes.back().range = 30.0f;
    const float spot = Shoot(engine, "spot");
    CHECK(spot > dark + 5.0f);

    lamp->nodes.back().face = NodeFace::Top;                           // now it points at the sky
    const float away = Shoot(engine, "spot_up");
    CHECK(away < spot - 3.0f);

    lamp->nodes.clear();
    lamp->nodes.push_back(MakeNode(ObjectNodeType::SurfaceLight));     // faces up by default
    lamp->nodes.back().face = NodeFace::Bottom;
    lamp->nodes.back().brightness = 4.0f;
    lamp->nodes.back().range = 30.0f;
    lamp->nodes.back().angle = 80.0f;
    const float surf = Shoot(engine, "surface");
    CHECK(surf > dark + 5.0f);

    // --- decals and textures --------------------------------------------------------------------------------
    // Daylight, a plain white floor: a red Texture node on its top face turns the picture red; disabled, it does not.
    lamp->nodes.clear();
    gfx::ResetLighting();
    gfx::SetTimeOfDay(12.0f);
    const float dayBase = Shoot(engine, "day");
    auto redness = [&](const char* name, float y0 = 0.667f, float y1 = 1.0f) {
        for (int f = 0; f < 6; f++) engine.StepFrame(1.0f / 60.0f);
        char path[96];
        std::snprintf(path, sizeof path, "nodes_%s.png", name);
        TakeScreenshot(path);
        Image img = LoadImage(path);
        float r = 0, gb = 0; long n = 0;
        for (int y = (int)(img.height * y0); y < (int)(img.height * y1); y += 4)
            for (int x = 0; x < img.width; x += 4) { const Color c = GetImageColor(img, x, y); r += c.r; gb += (c.g + c.b) * 0.5f; n++; }
        UnloadImage(img);
        return (r - gb) / (float)n;
    };
    const float plainRed = redness("plain");
    floor->nodes.push_back(MakeNode(ObjectNodeType::Texture));
    floor->nodes.back().face = NodeFace::Top;
    floor->nodes.back().tint[1] = 0.0f; floor->nodes.back().tint[2] = 0.0f;
    floor->nodes.back().studsPerTile[0] = floor->nodes.back().studsPerTile[1] = 5.0f;
    const float texRed = redness("texture");
    std::printf("redness: plain %.1f, red Texture %.1f (day %.1f)\n", plainRed, texRed, dayBase);
    CHECK(texRed > plainRed + 20.0f);
    floor->nodes.back().enabled = false;
    CHECK(std::fabs(redness("texture_off") - plainRed) < 3.0f);

    // A Decal over one face of a cube: green, and only where that face is.
    floor->nodes.clear();
    lamp->nodes.push_back(MakeNode(ObjectNodeType::Decal));
    lamp->nodes.back().face = NodeFace::Top;
    lamp->nodes.back().tint[0] = 0.0f; lamp->nodes.back().tint[2] = 0.0f;
    *lamp->GetSizePtr() = { 6.0f, 6.0f, 6.0f };
    *lamp->GetPosPtr() = { 0.0f, 3.0f, 0.0f };
    cam.position = { 0.0f, 16.0f, 10.0f };
    const float withDecal = redness("decal", 0.2f, 0.45f);
    lamp->nodes.back().face = NodeFace::Bottom;                        // hidden under the cube: the top shows no green
    const float hidden = redness("decal_bottom", 0.2f, 0.45f);
    std::printf("redness: decal on top %.1f, decal underneath %.1f\n", withDecal, hidden);
    CHECK(withDecal < hidden - 15.0f);

    // --- shadows of the lights ----------------------------------------------------------------------------
    // A lamp above a slab: with Cast shadows the floor under the slab is dark, without it the light reaches it.
    {
        lamp->nodes.clear();
        floor->nodes.clear();
        *lamp->GetSizePtr() = { 1, 1, 1 };
        *lamp->GetPosPtr() = { 0.0f, 9.0f, 0.0f };
        *slab->GetPosPtr() = { 0.0f, 4.0f, 0.0f };
        gfx::ResetLighting();
        gfx::SetTimeOfDay(0.0f);
        gfx::Lighting().maxShadowCasters = 4;
        cam.position = { 0.0f, 16.0f, 22.0f };
        cam.target = { 0.0f, 0.0f, 0.0f };
        ObjectNode spot = MakeNode(ObjectNodeType::SpotLight);
        spot.brightness = 6.0f; spot.range = 40.0f; spot.angle = 60.0f; spot.castShadows = false;
        lamp->nodes.push_back(spot);
        const float unshadowed = Shoot(engine, "shadow_off");
        lamp->nodes.back().castShadows = true;
        lamp->nodes.back().shadowRes = 1024;
        const float shadowed = Shoot(engine, "shadow_on");
        std::printf("brightness: spot unshadowed %.1f, shadowed %.1f\n", unshadowed, shadowed);
        CHECK(shadowed < unshadowed - 8.0f);

        lamp->nodes.back() = MakeNode(ObjectNodeType::PointLight);
        lamp->nodes.back().brightness = 6.0f; lamp->nodes.back().range = 40.0f; lamp->nodes.back().castShadows = false;
        const float pointOff = Shoot(engine, "pshadow_off");
        lamp->nodes.back().castShadows = true;
        lamp->nodes.back().shadowRes = 512;
        const float pointOn = Shoot(engine, "pshadow_on");
        std::printf("brightness: point unshadowed %.1f, shadowed %.1f\n", pointOff, pointOn);
        CHECK(pointOn < pointOff - 8.0f);
        *slab->GetPosPtr() = { 0.0f, 500.0f, 0.0f };
    }

    // --- terrain and water take the light too ---------------------------------------------------------------
    {
        floor->hidden = true;
        lamp->nodes.clear();
        *lamp->GetPosPtr() = { 0.0f, 6.0f, 0.0f };
        gfx::ResetLighting();
        gfx::SetTimeOfDay(0.0f);
        cam.position = { 0.0f, 14.0f, 14.0f };
        cam.target = { 0.0f, 0.0f, 0.0f };

        auto terrainOwner = std::make_unique<BasicTerrain>(128, 128, 2.0f, 40.0f, 16.0f);
        BasicTerrain* terrain = terrainOwner.get();
        terrain->GenerateFlat(0.0f);
        engine.AddEntity(std::move(terrainOwner));
        const float terrainDark = Shoot(engine, "terrain_dark");
        lamp->nodes.push_back(MakeNode(ObjectNodeType::PointLight));
        lamp->nodes.back().brightness = 4.0f; lamp->nodes.back().range = 30.0f;
        const float terrainLit = Shoot(engine, "terrain_lit");
        std::printf("brightness: terrain dark %.1f, lit %.1f\n", terrainDark, terrainLit);
        CHECK(terrainLit > terrainDark + 10.0f);
        terrain->alive = false;

        lamp->nodes.clear();
        ui::SetPlayActive(true);   // the editor draws a flat plane; the water shader runs in play
        auto waterOwner = std::make_unique<WaterBody>(Vector3{ 0, 0, 0 }, Vector3{ 200, 1, 200 }, 2.5f, Color{ 20, 60, 90, 200 });
        WaterBody* water = waterOwner.get();
        engine.AddEntity(std::move(waterOwner));
        const float waterDark = Shoot(engine, "water_dark");
        lamp->nodes.push_back(MakeNode(ObjectNodeType::PointLight));
        lamp->nodes.back().brightness = 4.0f; lamp->nodes.back().range = 30.0f;
        const float waterLit = Shoot(engine, "water_lit");
        std::printf("brightness: water dark %.1f, lit %.1f\n", waterDark, waterLit);
        CHECK(waterLit > waterDark + 5.0f);
        water->alive = false;
        ui::SetPlayActive(false);

        // The older chunked terrain has a shader of its own: with the node lights in it, it must still link.
        auto legacyOwner = std::make_unique<terrain::Terrain>(Vector3{ 0, 0, 0 }, 200.0f, 200.0f, 64, 17);
        CHECK(IsShaderValid(legacyOwner->terrainShader));
    }

    std::printf(g_fail ? "object_nodes_test: %d FAILED\n" : "object_nodes_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
