#pragma once

#include "GraphicsBackendTypes.hpp"
#include <vulkan/vulkan.h>
#include <vector>
#include <string>

namespace backend {

// This header is a leaf: it pulls in <vulkan/vulkan.h> and VMA, so it must never
// be included from GraphicsBackend.hpp. Code that only needs the pointer does
// `backend::IVulkanExtensions*` against the forward declaration there.
using gpu::WindowHandle;
using gpu::BufferHandle;
using gpu::TextureHandle;
using gpu::SamplerHandle;
using gpu::PipelineHandle;
using gpu::ShaderHandle;
using gpu::FramebufferHandle;

struct IVulkanExtensions {
    virtual ~IVulkanExtensions() = default;
    
    // ===== Instance / Device =====
    virtual VkInstance GetInstance() = 0;
    virtual VkPhysicalDevice GetPhysicalDevice() = 0;
    virtual VkDevice GetDevice() = 0;
    
    // ===== Queue =====
    virtual uint32_t GetGraphicsQueueFamily() = 0;
    virtual VkQueue GetGraphicsQueue() = 0;
    virtual uint32_t GetPresentQueueFamily() = 0;
    virtual VkQueue GetPresentQueue() = 0;
    virtual uint32_t GetComputeQueueFamily() = 0;
    virtual VkQueue GetComputeQueue() = 0;
    
    // ===== Swapchain (per-window) =====
    virtual VkSwapchainKHR GetSwapchain(WindowHandle window) = 0;
    virtual std::vector<VkImage> GetSwapchainImages(WindowHandle window) = 0;
    virtual std::vector<VkImageView> GetSwapchainImageViews(WindowHandle window) = 0;
    virtual VkFormat GetSwapchainFormat(WindowHandle window) = 0;
    virtual VkColorSpaceKHR GetSwapchainColorSpace(WindowHandle window) = 0;
    virtual VkExtent2D GetSwapchainExtent(WindowHandle window) = 0;
    virtual uint32_t GetSwapchainImageCount(WindowHandle window) = 0;
    
    // ===== Synchronization (per-frame, per-window) =====
    virtual VkSemaphore GetImageAcquiredSemaphore(WindowHandle window, uint32_t frameIndex) = 0;
    virtual VkSemaphore GetRenderCompleteSemaphore(WindowHandle window, uint32_t frameIndex) = 0;
    virtual VkFence GetInFlightFence(WindowHandle window, uint32_t frameIndex) = 0;
    virtual uint32_t GetCurrentFrameIndex(WindowHandle window) = 0;
    
    // ===== Command Buffers =====
    virtual VkCommandPool GetCommandPool(WindowHandle window) = 0;
    virtual VkCommandBuffer GetCurrentCommandBuffer(WindowHandle window) = 0;
    virtual std::vector<VkCommandBuffer> GetCommandBuffers(WindowHandle window, uint32_t frameIndex) = 0;
    
    // ===== Resource Handles (for interop) =====
    virtual VkBuffer GetBufferHandle(BufferHandle handle) = 0;
    virtual VkDeviceMemory GetBufferMemory(BufferHandle handle) = 0;
    virtual VkImage GetTextureHandle(TextureHandle handle) = 0;
    virtual VkImageView GetTextureView(TextureHandle handle) = 0;
    virtual VkDeviceMemory GetTextureMemory(TextureHandle handle) = 0;
    virtual VkSampler GetSamplerHandle(SamplerHandle handle) = 0;
    virtual VkPipeline GetPipelineHandle(PipelineHandle handle) = 0;
    virtual VkPipelineLayout GetPipelineLayout(PipelineHandle handle) = 0;
    virtual VkDescriptorSetLayout GetDescriptorSetLayout(ShaderHandle shader, uint32_t set) = 0;
    virtual VkDescriptorPool GetDescriptorPool() = 0;
    virtual VkDescriptorSet GetDescriptorSet(ShaderHandle shader, uint32_t set, uint32_t frameIndex) = 0;
    virtual VkRenderPass GetRenderPass(FramebufferHandle handle) = 0;
    virtual VkFramebuffer GetFramebufferHandle(FramebufferHandle handle) = 0;
    
    // ===== Memory Allocation (VMA) =====
    virtual VmaAllocator GetVmaAllocator() = 0;
    virtual VmaAllocation GetBufferAllocation(BufferHandle handle) = 0;
    virtual VmaAllocation GetTextureAllocation(TextureHandle handle) = 0;
    
    // ===== Pipeline Cache =====
    virtual VkPipelineCache GetPipelineCache() = 0;
    virtual void SavePipelineCache(const std::string& path) = 0;
    virtual void LoadPipelineCache(const std::string& path) = 0;
    
    // ===== Debug / Validation =====
    virtual void SetDebugName(VkObjectType objectType, uint64_t handle, const char* name) = 0;
    virtual void BeginDebugLabel(VkCommandBuffer cmd, const char* name, float color[4]) = 0;
    virtual void EndDebugLabel(VkCommandBuffer cmd) = 0;
    
    // ===== Custom Rendering =====
    // For advanced use cases: execute custom Vulkan commands
    virtual void ExecuteCommands(WindowHandle window, std::function<void(VkCommandBuffer)> recordCommands) = 0;
    
    // ===== Ray Tracing (if supported) =====
    virtual bool SupportsRayTracing() = 0;
    virtual VkAccelerationStructureKHR CreateBottomLevelAS(const std::vector<VkAccelerationStructureGeometryKHR>& geometries) = 0;
    virtual VkAccelerationStructureKHR CreateTopLevelAS(const std::vector<VkAccelerationStructureInstanceKHR>& instances) = 0;
    
    // ===== Window Handle (for WSI) =====
#if defined(_WIN32)
    virtual HWND GetWin32WindowHandle(WindowHandle window) = 0;
#elif defined(__linux__)
    virtual void* GetX11WindowHandle(WindowHandle window) = 0;  // X11 Window
    virtual void* GetWaylandSurface(WindowHandle window) = 0;   // wl_surface*
#endif
};

} // namespace backend