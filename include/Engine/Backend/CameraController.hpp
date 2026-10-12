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

private:
    Camera3D& camera;
    float yaw = 0.0f;
    float pitch = 0.0f;
};