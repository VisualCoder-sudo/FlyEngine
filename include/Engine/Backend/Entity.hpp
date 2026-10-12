#pragma once

#include "Frustum.hpp"

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

    // Trait system: editor-only entities return true to be skipped in Player builds
    virtual bool IsEditorOnly() const { return false; }
    virtual bool IsGameplay() const { return true; }

    // Frustum culling: return true if this entity should be rendered given the
    // current view frustum. Default implementation returns true (always visible).
    // Override to implement culling using GetBoundingBox() or custom logic.
    // Physics and collisions are still processed regardless of this result.
    virtual bool IsVisible(const Frustum& frustum) const { return true; }

    // Get the axis-aligned bounding box for frustum culling.
    // Default returns an empty box at origin; override to provide actual bounds.
    virtual BoundingBox GetCullBounds() const { return BoundingBox{}; }

    bool alive = true; // set to false to have the Engine remove it
};

