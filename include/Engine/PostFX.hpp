#pragma once
#include "raylib.h"

// From the lit scene to the picture on screen.
//
// Engine::Draw() renders everything 3D between BeginScene() and EndScene(). The
// scene goes into a float render target in linear light (a white surface in
// full noon sun is about 1; the sun's disc and lamps are far brighter). EndScene()
// then runs the post passes (shaders/post.glsl) and writes the finished picture
// to the screen target, where the 2D pass and the editor UI are drawn over it.
namespace gfx {

void InitPostFX();
void ShutdownPostFX();

// Binds the scene target, clears it and draws the sky. Follow with BeginMode3D().
void BeginScene(const Camera3D& camera);
// Call after EndMode3D(): post-processes the scene into the screen target.
void EndScene();
bool IsSceneActive();

// How much of the machine the picture may use. Not part of a scene: it is the
// player's (or the editor user's) choice, see the Rendering preferences.
struct RenderQuality {
    int tier = 2;                 // 0 = low, 1 = medium, 2 = high, 3 = ultra; SetQualityTier() fills in the rest
    float renderScale = 1.0f;     // the scene is rendered at this fraction of the window size
    int antiAliasing = 1;         // 0 = off, 1 = FXAA, 2 = temporal
    bool bloom = true;
    bool ambientOcclusion = true;
    int clouds = 2;               // 0 = flat layer, 1 = volumetric (fewer steps), 2 = volumetric, 3 = volumetric (more steps)
    int volumetrics = 2;          // 0 = plain distance fog, 1 = light shafts (fewer steps), 2 = light shafts
    int shadowCascades = 3;       // 1..4
};
RenderQuality& Quality();
void SetQualityTier(int tier);

// The 1x1 texture holding the exposure multiplier of this frame.
Texture2D GetExposureTexture();

// Tone curves on the CPU, the same as in shaders/post.glsl (0 = none, 1 = neutral,
// 2 = ACES, 3 = AgX). Display colours here are linear, 0..1.
Vector3 TonemapApply(int curve, Vector3 scene);
Vector3 TonemapInverse(int curve, Vector3 display);   // the scene light that the curve turns into `display`
Vector3 SrgbToLinear(Vector3 c);
Vector3 LinearToSrgb(Vector3 c);
Vector3 SrgbToLinear(Color c);
// The scene light (before exposure) that reaches the screen as exactly this colour.
Vector3 DisplayColorToScene(Color c);

} // namespace gfx
