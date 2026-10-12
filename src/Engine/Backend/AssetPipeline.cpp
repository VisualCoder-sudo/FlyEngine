// ============================================================================
// gpu::IAssetPipeline implementation
// KTX2 textures + glTF/meshoptimizer meshes
// ============================================================================

#include "Engine/Backend/IAssetPipeline.hpp"
#include "Engine/Backend/GraphicsBackendTypes.hpp"
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <optional>
#include <mutex>
#include <cstring>

#include "tinygltf/tiny_gltf_v3.h"
#include "meshoptimizer/src/meshoptimizer.h"
#include "ktxsm/lib/include/ktx.h"
// ktxvulkan.h requires Vulkan headers; we use ktxTexture2->vkFormat directly
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

namespace fs = std::filesystem;

namespace gpu {

// ============================================================================
// Internal helpers
// ============================================================================

struct AssetPipelineImpl : public IAssetPipeline {
    std::vector<std::string> searchPaths_;
    std::unordered_map<std::string, TextureAsset> textureCache_;
    std::unordered_map<std::string, MeshAsset> modelCache_;
    std::mutex mutex_;

    // Find file in search paths
    fs::path FindFile(const std::string& path) {
        if (fs::exists(path)) return fs::absolute(path);
        for (const auto& sp : searchPaths_) {
            fs::path candidate = fs::path(sp) / path;
            if (fs::exists(candidate)) return fs::absolute(candidate);
        }
        return {};
    }

    // Map VkFormat (as uint32_t) to TextureFormat
    // Common VkFormat values (from vulkan.h):
    TextureFormat VkFormatToTextureFormat(uint32_t vkFormat) {
        switch (vkFormat) {
            case 46:  // VK_FORMAT_R8G8B8A8_UNORM
            case 47:  // VK_FORMAT_R8G8B8A8_SRGB
                return TextureFormat::RGBA8;
            case 130: // VK_FORMAT_BC7_UNORM_BLOCK
            case 131: // VK_FORMAT_BC7_SRGB_BLOCK
                return TextureFormat::BC7;
            case 126: // VK_FORMAT_BC1_RGB_UNORM_BLOCK
            case 127: // VK_FORMAT_BC1_RGB_SRGB_BLOCK
                return TextureFormat::BC1_RGB;
            case 128: // VK_FORMAT_BC3_UNORM_BLOCK
            case 129: // VK_FORMAT_BC3_SRGB_BLOCK
                return TextureFormat::BC3;
            case 136: // VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK
            case 137: // VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK
                return TextureFormat::ETC2_RGB;
            case 138: // VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK
            case 139: // VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK
                return TextureFormat::ETC2_RGBA;
            case 140: // VK_FORMAT_ASTC_4x4_UNORM_BLOCK
            case 141: // VK_FORMAT_ASTC_4x4_SRGB_BLOCK
                return TextureFormat::ASTC_4x4;
            default:
                return TextureFormat::RGBA8;
        }
    }

    // KTX2 texture loading
    std::optional<TextureAsset> LoadKTX2Texture(const fs::path& path) {
        ktxTexture2* ktxTex = nullptr;
        KTX_error_code result = ktxTexture2_CreateFromNamedFile(
            path.string().c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktxTex);
        if (result != KTX_SUCCESS || !ktxTex) {
            return std::nullopt;
        }

        TextureAsset asset;
        asset.width = ktxTex->baseWidth;
        asset.height = ktxTex->baseHeight;
        asset.mipLevels = ktxTex->numLevels;
        
        // Map KTX format to our TextureFormat
        asset.format = VkFormatToTextureFormat(ktxTex->vkFormat);

        // Store KTX texture pointer for later GPU upload (handle assigned by backend)
        asset.handle = INVALID_HANDLE; // Will be set by backend when uploaded

        ktxTexture2_Destroy(ktxTex);
        return asset;
    }

    // Fallback stb_image loading
    std::optional<TextureAsset> LoadImageTexture(const fs::path& path) {
        int w, h, comp;
        stbi_uc* data = stbi_load(path.string().c_str(), &w, &h, &comp, 4);
        if (!data) return std::nullopt;

        TextureAsset asset;
        asset.width = w;
        asset.height = h;
        asset.mipLevels = 1;
        asset.format = TextureFormat::RGBA8;
        asset.handle = INVALID_HANDLE;

        stbi_image_free(data);
        return asset;
    }

    // Helper: find position offset in vertex layout
    // Position is always at location 0 in our layout
    size_t FindPositionOffset(const VertexLayout& layout) {
        for (const auto& attr : layout.attributes) {
            if (attr.location == 0) return attr.offset;
        }
        return 0;
    }

    // glTF model loading with meshoptimizer
    std::optional<MeshAsset> LoadGLTFModel(const fs::path& path) {
        tg3_model model{};
        tg3_error_stack errors{};
        tg3_parse_options options{};
        tg3_parse_options_init(&options);
        options.required_sections = TG3_REQUIRE_ALL;
        options.validate_indices = 1;
        tg3_error_stack_init(&errors);

        tg3_error_code result = tg3_parse_file(&model, &errors, path.string().c_str(), options.required_sections, &options);
        if (result != TG3_OK) {
            tg3_model_free(&model);
            tg3_error_stack_free(&errors);
            return std::nullopt;
        }

        MeshAsset asset;
        
        // For each mesh, create MeshData
        for (uint32_t mi = 0; mi < model.meshes_count; ++mi) {
            const tg3_mesh* mesh = &model.meshes[mi];
            
            for (uint32_t pi = 0; pi < mesh->primitives_count; ++pi) {
                const tg3_primitive* prim = &mesh->primitives[pi];
                
                // Get attribute accessors
                int posIdx = -1;
                int normalIdx = -1;
                int texIdx = -1;
                int tangentIdx = -1;
                int colorIdx = -1;
                int indicesIdx = prim->indices;
                
                for (uint32_t ai = 0; ai < prim->attributes_count; ++ai) {
                    const tg3_str_int_pair* attr = &prim->attributes[ai];
                    std::string name(attr->key.data, attr->key.len);
                    if (name == "POSITION") posIdx = attr->value;
                    else if (name == "NORMAL") normalIdx = attr->value;
                    else if (name == "TEXCOORD_0") texIdx = attr->value;
                    else if (name == "TANGENT") tangentIdx = attr->value;
                    else if (name == "COLOR_0") colorIdx = attr->value;
                }

                if (posIdx < 0) continue;

                // Build vertex layout
                MeshData meshData;
                meshData.dynamic = false;
                
                VertexLayout layout;
                uint32_t offset = 0;
                uint32_t location = 0;
                
                // Position (always required)
                layout.attributes.push_back({ location++, offset, TextureFormat::RGB32F, 0, false, 1 });
                offset += 12;
                
                // Normal
                if (normalIdx >= 0) {
                    layout.attributes.push_back({ location++, offset, TextureFormat::RGB32F, 0, false, 1 });
                    offset += 12;
                }
                
                // TexCoord
                if (texIdx >= 0) {
                    layout.attributes.push_back({ location++, offset, TextureFormat::RG32F, 0, false, 1 });
                    offset += 8;
                }
                
                // Tangent
                if (tangentIdx >= 0) {
                    layout.attributes.push_back({ location++, offset, TextureFormat::RGBA32F, 0, false, 1 });
                    offset += 16;
                }
                
                // Color
                if (colorIdx >= 0) {
                    layout.attributes.push_back({ location++, offset, TextureFormat::RGBA32F, 0, false, 1 });
                    offset += 16;
                }
                
                layout.stride = offset;
                meshData.layout = layout;

                // Extract vertex data
                const tg3_accessor* posAcc = &model.accessors[posIdx];
                const tg3_buffer_view* posView = &model.buffer_views[posAcc->buffer_view];
                const tg3_buffer* posBuf = &model.buffers[posView->buffer];
                uint32_t vertexCount = posAcc->count;
                meshData.vertexCount = vertexCount;
                meshData.vertexData.resize(vertexCount * layout.stride);

                // Read position data
                const uint8_t* posData = posBuf->data.data + posView->byte_offset + posAcc->byte_offset;
                for (uint32_t v = 0; v < vertexCount; ++v) {
                    const float* src = reinterpret_cast<const float*>(posData + v * posView->byte_stride);
                    float* dst = reinterpret_cast<float*>(meshData.vertexData.data() + v * layout.stride);
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                }

                // Read normals
                if (normalIdx >= 0) {
                    const tg3_accessor* acc = &model.accessors[normalIdx];
                    const tg3_buffer_view* view = &model.buffer_views[acc->buffer_view];
                    const tg3_buffer* buf = &model.buffers[view->buffer];
                    const uint8_t* srcData = buf->data.data + view->byte_offset + acc->byte_offset;
                    for (uint32_t v = 0; v < vertexCount; ++v) {
                        const float* src = reinterpret_cast<const float*>(srcData + v * view->byte_stride);
                        float* dst = reinterpret_cast<float*>(meshData.vertexData.data() + v * layout.stride + 12);
                        dst[0] = src[0];
                        dst[1] = src[1];
                        dst[2] = src[2];
                    }
                }

                // Read texcoords
                if (texIdx >= 0) {
                    const tg3_accessor* acc = &model.accessors[texIdx];
                    const tg3_buffer_view* view = &model.buffer_views[acc->buffer_view];
                    const tg3_buffer* buf = &model.buffers[view->buffer];
                    const uint8_t* srcData = buf->data.data + view->byte_offset + acc->byte_offset;
                    for (uint32_t v = 0; v < vertexCount; ++v) {
                        const float* src = reinterpret_cast<const float*>(srcData + v * view->byte_stride);
                        float* dst = reinterpret_cast<float*>(meshData.vertexData.data() + v * layout.stride + (normalIdx >= 0 ? 24 : 12));
                        dst[0] = src[0];
                        dst[1] = src[1];
                    }
                }

                // Read indices
                if (indicesIdx >= 0) {
                    const tg3_accessor* acc = &model.accessors[indicesIdx];
                    const tg3_buffer_view* view = &model.buffer_views[acc->buffer_view];
                    const tg3_buffer* buf = &model.buffers[view->buffer];
                    const uint8_t* srcData = buf->data.data + view->byte_offset + acc->byte_offset;
                    uint32_t indexCount = acc->count;
                    meshData.indexCount = indexCount;
                    meshData.indexData.resize(indexCount);
                    
                    if (acc->component_type == 5123) { // UNSIGNED_SHORT
                        const uint16_t* src = reinterpret_cast<const uint16_t*>(srcData);
                        for (uint32_t i = 0; i < indexCount; ++i) meshData.indexData[i] = src[i];
                    } else if (acc->component_type == 5125) { // UNSIGNED_INT
                        const uint32_t* src = reinterpret_cast<const uint32_t*>(srcData);
                        for (uint32_t i = 0; i < indexCount; ++i) meshData.indexData[i] = src[i];
                    }
                }

                // Optimize mesh with meshoptimizer
                if (meshData.indexCount > 0 && meshData.vertexCount > 0) {
                    // Vertex cache optimization
                    std::vector<uint32_t> remap(meshData.vertexCount);
                    uint32_t optVertexCount = meshopt_generateVertexRemap(
                        remap.data(), meshData.indexData.data(), meshData.indexCount,
                        meshData.vertexData.data(), meshData.vertexCount, layout.stride);
                    
                    std::vector<uint8_t> optVertexData(optVertexCount * layout.stride);
                    meshopt_remapVertexBuffer(optVertexData.data(), meshData.vertexData.data(),
                                             meshData.vertexCount, layout.stride, remap.data());
                    
                    std::vector<uint32_t> optIndexData(meshData.indexCount);
                    meshopt_remapIndexBuffer(optIndexData.data(), meshData.indexData.data(),
                                            meshData.indexCount, remap.data());
                    
                    // Overdraw optimization - need position buffer as float*
                    size_t posOffset = FindPositionOffset(layout);
                    meshopt_optimizeOverdraw(optIndexData.data(), optIndexData.data(),
                                          meshData.indexCount,
                                          reinterpret_cast<const float*>(optVertexData.data() + posOffset),
                                          optVertexCount, layout.stride, 1.05f);
                    
                    // Vertex fetch optimization
                    meshopt_optimizeVertexFetch(optVertexData.data(), optIndexData.data(),
                                               meshData.indexCount, optVertexData.data(),
                                               optVertexCount, layout.stride);
                    
                    meshData.vertexData = std::move(optVertexData);
                    meshData.indexData = std::move(optIndexData);
                    meshData.vertexCount = optVertexCount;
                }

                asset.meshes.push_back(std::move(meshData));
                asset.meshNames.push_back(std::string(mesh->name.data, mesh->name.len) + "_" + std::to_string(pi));
            }
        }

        tg3_model_free(&model);
        tg3_error_stack_free(&errors);
        
        if (asset.meshes.empty()) return std::nullopt;
        return asset;
    }

public:
    std::optional<TextureAsset> LoadTexture(const std::string& path) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = textureCache_.find(path);
        if (it != textureCache_.end()) return it->second;

        fs::path fullPath = FindFile(path);
        if (fullPath.empty()) return std::nullopt;

        std::optional<TextureAsset> asset;
        
        if (fullPath.extension() == ".ktx2" || fullPath.extension() == ".ktx") {
            asset = LoadKTX2Texture(fullPath);
        } else {
            asset = LoadImageTexture(fullPath);
        }

        if (asset) {
            textureCache_[path] = *asset;
            return *asset;
        }
        return std::nullopt;
    }

    std::optional<TextureAsset> LoadTextureKTX2(const std::string& path) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = textureCache_.find(path);
        if (it != textureCache_.end()) return it->second;

        fs::path fullPath = FindFile(path);
        if (fullPath.empty()) return std::nullopt;

        auto asset = LoadKTX2Texture(fullPath);
        if (asset) {
            textureCache_[path] = *asset;
        }
        return asset;
    }

    std::optional<MeshAsset> LoadModel(const std::string& path) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = modelCache_.find(path);
        if (it != modelCache_.end()) return it->second;

        fs::path fullPath = FindFile(path);
        if (fullPath.empty()) return std::nullopt;

        auto asset = LoadGLTFModel(fullPath);
        if (asset) {
            modelCache_[path] = *asset;
        }
        return asset;
    }

    std::optional<MeshAsset> LoadModelGLTF(const std::string& path) override {
        return LoadModel(path);
    }

    // Offline processing functions
    bool ProcessTextureToKTX2(const std::string& input, const std::string& output) override {
        // ktx_read library doesn't include encoding functionality (no basisu encoder)
        // This would require the full ktx library which conflicts with raylib's qoi symbols
        (void)input;
        (void)output;
        return false;
    }

    bool ProcessModelToGLTF(const std::string& input, const std::string& output) override {
        // For now, just copy if already glTF/GLB
        fs::path inPath = FindFile(input);
        if (inPath.empty()) return false;
        
        if (inPath.extension() == ".gltf" || inPath.extension() == ".glb") {
            std::error_code ec;
            fs::copy_file(inPath, output, fs::copy_options::overwrite_existing, ec);
            return !ec;
        }
        return false; // FBX/OBJ -> glTF needs assimp, not implemented
    }

    bool OptimizeMesh(const std::string& input, const std::string& output) override {
        // Not fully implemented - needs full glTF writer
        (void)input;
        (void)output;
        return false;
    }

    void SetSearchPaths(const std::vector<std::string>& paths) override {
        std::lock_guard<std::mutex> lock(mutex_);
        searchPaths_ = paths;
    }

    void ClearCache() override {
        std::lock_guard<std::mutex> lock(mutex_);
        textureCache_.clear();
        modelCache_.clear();
    }
};

std::unique_ptr<IAssetPipeline> CreateAssetPipeline() {
    return std::make_unique<AssetPipelineImpl>();
}

} // namespace gpu