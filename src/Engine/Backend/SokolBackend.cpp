// ============================================================================
// gpu::IGraphicsBackend implementation for Sokol (Vulkan / Metal / D3D11 / GL).
//
// STATUS: scaffolding only. Nothing here talks to a GPU yet.
//
// This file exists now, rather than after the Vulkan work, for three reasons:
//
//   1. It proves the interface is implementable a second time, independently of
//      raylib. If only the raylib backend fits IGraphicsBackend, the abstraction
//      is shaped around raylib and the Sokol port will not fit through it.
//
//   2. It owns the backend selection. IGraphicsBackend::Create lives in
//      RaylibBackend.cpp (the reference backend); this file supplies
//      CreateSokolBackend, which that factory calls. Choosing a backend is
//      therefore one translation unit's concern and adding one does not require
//      editing the other.
//
//   3. It isolates the sokol dependency. Sokol headers are included here and
//      nowhere else in the engine, so everything above this line stays free of
//      them.
//
// Every method below returns a failure value (INVALID_HANDLE / nullptr / false)
// rather than a plausible-looking fake. A stub that returned 0 for a handle
// would look like "resource 0" and let broken call sites run for hours before
// anyone noticed; INVALID_HANDLE is unambiguously wrong, so a stubbed path fails
// loudly at the first use.
//
// The Vulkan/Metal escape hatches are deliberately NOT included here. They pull
// in <vulkan/vulkan.h> and VMA, which would make this file unbuildable on a
// machine without the Vulkan SDK. They are included by SokolEscapeHatches.cpp
// once the real backend lands.
// ============================================================================

#include "Engine/Backend/GraphicsBackend.hpp"
#include "Engine/Backend/Math.hpp"

// Sokol is implementation-private to this file, and is only included when a
// Sokol native API was selected in CMake. SokolBackend.cpp is always compiled
// (see below), so the include has to be conditional rather than unconditional.
#if defined(FLYENGINE_SOKOL_ENABLED)
#include "sokol_gfx.h"
#endif

#include <cstdio>
#include <memory>

namespace {

using gpu::BackendType;
using gpu::BufferDesc;
using gpu::BufferHandle;
using gpu::BufferUsage;
using gpu::CursorType;
using gpu::FramebufferDesc;
using gpu::FramebufferHandle;
using gpu::IGraphicsBackend;
using gpu::IndexType;
using gpu::KeyCode;
using gpu::Mat4;
using gpu::MeshData;
using gpu::MeshHandle;
// gpu::MouseButton is NOT imported unqualified. The build's precompiled header
// force-includes <raylib.h> into every translation unit, so `::MouseButton`
// (raylib's enum) is already visible here even though this file never includes
// raylib.h itself. A bare `using gpu::MouseButton;` therefore makes the name
// ambiguous, and the resulting error -- "marked 'override', but does not
// override" -- points at the interface rather than at the collision, which is
// why it was worth fixing at the source rather than papering over. The three gpu
// types that share a name with a raylib global are Color, Camera3D and
// MouseButton; those must always be spelled gpu::.
using gpu::PipelineDesc;
using gpu::PipelineHandle;
using gpu::RenderPassDesc;
using gpu::SamplerDesc;
using gpu::SamplerHandle;
using gpu::ShaderDesc;
using gpu::ShaderHandle;
using gpu::TextureData;
using gpu::TextureHandle;
using gpu::UniformType;
using gpu::Vec2;
using gpu::WindowDesc;
using gpu::WindowHandle;

// Which native API this instantiation targets. Sokol selects its backend at
// compile time (SOKOL_VULKAN / SOKOL_METAL / ...), so the requested
// BackendType can only be honoured if it matches that choice. One backend
// object per native API is therefore built at a time, not at runtime.
struct SokolState {
    BackendType native = BackendType::SokolVulkan;
    WindowHandle currentWindow = gpu::INVALID_HANDLE;
    WindowHandle nextWindow = 1;
    gpu::DeviceCaps caps;
};

SokolState g_state;

} // namespace

// ============================================================================
// SokolBackend
// ============================================================================

class SokolBackend final : public gpu::IGraphicsBackend {
public:
    explicit SokolBackend(BackendType native) { g_state.native = native; }

    static std::unique_ptr<IGraphicsBackend> Create(BackendType native) {
        return std::make_unique<SokolBackend>(native);
    }

    // ===== Window Management =====
    // Not yet implemented: creating a window needs sokol_app plus a platform
    // WSI layer, and sokol_app is deliberately not included yet (see the file
    // header). Reporting INVALID_HANDLE rather than a fake handle keeps callers
    // from proceeding as if a window existed.
    WindowHandle CreateWindow(const WindowDesc&) override { return gpu::INVALID_HANDLE; }

    void DestroyWindow(WindowHandle) override {}
    void SetWindowTitle(WindowHandle, const char*) override {}
    void SetWindowSize(WindowHandle, uint32_t, uint32_t) override {}
    bool ShouldClose(WindowHandle) override { return true; }
    void PollEvents() override {}
    void SetCurrentWindow(WindowHandle window) override { g_state.currentWindow = window; }
    WindowHandle GetCurrentWindow() override { return g_state.currentWindow; }
    uint32_t GetWindowWidth(WindowHandle) override { return 0; }
    uint32_t GetWindowHeight(WindowHandle) override { return 0; }

    // ===== Frame Lifecycle =====
    void BeginFrame(WindowHandle) override {}
    void EndFrame(WindowHandle) override {}
    void WaitIdle() override {}

    // ===== Input =====
    bool IsKeyDown(WindowHandle, KeyCode) override { return false; }
    bool IsKeyPressed(WindowHandle, KeyCode) override { return false; }
    bool IsKeyReleased(WindowHandle, KeyCode) override { return false; }
    bool IsMouseButtonDown(WindowHandle, gpu::MouseButton) override { return false; }
    Vec2 GetMousePosition(WindowHandle) override { return {0.0f, 0.0f}; }
    float GetMouseWheelDelta(WindowHandle) override { return 0.0f; }
    void SetMouseCursor(WindowHandle, CursorType) override {}
    void SetMouseLocked(WindowHandle, bool) override {}
    bool IsMouseLocked(WindowHandle) override { return false; }

    // ===== Time =====
    // Sokol tracks frame time itself; these are placeholders until sokol_app is
    // wired up. Reporting 0 would make GetFrameTime() divide by zero in any
    // caller that integrates with it, so a nominal 60 Hz is returned instead.
    double GetTime() override { return 0.0; }
    float GetFrameTime() override { return 1.0f / 60.0f; }

    // ===== Shader System =====
    ShaderHandle CreateShader(const ShaderDesc&) override { return gpu::INVALID_HANDLE; }
    void DestroyShader(ShaderHandle) override {}
    int32_t GetUniformLocation(ShaderHandle, const char*) override { return -1; }
    int32_t GetUniformBlockIndex(ShaderHandle, const char*) override { return -1; }
    void SetUniform(ShaderHandle, int32_t, const void*, UniformType, uint32_t) override {}
    void SetUniformBlock(ShaderHandle, uint32_t, BufferHandle, uint32_t, uint32_t) override {}

    // ===== Pipelines =====
    PipelineHandle CreatePipeline(const PipelineDesc&) override { return gpu::INVALID_HANDLE; }
    void DestroyPipeline(PipelineHandle) override {}

    // ===== Buffers =====
    BufferHandle CreateBuffer(const BufferDesc&) override { return gpu::INVALID_HANDLE; }
    void DestroyBuffer(BufferHandle) override {}
    void UpdateBuffer(BufferHandle, const void*, uint32_t, uint32_t) override {}
    void* MapBuffer(BufferHandle) override { return nullptr; }
    void UnmapBuffer(BufferHandle) override {}

    // ===== Meshes =====
    MeshHandle CreateMesh(const MeshData&) override { return gpu::INVALID_HANDLE; }
    void DestroyMesh(MeshHandle) override {}
    void DrawMesh(MeshHandle, PipelineHandle, const Mat4&, uint32_t) override {}
    void DrawMeshIndexed(MeshHandle, PipelineHandle, uint32_t, uint32_t, const Mat4&) override {}

    // ===== Instanced Drawing =====
    BufferHandle CreateInstanceBuffer(const Mat4*, uint32_t, BufferUsage) override {
        return gpu::INVALID_HANDLE;
    }
    void UpdateInstanceBuffer(BufferHandle, const Mat4*, uint32_t) override {}
    void DrawMeshInstanced(MeshHandle, PipelineHandle, BufferHandle, uint32_t) override {}

    // ===== Textures =====
    TextureHandle CreateTexture(const TextureData&) override { return gpu::INVALID_HANDLE; }
    TextureHandle CreateTextureFromFile(const char*) override { return gpu::INVALID_HANDLE; }
    void DestroyTexture(TextureHandle) override {}
    void UpdateTexture(TextureHandle, const void*, uint32_t, uint32_t, uint32_t) override {}
    void GenerateMipmaps(TextureHandle) override {}

    // ===== Samplers =====
    SamplerHandle CreateSampler(const SamplerDesc&) override { return gpu::INVALID_HANDLE; }
    void DestroySampler(SamplerHandle) override {}

    // ===== Framebuffers / Render Passes =====
    FramebufferHandle CreateFramebuffer(const FramebufferDesc&) override {
        return gpu::INVALID_HANDLE;
    }
    void DestroyFramebuffer(FramebufferHandle) override {}
    void BeginRenderPass(FramebufferHandle, const RenderPassDesc&) override {}
    void EndRenderPass() override {}
    void BeginSwapchainPass(WindowHandle, const RenderPassDesc&) override {}
    void EndSwapchainPass() override {}

    // ===== Resource Binding =====
    void BindPipeline(PipelineHandle) override {}
    void BindVertexBuffer(BufferHandle, uint32_t) override {}
    void BindIndexBuffer(BufferHandle, IndexType) override {}
    void BindTexture(uint32_t, TextureHandle, SamplerHandle) override {}
    void BindUniformBuffer(uint32_t, BufferHandle, uint32_t, uint32_t) override {}

    // ===== Draw Calls =====
    void Draw(uint32_t, uint32_t, uint32_t) override {}
    void DrawIndexed(uint32_t, uint32_t, uint32_t) override {}
    void DispatchCompute(uint32_t, uint32_t, uint32_t) override {}

    // ===== Scissor / Viewport =====
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void SetScissor(uint32_t, uint32_t, uint32_t, uint32_t) override {}

    // ===== Debug / ImGui =====
    // sokol_imgui.h is the bridge used here once rendering exists; it is not
    // included yet so this file stays free of ImGui's headers.
    void InitDebugUI(WindowHandle) override {}
    void ShutdownDebugUI() override {}
    void BeginDebugUIFrame(WindowHandle) override {}
    void EndDebugUIFrame() override {}

    // ===== Escape Hatches =====
    // Both stay null until SokolEscapeHatches.cpp implements them. The base
    // class already defaults to nullptr, so these overrides are omitted rather
    // than restated.

    // ===== Capabilities =====
    const gpu::DeviceCaps& GetCaps() const override { return g_state.caps; }
};

// ============================================================================
// Factory entry point, called by IGraphicsBackend::Create
// ============================================================================

std::unique_ptr<gpu::IGraphicsBackend> CreateSokolBackend(gpu::BackendType type) {
    if (type != BackendType::SokolVulkan && type != BackendType::SokolMetal &&
        type != BackendType::SokolD3D11 && type != BackendType::SokolGL) {
        return nullptr;
    }

#if !defined(FLYENGINE_SOKOL_ENABLED)
    // No Sokol native API was selected at configure time. This is the default
    // build, and asking for a Sokol backend here must fail rather than return a
    // stub that reports success -- IGraphicsBackend::Create returning nullptr is
    // what lets a caller detect the misconfiguration and fall back to raylib.
    std::fprintf(stderr,
                 "flyengine: a Sokol backend was requested but none was built in.\n"
                 "             Reconfigure with -DFLYENGINE_BACKEND_SOKOL_VULKAN=ON\n"
                 "             (or _METAL / _D3D11 / _GL) to enable it.\n");
    return nullptr;
#else
    // Sokol binds to its native API at compile time, so a request for an API
    // this binary was not compiled for has to fail loudly instead of quietly
    // returning, say, a Vulkan backend for a Metal request. sokol_gfx.h selects
    // the API from these macros, so reading them back here is exact.
#if defined(SOKOL_VULKAN)
    constexpr BackendType kCompiledAs = BackendType::SokolVulkan;
#elif defined(SOKOL_METAL)
    constexpr BackendType kCompiledAs = BackendType::SokolMetal;
#elif defined(SOKOL_D3D11)
    constexpr BackendType kCompiledAs = BackendType::SokolD3D11;
#elif defined(SOKOL_GLCORE)
    constexpr BackendType kCompiledAs = BackendType::SokolGL;
#else
#error "FLYENGINE_SOKOL_ENABLED requires exactly one of SOKOL_VULKAN/SOKOL_METAL/SOKOL_D3D11/SOKOL_GLCORE"
#endif

    if (type != kCompiledAs) {
        std::fprintf(stderr,
                     "flyengine: this binary's sokol backend is not the requested native API\n");
        return nullptr;
    }

    return SokolBackend::Create(type);
#endif
}