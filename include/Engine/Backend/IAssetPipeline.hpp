#pragma once

#include "GraphicsBackendTypes.hpp"
#include <string>
#include <vector>
#include <optional>

// MeshAsset / TextureAsset are declared in GraphicsBackendTypes.hpp (namespace
// gfx) so the asset pipeline and the graphics backend agree on one definition.

namespace gpu {

class IAssetPipeline {
public:
    virtual ~IAssetPipeline() = default;
    
    // Texture loading (KTX2 preferred, fallback to stb_image)
    virtual std::optional<TextureAsset> LoadTexture(const std::string& path) = 0;
    virtual std::optional<TextureAsset> LoadTextureKTX2(const std::string& path) = 0;
    
    // Mesh/Model loading (glTF + meshoptimizer)
    virtual std::optional<MeshAsset> LoadModel(const std::string& path) = 0;
    virtual std::optional<MeshAsset> LoadModelGLTF(const std::string& path) = 0;
    
    // Pre-processing (offline, build-time)
    virtual bool ProcessTextureToKTX2(const std::string& input, const std::string& output) = 0;
    virtual bool ProcessModelToGLTF(const std::string& input, const std::string& output) = 0;
    virtual bool OptimizeMesh(const std::string& input, const std::string& output) = 0;  // meshoptimizer
    
    // Runtime asset management
    virtual void SetSearchPaths(const std::vector<std::string>& paths) = 0;
    virtual void ClearCache() = 0;
};

} // namespace gpu

// Factory function
namespace gpu {
std::unique_ptr<IAssetPipeline> CreateAssetPipeline();  // defined in AssetPipeline.cpp
}

// ============================================================================
// KTX2 Texture Format Notes
// ============================================================================
// KTX2 is the Khronos standard for GPU-ready textures
// Features:
// - Supercompression (Basis Universal, Zstandard)
// - Mipmaps included
// - Cubemap/array/3D support
// - sRGB/linear color space flags
// - Directly uploadable to GPU (Vulkan: vkCmdCopyBufferToImage)
//
// Usage:
// 1. Offline: ProcessTextureToKTX2("texture.png", "texture.ktx2")
// 2. Runtime: LoadTextureKTX2("texture.ktx2") -> TextureAsset
//
// Compression options:
// - UASTC (high quality, larger) -> BasisU transcodes to BC7/ASTC/ETC2
// - ETC1S (smaller, lower quality) -> transcodes to ETC2/BC1
//
// For Vulkan: prefer UASTC + Zstd supercompression

// ============================================================================
// glTF Model Format Notes
// ============================================================================
// glTF 2.0 is the Khronos standard for 3D assets
// Features:
// - PBR materials (metallic-roughness, specular-glossiness)
// - Meshes with primitives (position, normal, tangent, texcoord, color, joints, weights)
// - Skins, animations, morph targets
// - Extensions: KHR_mesh_quantization, KHR_texture_transform, EXT_mesh_gpu_instancing
//
// Usage:
// 1. Offline: ProcessModelToGLTF("model.fbx", "model.glb") + OptimizeMesh("model.glb", "model_opt.glb")
// 2. Runtime: LoadModelGLTF("model_opt.glb") -> MeshAsset
//
// meshoptimizer optimizations:
// - Vertex cache optimization (Forsyth)
// - Overdraw optimization
// - Vertex fetch optimization
// - Vertex quantization (if KHR_mesh_quantization)