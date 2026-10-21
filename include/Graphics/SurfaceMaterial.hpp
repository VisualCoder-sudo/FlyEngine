#pragma once
#include "raylib.h"

namespace gfx {

// A surface's physically based material (shaders/lit.glsl). Colours are sRGB like every picked colour.
struct SurfaceMaterial {
    float metallic = 0.0f;          // 0 = plastic, wood, stone...; 1 = bare metal (its colour tints the reflection)
    float roughness = 0.6f;         // 0 = mirror, 1 = fully matte
    Color emissive = { 255, 255, 255, 255 };
    float emissiveStrength = 0.0f;  // 0 = gives off no light; 1 = as bright as a white surface in full sun
    bool operator==(const SurfaceMaterial& o) const {
        return metallic == o.metallic && roughness == o.roughness && emissiveStrength == o.emissiveStrength &&
               emissive.r == o.emissive.r && emissive.g == o.emissive.g && emissive.b == o.emissive.b;
    }
    bool operator!=(const SurfaceMaterial& o) const { return !(*this == o); }
};

} // namespace gfx
