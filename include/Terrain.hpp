#pragma once

#include "Entity.hpp"
#include "TerrainTypes.hpp"
#include "raylib.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <algorithm>

// Avoid Windows min/max macros
#undef min
#undef max

namespace phys { class Simulation; }

namespace terrain {

// Forward declare terrain module classes for friend access
class TerrainSculpt;
class TerrainPhysics;
class TerrainMesh;
class TerrainIO;

class Terrain : public Entity {
public:
    // Construction
    Terrain(Vector3 center, float width, float depth, int chunkSize = 256, int chunkResolution = 65);
    ~Terrain() override;

    // Entity interface
    void Update(float dt) override;
    void Draw() override;
    void DrawOverlay3D() override;

    // Heightmap operations
    bool LoadHeightmap(const std::string& path, const HeightmapImportSettings& settings = {});
    bool SaveHeightmap(const std::string& path) const;
    void GenerateFromNoise(const NoiseParams& params);
    void CreateBlank(float defaultHeight = 0.0f);

    // Sculpting (world-space coordinates)
    void RaiseTerrain(Vector2 center, float radius, float strength);
    void LowerTerrain(Vector2 center, float radius, float strength);
    void SmoothTerrain(Vector2 center, float radius, float strength);
    void FlattenTerrain(Vector2 center, float radius, float targetHeight);
    void RampTerrain(Vector2 start, Vector2 end, float startHeight, float endHeight);
    void NoiseTerrain(Vector2 center, float radius, const NoiseParams& params);

    // Generic brush application
    void ApplyBrush(const TerrainBrush& brush, Vector2 center);

    // Query
    float GetHeightAt(float x, float z) const;
    Vector3 GetNormalAt(float x, float z) const;
    bool Raycast(const Ray& ray, float* outDistance, Vector3* outHitPoint, Vector3* outNormal) const;

    // Chunk management
    void UpdateChunks(const Camera3D& camera);
    void RebuildDirtyChunks();
    void RebuildAllChunks();
    void UploadDirtyChunks();

    // Materials
    void AddLayer(const TerrainLayer& layer);
    void RemoveLayer(int index);
    void SetLayer(int index, const TerrainLayer& layer);
    int GetLayerCount() const { return (int)layers.size(); }
    const TerrainLayer& GetLayer(int index) const { return layers[index]; }
    TerrainLayer& GetLayer(int index) { return layers[index]; }
    void PaintLayer(Vector2 center, float radius, float strength, int layerIndex, bool erase = false);

    // Reload the material albedo/normal/roughness textures for every layer from
    // the paths stored in the terrain file. `baseDir` resolves project-relative
    // paths. Safe to call after LoadFromFile/ReadFromStream.
    void ReloadMaterialTextures(const std::string& baseDir = "");

    // Undo/Redo
    bool Undo();
    bool Redo();
    void ClearUndoRedo();

    // Physics
    void SetPhysicsMode(PhysicsMode mode);
    PhysicsMode GetPhysicsMode() const { return physicsMode; }
    void RebuildPhysics(phys::Simulation* sim = nullptr);
    void SetPhysicsSimulation(phys::Simulation* sim) { physicsSim = sim; }

    // Serialization
    bool SaveToFile(const std::string& path) const;
    bool LoadFromFile(const std::string& path);

    // Full per-terrain block payload (transform + layers + chunks) written to /
    // read from a stream. The registry wraps these in TERRAIN# blocks inside the
    // single terrain.terrain file.
    bool WriteToStream(std::ostream& out) const;
    bool ReadFromStream(std::istream& in);

    // Transform interface (for gizmo) — size is authoritative
    // size.x = width, size.z = depth, size.y = vertical scale (unused)
    Vector3* GetPosPtr() { return &center; }
    Vector3* GetSizePtr() { return &size; }
    Vector3* GetRotationPtr() { return &rotation; }
    Vector3* GetOriginPtr() { return &origin; }
    Vector3 GetOriginWorld() const { return center; }

    // Properties
    const std::string& GetName() const { return name; }
    void SetName(const std::string& n) { name = n; }
    int GetChunkCount() const { return gridWidth * gridDepth; }
    int GetGridWidth() const { return gridWidth; }
    int GetGridDepth() const { return gridDepth; }
    float GetWorldWidth() const { return size.x; }
    float GetWorldDepth() const { return size.z; }
    float GetMinHeight() const { return minHeight; }
    float GetMaxHeight() const { return maxHeight; }
    int GetChunkSize() const { return (int)chunkWorldSize; }
    int GetChunkResolution() const { return chunkResolution; }

    // Draw camera for brush preview
    static void SetDrawCamera(Camera3D* cam);

    // Editor
    bool isSelected = false;
    bool showWireframe = false;
    bool showChunkBounds = false;
    bool showLODColors = false;

    // Callbacks
    std::function<void()> onHeightmapChanged;
    std::function<void()> onPhysicsRebuilt;

// Friend declarations for terrain module accessor classes
    friend class TerrainSculptAccess;
    friend class TerrainPhysicsAccess;
    friend class TerrainMeshAccess;
    friend class TerrainIOAccess;

public:
    // Core data (accessible by terrain modules)
    Vector3 center = {0, 0, 0};
    Vector3 size = {1000, 1, 1000};        // x=width, z=depth
    Vector3 rotation = {0, 0, 0};
    Vector3 origin = {0, 0, 0};
    std::string name = "Terrain";

    // Chunk grid
    int gridWidth = 0;
    int gridDepth = 0;
    float chunkWorldSize = 256.0f;
    int chunkResolution = 65;               // Vertices per edge (power of 2 + 1)
    std::vector<TerrainChunk> chunks;

    // LOD
    LODConfig lodConfig;

    // Materials
    std::vector<TerrainLayer> layers;
    Shader terrainShader = {0};
    bool shaderLoaded = false;
    int shaderLocs[32] = {-1};              // Cached uniform locations

    // Physics
    PhysicsMode physicsMode = PhysicsMode::Heightfield;
    phys::Simulation* physicsSim = nullptr;
    void* physicsBody = nullptr;

    // Height range (for quantization)
    float minHeight = 0.0f;
    float maxHeight = 200.0f;

    // Rebuild flags
    bool needsFullRebuild = false;
    bool needsPhysicsRebuild = false;

    // Sculpting undo/redo
    struct UndoEntry {
        int chunkIndex = -1;
        std::vector<float> previousHeightmap;
        std::vector<uint8_t> previousSplatmap;
    };
    std::vector<UndoEntry> undoStack;
    std::vector<UndoEntry> redoStack;
    static constexpr int MAX_UNDO_ENTRIES = 50;

    // Public methods (accessible by terrain modules)
    void InitializeChunks();
    void CreateChunk(int gx, int gz);
    void DestroyChunks();
    TerrainChunk* GetChunkAt(float worldX, float worldZ);
    const TerrainChunk* GetChunkAt(float worldX, float worldZ) const;
    std::vector<TerrainChunk*> GetChunksInRadius(Vector2 center, float radius);
    std::vector<const TerrainChunk*> GetChunksInRadius(Vector2 center, float radius) const;

    void RebuildChunkMesh(TerrainChunk& chunk, int lod = 0);
    void RebuildChunkSplatmap(TerrainChunk& chunk);
    void UploadChunk(TerrainChunk& chunk);
    void UpdateChunkLOD(TerrainChunk& chunk, float cameraDistance);

    void MarkChunkDirty(int gx, int gz);
    void MarkChunkDirty(TerrainChunk* chunk);
    void MarkAllChunksDirty();

    void PushUndo(int chunkIndex);
    void ClearRedo();

    void LoadTerrainShader();
    void CacheShaderUniforms();
    void UpdateShaderUniforms(const Camera3D& camera);
    void DrawChunk(const TerrainChunk& chunk, const Camera3D& camera);

    // Physics integration
    void CreatePhysicsHeightfield();
    void CreatePhysicsTriangleMesh();
    void DestroyPhysics();
    void UpdatePhysicsTransform();

    // Serialization helpers
    bool WriteChunkData(std::ostream& out, const TerrainChunk& chunk) const;
    bool ReadChunkData(std::istream& in, TerrainChunk& chunk);
    void QuantizeHeightmap(const std::vector<float>& src, std::vector<uint16_t>& dst, float minH, float maxH) const;
    void DequantizeHeightmap(const std::vector<uint16_t>& src, std::vector<float>& dst, float minH, float maxH) const;
    bool WriteLayerData(std::ostream& out, const TerrainLayer& layer) const;
    bool ReadLayerData(std::istream& in, TerrainLayer& layer);
};

} // namespace terrain

// Accessor classes for terrain module private member access
namespace terrain {

class TerrainSculptAccess {
public:
    static void PushUndo(Terrain& terrain, int chunkIndex) { terrain.PushUndo(chunkIndex); }
    static void ClearRedo(Terrain& terrain) { terrain.ClearRedo(); }
    static std::vector<TerrainChunk>& GetChunks(Terrain& terrain) { return terrain.chunks; }
    static std::vector<TerrainChunk*> GetChunksInRadius(Terrain& terrain, Vector2 center, float radius) { return terrain.GetChunksInRadius(center, radius); }
    static std::vector<const TerrainChunk*> GetChunksInRadiusConst(const Terrain& terrain, Vector2 center, float radius) { return terrain.GetChunksInRadius(center, radius); }
    static float& MinHeight(Terrain& terrain) { return terrain.minHeight; }
    static float& MaxHeight(Terrain& terrain) { return terrain.maxHeight; }
    static bool& NeedsPhysicsRebuild(Terrain& terrain) { return terrain.needsPhysicsRebuild; }
    static std::function<void()>& OnHeightmapChanged(Terrain& terrain) { return terrain.onHeightmapChanged; }
    static std::vector<Terrain::UndoEntry>& UndoStack(Terrain& terrain) { return terrain.undoStack; }
    static std::vector<Terrain::UndoEntry>& RedoStack(Terrain& terrain) { return terrain.redoStack; }
    static void ClearUndoRedo(Terrain& terrain) { terrain.undoStack.clear(); terrain.redoStack.clear(); }
    static void MarkAllChunksDirty(Terrain& terrain) { terrain.MarkAllChunksDirty(); }
};

class TerrainPhysicsAccess {
public:
    static int GridWidth(Terrain& terrain) { return terrain.gridWidth; }
    static int GridDepth(Terrain& terrain) { return terrain.gridDepth; }
    static int ChunkResolution(Terrain& terrain) { return terrain.chunkResolution; }
    static float ChunkWorldSize(Terrain& terrain) { return terrain.chunkWorldSize; }
    static float MinHeight(Terrain& terrain) { return terrain.minHeight; }
    static float MaxHeight(Terrain& terrain) { return terrain.maxHeight; }
    static std::vector<TerrainChunk>& Chunks(Terrain& terrain) { return terrain.chunks; }
    static float MinHeightVal(Terrain& terrain) { return terrain.minHeight; }
    static float MaxHeightVal(Terrain& terrain) { return terrain.maxHeight; }
    static void*& PhysicsBody(Terrain& terrain) { return terrain.physicsBody; }
    static phys::Simulation*& PhysicsSim(Terrain& terrain) { return terrain.physicsSim; }
    static Vector3& Center(Terrain& terrain) { return terrain.center; }
    static Vector3& Rotation(Terrain& terrain) { return terrain.rotation; }
};

class TerrainMeshAccess {
public:
    static std::vector<TerrainChunk>& Chunks(Terrain& terrain) { return terrain.chunks; }
    static int GridWidth(Terrain& terrain) { return terrain.gridWidth; }
    static int GridDepth(Terrain& terrain) { return terrain.gridDepth; }
    static float ChunkWorldSize(Terrain& terrain) { return terrain.chunkWorldSize; }
};

class TerrainIOAccess {
public:
    static std::vector<TerrainChunk>& Chunks(Terrain& terrain) { return terrain.chunks; }
    static int GridWidth(Terrain& terrain) { return terrain.gridWidth; }
    static int GridDepth(Terrain& terrain) { return terrain.gridDepth; }
    static int ChunkResolution(Terrain& terrain) { return terrain.chunkResolution; }
    static float ChunkWorldSize(Terrain& terrain) { return terrain.chunkWorldSize; }
    static float MinHeight(Terrain& terrain) { return terrain.minHeight; }
    static float MaxHeight(Terrain& terrain) { return terrain.maxHeight; }
    static float& MinHeightRef(Terrain& terrain) { return terrain.minHeight; }
    static float& MaxHeightRef(Terrain& terrain) { return terrain.maxHeight; }
    static Vector3& Size(Terrain& terrain) { return terrain.size; }
    static void InitializeChunks(Terrain& terrain) { terrain.InitializeChunks(); }
    static bool& NeedsFullRebuild(Terrain& terrain) { return terrain.needsFullRebuild; }
    static bool& NeedsPhysicsRebuild(Terrain& terrain) { return terrain.needsPhysicsRebuild; }
};

} // namespace terrain