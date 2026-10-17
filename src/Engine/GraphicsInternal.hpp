#pragma once
#include "raylib.h"

// The city's instanced lit program, owned by Graphics.cpp, for CityInstancing.cpp.
namespace gfx {
namespace detail {

Shader& CityInstancedShader();
Material& CityInstancedMaterial();
bool CityInstancedReady();
void SetupInstancedLighting();   // hands the scene's light to the instanced shader (once a frame); call before each instanced draw

} // namespace detail
} // namespace gfx
