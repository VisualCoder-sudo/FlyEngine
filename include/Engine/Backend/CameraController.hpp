#pragma once
#include "Entity.hpp"
#include "raylib.h"

class CameraController : public Entity {
public:
    CameraController(Camera3D& cam);
    void Update(float dt) override;

    // Move the camera so `target` sits at the center of the current view, at a
    // sensible distance. Used by the editor's F-key focus.
    void FocusOn(Vector3 target);

    bool IsEditorOnly() const override { return true; }

    // Top-down orthographic view for plugin sketch modes. `viewHeight` is the
    // world-space height visible on screen. While active the fly controls are
    // replaced by map controls: WASD/arrows or right/middle-drag pan across the
    // ground (X and Z), the mouse wheel zooms towards the cursor.
    void EnterTopDown(Vector3 center, float viewHeight);
    void SetTopDownView(Vector3 center, float viewHeight);
    void ExitTopDown();
    bool IsTopDown() const { return topDown; }
    Vector3 TopDownCenter() const { return tdCenter; }
    float TopDownViewHeight() const { return tdHeight; }

private:
    Camera3D& camera;
    float yaw = 0.0f;
    float pitch = 0.0f;

    bool topDown = false;
    Camera3D savedCamera{};
    Vector3 tdCenter{};
    float tdHeight = 30.0f;
    bool tdDragging = false;

    void UpdateTopDown(float dt);
};