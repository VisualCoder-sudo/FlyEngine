#pragma once
#include "raylib.h"
#include "Atmosphere.hpp"

// Volumetric clouds (shaders/clouds.glsl): a layer of cloud between two heights
// in the atmosphere sky, lit by the sun and the sky, casting its shadow on the
// world. The Clouds item of the Explorer's Lighting section sets how much of the
// sky they cover, how high and thick the layer is and where the wind takes them;
// the Weather item's cloud cover and rain bring cloud of their own.
namespace gfx {

void InitClouds();
void ShutdownClouds();

// How much of the sky is cloud now, 0..1 (the Clouds item and the Weather item together).
float CloudCoverageNow();
// True when clouds are drawn: an atmosphere sky, some cover, and the noise textures made (they are
// built on other threads when clouds are first wanted, which takes a moment).
bool CloudsActive();

// Moves the clouds with the wind.
void UpdateClouds(float dt);

// Draws the clouds this view sees into their own (smaller) target and returns it: rgb = their light,
// a = how much of what is behind still shows. sceneDepth is the scene target's depth texture.
Texture2D RenderClouds(const ViewInfo& view, Texture2D sceneDepth, int sceneWidth, int sceneHeight, int frame);

// The clouds' shadow on the world, redrawn when the frame's lighting is set up: a texture over a
// square of ground round the camera (r = how much sunlight gets through). params: xy = the square's
// corner (world xz), z = 1 / its size, w = the height of the cloud base; with no clouds the texture
// is white and w is 0.
void UpdateCloudShadow(const Camera3D& camera);
Texture2D GetCloudShadowTexture();
Vector4 GetCloudShadowParams();
// The share of direct sunlight the clouds let through on average (for light that is not shaded per pixel).
float CloudSunlightNow();

} // namespace gfx
