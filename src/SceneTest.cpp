// `Flyengine --testscene [frames]`: renders a scene that exercises the paths the
// water test does not -- BasicTerrain with every sculpt/paint tool (so the
// splatmap texture and the terrain mesh are rebuilt repeatedly), a generated
// city (roads, instanced buildings, shadows), and a mix of lit shapes. Meant to
// be run from a Debug build, where sokol's validation layer aborts on any
// invalid GPU usage; it also saves screenshots to scene_*.png for a visual check.
#include "../include/Engine.hpp"
#include "../include/Engine/Graphics.hpp"
#include "../include/Engine/Frontend/ui.hpp"
#include "../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../include/Engine/Backend/ScatteredObject.hpp"
#include "../include/Terrain/BasicTerrain.hpp"
#include "../include/CityGen/City.hpp"
#include "raylib.h"
#include "raymath.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace scenetest {

namespace {
struct ScopedUI {
    ScopedUI() { ui::Init(); }
    ~ScopedUI() noexcept { ui::Unload(); }
};
std::vector<ScatteredObject*> g_objects;
std::vector<std::unique_ptr<ModelGroup>> g_models;
}

int Run(int argc, char** argv) {
    int frames = argc > 2 ? std::atoi(argv[2]) : 360;
    const bool cityCloseup = argc > 3 && std::string(argv[3]) == "city";
    if (frames < 120) frames = 120;
    std::printf("[scene] %d frames\n", frames);

    Engine engine(1280, 720, "Flyengine Scene Test");
    engine.SetClearColor(Color{ 90, 120, 160, 255 });
    const ScopedUI uiScope;
    ui::SetSceneObjects(&g_objects);
    ui::SetSceneModels(&g_models);
    auto sim = std::make_unique<phys::Simulation>(g_objects);
    ui::SetSimulation(sim.get());
    engine.AddEntity(std::move(sim));

    // Terrain: painted and sculpted every frame below.
    auto terrainOwner = std::make_unique<BasicTerrain>(128, 128, 2.0f, 40.0f, 16.0f);
    BasicTerrain* terrain = terrainOwner.get();
    terrain->GenerateFlat(0.0f);
    engine.AddEntity(std::move(terrainOwner));

    // City next to it.
    auto cityOwner = std::make_unique<city::City>();
    city::City* cityPtr = cityOwner.get();
    cityPtr->GenerateGrid(Vector2{ 260.0f, 0.0f });
    engine.AddEntity(std::move(cityOwner));

    // A few lit shapes of every kind.
    const ShapeType shapes[] = { ShapeType::Cube, ShapeType::Sphere, ShapeType::Cylinder, ShapeType::Wedge };
    for (int i = 0; i < 12; ++i) {
        auto obj = std::make_unique<ScatteredObject>(
            Vector3{ -40.0f + 7.0f * (float)i, 3.0f, 60.0f }, Vector3{ 4, 4, 4 },
            Color{ (unsigned char)(60 + i * 15), 140, (unsigned char)(220 - i * 12), 255 }, shapes[i % 4]);
        g_objects.push_back(obj.get());
        engine.AddEntity(std::move(obj));
    }

    gfx::SetShadowsEnabled(!(argc > 4 && std::string(argv[4]) == "noshadow"));
    ui::SetPlayActive(false);

    const BasicTerrain::Tool tools[] = {
        BasicTerrain::Tool::Raise, BasicTerrain::Tool::Paint, BasicTerrain::Tool::Smooth,
        BasicTerrain::Tool::Lower, BasicTerrain::Tool::Paint, BasicTerrain::Tool::Noise,
        BasicTerrain::Tool::Flatten, BasicTerrain::Tool::Erode, BasicTerrain::Tool::Ramp,
    };

    for (int f = 0; f < frames; ++f) {
        // Brush the terrain: a different tool every 20 frames, painting layers in turn.
        BasicTerrain::Brush brush;
        brush.tool = tools[(f / 20) % 9];
        brush.paintLayer = (f / 7) % 4;
        brush.paintErase = ((f / 50) % 4) == 3;
        brush.radius = 12.0f + 10.0f * std::sin(f * 0.05f);
        brush.strength = 40.0f;
        terrain->ApplyBrush(brush, Vector2{ 40.0f * std::cos(f * 0.04f), 40.0f * std::sin(f * 0.04f) }, 1.0f / 60.0f);

        // Orbit across terrain, city and shapes.
        const float ang = f * 0.012f;
        Camera3D& cam = engine.GetCamera();
        if (argc > 3 && std::string(argv[3]) == "terrain") {
            cam.position = Vector3{ 70.0f, 70.0f, 70.0f };
            cam.target = Vector3{ 0.0f, 0.0f, 0.0f };
        } else if (argc > 3 && std::string(argv[3]) == "citynear") {
            cam.position = Vector3{ 262.0f, 14.0f, 40.0f };
            cam.target = Vector3{ 258.0f, 8.0f, 10.0f };
        } else if (cityCloseup) {
            cam.position = Vector3{ 330.0f, 45.0f, 70.0f };
            cam.target = Vector3{ 260.0f, 8.0f, 10.0f };
        } else {
            cam.position = Vector3{ 130.0f + std::cos(ang) * 190.0f, 90.0f + 25.0f * std::sin(ang * 2.0f), 30.0f + std::sin(ang) * 190.0f };
            cam.target = Vector3{ 110.0f, 0.0f, 20.0f };
        }
        cam.up = Vector3{ 0, 1, 0 };
        cam.fovy = 55.0f;
        cam.projection = CAMERA_PERSPECTIVE;

        engine.StepFrame(1.0f / 60.0f);

        if (f == frames / 4 || f == frames / 2 || f == frames - 1) {
            char name[64];
            std::snprintf(name, sizeof(name), "scene_%03d.png", f);
            TakeScreenshot(name);
        }
    }
    std::printf("[scene] done\n");
    return 0;
}

} // namespace scenetest
