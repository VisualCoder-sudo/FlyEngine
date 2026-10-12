// A new project starts with the atmosphere sky, clouds, a Weather item and a Picture item, so the sky, weather and
// picture settings are in the Explorer and visible from the first frame. Creates a project under a temporary $HOME,
// opens it the way the editor does, and checks the lighting and the picture. Opens a window, so it is not registered
// with ctest: build/tests/new_project_test
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/Atmosphere.hpp"
#include "Engine/Clouds.hpp"
#include "Engine/Graphics.hpp"
#include "Engine/Frontend/ProjectManager.hpp"
#include "Engine/Backend/ScenePersistence.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
    const std::filesystem::path home = std::filesystem::temp_directory_path() / "fly_new_project_test_home";
    std::filesystem::remove_all(home);
    std::filesystem::create_directories(home);
    setenv("HOME", home.c_str(), 1);
    unsetenv("XDG_CONFIG_HOME");

    Engine engine(960, 540, "new_project_test", 60);
    engine.SetPlayerBuild(true);

    // The editor starts with the plain default lighting; creating a project must not disturb what is open.
    gfx::ResetLighting();
    gfx::SetTimeOfDay(9.0f);
    CHECK(project::CreateProject("SkyDefaults", "Blank"));
    CHECK(gfx::Lighting().timeOfDay == 9.0f && !gfx::Lighting().hasSky);

    // Open it as the editor does.
    std::vector<ScatteredObject*> objects;
    std::vector<std::unique_ptr<ModelGroup>> models;
    project::Info info;
    CHECK(project::OpenProjectFile(project::GetProjectFolder("SkyDefaults"), engine, objects, models, info));
    const gfx::LightingSettings& L = gfx::Lighting();
    CHECK(L.hasSky && L.skyMode == 1 && gfx::AtmosphereActive());
    CHECK(L.hasClouds && L.hasWeather && L.hasPicture);
    CHECK(L.overcast == 0.0f && L.rain == 0.0f);        // the Weather item is there, and clear

    // And it shows: a blue sky overhead, clouds drawn, and changing the time or the weather changes the picture.
    Camera3D& cam = engine.GetCamera();
    cam.position = { 0.0f, 3.0f, 0.0f }; cam.target = { 0.0f, 25.0f, -60.0f }; cam.up = { 0, 1, 0 }; cam.fovy = 60.0f;
    auto shoot = [&](const char* name, int frames) {
        for (int f = 0; f < frames; f++) engine.StepFrame(1.0f / 60.0f);
        TakeScreenshot(name);
        Image img = LoadImage(name);
        double r = 0, g = 0, b = 0; long n = 0;
        for (int y = 0; y < img.height / 4; y += 2)
            for (int x = 0; x < img.width; x += 2) { const Color c = GetImageColor(img, x, y); r += c.r; g += c.g; b += c.b; n++; }
        UnloadImage(img);
        return Vector3{ (float)(r / n), (float)(g / n), (float)(b / n) };
    };
    const double w0 = GetTime();
    while (!gfx::CloudsActive() && GetTime() - w0 < 30.0) engine.StepFrame(1.0f / 60.0f);
    CHECK(gfx::CloudsActive());
    const Vector3 day = shoot("newproj_day.png", 30);
    std::printf("new project, 15:00: top of the picture (%.0f,%.0f,%.0f)\n", day.x, day.y, day.z);
    CHECK(day.z > day.x + 30.0f);                       // blue

    gfx::SetTimeOfDay(23.0f);
    const Vector3 night = shoot("newproj_night.png", 40);
    std::printf("new project, 23:00: (%.0f,%.0f,%.0f)\n", night.x, night.y, night.z);
    CHECK(night.x + night.y + night.z < (day.x + day.y + day.z) * 0.5f);

    gfx::SetTimeOfDay(15.0f);
    gfx::Lighting().overcast = 1.0f; gfx::Lighting().rain = 0.7f;
    const Vector3 storm = shoot("newproj_storm.png", 40);
    std::printf("new project, storm: (%.0f,%.0f,%.0f)\n", storm.x, storm.y, storm.z);
    CHECK(storm.z - storm.x < (day.z - day.x) * 0.5f);  // greyer

    std::filesystem::remove_all(home);
    std::printf(g_fail ? "new_project_test: %d FAILED\n" : "new_project_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
