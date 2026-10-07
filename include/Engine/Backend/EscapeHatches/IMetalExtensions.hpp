#pragma once

#include "GraphicsBackendTypes.hpp"
#include <vector>
#include <string>

using gpu::WindowHandle;
using gpu::BufferHandle;
using gpu::TextureHandle;
using gpu::SamplerHandle;
using gpu::PipelineHandle;

// Forward declare Metal types (avoid including Metal headers in cross-platform header)
struct IMetalDevice;
struct IMetalCommandQueue;
struct IMetalCommandBuffer;
struct IMetalTexture;
struct IMetalBuffer;
struct IMetalPipelineState;
struct IMetalDepthStencilState;
struct IMetalSamplerState;
struct IMetalRenderPassDescriptor;
struct IMetalDrawable;
struct CAMetalLayer;

namespace backend {

struct IMetalExtensions {
    virtual ~IMetalExtensions() = default;
    
    // ===== Device / Queue =====
    virtual IMetalDevice* GetDevice() = 0;
    virtual IMetalCommandQueue* GetCommandQueue() = 0;
    
    // ===== Swapchain (per-window) =====
    virtual CAMetalLayer* GetMetalLayer(WindowHandle window) = 0;
    virtual IMetalDrawable* GetCurrentDrawable(WindowHandle window) = 0;
    virtual IMetalTexture* GetCurrentTexture(WindowHandle window) = 0;
    virtual IMetalRenderPassDescriptor* GetCurrentRenderPassDescriptor(WindowHandle window) = 0;
    
    // ===== Resource Handles =====
    virtual IMetalBuffer* GetBufferHandle(BufferHandle handle) = 0;
    virtual IMetalTexture* GetTextureHandle(TextureHandle handle) = 0;
    virtual IMetalSamplerState* GetSamplerHandle(SamplerHandle handle) = 0;
    virtual IMetalPipelineState* GetPipelineHandle(PipelineHandle handle) = 0;
    virtual IMetalDepthStencilState* GetDepthStencilState(PipelineHandle handle) = 0;
    
    // ===== Command Buffer =====
    virtual IMetalCommandBuffer* GetCurrentCommandBuffer(WindowHandle window) = 0;
    
    // ===== Window Handle =====
    virtual void* GetNSView(WindowHandle window) = 0;  // NSView* for CAMetalLayer
    
    // ===== Debug =====
    // Named per resource kind rather than overloaded: every gfx handle type is an
    // alias of uint32_t, so overloading on them would be a redeclaration.
    virtual void SetBufferDebugName(BufferHandle handle, const char* name) = 0;
    virtual void SetTextureDebugName(TextureHandle handle, const char* name) = 0;
    virtual void SetPipelineDebugName(PipelineHandle handle, const char* name) = 0;
    virtual void PushDebugGroup(IMetalCommandBuffer* cmd, const char* name) = 0;
    virtual void PopDebugGroup(IMetalCommandBuffer* cmd) = 0;
};

} // namespace backend