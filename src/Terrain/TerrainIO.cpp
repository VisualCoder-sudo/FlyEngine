#include "../../include/Terrain/TerrainIO.hpp"
#include "../../include/Terrain/Terrain.hpp"
#include "raylib.h"
#include "raymath.h"
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace terrain {

// ============================================================================
// Image Loading Helpers
// ============================================================================

static Image LoadHeightmapImage(const std::string& path, HeightmapFormat& outFormat) {
    std::string ext = path.substr(path.find_last_of('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    if (ext == "png") outFormat = HeightmapFormat::PNG;
    else if (ext == "raw" || ext == "r16") outFormat = HeightmapFormat::RAW16;
    else if (ext == "exr") outFormat = HeightmapFormat::EXR;
    else if (ext == "tiff" || ext == "tif") outFormat = HeightmapFormat::TIFF;
    else outFormat = HeightmapFormat::PNG;
    
    Image img = {0};
    
    if (outFormat == HeightmapFormat::RAW16) {
        // RAW16: 16-bit grayscale, no header
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return img;
        
        const std::streamoff fileSize = file.tellg();
        file.seekg(0);
        
        // Header-less, so the size is the only hint: it must be a square of
        // 16-bit samples. Anything else used to be treated as 1024x1024, which
        // read past the end of a smaller buffer.
        if (fileSize < 2 || fileSize > (std::streamoff)512 * 1024 * 1024) return img;
        const size_t pixelCount = (size_t)fileSize / 2;
        size_t dim = (size_t)std::sqrt((double)pixelCount);
        while (dim * dim < pixelCount) dim++;
        while (dim * dim > pixelCount) dim--;
        if (dim * dim != pixelCount) return img;
        
        std::vector<uint16_t> data(pixelCount);
        if (!file.read(reinterpret_cast<char*>(data.data()), (std::streamsize)(pixelCount * 2))) return img;
        
        img.data = MemAlloc((unsigned int)(pixelCount * 2));
        img.width = (int)dim;
        img.height = (int)dim;
        img.format = PIXELFORMAT_UNCOMPRESSED_R16;
        img.mipmaps = 1;
        std::memcpy(img.data, data.data(), pixelCount * 2);
    } else {
        // Use raylib for PNG, EXR, TIFF
        img = LoadImage(path.c_str());
        
        // Convert to grayscale if needed
        if (img.format != PIXELFORMAT_UNCOMPRESSED_GRAYSCALE && 
            img.format != PIXELFORMAT_UNCOMPRESSED_R16) {
            ImageColorGrayscale(&img);
        }
    }
    
    return img;
}

static std::vector<float> ImageToHeightmap(const Image& img, bool flipY) {
    int w = img.width;
    int h = img.height;
    std::vector<float> heightmap(w * h);
    
    if (img.format == PIXELFORMAT_UNCOMPRESSED_R16) {
        const uint16_t* data = (const uint16_t*)img.data;
        for (int z = 0; z < h; z++) {
            int srcZ = flipY ? (h - 1 - z) : z;
            for (int x = 0; x < w; x++) {
                heightmap[z * w + x] = data[srcZ * w + x] / 65535.0f;
            }
        }
    } else if (img.format == PIXELFORMAT_UNCOMPRESSED_GRAYSCALE) {
        const uint8_t* data = (const uint8_t*)img.data;
        for (int z = 0; z < h; z++) {
            int srcZ = flipY ? (h - 1 - z) : z;
            for (int x = 0; x < w; x++) {
                heightmap[z * w + x] = data[srcZ * w + x] / 255.0f;
            }
        }
    } else {
        // Convert to grayscale first
        Image gray = img;
        ImageColorGrayscale(&gray);
        const uint8_t* data = (const uint8_t*)gray.data;
        for (int z = 0; z < h; z++) {
            int srcZ = flipY ? (h - 1 - z) : z;
            for (int x = 0; x < w; x++) {
                heightmap[z * w + x] = data[srcZ * w + x] / 255.0f;
            }
        }
        UnloadImage(gray);
    }
    
    return heightmap;
}

// ============================================================================
// Resampling
// ============================================================================

static std::vector<float> ResampleHeightmap(const std::vector<float>& src, int srcW, int srcH, int dstW, int dstH) {
    std::vector<float> dst((size_t)dstW * dstH);
    
    // dstW/dstH of 1 would divide by zero; callers validate, this keeps it safe.
    float scaleX = dstW > 1 ? (float)(srcW - 1) / (dstW - 1) : 0.0f;
    float scaleY = dstH > 1 ? (float)(srcH - 1) / (dstH - 1) : 0.0f;
    
    for (int z = 0; z < dstH; z++) {
        for (int x = 0; x < dstW; x++) {
            float sx = x * scaleX;
            float sz = z * scaleY;
            
            int x0 = (int)sx;
            int z0 = (int)sz;
            int x1 = std::min(x0 + 1, srcW - 1);
            int z1 = std::min(z0 + 1, srcH - 1);
            
            float fx = sx - x0;
            float fz = sz - z0;
            
            float h00 = src[z0 * srcW + x0];
            float h10 = src[z0 * srcW + x1];
            float h01 = src[z1 * srcW + x0];
            float h11 = src[z1 * srcW + x1];
            
            float h0 = h00 + (h10 - h00) * fx;
            float h1 = h01 + (h11 - h01) * fx;
            dst[z * dstW + x] = h0 + (h1 - h0) * fz;
        }
    }
    
    return dst;
}

// ============================================================================
// Heightmap Import
// ============================================================================

// Rejects import settings that would divide by zero, overflow the target grid
// size, or ask for an absurd allocation (NaN/zero chunk size, resolution < 2...).
static bool ValidImportSettings(const HeightmapImportSettings& s) {
    if (!std::isfinite((float)s.worldWidth) || !std::isfinite((float)s.worldDepth) ||
        !std::isfinite((float)s.targetChunkSize) || !std::isfinite((float)s.minHeight) ||
        !std::isfinite((float)s.maxHeight)) return false;
    if (!((float)s.worldWidth > 0.0f) || !((float)s.worldDepth > 0.0f) || !((float)s.targetChunkSize > 0.0f)) return false;
    if (s.targetResolution < 2 || s.targetResolution > 1025) return false;
    const double gw = std::max(1.0, std::ceil((double)s.worldWidth / s.targetChunkSize));
    const double gd = std::max(1.0, std::ceil((double)s.worldDepth / s.targetChunkSize));
    if (gw > 1024.0 || gd > 1024.0) return false;
    const double tw = gw * (s.targetResolution - 1) + 1, th = gd * (s.targetResolution - 1) + 1;
    return tw * th <= 64.0 * 1024.0 * 1024.0;
}

bool LoadHeightmap(Terrain& terrain, const std::string& path, const HeightmapImportSettings& settings) {
    if (!ValidImportSettings(settings)) return false;
    HeightmapFormat format;
    Image img = LoadHeightmapImage(path, format);
    if (img.data == nullptr) return false;
    
    const int srcW = img.width;
    const int srcH = img.height;
    if (srcW < 1 || srcH < 1) { UnloadImage(img); return false; }
    std::vector<float> fullHeightmap = ImageToHeightmap(img, settings.flipY);
    UnloadImage(img);
    
    // Calculate target dimensions
    int gridW = std::max(1, (int)std::ceil(settings.worldWidth / settings.targetChunkSize));
    int gridH = std::max(1, (int)std::ceil(settings.worldDepth / settings.targetChunkSize));
    int targetW = gridW * (settings.targetResolution - 1) + 1;
    int targetH = gridH * (settings.targetResolution - 1) + 1;
    
    // Resample to target grid
    std::vector<float> resampled = ResampleHeightmap(fullHeightmap, srcW, srcH, targetW, targetH);
    
    // Apply height scaling
    float heightRange = settings.maxHeight - settings.minHeight;
    float minH = FLT_MAX, maxH = -FLT_MAX;
    
    for (float& h : resampled) {
        h = settings.minHeight + h * heightRange;
        minH = std::min(minH, h);
        maxH = std::max(maxH, h);
    }
    
    // Update terrain
    terrain.chunkWorldSize = (float)settings.targetChunkSize;
    terrain.chunkResolution = settings.targetResolution;
    terrain.minHeight = minH;
    terrain.maxHeight = maxH;
    terrain.size.x = settings.worldWidth;
    terrain.size.z = settings.worldDepth;
    terrain.InitializeChunks();
    
    // Distribute to chunks
    int res = settings.targetResolution;
    for (int gz = 0; gz < gridH; gz++) {
        for (int gx = 0; gx < gridW; gx++) {
            int chunkIdx = gz * gridW + gx;
            TerrainChunk& chunk = terrain.chunks[chunkIdx];
            
            for (int z = 0; z < res; z++) {
                for (int x = 0; x < res; x++) {
                    int srcX = gx * (res - 1) + x;
                    int srcZ = gz * (res - 1) + z;
                    if (srcX < targetW && srcZ < targetH) {
                        chunk.heightmap[z * res + x] = resampled[srcZ * targetW + srcX];
                    }
                }
            }
            
            chunk.dirty = true;
            chunk.physicsDirty = true;
        }
    }
    
    terrain.needsFullRebuild = true;
    terrain.needsPhysicsRebuild = true;
    
    return true;
}

// ============================================================================
// Heightmap Export
// ============================================================================

bool SaveHeightmap(const Terrain& terrain, const std::string& path) {
    // Combine all chunks into single heightmap
    int gridW = terrain.gridWidth;
    int gridH = terrain.gridDepth;
    int res = terrain.chunkResolution;
    int totalW = gridW * (res - 1) + 1;
    int totalH = gridH * (res - 1) + 1;
    
    std::vector<float> fullHeightmap(totalW * totalH);
    
    for (int gz = 0; gz < gridH; gz++) {
        for (int gx = 0; gx < gridW; gx++) {
            const TerrainChunk& chunk = terrain.chunks[gz * gridW + gx];
            
            for (int z = 0; z < res; z++) {
                for (int x = 0; x < res; x++) {
                    int dstX = gx * (res - 1) + x;
                    int dstZ = gz * (res - 1) + z;
                    if (dstX < totalW && dstZ < totalH) {
                        fullHeightmap[dstZ * totalW + dstX] = chunk.heightmap[z * res + x];
                    }
                }
            }
        }
    }
    
    // Normalize to 0-1
    float range = terrain.maxHeight - terrain.minHeight;
    if (range <= 0) range = 1.0f;
    
    std::vector<uint16_t> data(totalW * totalH);
    for (size_t i = 0; i < fullHeightmap.size(); i++) {
        float normalized = (fullHeightmap[i] - terrain.minHeight) / range;
        normalized = std::clamp(normalized, 0.0f, 1.0f);
        data[i] = (uint16_t)(normalized * 65535.0f);
    }
    
    // Save as PNG (16-bit grayscale)
    Image img = {
        data.data(),
        totalW,
        totalH,
        1,
        PIXELFORMAT_UNCOMPRESSED_R16
    };
    
    bool result = ExportImage(img, path.c_str());
    
    return result;
}

// ============================================================================
// Procedural Generation
// ============================================================================

static float Noise2D(float x, float z, int seed) {
    // Simple hash-based noise
    int n = (int)(x * 123.456f) + (int)(z * 789.123f) + seed * 4567;
    n = (n << 13) ^ n;
    return (1.0f - ((n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.0f);
}

static float FractalNoise(float x, float z, const NoiseParams& params) {
    float value = 0.0f;
    float amplitude = 1.0f;
    float frequency = params.scale;
    float maxAmplitude = 0.0f;
    
    for (int i = 0; i < params.octaves; i++) {
        value += Noise2D(x * frequency, z * frequency, params.seed + i) * amplitude;
        maxAmplitude += amplitude;
        amplitude *= params.persistence;
        frequency *= params.lacunarity;
    }
    
    return maxAmplitude > 0.0f ? value / maxAmplitude : 0.0f;
}

void GenerateFromNoise(Terrain& terrain, const NoiseParams& params) {
    int gridW = terrain.gridWidth;
    int gridH = terrain.gridDepth;
    int res = terrain.chunkResolution;
    float worldScale = params.scale;
    
    for (int gz = 0; gz < gridH; gz++) {
        for (int gx = 0; gx < gridW; gx++) {
            TerrainChunk& chunk = terrain.chunks[gz * gridW + gx];
            
            float chunkWorldX = (gx - gridW * 0.5f + 0.5f) * terrain.chunkWorldSize;
            float chunkWorldZ = (gz - gridH * 0.5f + 0.5f) * terrain.chunkWorldSize;
            
            for (int z = 0; z < res; z++) {
                for (int x = 0; x < res; x++) {
                    float worldX = chunkWorldX + (x / (float)(res - 1) - 0.5f) * terrain.chunkWorldSize;
                    float worldZ = chunkWorldZ + (z / (float)(res - 1) - 0.5f) * terrain.chunkWorldSize;
                    
                    float noise = FractalNoise(worldX, worldZ, params);
                    chunk.heightmap[z * res + x] = noise * params.amplitude;
                }
            }
            
            chunk.dirty = true;
            chunk.physicsDirty = true;
        }
    }
    
    terrain.minHeight = -params.amplitude;
    terrain.maxHeight = params.amplitude;
    terrain.needsFullRebuild = true;
    terrain.needsPhysicsRebuild = true;
}

// ============================================================================
// RAW16 Export/Import (for external tools)
// ============================================================================

bool ExportRaw16(const Terrain& terrain, const std::string& path) {
    int gridW = terrain.gridWidth;
    int gridH = terrain.gridDepth;
    int res = terrain.chunkResolution;
    int totalW = gridW * (res - 1) + 1;
    int totalH = gridH * (res - 1) + 1;
    
    std::vector<float> fullHeightmap(totalW * totalH);
    
    for (int gz = 0; gz < gridH; gz++) {
        for (int gx = 0; gx < gridW; gx++) {
            const TerrainChunk& chunk = terrain.chunks[gz * gridW + gx];
            
            for (int z = 0; z < res; z++) {
                for (int x = 0; x < res; x++) {
                    int dstX = gx * (res - 1) + x;
                    int dstZ = gz * (res - 1) + z;
                    if (dstX < totalW && dstZ < totalH) {
                        fullHeightmap[dstZ * totalW + dstX] = chunk.heightmap[z * res + x];
                    }
                }
            }
        }
    }
    
    float range = terrain.maxHeight - terrain.minHeight;
    if (range <= 0) range = 1.0f;
    
    std::vector<uint16_t> data(totalW * totalH);
    for (size_t i = 0; i < fullHeightmap.size(); i++) {
        float normalized = (fullHeightmap[i] - terrain.minHeight) / range;
        normalized = std::clamp(normalized, 0.0f, 1.0f);
        data[i] = (uint16_t)(normalized * 65535.0f);
    }
    
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(data.data()), data.size() * sizeof(uint16_t));
    return file.good();
}

bool ImportRaw16(Terrain& terrain, const std::string& path, const HeightmapImportSettings& settings) {
    if (!ValidImportSettings(settings)) return false;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    
    const std::streamoff fileSize = file.tellg();
    file.seekg(0);
    
    if (fileSize < 2 || fileSize > (std::streamoff)512 * 1024 * 1024) return false;
    const int pixelCount = (int)(fileSize / 2);
    int dim = (int)std::sqrt((double)pixelCount);
    while ((int64_t)dim * dim < pixelCount) dim++;
    while ((int64_t)dim * dim > pixelCount) dim--;
    if ((int64_t)dim * dim != pixelCount) return false;
    
    std::vector<uint16_t> data(pixelCount);
    if (!file.read(reinterpret_cast<char*>(data.data()), (std::streamsize)pixelCount * 2)) return false;
    
    std::vector<float> heightmap(pixelCount);
    for (int i = 0; i < pixelCount; i++) {
        heightmap[i] = data[i] / 65535.0f;
    }
    
    // Resample to terrain grid
    int gridW = std::max(1, (int)std::ceil(settings.worldWidth / settings.targetChunkSize));
    int gridH = std::max(1, (int)std::ceil(settings.worldDepth / settings.targetChunkSize));
    int targetW = gridW * (settings.targetResolution - 1) + 1;
    int targetH = gridH * (settings.targetResolution - 1) + 1;
    
    std::vector<float> resampled = ResampleHeightmap(heightmap, dim, dim, targetW, targetH);
    
    float heightRange = settings.maxHeight - settings.minHeight;
    float minH = FLT_MAX, maxH = -FLT_MAX;
    
    for (float& h : resampled) {
        h = settings.minHeight + h * heightRange;
        minH = std::min(minH, h);
        maxH = std::max(maxH, h);
    }
    
    terrain.chunkWorldSize = (float)settings.targetChunkSize;
    terrain.chunkResolution = settings.targetResolution;
    terrain.minHeight = minH;
    terrain.maxHeight = maxH;
    terrain.size.x = settings.worldWidth;
    terrain.size.z = settings.worldDepth;
    terrain.InitializeChunks();
    
    int res = settings.targetResolution;
    for (int gz = 0; gz < gridH; gz++) {
        for (int gx = 0; gx < gridW; gx++) {
            int chunkIdx = gz * gridW + gx;
            TerrainChunk& chunk = terrain.chunks[chunkIdx];
            
            for (int z = 0; z < res; z++) {
                for (int x = 0; x < res; x++) {
                    int srcX = gx * (res - 1) + x;
                    int srcZ = gz * (res - 1) + z;
                    if (srcX < targetW && srcZ < targetH) {
                        chunk.heightmap[z * res + x] = resampled[srcZ * targetW + srcX];
                    }
                }
            }
            
            chunk.dirty = true;
            chunk.physicsDirty = true;
        }
    }
    
    terrain.needsFullRebuild = true;
    terrain.needsPhysicsRebuild = true;
    
    return true;
}

} // namespace terrain