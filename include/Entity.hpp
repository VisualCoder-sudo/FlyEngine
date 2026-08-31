#pragma once

// Base class for anything that lives in the world.
// Inherit from this, override Update/Draw, and add it to the Engine.
class Entity {
public:
    virtual ~Entity() = default;

    virtual void Update(float deltaTime) {}
    virtual void Draw() {}
    // Drawn after every entity in the 3D pass, so editor overlays (transform
    // gizmos) always render on top of the scene regardless of entity order.
    virtual void DrawOverlay3D() {}
    virtual bool IsTransparent() const { return false; }

    bool alive = true; // set to false to have the Engine remove it
};

