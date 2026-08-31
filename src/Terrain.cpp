#include "Terrain.hpp"
#include "TerrainMesh.hpp"
#include "TerrainIO.hpp"
#include "TerrainSculpt.hpp"
#include "TerrainPhysics.hpp"
#include "TerrainEditor.hpp"
#include "TerrainRegistry.hpp"
#include "raylib.h"
#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>

// Avoid Windows min/max macros
#undef min
#undef max

namespace terrain {

// Static camera for drawing
static Camera3D* g_drawCamera = nullptr;

// ============================================================================
// Construction / Destruction
// ============================================================================

Terrain::Terrain(Vector3 center_, float width, float depth, int chunkSize, int chunkResolution)
    : center(center_), size({width, 1.0f, depth}), chunkWorldSize((float)chunkSize), chunkResolution(chunkResolution | 1) {
    // Ensure resolution is power of 2 + 1
    if ((chunkResolution & (chunkResolution - 1)) != 0) {
        chunkResolution = 65;
    }
    
    InitializeChunks();
    LoadTerrainShader();
    needsFullRebuild = true;

    // Auto-register so the registry always mirrors the live set of terrains,
    // no matter which path created this one. The slot order follows creation
    // order; deleting a terrain renumbers the rest without touching names.
    TerrainRegistry::Get().Register(this);
}

Terrain::~Terrain() {
    TerrainRegistry::Get().Unregister(this);
    DestroyChunks();
    DestroyPhysics();
    if (shaderLoaded) {
        UnloadShader(terrainShader);
    }
    for (auto& layer : layers) {
        if (layer.albedo.id) UnloadTexture(layer.albedo);
        if (layer.normal.id) UnloadTexture(layer.normal);
        if (layer.roughness.id) UnloadTexture(layer.roughness);
    }
}

// ============================================================================
// Entity Interface
// ============================================================================

void Terrain::Update(float dt) {
    if (needsFullRebuild) {
        RebuildAllChunks();
        needsFullRebuild = false;
    }
    
    RebuildDirtyChunks();
    
    if (needsPhysicsRebuild && physicsSim) {
        RebuildPhysics(physicsSim);
        needsPhysicsRebuild = false;
        if (onPhysicsRebuilt) onPhysicsRebuilt();
    }
}

void Terrain::Draw() {
    if (!shaderLoaded) return;
    
    Camera3D camera = g_drawCamera ? *g_drawCamera : Camera3D{};
    
    BeginShaderMode(terrainShader);
    UpdateShaderUniforms(camera);
    
    for (auto& chunk : chunks) {
        if (chunk.gpuModel.meshCount > 0) {
            DrawChunk(chunk, camera);
        }
    }
    
    EndShaderMode();
    
    // Debug overlays
    if (showWireframe) {
        for (auto& chunk : chunks) {
            if (chunk.gpuModel.meshCount > 0) {
                Vector3 pos = { chunk.transform.m12, chunk.transform.m13, chunk.transform.m14 };
                DrawModelWires(chunk.gpuModel, pos, 1.0f, RED);
            }
        }
    }
    
    if (showChunkBounds) {
        for (auto& chunk : chunks) {
            DrawBoundingBox(chunk.bounds, YELLOW);
        }
    }
    
    if (showLODColors) {
        static Color lodColors[3] = { GREEN, YELLOW, RED };
        for (auto& chunk : chunks) {
            if (chunk.gpuModel.meshCount > 0) {
                Color c = lodColors[std::clamp(chunk.lod, 0, 2)];
                c.a = 100;
                Vector3 pos = { chunk.transform.m12, chunk.transform.m13, chunk.transform.m14 };
                DrawModel(chunk.gpuModel, pos, 1.0f, c);
            }
        }
    }
}

void Terrain::DrawOverlay3D() {
    // Draw brush preview in editor mode
    if (isSelected && terrain::IsTerrainEditorActive()) {
        terrain::DrawTerrainBrushPreview(*g_drawCamera);
    }
}

// ============================================================================
// Chunk Management
// ============================================================================

void Terrain::InitializeChunks() {
    DestroyChunks();
    
    gridWidth = std::max(1, (int)ceilf(size.x / chunkWorldSize));
    gridDepth = std::max(1, (int)ceilf(size.z / chunkWorldSize));
    
    chunks.resize(gridWidth * gridDepth);
    
    float halfWidth = size.x * 0.5f;
    float halfDepth = size.z * 0.5f;
    
    for (int gz = 0; gz < gridDepth; gz++) {
        for (int gx = 0; gx < gridWidth; gx++) {
            int idx = gz * gridWidth + gx;
            TerrainChunk& chunk = chunks[idx];
            chunk.chunkCoord = { (float)gx, (float)gz };
            chunk.resolution = chunkResolution;
            chunk.worldSize = chunkWorldSize;
            chunk.heightmap.resize(chunkResolution * chunkResolution, 0.0f);
            chunk.splatmap.resize(chunkResolution * chunkResolution * 4, 0);
            CreateChunk(gx, gz);
        }
    }
    
    // Set default layer if none exist
    if (layers.empty()) {
        TerrainLayer defaultLayer;
        defaultLayer.name = "Base";
        defaultLayer.tileSize = 10.0f;
        defaultLayer.blendRange = 0.1f;
        defaultLayer.valid = true;
        layers.push_back(defaultLayer);
        
        // Fill splatmaps with base layer (R channel = 255)
        for (auto& chunk : chunks) {
            std::fill(chunk.splatmap.begin(), chunk.splatmap.end(), 0);
            for (size_t i = 0; i < chunk.splatmap.size(); i += 4) {
                chunk.splatmap[i] = 255; // R channel = layer 0
            }
        }
    }
}

void Terrain::CreateChunk(int gx, int gz) {
    int idx = gz * gridWidth + gx;
    TerrainChunk& chunk = chunks[idx];
    
    float chunkX = (gx - gridWidth * 0.5f + 0.5f) * chunkWorldSize;
    float chunkZ = (gz - gridDepth * 0.5f + 0.5f) * chunkWorldSize;
    
    chunk.transform = MatrixTranslate(chunkX, 0.0f, chunkZ);
    
    // Compute bounds
    float halfSize = chunkWorldSize * 0.5f;
    chunk.bounds.min = { chunkX - halfSize, minHeight, chunkZ - halfSize };
    chunk.bounds.max = { chunkX + halfSize, maxHeight, chunkZ + halfSize };
    
    chunk.dirty = true;
    chunk.physicsDirty = true;
}

void Terrain::DestroyChunks() {
    for (auto& chunk : chunks) {
        if (chunk.cpuMesh.vertices) UnloadMesh(chunk.cpuMesh);
        if (chunk.gpuModel.meshCount > 0) UnloadModel(chunk.gpuModel);
        for (int i = 0; i < 3; i++) {
            if (chunk.lodMeshes[i].vertices) UnloadMesh(chunk.lodMeshes[i]);
            if (chunk.lodModels[i].meshCount > 0) UnloadModel(chunk.lodModels[i]);
        }
        chunk = TerrainChunk{};
    }
    chunks.clear();
    gridWidth = 0;
    gridDepth = 0;
}

TerrainChunk* Terrain::GetChunkAt(float worldX, float worldZ) {
    // Convert world position to chunk grid coordinates
    float localX = worldX - center.x + size.x * 0.5f;
    float localZ = worldZ - center.z + size.z * 0.5f;
    
    int gx = (int)floorf(localX / chunkWorldSize);
    int gz = (int)floorf(localZ / chunkWorldSize);
    
    if (gx < 0 || gx >= gridWidth || gz < 0 || gz >= gridDepth) return nullptr;
    return &chunks[gz * gridWidth + gx];
}

const TerrainChunk* Terrain::GetChunkAt(float worldX, float worldZ) const {
    return const_cast<Terrain*>(this)->GetChunkAt(worldX, worldZ);
}

std::vector<TerrainChunk*> Terrain::GetChunksInRadius(Vector2 center, float radius) {
    std::vector<TerrainChunk*> result;
    float r2 = radius * radius;
    
    // Find chunk range
    float localX = center.x - this->center.x + size.x * 0.5f;
    float localZ = center.y - this->center.z + size.z * 0.5f;
    
    int minGx = std::max(0, (int)floorf((localX - radius) / chunkWorldSize));
    int maxGx = std::min(gridWidth - 1, (int)floorf((localX + radius) / chunkWorldSize));
    int minGz = std::max(0, (int)floorf((localZ - radius) / chunkWorldSize));
    int maxGz = std::min(gridDepth - 1, (int)floorf((localZ + radius) / chunkWorldSize));
    
    for (int gz = minGz; gz <= maxGz; gz++) {
        for (int gx = minGx; gx <= maxGx; gx++) {
            TerrainChunk* chunk = &chunks[gz * gridWidth + gx];
            
            // Quick bounding box check
            float dx = std::max(0.0f, std::max(chunk->bounds.min.x - center.x, center.x - chunk->bounds.max.x));
            float dz = std::max(0.0f, std::max(chunk->bounds.min.z - center.y, center.y - chunk->bounds.max.z));
            if (dx * dx + dz * dz <= r2) {
                result.push_back(chunk);
            }
        }
    }
    
    return result;
}

std::vector<const TerrainChunk*> Terrain::GetChunksInRadius(Vector2 center, float radius) const {
    std::vector<const TerrainChunk*> result;
    auto nonConst = const_cast<Terrain*>(this)->GetChunksInRadius(center, radius);
    result.reserve(nonConst.size());
    for (auto* c : nonConst) result.push_back(c);
    return result;
}

void Terrain::MarkChunkDirty(int gx, int gz) {
    if (gx < 0 || gx >= gridWidth || gz < 0 || gz >= gridDepth) return;
    chunks[gz * gridWidth + gx].dirty = true;
}

void Terrain::MarkChunkDirty(TerrainChunk* chunk) {
    if (chunk) chunk->dirty = true;
}

void Terrain::MarkAllChunksDirty() {
    for (auto& chunk : chunks) {
        chunk.dirty = true;
        chunk.physicsDirty = true;
    }
}

// ============================================================================
// Mesh Rebuilding
// ============================================================================

void Terrain::RebuildAllChunks() {
    for (auto& chunk : chunks) {
        RebuildChunkMesh(chunk, 0);
        RebuildChunkMesh(chunk, 1);
        RebuildChunkMesh(chunk, 2);
        RebuildChunkSplatmap(chunk);
        chunk.dirty = true;
        chunk.physicsDirty = true;
    }
    needsPhysicsRebuild = true;
}

void Terrain::RebuildDirtyChunks() {
    for (auto& chunk : chunks) {
        if (chunk.dirty) {
            RebuildChunkMesh(chunk, chunk.lod);
            RebuildChunkSplatmap(chunk);
            chunk.dirty = false;
            chunk.needsUpload = true;
        }
    }
}

void Terrain::UploadDirtyChunks() {
    for (auto& chunk : chunks) {
        if (chunk.needsUpload) {
            UploadChunk(chunk);
        }
    }
}

void Terrain::RebuildChunkMesh(TerrainChunk& chunk, int lod) {
    int res = ResolutionForLOD(chunk.resolution, lod);
    float scale = chunkWorldSize / (res - 1);
    
    Mesh mesh = GenerateTerrainMesh(chunk.heightmap.data(), res, scale, minHeight, maxHeight);
    
    if (lod == 0) {
        if (chunk.cpuMesh.vertices) UnloadMesh(chunk.cpuMesh);
        chunk.cpuMesh = mesh;
    } else {
        if (chunk.lodMeshes[lod].vertices) UnloadMesh(chunk.lodMeshes[lod]);
        chunk.lodMeshes[lod] = mesh;
    }
    
    // Update bounds
    chunk.bounds.min.y = minHeight;
    chunk.bounds.max.y = maxHeight;
}

void Terrain::RebuildChunkSplatmap(TerrainChunk& chunk) {
    // Splatmap is already in chunk.splatmap, just need to update GPU texture
    // This is handled in UploadChunk
}

void Terrain::UploadChunk(TerrainChunk& chunk) {
    // Upload LOD 0 mesh (current LOD)
    Mesh* currentMesh = (chunk.lod == 0) ? &chunk.cpuMesh : &chunk.lodMeshes[chunk.lod];
    Model* currentModel = (chunk.lod == 0) ? &chunk.gpuModel : &chunk.lodModels[chunk.lod];
    
    if (currentMesh->vertices) {
        if (currentModel->meshCount > 0) UnloadModel(*currentModel);
        *currentModel = LoadModelFromMesh(*currentMesh);
    }
    
    // Upload splatmap texture
    if (!chunk.splatmap.empty()) {
        Image splatImg = {
            chunk.splatmap.data(),
            chunk.resolution,
            chunk.resolution,
            1,
            PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
        };
        Texture2D splatTex = LoadTextureFromImage(splatImg);
        SetTextureFilter(splatTex, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(splatTex, TEXTURE_WRAP_CLAMP);
        
        // Store splatmap texture in model material
        if (currentModel->materialCount > 0 && currentModel->materials[0].maps[MATERIAL_MAP_ALBEDO].texture.id) {
            // We'll bind splatmap in shader via uniform
        }
        // Note: We need to bind this texture when drawing
        // For now, store it temporarily - proper implementation needs texture array or bind per chunk
        UnloadTexture(splatTex); // TODO: Keep reference
    }
    
    chunk.needsUpload = false;
}

void Terrain::UpdateChunks(const Camera3D& camera) {
    Vector3 camPos = camera.position;
    
    for (auto& chunk : chunks) {
        // Distance from camera to chunk center
        Vector3 chunkCenter = Vector3Transform({0, 0, 0}, chunk.transform);
        float dist = Vector3Distance(camPos, chunkCenter);
        
        // Determine LOD
        int newLod = 0;
        if (dist > lodConfig.lod2Distance) newLod = 2;
        else if (dist > lodConfig.lod1Distance) newLod = 1;
        
        if (newLod != chunk.lod) {
            chunk.lod = newLod;
            chunk.dirty = true; // Will rebuild with new LOD
        }
        
        // Culling
        if (dist > lodConfig.cullDistance) {
            // Could unload GPU mesh here for memory savings
        }
    }
}

// ============================================================================
// Heightmap Operations
// ============================================================================

bool Terrain::LoadHeightmap(const std::string& path, const HeightmapImportSettings& settings) {
    return terrain::LoadHeightmap(*this, path, settings);
}

bool Terrain::SaveHeightmap(const std::string& path) const {
    return terrain::SaveHeightmap(*this, path);
}

void Terrain::GenerateFromNoise(const NoiseParams& params) {
    terrain::GenerateFromNoise(*this, params);
}

void Terrain::CreateBlank(float defaultHeight) {
    for (auto& chunk : chunks) {
        std::fill(chunk.heightmap.begin(), chunk.heightmap.end(), defaultHeight);
        chunk.dirty = true;
        chunk.physicsDirty = true;
    }
    minHeight = defaultHeight;
    maxHeight = defaultHeight;
    needsFullRebuild = true;
}

// ============================================================================
// Sculpting
// ============================================================================

void Terrain::RaiseTerrain(Vector2 center, float radius, float strength) {
    TerrainBrush brush;
    brush.tool = TerrainTool::Raise;
    brush.radius = radius;
    brush.strength = strength;
    brush.addMode = true;
    ApplyBrush(brush, center);
}

void Terrain::LowerTerrain(Vector2 center, float radius, float strength) {
    TerrainBrush brush;
    brush.tool = TerrainTool::Lower;
    brush.radius = radius;
    brush.strength = strength;
    brush.addMode = false;
    ApplyBrush(brush, center);
}

void Terrain::SmoothTerrain(Vector2 center, float radius, float strength) {
    TerrainBrush brush;
    brush.tool = TerrainTool::Smooth;
    brush.radius = radius;
    brush.strength = strength;
    ApplyBrush(brush, center);
}

void Terrain::FlattenTerrain(Vector2 center, float radius, float targetHeight) {
    TerrainBrush brush;
    brush.tool = TerrainTool::Flatten;
    brush.radius = radius;
    brush.strength = 1.0f;
    brush.targetHeight = targetHeight;
    ApplyBrush(brush, center);
}

void Terrain::RampTerrain(Vector2 start, Vector2 end, float startHeight, float endHeight) {
    terrain::RampTerrain(*this, start, end, startHeight, endHeight);
}

void Terrain::NoiseTerrain(Vector2 center, float radius, const NoiseParams& params) {
    TerrainBrush brush;
    brush.tool = TerrainTool::Noise;
    brush.radius = radius;
    brush.strength = params.amplitude;
    ApplyBrush(brush, center);
}

void Terrain::ApplyBrush(const TerrainBrush& brush, Vector2 center) {
    terrain::ApplyBrush(*this, brush, center);
    
    if (onHeightmapChanged) onHeightmapChanged();
    needsPhysicsRebuild = true;
}

// ============================================================================
// Query
// ============================================================================

float Terrain::GetHeightAt(float x, float z) const {
    const TerrainChunk* chunk = GetChunkAt(x, z);
    if (!chunk) return 0.0f;
    
    Vector2 uv = WorldToHeightmapUV(x, z, *chunk);
    return SampleHeightmapBilinear(chunk->heightmap, chunk->resolution, uv.x, uv.y);
}

Vector3 Terrain::GetNormalAt(float x, float z) const {
    const TerrainChunk* chunk = GetChunkAt(x, z);
    if (!chunk) return {0, 1, 0};
    
    Vector2 uv = WorldToHeightmapUV(x, z, *chunk);
    int ix = (int)uv.x;
    int iz = (int)uv.y;
    
    float worldScale = chunkWorldSize / (chunk->resolution - 1);
    return ComputeNormalFromHeightmap(chunk->heightmap, chunk->resolution, ix, iz, worldScale);
}

bool Terrain::Raycast(const Ray& ray, float* outDistance, Vector3* outHitPoint, Vector3* outNormal) const {
    // Simple raycast against terrain chunks
    // For better performance, use physics engine raycast
    float closestDist = FLT_MAX;
    Vector3 hitPoint = {0}, hitNormal = {0};
    bool hit = false;
    
    for (const auto& chunk : chunks) {
        // Quick bounds check
        RayCollision rc = GetRayCollisionBox(ray, chunk.bounds);
        if (!rc.hit) continue;
        
        // Sample heightmap along ray (simplified)
        // In practice, use physics engine
        float dist = rc.distance;
        if (dist < closestDist) {
            closestDist = dist;
            hitPoint = rc.point;
            hitNormal = GetNormalAt(hitPoint.x, hitPoint.z);
            hit = true;
        }
    }
    
    if (hit) {
        if (outDistance) *outDistance = closestDist;
        if (outHitPoint) *outHitPoint = hitPoint;
        if (outNormal) *outNormal = hitNormal;
    }
    
    return hit;
}

// ============================================================================
// Materials
// ============================================================================

void Terrain::AddLayer(const TerrainLayer& layer) {
    if (layers.size() >= 4) return; // Max 4 layers per chunk (RGBA)
    layers.push_back(layer);
    
    // Update splatmaps for all chunks
    for (auto& chunk : chunks) {
        RebuildChunkSplatmap(chunk);
        chunk.dirty = true;
    }
}

void Terrain::RemoveLayer(int index) {
    if (index < 0 || index >= (int)layers.size()) return;
    layers.erase(layers.begin() + index);
    
    // Remap splatmaps
    for (auto& chunk : chunks) {
        RebuildChunkSplatmap(chunk);
        chunk.dirty = true;
    }
}

void Terrain::PaintLayer(Vector2 center, float radius, float strength, int layerIndex, bool erase) {
    terrain::PaintLayer(*this, center, radius, strength, layerIndex, erase);
    
    if (onHeightmapChanged) onHeightmapChanged();
}

void Terrain::ReloadMaterialTextures(const std::string& baseDir) {
    struct Slot { Texture2D* tex; const std::string& path; };
    for (auto& layer : layers) {
        auto resolve = [&](const std::string& p) -> std::string {
            if (p.empty()) return {};
            // If the stored path is absolute, use it as-is.
            bool absolute = !p.empty() &&
                (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':'));
            if (absolute || baseDir.empty()) return p;
            return baseDir + "/" + p;
        };

        if (layer.albedo.id) { UnloadTexture(layer.albedo); layer.albedo = {0}; }
        if (layer.normal.id) { UnloadTexture(layer.normal); layer.normal = {0}; }
        if (layer.roughness.id) { UnloadTexture(layer.roughness); layer.roughness = {0}; }

        std::string ap = resolve(layer.albedoPath);
        if (!ap.empty()) layer.albedo = LoadTexture(ap.c_str());

        std::string np = resolve(layer.normalPath);
        if (!np.empty()) layer.normal = LoadTexture(np.c_str());

        std::string rp = resolve(layer.roughnessPath);
        if (!rp.empty()) layer.roughness = LoadTexture(rp.c_str());

        layer.valid = layer.albedo.id > 0 || layer.normal.id > 0 || layer.roughness.id > 0;
    }
}

// ============================================================================
// Physics
// ============================================================================

void Terrain::SetPhysicsMode(PhysicsMode mode) {
    physicsMode = mode;
    needsPhysicsRebuild = true;
}

void Terrain::RebuildPhysics(phys::Simulation* sim) {
    if (!sim) sim = physicsSim;
    if (!sim) return;
    
    DestroyPhysics();
    
    if (physicsMode == PhysicsMode::Heightfield) {
        CreatePhysicsHeightfield();
    } else {
        CreatePhysicsTriangleMesh();
    }
    
    UpdatePhysicsTransform();
}

// ============================================================================
// Serialization
// ============================================================================

bool Terrain::WriteChunkData(std::ostream& out, const TerrainChunk& chunk) const {
    TerrainChunkHeader ch;
    ch.chunkX = (uint32_t)chunk.chunkCoord.x;
    ch.chunkZ = (uint32_t)chunk.chunkCoord.y;
    ch.minHeight = minHeight;
    ch.maxHeight = maxHeight;

    // Heightmap: quantize float -> 16-bit.
    std::vector<uint16_t> quantized;
    QuantizeHeightmap(chunk.heightmap, quantized, minHeight, maxHeight);
    size_t origSize = quantized.size() * sizeof(uint16_t);
    ch.originalSize = (uint32_t)origSize;
    ch.compressedSize = (uint32_t)origSize; // Same since no compression

    // Splatmap (RGBA8, 4 layers).
    size_t splatOrig = 0;
    if (!chunk.splatmap.empty()) {
        ch.hasSplatmap = true;
        splatOrig = chunk.splatmap.size();
        ch.splatmapOriginalSize = (uint32_t)splatOrig;
        ch.splatmapCompressedSize = (uint32_t)splatOrig;
    }

    out.write(reinterpret_cast<const char*>(&ch), sizeof(ch));
    out.write(reinterpret_cast<const char*>(quantized.data()), (std::streamsize)origSize);
    if (ch.hasSplatmap) {
        out.write(reinterpret_cast<const char*>(chunk.splatmap.data()), (std::streamsize)splatOrig);
    }
    return out.good();
}

bool Terrain::ReadChunkData(std::istream& in, TerrainChunk& chunk) {
    TerrainChunkHeader ch;
    in.read(reinterpret_cast<char*>(&ch), sizeof(ch));
    if (!in) return false;

    chunk.chunkCoord = { (float)ch.chunkX, (float)ch.chunkZ };

    std::vector<uint16_t> quantized(ch.originalSize / sizeof(uint16_t));
    in.read(reinterpret_cast<char*>(quantized.data()), (std::streamsize)ch.originalSize);
    DequantizeHeightmap(quantized, chunk.heightmap, minHeight, maxHeight);

    if (ch.hasSplatmap) {
        chunk.splatmap.resize(ch.splatmapOriginalSize);
        in.read(reinterpret_cast<char*>(chunk.splatmap.data()), (std::streamsize)ch.splatmapOriginalSize);
    }

    chunk.dirty = true;
    chunk.physicsDirty = true;
    return in.good();
}

bool Terrain::WriteLayerData(std::ostream& out, const TerrainLayer& layer) const {
    TerrainLayerHeader lh;
    strncpy(lh.name, layer.name.c_str(), 63);
    lh.tileSize = layer.tileSize;
    lh.blendRange = layer.blendRange;
    strncpy(lh.albedoPath, layer.albedoPath.c_str(), 255);
    strncpy(lh.normalPath, layer.normalPath.c_str(), 255);
    strncpy(lh.roughnessPath, layer.roughnessPath.c_str(), 255);
    out.write(reinterpret_cast<const char*>(&lh), sizeof(lh));
    return out.good();
}

bool Terrain::ReadLayerData(std::istream& in, TerrainLayer& layer) {
    TerrainLayerHeader lh;
    in.read(reinterpret_cast<char*>(&lh), sizeof(lh));
    if (!in) return false;

    layer.name = lh.name;
    layer.tileSize = lh.tileSize;
    layer.blendRange = lh.blendRange;
    layer.albedoPath = lh.albedoPath;
    layer.normalPath = lh.normalPath;
    layer.roughnessPath = lh.roughnessPath;
    layer.valid = false; // Re-resolved/loaded by the caller against project dir.
    return true;
}

bool Terrain::WriteToStream(std::ostream& out) const {
    // Header doubles as the per-terrain payload descriptor. The registry wraps
    // this payload in a TERRAIN# block; here we just write the payload itself.
    TerrainHeader header;
    header.magic[0] = 'F'; // brand the payload so ReadFromStream can validate
    header.chunkCount = (uint32_t)chunks.size();
    header.chunkResolution = (uint32_t)chunkResolution;
    header.chunkWorldSize = chunkWorldSize;
    header.minHeight = minHeight;
    header.maxHeight = maxHeight;
    header.gridWidth = gridWidth;
    header.gridDepth = gridDepth;
    header.physicsMode = physicsMode;
    header.layerCount = (uint32_t)layers.size();

    header.posX = center.x; header.posY = center.y; header.posZ = center.z;
    header.sizeX = size.x;  header.sizeY = size.y;  header.sizeZ = size.z;
    header.rotX = rotation.x; header.rotY = rotation.y; header.rotZ = rotation.z;

    out.write(reinterpret_cast<const char*>(&header), sizeof(header));

    for (const auto& layer : layers) {
        if (!WriteLayerData(out, layer)) return false;
    }
    for (const auto& chunk : chunks) {
        if (!WriteChunkData(out, chunk)) return false;
    }
    return out.good();
}

bool Terrain::ReadFromStream(std::istream& in) {
    TerrainHeader header;
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in) return false;
    if (header.magic[0] != 'F') return false; // must be a terrain payload

    // Apply settings.
    chunkResolution = header.chunkResolution;
    chunkWorldSize = header.chunkWorldSize;
    minHeight = header.minHeight;
    maxHeight = header.maxHeight;
    gridWidth = header.gridWidth;
    gridDepth = header.gridDepth;
    physicsMode = header.physicsMode;

    // Transform round-trip.
    center = { header.posX, header.posY, header.posZ };
    size   = { header.sizeX, header.sizeY, header.sizeZ };
    rotation = { header.rotX, header.rotY, header.rotZ };

    layers.clear();
    for (uint32_t i = 0; i < header.layerCount; i++) {
        TerrainLayer layer;
        if (!ReadLayerData(in, layer)) return false;
        layers.push_back(layer);
    }

    InitializeChunks();

    for (uint32_t i = 0; i < header.chunkCount; i++) {
        TerrainChunk* chunk = nullptr;
        // Reading is done in file order; match chunks by (gx, gz).
        std::streampos pos = in.tellg();
        // Peek the chunk header to find which (gx, gz) it belongs to.
        TerrainChunkHeader ch;
        in.read(reinterpret_cast<char*>(&ch), sizeof(ch));
        if (!in) return false;
        int gx = (int)ch.chunkX;
        int gz = (int)ch.chunkZ;
        if (gx >= 0 && gx < gridWidth && gz >= 0 && gz < gridDepth) {
            chunk = &chunks[gz * gridWidth + gx];
        }
        in.seekg(pos); // rewind to start of this chunk record
        if (chunk) {
            if (!ReadChunkData(in, *chunk)) return false;
        } else {
            // Skip unknown chunk record.
            TerrainChunk tmp;
            if (!ReadChunkData(in, tmp)) return false;
        }
    }

    needsFullRebuild = true;
    needsPhysicsRebuild = true;
    return in.good();
}

bool Terrain::SaveToFile(const std::string& path) const {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    return WriteToStream(file);
}

bool Terrain::LoadFromFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    return ReadFromStream(file);
}

void Terrain::QuantizeHeightmap(const std::vector<float>& src, std::vector<uint16_t>& dst, float minH, float maxH) const {
    dst.resize(src.size());
    float range = maxH - minH;
    if (range <= 0.0f) range = 1.0f;
    
    for (size_t i = 0; i < src.size(); i++) {
        float normalized = (src[i] - minH) / range;
        normalized = std::clamp(normalized, 0.0f, 1.0f);
        dst[i] = (uint16_t)(normalized * 65535.0f);
    }
}

void Terrain::DequantizeHeightmap(const std::vector<uint16_t>& src, std::vector<float>& dst, float minH, float maxH) const {
    dst.resize(src.size());
    float range = maxH - minH;
    
    for (size_t i = 0; i < src.size(); i++) {
        float normalized = src[i] / 65535.0f;
        dst[i] = minH + normalized * range;
    }
}

// ============================================================================
// Shader
// ============================================================================

void Terrain::LoadTerrainShader() {
    if (shaderLoaded) return;
    
    // Load from files (will create default if not found)
    terrainShader = LoadShader("shaders/terrain.vert", "shaders/terrain.frag");
    if (terrainShader.id == 0) {
        // Fallback: create a simple default shader
        terrainShader = LoadShaderFromMemory(
            "#version 330\n"
            "in vec3 vertexPosition;\n"
            "in vec3 vertexNormal;\n"
            "in vec2 vertexTexCoord;\n"
            "uniform mat4 mvp;\n"
            "out vec2 texCoord;\n"
            "out vec3 normal;\n"
            "void main() {\n"
            "    texCoord = vertexTexCoord;\n"
            "    normal = vertexNormal;\n"
            "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
            "}\n",
            "#version 330\n"
            "in vec2 texCoord;\n"
            "in vec3 normal;\n"
            "out vec4 fragColor;\n"
            "uniform sampler2D texture0;\n"
            "void main() {\n"
            "    vec3 color = texture(texture0, texCoord).rgb;\n"
            "    float diff = max(dot(normalize(normal), vec3(0.5, 1.0, 0.3)), 0.0);\n"
            "    fragColor = vec4(color * diff, 1.0);\n"
            "}\n"
        );
    }
    shaderLoaded = true;
    CacheShaderUniforms();
}

void Terrain::CacheShaderUniforms() {
    shaderLocs[0] = GetShaderLocation(terrainShader, "viewProj");
    shaderLocs[1] = GetShaderLocation(terrainShader, "model");
    shaderLocs[2] = GetShaderLocation(terrainShader, "minHeight");
    shaderLocs[3] = GetShaderLocation(terrainShader, "maxHeight");
    shaderLocs[4] = GetShaderLocation(terrainShader, "layerCount");
    shaderLocs[5] = GetShaderLocation(terrainShader, "albedoTex[0]");
    shaderLocs[6] = GetShaderLocation(terrainShader, "normalTex[0]");
    shaderLocs[7] = GetShaderLocation(terrainShader, "roughnessTex[0]");
    shaderLocs[8] = GetShaderLocation(terrainShader, "tileSize[0]");
    shaderLocs[9] = GetShaderLocation(terrainShader, "splatmap");
    shaderLocs[10] = GetShaderLocation(terrainShader, "cameraPos");
    shaderLocs[11] = GetShaderLocation(terrainShader, "lightDir");
    shaderLocs[12] = GetShaderLocation(terrainShader, "lightColor");
    shaderLocs[13] = GetShaderLocation(terrainShader, "ambientColor");
    shaderLocs[14] = GetShaderLocation(terrainShader, "fogDensity");
    shaderLocs[15] = GetShaderLocation(terrainShader, "fogColor");
    // ... more as needed
}

void Terrain::UpdateShaderUniforms(const Camera3D& camera) {
    // Compute view and projection matrices manually
    Matrix view = MatrixLookAt(camera.position, camera.target, camera.up);
    Matrix proj = MatrixPerspective(camera.fovy * DEG2RAD, 16.0f/9.0f, 0.01f, 10000.0f);
    Matrix viewProj = MatrixMultiply(view, proj);
    SetShaderValueMatrix(terrainShader, shaderLocs[0], viewProj);
    SetShaderValue(terrainShader, shaderLocs[2], &minHeight, SHADER_UNIFORM_FLOAT);
    SetShaderValue(terrainShader, shaderLocs[3], &maxHeight, SHADER_UNIFORM_FLOAT);
    int layerCount = (int)layers.size();
    SetShaderValue(terrainShader, shaderLocs[4], &layerCount, SHADER_UNIFORM_INT);
    Vector3 camPos = camera.position;
    SetShaderValue(terrainShader, shaderLocs[10], &camPos, SHADER_UNIFORM_VEC3);
    
    // Light direction (simple directional)
    Vector3 lightDir = Vector3Normalize({ -0.5f, -1.0f, -0.3f });
    SetShaderValue(terrainShader, shaderLocs[11], &lightDir, SHADER_UNIFORM_VEC3);
    Vector3 lightColor = { 1.0f, 0.95f, 0.8f };
    SetShaderValue(terrainShader, shaderLocs[12], &lightColor, SHADER_UNIFORM_VEC3);
    Vector3 ambient = { 0.2f, 0.2f, 0.25f };
    SetShaderValue(terrainShader, shaderLocs[13], &ambient, SHADER_UNIFORM_VEC3);
    
    // Bind layer textures
    for (size_t i = 0; i < layers.size() && i < 4; i++) {
        const TerrainLayer& layer = layers[i];
        if (layer.albedo.id) {
            SetShaderValueTexture(terrainShader, shaderLocs[5 + i * 4], layer.albedo);
        }
        if (layer.normal.id) {
            SetShaderValueTexture(terrainShader, shaderLocs[6 + i * 4], layer.normal);
        }
        if (layer.roughness.id) {
            SetShaderValueTexture(terrainShader, shaderLocs[7 + i * 4], layer.roughness);
        }
        SetShaderValue(terrainShader, shaderLocs[8 + i * 4], &layer.tileSize, SHADER_UNIFORM_FLOAT);
    }
}

void Terrain::DrawChunk(const TerrainChunk& chunk, const Camera3D& camera) {
    Model* model = (chunk.lod == 0) ? const_cast<Model*>(&chunk.gpuModel) : const_cast<Model*>(&chunk.lodModels[chunk.lod]);
    
    if (model->meshCount == 0) return;
    
    SetShaderValueMatrix(terrainShader, shaderLocs[1], chunk.transform);
    
    // Bind splatmap for this chunk (would need texture array or bind per draw)
    // For now, use first chunk's splatmap or a shared one
    
    DrawModel(*model, {0, 0, 0}, 1.0f, WHITE);
}

// ============================================================================
// Physics Integration (delegated to TerrainPhysics.cpp)
// ============================================================================

void Terrain::CreatePhysicsHeightfield() {
    terrain::CreatePhysicsHeightfield(*this);
}

void Terrain::CreatePhysicsTriangleMesh() {
    terrain::CreatePhysicsTriangleMesh(*this);
}

void Terrain::DestroyPhysics() {
    terrain::DestroyPhysics(*this);
}

void Terrain::UpdatePhysicsTransform() {
    terrain::UpdatePhysicsTransform(*this);
}

void Terrain::SetDrawCamera(Camera3D* cam) {
    g_drawCamera = cam;
}

} // namespace terrain