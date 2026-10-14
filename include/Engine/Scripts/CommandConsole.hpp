#pragma once
#include "raylib.h"
#include <string>

namespace console {

void Update();               // call every frame (keyboard + mouse handling)
void Draw();                 // call last, inside the 2D pass

bool IsActive();             // the bar's text field has focus
bool IsOverBar(Vector2 point);
Rectangle GetBounds();       // bar rect, used for UI hit-testing

// Runs one console line without touching the bar. Returns false (and the message in *out) on an error.
bool RunLine(const std::string& line, std::string* out);

void Focus();
void Blur();

// Retained for API compatibility. Commands reach the camera through the
// ScriptRuntime world binding, so this is a no-op.
void AttachCamera(Camera3D& cam);

} // namespace console
