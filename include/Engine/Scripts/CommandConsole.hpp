#pragma once
#include "raylib.h"

namespace console {

void Update();               // call every frame (keyboard + mouse handling)
void Draw();                 // call last, inside the 2D pass

bool IsActive();             // the bar's text field has focus
bool IsOverBar(Vector2 point);
Rectangle GetBounds();       // bar rect, used for UI hit-testing

void Focus();
void Blur();

// Retained for API compatibility. C# commands reach the camera through the
// ScriptRuntime world binding, so this is a no-op.
void AttachCamera(Camera3D& cam);

} // namespace console
