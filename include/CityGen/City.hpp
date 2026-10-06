#pragma once

#include "../Engine/Backend/Entity.hpp"
#include "raylib.h"
#include "raymath.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

class Engine;

namespace city {

// ---------------------------------------------------------------------------
// Shared palette used for instanced building tints.
// ---------------------------------------------------------------------------
constexpr int kBuildingColorBuckets = 8;
extern const Color kBuildingTints[kBuildingColorBuckets];

// Instanced building silhouette shapes. Wedges fill angled corners where two
// streets meet; slants are sheared slabs that sit along edges/interiors.
constexpr int kBuildingShapes = 3;
enum : int {
    kBuildingBox = 0,
    kBuildingWedge = 1,
    kBuildingSlant = 2,
};

// ---------------------------------------------------------------------------
// Parameters that define a procedurally generated city. Building geometry is a
// pure function of (params, seed, block polygon) so node edits regenerate the
// same style deterministically.
// ---------------------------------------------------------------------------
struct CityParams {
    // Layout
    int gridX = 6;              // interior intersections along X (2..64)
    int gridZ = 6;              // interior intersections along Z (2..64)
    float cellSize = 40.0f;     // base distance between road lines (m)

    bool organic = false;
    float organicStrength = 0.45f; // 0..1 scale applied to the FBM warp
    float organicScale = 40.0f;    // world-space wavelength of the warp field
    int noiseOctaves = 3;
    int seed = 1337;

    // Roads
    int lanes = 2;              // lanes per road (1..8)
    float laneWidth = 3.2f;     // width of a single lane (m)
    float sidewalk = 2.0f;      // sidewalk width that roads reserve (m)
    float cornerRadius = 6.0f;  // fillet radius of intersection/bend corners (m); 0 = sharp

    // Buildings
    float avgHeight = 14.0f;    // mean building height (m)
    float heightVariance = 0.45f; // 0..1 relative spread around avgHeight
    float buildingSize = 11.0f; // target building footprint (m)
    float buildingGap = 2.0f;   // clearance between buildings (m)

    // Parks / open space
    float parkThreshold = 2000.0f; // block area (m^2) above which it becomes a park
    float parkInset = 3.0f;       // grass margin kept between the roads and a park

    float RoadWidth() const {
        return lanes * laneWidth + sidewalk * 2.0f;
    }
    // Distance from each road centerline to the block polygon inset edge.
    float InsetDist() const {
        return RoadWidth() * 0.5f + buildingGap * 0.5f;
    }
};

bool operator==(const CityParams& a, const CityParams& b);
bool operator!=(const CityParams& a, const CityParams& b);

struct RoadNode {
    Vector2 pos{ 0.0f, 0.0f };
    bool boundary = false; // part of the outside loop (kept honest by the editor)
    bool junction = false; // 2+ distinct edge directions; strip ends get trimmed, cap emitted
};

struct RoadEdge {
    int a = 0;
    int b = 0;
    int lanes = 0; // per-edge lane override; 0 = use params.lanes
};

struct Building {
    Vector3 center{};
    Vector3 size{};
    float angleY = 0.0f; // rotation about +Y; local x-axis aligns with the block street
    int shape = kBuildingBox; // kBuildingBox / kBuildingWedge / kBuildingSlant
    int colorBucket = 0;
};

struct Block {
    std::vector<int> nodes;      // ordered indices into City::nodes (CCW)
    float area = 0.0f;
    bool park = false;
    std::vector<Vector2> inset;  // building placement polygon (CCW)
    std::vector<Vector2> parkPoly; // grass polygon for parks (CCW)
    std::vector<Building> buildings;
    // Persistent ID, assigned once when the block is first created and kept
    // across rebuilds (unlike its index in City::blocks, which shifts on
    // every rebuild as blocks are added/removed). This is what a building
    // override's key stays valid against.
    uint64_t id = 0;
};

// A per-building edit that survives regeneration: layout runs as normal, then
// any override for that building's (blockId, slot) is reapplied on top. Keyed
// this way rather than stored on Building itself, since Building is rebuilt
// from scratch by LayoutBlock every time -- only the override survives that.
struct BuildingOverride {
    bool hasHeight = false;
    float height = 0.0f;
    bool hasOffset = false;
    Vector2 posOffset{}; // added to the building's laid-out (x, z) center
};

// ---------------------------------------------------------------------------
// City: an Entity that owns a road graph plus the geometry derived from it.
// ---------------------------------------------------------------------------
class City : public Entity {
public:
    City();
    ~City() override;

    void Update(float deltaTime) override;
    void Draw() override;
    void DrawOverlay3D() override;

    const std::string& GetName() const { return name; }
    void SetName(const std::string& n) { name = n; }

    CityParams& GetParams() { return params; }
    const CityParams& GetParams() const { return params; }

    // Build a fresh regular/organic grid centered so that `origin` lands at the
    // mean of the inner nodes. Clears all editing (nodes/edges/blocks) and then
    // calls RebuildAll().
    void GenerateGrid(const Vector2& origin);
    // Rebuilds everything from the graph. Small cities rebuild synchronously; big
    // ones (see kAsyncRebuildNodes in City.cpp) rebuild on a worker thread while
    // the previous geometry keeps drawing, and the result is swapped in once its
    // GPU upload has been spread over a few frames. Requests made while a job is
    // running are coalesced (latest graph wins).
    void RebuildAll();
    bool IsRebuilding() const { return rebuild != nullptr; }

    const std::vector<RoadNode>& GetNodes() const { return nodes; }
    std::vector<RoadNode>& GetNodes() { return nodes; }
    const std::vector<RoadEdge>& GetEdges() const { return edges; }
    std::vector<RoadEdge>& GetEdges() { return edges; }
    const std::vector<Block>& GetBlocks() const { return blocks; }

    // Edits (each leaves the mesh consistent).
    // Moves a node. rebuildAll=false is the low-latency path used while the
    // user drags a node in the viewport: it only updates the node position and
    // skips the (expensive) full RebuildAll, which the caller throttles and
    // finalizes on release.
    void MoveNode(int index, const Vector2& pos, bool rebuildAll = true);
    // Incremental rebuild after node `index` moved (topology unchanged): only the
    // roads, blocks and buildings that depend on that node are regenerated, and
    // the result is identical to RebuildAll(). Falls back to a full rebuild when
    // the move changes the graph's face structure.
    void RebuildAfterNodeMove(int index);
    int  AddNode(const Vector2& pos, bool boundary);
    int  InsertNodeOnEdge(int edgeIndex, const Vector2& pos); // returns new node index
    // Road-tool placement: adds a node at pos and connects it to the road network
    // (nearest road point, splitting that edge; edges it crosses are split too, so
    // the graph stays planar). Returns the new node index.
    int  AddNodeConnected(const Vector2& pos);
    void DeleteNode(int index);
    void SetEdgeLaneOverride(int edgeIndex, int lanes);

    // Building overrides. `slot` is the building's index within its block's
    // `buildings` vector, which LayoutBlock fills deterministically, so the
    // same (blockId, slot) names the same physical building across rebuilds
    // as long as that block's polygon hasn't changed shape.
    void SetBuildingHeightOverride(uint64_t blockId, int slot, float height);
    void SetBuildingOffsetOverride(uint64_t blockId, int slot, Vector2 offset);
    void ClearBuildingOverride(uint64_t blockId, int slot);
    bool HasBuildingOverride(uint64_t blockId, int slot) const;
    Vector2 GetBuildingOffsetOverride(uint64_t blockId, int slot) const; // {0,0} if none
    // Ray-picks a building's roof/walls; returns block index (into GetBlocks())
    // and building slot within it, or false if nothing was hit.
    bool PickBuilding(const Ray& ray, int& outBlock, int& outSlot, float* outDist = nullptr) const;

    // Picking helpers (screen-space ray vs node spheres / road centerlines).
    int  PickNode(const Ray& ray, float tolerance, float* outDist = nullptr) const;
    int  PickRoad(const Ray& ray, float tolerance, float* outParam = nullptr) const;
    // True when p lies on any block pad or within road width of any road line.
    bool ContainsPoint(const Vector2& p) const;

    // Geometry helpers used by the editor.
    Vector2 NodePos(int i) const;
    Vector2 EdgeMidpoint(int i) const;

    // Serialization (text). Name is included so the same payload works for
    // both the .city sidecar blocks and the in-memory undo snapshots.
    bool WriteToStream(std::ostream& out) const;
    bool ReadFromStream(std::istream& in);

private:
    void ClearGraph();
    void ComputeBlocks();   // fills blocks from the graph faces
    void LayoutBuildings(); // fills per-block building boxes (parks first)
    void LayoutBlock(Block& block); // one block: park polygon or parcelled buildings
    void ComputeJunctionFlags(); // sets node.junction (depends only on the graph)
    bool JunctionFlagFor(int ni) const;
    void BuildAdjacency();       // nodeEdges
    std::vector<int> AngularRing(int v) const; // neighbour ids of v sorted by angle
    void ClearGeometry();
    int  SplitEdge(int edgeIndex, const Vector2& pos); // no rebuild
    void ConnectWithSplits(int a, int b);              // no rebuild

    std::string name = "City";
    CityParams params;

    std::vector<RoadNode> nodes;
    std::vector<RoadEdge> edges;
    std::vector<Block> blocks;
    uint64_t nextBlockId = 1; // 0 is reserved/invalid
    std::unordered_map<uint64_t, BuildingOverride> buildingOverrides; // key: (blockId<<20)|slot
    static uint64_t OverrideKey(uint64_t blockId, int slot) { return (blockId << 20) | (uint64_t)(uint32_t)slot; }
    void ApplyBuildingOverrides(Block& block); // post-pass after LayoutBlock fills block.buildings
    void RefreshBuildingTile(uint64_t blockId); // rebuilds the one tile a block lives in after an override edit
    // Dead-end road spurs (pruned from face extraction); buildings keep clear of them.
    std::vector<std::pair<Vector2, Vector2>> spurSegs;

    // Spatial tiles. Each tile owns the road/pad/park mesh chunks and the
    // building instances of the elements assigned to it (by their centre) plus a
    // tight bounding box, so Draw() culls per tile and an edit only regenerates
    // the tiles it touches. Building tint is packed into each instance transform.
    struct Tile {
        std::vector<int> blocks, edges, nodes;      // member elements
        std::vector<Model> roadModels;              // 16-bit-safe indexed chunks (GPU)
        std::vector<Mesh> raw;                      // CPU-only chunks awaiting UploadTile()
        std::vector<Matrix> inst[kBuildingShapes];  // per-shape instance transforms
        unsigned int instVbo[kBuildingShapes] = {}; // persistent GPU instance buffers
        int instVboCount[kBuildingShapes] = {};
        Vector3 roadMin{}, roadMax{}, bldgMin{}, bldgMax{};
        bool hasRoad = false, hasBldg = false;
    };
    int64_t TileKeyOf(const Vector2& p) const;
    Vector2 BlockCenter(const Block& b) const;
    void BuildTile(Tile& t);
    void DestroyTile(Tile& t);
    void ComputeAllCPU();   // graph -> blocks -> tiles (CPU only; thread-safe on a private City)
    void AssignTiles();     // put every node/edge/block into its tile
    void ComputeTileCPU(Tile& t);
    void UploadTile(Tile& t);
    void UploadAllTiles();
    void DestroyAllTiles();
    void ResetTileCPU(Tile& t);

    // Background rebuild (see RebuildAll).
    struct RebuildJob;
    std::unique_ptr<RebuildJob> rebuild;
    uint64_t rebuildRequestId = 0;
    void RequestRebuild();
    void StartRebuildJob();
    void PumpRebuild();      // called from Update(): poll/upload/adopt the job
    void CancelRebuild();
    void AdoptRebuild(City& w);

    std::unordered_map<int64_t, Tile> tiles;
    std::vector<int64_t> nodeTile, edgeTile, blockTile; // tile key per element
    std::vector<std::vector<int>> nodeEdges;   // incident edge ids per node
    std::vector<std::vector<int>> nodeBlocks;  // blocks containing each node
    std::vector<std::vector<int>> nodeRing;    // angular neighbour order per node
    std::vector<char> edgeSpur;                // edge is a pruned dead-end spur
    std::vector<Matrix> visInst[kBuildingShapes]; // per-frame visible instances (scratch)
    std::vector<const Tile*> visRoad, visBldg;     // per-frame visible tiles (scratch)

    // Editor overlay: the node markers are drawn as a single instanced draw per
    // color (2 calls total) instead of a DrawCube per node -- 10,000 draw calls
    // per frame at a 99x99 grid. Rebuilt each DrawOverlay3D, cheap at any grid
    // size. [0]=interior, [1]=boundary.
    std::vector<Matrix> nodeMarkerTransforms[2];

    bool hasGeometry = false;
};

// ---------------------------------------------------------------------------
// Registry + sidecar (mirrors the terrain.terrain pattern).
//
//     CityFileHeader { magic "FLYCITY1", version, cityCount }
//     for each city:
//         CityBlockHeader { nameLen, dataSize }
//         name bytes (UTF-8)
//         data bytes  <- City::WriteToStream() payload
// ---------------------------------------------------------------------------
class CityRegistry {
public:
    void Register(City* c) {
        if (!c) return;
        if (std::find(cities.begin(), cities.end(), c) != cities.end()) return;
        cities.push_back(c);
    }
    void Unregister(City* c) {
        cities.erase(std::remove(cities.begin(), cities.end(), c), cities.end());
    }
    bool Contains(City* c) const {
        return std::find(cities.begin(), cities.end(), c) != cities.end();
    }
    const std::vector<City*>& GetCities() const { return cities; }
    std::vector<City*>& GetCities() { return cities; }
    size_t Count() const { return cities.size(); }

    City* GetByName(const std::string& name) const {
        for (auto* c : cities) if (c && c->GetName() == name) return c;
        return nullptr;
    }
    bool NameExists(const std::string& name) const {
        return GetByName(name) != nullptr;
    }
    std::string MakeUniqueName(const std::string& base) const {
        if (!NameExists(base)) return base;
        for (int i = 2; i < 100000; i++) {
            std::string candidate = base + " (" + std::to_string(i) + ")";
            if (!NameExists(candidate)) return candidate;
        }
        return base + " (" + std::to_string(rand()) + ")";
    }

    bool WriteFile(const std::string& path) const;
    // `create` must build a City, add it to the engine and return the raw
    // pointer. Registers and hydrates the city from the payload.
    int ReadFile(const std::string& path, const std::function<City*()>& create);

    static CityRegistry& Get() {
        static CityRegistry instance;
        return instance;
    }

private:
    CityRegistry() = default;
    std::vector<City*> cities;
};
inline CityRegistry& GetCityRegistry() { return CityRegistry::Get(); }

// ---------------------------------------------------------------------------
// Undo integration. The interaction manager registers a callback via
// SetCityEditCallback(); the city editor calls NotifyCityEdit() whenever a
// committed edit (drag release, nudge, regen, delete...) should be undoable.
// ---------------------------------------------------------------------------
void SetCityEditCallback(const std::function<void()>& cb);
void NotifyCityEdit();

// Snapshot extension for the undo system: the scene bytes plus a "CITY n"
// section holding each registered city's WriteToStream payload.
std::string SerializeCitiesSnapshot();
// Reads a snapshot's trailing CITY section (returns false if absent/broken).
// Acts on the CityRegistry + engine: removes current cities and recreates the
// snapshot's cities. `create` mirrors the sidecar ReadFile contract.
int RestoreCitiesFromSnapshot(std::istream& in, Engine& engine,
                              const std::function<City*()>& create);

} // namespace city