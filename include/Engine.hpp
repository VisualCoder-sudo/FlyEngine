#pragma once

#include <memory>
#include <vector>
#include <string>
#include <functional>

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

    // Custom render loop support - call these manually instead of Run()
    void UpdateFrame(float deltaTime);  // Runs entity updates
    void RenderFrame();                 // Runs Draw (3D + 2D passes)
    
    // Optional callback for custom overlay rendering (called after 3D pass, before 2D pass)
    std::function<void()> onDrawOverlay3D;
    std::function<void()> onDrawOverlay2D;

    // When true, Draw() records per-pass GPU/thick-pass wall times on each frame
    // (ms). Default off; the stress harness turns it on.
    bool timingEnabled = false;
    double lastShadowMs = 0.0;
    double lastReflectionMs = 0.0;
    double lastOpaqueMs = 0.0;
    double lastTransparentMs = 0.0;
    double last2DMs = 0.0;

    // Player build: skips editor-only entities (ObjectInteractionManager, CameraController, etc.)
    bool isPlayerBuild = false;
    void SetPlayerBuild(bool v) { isPlayerBuild = v; }
    bool IsPlayerBuild() const { return isPlayerBuild; }

    // ---- Pause and time scale ------------------------------------------------
    //
    // Pausing freezes entity updates (physics, character controller, scripts)
    // while leaving rendering, input handling and the debug overlays running,
    // so a frozen frame stays on screen and can still be orbited and inspected.
    // This is what makes the physics debug layers usable: a contact or a bad
    // collision shape scrolls past in a fraction of a second otherwise.
    bool IsPaused() const { return paused; }
    void SetPaused(bool p) { paused = p; }
    void TogglePause() { paused = !paused; }

    // Queue one frame of simulation to run while paused. Steps queue instead of
    // applying immediately so that a held key (or console key auto-repeat)
    // advances a frame per repeat rather than dumping a burst at once.
    void StepFrame() { if (paused && pendingSteps < kMaxQueuedSteps) ++pendingSteps; }

    // Global multiplier on the dt handed to entities. 0.5 is half speed, 2.0 is
    // double. Applied to every entry point, so the editor loop and the player's
    // custom loop behave the same. Does not affect the frame-time statistics,
    // which always report real elapsed time.
    float GetTimeScale() const { return timeScale; }
    void SetTimeScale(float s);

private:
    // Cap on queued steps so a stuck key cannot build an unbounded backlog that
    // then plays back as a long freeze-then-fast-forward burst.
    static constexpr int kMaxQueuedSteps = 60;

    void Update(float deltaTime);
    void Draw();

    Color clearColor = RAYWHITE;
    Camera3D camera{};
    std::vector<std::unique_ptr<Entity>> entities;
    std::vector<std::unique_ptr<Entity>> pendingEntities;
    bool isUpdating = false;
    bool paused = false;
    int pendingSteps = 0;
    float timeScale = 1.0f;
};
