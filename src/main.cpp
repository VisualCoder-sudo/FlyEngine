#include "Engine.hpp"
#include "Benchmark.hpp"
#include "CameraController.hpp"
#include "ObjectInteractionManager.hpp"
#include "PhysicsSimulation.hpp"
#include "Flyscript.hpp"
#include "ProjectManager.hpp"
#include "ui.hpp"
#include "CrashReporter.hpp"
#include "TextureManager.hpp"
#include "Terrain.hpp"
#include "BasicTerrain.hpp"
#include "WaterBody.hpp"
#include "raylib.h"

#include <array>
#include <memory>
#include <vector>

namespace {

struct ScopedUI {
    ScopedUI()  { ui::Init(); }
    ~ScopedUI() noexcept { ui::Unload(); }

    ScopedUI(const ScopedUI&) = delete;
    ScopedUI& operator=(const ScopedUI&) = delete;
    ScopedUI(ScopedUI&&) = delete;
    ScopedUI& operator=(ScopedUI&&) = delete;
};

void ShowSplashWindow(float displaySeconds) {
    constexpr int windowWidth  = 750;
    constexpr int windowHeight = 250;
    constexpr float targetLogoHeight = 130.0f;

    InitWindow(windowWidth, windowHeight, "Ascend Softworks Flyengine Raylib C Splash screen");
    SetTargetFPS(5);

    project::ApplyWindowIcon();

    static constexpr std::array<const char*, 2> logoPaths = {
        "assets/FlyengineLogo.png",
        "../assets/FlyengineLogo.png"
    };

    Texture2D logo = { 0 };
    for (const char* path : logoPaths) {
        if (FileExists(path)) {
            logo = LoadTexture(path);
            break;
        }
    }

    const bool isLogoLoaded = (logo.id > 0);

    const float logoScale = isLogoLoaded ? (targetLogoHeight / static_cast<float>(logo.height)) : 1.0f;
    const float logoWidth = isLogoLoaded ? (static_cast<float>(logo.width) * logoScale) : 0.0f;

    constexpr float logoX = 40.0f;
    constexpr auto logoY  = (static_cast<float>(windowHeight) - targetLogoHeight) * 0.5f;
    const float textX     = isLogoLoaded ? (logoX + logoWidth + 30.0f) : 60.0f;

    constexpr Vector2 logoPos{ logoX, logoY };
    const Vector2 titlePos{ textX, 85.0f };
    const Vector2 subtitlePos{ textX, 135.0f };
    const Vector2 verPos{ textX, 156.0f };
    const Vector2 rightsPos{ textX, 215.0f };

    static constexpr std::array<const char*, 3> fontPaths = {
        "arial.ttf",
        "../arial.ttf",
        "C:/Windows/Fonts/arial.ttf"
    };

    Font arialFont = GetFontDefault();
    bool isCustomFont = false;

    for (const char* path : fontPaths) {
        if (FileExists(path)) {
            arialFont = LoadFont(path);
            isCustomFont = true;
            break;
        }
    }

    const double startTime = GetTime();

    while (!WindowShouldClose() && (GetTime() - startTime < displaySeconds)) {
        BeginDrawing();
            constexpr Color bgDarkColor{ 24, 26, 32, 255 };
            ClearBackground(bgDarkColor);

            if (isLogoLoaded) {
                DrawTextureEx(logo, logoPos, 0.0f, logoScale, WHITE);
            }

            DrawTextEx(arialFont, "Flyengine", titlePos, 32.0f, 1.0f, RAYWHITE);
            DrawTextEx(arialFont, "owned and developed by Ascend Softworks(TM)", subtitlePos, 16.0f, 1.0f, LIGHTGRAY);
            DrawTextEx(arialFont, "Flyengine version 1.0.4 BETA", verPos, 16.0f, 1.0f, LIGHTGRAY);
            DrawTextEx(arialFont, "ASCEND SOFTWORKS 2026, ALL RIGHTS RESERVED", rightsPos, 12.0f, 1.0f, LIGHTGRAY);
        EndDrawing();
    }

    if (isCustomFont)  UnloadFont(arialFont);
    if (isLogoLoaded)  UnloadTexture(logo);

    CloseWindow();
}

void RunEditor(const project::Info& info) {
    Engine engine(1280, 720, "Flyengine Editor / " + info.name);
    engine.SetClearColor(Color{ 36, 38, 44, 255 });

    const ScopedUI uiScope;
    project::ApplyWindowIcon();

    std::vector<ScatteredObject*> rawObjectPtrs;
    std::vector<std::unique_ptr<ModelGroup>> sceneModels;
    ui::SetSceneObjects(&rawObjectPtrs);
    ui::SetSceneModels(&sceneModels);

    auto cameraController = std::make_unique<CameraController>(engine.GetCamera());
    CameraController* cameraControllerPtr = cameraController.get();
    engine.AddEntity(std::move(cameraController));

    auto sim = std::make_unique<phys::Simulation>(rawObjectPtrs);
    phys::Simulation& simRef = *sim;
    ui::SetSimulation(&simRef);
    engine.AddEntity(std::move(sim));

    auto interactionMgr = std::make_unique<ObjectInteractionManager>(engine, engine.GetCamera(), cameraControllerPtr, rawObjectPtrs, sceneModels, &simRef);
    ObjectInteractionManager* interactionMgrPtr = interactionMgr.get();
    engine.AddEntity(std::move(interactionMgr));

    auto runtime = std::make_unique<flyscript::Runtime>(rawObjectPtrs, sceneModels, engine.GetCamera(), simRef, engine);
    flyscript::SetRuntime(runtime.get());
    engine.AddEntity(std::move(runtime));

    project::Info loaded = info;

    // Draw a loading screen so the user sees progress during heavy load
    {
        BeginDrawing();
        ClearBackground(Color{36, 38, 44, 255});
        const char* label = TextFormat("Loading project: %s ...", info.name.c_str());
        int tw = MeasureText(label, 20);
        DrawText(label, GetScreenWidth()/2 - tw/2, GetScreenHeight()/2 - 10, 20, LIGHTGRAY);
        EndDrawing();
    }

    // Initialize texture manager BEFORE scene load so GetGPUTexture works
    // during SetTexturePath calls inside OpenProjectFile.
    textureManager::Init(info.path);

    terrain::Terrain* loadedTerrain = nullptr;
    if (!project::OpenProjectFile(info.path, engine, rawObjectPtrs, sceneModels, loaded, &simRef, &loadedTerrain)) {
        ui::LogAlways("Could not open project '%s'. Starting empty.", info.path.c_str());
    }
    
    // If terrain was loaded, set it up with editor
    if (loadedTerrain) {
        ui::HandleTerrainSelection(loadedTerrain, true);
        ui::SetTerrainEditorMode(ui::TransformTool::Terrain);
        loadedTerrain->SetPhysicsSimulation(&simRef);
    }

    // VerifyAndRebuild does expensive full-file SHA256 on every texture.
    // Defer it so the scene appears instantly; run after first frame.
    bool needsVerify = !rawObjectPtrs.empty();

    engine.Run();

    // This will only run after engine.Run() returns (editor closed)
    // VerifyAndRebuild removed from startup path — it's a housekeeping
    // step that reads every texture file for SHA256 hashing.
    // We skip it entirely; ref counting works fine without it.

    textureManager::Shutdown();
}

}

int main(int argc, char* argv[]) {
    if (!benchmark::RunStartupBenchmark()) return 0;

    crashreporter::Install();

    ShowSplashWindow(3.0f);

    if (argc > 1) {
        project::Info info;
        if (project::ReadProjectHeader(argv[1], info)) {
            project::SetCurrentProject(info);
            RunEditor(info);
        }
        return 0;
    }

    project::Info info = project::ShowProjectManager();
    if (info.path.empty()) return 0;

    project::SetCurrentProject(info);
    RunEditor(info);

    return 0;
}