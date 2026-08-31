#pragma once

#include <memory>
#include <vector>
#include <string>

#include "raylib.h"
#include "Entity.hpp"

// Very small wrapper around a Raylib window + game loop.
// Create one, AddEntity() whatever you want in the world, call Run().
class Engine {
public:
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

private:
    void Update(float deltaTime);
    void Draw();

    Color clearColor = RAYWHITE;
    Camera3D camera{};
    std::vector<std::unique_ptr<Entity>> entities;
    std::vector<std::unique_ptr<Entity>> pendingEntities;
    bool isUpdating = false;
};
