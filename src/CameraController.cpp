#include "CameraController.hpp"
#include "ui.hpp"
#include "raymath.h"
#include <cmath>

CameraController::CameraController(Camera3D& cam)
    : camera(cam) {
    Vector3 dir = {
        camera.target.x - camera.position.x,
        camera.target.y - camera.position.y,
        camera.target.z - camera.position.z
    };
    float xzLen = sqrtf(dir.x * dir.x + dir.z * dir.z);
    yaw = atan2f(dir.z, dir.x);
    pitch = atan2f(dir.y, xzLen);
}

void CameraController::Update(float dt) {
    static Vector2 rmbPressPos = { 0, 0 };
    static float dragDist = 0.0f;
    static bool isCapturingMouse = false;

    // 1. Right Click Down
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        rmbPressPos = GetMousePosition();
        dragDist = 0.0f;
        isCapturingMouse = false;
    }

    // 2. Right Click Drag
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        Vector2 delta = GetMouseDelta();
        dragDist += fabsf(delta.x) + fabsf(delta.y);

        // Transition into camera rotation ONLY after dragging past 5 pixels
        if (dragDist > 5.0f && !isCapturingMouse) {
            isCapturingMouse = true;
            ui::CloseContextMenu(); // Dismiss menu as soon as camera drag starts
            DisableCursor();        // Lock hardware cursor for smooth 3D camera look
        }

        if (isCapturingMouse) {
            float sensitivity = 0.003f;
            yaw += delta.x * sensitivity;
            pitch -= delta.y * sensitivity;

            const float maxPitch = 89.0f * (PI / 180.0f);
            if (pitch > maxPitch) pitch = maxPitch;
            if (pitch < -maxPitch) pitch = -maxPitch;
        }
    }

    // 3. Right Click Release
    if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) {
        if (isCapturingMouse) {
            EnableCursor();
            SetMousePosition((int)rmbPressPos.x, (int)rmbPressPos.y); // Return cursor to original click point
            isCapturingMouse = false;
        }
    }

    // Freeze camera keyboard control if editing text field
    if (ui::IsEditingText()) return;

    // Ctrl is used for editor shortcuts (Ctrl+S save, Ctrl+D duplicate);
    // don't let those keys also fly the camera.
    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) return;

    // WASD Movement
    Vector3 forward = { cosf(pitch) * cosf(yaw), sinf(pitch), cosf(pitch) * sinf(yaw) };
    Vector3 right = { -sinf(yaw), 0.0f, cosf(yaw) };

    Vector3 move = { 0.0f, 0.0f, 0.0f };
    float moveSpeed = 12.0f * dt;

    if (IsKeyDown(KEY_W)) { move.x += forward.x * moveSpeed; move.y += forward.y * moveSpeed; move.z += forward.z * moveSpeed; }
    if (IsKeyDown(KEY_S)) { move.x -= forward.x * moveSpeed; move.y -= forward.y * moveSpeed; move.z -= forward.z * moveSpeed; }
    if (IsKeyDown(KEY_A)) { move.x -= right.x * moveSpeed; move.z -= right.z * moveSpeed; }
    if (IsKeyDown(KEY_D)) { move.x += right.x * moveSpeed; move.z += right.z * moveSpeed; }
    if (IsKeyDown(KEY_E)) move.y += moveSpeed;
    if (IsKeyDown(KEY_Q)) move.y -= moveSpeed;

    camera.position.x += move.x;
    camera.position.y += move.y;
    camera.position.z += move.z;

    camera.target = {
        camera.position.x + forward.x,
        camera.position.y + forward.y,
        camera.position.z + forward.z
    };
}

void CameraController::FocusOn(Vector3 target) {
    Vector3 forward = { cosf(pitch) * cosf(yaw), sinf(pitch), cosf(pitch) * sinf(yaw) };
    float dist = Clamp(Vector3Distance(camera.position, target), 3.0f, 20.0f);
    camera.position = Vector3Subtract(target, Vector3Scale(forward, dist));
    camera.target = target;
}