#pragma once

#include "../../Engine/Backend/Entity.hpp"
#include "../../Engine/Backend/Frustum.hpp"
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

    struct DetailParams {
        float intensity = 0.05f;
        float scale = 12.0f;
        float speed = 1.0f;
        float _pad = 0.0f;
    };

    struct ReflectionParams {
        float strength = 0.55f;
        float distortion = 1.0f;
        float distanceFade = 0.4f;
        float _pad = 0.0f;
    };

    struct GridParams {
        int baseResolution = 64;
        int maxResolution = 256;
        float densityThreshold = 0.5f;
        bool adaptive = true;
    };

    struct Chunk {
        int gridX = 0, gridZ = 0;
        float cellSize = 0.0f; // meters per cell (near disk)
        Mesh mesh = { 0 };
        Model model = { 0 };
        int currentLod = -1;
        float fade = 0.0f;
        float lodDist = 0.0f; // camera distance at last LOD swap (hysteresis anchor)
    };

    static constexpr float CHUNK_SIZE = 20.0f;
    static constexpr float MAX_RENDER_DISTANCE = 350.0f;
    static constexpr float HORIZON_DISTANCE = 600.0f;
    static constexpr float FADE_SPEED = 4.0f;
    static constexpr int LOD_COUNT = 5;
    static constexpr int LOD_RESOLUTIONS[LOD_COUNT] = { 0, 4, 8, 16, 32 };
    static constexpr float LOD_DISTANCES[LOD_COUNT] = { 350.0f, 250.0f, 150.0f, 70.0f };

    WaterBody(Vector3 position, Vector3 size, float waterHeight, Color baseColor = { 30, 190, 220, 160 });
    ~WaterBody() override;

    void Update(float dt) override;
    void Draw() override;
    void DrawOverlay3D() override;

    // Frustum culling support
    bool IsVisible(const Frustum& frustum) const override;
    BoundingBox GetCullBounds() const override { return GetBoundingBox(); }

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

    const DetailParams& GetDetailParams() const { return detail; }
    void SetDetailParams(const DetailParams& p) { detail = p; }

    const ReflectionParams& GetReflectionParams() const { return reflection; }
    void SetReflectionParams(const ReflectionParams& p) { reflection = p; }

    const GridParams& GetGridParams() const { return grid; }
    void SetGridParams(const GridParams& p) { grid = p; MarkMeshDirty(); }

    // CPU-side height query (for physics/buoyancy). Includes the dynamic
    // ripple layer (boat wakes, splashes) on top of the procedural waves.
    // rippleWeight scales the wake/splash layer (physics uses < 1 so a body's own
    // wake can't feed back into its buoyancy).
    float GetHeightAt(float x, float z, float rippleWeight = 1.0f) const;

    // --- Dynamic disturbance layer (wakes, splashes, foam trails) ----------
    // A camera-centred 2D wave-equation grid layered on the noise waves. It is
    // only simulated while something is disturbing it and costs nothing idle.
    struct RippleParams {
        bool enabled = true;
        float waveSpeed = 3.2f;      // m/s ripples travel
        float damping = 0.992f;      // per 60 Hz step; lower = ripples die sooner
        float wakeStrength = 1.0f;   // wake size multiplier (1 = default)
        float splashStrength = 0.07f;// crater depth per m/s of impact speed
        float foamLifetime = 5.0f;   // seconds for a foam trail to fade
        float maxDisplacement = 1.2f;// clamp on ripple height (m)
    };
    const RippleParams& GetRippleParams() const { return ripple; }
    void SetRippleParams(const RippleParams& p) { ripple = p; }

    // Call every physics step for a body touching the water. `id` identifies
    // the body so its path can be interpolated (no gaps at high speed).
    void AddBodyWake(const void* id, Vector3 worldPos, Vector3 velocity,
                     float radius, float submergedFraction, float dt);
    // A body hitting the surface: crater + rebound ring + foam burst.
    void AddSplash(Vector3 worldPos, float impactSpeed, float radius);
    // Ripple layer height only (no noise waves).
    float GetRippleHeightAt(float x, float z) const;

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

    // Diagnostics for load/coverage audit (stress harness + editor debug).
    struct DebugStats {
        int totalChunks = 0;
        int coarseChunkCount = 0;
        int lodCounts[LOD_COUNT] = {};
        bool farShellBuilt = false;
        int farShellResolution = 0;
        int farShellVertCount = 0;
        float horizonDistance = 0.0f;
    };
    DebugStats GetDebugStats() const;

    // Diagnostic: writes an ASCII XZ coverage map over the whole body using the
    // exact Draw() rules (chunk presence + behind-camera cull + far-shell rim
    // clip). '.' = void (no water drawn), 'v' = chunk exists but is culled by
    // the behind-camera dot test, 'C' = chunk drawn, 'S' = shell only,
    // '+' = chunk + shell. cellsPerAxis samples across the body.
    void WriteCoverageMap(const Camera3D& camera, const char* path, int cellsPerAxis) const;
    bool DebugHasChunkKey(int gx, int gz) const { return chunks.count(ChunkKey(gx, gz)) > 0; }
    int  DebugChunkCount() const { return (int)chunks.size(); }

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
    void BuildChunkMesh(Chunk& chunk, int resolution, float cellSize, float worldMinX, float worldMaxX, float worldMinZ, float worldMaxZ);
    void ReleaseChunkResources(Chunk& chunk);
    int GetLodForDistance(float dist) const;
    void UpdateShaderUniforms(const Camera3D& camera, float globalTime);
    void ReleaseGpuResources();
    void BuildFarShell();

    // Ripple simulation internals
    static constexpr int RIPPLE_N = 256;            // grid cells per axis
    static constexpr float RIPPLE_WINDOW = 128.0f;  // metres covered
    void EnsureRippleGrid();
    void RecentreRipples(float camX, float camZ);
    void StepRipples(float dt);
    void StampGaussian(float wx, float wz, float radius, float dHeight, float dVel, float dFoam);
    void UploadRippleTexture();
    void BindRippleTexture();
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
    DetailParams detail;
    ReflectionParams reflection;
    GridParams grid;

    // Rendering
    Shader shader = { 0 };
    bool shaderLoaded = false;
    bool customShader = false;
    std::unordered_map<int64_t, Chunk> chunks;
    bool meshDirty = true;

    // One full-body mesh covering everything beyond the detailed chunk disk, so
    // km-scale oceans have no unloaded gap in the distance. Vertices are world
    // space (wModel = identity when drawn); the fragment shader clips the shell
    // to fragments outside HORIZON_DISTANCE of the camera (see farRimParams).
    Mesh farShellMesh = { 0 };
    Model farShellModel = { 0 };
    bool farShellBuilt = false;
    Vector3 lastShellSize = { 0, 0, 0 };
    Vector3 lastShellPos = { 0, 0, 0 };

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
    int detailParamsLoc = -1;
    int reflVPLoc = -1;
    int reflTexLoc = -1;
    int reflParamsLoc = -1;
    int chunkFadeLoc = -1;
    int objectCountLoc = -1;
    int objectPositionsLoc = -1;
    int farRimLoc = -1;
    int rippleParamsLoc = -1;
    int rippleTexLoc = -1;    // fragment-stage sampler (rippleTexFS)
    int rippleTexVSLoc = -1;  // vertex-stage sampler (rippleTexVS)

    // Ripple state (lazily allocated on first disturbance)
    RippleParams ripple;
    std::vector<float> rippleH, rippleV, rippleFoam;
    std::vector<unsigned short> rippleHalf;  // RGBA16F upload staging
    float rippleOriginX = 0.0f, rippleOriginZ = 0.0f; // world pos of cell (0,0)
    float rippleAccum = 0.0f;
    float rippleIdleTime = 0.0f;
    bool rippleActive = false;
    bool ripplePendingUpload = false;
    double rippleLastSimTime = -1.0;
    Texture2D rippleTex = { 0 };
    struct WakeTrack { Vector2 last; double time; };
    std::unordered_map<const void*, WakeTrack> wakeTracks;
    static constexpr int RIPPLE_SLOT = 12;

    // Camera for shader
    static Camera3D* s_activeCamera;
    static Engine* s_activeEngine;
    static std::vector<WaterBody*> s_instances;

    // Last camera XZ seen by UpdateChunks, for teleport detection (prewarm).
    Vector2 lastChunkCamXZ = { 1e30f, 1e30f };
};