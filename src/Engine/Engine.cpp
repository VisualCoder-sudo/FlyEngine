#include "../../include/Engine.hpp"
#include "../../include/Engine/Graphics.hpp"
#include "../../include/Engine/Frontend/ui.hpp"
#include "../../include/Engine/Scripts/CommandConsole.hpp"
#include "../../include/Engine/Backend/ScatteredObject.hpp"
#include "../include/Terrain/Terrain.hpp"
#include "../../include/Terrain/BasicTerrain.hpp"
#include "../../include/Terrain/Water/WaterBody.hpp"
#include <algorithm>

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
    entities.clear();
    pendingEntities.clear();
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

void Engine::Update(float deltaTime) {
    console::Update();
    ui::UpdateInput();

    isUpdating = true;
    for (auto& entity : entities) {
        entity->Update(deltaTime);
    }
    isUpdating = false;

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
    ClearBackground(clearColor);

    double p0 = GetTime();
    // 0. Shadow map pass: render occluders from the light's point of view.
    gfx::BeginShadowPass();
    for (auto& entity : entities) {
        entity->Draw();
    }
    gfx::EndShadowPass();
    double p1 = GetTime();

    gfx::UpdateLighting(camera);

    // 0.5 Planar reflection pass — render the opaque world from a camera mirrored
    // below the first play-active water body's surface. Water shaders sample this
    // image during the transparent pass, so mountains and objects show up in the water.
    if (gfx::IsReflectionsEnabled() && ui::IsPlayActive()) {
        WaterBody* reflBody = nullptr;
        for (auto& entity : entities) {
            auto* wb = dynamic_cast<WaterBody*>(entity.get());
            if (wb) { reflBody = wb; break; }
        }
        if (reflBody) {
            Camera3D reflCam = gfx::BeginReflectionPass(reflBody->GetWaterHeight(), camera, clearColor);
            terrain::Terrain::SetDrawCamera(&reflCam);
            BasicTerrain::SetActiveCamera(&reflCam);
            BeginMode3D(reflCam);
                gfx::DrawGround();
                // Draw opaque entities first (they write depth). Anything fully
                // underwater has no reflection (the mirrored camera would wrongly
                // lift it above the surface), so skip it.
                for (auto& entity : entities) {
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
    double pOpaque = GetTime();
    BeginMode3D(camera);
        gfx::DrawGround();
        // Draw opaque entities first (they write depth)
        for (auto& entity : entities) {
            if (!entity->IsTransparent()) entity->Draw();
        }
        pOpaque = GetTime();
        // Draw transparent entities last (water renders on top, reads depth)
        for (auto& entity : entities) {
            if (entity->IsTransparent()) entity->Draw();
        }
        // Editor overlays (transform gizmos) render after every entity so they
        // stay readable even when buried inside another object.
        for (auto& entity : entities) {
            entity->DrawOverlay3D();
        }
    EndMode3D();
    double p3 = GetTime();

    // 2. 2D Pass
    ui::Draw();

    // ImGui pass (draws after every raylib overlay so it stays on top)
    ui::DrawImGuiFrame(camera);

    EndDrawing();
    double p4 = GetTime();

    if (timingEnabled) {
        lastShadowMs = (p1 - p0) * 1000.0;
        lastReflectionMs = (p2 - p1) * 1000.0;
        lastOpaqueMs = (pOpaque - p2) * 1000.0;
        lastTransparentMs = (p3 - pOpaque) * 1000.0;
        last2DMs = (p4 - p3) * 1000.0;
    }
}
void Engine::Run() {
    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        Update(dt);
        Draw();
    }
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