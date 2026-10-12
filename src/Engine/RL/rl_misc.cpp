// Camera/screen-space queries (from raylib's rcore.c), text module lifetime,
// and screenshot capture.
#include "rl_internal.hpp"
#include "rlgl.h"
#include "flyapp.h"

#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
// Defined in raylib's rtext.c (src/Engine/RL/raylib).
void LoadFontDefault(void);
void UnloadFontDefault(void);
}

namespace rli {

void TextInit() { LoadFontDefault(); }
void TextShutdown() { UnloadFontDefault(); }

} // namespace rli

using namespace rli;

static Matrix CameraProjMatrix(const Camera& camera, int width, int height) {
    const double aspect = (double)width / (double)(height > 0 ? height : 1);
    if (camera.projection == CAMERA_ORTHOGRAPHIC) {
        const double top = camera.fovy / 2.0;
        const double right = top * aspect;
        return MatrixOrtho(-right, right, -top, top, rlGetCullDistanceNear(), rlGetCullDistanceFar());
    }
    return MatrixPerspective(camera.fovy * DEG2RAD, aspect, rlGetCullDistanceNear(), rlGetCullDistanceFar());
}

Ray GetScreenToWorldRay(Vector2 position, Camera camera) {
    return GetScreenToWorldRayEx(position, camera, GetScreenWidth(), GetScreenHeight());
}

Ray GetScreenToWorldRayEx(Vector2 position, Camera camera, int width, int height) {
    Ray ray{};
    const float x = (2.0f * position.x) / (float)width - 1.0f;
    const float y = 1.0f - (2.0f * position.y) / (float)height;
    const Matrix matView = MatrixLookAt(camera.position, camera.target, camera.up);
    const Matrix matProj = CameraProjMatrix(camera, width, height);
    const Vector3 nearPoint = Vector3Unproject({ x, y, 0.0f }, matProj, matView);
    const Vector3 farPoint = Vector3Unproject({ x, y, 1.0f }, matProj, matView);
    const Vector3 cameraPlanePointerPos = Vector3Unproject({ x, y, -1.0f }, matProj, matView);
    ray.direction = Vector3Normalize(Vector3Subtract(farPoint, nearPoint));
    ray.position = camera.projection == CAMERA_ORTHOGRAPHIC ? cameraPlanePointerPos : camera.position;
    return ray;
}

Matrix GetCameraMatrix(Camera camera) {
    return MatrixLookAt(camera.position, camera.target, camera.up);
}

Vector2 GetWorldToScreen(Vector3 position, Camera camera) {
    return GetWorldToScreenEx(position, camera, GetScreenWidth(), GetScreenHeight());
}

Vector2 GetWorldToScreenEx(Vector3 position, Camera camera, int width, int height) {
    const Matrix matProj = CameraProjMatrix(camera, width, height);
    const Matrix matView = MatrixLookAt(camera.position, camera.target, camera.up);
    Quaternion worldPos = { position.x, position.y, position.z, 1.0f };
    worldPos = QuaternionTransform(worldPos, matView);
    worldPos = QuaternionTransform(worldPos, matProj);
    const Vector3 ndc = { worldPos.x / worldPos.w, -worldPos.y / worldPos.w, worldPos.z / worldPos.w };
    return { (ndc.x + 1.0f) / 2.0f * (float)width, (ndc.y + 1.0f) / 2.0f * (float)height };
}

// Screenshots read back the offscreen main target (see flyapp_read_image_rgba8).
// Readback is only possible between frames, so a request made mid-frame (or
// from another thread, like TechTools' async capture) is queued and served
// right after that frame is submitted.
namespace {
std::mutex g_shotMutex;
std::vector<std::string> g_pendingShots;
}

namespace rli {

bool CaptureMainTarget(const char* fileName) {
    GfxState& g = Gfx();
    RenderTargetRec& main = g.targets[g.mainTarget];
    TextureRec* tex = GetTexture(main.colorTex);
    if (!tex) return false;
    Image img{};
    img.width = main.width;
    img.height = main.height;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    img.data = RL_MALLOC((size_t)img.width * (size_t)img.height * 4);
    bool ok = flyapp_read_image_rgba8(tex->image.id, img.width, img.height, img.data);
    if (ok) {
        // Alpha in the main target is not meaningful; save it opaque.
        unsigned char* px = (unsigned char*)img.data;
        for (size_t i = 3; i < (size_t)img.width * (size_t)img.height * 4; i += 4) px[i] = 255;
        ok = ExportImage(img, fileName);
    }
    UnloadImage(img);
    if (ok) TraceLog(LOG_INFO, "SYSTEM: [%s] Screenshot taken successfully", fileName);
    else TraceLog(LOG_WARNING, "SYSTEM: [%s] Screenshot failed", fileName);
    return ok;
}

void ProcessPendingScreenshots() {
    std::vector<std::string> shots;
    {
        std::lock_guard<std::mutex> lock(g_shotMutex);
        shots.swap(g_pendingShots);
    }
    for (const std::string& f : shots) CaptureMainTarget(f.c_str());
}

} // namespace rli

void TakeScreenshot(const char* fileName) {
    if (!fileName || !*fileName) return;
    GfxState& g = Gfx();
    if (g.initialized && !g.frameActive && std::this_thread::get_id() == g.mainThread) {
        CaptureMainTarget(fileName);
        return;
    }
    std::lock_guard<std::mutex> lock(g_shotMutex);
    g_pendingShots.emplace_back(fileName);
}
