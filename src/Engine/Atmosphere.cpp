#include "../../include/Engine/Atmosphere.hpp"
#include "../../include/Engine/Graphics.hpp"
#include "../../include/Engine/PostFX.hpp"
#include "raymath.h"

namespace gfx {

namespace {

struct AtmosphereState {
    bool initialized = false;
    Shader flatShader{};
    int flatSkyLoc = -1, flatExposureLoc = -1;
};
AtmosphereState as;

} // namespace

void InitAtmosphere() {
    if (as.initialized) return;
    as.flatShader = LoadShaderProgram("sky_flat");
    as.flatSkyLoc = GetShaderLocation(as.flatShader, "skyHdr");
    as.flatExposureLoc = GetShaderLocation(as.flatShader, "exposureTex");
    as.initialized = true;
}

void ShutdownAtmosphere() {
    if (!as.initialized) return;
    UnloadShader(as.flatShader);
    as = AtmosphereState{};
}

bool AtmosphereActive() { return false; }

void DrawSky(const Camera3D&) {
    if (!as.initialized) InitAtmosphere();
    // One colour: handed over as the light that the exposure and the tone curve
    // turn back into exactly that colour.
    const Vector3 hdr = DisplayColorToScene(CurrentSky());
    const Vector4 sky = { hdr.x, hdr.y, hdr.z, 1.0f };
    SetShaderValue(as.flatShader, as.flatSkyLoc, &sky, SHADER_UNIFORM_VEC4);
    SetShaderValueTexture(as.flatShader, as.flatExposureLoc, GetExposureTexture());
    DrawFullscreen(as.flatShader, -1);
}

} // namespace gfx
