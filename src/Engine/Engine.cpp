#include "../../include/Engine.hpp"
#include "../../include/Engine/Graphics.hpp"
#include "../../include/Engine/Frontend/ui.hpp"
#include "../../include/Engine/Scripts/CommandConsole.hpp"
#include "../../include/Engine/Backend/ScatteredObject.hpp"
#include "../include/Terrain/Terrain.hpp"
#include "../../include/Terrain/BasicTerrain.hpp"
#include "../../include/Terrain/Water/WaterBody.hpp"
#include <algorithm>
#include <cmath>

Engine::Engine(int width, int height, const std::string& title, int targetFPS) {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(width, height, title.c_str());
    SetWindowMinSize(1024, 640);
    SetExitKey(KEY_NULL);
    SetTargetFPS(targetFPS);

    gfx::Init();

    camera.position = { 10.0f, 10.0f, 10.0f };
    camera.target = { 0.0f, 0.0f, 0.0f };
    camera.up = { 0.0f, 1.0f, 0.0f };
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    console::AttachCamera(camera);
}

Engine::~Engine() {
    // Destroy in reverse creation order: entities added later (character
    // controller, interaction manager) hold references to ones added earlier
    // (the physics simulation). vector::clear() destroys front to back, which
    // crashed the Player on exit.
    while (!pendingEntities.empty()) pendingEntities.pop_back();
    while (!entities.empty()) entities.pop_back();
    gfx::Shutdown();
    CloseWindow();
}

Entity* Engine::AddEntity(std::unique_ptr<Entity> entity) {
    Entity* rawEntity = entity.get();
    if (isUpdating) {
        pendingEntities.push_back(std::move(entity));
        return rawEntity;
    }
    entities.push_back(std::move(entity));
    return rawEntity;
}

void Engine::SetTimeScale(float s) {
    // Clamped rather than rejected: time scale is reachable from the console,
    // where a typo like "timescale 0" would otherwise silently stop the world.
    // Zero is still allowed (that is a legitimate freeze), the floor just stops
    // a negative value from running time backwards into physics.
    timeScale = std::clamp(s, 0.0f, 10.0f);
}

void Engine::Update(float deltaTime) {
    console::Update();
    gfx::TickLighting(deltaTime);
    if (!isPlayerBuild) ui::UpdateInput();

    // Decide whether entities tick this frame. A paused engine skips the entity
    // loop outright rather than calling Update(0): update code routinely
    // branches on dt and sometimes divides by it, so a zero dt is not a safe
    // way to say "no time passed".
    bool tickEntities = true;
    if (paused) {
        if (pendingSteps > 0) {
            --pendingSteps;       // a queued single-frame step consumes one
        } else {
            tickEntities = false;
        }
    }

    if (tickEntities) {
        isUpdating = true;
        for (auto& entity : entities) {
            if (isPlayerBuild && entity->IsEditorOnly()) continue;
            entity->Update(deltaTime * timeScale);
        }
        isUpdating = false;
    }

    // Housekeeping runs even on a frozen frame, so entities that marked
    // themselves dead are still reaped and queued additions still land. A
    // paused engine that refused new entities would be a nasty surprise.

    // Remove anything that marked itself dead.
    entities.erase(
        std::remove_if(entities.begin(), entities.end(),
            [](const std::unique_ptr<Entity>& e) { return !e->alive; }),
        entities.end());

    for (auto& entity : pendingEntities) entities.push_back(std::move(entity));
    pendingEntities.clear();
}

void Engine::Draw() {
    // Update LOD camera position so objects can compute distance-based textures
    SetLODCameraPos(camera.position);

    BeginDrawing();
    const Color skyColor = gfx::SkyColor(clearColor);
    ClearBackground(skyColor);

    double p0 = GetTime();
    // 0. Shadow map pass: render occluders from the light's point of view.
    gfx::BeginShadowPass();
    for (auto& entity : entities) {
        if (isPlayerBuild && entity->IsEditorOnly()) continue;
        entity->Draw();
    }
    gfx::EndShadowPass();
    double p1 = GetTime();

    gfx::UpdateLighting(camera);

    // 0.5 Planar reflection pass - render the opaque world from a camera mirrored
    // below the first play-active water body's surface. Water shaders sample this
    // image during the transparent pass, so mountains and objects show up in the water.
    if (gfx::IsReflectionsEnabled() && ui::IsPlayActive()) {
        WaterBody* reflBody = nullptr;
        for (auto& entity : entities) {
            auto* wb = dynamic_cast<WaterBody*>(entity.get());
            if (wb) { reflBody = wb; break; }
        }
        if (reflBody) {
            Camera3D reflCam = gfx::BeginReflectionPass(reflBody->GetWaterHeight(), camera, skyColor);
            terrain::Terrain::SetDrawCamera(&reflCam);
            BasicTerrain::SetActiveCamera(&reflCam);
            BeginMode3D(reflCam);
                gfx::DrawGround();
                // Draw opaque entities first (they write depth). Anything fully
                // underwater has no reflection (the mirrored camera would wrongly
                // lift it above the surface), so skip it.
                for (auto& entity : entities) {
                    if (isPlayerBuild && entity->IsEditorOnly()) continue;
                    if (entity->IsTransparent()) continue;
                    auto* obj = dynamic_cast<ScatteredObject*>(entity.get());
                    if (obj) {
                        Vector3 op = *obj->GetPosPtr();
                        Vector3 os = *obj->GetSizePtr();
                        if (op.y + os.y * 0.5f < reflBody->GetWaterHeight()) continue;
                    }
                    entity->Draw();
                }
            // Must be called while still inside BeginMode3D(reflCam): it reads the
            // mirrored camera's matrices, then ends both the 3D and texture modes.
            gfx::EndReflectionPass();
            terrain::Terrain::SetDrawCamera(&camera);
            BasicTerrain::SetActiveCamera(&camera);
        }
    }
    double p2 = GetTime();

    // 1. 3D Pass
    terrain::Terrain::SetDrawCamera(&camera);
    BasicTerrain::SetActiveCamera(&camera);
    WaterBody::SetActiveCamera(&camera);
    WaterBody::SetActiveEngine(this);
    
    // Reset debug counters at start of frame
    gfx::ResetFrameStats();
    
    double pOpaque = GetTime();
    BeginMode3D(camera);
        gfx::DrawGround();
        gfx::IncrementDrawCallCount(1); // ground plane
        gfx::AddMeshCount(1);
        gfx::AddTriangleCount(6); // ground plane is 2 triangles
        // Draw opaque entities first (they write depth)
        for (auto& entity : entities) {
            if (isPlayerBuild && entity->IsEditorOnly()) continue;
            if (!entity->IsTransparent()) {
                entity->Draw(); // Entity handles its own frustum culling and draw call counting
            }
        }
        pOpaque = GetTime();
        // Draw transparent entities last (water renders on top, reads depth)
        for (auto& entity : entities) {
            if (isPlayerBuild && entity->IsEditorOnly()) continue;
            if (entity->IsTransparent()) {
                entity->Draw(); // Entity handles its own frustum culling and draw call counting
            }
        }
        // Editor overlays (transform gizmos) render after every entity so they
        // stay readable even when buried inside another object.
        for (auto& entity : entities) {
            if (isPlayerBuild && entity->IsEditorOnly()) continue;
            entity->DrawOverlay3D();
        }
    EndMode3D();
    double p3 = GetTime();

    // Custom 3D overlay callback (debug stats, etc.)
    if (onDrawOverlay3D) {
        onDrawOverlay3D();
    }

    // 2. 2D Pass
    if (!isPlayerBuild) ui::Draw();

    // ImGui pass (draws after every raylib overlay so it stays on top)
    if (!isPlayerBuild) ui::DrawImGuiFrame(camera);

    // Custom 2D overlay callback (debug stats, etc.)
    if (onDrawOverlay2D) {
        onDrawOverlay2D();
    }

    EndDrawing();
    double p4 = GetTime();

    // The GetTime() calls above run unconditionally, so publish the results
    // unconditionally too -- the F2 overlay shows them, and gating them behind
    // timingEnabled meant the editor showed nothing while the stress harness
    // saw the real numbers.
    gfx::FrameTimings t;
    t.shadowMs      = (p1 - p0) * 1000.0;
    t.reflectionMs  = (p2 - p1) * 1000.0;
    t.opaqueMs      = (pOpaque - p2) * 1000.0;
    t.transparentMs = (p3 - pOpaque) * 1000.0;
    t.twoDMs        = (p4 - p3) * 1000.0;
    gfx::SetFrameTimings(t);

    // Kept as public members for the stress harness, which still gates on
    // timingEnabled so its own numbers are only read when it asked for them.
    if (timingEnabled) {
        lastShadowMs = t.shadowMs;
        lastReflectionMs = t.reflectionMs;
        lastOpaqueMs = t.opaqueMs;
        lastTransparentMs = t.transparentMs;
        last2DMs = t.twoDMs;
    }
}
void Engine::Run() {
    while (!WindowShouldClose()) {
        // Clamp the frame delta so a hitch (window drag, modal hang, debugger
        // break, tab switch) can't push physics/simulation into a multi-second
        // jump that destabilizes the fixed-step integrator.
        float dt = fminf(GetFrameTime(), MAX_FRAME_DT);
        Update(dt);
        Draw();
    }
}

void Engine::UpdateFrame(float deltaTime) {
    Update(deltaTime);
}

void Engine::RenderFrame() {
    Draw();
}

void Engine::StepFrame(float deltaTime) {
    Update(deltaTime);
    Draw();
}

void Engine::RemoveEntity(Entity* entity) {
    if (!entity) return;
    entity->alive = false;

    if (!isUpdating) {
        entities.erase(
            std::remove_if(entities.begin(), entities.end(),
                [entity](const std::unique_ptr<Entity>& e) { return e.get() == entity; }),
            entities.end());
    }
}