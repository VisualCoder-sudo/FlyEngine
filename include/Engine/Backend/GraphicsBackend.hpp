#pragma once

#include "GraphicsBackendTypes.hpp"
#include "Math.hpp"
#include <memory>
#include <vector>
#include <functional>
#include <string>

// Forward declare escape hatches. Kept out of this header's includes so
// IGraphicsBackend stays free of <vulkan/vulkan.h> and <Metal/Metal.h>.
namespace backend {
    struct IVulkanExtensions;
    struct IMetalExtensions;
}

namespace gpu {

class IGraphicsBackend {
public:
    virtual ~IGraphicsBackend() = default;

    // ===== Factory =====
    static std::unique_ptr<IGraphicsBackend> Create(BackendType type = BackendType::Auto);

    // ===== Window Management (Multi-window) =====
    virtual WindowHandle CreateWindow(const WindowDesc& desc) = 0;
    virtual void DestroyWindow(WindowHandle window) = 0;
    virtual void SetWindowTitle(WindowHandle window, const char* title) = 0;
    virtual void SetWindowSize(WindowHandle window, uint32_t width, uint32_t height) = 0;
    virtual bool ShouldClose(WindowHandle window) = 0;
    virtual void PollEvents() = 0;
    
    // Current window context (for single-window code)
    virtual void SetCurrentWindow(WindowHandle window) = 0;
    virtual WindowHandle GetCurrentWindow() = 0;
    virtual uint32_t GetWindowWidth(WindowHandle window) = 0;
    virtual uint32_t GetWindowHeight(WindowHandle window) = 0;

    // ===== Frame Lifecycle =====
    virtual void BeginFrame(WindowHandle window) = 0;
    virtual void EndFrame(WindowHandle window) = 0;
    virtual void WaitIdle() = 0;

    // ===== Input (Generic key/mouse codes) =====
    virtual bool IsKeyDown(WindowHandle window, KeyCode key) = 0;
    virtual bool IsKeyPressed(WindowHandle window, KeyCode key) = 0;
    virtual bool IsKeyReleased(WindowHandle window, KeyCode key) = 0;
    virtual bool IsMouseButtonDown(WindowHandle window, MouseButton button) = 0;
    virtual Vec2 GetMousePosition(WindowHandle window) = 0;
    virtual float GetMouseWheelDelta(WindowHandle window) = 0;
    virtual void SetMouseCursor(WindowHandle window, CursorType cursor) = 0;
    virtual void SetMouseLocked(WindowHandle window, bool locked) = 0;
    virtual bool IsMouseLocked(WindowHandle window) = 0;

    // ===== Time =====
    virtual double GetTime() = 0;
    virtual float GetFrameTime() = 0;

    // ===== Shader System =====
    virtual ShaderHandle CreateShader(const ShaderDesc& desc) = 0;
    virtual void DestroyShader(ShaderHandle handle) = 0;
    virtual int32_t GetUniformLocation(ShaderHandle shader, const char* name) = 0;
    virtual int32_t GetUniformBlockIndex(ShaderHandle shader, const char* name) = 0;
    
    // Uniform updates
    virtual void SetUniform(ShaderHandle shader, int32_t location, const void* data, UniformType type, uint32_t count = 1) = 0;
    virtual void SetUniformBlock(ShaderHandle shader, uint32_t binding, BufferHandle buffer, uint32_t offset, uint32_t size) = 0;

    // ===== Pipeline State Objects =====
    virtual PipelineHandle CreatePipeline(const PipelineDesc& desc) = 0;
    virtual void DestroyPipeline(PipelineHandle handle) = 0;

    // ===== Buffers =====
    virtual BufferHandle CreateBuffer(const BufferDesc& desc) = 0;
    virtual void DestroyBuffer(BufferHandle handle) = 0;
    virtual void UpdateBuffer(BufferHandle handle, const void* data, uint32_t size, uint32_t offset = 0) = 0;
    virtual void* MapBuffer(BufferHandle handle) = 0;  // For persistent mapping
    virtual void UnmapBuffer(BufferHandle handle) = 0;

    // ===== Meshes (Vertex/Index buffers + layout) =====
    virtual MeshHandle CreateMesh(const MeshData& data) = 0;
    virtual void DestroyMesh(MeshHandle handle) = 0;
    virtual void DrawMesh(MeshHandle mesh, PipelineHandle pipeline, const Mat4& transform, uint32_t instanceCount = 1) = 0;
    virtual void DrawMeshIndexed(MeshHandle mesh, PipelineHandle pipeline, uint32_t indexCount, uint32_t indexOffset, const Mat4& transform) = 0;

    // ===== Instanced Drawing (For city buildings) =====
    virtual BufferHandle CreateInstanceBuffer(const Mat4* transforms, uint32_t count, BufferUsage usage) = 0;
    virtual void UpdateInstanceBuffer(BufferHandle handle, const Mat4* transforms, uint32_t count) = 0;
    virtual void DrawMeshInstanced(MeshHandle mesh, PipelineHandle pipeline, BufferHandle instanceBuffer, uint32_t count) = 0;

    // ===== Textures =====
    virtual TextureHandle CreateTexture(const TextureData& data) = 0;
    virtual TextureHandle CreateTextureFromFile(const char* path) = 0;  // Uses asset pipeline
    virtual void DestroyTexture(TextureHandle handle) = 0;
    virtual void UpdateTexture(TextureHandle handle, const void* pixels, uint32_t width, uint32_t height, uint32_t mipLevel = 0) = 0;
    virtual void GenerateMipmaps(TextureHandle handle) = 0;

    // ===== Samplers =====
    virtual SamplerHandle CreateSampler(const SamplerDesc& desc) = 0;
    virtual void DestroySampler(SamplerHandle handle) = 0;

    // ===== Framebuffers / Render Passes =====
    virtual FramebufferHandle CreateFramebuffer(const FramebufferDesc& desc) = 0;
    virtual void DestroyFramebuffer(FramebufferHandle handle) = 0;
    
    // Render pass helpers
    virtual void BeginRenderPass(FramebufferHandle fb, const RenderPassDesc& desc) = 0;
    virtual void EndRenderPass() = 0;
    
    // Swapchain render pass (for main window)
    virtual void BeginSwapchainPass(WindowHandle window, const RenderPassDesc& desc) = 0;
    virtual void EndSwapchainPass() = 0;

    // ===== Resource Binding =====
    virtual void BindPipeline(PipelineHandle pipeline) = 0;
    virtual void BindVertexBuffer(BufferHandle buffer, uint32_t slot = 0) = 0;
    virtual void BindIndexBuffer(BufferHandle buffer, IndexType type = IndexType::UInt32) = 0;
    virtual void BindTexture(uint32_t binding, TextureHandle texture, SamplerHandle sampler = INVALID_HANDLE) = 0;
    virtual void BindUniformBuffer(uint32_t binding, BufferHandle buffer, uint32_t offset, uint32_t size) = 0;

    // ===== Draw Calls =====
    virtual void Draw(uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0) = 0;
    virtual void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0) = 0;
    virtual void DispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) = 0;

    // ===== Scissor / Viewport =====
    virtual void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;
    virtual void SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;

    // ===== Debug / ImGui =====
    virtual void InitDebugUI(WindowHandle window) = 0;
    virtual void ShutdownDebugUI() = 0;
    virtual void BeginDebugUIFrame(WindowHandle window) = 0;
    virtual void EndDebugUIFrame() = 0;

    // ===== Escape Hatches (Runtime virtual) =====
    // Returned as backend::*, forward-declared at the top of this header so the
    // interface never has to include <vulkan/vulkan.h> or <Metal/Metal.h>.
    // Backends override these; they default to nullptr so backends that have no
    // native path (e.g. the raylib backend) need not implement them.
    virtual backend::IVulkanExtensions* AsVulkan() { return nullptr; }
    virtual backend::IMetalExtensions* AsMetal()   { return nullptr; }

    // ===== Capabilities Query =====
    virtual const DeviceCaps& GetCaps() const = 0;
};

} // namespace gpu

// =============================================================================
// Math re-exports
// =============================================================================
//
// Deliberately minimal. The gpu types (Vec3, Mat4, Color, ...) need no re-export
// because they are declared in this same namespace. Only the free functions from
// namespace math are brought in, and only the ones a call site is likely to want
// through the backend namespace.
//
// There is intentionally NO `gpu::backend` global and no inline Init/BeginFrame/
// IsKeyDown wrappers here. An earlier draft had them, and they were a trap: the
// global was declared extern and never defined, so the first call site to use it
// would have hit a link error with nothing in the header to explain why. The
// engine's real render API is namespace gfx (include/Engine/Graphics.hpp), and
// nothing calls these wrappers today. A convenience shim should be added when
// there is a caller for it, not before -- and if one is added, the backend
// instance should be owned by whoever created it (a unique_ptr), not hidden in a
// global.
//
// Depth range is the one thing that must not be smoothed over. Only the
// OpenGL-range projections are re-exported; the ZO variants (math::PerspectiveZO /
// math::OrthoZO) are deliberately omitted, so a call site that writes
// gpu::Perspective on a Vulkan path fails to compile instead of silently
// building a [-1, +1] projection for a [0, +1] depth buffer. Vulkan code must
// spell out math::PerspectiveZO, which is the reminder that it picked the range.
namespace gpu {

using math::Identity;
using math::ComposeTRS;

using math::Translate;
using math::RotateX;
using math::RotateY;
using math::RotateZ;
using math::RotateXYZ;
using math::Scale;
using math::Mul;

using math::LookAt;
using math::Ortho;
using math::Perspective;

using math::Transpose;
using math::Inverse;

using math::Add;
using math::Sub;
using math::Cross;
using math::Dot;
using math::Length;
using math::Normalize;
using math::Lerp;

} // namespace gpu
