#pragma once

#include "raylib.h"
#include "raymath.h"
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>

// Avoid Windows min/max macros
#undef min
#undef max

namespace terrain {

// ============================================================================
// Enums
// ============================================================================

enum class TerrainTool {
    Raise,        // Add height
    Lower,        // Subtract height
    Smooth,       // Average neighbors (laplacian)
    Flatten,      // Set to target height
    Paint,        // Paint texture layers
    Erode,        // Thermal/hydraulic erosion
    Noise,        // Add procedural noise
    Ramp,         // Linear gradient between two points
    None          // No tool selected
};

enum class PhysicsMode {
    Heightfield,    // Box3D heightfield (fast, 2.5D)
    TriangleMesh    // Box3D trimesh (precise, matches visual)
};

enum class HeightmapFormat {
    PNG,
    RAW16,
    EXR,
    TIFF
};

// ============================================================================
// Configuration Structs
// ============================================================================

struct TerrainBrush {
    TerrainTool tool = TerrainTool::Raise;
    float radius = 10.0f;           // World units
    float strength = 40.0f;         // Per-second for continuous, absolute for single (matches BasicTerrain)
    float hardness = 0.5f;          // 0=soft, 1=hard edge
    float targetHeight = 0.0f;      // For Flatten
    bool addMode = true;            // Raise vs Lower (toggle)

    // Brush shape
    enum class Shape { Circle, Square };
    Shape shape = Shape::Circle;

    // Paint tool settings
    int paintLayer = 0;      // Which texture layer to paint (0-3)
    bool paintErase = false;  // True = erase mode (decrease selected layer)

    // Noise tool settings
    int noiseSeed = 1337;           // Deterministic seed for noise
    float noiseScale = 40.0f;       // Feature size in meters (larger = broader features)

    // Erosion tool settings (mirrors BasicTerrain::ErosionSettings)
    float erosionThermal = 0.5f;       // Amount of talus relaxation per pass
    float erosionHydraulic = 0.8f;     // Amount of rain-flow carving per pass
    float erosionTalusDeg = 3.0f;      // Repose angle in degrees; steeper slopes erode
    float erosionRain = 0.05f;         // Rainfall per pass (water added)
    float erosionEvaporation = 0.03f;  // Fraction of water lost per pass
    float erosionDeposit = 0.6f;       // Fraction of eroded soil re-deposited (0 = washed away)
    int erosionIterations = 6;         // Passes to run per action
};

struct HeightmapImportSettings {
    float worldWidth = 1000.0f;      // World X size
    float worldDepth = 1000.0f;      // World Z size
    float maxHeight = 200.0f;        // Max elevation (meters)
    float minHeight = 0.0f;          // Min elevation
    bool flipY = false;              // Image origin correction
    int targetChunkSize = 256;       // Chunk world size
    int targetResolution = 65;       // Vertices per chunk edge (power of 2 + 1)
    bool generateMips = true;        // For LOD
};

struct LODConfig {
    float lod0Distance = 50.0f;      // Full res
    float lod1Distance = 200.0f;     // Half res
    float lod2Distance = 500.0f;     // Quarter res
    float cullDistance = 2000.0f;    // Don't render
    bool useGeomorphing = true;      // Smooth LOD transitions
};

struct TerrainLayer {
    std::string name;
    Texture2D albedo = {0};
    Texture2D normal = {0};
    Texture2D roughness = {0};
    float tileSize = 10.0f;          // World units per texture repeat
    float blendRange = 0.1f;         // 0-1 blend falloff
    bool valid = false;              // Textures loaded successfully

    // Source paths (persisted in terrain.terrain). Reloaded on scene load so
    // painted materials round-trip instead of being lost.
    std::string albedoPath;
    std::string normalPath;
    std::string roughnessPath;
};

struct NoiseParams {
    float scale = 0.01f;
    float amplitude = 50.0f;
    int octaves = 4;
    float persistence = 0.5f;
    float lacunarity = 2.0f;
    int seed = 1337;
};

// ============================================================================
// Data Structures
// ============================================================================

// Heightmap storage: chunked 2D grid of float heights
struct TerrainChunk {
    int lod = 0;                          // 0 = full res, 1 = half, etc.
    bool dirty = true;
    bool needsUpload = false;
    Mesh cpuMesh = {0};                   // CPU-side for editing/physics
    Model gpuModel = {0};                 // GPU upload
    BoundingBox bounds = {0};
    Vector2 chunkCoord = {0, 0};          // Grid position (chunk indices)
    int resolution = 65;                  // Vertices per edge
    float worldSize = 256.0f;             // World units per chunk
    Matrix transform = MatrixIdentity();  // World transform
    
    // Heightmap data (resolution x resolution floats)
    std::vector<float> heightmap;
    
    // Splatmap data (resolution x resolution RGBA8)
    std::vector<uint8_t> splatmap;        // 4 layers packed in RGBA
    
    // LOD meshes
    Mesh lodMeshes[3] = {{0}, {0}, {0}};  // LOD 0, 1, 2
    Model lodModels[3] = {{0}, {0}, {0}};
    bool lodMeshesCreated = false;
    
    // Physics
    void* physicsShape = nullptr;         // Box3D shape pointer
    bool physicsDirty = true;
};

// Per-vertex splat weights (4 layers per chunk, packed in RGBA)
struct SplatmapData {
    std::vector<uint8_t> weightMap;       // RGBA8, same resolution as heightmap
    int layerCount = 0;                   // Active layers (max 4 per chunk)
    int resolution = 65;
};

// Serialization structures
struct TerrainHeader {
    char magic[8] = {'F', 'L', 'T', 'E', 'R', 'R', 'A', 'N'};
    uint32_t version = 1;
    uint32_t chunkCount = 0;
    uint32_t chunkResolution = 65;
    float chunkWorldSize = 256.0f;
    float minHeight = 0.0f;
    float maxHeight = 200.0f;
    int gridWidth = 0;        // Chunks in X
    int gridDepth = 0;        // Chunks in Z
    PhysicsMode physicsMode = PhysicsMode::Heightfield;
    uint32_t layerCount = 0;

    // Transform (round-trips the terrain's position/size/rotation, which the
    // older format silently dropped).
    float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    float sizeX = 1000.0f, sizeY = 1.0f, sizeZ = 1000.0f;
    float rotX = 0.0f, rotY = 0.0f, rotZ = 0.0f;
};

// On-disk block wrapper. terrain.terrain stores one block per terrain, in slot
// order (TERRAIN1, TERRAIN2, ...). The slot number is pure ordering metadata;
// the quoted Explorer name is the authoritative identifier every other system
// (Flyscript, physics, editor, materials) keys off of.
struct TerrainBlockHeader {
    char mark[3] = {'T', 'E', 'R'};
    uint32_t terrainIndex = 0;            // 1-based slot (renumbered on delete)
    uint32_t nameLen = 0;                 // length of the UTF-8 name that follows
    uint32_t dataSize = 0;                // bytes of the nested Terrain payload
};

// Registry-wide file header
struct TerrainFileHeader {
    char magic[8] = {'F', 'L', 'Y', 'T', 'E', 'R', 'R', '1'};
    uint32_t version = 1;
    uint32_t terrainCount = 0;
};

struct TerrainChunkHeader {
    uint32_t chunkX = 0;
    uint32_t chunkZ = 0;
    uint32_t compressedSize = 0;
    uint32_t originalSize = 0;      // resolution * resolution * sizeof(float)
    float minHeight = 0.0f;
    float maxHeight = 200.0f;
    bool hasSplatmap = false;
    uint32_t splatmapCompressedSize = 0;
    uint32_t splatmapOriginalSize = 0;
};

struct TerrainLayerHeader {
    char name[64] = {0};
    float tileSize = 10.0f;
    float blendRange = 0.1f;
    char albedoPath[256] = {0};
    char normalPath[256] = {0};
    char roughnessPath[256] = {0};
};

// ============================================================================
// Utility Functions
// ============================================================================

inline int ResolutionForLOD(int baseResolution, int lod) {
    // baseResolution is power of 2 + 1 (e.g., 65, 129, 257)
    // Each LOD halves the resolution (minus 1 for shared edges)
    int res = baseResolution;
    for (int i = 0; i < lod; i++) {
        res = (res - 1) / 2 + 1;
    }
    return std::max(res, 2);
}

inline int GetVertexIndex(int x, int z, int resolution) {
    return z * resolution + x;
}

inline Vector2 WorldToChunkUV(float worldX, float worldZ, const TerrainChunk& chunk) {
    // Convert world position to chunk-local UV [0,1]
    float halfSize = chunk.worldSize * 0.5f;
    float localX = worldX - (chunk.chunkCoord.x * chunk.worldSize - halfSize);
    float localZ = worldZ - (chunk.chunkCoord.y * chunk.worldSize - halfSize);
    return { localX / chunk.worldSize, localZ / chunk.worldSize };
}

inline Vector2 WorldToHeightmapUV(float worldX, float worldZ, const TerrainChunk& chunk) {
    // Convert world position to heightmap pixel coordinates
    Vector2 uv = WorldToChunkUV(worldX, worldZ, chunk);
    return { uv.x * (chunk.resolution - 1), uv.y * (chunk.resolution - 1) };
}

// Bilinear interpolation on heightmap
inline float SampleHeightmapBilinear(const std::vector<float>& heightmap, int resolution, float u, float v) {
    int x0 = (int)floorf(u);
    int z0 = (int)floorf(v);
    int x1 = x0 + 1;
    int z1 = z0 + 1;
    
    x0 = std::clamp(x0, 0, resolution - 1);
    x1 = std::clamp(x1, 0, resolution - 1);
    z0 = std::clamp(z0, 0, resolution - 1);
    z1 = std::clamp(z1, 0, resolution - 1);
    
    float fx = u - x0;
    float fz = v - z0;
    
    float h00 = heightmap[GetVertexIndex(x0, z0, resolution)];
    float h10 = heightmap[GetVertexIndex(x1, z0, resolution)];
    float h01 = heightmap[GetVertexIndex(x0, z1, resolution)];
    float h11 = heightmap[GetVertexIndex(x1, z1, resolution)];
    
    float h0 = h00 + (h10 - h00) * fx;
    float h1 = h01 + (h11 - h01) * fx;
    
    return h0 + (h1 - h0) * fz;
}

// Compute normal from heightmap neighbors
inline Vector3 ComputeNormalFromHeightmap(const std::vector<float>& heightmap, int resolution, int x, int z, float worldScale) {
    float hL = (x > 0) ? heightmap[GetVertexIndex(x - 1, z, resolution)] : heightmap[GetVertexIndex(x, z, resolution)];
    float hR = (x < resolution - 1) ? heightmap[GetVertexIndex(x + 1, z, resolution)] : heightmap[GetVertexIndex(x, z, resolution)];
    float hD = (z > 0) ? heightmap[GetVertexIndex(x, z - 1, resolution)] : heightmap[GetVertexIndex(x, z, resolution)];
    float hU = (z < resolution - 1) ? heightmap[GetVertexIndex(x, z + 1, resolution)] : heightmap[GetVertexIndex(x, z, resolution)];
    
    Vector3 normal = {
        (hL - hR) * worldScale,
        2.0f * worldScale,
        (hD - hU) * worldScale
    };
    
    return Vector3Normalize(normal);
}

} // namespace terrain