#pragma once

#include "../Engine/Backend/Entity.hpp"
#include "raylib.h"
#include "../Terrain/BasicTerrain.hpp"
#include "raymath.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

class Engine;
class BasicTerrain;

namespace city {

// ---------------------------------------------------------------------------
// Shared palette used for instanced building tints.
// ---------------------------------------------------------------------------
constexpr int kBuildingColorBuckets = 8;
extern const Color kBuildingTints[kBuildingColorBuckets];

// Instanced building silhouette shapes. Wedges fill angled corners where two
// streets meet; slants are sheared slabs that sit along edges/interiors.
constexpr int kBuildingShapes = 21;   // instanced shape ids (props and agents share the table; 11/12 are drawn outside the tiles)
constexpr float kFloorHeight = 3.4f;   // metres per storey: building heights are whole storeys, the facade shader uses the same value
enum : int {
    kBuildingBox = 0,
    kBuildingWedge = 1,
    kBuildingSlant = 2,
    kBuildingGable = 3,   // box with a pitched roof (suburban houses)
    kBuildingTower = 4,   // stepped tower: full-width base, narrower shaft, narrowest crown
    // Props and agents share the instancing pipeline (vertex-coloured, no facade, no collision).
    kPropLamp = 5,
    kPropTree = 6,
    kPropCar = 7,
    kPropPerson = 8,
    kPropSignal = 9,
    kBuildingShed = 10,   // box with a single-slope roof
    kPropCarGlass = 11,   // car windows (untinted), same pose as kPropCar
    kPropWheel = 12,      // one car wheel (rolls about its local z axis)
    // Sidewalk furniture (local +z points toward the road).
    kPropBench = 13,
    kPropHydrant = 14,
    kPropBollard = 15,
    kPropBusStop = 16,    // shelter ~3 m wide along the road; the advert panel glows at night
    kPropSign = 17,       // street name sign on a pole
    kPropBus = 18,        // 11 m bus body (tinted by the line colour), forward is local +x like the car
    kPropBusGlass = 19,   // bus windows and lights (untinted)
    kPropFountain = 20,   // round stone fountain (~5.4 m across), in the middle of plazas and big parks
};
inline bool IsPropShape(int s) { return s >= 5 && s <= 9; }

// Overall look of procedurally generated buildings.
enum class BuildingStyle : int { Modern = 0, Brick = 1, Industrial = 2, Suburban = 3 };

// ---------------------------------------------------------------------------
// Parameters that define a procedurally generated city. Building geometry is a
// pure function of (params, seed, block polygon) so node edits regenerate the
// same style deterministically.
// ---------------------------------------------------------------------------
// One entry of the car colour table. `other` entries pick a random vivid colour instead of `color`.
struct CarColor {
    std::string name;
    Color color{ 0, 0, 0, 255 };
    float weight = 0.0f;    // relative spawn chance (percent-like; normalised over the table)
    bool other = false;
};

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
    float lodDistance = 450.0f; // tiles farther than this from the camera draw their buildings as plain boxes in one buffer, without props (a third of it: no props); 0 = off
    float maxGrade = 15.0f;     // steepest road slope (%, at the steepest point of a ramp) the editor allows when you raise a node: its neighbours follow so hills spread out; 0 = off

    // Buildings
    float avgHeight = 14.0f;    // mean building height (m)
    float heightVariance = 0.45f; // 0..1 relative spread around avgHeight
    float buildingSize = 11.0f; // target building footprint (m)
    float buildingGap = 2.0f;   // clearance between buildings (m)

    // Parks / open space
    float parkThreshold = 2000.0f; // block area (m^2) above which it becomes a park
    float parkInset = 3.0f;       // grass margin kept between the roads and a park

    // Look
    int style = 0;                // BuildingStyle
    float shapeVariety = 0.5f;    // 0..1 chance that a parcel gets a non-flat roof / stepped shape (gable, shed, tower)
    float shortChance = 0.15f;    // 0..1 chance that a building is much shorter than its neighbours (1-2 storeys)
    float footprintVariety = 0.15f; // 0..1 how much a building's footprint may shrink from its parcel
    bool furniture = true;        // streetlights and trees along roads
    int cars = 0;                 // Play-mode traffic: number of cars
    int pedestrians = 0;          // Play-mode traffic: number of pedestrians
    bool leftHandTraffic = false;         // cars drive on the left (UK, Japan, ...) instead of the right
    bool carColorAll = false;             // every colour in the table is equally likely (weights ignored)
    std::vector<CarColor> carColors = {   // spawn chances of car colours (not part of the layout; changing them never rebuilds)
        { "Black", { 22, 22, 26, 255 }, 55.0f, false },
        { "Grey", { 128, 131, 138, 255 }, 25.0f, false },
        { "White", { 232, 232, 236, 255 }, 15.0f, false },
        { "Other", { 200, 40, 40, 255 }, 5.0f, true } };
    bool routedTraffic = true;            // cars drive to destinations (shortest route, busier downtown) instead of wandering
    bool rushHours = false;               // the number of cars follows the time of day (peaks 7-9 and 16-19, quiet at night)
    float trafficDetailDistance = 150.0f; // cars farther than this from the camera run the cheap model (0 = always detailed)

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

// One step of a traffic-signal cycle. `mask` has one bit per compass sector of the arriving road
// (bit 0 = road to the east of the junction, 1 = north-east, 2 = north, ... counter-clockwise).
struct SignalPhase {
    float duration = 9.0f;   // green time, seconds
    uint8_t mask = 0;
};

struct RoadNode {
    Vector2 pos{ 0.0f, 0.0f };
    bool boundary = false; // part of the outside loop (kept honest by the editor)
    bool junction = false; // 2+ distinct edge directions; strip ends get trimmed, cap emitted
    float h = 0.0f;        // road surface height above the city's ground plane
    int jkind = 0;         // JunctionKind: what the junction looks like (crosswalks, stop lines, ...)
    // Traffic-light programme (used when jkind == TrafficLight). Empty = default two-phase cycle.
    std::vector<SignalPhase> phases;
    float yellow = 1.5f;   // amber time after each green
    float allRed = 1.0f;   // all-red clearance after the amber
    float sigOffset = 0.0f; // shifts this junction's cycle (for green waves)
    // Roundabout (jkind == Roundabout) settings; 0 = automatic size from the connected roads.
    float rbIsland = 0.0f;  // central island radius (m)
    float rbRing = 0.0f;    // width of the circulating roadway (m)
    bool rbSplitters = true;   // concrete splitter islands where roads meet the ring
    bool rbConcrete = false;   // island is solid concrete instead of grass
};

// Junction treatment at a node with 2+ roads. Plain keeps the bare plate.
enum class JunctionKind : int { Plain = 0, Crosswalks = 1, TrafficLight = 2, Stop = 3, Roundabout = 4 };

// Road class presets (Street..Path): shorthand for lanes/width/sidewalk/curb/markings.
enum class RoadType : int { Street = 0, Avenue = 1, Highway = 2, Path = 3, Pedestrian = 4 };

struct RoadEdge {
    int a = 0;
    int b = 0;
    int lanes = 0; // per-edge lane override; 0 = use params.lanes
    float width = 0.0f;     // per-road lane width override; 0 = params.laneWidth
    float sidewalk = -1.0f; // per-road sidewalk width; < 0 = params.sidewalk
    int type = 0;           // RoadType (informational; presets write the fields below)
    bool bridge = false;    // elevated deck with side walls (and pillars)
    float curbH = 0.0f;     // sidewalk raised above the asphalt by this much
    bool markings = false;  // centre line / lane markings
    float bank = 0.0f;      // cross-slope on curves, degrees (positive banks toward the inside)
    int oneWay = 0;         // 0 both directions, 1 = a->b only, 2 = b->a only
    float speedLimit = 0.0f; // m/s; 0 = default for the road type
    // Lane use at the junction ends, 4 bits per lane (lane 0 = nearest the centre line):
    // bit0 = turn left, bit1 = straight, bit2 = turn right; 0 = no restriction. turnA is for traffic
    // arriving at node a (travelling b -> a), turnB for traffic arriving at node b.
    uint16_t turnA = 0, turnB = 0;
};

struct Building {
    Vector3 center{};
    Vector3 size{};
    float angleY = 0.0f; // rotation about +Y; local x-axis aligns with the block street
    int shape = kBuildingBox; // kBuildingBox / kBuildingWedge / kBuildingSlant
    int colorBucket = 0;
    int floors = 1;        // storeys; size.y is floors * kFloorHeight (plus any foundation under a slope)
    int placedIndex = -1; // >= 0: a user-placed building (index into City::GetPlacedBuildings())
    int style = 0;         // BuildingStyle used for the facade tint (districts may differ from CityParams::style)
    float foundation = 0.0f; // height of the plinth under the first floor (m): the part sunk into a sloped pad
};

// A building plopped by the Building Insert tool. Survives regeneration: blocks
// lay their procedural parcels out around these footprints.
struct PlacedBuilding {
    Vector2 center{};
    float sizeX = 11.0f;  // along the street (local x)
    float sizeZ = 11.0f;  // depth
    float height = 14.0f;
    float angleY = 0.0f;  // rotation about +Y (same convention as Building::angleY)
    int colorBucket = 0;
    int shape = 0;        // kBuildingBox / Gable / Tower ...
    bool free = false;    // placed with NoCollision: may sit off any block / on roads / overlapping others
};

struct Block {
    std::vector<int> nodes;      // ordered indices into City::nodes (CCW)
    float area = 0.0f;
    bool park = false;
    bool plaza = false;          // a park block drawn as a paved square (with a fountain) instead of grass
    std::vector<Vector2> inset;  // building placement polygon (CCW)
    std::vector<Vector2> parkPoly; // grass polygon for parks (CCW)
    std::vector<Building> buildings;
    // Persistent ID, assigned once when the block is first created and kept
    // across rebuilds (unlike its index in City::blocks, which shifts on
    // every rebuild as blocks are added/removed). This is what a building
    // override's key stays valid against.
    uint64_t id = 0;
};

// A district shapes the buildings around a point: a Downtown raises their height towards its centre
// and falls off smoothly to the edge of its radius; a Suburb lowers them and switches the style.
// Several districts may coexist and overlap (their height changes add up; the strongest sets the style).
enum class DistrictKind : int { Downtown = 0, Suburb = 1, Industrial = 2 };
struct District {
    Vector2 pos{};
    float radius = 120.0f;     // influence radius (m)
    float peak = 3.0f;         // height multiplier at the centre (1 = no change; Downtown > 1, Suburb < 1)
    int kind = 0;              // DistrictKind
    int style = -1;            // BuildingStyle inside the district; -1 = the city's style
    std::string name;
};

// Public transit. A stop sits beside a road and serves one direction of travel; a line is an ordered, cycling
// list of stops that its buses visit (a two-stop line shuttles back and forth). Stops are stored by where they
// were placed and re-attached to the road graph at every rebuild, so editing roads never leaves them dangling.
struct BusStop {
    Vector2 pos{};            // centre line of the road where it was placed (world x, z)
    Vector2 heading{ 1.0f, 0.0f };   // direction of travel it serves
    std::string name;
    // Resolved against the road graph (RebuildAll): edge < 0 = not on a road any more (dormant).
    int edge = -1;
    float s = 0.0f;           // metres from the edge's node a
    bool fwd = true;          // serves travel a -> b
};
struct BusLine {
    std::string name;
    Color color{ 40, 110, 200, 255 };
    std::vector<int> stops;   // indices into City::GetBusStops(), visited in order, cycling
    int buses = 2;
};

// Painted land use for a block. Auto = the procedural area-based rule.
enum class BlockKind : int { Auto = 0, Park = 1, Buildings = 2, Concrete = 3, Plaza = 4 };

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
    // 0..1 while a big rebuild runs (worker compute ~97 %, then the GPU upload); 1 when idle.
    float RebuildProgress() const;
    // Seconds the current rebuild has been running (0 when idle).
    float RebuildSeconds() const;
    // Seconds a rebuild runs before the "attempting to load" message shows (default 5; tests lower it).
    void SetLoadingMessageDelay(float seconds) { loadingMessageDelay = seconds; }

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
    // True when putting node `index` at `pos` would make one of its roads cross another road.
    // Crossing roads break the planar block faces (overlapping pads/parks), so the editor refuses
    // such moves.
    bool MoveWouldCross(int index, const Vector2& pos) const;
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

    // Road drawing. SnapRoadPoint resolves a cursor point to an existing node, a
    // point on an existing road, or itself (free ground).
    struct RoadSnap { Vector2 pos{}; int node = -1; int edge = -1; };
    RoadSnap SnapRoadPoint(const Vector2& p) const;
    // Adds a road along `pts` (a sampled curve): the ends snap to nodes/roads, the
    // path is split wherever it crosses existing roads, and the city rebuilds once.
    // Returns the node index at the final point, or -1 if nothing was added.
    // New nodes get heights ramping between the end nodes' heights (an end that snaps to an
    // existing node keeps that node's height; free ends take hStart / hEnd).
    int AddRoadPath(const std::vector<Vector2>& pts, float hStart = 0.0f, float hEnd = 0.0f);
    // Removes the whole road through edgeIndex: the chain of edges up to the
    // nearest junctions/dead ends, plus nodes left without roads.
    void DeleteRoadChain(int edgeIndex);
    void SetEdgeLaneOverride(int edgeIndex, int lanes);
    // Elevation. Height of the road centreline along edge `ei` at fraction s (0 = node a,
    // 1 = node b): flat through each junction, easing between the two node heights.
    float EdgeProfileY(int ei, float s) const;
    // Half widths of a road: the asphalt ribbon, and the full slab including sidewalks. They use
    // the road's own lane count / lane width / sidewalk when set, else the city's params.
    float EdgeAsphaltHalf(int ei) const;
    float EdgeSlabHalf(int ei) const;
    void SetNodeHeight(int index, float h);
    void SetNodeJunctionKind(int index, int kind);
    // Replaces the per-road properties (lanes/width/sidewalk/type/bridge/curb/markings/bank).
    void SetEdgeProps(int edgeIndex, const RoadEdge& props);
    // Fixed-ends smoothing of the node heights along the road chain through edgeIndex.
    void SmoothRoadHeights(int edgeIndex, int iterations = 8);
    // Sets the heights of the road chain through edgeIndex to a constant grade (%) from the
    // chain's `fromNode` end; the far end follows.
    void SetRoadGrade(int edgeIndex, int fromNode, float gradePercent);
    // Ids of the chain of edges (up to junctions/dead ends) that contains edgeIndex, plus the
    // node sequence from one end to the other.
    void GetRoadChain(int edgeIndex, std::vector<int>& outEdges, std::vector<int>& outNodes) const;

    // Play-mode collision for buildings and the road/pad surface. Rebuilds.
    bool GetCollisionEnabled() const { return collisionEnabled; }
    void SetCollisionEnabled(bool on);
    // Called by city::AttachPhysicsWorld / DetachPhysicsWorld.
    void CreateAllTilePhysics();
    void DestroyAllTilePhysics();

    // Painted block land use, keyed by persistent block id (survives rebuilds).
    // Auto removes the override. Triggers a rebuild.
    void SetBlockKind(uint64_t blockId, BlockKind kind);
    BlockKind GetBlockKind(uint64_t blockId) const;
    // Index into GetBlocks() of the block whose pad contains p, or -1.
    int PickBlockAt(const Vector2& p) const;

    // User-placed buildings. A placed building renders with the block its centre
    // lies in; that block's procedural parcels shrink or vanish to make room.
    const std::vector<PlacedBuilding>& GetPlacedBuildings() const { return placed; }
    int  AddPlacedBuilding(const PlacedBuilding& pb);          // returns its index
    void UpdatePlacedBuilding(int index, const PlacedBuilding& pb);
    void RemovePlacedBuilding(int index);
    // True when the footprint sits on a block (clear of the roads) and does not
    // overlap another placed building (`ignoreIndex` is skipped).
    bool CanPlaceBuilding(const PlacedBuilding& pb, int ignoreIndex = -1) const;
    // Road snapping: puts pb (sizes already set) beside the nearest road to
    // `cursor`, facing it. Returns false when no road is close enough.
    bool SnapBuildingToRoad(const Vector2& cursor, PlacedBuilding& pb) const;

    // Districts (see District). A district acts through its position, so dragging one re-lays-out the
    // buildings it covers. Painted blocks belong to a district at full strength.
    const std::vector<District>& GetDistricts() const { return districts; }
    int  AddDistrict(const District& d);                   // returns its index
    void UpdateDistrict(int index, const District& d);
    void MoveDistrict(int index, const Vector2& pos, bool rebuild = true);
    void RemoveDistrict(int index);
    // Replaces all districts with one downtown at the middle of the city whose height fades out to the edge.
    void AutoDistricts(float peak = 3.0f);
    void PaintBlockDistrict(uint64_t blockId, int district);   // -1 clears
    int  GetBlockDistrict(uint64_t blockId) const;
    // Height multiplier and style (-1 = city style) the districts give to a block centred at p.
    void DistrictAt(const Vector2& p, uint64_t blockId, float& heightMul, int& style) const;
    static District MakeDistrict(DistrictKind kind, const Vector2& pos);
    // Height of the editor marker's top (it rises above the tallest buildings the district makes) and
    // a ray pick of the marker pillar; returns the district index or -1.
    float DistrictMarkerHeight(const District& d) const;
    int PickDistrict(const Ray& ray) const;

    // Public transit (see BusStop / BusLine). Everything can be generated automatically (AutoTransit) or built by
    // hand: place stops on roads, then add them to lines in the order the buses should visit them.
    const std::vector<BusStop>& GetBusStops() const { return busStops; }
    const std::vector<BusLine>& GetBusLines() const { return busLines; }
    // Snaps a cursor point to the road: the stop position and the direction it would serve (the kerb the
    // cursor is on). False when no road is close enough.
    bool SnapBusStop(const Vector2& cursor, Vector2& pos, Vector2& heading) const;
    int  AddBusStop(const Vector2& pos, const Vector2& heading);   // returns its index, -1 if not on a road
    void RemoveBusStop(int index);                                  // also removes it from every line
    int  AddBusLine(const BusLine& line);
    void UpdateBusLine(int index, const BusLine& line);
    void RemoveBusLine(int index);
    void AddStopToLine(int line, int stop);
    void RemoveStopFromLine(int line, int position);
    void ClearTransit();
    // Replaces all stops and lines with generated ones: each line links two far-apart hubs (downtown first) and
    // stops are spread along the route there and back.
    void AutoTransit(int lineCount = 3, int stopsPerLine = 6);
    int  PickBusStop(const Ray& ray) const;                         // marker pillar under the cursor, or -1
    float BusStopMarkerHeight() const { return 9.0f; }

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

    // Order-independent hashes of the generated geometry (road/pad/park triangles
    // and building instances), for tests that compare rebuild paths.
    struct GeometryHashes { uint64_t road = 0, buildings = 0; size_t roadTris = 0, instances = 0; };
    GeometryHashes DebugGeometryHashes() const;
    // Number of placed instances of an instanced shape (props, buildings) and the pose of the n-th one
    // (position and yaw about +Y); for tests and screenshots.
    // All road / pad / park surface triangles of the city (world space, 3 vertices per triangle); needs
    // collision enabled (the default). For geometry audits in tests.
    void DebugSurfaceTriangles(std::vector<Vector3>& out) const;
    void DebugSurfaceLayers(std::vector<float>& out) const;   // the stack layer of each triangle returned by DebugSurfaceTriangles

    // ---- Terrain integration (City panel > Terrain) ----
    // Road height of every node from the terrain under it (+ offset). maxGradePercent > 0 then relaxes the heights so
    // no road is steeper than that (measured over the sloping part of the road, i.e. without the level junction zone).
    // Returns how many nodes got a terrain height (nodes off the terrain keep theirs).
    int SnapToTerrain(const BasicTerrain& terrain, float offset, float maxGradePercent);
    // Only the grade relaxation, on the heights the nodes have now. Returns the steepest remaining grade (%).
    // Relaxes node heights until no road ramp is steeper (at its steepest point) than maxGradePercent. `pinnedNode` keeps
    // that node exactly where it is: the other nodes give way (the editor pins the node you just raised).
    float LimitRoadGrades(float maxGradePercent, int pinnedNode = -1);
    // Sets a node's height, then lets its neighbours follow so no ramp is steeper than params.maxGrade (editor use).
    void SetNodeHeightSmooth(int index, float h);
    // Reshapes the terrain under and around the city: each heightmap vertex near the road, pad and park surfaces is
    // set to the lowest city surface around it minus `clearance`, and the ground fades back to its natural height over
    // `margin` metres. Bridge spans do not count. Returns the number of vertices changed.
    int ShapeTerrainToCity(BasicTerrain& terrain, float clearance, float margin);

    // Geometry diagnostics ("Show geometry problems" in the editor): suspicious spots in the road/pad/park surface.
    //  steep - a surface triangle steeper than 60 degrees (a cliff)
    //  thin  - a long triangle that is paper-thin seen from above (a spike)
    //  steps - a place where the surface has an open edge and another open edge at the same x,z but a different
    //          height: a crack or step between neighbouring surfaces (e.g. a sloped pad that does not meet the road)
    struct GeometryProblems {
        std::vector<std::array<Vector3, 3>> steep, thin;
        std::vector<std::pair<Vector3, Vector3>> steps;   // (low, high) point at the same x,z
        size_t padSamplesOnRoad = 0;                      // sample points of pads that lie over road asphalt (the rate denominator)
        std::vector<float> padOverDelta;                  // how far (m, with the layer bias) the pad is above the road there
        std::vector<Vector3> padOverRoad;                 // a pad / park drawn on top of road asphalt (it hides the road)
    };
    void ComputeGeometryProblems(GeometryProblems& out) const;
    // The last computed set, refreshed about twice a second while the editor asks for it.
    const GeometryProblems& GetGeometryProblemsCached();
    int CountInstances(int shape) const;
    bool FindBus(int index, Vector3& pos, float& yaw) const;
    bool FindSlopedCar(Vector3& pos, float& yaw, float& pitch, bool needBlinker = false) const;   // a detailed car on a noticeable slope (tests / screenshots)   // pose of the n-th bus (tests / screenshots)
    std::string DebugBuses() const;
    std::string DebugPeds(int count) const;
    bool FindWalkingPed(int index, Vector3& pos, float& yaw) const;   // the n-th pedestrian that is on the street (tests / screenshots)
    bool FindQueuedPed(int index, Vector3& pos, float& yaw) const;   // the n-th pedestrian queued at a bus stop (tests / screenshots)   // the first few pedestrians: road, position, destination, state   // one line per bus: road, position, speed, target stop, dwell
    bool FindInstance(int shape, int index, Vector3& pos, float& yaw) const;

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
    float FreeGroundY(const Vector2& p) const;               // ground height under a NoCollision building
    int EdgeBetween(int a, int b) const;                   // edge joining nodes a and b, or -1
    // Fractions of the edge, from each end, over which the road surface is exactly level (the junction plateau).
    // It covers the junction plate and the (possibly skewed) cut where the road strip meets it.
public:
    void EdgePlateau(int ei, float len, float& atA, float& atB) const;
private:
    float EdgeRampU(int ei, float s) const;                // 0..1 eased ramp position along an edge (flat at junctions)
    // Cross-section stations (fractions 0..1 along edge a->b) shared by the road surface and the block
    // pads beside it, and the piecewise-linear surface height through them.
    std::vector<float> EdgeStations(int ei) const;
    float EdgeSurfaceY(int ei, float s) const;
    float BlockRoadHalf(const Block& block) const;         // widest road half width around a block
    struct BlockSurface;                                   // pad height field (see City.cpp)
    void BuildBlockSurface(const Block& block, BlockSurface& out) const;
    void FitBuildingsToSurface(Block& block) const;        // sit buildings on sloped pads
    void LayoutBlockProcedural(Block& block); // parcels/park only
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
    // Play-mode traffic: cars follow lanes along the road graph, pedestrians walk the sidewalks.
    struct Agent {
        bool car = true;
        int edge = 0;
        bool fwd = true;        // travelling a -> b
        float s = 0.0f;         // metres from the start node of the travel direction
        float speed = 0.0f;
        float maxSpeed = 8.0f;  // desired speed (personal, randomised)
        float accel = 2.0f;     // m/s^2
        float decel = 3.0f;     // comfortable braking, m/s^2
        float headway = 1.4f;   // desired time gap to the car ahead, s
        float minGap = 2.2f;    // bumper gap when stopped, m
        int lane = 0;           // target lane counted from the road centre (cars) / side (pedestrians: 0 or 1)
        float laneF = 0.0f;     // lane the car is actually in (eases toward `lane` on a lane change)
        float speedFactor = 1.0f; // personal fraction of the road's speed limit
        int turn = 0;           // upcoming manoeuvre for the indicator: -1 left, 0 none, +1 right
        float wait = 0.0f;      // stop-sign wait timer
        bool released = false;  // already cleared this junction approach
        Vector3 pos{};
        Vector3 lastPos{};      // pose at the previous traffic step (jump detection in the stats)
        float yaw = 0.0f;
        bool placed = false;    // pos/yaw initialised
        int dest = -1;          // destination node of the current trip (routed traffic), -1 = none
        // Pedestrians (see the pedestrian branch of StepTraffic):
        float scale = 1.0f;     // body size
        float idle = 0.0f;      // seconds left standing at a destination
        int rideStop = -1;      // bus stop to catch (rideState 0..2) or to get off at (rideState 3)
        int rideState = 0;      // 0 walking to dest, 1 walking along the stop's road to the stop, 2 queued at the stop, 3 riding a bus (hidden)
        float waitStop = 0.0f;  // seconds queued at the stop
        int parkPhase = 0;      // 0 = on the street, 1 = walking into a park, 2 = sitting there, 3 = walking back out
        Vector3 parkFrom{}, parkTo{};   // the sidewalk point it left and the spot it is going to in the park
        float parkT = 0.0f;     // 0..1 along that walk
        // Buses:
        uint32_t busId = 0;     // a bus's id; a rider carries the id of the bus it is on
        int atStop = -1;        // the stop the bus is standing at (-1 = none)
        int riders = 0;         // passengers on board
        int bus = -1;           // >= 0: a bus of that line (index into GetBusLines())
        int busTarget = 0;      // index into the line's stops: the stop it is heading for
        int busSkip = -1;       // edge*2+dir of a stop it just left (ignored until it is on another road)
        float dwell = 0.0f;     // seconds waited at the current stop
        float length = 4.2f;    // bumper-to-bumper length (m): leaders and followers keep their gap from it
        float wheelRot = 0.0f;  // wheel roll angle (rad)
        float colorRoll = 0.0f; // 0..1 draw deciding the colour from the table (stable while the table changes)
        float colorRoll2 = 0.0f;// picks the vivid colour of an "Other" entry
        float stuck = 0.0f;     // seconds spent (almost) stopped; long waits release the car (deadlock breaker)
        float nearTime = 0.0f;  // seconds spent in the detailed model (crash statistics ignore fresh arrivals)
        bool far = false;       // beyond the detail distance: constant speed, snap turns, box
        // Junction crossing along a quadratic Bezier (gradual turn).
        int nextEdge = -1;
        bool nextFwd = true;
        bool inJ = false;
        float jt = 0.0f, jlen = 1.0f, jExit = 0.0f, jVMax = 99.0f;
        int jFrom = -1, jTo = -1, jNode = -1, jLaneB = 0;   // jFrom/jTo: the road pair the planned path belongs to
        std::vector<Vector2> jpath;   // planned path through the next junction (cars)
        std::vector<float> jcum;      // cumulative length along jpath
        Vector2 jc0{}, jc1{}, jc2{};
        float jy0 = 0.0f, jy2 = 0.0f;
        Color color{ 200, 200, 200, 255 };
        uint32_t rng = 1;
    };
    std::vector<Agent> agents;
public:
    void ClearAgents() { agents.clear(); simTime = 0.0f; tripsCompleted = 0; busStopsServedCount = 0; busSig = 0; pedTripsCount = boardingsCount = alightingsCount = parkVisitsCount = 0; trafficStats = TrafficStats{}; }
    // Runs one traffic step without Play mode (tests).
    void TrafficStepForTest(float dt) { StepTraffic(dt); }
    const std::vector<int>& RouteDestinations() { PickDestinationPool(); return routes.pool; }
    // Travel time (s) from every node to `dest` along allowed one-way/car roads (infinity = unreachable).
    // avoidEdge >= 0: that road is not used to get there (buses must reach a stop road's start from elsewhere).
    const std::vector<float>& RouteField(int dest, int avoidEdge = -1);
    // Walking distance (m) from every node to `dest` on foot: every road but highways, in both directions.
    const std::vector<float>& RouteFieldWalk(int dest);
private:
    float trafficClock = 0.0f;
    int tripsCompleted = 0;
    int busStopsServedCount = 0;
    int pedTripsCount = 0, boardingsCount = 0, alightingsCount = 0, parkVisitsCount = 0;
    uint32_t nextBusId = 1;
    uint64_t busSig = 0;   // signature of lines + stops + roads: buses are respawned when it changes
    uint64_t graphVersion = 0;   // bumped on every rebuild: invalidates the cached routes
    struct RouteCache {
        uint64_t version = ~0ull;
        std::vector<int> pool;                              // popular destination nodes (downtown weighs more)
        std::vector<int> parkNodes;                         // road nodes at a corner of a park or plaza (pedestrians like to visit)
        std::unordered_map<int64_t, std::vector<float>> dist;   // dest node -> travel time field
    } routes;
    void PickDestinationPool();
    void StartParkVisit(Agent& a, int node, const Vector3& from);   // at a park corner: maybe walk in, sit a while, walk back
    void PickPedTrip(Agent& a, bool allowBus);       // gives a pedestrian its next destination (or a bus stop to catch)
    int PedKerbLane(int stopIdx) const;              // which sidewalk (0/1) of the stop's road the shelter is on
    int PickDestination(uint32_t& rng);
    Vector3 trafficFocus{};      // camera position (set while drawing), drives the traffic detail distance
    Vector3 lodFocus{};             // the main camera position of the last frame (distance LOD of far tiles)
    bool hasLodFocus = false;
    bool hasTrafficFocus = false;
    unsigned trafficFrame = 0;
    float simTime = 0.0f;       // seconds of Play-mode traffic simulated
public:
    struct TrafficStats { int cars = 0, nearCars = 0, farCars = 0, peds = 0, stopped = 0; float avgSpeed = 0.0f, minGap = 0.0f, stepMs = 0.0f, maxWalkerSpeed = 0.0f; int overlaps = 0, jumps = 0, trips = 0, buses = 0, busStopsServed = 0, riders = 0, queued = 0, pedTrips = 0, boardings = 0, alightings = 0, parkVisits = 0, inParks = 0; };
    const TrafficStats& GetTrafficStats() const { return trafficStats; }
private:
    TrafficStats trafficStats;
    void LanePose(int edge, bool fwd, float s, float lane, bool car, Vector3& pos, Vector2& heading) const;
    float CarPitch(const Agent& a) const;   // tilt of a car or bus along the road (rad, nose up positive)
public:
    // Traffic signals: sector (0..7) of the road `edge` as seen from `node`, and the light shown to
    // traffic arriving along it (0 red, 1 amber, 2 green).
    int ApproachSector(int node, int edge) const;
    int SignalState(int node, int edge) const;
    float EdgeSpeedLimit(int edge) const;
    // Distance from a junction node along road `edge` to where its asphalt strip starts (the end of the junction plate).
    float ArmClear(int edge) const;
    float RoundaboutRadius(int node) const;   // radius of the roundabout's central island
    float RoundaboutRingWidth(int node) const; // width of the circulating roadway
    float RoundaboutOuterRadius(int node, bool asphalt) const;
    // Sets the roundabout's size (0 = auto) and look, then rebuilds.
    void SetRoundaboutProps(int node, float island, float ring, bool splitters, bool concrete);
    // True when the circle of radius r around (x,z) touches a roundabout's ring (buildings keep clear of it).
    bool TouchesRoundabout(const Vector2& p, float r) const; // outer edge of the roadway (asphalt) or of the sidewalk ring (slab)
    void SetEdgeTurnLanes(int edge, uint16_t turnA, uint16_t turnB, float speedLimit);
    float SignalClock() const { return trafficClock; }
private:
    void PlanJunction(Agent& a);
    void StepTraffic(float dt);
    void SpawnAgent(Agent& a, bool car);
    Color PickCarColor(const Agent& a) const;
    void AgentTarget(const Agent& a, Vector3& pos, float& yaw) const;
    uint64_t nextBlockId = 1; // 0 is reserved/invalid
    std::unordered_map<uint64_t, BuildingOverride> buildingOverrides; // key: (blockId<<20)|slot
    std::vector<PlacedBuilding> placed;
    std::vector<District> districts;
    GeometryProblems problemsCache;
    double problemsTime = -1e9;
    std::vector<BusStop> busStops;
    std::vector<BusLine> busLines;
    void ResolveBusStops();
    std::vector<int> RouteNodes(int from, int to);   // node path along the fastest route (empty if unreachable)
    std::unordered_map<uint64_t, int> blockDistricts;   // painted block id -> district index
    bool collisionEnabled = true;
    std::unordered_map<uint64_t, BlockKind> blockKinds; // key: block id; absent = Auto
    static uint64_t OverrideKey(uint64_t blockId, int slot) { return (blockId << 20) | (uint64_t)(uint32_t)slot; }
    void ApplyBuildingOverrides(Block& block); // post-pass after LayoutBlock fills block.buildings
    void RefreshBuildingTile(uint64_t blockId); // rebuilds the one tile a block lives in after an override edit
    // Dead-end road spurs (pruned from face extraction); buildings keep clear of them.
    std::vector<std::pair<Vector2, Vector2>> spurSegs;

    // Spatial tiles. Each tile owns the road/pad/park mesh chunks and the
    // building instances of the elements assigned to it (by their centre) plus a
    // tight bounding box, so Draw() culls per tile and an edit only regenerates
    // the tiles it touches. Building tint is packed into each instance transform.
    // CPU-side collision geometry of a tile (only filled when collision is on).
    struct CollBox { Vector3 center; Vector3 half; float angleY; int shape; };
    struct TileCollision {
        std::vector<CollBox> buildings;             // oriented boxes; non-box shapes become hulls
        std::vector<Vector3> surfVerts;             // road/pad/park surface triangles
        std::vector<int> surfIdx;
        std::vector<unsigned char> surfSkip;        // per surface triangle (as in surfIdx): 1 = does not shape the terrain (bridge span)
        std::vector<float> surfLayer;               // per surface vertex: the road shader's stack layer (pad 0.06, asphalt 0.10 ...)
    };
    struct TilePhysics;                             // Box3D body/hulls/mesh (City.cpp)
    struct Tile {
        TileCollision coll;
        std::shared_ptr<TilePhysics> phys;          // live only while a physics world is attached
        std::vector<int> blocks, edges, nodes;      // member elements
        std::vector<Model> roadModels;              // 16-bit-safe indexed chunks (GPU)
        std::vector<Mesh> raw;                      // CPU-only chunks awaiting UploadTile()
        std::vector<Matrix> inst[kBuildingShapes];  // per-shape instance transforms
        unsigned int instVbo[kBuildingShapes] = {}; // persistent GPU instance buffers
        int instVboCount[kBuildingShapes] = {};
        std::vector<Matrix> farInst;                // every building as a plain box (what a distant tile draws: one buffer, no props)
        unsigned int farVbo = 0;
        int farVboCount = 0;
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
    void CreateTilePhysics(Tile& t);   // main thread; needs an attached world
    void DestroyTilePhysics(Tile& t);

    // Background rebuild (see RebuildAll).
    struct RebuildJob;
    std::unique_ptr<RebuildJob> rebuild;
    float loadingMessageDelay = 5.0f;
    uint64_t rebuildRequestId = 0;
    std::chrono::steady_clock::time_point rebuildStarted{};   // when the running rebuild was first requested (kept while requests coalesce)
    std::atomic<float>* buildProgress = nullptr;               // set on the worker City: where ComputeAllCPU reports 0..1
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

// Runs the traffic simulation (cars, buses, pedestrians) as in Play mode, without a physics world. For tests.
void SetTrafficRunning(bool on);
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