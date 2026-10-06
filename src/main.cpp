#include "../include/Engine.hpp"
#include "../include/Engine/Backend/Benchmark.hpp"
#include "../include/Engine/Backend/CameraController.hpp"
#include "../include/Engine/Frontend/ObjectInteractionManager.hpp"
#include "../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../include/Engine/Scripts/CoreCLRHost.hpp"
#include "../include/Engine/Scripts/ScriptCompiler.hpp"
#include "../include/Engine/Frontend/ProjectManager.hpp"
#include "../include/Engine/Frontend/ui.hpp"
#include "../include/Engine/Backend/CrashReporter.hpp"
#include "../include/Engine/Backend/TextureManager.hpp"
#include "../include/Terrain/Terrain.hpp"
#include "../include/Terrain/BasicTerrain.hpp"
#include "../include/Terrain/Water/WaterBody.hpp"
#include "../include/Terrain/Water/WaterStressTest.hpp"
#include "raylib.h"
#include "Engine/Platform/Platform.hpp"

#include "../include/Engine/Scripts/NativeScriptHost.hpp"

#include <array>
#include <exception>
#include <memory>
#include <vector>
#include <filesystem>

#if defined(_MSC_VER)
    #include <crtdbg.h>
#endif

namespace fs = std::filesystem;

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

    InitWindow(windowWidth, windowHeight, "Ascend Softworks Flyengine Splash Screen");
    SetTargetFPS(5);

    project::ApplyWindowIcon();

    // ResolveAsset knows about the source tree, the install prefix and the
    // build directory, so this works the same however the process was started.
    const std::string logoPath = platform::ResolveAsset("assets/FlyengineLogo.png");

    Texture2D logo = { 0 };
    if (!logoPath.empty()) logo = LoadTexture(logoPath.c_str());

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

    // Same font resolution the editor UI uses, so the splash does not flash a
    // different typeface than the window that follows it.
    Font arialFont = GetFontDefault();
    bool isCustomFont = false;

    const std::string& fontPath = platform::ResolveFontPath();
    if (!fontPath.empty()) {
        arialFont = LoadFont(fontPath.c_str());
        isCustomFont = true;
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

    // Compile the project's C# scripts into Scripts/FlyScript.dll before the
    // CLR boots. If the build fails (e.g. no dotnet SDK), Initialize below
    // falls back to any pre-existing FlyScript.dll.
    scriptCompiler::EnsureBuilt(info.path);

    // CoreCLR host for C# scripting (runs as an Entity, gets Update called each frame).
    auto coreClrHost = std::make_unique<CoreCLRHost>();
    CoreCLRHost* coreClrHostPtr = coreClrHost.get();
    if (coreClrHost->Initialize(info.path)) {
        ui::LogAlways("CoreCLR host initialized for project: %s", info.path.c_str());
        engine.AddEntity(std::move(coreClrHost));
    } else {
        ui::LogAlways("CoreCLR host failed, C# Scripting unable to load: %s", coreClrHost->GetError().c_str());
    }

    project::Info loaded = info;

    // Draw a loading screen so the user sees progress during heavy load
    {
        BeginDrawing();
        ClearBackground(Color{36, 38, 44, 255});
        const char* label = TextFormat("Attempting to open project: %s ...", info.name.c_str());
        int tw = MeasureText(label, 20);
        DrawText(label, GetScreenWidth()/2 - tw/2, GetScreenHeight()/2 - 10, 20, LIGHTGRAY);
        EndDrawing();
    }

    // Initialize texture manager BEFORE scene load so GetGPUTexture works
    // during SetTexturePath calls inside OpenProjectFile.
    textureManager::Init(info.path);

    terrain::Terrain* loadedTerrain = nullptr;
    if (!project::OpenProjectFile(info.path, engine, rawObjectPtrs, sceneModels, loaded, &simRef, &loadedTerrain)) {
        ui::LogAlways("Failed to open project '%s'. Starting empty.", info.path.c_str());
    }

    // Bind the loaded world into the script runtime so FlyNative_* and standalone
    // scripts can read/mutate it. Safe even when the CLR failed to load: the
    // runtime still owns the script list and world services for the editor UI.
    if (coreClrHostPtr) {
        coreClrHostPtr->BindWorld(rawObjectPtrs, sceneModels, engine.GetCamera(), simRef, engine);
        // Make every IScript type in the compiled assembly show up in the
        // explorer's SCRIPTS group (and run on Play), not just saved entries.
        coreClrHostPtr->SyncStandaloneScripts();
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

    // Native plugins: SetEngine must come before Initialize, which loads
    // plugins/nat/*/build/*.so and calls on_load. The engine owns the host
    // and ticks it each frame (also handles hot-reload).
    auto nativeHost = std::make_unique<NativeScript::NativeScriptHost>();
    nativeHost->SetEngine(&engine);
    nativeHost->Initialize(info.path);
    engine.AddEntity(std::move(nativeHost));

    engine.Run();

    // This will only run after engine.Run() returns (editor closed)
    // VerifyAndRebuild removed from startup path - it's a housekeeping
    // step that reads every texture file for SHA256 hashing.
    // We skip it entirely; ref counting works fine without it.

    textureManager::Shutdown();
}

}

int main(int argc, char* argv[]) {
    if (!benchmark::RunStartupBenchmark()) return 0;

    crashreporter::Install();

#ifdef _DEBUG
    // Route MSVC debug assertions to the console (visible in the IDE run
    // panel) instead of blocking on a modal dialog; the SEH crash handler
    // never sees these, so this is the only way to capture them.
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif

    std::set_terminate([] {
        if (std::exception_ptr ep = std::current_exception()) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                crashreporter::LogFatal(e.what());
            } catch (...) {
                crashreporter::LogFatal("unknown C++ exception");
            }
        } else {
            crashreporter::LogFatal("terminate called without an active exception");
        }
        std::abort();
    });

    // Headless-style water system stress test - bypasses the splash screen and
    // project manager entirely:
    //   Flyengine.exe --testwater [objectCount] [frames]
    //   Flyengine.exe --create-project <name> [path] [--template <template>]
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--testwater") {
            watertest::Run(argc, argv);
            return 0;
        }
        if (std::string(argv[i]) == "--testscene") {
            return scenetest::Run(argc, argv);
        }
        if (std::string(argv[i]) == "--create-project") {
            if (i + 1 >= argc) {
                ui::LogAlways("ERROR: --create-project requires a project name");
                return 1;
            }
            std::string name = argv[++i];
            std::string templateName = "Empty";
            
            // Parse optional arguments
            for (int j = i + 1; j < argc; j++) {
                if (std::string(argv[j]) == "--template" && j + 1 < argc) {
                    templateName = argv[++j];
                }
            }
            
            // Create project using ProjectManager
            if (project::CreateProject(name, templateName)) {
                ui::LogAlways("Project '%s' created successfully", name.c_str());
                return 0;
            } else {
                ui::LogAlways("ERROR: Failed to create project '%s'", name.c_str());
                return 1;
            }
        }
    }

    ShowSplashWindow(3.0f);

    if (argc > 1) {
        // ReadProjectHeader expects a project FOLDER, not a .flyproj file.
        // If given a .flyproj file, use its parent directory.
        std::string arg = argv[1];
        fs::path p(arg);
        if (p.extension() == ".flyproj") {
            p = p.parent_path();
        }
        // If parent_path is empty (bare filename), use current directory
        if (p.empty()) {
            p = fs::current_path();
        }
        std::string projectFolder = p.string();
        project::Info info;
        if (project::ReadProjectHeader(projectFolder, info)) {
            project::SetCurrentProject(info);
            RunEditor(info);
        } else {
            ui::LogAlways("ERROR: ReadProjectHeader failed for folder: %s", projectFolder.c_str());
        }
        return 0;
    }

    project::Info info = project::ShowProjectManager();
    if (info.path.empty()) return 0;

    project::SetCurrentProject(info);
    RunEditor(info);

    return 0;
}