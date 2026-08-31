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

// Gives the console a live handle to the 3D camera so commands like
// "game.Camera.FOV.value = 60" can reach it. Call once after the camera exists.
void AttachCamera(Camera3D& cam);

} // namespace console
