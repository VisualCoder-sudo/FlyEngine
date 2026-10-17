#include "../../include/Engine/Graphics.hpp"
#include "../../include/Engine/Lighting.hpp"
#include "../../include/Engine/PostFX.hpp"
#include "../../include/Engine/Frontend/ui.hpp"
#include "ShadowsInternal.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <atomic>
#include <cmath>
#include <cstring>

namespace {

using gfx::shadow::kMaxCascades;

int shadowMapResolution = 2048;
int shadowQuality = 52;

int QualityToResolution(int quality) {
    // Shadow acne fix (3): raise the texel budget for a given quality slider.
    // Base shifted 8 -> 9 (log2), so default 52 now yields 2048 (4x texels of
    // the old 1024) - the generated shadow map is sharper and each acne stripe
    // is a few texels narrower, cutting the visible flicker. Low 512 barely
    // differs; High reaches 8192 for crisp silhouettes from up high.
    quality = Clamp(quality, 5, 100);
    float t = (float)(quality - 5) / 95.0f;
    int log2 = 9 + (int)std::lroundf(t * 4.0f);
    return 1 << log2;
}

bool inShadowPass = false;
// Shadow-map reuse state.
bool shadowReuseEnabled = true;
std::atomic<bool> shadowsDirty{ true };
bool shadowPassReused = false;
float shadowStableHalf = 0.0f; // hysteresis-held shadow box half-size (m)
bool shadowHaveRendered = false;
Vector3 shadowLastCenter{};
float shadowLastHalf = 0.0f;
int shadowLastRes = 0;
int shadowFramesSinceRender = 0;
int shadowInputHold = 0;
constexpr int kShadowMaxAge = 90;   // force a refresh at least this often (frames)
constexpr int kShadowInputHold = 3; // keep re-rendering this many frames after input
// The depth map is only reused after the view and the scene have been still this long. Reuse refreshes
// every kShadowMaxAge frames, which looks like shadows updating once a second (cars, pedestrians), so
// it must not start the moment the camera stops.
constexpr double kShadowStillSeconds = 30.0;
double shadowStillSince = 0.0;

// Any input that could edit the scene (or move the camera) this frame.
bool SceneInputActivity() {
    for (int b = 0; b < 7; b++)
        if (IsMouseButtonDown(b) || IsMouseButtonPressed(b) || IsMouseButtonReleased(b)) return true;
    if (GetMouseWheelMove() != 0.0f) return true;
    for (int k = 32; k <= 348; k++)
        if (IsKeyDown(k) || IsKeyPressed(k) || IsKeyReleased(k)) return true;
    return false;
}

Camera3D shadowViewCamera{};   // last camera seen by UpdateLighting (the shadow frustum follows it)
bool haveShadowViewCamera = false;
constexpr float kShadowNear = 1.0f;
constexpr float kShadowFar = 430.0f;
constexpr float kShadowLightDist = 330.0f; // light eye distance from the frustum centre
constexpr float kShadowMinHalf = 40.0f;
constexpr float kShadowMaxHalf = 150.0f;

bool shadowsEnabled = true;

RenderTexture2D shadowMap{};
// The further shadow cascades: boxes of ground round the camera, each about three times as wide as
// the one before, drawn into maps of their own. They need not be fresh every frame (what they show
// is far away), so one of them is redrawn per frame, the further the less often.
constexpr float kCascadeGrowth = 2.7f;
constexpr int kFarCascadeMaxRes = 2048;
struct FarCascade {
    RenderTexture2D map{};
    Matrix viewProj = MatrixIdentity();
    bool valid = false;
    int res = 0;
    float half = 0.0f;
    Vector3 sun{};
};
FarCascade farCascades[kMaxCascades - 1];
int activeFarCascade = 0;       // the far cascade being drawn (1..3), 0 = none
unsigned shadowFrame = 0;
bool nearCascadeReused = false; // the first cascade kept its map this frame (the scene is idle)
Camera3D lightCamera{};
Matrix lightView = MatrixIdentity();
Matrix lightProj = MatrixIdentity();
Matrix lightViewProj = MatrixIdentity();

}

namespace gfx {

void SetShadowsEnabled(bool enabled) { shadowsEnabled = enabled; shadowsDirty = true; }

bool IsShadowsEnabled() { return shadowsEnabled; }

void SetShadowQuality(int quality) {
    quality = Clamp(quality, 5, 100);
    if (quality == shadowQuality && shadowMap.id > 0) return;
    shadowQuality = quality;

    int resolution = QualityToResolution(quality);
    if (resolution == shadowMapResolution && shadowMap.id > 0) return;
    shadowMapResolution = resolution;
    shadowsDirty = true;

    if (shadowMap.id > 0) {
        UnloadRenderTexture(shadowMap);
        shadowMap = { 0 };
    }
    shadowMap = LoadRenderTextureDepth(shadowMapResolution, shadowMapResolution);
}

int GetShadowQuality() { return shadowQuality; }

void BeginShadowPass() {
    if (!shadowsEnabled || shadowMap.id == 0) return;

    inShadowPass = true;

    // The shadow frustum follows the viewer, but it must be STABLE while the
    // viewer only rotates: centring it on where the view ray hits the ground made
    // the box jump by hundreds of metres (and rescale) with tiny pitch changes at
    // street level, which showed up as shadows popping and glitching inside the
    // city. So:
    //   * size depends on camera HEIGHT only, with hysteresis (no per-frame rescale);
    //   * the centre sits ahead of the camera along its HORIZONTAL look direction
    //     (pitch-independent), blending to the ground-hit point only when the
    //     camera looks steeply down (top-down editing).
    Vector3 focus = { 0.0f, 0.0f, 0.0f };
    float half = shadowStableHalf > 0.0f ? shadowStableHalf : kShadowMinHalf;
    if (haveShadowViewCamera) {
        const Camera3D& cam = shadowViewCamera;
        const Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
        const float camH = fmaxf(cam.position.y, 0.0f);

        const float targetHalf = fminf(fmaxf(64.0f + 0.6f * camH, kShadowMinHalf), kShadowMaxHalf);
        if (shadowStableHalf <= 0.0f || fabsf(targetHalf - shadowStableHalf) > 12.0f)
            shadowStableHalf = ceilf(targetHalf / 8.0f) * 8.0f;
        half = shadowStableHalf;

        Vector3 fh = { fwd.x, 0.0f, fwd.z };
        const float fl = Vector3Length(fh);
        fh = fl > 1e-3f ? Vector3Scale(fh, 1.0f / fl) : Vector3{ 0.0f, 0.0f, -1.0f };
        const Vector3 ahead = { cam.position.x + fh.x * half * 0.35f, 0.0f,
                                cam.position.z + fh.z * half * 0.35f };
        Vector3 hit = ahead;
        if (cam.position.y > 0.0f && fwd.y < -0.05f) {
            const float tHit = fminf(-cam.position.y / fwd.y, 800.0f);
            hit = Vector3Add(cam.position, Vector3Scale(fwd, tHit));
            hit.y = 0.0f;
        }
        const float w = fminf(fmaxf((-fwd.y - 0.5f) / 0.3f, 0.0f), 1.0f);
        focus = Vector3Lerp(ahead, hit, w);
    }

    // Snap the frustum centre to whole shadow texels in light space (no shimmering
    // while the camera moves). Basis matches raylib's lookAt for up = (0,1,0).
    const Vector3 sunDir = SunDirection();
    {   // a moved sun changes every shadow
        static Vector3 lastSun{};
        if (!Vector3Equals(lastSun, sunDir)) { lastSun = sunDir; shadowsDirty = true; }
    }
    const Vector3 lightRight = Vector3Normalize(Vector3CrossProduct(sunDir, { 0.0f, 1.0f, 0.0f }));
    const Vector3 lightUp = Vector3CrossProduct(lightRight, sunDir);
    const float texel = (2.0f * half) / (float)shadowMapResolution;
    const float cx = Vector3DotProduct(focus, lightRight);
    const float cy = Vector3DotProduct(focus, lightUp);
    const float sx = floorf(cx / texel) * texel;
    const float sy = floorf(cy / texel) * texel;
    focus = Vector3Add(focus, Vector3Add(Vector3Scale(lightRight, sx - cx), Vector3Scale(lightUp, sy - cy)));

    lightCamera.target = focus;
    lightCamera.position = Vector3Subtract(focus, Vector3Scale(sunDir, kShadowLightDist));
    lightCamera.up = { 0.0f, 1.0f, 0.0f };
    lightCamera.projection = CAMERA_ORTHOGRAPHIC;

    // Reuse the previous depth map when nothing that could change it happened.
    if (SceneInputActivity()) shadowInputHold = kShadowInputHold;
    const bool frustumSame = shadowHaveRendered && shadowLastRes == shadowMapResolution &&
                             shadowLastHalf == half && Vector3Equals(shadowLastCenter, focus);
    const double now = GetTime();
    if (shadowInputHold > 0 || !frustumSame || shadowsDirty.load()) shadowStillSince = now;
    shadowPassReused = shadowReuseEnabled && frustumSame && !shadowsDirty.load() &&
                       shadowInputHold == 0 && shadowFramesSinceRender < kShadowMaxAge &&
                       now - shadowStillSince >= kShadowStillSeconds &&
                       !ui::IsPlayActive();
    if (shadowInputHold > 0) shadowInputHold--;
    nearCascadeReused = shadowPassReused;

    BeginTextureMode(shadowMap);
    if (shadowPassReused) {
        // Keep the depth map as is: entities still "draw" between Begin/EndShadowPass,
        // but an empty scissor rect discards every fragment.
        rlEnableScissorTest();
        rlScissor(0, 0, 0, 0);
        shadowFramesSinceRender++;
    } else {
        ClearBackground(WHITE);
        shadowsDirty = false;
        shadowHaveRendered = true;
        shadowLastCenter = focus;
        shadowLastHalf = half;
        shadowLastRes = shadowMapResolution;
        shadowFramesSinceRender = 0;
    }

    BeginMode3D(lightCamera);
    rlSetMatrixProjection(MatrixOrtho(-half, half, -half, half, kShadowNear, kShadowFar));

    lightView = rlGetMatrixModelview();
    lightProj = rlGetMatrixProjection();
}

void EndShadowPass() {
    if (!shadowsEnabled || shadowMap.id == 0) return;

    EndMode3D();
    EndTextureMode();
    if (shadowPassReused) rlDisableScissorTest();
    shadowPassReused = false;

    inShadowPass = false;
    lightViewProj = MatrixMultiply(lightView, lightProj);

    // The frustum moves every frame, so hand the fresh matrix to the receiving
    // shaders now instead of relying on UpdateLighting() running after this pass.
    detail::UploadLightViewProj(lightViewProj);
}

bool IsInShadowPass() {
    return inShadowPass;
}

int ShadowCascadeCount() {
    return (shadowsEnabled && shadowMap.id != 0) ? Clamp(Quality().shadowCascades, 1, kMaxCascades) : 1;
}

bool BeginShadowCascade(int index) {
    if (!shadowsEnabled || shadowMap.id == 0) return false;
    if (index <= 0) {
        shadowFrame++;
        BeginShadowPass();
        return true;
    }
    if (index >= kMaxCascades || !haveShadowViewCamera) return false;

    FarCascade& fc = farCascades[index - 1];
    const int res = shadowMapResolution < kFarCascadeMaxRes ? shadowMapResolution : kFarCascadeMaxRes;
    const float half = (shadowStableHalf > 0.0f ? shadowStableHalf : kShadowMinHalf) * powf(kCascadeGrowth, (float)index);
    const Vector3 sunDir = SunDirection();
    const bool stale = !fc.valid || fc.res != res || fc.half != half || !Vector3Equals(fc.sun, sunDir);
    if (!stale) {
        // Cascade 1 on every 2nd frame, 2 on every 4th, 3 on every 8th, never two in one frame.
        const unsigned period = 1u << index;
        if (nearCascadeReused || (shadowFrame % period) != period / 2) return false;
    }
    if (fc.map.id == 0 || fc.res != res) {
        if (fc.map.id > 0) UnloadRenderTexture(fc.map);
        fc.map = LoadRenderTextureDepth(res, res);
        fc.res = res;
    }

    // Centred a little ahead of the camera on the ground, in whole texels (as the first cascade is).
    const Camera3D& cam = shadowViewCamera;
    Vector3 fh = { cam.target.x - cam.position.x, 0.0f, cam.target.z - cam.position.z };
    const float fl = Vector3Length(fh);
    fh = fl > 1e-3f ? Vector3Scale(fh, 1.0f / fl) : Vector3{ 0.0f, 0.0f, -1.0f };
    Vector3 focus = { cam.position.x + fh.x * half * 0.35f, 0.0f, cam.position.z + fh.z * half * 0.35f };
    const Vector3 lightRight = Vector3Normalize(Vector3CrossProduct(sunDir, { 0.0f, 1.0f, 0.0f }));
    const Vector3 lightUp = Vector3CrossProduct(lightRight, sunDir);
    const float texel = (2.0f * half) / (float)res;
    const float cx = Vector3DotProduct(focus, lightRight), cy = Vector3DotProduct(focus, lightUp);
    focus = Vector3Add(focus, Vector3Add(Vector3Scale(lightRight, floorf(cx / texel) * texel - cx),
                                         Vector3Scale(lightUp, floorf(cy / texel) * texel - cy)));

    // A wider box sees further along the light as well: its depth range grows with it.
    const float k = half / 64.0f;
    Camera3D cascadeCamera = lightCamera;
    cascadeCamera.target = focus;
    cascadeCamera.position = Vector3Subtract(focus, Vector3Scale(sunDir, kShadowLightDist * k));
    cascadeCamera.up = { 0.0f, 1.0f, 0.0f };
    cascadeCamera.projection = CAMERA_ORTHOGRAPHIC;

    BeginTextureMode(fc.map);
    ClearBackground(WHITE);
    BeginMode3D(cascadeCamera);
    rlSetMatrixProjection(MatrixOrtho(-half, half, -half, half, kShadowNear * k, kShadowFar * k));
    fc.viewProj = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    fc.half = half;
    fc.sun = sunDir;
    fc.valid = true;
    inShadowPass = true;
    activeFarCascade = index;
    return true;
}

void EndShadowCascade() {
    if (activeFarCascade == 0) { EndShadowPass(); return; }
    EndMode3D();
    EndTextureMode();
    inShadowPass = false;
    activeFarCascade = 0;
}

Texture2D GetShadowMapTexture() { return (shadowsEnabled && shadowHaveRendered) ? shadowMap.depth : Texture2D{}; }
Matrix GetLightViewProj() { return lightViewProj; }
float GetShadowRange() { return (shadowStableHalf > 0.0f ? shadowStableHalf : kShadowMinHalf) * 1.3f; }

void SetShadowReuseEnabled(bool enabled) { shadowReuseEnabled = enabled; shadowsDirty = true; }
bool GetShadowReuseEnabled() { return shadowReuseEnabled; }
void MarkShadowsDirty() { shadowsDirty = true; }
bool IsShadowPassReused() { return inShadowPass && shadowPassReused; }

namespace shadow {

void Init() {
    // Depth-only target: the lit shaders sample it through a comparison
    // sampler (hardware PCF), so no color attachment is needed.
    shadowMap = LoadRenderTextureDepth(shadowMapResolution, shadowMapResolution);

    lightCamera.position = Vector3Scale(SunDirection(), -40.0f);
    lightCamera.target = { 0.0f, 0.0f, 0.0f };
    lightCamera.up = { 0.0f, 1.0f, 0.0f };
    lightCamera.fovy = 45.0f;
    lightCamera.projection = CAMERA_ORTHOGRAPHIC;
}

void Shutdown() {
    if (shadowMap.id > 0) UnloadRenderTexture(shadowMap);
    for (FarCascade& fc : farCascades) {
        if (fc.map.id > 0) UnloadRenderTexture(fc.map);
        fc = FarCascade{};
    }
}

void SetViewCamera(const Camera3D& camera) {
    shadowViewCamera = camera;
    haveShadowViewCamera = true;
}

void BindMap() {
    if (shadowsEnabled && shadowMap.depth.id > 0) {
        rlActiveTextureSlot(kTextureSlot);
        rlEnableTexture(shadowMap.depth.id);
    }
}

Texture2D FarMapDepth(int farIndex) { return farCascades[farIndex].map.depth; }

// The cascades for the lit shaders: their matrices (column-major, one after another) and how many
// there are (every cascade up to the first that has no map yet).
int CascadeData(float out[16 * kMaxCascades]) {
    const Matrix id = MatrixIdentity();
    int count = 0;
    for (int i = 0; i < kMaxCascades; i++) {
        const bool have = shadowsEnabled && count == i && i < ShadowCascadeCount() && (i == 0 || farCascades[i - 1].valid);
        const float16 f = MatrixToFloatV(have ? (i == 0 ? lightViewProj : farCascades[i - 1].viewProj) : id);
        memcpy(out + 16 * i, f.v, sizeof(float) * 16);
        if (have) count++;
    }
    return count;
}

} // namespace shadow

} // namespace gfx
