#include "Engine.hpp"
#include "Graphics.hpp"
#include "ui.hpp"
#include "CommandConsole.hpp"
#include "ScriptEditor.hpp"
#include "ScatteredObject.hpp"
#include "Terrain.hpp"
#include "BasicTerrain.hpp"
#include "WaterBody.hpp"
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
    scriptEditor::Update();
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

    // 0. Shadow map pass: render occluders from the light's point of view.
    gfx::BeginShadowPass();
    for (auto& entity : entities) {
        entity->Draw();
    }
    gfx::EndShadowPass();

    gfx::UpdateLighting(camera);

    // 1. 3D Pass
    terrain::Terrain::SetDrawCamera(&camera);
    BasicTerrain::SetActiveCamera(&camera);
    WaterBody::SetActiveCamera(&camera);
    WaterBody::SetActiveEngine(this);
    BeginMode3D(camera);
        gfx::DrawGround();
        // Draw opaque entities first (they write depth)
        for (auto& entity : entities) {
            if (!entity->IsTransparent()) entity->Draw();
        }
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

    // 2. 2D Pass
    ui::Draw();
    scriptEditor::Draw();

    // ImGui pass (draws after every raylib overlay so it stays on top)
    ui::DrawImGuiFrame();

    EndDrawing();
}
void Engine::Run() {
    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        Update(dt);
        Draw();
    }
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