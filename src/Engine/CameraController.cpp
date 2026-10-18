#include "../../include/Engine/Backend/CameraController.hpp"
#include "../../include/Engine/Frontend/ui.hpp"
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

void CameraController::EnterTopDown(Vector3 center, float viewHeight) {
    if (!topDown) savedCamera = camera;
    topDown = true;
    tdDragging = false;
    SetTopDownView(center, viewHeight);
}

void CameraController::SetTopDownView(Vector3 center, float viewHeight) {
    if (!topDown) return;
    tdCenter = center;
    tdHeight = Clamp(viewHeight, 2.0f, 400.0f);
    camera.projection = CAMERA_ORTHOGRAPHIC;
    camera.fovy = tdHeight;
    camera.position = { center.x, center.y + 150.0f, center.z };
    camera.target = center;
    camera.up = { 0.0f, 0.0f, -1.0f };   // screen right = +X, screen down = +Z
}

void CameraController::ExitTopDown() {
    if (!topDown) return;
    topDown = false;
    tdDragging = false;
    camera = savedCamera;
}

// Map controls. The view is orthographic, so a screen pixel is a fixed length on the ground.
void CameraController::UpdateTopDown(float dt) {
    const float screenW = (float)GetScreenWidth();
    const float screenH = (float)std::max(1, GetScreenHeight());
    const Vector2 mouse = GetMousePosition();
    const bool overUI = ui::IsMouseOverUI();
    Vector3 center = tdCenter;
    float height = tdHeight;
    const float worldPerPixel = height / screenH;

    // Drag pan: grab the ground with the right or middle button.
    const bool dragButton = IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE);
    if (!dragButton) tdDragging = false;
    else if (!tdDragging && !overUI &&
             (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) || IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE))) tdDragging = true;
    if (tdDragging) {
        const Vector2 delta = GetMouseDelta();
        center.x -= delta.x * worldPerPixel;
        center.z -= delta.y * worldPerPixel;
    }

    // Wheel zoom keeps the ground point under the cursor where it is.
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f && !overUI) {
        const float newHeight = Clamp(height * powf(0.88f, wheel), 2.0f, 400.0f);
        const float dx = mouse.x - screenW * 0.5f, dz = mouse.y - screenH * 0.5f;
        center.x += dx * (worldPerPixel - newHeight / screenH);
        center.z += dz * (worldPerPixel - newHeight / screenH);
        height = newHeight;
    }

    // Keys: WASD or arrows, shift for speed. Not while typing or while Ctrl is an editor shortcut.
    if (!ui::IsEditingText() && !IsKeyDown(KEY_LEFT_CONTROL) && !IsKeyDown(KEY_RIGHT_CONTROL)) {
        const float step = height * 0.8f * dt * (IsKeyDown(KEY_LEFT_SHIFT) ? 2.5f : 1.0f);
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP))    center.z -= step;
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN))  center.z += step;
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))  center.x -= step;
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) center.x += step;
    }

    SetTopDownView(center, height);
}

void CameraController::Update(float dt) {
    if (topDown) { UpdateTopDown(dt); return; }

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