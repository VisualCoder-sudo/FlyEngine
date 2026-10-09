#pragma once

#include "../Engine/Backend/Entity.hpp"
#include "../Engine/Backend/Frustum.hpp"
#include "raylib.h"
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <cstdint>

class BasicTerrain : public Entity {
public:
    // Order must stay in sync with TerrainToolIconType and the tool-name table
    // in ui.cpp (identity mapping via static_cast<int>, icon kind == enum index).
    enum class Tool { Raise, Lower, Smooth, Flatten, Paint, Erode, Noise, Ramp, None };
    enum class Shape { Circle, Square };

    struct Brush {
        Tool tool = Tool::None;
        Shape shape = Shape::Circle;
        float radius = 25.0f;
        float strength = 40.0f;
        float hardness = 0.5f;
        float targetHeight = 0.0f;
        int paintLayer = 0;      // Which texture layer to paint (0-3)
        bool paintErase = false;  // True = erase mode (decrease selected layer)

        // Noise tool settings (shared brush config, like the others)
        int noiseSeed = 1337;      // Deterministic seed for the Noise brush
        float noiseScale = 40.0f;  // Feature size in meters (larger = broader lumps)
    };

    // Instance registry (like WaterBody::s_instances)
    static const std::vector<BasicTerrain*>& GetInstances() { return s_instances; }

    BasicTerrain(int width = 256, int depth = 256, float scale = 8.0f, float maxHeight = 100.0f, float textureTiling = 32.0f);
    ~BasicTerrain() override;

    // Naming
    const std::string& GetName() const { return name; }
    void SetName(const std::string& n) { name = n; }

    void Update(float dt) override;
    void Draw() override;
    void DrawOverlay3D() override;

    // Frustum culling support
    bool IsVisible(const Frustum& frustum) const override;
    BoundingBox GetCullBounds() const override;

    // Heightmap operations
    bool LoadHeightmap(const std::string& path);
    bool SaveHeightmap(const std::string& path) const;
    void GenerateFlat(float height = 0.0f);

    // Project persistence. Unlike Load/SaveHeightmap (a lossy 8-bit grayscale
    // image meant for export), these round-trip losslessly: the heightmap goes
    // out as raw float32 and the splatmap as raw RGBA8, each deflated. This is
    // the only path a BasicTerrain is reachable from during save/load -- see
    // BasicTerrainIO.cpp and the v18 section in ScenePersistence.cpp.
    bool SaveToFile(const std::string& path) const;
    bool LoadFromFile(const std::string& path);
    // Writes every live BasicTerrain into one sidecar. Returns false only on
    // an I/O failure; a zero-terrain write still succeeds.
    static bool WriteCollection(const std::string& path);
    // Reads a sidecar, invoking makeInstance() once per record to obtain an
    // engine-owned instance to fill in. makeInstance returns the raw pointer it
    // already handed to the engine, matching TerrainRegistry::ReadFile and the
    // city loader. Returns the number loaded, or 0 if the file is
    // missing/corrupt.
    static int ReadCollection(const std::string& path,
                              const std::function<BasicTerrain*()>& makeInstance);
    // Test seam for ReadCollection's header validation; also the single-record
    // parser used by LoadFromFile.
    static bool ReadInstance(std::istream& is, BasicTerrain& t);

    // Reallocates the heightmap and splatmap for new dimensions, discarding any
    // existing GPU state so stale buffers are not aliased by the fresh mesh.
    // Used by the loader, where w/d come from the file rather than the
    // constructor.
    void Reshape(int newWidth, int newDepth, float newScale,
                 float newMinHeight, float newMaxHeight, float newTiling);
    void MarkAllDirty() { meshDirty = true; splatmapDirty = true; paintColorDirty = true; }
    float* GetHeightData() { return heightmap.data(); }
    const float* GetHeightData() const { return heightmap.data(); }
    size_t GetHeightBytes() const { return heightmap.size() * sizeof(float); }
    uint8_t* GetSplatData() { return splatmap.data(); }
    const uint8_t* GetSplatData() const { return splatmap.data(); }
    size_t GetSplatBytes() const { return splatmap.size(); }
    float GetTextureTiling() const { return textureTiling; }

    // Brush operations
    // dt scales Raise/Lower (units per second) so painting is frame-rate
    // independent; one-shot stamps pass dt = 1.
    void ApplyBrush(const Brush& brush, Vector2 worldPos, float dt = 1.0f);
    void StampBrush(const Brush& brush, Vector2 worldPos);   // one-click: gentle bump/dent
    void StampPaintBrush(const Brush& brush, Vector2 worldPos); // one-click paint
    void ApplyPaintBrush(const Brush& brush, Vector2 worldPos, float dt = 1.0f);

    // Grid erosion: thermal (talus relaxation) + hydraulic (rain/flow/sediment)
    // passes over the heightmap. ErodeRegion() applies the settings to the
    // inclusive cell rectangle (used by the Generate tab's "Erode region/all"
    // buttons); RunErosion() is the state-free core the brush blends by weight.
    struct ErosionSettings {
        float thermal = 0.5f;       // amount of talus relaxation per pass
        float hydraulic = 0.8f;     // amount of rain-flow carving per pass
        float talusDeg = 3.0f;      // repose angle in degrees; steeper slopes erode
        float rain = 0.05f;         // rainfall per pass (water added)
        float evaporation = 0.03f;  // fraction of water lost per pass
        float deposit = 0.6f;       // fraction of eroded soil re-deposited (0 = washed away)
        int iterations = 6;         // passes to run per action
    };
    void ErodeRegion(int x0, int z0, int x1, int z1, const ErosionSettings& s, float dt = 1.0f);
    static void RunErosion(float* heights, int w, int d, float cellSize,
                           const ErosionSettings& s, int iterations);

    // Ramp tool: reshapes a band of width `bandWidth` around the segment from
    // start to end so the surface grades linearly from the height at start to
    // the height at end. strength uses the same scale as the brush Strength
    // slider (2..200): higher snaps closer to the target grade per action.
    void ApplyRamp(Vector2 start, Vector2 end, float strength, float bandWidth);
    float GetHeightAt(float x, float z) const;
    Vector3 GetNormalAt(float x, float z) const;
    bool Raycast(const Ray& ray, float* outDistance, Vector3* outHitPoint, Vector3* outNormal) const;

    // Procedural generation: fills the region defined by genBoxPos/genBoxSize
    // with hills and/or mountains (blended smoothly into the surrounding
    // terrain via a falloff band at the box edges, so there's no hard seam),
    // and optionally auto-paints textures by height/slope. genBoxPos.y/size.y
    // set the vertical base and amplitude of the generated terrain. Position
    // and size are edited directly by the Generate panel (drag sliders) and
    // previewed as a wireframe box in DrawOverlay3D() while showGenBox is set.
    void GenerateTerrainInBox();

    Vector3 genBoxPos = { 0.0f, 30.0f, 0.0f };
    Vector3 genBoxSize = { 100.0f, 60.0f, 100.0f };
    int genSeed = 1337;
    bool genHills = true;
    bool genMountains = false;
    bool genTextures = true;
    bool genBoxInitialized = false; // lets the UI snap the box to the terrain footprint once, on first open
    bool showGenBox = false;        // set by the UI while the Generate tab is open

    // Editor
    bool isSelected = false;
    bool editActive = false;      // true while player is actively editing this terrain
    bool showWireframe = false;

    // Shared brush configuration for the always-visible terrain panel: tools
    // can be set up before any terrain exists and apply to every terrain
    // (Roblox-style global tool settings).
    static Brush& GetBrush() { return s_brush; }

    // Texture paint layer registry (shared across all BasicTerrain instances)
    static void LoadTerrainTextures();
    static int GetLayerCount() { return s_layerCount; }
    static Texture2D* GetLayerTextures() { return s_layerTextures; }
    static const std::string* GetLayerNames() { return s_layerNames; }
    Texture2D GetSplatmapTexture() const { return splatmapTexture; }

    int GetWidth() const { return width; }
    int GetDepth() const { return depth; }
    int GetSplatWidth() const { return splatWidth; }
    int GetSplatDepth() const { return splatDepth; }
    float GetScale() const { return scale; }
    float GetMinHeight() const { return minHeight; }
    float GetMaxHeight() const { return maxHeight; }

    // Engine provides the active 3D camera so the terrain can pick the mouse ray
    static void SetActiveCamera(Camera3D* cam) { s_activeCamera = cam; }

    // The terrain currently being edited (drives the terrain tool panel)
    static BasicTerrain* GetActive() { return s_active; }
    static void SetActive(BasicTerrain* t) { s_active = t; }
    static void SetEditorActive(bool active) { s_editorActive = active; }
    static bool IsEditorActive() { return s_editorActive; }

private:
    static Camera3D* s_activeCamera;
    static BasicTerrain* s_active;
    static Brush s_brush;
    static bool s_editorActive;
    static std::vector<BasicTerrain*> s_instances;
    std::string name = "Terrain";

    void RebuildMesh();
    // Adaptive terrain mesh: collapses locally-flat regions of the heightmap
    // into single quads (2 triangles) instead of their full per-vertex grid,
    // so e.g. a flat plain renders as just 2 triangles while hills keep full
    // detail. BuildDenseMesh() is the original always-full-resolution
    // generator, kept as the guaranteed-safe fallback (see RebuildMesh()).
    bool BuildAdaptiveMesh();
    void BuildDenseMesh();
    bool IsBlockFlat(int x0, int z0, int x1, int z1) const;
    void CollectAdaptiveBlock(int x0, int z0, int x1, int z1,
                               std::vector<float>& verts, std::vector<float>& normals,
                               std::vector<float>& uvs, std::vector<unsigned short>& indices) const;
    Vector3 ComputeGridNormal(int x, int z) const;
    void UpdateSplatmapTexture();
    void UpdatePaintColorTexture();
    float HeightLocal(float lx, float lz) const;
    // Auto-textures the given world-space XZ rectangle by height/slope after
    // GenerateTerrainInBox() has written new heights there.
    void AutoPaintByHeightSlope(float minWX, float maxWX, float minWZ, float maxWZ);

    int width;
    int depth;
    float scale;
    float maxHeight;
    float minHeight = -50.0f;
    float textureTiling = 32.0f;  // UV repeat count across terrain

    // Splatmap is stored at a higher pixel resolution than the heightmap grid
    // so painted layer blends look smooth rather than following the (much
    // coarser) mesh vertex spacing. Independent of width/depth; the shader
    // already samples it via terrain-normalized UV (splatUV = texCoord /
    // textureTiling), so this purely adds paint detail. Capped to keep memory
    // reasonable on large terrains.
    static constexpr int kSplatSupersample = 4;
    static constexpr int kSplatMaxDim = 2048;
    int splatWidth;
    int splatDepth;

public:
    Vector3 position = {0, 0, 0};

private:
    std::vector<float> heightmap;

    Mesh mesh = {0};
    Model model = {0};
    Texture2D terrainTexture = {0};
    bool meshDirty = true;

    // The material shader raylib assigns by default in LoadModelFromMesh().
    // Cached so Draw() can swap back to it for the flat-color fallback path
    // without clobbering the shared s_terrainShader.
    Shader defaultMaterialShader = {0};

    // Brush preview
    Vector2 brushWorldPos = {0, 0};
    bool brushValid = false;

    // Ramp tool drag state (start captured on press, end follows the cursor)
    Vector2 rampStart = {0, 0};
    bool rampDragging = false;

    // Splatmap data (RGBA per vertex, max 4 layers)
    std::vector<uint8_t> splatmap;
    Texture2D splatmapTexture = {0};
    bool splatmapDirty = true;

    // Simple color-blend paint texture rendered through the DEFAULT shader as the
    // material albedo map (guaranteed-visible basic paint tool, no custom shader).
    Texture2D paintColorTexture = {0};
    bool paintColorDirty = true;

    // Shader for splatmap blending (shared across all instances)
    static Shader s_terrainShader;
    static int s_shaderSplatmapLoc;
    static int s_shaderAlbedoLocs[4];
    static int s_shaderLayerCountLoc;
    static int s_shaderLightDirLoc;
    static int s_shaderLightColorLoc;
    static int s_shaderAmbientColorLoc;
    static int s_shaderFogDensityLoc;
    static int s_shaderFogColorLoc;
    static int s_shaderTextureTilingLoc;

    // Static layer registry (shared across all BasicTerrain instances)
    static Texture2D s_layerTextures[4];
    static std::string s_layerNames[4];
    static int s_layerCount;

    // Solid colors used by the simple color paint tool (one per layer).
    static Color s_layerColors[4];
};