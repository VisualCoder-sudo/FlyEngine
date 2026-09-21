#pragma once

#include <memory>
#include <vector>
#include <string>

#include "raylib.h"
#include "Engine/Backend/Entity.hpp"

// Very small wrapper around a Raylib window + game loop.
// Create one, AddEntity() whatever you want in the world, call Run().
class Engine {
public:
    // Largest per-frame time delta fed to entities (seconds). Prevents a long
    // stall from exploding physics sub-stepping / script dt.
    static constexpr float MAX_FRAME_DT = 0.05f;

    Engine(int width, int height, const std::string& title, int targetFPS = 180);
    ~Engine();

    // Takes ownership of the entity.
    Entity* AddEntity(std::unique_ptr<Entity> entity);
    void RemoveEntity(Entity* entity);

    // Background color used each frame before entities draw.
    void SetClearColor(Color color) { clearColor = color; }

    // The 3D camera entities are drawn through. Grab it to move/aim it.
    Camera3D& GetCamera() { return camera; }
    const std::vector<std::unique_ptr<Entity>>& GetEntities() const { return entities; }
    std::vector<std::unique_ptr<Entity>>& GetEntities() { return entities; }

    void Run();

    // Advance exactly one frame manually (Update + Draw). Used by the headless-ish
    // water stress harness so per-stage timings can be sampled deterministically.
    void StepFrame(float deltaTime);

    // When true, Draw() records per-pass GPU/thick-pass wall times on each frame
    // (ms). Default off; the stress harness turns it on.
    bool timingEnabled = false;
    double lastShadowMs = 0.0;
    double lastReflectionMs = 0.0;
    double lastOpaqueMs = 0.0;
    double lastTransparentMs = 0.0;
    double last2DMs = 0.0;

private:
    void Update(float deltaTime);
    void Draw();

    Color clearColor = RAYWHITE;
    Camera3D camera{};
    std::vector<std::unique_ptr<Entity>> entities;
    std::vector<std::unique_ptr<Entity>> pendingEntities;
    bool isUpdating = false;
};
