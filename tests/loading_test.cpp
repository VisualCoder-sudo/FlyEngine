// The loading screens of the editor and the player: settings (kept per machine, and per project for the player),
// progress rules, and pictures of both (loading_<name>.png). Opens a window, so it is not registered with ctest:
// build/tests/loading_test
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/LoadingScreen.hpp"
#include "Engine/Frontend/ProjectManager.hpp"
#include "Engine/Backend/ScenePersistence.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// Mean colour of a part of the last picture.
static Color Probe(const char* path, float x0, float y0, float x1, float y1) {
    Image img = LoadImage(path);
    long r = 0, g = 0, b = 0, n = 0;
    for (int y = (int)(y0 * img.height); y < (int)(y1 * img.height); y += 2)
        for (int x = (int)(x0 * img.width); x < (int)(x1 * img.width); x += 2) {
            const Color c = GetImageColor(img, x, y);
            r += c.r; g += c.g; b += c.b; n++;
        }
    UnloadImage(img);
    return n ? Color{ (unsigned char)(r / n), (unsigned char)(g / n), (unsigned char)(b / n), 255 } : Color{};
}

int main() {
    const std::filesystem::path home = std::filesystem::temp_directory_path() / "fly_loading_test_home";
    std::filesystem::remove_all(home);
    std::filesystem::create_directories(home);
    setenv("HOME", home.c_str(), 1);
    unsetenv("XDG_CONFIG_HOME");

    Engine engine(960, 540, "loading_test", 60);
    engine.SetPlayerBuild(true);

    // ---- Defaults: the editor shows logs, the player only the percentage.
    loading::Load();
    CHECK(loading::GetSettings().editor.enabled && loading::GetSettings().editor.showLogs);
    CHECK(loading::GetSettings().player.enabled && !loading::GetSettings().player.showLogs && loading::GetSettings().player.showPercent);

    // ---- Settings are kept per machine...
    {
        loading::Settings& s = loading::GetSettings();
        s.editor.showLogs = false; s.editor.logLines = 11;
        s.player.enabled = false; s.player.accent = { 250, 120, 10, 255 };
        std::snprintf(s.player.title, sizeof s.player.title, "Welcome to {name}!");
        loading::Save();
        loading::Load();
        CHECK(!loading::GetSettings().editor.showLogs && loading::GetSettings().editor.logLines == 11);
        CHECK(!loading::GetSettings().player.enabled && loading::GetSettings().player.accent.r == 250 && loading::GetSettings().player.accent.g == 120);
        CHECK(std::string(loading::GetSettings().player.title) == "Welcome to {name}!");
    }
    // ...and a project's own player look wins over them, for the player only.
    {
        const std::string folder = (home / "MyGame").string();
        std::filesystem::create_directories(folder);
        CHECK(!loading::HasProjectOverride(folder));
        loading::GetSettings().player.enabled = true;
        loading::GetSettings().player.background = { 10, 40, 90, 255 };
        std::snprintf(loading::GetSettings().player.title, sizeof loading::GetSettings().player.title, "My Game");
        CHECK(loading::SaveProjectOverride(folder) && loading::HasProjectOverride(folder));
        loading::Load();                                        // the machine's alone: the player is off again
        CHECK(!loading::GetSettings().player.enabled);
        loading::Load(folder);
        CHECK(loading::GetSettings().player.enabled && loading::GetSettings().player.background.b == 90);
        CHECK(std::string(loading::GetSettings().player.title) == "My Game");
        CHECK(!loading::GetSettings().editor.showLogs);          // the editor's settings are not the project's to change
        CHECK(loading::RemoveProjectOverride(folder) && !loading::HasProjectOverride(folder));
        loading::Load();
    }

    // ---- Progress: ranges map onto the bar, and it never goes back.
    {
        loading::Load();
        loading::GetSettings().player.enabled = true;
        loading::Begin(loading::Target::Player, "Demo");
        CHECK(loading::Active());
        loading::Range(0.2f, 0.6f);
        loading::Progress(0.5f, "Half of the middle");
        CHECK(std::fabs(loading::Fraction() - 0.4f) < 1e-4f);
        loading::Progress(0.0f, "Going back");
        CHECK(std::fabs(loading::Fraction() - 0.4f) < 1e-4f);
        loading::Progress(7.0f);
        CHECK(std::fabs(loading::Fraction() - 0.6f) < 1e-4f);
        loading::End();
        CHECK(!loading::Active() && loading::Fraction() == 1.0f);
        loading::Progress(0.1f, "ignored");                      // outside a load: nothing happens
        CHECK(loading::Fraction() == 1.0f);
    }

    // ---- Pictures. The editor's: percentage, stage, log lines. The player's: just the percentage.
    loading::Load();
    loading::GetSettings().editor.showLogs = true;
    loading::GetSettings().editor.logLines = 8;
    loading::Begin(loading::Target::Editor, "Skyline");
    loading::Range(0.0f, 1.0f);
    loading::Log("Opening project 'Skyline'");
    loading::Log("[Scripts] Building 3 script file(s)");
    loading::Log("[compiler] c++ -std=c++20 -O1 ...");
    loading::Progress(0.31f, "Building the project's scripts");
    loading::Log("[terrain] loaded 1 terrain(s) from terrain.terrain");
    loading::Log("[city] loaded 1 city(ies) from city.city");
    loading::Progress(0.64f, "Loading objects (256 / 400)");
    loading::Redraw();
    TakeScreenshot("loading_editor.png");
    const Color editorBg = Probe("loading_editor.png", 0.01f, 0.01f, 0.08f, 0.08f);
    const Color editorBar = Probe("loading_editor.png", 0.25f, 0.70f, 0.75f, 0.95f);   // where the log box is
    std::printf("editor loading screen: corner (%d,%d,%d)\n", editorBg.r, editorBg.g, editorBg.b);
    CHECK(editorBg.r == 36 && editorBg.g == 38 && editorBg.b == 44);
    CHECK(editorBar.b < editorBg.b - 4);                          // the log box (darker than the ground) is there
    loading::End();

    loading::GetSettings().player = loading::Look{};         // the defaults (the saved ones above had it off)
    loading::Begin(loading::Target::Player, "Skyline");
    loading::Progress(0.64f, "Loading objects");
    loading::Redraw();
    TakeScreenshot("loading_player.png");
    const Color playerBg = Probe("loading_player.png", 0.01f, 0.01f, 0.08f, 0.08f);
    CHECK(playerBg.r == 25 && playerBg.g == 27 && playerBg.b == 32);
    loading::End();

    // A custom look: light background, orange accent, own title, everything on.
    {
        loading::Look& l = loading::GetSettings().player;
        l.background = { 240, 236, 226, 255 }; l.accent = { 230, 90, 20, 255 };
        l.showStage = true; l.showLogs = true; l.logLines = 5;
        std::snprintf(l.title, sizeof l.title, "{name}: loading");
        loading::Begin(loading::Target::Player, "Skyline");
        loading::Log("[Player] Initializing native plugins...");
        loading::Log("[Player] Native plugins initialized successfully");
        loading::Progress(0.82f, "Loading city");
        loading::Redraw();
        TakeScreenshot("loading_custom.png");
        CHECK(Probe("loading_custom.png", 0.01f, 0.01f, 0.08f, 0.08f).r == 240);
        const Color text = Probe("loading_custom.png", 0.40f, 0.26f, 0.60f, 0.31f);
        CHECK(text.r < 236);                                      // dark title text on the light ground
        loading::End();
    }

    // Switched off: nothing is drawn (the window keeps whatever it showed).
    {
        loading::Look& l = loading::GetSettings().player;
        l.enabled = false;
        ClearBackground(Color{ 1, 2, 3, 255 });
        BeginDrawing(); ClearBackground(Color{ 1, 2, 3, 255 }); EndDrawing();
        loading::Begin(loading::Target::Player, "Skyline");
        loading::Progress(0.5f, "Quiet");
        TakeScreenshot("loading_off.png");
        const Color c = Probe("loading_off.png", 0.4f, 0.3f, 0.6f, 0.7f);
        CHECK(c.r == 1 && c.g == 2 && c.b == 3);
        CHECK(std::fabs(loading::Fraction() - 0.5f) < 1e-4f);     // the progress is still tracked
        loading::End();
    }

    // ---- A real load reports its progress: a project with a city, opened as the editor does.
    {
        loading::Load();
        loading::GetSettings().editor.enabled = true;
        CHECK(project::CreateProject("Loaded", "Blank"));
        std::vector<ScatteredObject*> objects;
        std::vector<std::unique_ptr<ModelGroup>> models;
        project::Info info;
        loading::Begin(loading::Target::Editor, "Loaded");
        loading::Range(0.2f, 0.8f);
        CHECK(project::OpenProjectFile(project::GetProjectFolder("Loaded"), engine, objects, models, info));
        // The scene load runs the bar to 95 % of its range (lighting is the last step before it ends).
        std::printf("after the scene load the bar is at %.0f%%\n", loading::Fraction() * 100.0f);
        CHECK(loading::Fraction() > 0.2f + 0.6f * 0.9f);
        loading::End();
    }

    std::filesystem::remove_all(home);
    std::printf(g_fail ? "loading_test: %d FAILED\n" : "loading_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
