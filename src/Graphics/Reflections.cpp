#include <cstring>
#include "../../include/Graphics/Graphics.hpp"
#include "../../include/Graphics/Atmosphere.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <algorithm>
#include <cmath>

namespace {

int reflectionResolution = 768;
int reflectionQuality = 50;
constexpr int REFLECTION_TEXTURE_SLOT = 11;
bool reflectionEnabled = true;
bool inReflectionPass = false;
float reflPlaneY = 0.0f;
RenderTexture2D reflectionTarget{};
Matrix reflView = MatrixIdentity();
Matrix reflProj = MatrixIdentity();
Matrix reflViewProj = MatrixIdentity();

int ReflectionQualityToResolution(int quality) {
    quality = Clamp(quality, 5, 100);
    float t = (float)(quality - 5) / 95.0f;
    return 360 + (int)std::lroundf(t * 1080.0f); // 360..1440
}

// The mirrored camera must see the same aspect as the screen, or its horizontal FOV is narrower
// and the reflection only covers the centre. The long side gets the quality resolution.
int reflW = 768, reflH = 768;
void ReflectionDims(int res, int& w, int& h) {
    const float aspect = (float)GetScreenWidth() / fmaxf((float)GetScreenHeight(), 1.0f);
    if (aspect >= 1.0f) { w = res; h = std::max(1, (int)lroundf(res / aspect)); }
    else { h = res; w = std::max(1, (int)lroundf(res * aspect)); }
}

// Float, like the scene target: the water reflects the scene's linear light.
void CreateTarget() {
    reflectionTarget = LoadRenderTextureEx(reflW, reflH, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16, true);
    SetTextureFilter(reflectionTarget.texture, TEXTURE_FILTER_BILINEAR);
}

}

namespace gfx {

void InitReflections() {
    ReflectionDims(reflectionResolution, reflW, reflH);
    CreateTarget();
}

void ShutdownReflections() {
    if (reflectionTarget.id > 0) UnloadRenderTexture(reflectionTarget);
    reflectionTarget = {};
}

void SetReflectionsEnabled(bool enabled) { reflectionEnabled = enabled; }

bool IsReflectionsEnabled() { return reflectionEnabled; }

void SetReflectionQuality(int quality) {
    quality = Clamp(quality, 5, 100);
    if (quality == reflectionQuality && reflectionTarget.id > 0) return;
    reflectionQuality = quality;

    int resolution = ReflectionQualityToResolution(quality);
    if (resolution == reflectionResolution && reflectionTarget.id > 0) return;
    reflectionResolution = resolution;

    if (reflectionTarget.id > 0) UnloadRenderTexture(reflectionTarget);
    ReflectionDims(reflectionResolution, reflW, reflH);
    CreateTarget();
}

int GetReflectionQuality() { return reflectionQuality; }

Camera3D BeginReflectionPass(float planeHeight, const Camera3D& worldCamera, Color clearColor) {
    if (!reflectionEnabled || reflectionTarget.id == 0) {
        inReflectionPass = false;
        return worldCamera;
    }

    {   // Screen aspect changed (window resize): rebuild the target to match.
        int w, h;
        ReflectionDims(reflectionResolution, w, h);
        if (w != reflW || h != reflH) {
            UnloadRenderTexture(reflectionTarget);
            reflW = w; reflH = h;
            CreateTarget();
        }
    }

    inReflectionPass = true;
    reflPlaneY = planeHeight;

    // Mirror the camera about the horizontal plane y = planeHeight. Objects above
    // the water appear below the mirrored camera, producing a true planar mirror.
    Camera3D mirrored = worldCamera;
    float planeY = planeHeight;
    mirrored.position.y = 2.0f * planeY - mirrored.position.y;
    mirrored.target.y = 2.0f * planeY - mirrored.target.y;
    mirrored.up.y = -mirrored.up.y;

    // Guard against a degenerate basis when looking straight up/down (the mirrored
    // up vector becomes parallel to the view direction).
    Vector3 fwd = Vector3Normalize(Vector3Subtract(mirrored.target, mirrored.position));
    if (fabsf(Vector3DotProduct(fwd, mirrored.up)) > 0.999f) {
        mirrored.up = { 0.0f, 0.0f, 1.0f };
    }

    (void)clearColor;   // the sky pass covers the target
    BeginTextureMode(reflectionTarget);
    ClearBackground(BLACK);
    DrawSky(mirrored);

    return mirrored;
}

void ClipReflectionPassToWater() {
    if (!inReflectionPass || reflectionTarget.id == 0) return;

    Matrix view = rlGetMatrixModelview();
    Matrix proj = rlGetMatrixProjection();
    reflProj = proj;    // unclipped; the water shader samples with this one

    // Oblique near-plane clip (Lengyel): geometry below the water surface must not
    // be mirrored up, or submerged parts of objects show as ghosts in the reflection.
    // World plane y = planeY - bias, kept side is y above it.
    const float bias = 0.05f;
    const float pl[4] = { 0.0f, 1.0f, 0.0f, -(reflPlaneY - bias) };
    const Matrix invView = MatrixInvert(view);
    const float* v = MatrixToFloatV(invView).v;   // v[n] == mN (column-major)
    float c[4];
    for (int i = 0; i < 4; i++)
        c[i] = v[i*4+0]*pl[0] + v[i*4+1]*pl[1] + v[i*4+2]*pl[2] + v[i*4+3]*pl[3];

    // Camera sits at the view-space origin; it must be on the clipped side.
    if (c[3] >= 0.0f) return;

    float m[16];
    memcpy(m, MatrixToFloatV(proj).v, sizeof(m));
    auto sgn = [](float x) { return x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f); };
    float q[4] = { (sgn(c[0]) + m[8]) / m[0], (sgn(c[1]) + m[9]) / m[5], -1.0f, (1.0f + m[10]) / m[14] };
    float k = 2.0f / (c[0]*q[0] + c[1]*q[1] + c[2]*q[2] + c[3]*q[3]);
    m[2] = c[0]*k; m[6] = c[1]*k; m[10] = c[2]*k + 1.0f; m[14] = c[3]*k;
    rlSetMatrixProjection({ m[0], m[4], m[8],  m[12],
                            m[1], m[5], m[9],  m[13],
                            m[2], m[6], m[10], m[14],
                            m[3], m[7], m[11], m[15] });
}

void EndReflectionPass() {
    if (!inReflectionPass || reflectionTarget.id == 0) return;

    reflView = rlGetMatrixModelview();

    rlDrawRenderBatchActive();
    EndMode3D();
    EndTextureMode();

    inReflectionPass = false;
    reflViewProj = MatrixMultiply(reflView, reflProj);
}

bool IsInReflectionPass() {
    return inReflectionPass;
}

Matrix GetReflectionViewProj() { return reflViewProj; }

RenderTexture2D GetReflectionTarget() { return reflectionTarget; }

int GetReflectionTextureSlot() { return REFLECTION_TEXTURE_SLOT; }

} // namespace gfx
