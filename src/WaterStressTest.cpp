#include "../include/Terrain/Water/WaterStressTest.hpp"

#include "../include/Engine.hpp"
#include "../include/Engine/Graphics.hpp"
#include "../include/Engine/Frontend/ui.hpp"
#include "../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../include/Engine/Backend/ScatteredObject.hpp"
#include "../include/Terrain/Water/WaterBody.hpp"
#include "raylib.h"
#include "raymath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

struct ScopedUI {
    ScopedUI()  { ui::Init(); }
    ~ScopedUI() noexcept { ui::Unload(); }
};

struct FrameTiming {
    float frameMs = 0.0f;
    float shadowMs = 0.0f;
    float reflectionMs = 0.0f;
    float opaqueMs = 0.0f;
    float transparentMs = 0.0f;
    float totalMs = 0.0f;
};

class TimingStats {
public:
    void Add(const FrameTiming& t, double renderDtSeconds) {
        n++;
        sum.frameMs += t.frameMs;
        sum.shadowMs += t.shadowMs;
        sum.reflectionMs += t.reflectionMs;
        sum.opaqueMs += t.opaqueMs;
        sum.transparentMs += t.transparentMs;
        sumTotalMs += (float)(renderDtSeconds * 1000.0);
        max.frameMs = std::max(max.frameMs, t.frameMs);
        max.reflectionMs = std::max(max.reflectionMs, t.reflectionMs);
        max.totalMs = std::max(max.totalMs, (float)(renderDtSeconds * 1000.0));
    }

    FrameTiming Avg() const {
        if (n == 0) return FrameTiming{};
        FrameTiming a;
        a.frameMs = sum.frameMs / (float)n;
        a.shadowMs = sum.shadowMs / (float)n;
        a.reflectionMs = sum.reflectionMs / (float)n;
        a.opaqueMs = sum.opaqueMs / (float)n;
        a.transparentMs = sum.transparentMs / (float)n;
        return a;
    }

    int n = 0;
    FrameTiming sum{}, max{};
    float sumTotalMs = 0.0f;
    float avgTotalMs = 0.0f; // average wall frame time (includes physics/update)

    void Finalize() { if (n > 0) avgTotalMs = sumTotalMs / (float)n; }
};

std::vector<ScatteredObject*> g_objects;
std::vector<std::unique_ptr<ModelGroup>> g_models;

void SpawnWater(Engine& engine, float sizeX, float sizeZ, float waterHeight) {
    auto water = std::make_unique<WaterBody>(
        Vector3{ 0.0f, waterHeight, 0.0f },
        Vector3{ sizeX, 1.0f, sizeZ },
        waterHeight,
        Color{ 25, 160, 190, 170 });
    // A calmer setup reads as a lake/ocean; keep defaults otherwise.
    auto noise = water->GetNoiseParams();
    noise.amplitude = 0.6f;
    water->SetNoiseParams(noise);
    engine.AddEntity(std::move(water));
}

void SpawnUnanchoredObjects(Engine& engine, int count, float waterHeight) {
    // Spread `count` cubes across a square centred on the origin at a few
    // heights, all unanchored so they drop into the water (stress the physics
    // + buoyancy + proximity + reflection + shadow passes at once).
    int side = (int)ceilf(sqrtf((float)count));
    int placed = 0;
    float half = side * 1.5f * 0.5f;
    for (int z = 0; z < side && placed < count; z++) {
        for (int x = 0; x < side && placed < count; x++) {
            float px = x * 1.5f - half;
            float pz = z * 1.5f - half;
            float py = waterHeight + 30.0f + 3.0f * ((z + x) & 3);
            auto obj = std::make_unique<ScatteredObject>(
                Vector3{ px, py, pz },
                Vector3{ 0.8f, 0.8f, 0.8f },
                Color{ 90, 120, 160, 255 },
                ShapeType::Cube);
            obj->anchored = false;
            obj->isSelected = false;
            ScatteredObject* raw = obj.get();
            g_objects.push_back(raw);
            engine.AddEntity(std::move(obj));
            placed++;
        }
    }
}

void OrbitCamera(Camera3D& cam, double t, float waterHeight) {
    float radius = 900.0f;
    float ang = (float)t * 0.01f; // 0.01 rad/frame ≈ 9 m/s sweep at radius 900 (realistic pan)
    cam.position = Vector3{ cosf(ang) * radius, waterHeight + 360.0f, sinf(ang) * radius };
    cam.target = Vector3{ 0.0f, waterHeight, 0.0f };
    cam.up = Vector3{ 0.0f, 1.0f, 0.0f };
    cam.fovy = 55.0f;
    cam.projection = CAMERA_PERSPECTIVE;
}

void LogFrame(int i, int total, const FrameTiming& t, const WaterBody::DebugStats& w,
              const Camera3D& cam) {
    float expectedDisk = 3.14159265f * WaterBody::HORIZON_DISTANCE * WaterBody::HORIZON_DISTANCE /
                         (WaterBody::CHUNK_SIZE * WaterBody::CHUNK_SIZE);
    printf("[test] f=%4d/%d  frame=%6.2fms  shadow=%5.1f  refl=%5.1f  opaque=%6.1f  trans=%6.1f  | chunks=%d (disk expects %.0f, coarse=%d) lod1=%d lod2=%d lod3=%d lod4=%d shell=%s res=%d cam=(%.0f,%.0f,%.0f)\n",
           i, total, t.frameMs, t.shadowMs, t.reflectionMs, t.opaqueMs, t.transparentMs,
           w.totalChunks, expectedDisk, w.coarseChunkCount, w.lodCounts[1], w.lodCounts[2], w.lodCounts[3], w.lodCounts[4],
           w.farShellBuilt ? "yes" : "no", w.farShellResolution,
           cam.position.x, cam.position.y, cam.position.z);
}

void PrintSummary(const std::string& phase, int frames, const TimingStats& stats,
                  const WaterBody::DebugStats& water, bool reflectionsOn) {
    FrameTiming a = stats.Avg();
    printf("[summary] %-28s avg total=%5.2fms (%4.1f fps)  max total=%5.2fms\n",
           phase.c_str(), stats.avgTotalMs, 1000.0f / std::max(stats.avgTotalMs, 0.001f), stats.max.totalMs);
    printf("[summary]    shadow=%5.2f  reflection=%5.2f  opaque=%6.2f  transparent=%6.2f  (avg ms over %d frames)\n",
           a.shadowMs, a.reflectionMs, a.opaqueMs, a.transparentMs, stats.n);
    printf("[summary]    peak frame=%5.2fms  peak reflection=%5.2fms\n", stats.max.frameMs, stats.max.reflectionMs);
    printf("[summary]    water: chunks=%d (coarse=%d lod1=%d lod2=%d lod3=%d lod4=%d) farShell=%s res=%d verts=%d horizon=%.0fm\n",
           water.totalChunks, water.coarseChunkCount, water.lodCounts[1], water.lodCounts[2],
           water.lodCounts[3], water.lodCounts[4], water.farShellBuilt ? "yes" : "no",
           water.farShellResolution, water.farShellVertCount, water.horizonDistance);
    printf("[summary]    reflections=%s\n", reflectionsOn ? "ON" : "OFF");
}

} // namespace

namespace watertest {

void Run(int argc, char** argv) {
    int objectCount = 5000;
    int frameCount = 720;
    if (argc > 2) objectCount = atoi(argv[2]);
    if (argc > 3) frameCount = atoi(argv[3]);
    objectCount = std::max(1, objectCount);
    frameCount = std::max(120, frameCount);

    printf("[test] Water stress test: %d unanchored objects, %d frames\n", objectCount, frameCount);

    Engine engine(1280, 720, "Flyengine Water Stress Test");
    engine.timingEnabled = true;
    engine.SetClearColor(Color{ 30, 32, 40, 255 });

    const ScopedUI uiScope;
    ui::SetSceneObjects(&g_objects);
    ui::SetSceneModels(&g_models);

    auto sim = std::make_unique<phys::Simulation>(g_objects);
    phys::Simulation* simPtr = sim.get();
    ui::SetSimulation(simPtr);
    engine.AddEntity(std::move(sim));

    constexpr float kWaterHeight = 2.0f;
    SpawnWater(engine, 6000.0f, 6000.0f, kWaterHeight);
    SpawnUnanchoredObjects(engine, objectCount, kWaterHeight);

    // Reflection target up front so the render target exists before the loop.
    gfx::SetReflectionsEnabled(true);
    gfx::SetReflectionQuality(gfx::GetReflectionQuality());

    WaterBody* water = WaterBody::GetInstances().empty() ? nullptr : WaterBody::GetInstances().front();

    // Unanchored objects start falling from frame one.
    ui::SetPlayActive(true);

    printf("[test] entities=%zu  playActive=%s\n",
           engine.GetEntities().size(), ui::IsPlayActive() ? "true" : "false");

    std::ofstream report("water_stress_report.txt", std::ios::app);
    report << "\n=== Water stress test: " << objectCount << " objects, " << frameCount
           << " frames, water " << 6000 << "x" << 6000 << " @" << kWaterHeight << " ===\n";

    TimingStats phaseStats;
    int framesDone = 0;
    int logEvery = 60;

    // Warm-up frames first (scene load + shell/chunk pop in), excluded from stats.
    for (int i = 0; i < 60; i++) {
        engine.StepFrame(1.0f / 60.0f);
    }

    // Diagnostic screenshots of the user-reported "darker middle" artifact: a low
    // camera nearly flat over the water, once level-ish and once slightly downward.
    // The images let us confirm the middle-vs-sides divergence is the reflection
    // edge fade rather than the shell/chunk disk. Reflections stay ON (the config
    // where the user sees it).
    {
        Camera3D diag = engine.GetCamera();
        diag.position = Vector3{ 0.0f, 3.0f, 0.0f };
        diag.target = Vector3{ 600.0f, 1.8f, -1200.0f };
        diag.up = Vector3{ 0.0f, 1.0f, 0.0f };
        diag.fovy = 55.0f;
        diag.projection = CAMERA_PERSPECTIVE;
        engine.GetCamera() = diag;
        engine.StepFrame(1.0f / 60.0f);
        TakeScreenshot("waterdiag_flat.png");

        diag.target = Vector3{ 400.0f, -0.5f, -800.0f };
        engine.GetCamera() = diag;
        engine.StepFrame(1.0f / 60.0f);
        TakeScreenshot("waterdiag_down.png");

        // Reflections OFF isolate: the column vanishes → it's the mirror term.
        gfx::SetReflectionsEnabled(false);
        diag.target = Vector3{ 600.0f, 1.8f, -1200.0f };
        engine.GetCamera() = diag;
        engine.StepFrame(1.0f / 60.0f);
        TakeScreenshot("waterdiag_flat_off.png");
        gfx::SetReflectionsEnabled(true);

        // Geometry probes: straight up (pure sky) and straight down (pure water).
        diag.target = Vector3{ 0.0f, 200.0f, 0.0f };
        engine.GetCamera() = diag;
        engine.StepFrame(1.0f / 60.0f);
        TakeScreenshot("waterdiag_up.png");
        diag.target = Vector3{ 0.0f, -200.0f, 0.0f };
        engine.GetCamera() = diag;
        engine.StepFrame(1.0f / 60.0f);
        TakeScreenshot("waterdiag_down_water.png");
    }

    // Coverage-geometry probes: ASCII maps of what Draw() actually renders at
    // several camera poses, plus matching screenshots - pinpoints any VOID
    // (unloaded) region and its shape around the body center.
    gfx::SetReflectionsEnabled(true);
    auto dumpCoverage = [&](const char* label, Vector3 pos, Vector3 tgt) {
        Camera3D c = engine.GetCamera();
        c.position = pos;
        c.target = tgt;
        c.up = Vector3{ 0.0f, 1.0f, 0.0f };
        c.fovy = 55.0f;
        c.projection = CAMERA_PERSPECTIVE;
        engine.GetCamera() = c;
        for (int wf = 0; wf < 15; wf++) engine.StepFrame(1.0f / 60.0f); // let the disk stream in
        char mapPath[160];
        snprintf(mapPath, sizeof(mapPath), "covmap_%s.txt", label);
        if (water) water->WriteCoverageMap(c, mapPath, 128);
        char shotPath[160];
        snprintf(shotPath, sizeof(shotPath), "waterdiag2_%s.png", label);
        TakeScreenshot(shotPath);
        printf("[covmap] %s chunks=%d -> %s (probe0,25=%d)\n", label,
               water ? water->GetDebugStats().totalChunks : -1, mapPath,
               (water && water->DebugHasChunkKey(0, 25)) ? 1 : 0);
    };

    dumpCoverage("level_center",    Vector3{ 0.0f, 3.0f, 0.0f },    Vector3{ 400.0f, 1.8f, -1200.0f });
    dumpCoverage("overhead_center", Vector3{ 0.0f, 340.0f, 0.0f },  Vector3{ 0.0f, 0.0f, 0.0f });
    dumpCoverage("pitch_center",    Vector3{ 0.0f, 200.0f, 300.0f }, Vector3{ 0.0f, 0.0f, 0.0f });
    dumpCoverage("look_negz",       Vector3{ 0.0f, 3.0f, 0.0f },    Vector3{ -400.0f, 1.8f, 900.0f });

    // Phase 1: reflections ON.
    gfx::SetReflectionsEnabled(true);
    for (int i = 0; i < frameCount; i++) {
        double t0 = GetTime();

        OrbitCamera(engine.GetCamera(), framesDone, kWaterHeight);
        engine.StepFrame(1.0f / 60.0f);

        double t1 = GetTime();
        FrameTiming ft;
        ft.frameMs = (float)(engine.lastShadowMs + engine.lastReflectionMs + engine.lastOpaqueMs +
                             engine.lastTransparentMs + engine.last2DMs);
        ft.shadowMs = (float)engine.lastShadowMs;
        ft.reflectionMs = (float)engine.lastReflectionMs;
        ft.opaqueMs = (float)engine.lastOpaqueMs;
        ft.transparentMs = (float)engine.lastTransparentMs;
        phaseStats.Add(ft, t1 - t0);
        framesDone++;

        if (i % logEvery == 0 || i == frameCount - 1) {
            WaterBody::DebugStats w = water ? water->GetDebugStats() : WaterBody::DebugStats{};
            LogFrame(framesDone, frameCount, ft, w, engine.GetCamera());
            report << "frame " << framesDone << ": total=" << ft.frameMs
                   << " shadow=" << ft.shadowMs << " refl=" << ft.reflectionMs
                   << " opaque=" << ft.opaqueMs << " trans=" << ft.transparentMs
                   << " chunks=" << w.totalChunks << " coarse=" << w.coarseChunkCount << "\n";
        }
    }
    WaterBody::DebugStats waterOn = water ? water->GetDebugStats() : WaterBody::DebugStats{};
    phaseStats.Finalize();
    PrintSummary("Phase 1: reflections ON", framesDone, phaseStats, waterOn, true);
    report << "Phase 1 (reflections ON) avg frame=" << phaseStats.Avg().frameMs
           << "ms reflection=" << phaseStats.Avg().reflectionMs << "ms fps="
           << 1000.0f / std::max(phaseStats.avgTotalMs, 0.001f) << "\n";

    TimingStats phase2;
    int framesFrozen = framesDone;

    // Phase 2: identical scene, reflections OFF - isolates the mirror pass cost.
    gfx::SetReflectionsEnabled(false);
    for (int i = 0; i < frameCount; i++) {
        double t0 = GetTime();

        OrbitCamera(engine.GetCamera(), framesFrozen, kWaterHeight);
        engine.StepFrame(1.0f / 60.0f);

        double t1 = GetTime();
        FrameTiming ft;
        ft.frameMs = (float)(engine.lastShadowMs + engine.lastReflectionMs + engine.lastOpaqueMs +
                             engine.lastTransparentMs + engine.last2DMs);
        ft.shadowMs = (float)engine.lastShadowMs;
        ft.reflectionMs = (float)engine.lastReflectionMs;
        ft.opaqueMs = (float)engine.lastOpaqueMs;
        ft.transparentMs = (float)engine.lastTransparentMs;
        phase2.Add(ft, t1 - t0);
        framesFrozen++;

        if (i % logEvery == 0 || i == frameCount - 1) {
            WaterBody::DebugStats w = water ? water->GetDebugStats() : WaterBody::DebugStats{};
            LogFrame(framesFrozen, frameCount, ft, w, engine.GetCamera());
        }
    }
    WaterBody::DebugStats waterOff = water ? water->GetDebugStats() : WaterBody::DebugStats{};
    phase2.Finalize();
    PrintSummary("Phase 2: reflections OFF", framesFrozen - framesDone, phase2, waterOff, false);
    report << "Phase 2 (reflections OFF) avg frame=" << phase2.Avg().frameMs
           << "ms fps=" << 1000.0f / std::max(phase2.avgTotalMs, 0.001f) << "\n";
    report.close();

    printf("[test] report written to water_stress_report.txt\n");
}

} // namespace watertest