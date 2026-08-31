#pragma once

#include "Entity.hpp"
#include "raylib.h"
#include "raymath.h"
#include <vector>
#include <string>
#include <unordered_map>

class Engine;

class WaterBody : public Entity {
public:
    struct NoiseParams {
        float amplitude = 0.5f;
        float frequency = 0.1f;
        float speed = 0.3f;
        Vector2 direction = { 1.0f, 0.0f };
        int octaves = 4;
        float persistence = 0.5f;
        float lacunarity = 2.0f;
        int seed = 1337;
    };

    struct FoamParams {
        float intensity = 0.5f;
        float scale = 4.0f;
        float threshold = 0.75f;
        Color color = WHITE;
    };

    struct GridParams {
        int baseResolution = 64;
        int maxResolution = 256;
        float densityThreshold = 0.5f;
        bool adaptive = true;
    };

    struct Chunk {
        int gridX = 0, gridZ = 0;
        Mesh mesh = { 0 };
        Model model = { 0 };
        int currentLod = -1;
        float fade = 0.0f;
    };

    static constexpr float CHUNK_SIZE = 20.0f;
    static constexpr float MAX_RENDER_DISTANCE = 350.0f;
    static constexpr float HORIZON_DISTANCE = 550.0f;
    static constexpr float FADE_SPEED = 4.0f;
    static constexpr int LOD_COUNT = 5;
    static constexpr int LOD_RESOLUTIONS[LOD_COUNT] = { 0, 4, 8, 16, 32 };
    static constexpr float LOD_DISTANCES[LOD_COUNT] = { 350.0f, 250.0f, 150.0f, 70.0f };

    WaterBody(Vector3 position, Vector3 size, float waterHeight, Color baseColor = { 30, 190, 220, 160 });
    ~WaterBody() override;

    void Update(float dt) override;
    void Draw() override;
    void DrawOverlay3D() override;

    bool IsTransparent() const override { return true; }

    // Transform interface (for gizmo)
    Vector3* GetPosPtr() { return &position; }
    Vector3* GetSizePtr() { return &size; }
    Vector3* GetRotationPtr() { return &rotation; }
    Vector3* GetOriginPtr() { return &origin; }
    Vector3 GetOriginWorld() const { return position; }

    // Properties
    const std::string& GetName() const { return name; }
    void SetName(const std::string& n) { name = n; }

    float GetWaterHeight() const { return waterHeight; }
    void SetWaterHeight(float h) { waterHeight = h; MarkMeshDirty(); }

    const Color& GetBaseColor() const { return baseColor; }
    void SetBaseColor(Color c) { baseColor = c; }

    float GetTransparency() const { return transparency; }
    void SetTransparency(float t) { transparency = Clamp(t, 0.0f, 1.0f); }

    const NoiseParams& GetNoiseParams() const { return noise; }
    void SetNoiseParams(const NoiseParams& p) { noise = p; MarkMeshDirty(); }

    const FoamParams& GetFoamParams() const { return foam; }
    void SetFoamParams(const FoamParams& p) { foam = p; }

    const GridParams& GetGridParams() const { return grid; }
    void SetGridParams(const GridParams& p) { grid = p; MarkMeshDirty(); }

    // CPU-side height query (for physics/buoyancy)
    float GetHeightAt(float x, float z) const;

    // Bounds
    BoundingBox GetBoundingBox() const;
    bool IntersectsXZ(const BoundingBox& box) const;

    // Mesh management
    void RebuildMesh();
    void MarkMeshDirty() { meshDirty = true; }

    // Serialization
    bool SaveToFile(const std::string& path) const;
    bool LoadFromFile(const std::string& path);

    // Editor
    bool isSelected = false;
    bool showWireframe = false;
    bool showGrid = false;

    // Live registry of all WaterBody instances (for explorer UI)
    static const std::vector<WaterBody*>& GetInstances() { return s_instances; }

    // Camera for shader (set by Engine before draw pass)
    static void SetActiveCamera(Camera3D* cam) { s_activeCamera = cam; }
    static void SetActiveEngine(Engine* eng) { s_activeEngine = eng; }

    // Shader data structure (must match GLSL)
    struct ShaderData {
        Vector3 position;
        float waterHeight;
        Vector3 size;
        float _pad0;
        Vector4 baseColor;           // rgba normalized
        Vector4 noiseParams1;        // amplitude, frequency, speed, octaves
        Vector4 noiseParams2;        // persistence, lacunarity, seed, time
        Vector2 noiseDirection;
        Vector2 _pad1;
        Vector4 foamParams;          // intensity, scale, threshold, _pad
        Vector3 foamColor;
        float _pad2;
        int gridBaseRes;
        int gridMaxRes;
        float densityThreshold;
        int adaptive;
    };

    WaterBody::ShaderData GetShaderData(float globalTime) const;

private:
    void InitializeShader();
    void UpdateChunks(const Camera3D& camera);
    void BuildChunkMesh(Chunk& chunk, int resolution, float worldMinX, float worldMaxX, float worldMinZ, float worldMaxZ);
    void ReleaseChunkResources(Chunk& chunk);
    int GetLodForDistance(float dist) const;
    void UpdateShaderUniforms(const Camera3D& camera, float globalTime);
    void ReleaseGpuResources();
    static int64_t ChunkKey(int gx, int gz);

    // Core data
    Vector3 position = { 0, 0, 0 };
    Vector3 size = { 100, 1, 100 };      // x=width, z=depth
    Vector3 rotation = { 0, 0, 0 };
    Vector3 origin = { 0, 0, 0 };
    float waterHeight = 0.0f;
    Color baseColor = { 0, 100, 200, 180 };
    float transparency = 0.3f;
    std::string name = "WaterBody";

    NoiseParams noise;
    FoamParams foam;
    GridParams grid;

    // Rendering
    Shader shader = { 0 };
    bool shaderLoaded = false;
    bool customShader = false;
    std::unordered_map<int64_t, Chunk> chunks;
    bool meshDirty = true;

    // Shader uniform locations
    int wModelLoc = -1;
    int wViewLoc = -1;
    int wProjLoc = -1;
    int cameraPosLoc = -1;
    int globalTimeLoc = -1;
    int permLoc = -1;
    int waterPosLoc = -1;
    int waterHeightLoc = -1;
    int waterSizeLoc = -1;
    int baseColorLoc = -1;
    int noiseParams1Loc = -1;
    int noiseParams2Loc = -1;
    int noiseDirectionLoc = -1;
    int foamParamsLoc = -1;
    int foamColorLoc = -1;
    int chunkFadeLoc = -1;
    int objectCountLoc = -1;
    int objectPositionsLoc = -1;

    // Camera for shader
    static Camera3D* s_activeCamera;
    static Engine* s_activeEngine;
    static std::vector<WaterBody*> s_instances;
};