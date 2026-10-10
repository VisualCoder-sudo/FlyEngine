#pragma once
#include "raylib.h"

// The sky: one colour (the editor's background, or the Sky item's colour), or a
// physical atmosphere with a sun, a moon and stars (shaders/sky.glsl).
namespace gfx {

void InitAtmosphere();
void ShutdownAtmosphere();

// True when the Sky item draws a physical atmosphere instead of one colour.
bool AtmosphereActive();

// Fills the current (float, scene-linear) target with the sky as `camera` sees
// it. Call right after clearing the target, before any geometry.
void DrawSky(const Camera3D& camera);

} // namespace gfx
