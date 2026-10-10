#pragma once
#include "raylib.h"

// The sky: one colour (the editor's background, or the Sky item's colour), or a
// physical atmosphere with a sun, a moon and stars (shaders/sky.glsl).
//
// The atmosphere is a planet-sized shell of air that scatters the sun's light.
// Nothing in it is painted: the blue overhead, the pale horizon, red sunsets and
// the dark blue after dusk all come out of the same few numbers (what the air
// scatters, how much haze hangs in it), which is also what makes other planets'
// skies a matter of changing those numbers.
namespace gfx {

void InitAtmosphere();
void ShutdownAtmosphere();

// True when the Sky item draws a physical atmosphere instead of one colour.
bool AtmosphereActive();

// Once a frame, before anything draws the sky: rebuilds the tables if the Sky
// item changed and redraws the sky-view table for this camera.
void UpdateAtmosphere(const Camera3D& camera);

// Fills the current (float, scene-linear) target with the sky as `camera` sees
// it. Call right after clearing the target, before any geometry.
void DrawSky(const Camera3D& camera);

// The light the atmosphere puts on the scene now (linear, see Graphics.hpp).
struct SkyLight {
    Vector3 sunRadiance{};      // on a surface facing the sun (or the moon, at night)
    Vector3 ambientSky{};       // from the whole sky on a surface facing up
    Vector3 ambientGround{};    // bounced off the ground on a surface facing down
    Vector3 skyRadiance{};      // the sky's own light well above the horizon (for reflections)
    Vector3 horizon{};          // the sky's light at the horizon (what distance fades into)
    Vector3 keyLight{};         // the sun's (or the moon's) light arriving through the air, before any cloud
    Vector3 clearSky{};         // the sky's light on an upward surface if there were no cloud (it lights the clouds' tops)
};
const SkyLight& AtmosphereLight();

// The atmosphere for other passes (clouds, fog): its uniforms (see
// fly_atmosphere.glsl) and tables.
struct AtmosphereLocs {
    int rayleigh = -1, mie = -1, mieAbs = -1, ozone = -1, planet = -1;
    int transLut = -1, msLut = -1, skyViewLut = -1;
};
AtmosphereLocs FindAtmosphereLocs(Shader shader);
void SetAtmosphereUniforms(Shader shader, const AtmosphereLocs& locs);
Vector3 AtmosphereSunLight();   // the sun's light above the air (linear)
Vector3 AtmosphereMoonLight();  // the moon's

// The view of a camera, for full-screen passes that need world positions (the
// fly_camera block in shaders/fly_common.glsl).
struct ViewInfo {
    Vector4 proj{};             // (1/P00, 1/P11, P20, P21); orthographic: (1/P00, 1/P11, P30, P31)
    Vector4 depth{};            // (P22, P32, orthographic ? 1 : 0, far plane)
    Vector3 right{}, up{}, fwd{}, pos{};
    Matrix invView{};           // camera to world
    Matrix viewProj{};          // world to clip space (OpenGL's: z in -w..w), without any jitter
    bool ortho = false;
    float pixelAngle = 0.0f;    // radians one pixel spans
};
ViewInfo MakeViewInfo(const Camera3D& camera, int width, int height);

// The air between the camera and the scene, for the fog pass (fs_fog in shaders/post.glsl): what it scatters
// per metre and the light it scatters. All zero under a sky of one colour (there is no air to speak of).
struct AerialParams {
    Vector4 airR{}, airM{}, airExt{};
    Vector4 light{};            // rgb = the sun's (or the moon's) light once through the air above
    Vector4 lightDir{};         // xyz = direction towards it
    Vector4 multi{};            // rgb = light scattered more than once, per unit of scattering
};
AerialParams GetAerialParams();

} // namespace gfx
