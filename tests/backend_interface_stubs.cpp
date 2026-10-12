// Stubbed raylib / rlgl / ImGui entry points for tests/backend_interface_test.cpp.
//
// WHY THIS EXISTS
// ---------------
// The interface tests drive gpu::IGraphicsBackend::Create and then poke at the
// real RaylibBackend implementation, so the code under test is genuine. What
// they cannot do is open a window: there is no display, and InitWindow needs
// one. So the raylib layer underneath is replaced with definitions that satisfy
// the linker and do nothing.
//
// This is only sound because the tests never assert on rendered output. They
// assert on the parts of the contract that live entirely inside
// RaylibBackend.cpp: factory routing, rejection of invalid input, no-op
// behaviour on stale handles, and the numeric key/button mappings. None of that
// depends on a real GL context. A pixel-comparison test would need a real
// library and a real device, and would belong in a different target.
//
// The signatures below are copied from raylib.h, rlgl.h and imgui.h rather than
// hand-written, so a change in the real headers shows up as a compile error here
// instead of as a silent ABI mismatch.
//
// LINKAGE NOTE
// ------------
// raylib and rlgl are `extern "C"`. imgui_impl_raylib.h is NOT: it has no
// extern "C" guard, so its four entry points are C++-mangled and must be defined
// as C++ here. Getting that wrong produces undefined references whose names
// look like the right ones, which is a confusing way to fail.

#include "raylib.h"
#include "rlgl.h"
#include "imgui.h"
#include "imgui_impl_raylib.h"

#include <cstring>

namespace {

// Handles the stubs hand out. Distinct values per resource kind so a test can
// tell a texture id from a buffer id, which is what the real GL does.
constexpr unsigned int kStubMeshId = 0x1001;
constexpr unsigned int kStubVboId = 0x1002;
constexpr unsigned int kStubEboId = 0x1003;
constexpr unsigned int kStubTextureId = 0x1004;
constexpr unsigned int kStubFramebufferId = 0x1005;
constexpr unsigned int kStubShaderId = 0x1006;

} // namespace

// ============================================================================
// Window, timing, input
// ============================================================================

extern "C" {

void SetConfigFlags(unsigned int) {}
void SetTargetFPS(int) {}
void InitWindow(int, int, const char*) {}
void CloseWindow(void) {}
void SetWindowTitle(const char*) {}
void SetWindowSize(int, int) {}
bool WindowShouldClose(void) { return false; }

int GetScreenWidth(void) { return 1920; }
int GetScreenHeight(void) { return 1080; }

void BeginDrawing(void) {}
void EndDrawing(void) {}
void ClearBackground(Color) {}

double GetTime(void) { return 0.0; }
float GetFrameTime(void) { return 1.0f / 60.0f; }

bool IsKeyDown(int) { return false; }
bool IsKeyPressed(int) { return false; }
bool IsKeyReleased(int) { return false; }
bool IsMouseButtonDown(int) { return false; }

Vector2 GetMousePosition(void) { return {0.0f, 0.0f}; }
float GetMouseWheelMove(void) { return 0.0f; }
void SetMouseCursor(int) {}
void DisableCursor(void) {}
void EnableCursor(void) {}
bool IsCursorHidden(void) { return false; }

void BeginTextureMode(RenderTexture2D) {}
void EndTextureMode(void) {}

// ============================================================================
// Shaders and textures
// ============================================================================

Shader LoadShaderFromMemory(const char*, const char*) {
    // A non-zero id, so the backend's `shader.id == 0` failure check is actually
    // being exercised rather than short-circuiting on a null result.
    return {kStubShaderId, nullptr};
}

void UnloadShader(Shader) {}
int GetShaderLocation(Shader, const char*) { return -1; }  // "not found"
void SetShaderValue(Shader, int, const void*, int) {}
void SetShaderValueV(Shader, int, const void*, int, int) {}
void SetShaderValueMatrix(Shader, int, Matrix) {}

Texture2D LoadTexture(const char*) { return {kStubTextureId, 0, 0, 0, 0}; }
Texture2D LoadTextureFromImage(Image) { return {kStubTextureId, 0, 0, 0, 0}; }
void UnloadTexture(Texture2D) {}
void UpdateTexture(Texture2D, const void*) {}
void GenTextureMipmaps(Texture2D*) {}

RenderTexture2D LoadRenderTexture(int, int) {
    return {kStubFramebufferId, {}, {}};
}
void UnloadRenderTexture(RenderTexture2D) {}

// ============================================================================
// rlgl: buffers, vertex arrays, draw calls
// ============================================================================

unsigned int rlLoadVertexArray(void) { return kStubMeshId; }
void rlUnloadVertexArray(unsigned int) {}

unsigned int rlLoadVertexBuffer(const void*, int, bool) { return kStubVboId; }
unsigned int rlLoadVertexBufferElement(const void*, int, bool) { return kStubEboId; }
void rlUnloadVertexBuffer(unsigned int) {}

void rlUpdateVertexBuffer(unsigned int, const void*, int, int) {}
void rlUpdateVertexBufferElements(unsigned int, const void*, int, int) {}

bool rlEnableVertexArray(unsigned int) { return true; }
void rlDisableVertexArray(void) {}
void rlEnableVertexBuffer(unsigned int) {}
void rlEnableVertexBufferElement(unsigned int) {}
void rlDisableVertexBuffer(void) {}

void rlEnableVertexAttribute(unsigned int) {}
void rlDisableVertexAttribute(unsigned int) {}
void rlSetVertexAttribute(unsigned int, int, int, bool, int, int) {}
void rlSetVertexAttributeDivisor(unsigned int, int) {}

void rlDrawVertexArray(int, int) {}
void rlDrawVertexArrayElements(int, int, const void*) {}
void rlDrawVertexArrayInstanced(int, int, int) {}
void rlDrawVertexArrayElementsInstanced(int, int, const void*, int) {}

void rlEnableShader(unsigned int) {}
void rlSetMatrixModelview(Matrix) {}

void rlActiveTextureSlot(int) {}
void rlEnableTexture(unsigned int) {}

void rlViewport(int, int, int, int) {}
void rlEnableScissorTest(void) {}
void rlScissor(int, int, int, int) {}

} // extern "C"

// ============================================================================
// ImGui
//
// imgui_impl_raylib.h declares these without an extern "C" guard, so they must
// be C++ definitions (see the linkage note at the top of this file). imgui.h
// declares ImGui::Render and ImGui::GetDrawData; those are defined here too
// rather than pulling in the whole ImGui library, since the test asserts nothing
// about ImGui behaviour. imgui.h's own declarations keep these definitions
// honest -- a signature change upstream becomes a compile error below.
// ============================================================================

bool ImGui_ImplRaylib_Init(void) { return true; }
void ImGui_ImplRaylib_Shutdown(void) {}
void ImGui_ImplRaylib_NewFrame(void) {}
void ImGui_ImplRaylib_RenderDrawData(ImDrawData*) {}

namespace ImGui {
void Render(void) {}
ImDrawData* GetDrawData(void) { return nullptr; }
} // namespace ImGui
