# Terrain System Plan for Flyengine

## Overview
A chunked, LOD-enabled terrain system supporting large worlds (10-100km²+), heightmap import, sculpting tools, multi-layer texture painting, and configurable physics (heightfield vs triangle mesh). Integrated into the existing Entity/Editor workflow.

---

## 1. Core Architecture

### 1.1 Terrain Entity (Terrain.hpp / Terrain.cpp)
`cpp
class Terrain : public Entity {
public:
    // Construction
    Terrain(Vector3 center, float width, float depth, int chunkSize = 256, int chunkResolution = 65);
    
    // Heightmap operations
    bool LoadHeightmap(const std::string& path);        // PNG, RAW, EXR, TIFF
    bool SaveHeightmap(const std::string& path) const;
    void GenerateFromNoise(NoiseParams params);        // Procedural fallback
    
    // Sculpting (world-space coordinates)
    void RaiseTerrain(Vector2 center, float radius, float strength);
    void LowerTerrain(Vector2 center, float radius, float strength);
    void SmoothTerrain(Vector2 center, float radius, float strength);
    void FlattenTerrain(Vector2 center, float radius, float targetHeight);
    
    // Query
    float GetHeightAt(float x, float z) const;
    Vector3 GetNormalAt(float x, float z) const;
    
    // Chunk management
    void UpdateChunks(const Camera3D& camera);         // LOD/streaming
    void RebuildDirtyChunks();
    
    // Materials
    void AddLayer(const TerrainLayer& layer);
    void PaintLayer(Vector2 center, float radius, float strength, int layerIndex);
    
    // Physics
    void SetPhysicsMode(PhysicsMode mode);             // Heightfield | TriangleMesh
    void RebuildPhysics();
    
    // Transform (for gizmo)
    Vector3* GetPosPtr() { return &center; }
    Vector3* GetSizePtr() { return &size; }            // x=width, z=depth
    Vector3* GetRotationPtr() { return &rotation; }
    Vector3* GetOriginPtr() { return &origin; }
};
`

### 1.2 Data Structures
`cpp
// Heightmap storage: chunked 2D grid of float heights
struct TerrainChunk {
    int lod = 0;                          // 0 = full res, 1 = half, etc.
    bool dirty = true;
    Mesh cpuMesh;                         // CPU-side for editing/physics
    Model gpuModel;                       // GPU upload
    BoundingBox bounds;
    Vector2 chunkCoord;                   // Grid position
};

// Material layer for splatmap painting
struct TerrainLayer {
    std::string name;
    Texture2D albedo;
    Texture2D normal;
    Texture2D roughness;
    float tileSize = 10.0f;               // World units per texture repeat
    float blendRange = 0.1f;              // 0-1 blend falloff
};

// Per-vertex splat weights (4 layers per chunk, packed in RGBA)
struct SplatmapData {
    Image weightMap;                      // RGBA8, same resolution as heightmap
    int layerCount = 0;                   // Active layers (max 4 per chunk)
};
`

### 1.3 Chunking Strategy
| World Size | Chunk Size | Chunk Resolution | Max Chunks |
|------------|------------|------------------|------------|
| 10 km²     | 256m       | 65 (64+1)        | 16x16 = 256 |
| 100 km²    | 512m       | 65               | 20x20 = 400 |

- **Chunk size**: World units per chunk (configurable, 128-1024m)
- **Chunk resolution**: Vertices per edge (65, 129, 257 - power of 2 + 1)
- **LOD**: Geomorphing or discrete LOD (0, 1, 2) based on camera distance
- **Streaming**: Only upload chunks within maxChunkDistance of camera

---

## 2. Heightmap Import Pipeline

### 2.1 Supported Formats
| Format | Bit Depth | Notes |
|--------|-----------|-------|
| PNG    | 8/16-bit  | Most common, lossless |
| RAW    | 16-bit    | Raw height data, no header |
| EXR    | 16/32-bit | HDR, professional pipelines |
| TIFF   | 16-bit    | GIS data, georeferenced |

### 2.2 Import Settings Dialog
`cpp
struct HeightmapImportSettings {
    float worldWidth = 1000.0f;      // World X size
    float worldDepth = 1000.0f;      // World Z size
    float maxHeight = 200.0f;        // Max elevation (meters)
    float minHeight = 0.0f;          // Min elevation
    bool flipY = false;              // Image origin correction
    int targetChunkSize = 256;       // Chunk world size
    int targetResolution = 65;       // Vertices per chunk edge
    bool generateMips = true;        // For LOD
};
`

### 2.3 Import Flow
1. User: Right-click -> Add Object -> **Landscape**
2. Dialog: New blank terrain **OR** Import heightmap
3. If import: File picker -> Import settings dialog -> Process
4. Processing:
   - Load image -> Convert to float heightmap (normalized 0-1)
   - Resample to chunk grid (bicubic)
   - Generate initial CPU meshes for all chunks
   - Upload GPU meshes for visible chunks
   - Build physics collision (async)
   - Create default material layers

---

## 3. Sculpting Tools (Priority 1)

### 3.1 Tool Types
`cpp
enum class TerrainTool {
    Raise,        // Add height
    Lower,        // Subtract height
    Smooth,       // Average neighbors (laplacian)
    Flatten,      // Set to target height
    Ramp,         // Linear gradient between two points
    Noise,        // Add procedural noise
    Erosion       // Thermal/hydraulic (future)
};
`

### 3.2 Brush Parameters
`cpp
struct TerrainBrush {
    TerrainTool tool = TerrainTool::Raise;
    float radius = 10.0f;           // World units
    float strength = 0.1f;          // Per-second for continuous, absolute for single
    float hardness = 0.5f;          // 0=soft, 1=hard edge
    float targetHeight = 0.0f;      // For Flatten
    bool addMode = true;            // Raise vs Lower (toggle)
};
`

### 3.3 Implementation Details
- **World-space brush**: Project mouse ray to terrain, get XZ center
- **Affected chunks**: Find chunks intersecting brush circle
- **CPU modification**: Modify heightmap float array in affected chunks
- **Dirty tracking**: Mark chunks dirty -> rebuild mesh + physics on next frame
- **Undo/Redo**: Store heightmap diff per stroke (compressed)

### 3.4 Editor Integration
- New TransformTool::Terrain in ui::TransformTool
- Toolbar: Sculpt / Paint / Select tabs
- Brush settings panel: Radius, Strength, Hardness, Tool dropdown
- Real-time preview: Draw brush circle on terrain under cursor

---

## 4. Texture Painting (Phase 2)

### 4.1 Layer System
- **Max 4 layers per chunk** (RGBA splatmap)
- **Global layer palette**: Up to 16 layers defined, 4 active per chunk
- **Layer blending**: Height-based + painted weights

### 4.2 Paint Tool
`cpp
struct PaintBrush {
    int layerIndex = 0;
    float radius = 10.0f;
    float strength = 0.2f;
    float hardness = 0.3f;
    bool eraseMode = false;         // Paint 0 weight
};
`

### 4.3 Shader
- Single terrain shader with:
  - 4 albedo + normal + roughness samplers
  - 1 splatmap (RGBA) per chunk
  - Triplanar mapping option for steep slopes
  - Distance-based texture blending

---

## 5. Rendering

### 5.1 Mesh Generation
- Use raylib GenMeshHeightmap() as base, but chunked
- **Vertex format**: Position, Normal, UV, Tangent (for normal mapping)
- **Index buffer**: Shared per resolution (static)

### 5.2 LOD System
`cpp
struct LODConfig {
    float lod0Distance = 50.0f;     // Full res
    float lod1Distance = 200.0f;    // Half res
    float lod2Distance = 500.0f;    // Quarter res
    float cullDistance = 2000.0f;   // Dont render
    bool useGeomorphing = true;     // Smooth LOD transitions
};
`

### 5.3 Draw Flow
`cpp
void Terrain::Draw() {
    UpdateChunks(camera);                    // Update LOD/visibility
    for (auto& chunk : visibleChunks) {
        if (chunk.needsUpload) UploadChunk(chunk);
        DrawModel(chunk.gpuModel, chunk.transform);
    }
}
`

---

## 6. Physics Integration

### 6.1 Physics Modes
`cpp
enum class PhysicsMode {
    Heightfield,    // Box3D heightfield (fast, 2.5D)
    TriangleMesh    // Box3D trimesh (precise, matches visual)
};
`

### 6.2 Implementation
`cpp
void Terrain::RebuildPhysics() {
    if (physicsMode == PhysicsMode::Heightfield) {
        // Build heightfield from chunk heightmaps
        // Box3D: b3HeightField or similar
        // One shape covering all chunks
    } else {
        // Build triangle mesh from LOD0 chunk meshes
        // Box3D: b3MeshShape
        // Can be heavy for large terrains -> async
    }
    physicsBody->SetShape(newShape);
}
`

### 6.3 Collision Queries
- GetHeightAt(x, z): Sample CPU heightmap (fast, for gameplay)
- Raycast(ray): Box3D raycast against terrain body
- GetNormalAt(x, z): Compute from heightmap neighbors

---

## 7. Editor Integration

### 7.1 Menu Addition
`cpp
// In ui.hpp MenuAction enum:
SpawnTerrain,        // New blank terrain
ImportTerrain,       // Import heightmap
`

### 7.2 Terrain Inspector Panel
`
Terrain
Size: [1000] x [1000]
Chunk Size: [256]  Res: [65]
Max Height: [200]
Physics: [Heightfield v]
[Rebuild Physics]
Layers
[+] Add Layer
v Grass     [Paint] [Remove]
  Dirt      [Paint] [Remove]
  Rock      [Paint] [Remove]
Tools
Tool: [Raise v]
Radius: [10]  Strength: [0.1]
Hardness: [0.5]
`

### 7.3 Viewport Overlay
- Brush circle at cursor position (world-projected)
- Real-time height preview under cursor
- Chunk bounds wireframe (toggle)
- LOD colorization (debug)

---

## 8. Serialization

### 8.1 Scene Format Extension
`cpp
// In ScenePersistence.hpp
bool SaveSceneToFile(..., Terrain* terrain = nullptr);
bool LoadSceneFromFile(..., Terrain*& outTerrain);

// Terrain data saved as:
// - Transform (pos, rot, scale)
// - Chunk grid dimensions
// - Heightmap data (compressed per chunk: zlib + quantization)
// - Layer definitions (paths + tile sizes)
// - Splatmaps (per chunk, PNG compressed)
// - Physics mode setting
`

### 8.2 Heightmap Compression
- Quantize float -> 16-bit (0-65535 = minHeight to maxHeight)
- Compress with zstd or zlib
- Store per-chunk for streaming loads

---

## 9. File Structure

`
include/
  Terrain.hpp           # Main terrain class
  TerrainTypes.hpp      # Enums, structs, brush config
  TerrainEditor.hpp     # Editor-specific terrain UI

src/
  Terrain.cpp           # Core implementation
  TerrainMesh.cpp       # Mesh generation, LOD, uploading
  TerrainSculpt.cpp     # Brush operations
  TerrainPhysics.cpp    # Physics integration
  TerrainIO.cpp         # Heightmap import/export
  TerrainEditor.cpp     # Editor UI, tool handling

shaders/
  terrain.vert          # Vertex shader (with LOD morphing)
  terrain.frag          # Fragment shader (splatmap blending)
`

---

## 10. Implementation Phases

### Phase 1: Core + Import (Week 1-2)
- [ ] Terrain entity class with chunked heightmap storage
- [ ] Heightmap import (PNG/RAW/EXR) with settings dialog
- [ ] Basic mesh generation using GenMeshHeightmap per chunk
- [ ] Single-draw rendering (no LOD yet)
- [ ] Add to Add Object menu as Landscape

### Phase 2: Sculpting Tools (Week 2-3)
- [ ] Brush system (raise/lower/smooth/flatten)
- [ ] CPU heightmap modification + dirty chunk tracking
- [ ] Mesh rebuild on dirty chunks
- [ ] Editor toolbar integration (Sculpt tab)
- [ ] Undo/Redo for terrain edits

### Phase 3: LOD + Streaming (Week 3-4)
- [ ] Multi-LOD mesh generation (65 -> 33 -> 17)
- [ ] Camera-based chunk visibility/LOD selection
- [ ] Geomorphing for smooth transitions
- [ ] Async chunk upload

### Phase 4: Physics (Week 4)
- [ ] Heightfield collision (Box3D)
- [ ] Triangle mesh collision (Box3D)
- [ ] Physics mode selector in inspector
- [ ] Async physics rebuild for large terrains

### Phase 5: Texture Painting (Week 5-6)
- [ ] Layer system + splatmap storage
- [ ] Paint brush tool
- [ ] Terrain shader with 4-layer blending
- [ ] Layer management UI

### Phase 6: Polish (Week 6)
- [ ] Serialization (save/load)
- [ ] Procedural generation (noise)
- [ ] Erosion simulation (optional)
- [ ] Performance profiling + optimization

---

## 11. Technical Considerations

### Memory Budget (100km², 256m chunks, 65² res)
| Component | Estimate |
|-----------|----------|
| Heightmap (float, all chunks) | 400 x 65² x 4B = 6.8 MB |
| Splatmaps (RGBA8, all chunks) | 400 x 65² x 4B = 6.8 MB |
| GPU meshes (LOD0, visible ~100) | 100 x 65² x 32B = 13.5 MB |
| **Total** | **~27 MB** |

### Performance Targets
- Sculpting: < 5ms per stroke (single chunk rebuild)
- Frame render: < 2ms for 100 visible chunks
- Physics rebuild: < 100ms async (large terrain)
- Import: < 5s for 4kx4k heightmap

### Raylib APIs to Use
- GenMeshHeightmap() - base mesh generation
- LoadImage() / LoadImageRaw() - heightmap loading
- UpdateMeshBuffer() - partial mesh updates
- rlEnableWireMode() - debug wireframe
- Raylib built-in math (raymath.h) for intersections

---

## 12. Open Questions

1. **Threading**: Should mesh rebuild/physics run on worker thread? (Engine currently single-threaded)
2. **Virtual Texturing**: For 100km²+, consider runtime virtual texturing instead of splatmaps?
3. **Vegetation/Detail**: Scatter system integration (grass, rocks) - separate system or built-in?
4. **Holes/Caves**: Support for terrain holes (vertical cliffs, caves) - requires different representation?
5. **Multi-terrain**: Multiple terrain entities in one scene? (Yes, each independent)

---

## 13. Dependencies
- **Box3D** (already in extern/) for physics
- **stb_image** (via raylib) for heightmap loading
- **zstd** or **miniz** for heightmap compression (optional, can use zlib)

---

*Plan created for Flyengine v1.0.0-beta*
*Estimated total: 6-8 weeks for full feature set*
