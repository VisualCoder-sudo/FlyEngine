#include "../../include/CityGen/City.hpp"
#include "../../include/CityGen/CityEditor.hpp"
#include "../../include/CityGen/CityGeometry.hpp"
#include "../../include/Engine.hpp"
#include "../../include/Engine/Graphics.hpp"
#include "../../include/Engine/Frontend/ui.hpp"

#include "rlgl.h"

#include <chrono>
#include <climits>
#include <cmath>
#include <iomanip>
#include <cstdint>
#include <future>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>

namespace city {

// The whole road/pad/parks slab sits this small base height above the ground
// so it separates cleanly from the terrain/ground plane in depth at any camera
// distance (the layer-to-layer gaps below are tiny by design).
constexpr float kRoadElevation = 0.04f;

// The city is stored in square tiles (roads, pads and buildings assigned by the
// centre of each element). A tile owns its own road mesh + building instances
// and a tight bounding box, so the renderer can frustum-cull whole tiles and an
// edit only has to regenerate the few tiles it touches.
constexpr float kTileSize = 96.0f;
constexpr int64_t kNoTile = INT64_MIN;

// Cities with at least this many road nodes rebuild on a worker thread.
constexpr size_t kAsyncRebuildNodes = 500;
// Max time per frame spent uploading a finished background rebuild to the GPU.
constexpr double kUploadBudgetMs = 3.0;

// ---------------------------------------------------------------------------
// Shared palette for instanced building tints.
// ---------------------------------------------------------------------------
const Color kBuildingTints[kBuildingColorBuckets] = {
    { 210, 205, 195, 255 }, // concrete light
    { 160, 158, 158, 255 }, // concrete gray
    { 190, 180, 160, 255 }, // sand
    { 130, 140, 150, 255 }, // steel blue-gray
    { 175, 150, 130, 255 }, // brick
    { 120, 125, 135, 255 }, // dark slate
    { 205, 190, 175, 255 }, // warm stone
    { 150, 165, 180, 255 }, // pale blue
};

bool operator==(const CityParams& a, const CityParams& b) {
    return a.gridX == b.gridX && a.gridZ == b.gridZ &&
           a.cellSize == b.cellSize && a.organic == b.organic &&
           a.organicStrength == b.organicStrength &&
           a.organicScale == b.organicScale &&
           a.noiseOctaves == b.noiseOctaves && a.seed == b.seed &&
           a.lanes == b.lanes && a.laneWidth == b.laneWidth &&
           a.sidewalk == b.sidewalk && a.avgHeight == b.avgHeight &&
           a.heightVariance == b.heightVariance &&
           a.buildingSize == b.buildingSize && a.buildingGap == b.buildingGap &&
           a.parkThreshold == b.parkThreshold && a.parkInset == b.parkInset &&
           a.cornerRadius == b.cornerRadius;
}
bool operator!=(const CityParams& a, const CityParams& b) { return !(a == b); }

// ---------------------------------------------------------------------------
// Small deterministic integer hash used for heights/tints (stable across
// rebuilds for a given seed + coordinates).
// ---------------------------------------------------------------------------
namespace {

uint32_t Mix(uint32_t a, uint32_t b) {
    a = (a ^ b) * 0x2127ab2d;
    a ^= a >> 15;
    return a;
}

uint32_t CoordHash(int cx, int cz, int seed) {
    return Mix((uint32_t)seed ^ 0x9e3779b9u, Mix((uint32_t)(cx * 0x1f1f1f) , (uint32_t)(cz * 0x9e3d9)) + 0x85ebca6bu);
}

float CoordHash01(int cx, int cz, int seed) {
    uint32_t h = CoordHash(cx, cz, seed);
    return (h & 0xffffff) / (float)0x1000000;
}

} // namespace

// ---------------------------------------------------------------------------
// Mesh builder for the static (vertex-color) road/block/park geometry.
// ---------------------------------------------------------------------------
namespace {

struct MeshBuilder {
    std::vector<float> verts, texcoords, normals;
    std::vector<unsigned char> colors;
    std::vector<int> indices;

    void Vertex(const Vector3& p, Color c) {
        verts.push_back(p.x); verts.push_back(p.y); verts.push_back(p.z);
        texcoords.push_back(0.0f); texcoords.push_back(0.0f);
        normals.push_back(0.0f); normals.push_back(1.0f); normals.push_back(0.0f);
        colors.push_back(c.r); colors.push_back(c.g); colors.push_back(c.b); colors.push_back(c.a);
    }

    void Triangle(int a, int b, int c) {
        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(c);
    }

    void Quad(const Vector3& a, const Vector3& b, const Vector3& c, const Vector3& d, Color col) {
        int base = (int)(verts.size() / 3);
        Vertex(a, col); Vertex(b, col); Vertex(c, col); Vertex(d, col);
        Triangle(base, base + 1, base + 2);
        Triangle(base, base + 2, base + 3);
    }

void Fan(const std::vector<Vector3>& pts, Color col) {
        if (pts.size() < 3) return;
        int base = (int)(verts.size() / 3);
        for (const auto& p : pts) Vertex(p, col);
        // Triangle orientation must match the strip quads (ccw in (0,1,0)-normal
        // convention): (base, i+1, i). Flipping to (base, i, i+1) makes the fan
        // face downward under raylib's backface culling and the junction plate
        // disappears (a hole where the trimmed strips meet).
        for (size_t i = 1; i + 1 < pts.size(); i++) {
            Triangle(base, base + (int)(i + 1), base + (int)i);
        }
    }

    void Clear() {
        verts.clear(); texcoords.clear(); normals.clear(); colors.clear(); indices.clear();
    }
};

} // namespace


// ---------------------------------------------------------------------------
// Frustum culling against the CURRENT rlgl matrices, so the same code culls the
// main view, the mirrored reflection view and the light's ortho box (shadow pass).
// ---------------------------------------------------------------------------
namespace {

struct Frustum {
    float pl[6][4]{};
    bool valid = false;

    bool Intersects(const Vector3& mn, const Vector3& mx) const {
        if (!valid) return true;
        for (int i = 0; i < 6; i++) {
            const float* p = pl[i];
            const float x = p[0] >= 0.0f ? mx.x : mn.x;
            const float y = p[1] >= 0.0f ? mx.y : mn.y;
            const float z = p[2] >= 0.0f ? mx.z : mn.z;
            if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0.0f) return false;
        }
        return true;
    }
};

Frustum ExtractFrustum(const Matrix& view, const Matrix& proj) {
    Frustum f;
    const Matrix m = MatrixMultiply(view, proj);
    // clip = M * (x,y,z,1); the rows below are the four clip components.
    const float r0[4] = { m.m0, m.m4, m.m8,  m.m12 };
    const float r1[4] = { m.m1, m.m5, m.m9,  m.m13 };
    const float r2[4] = { m.m2, m.m6, m.m10, m.m14 };
    const float r3[4] = { m.m3, m.m7, m.m11, m.m15 };
    if (r3[0] == 0.0f && r3[1] == 0.0f && r3[2] == 0.0f && r3[3] == 0.0f) return f; // no valid projection
    const float* rows[3] = { r0, r1, r2 };
    for (int a = 0; a < 3; a++) {
        for (int k = 0; k < 4; k++) {
            f.pl[a * 2 + 0][k] = r3[k] + rows[a][k]; // +w >= -axis
            f.pl[a * 2 + 1][k] = r3[k] - rows[a][k];
        }
    }
    f.valid = true;
    return f;
}

} // namespace

// ---------------------------------------------------------------------------
// City
// ---------------------------------------------------------------------------
struct City::RebuildJob {
    std::unique_ptr<City> work;   // private City the worker computes into
    std::future<void> fut;        // declared after `work` so it is joined first on destruction
    uint64_t requestId = 0;
    bool computing = true;
    std::vector<int64_t> keys;    // tiles to upload
    size_t next = 0;
};

City::City() = default;
City::~City() { ClearGeometry(); }

void City::Update(float) { PumpRebuild(); }

// CPU-side reset only (safe on a worker thread: touches no GPU state).
void City::ResetTileCPU(Tile& t) {
    for (Mesh& m : t.raw) {
        std::free(m.vertices); std::free(m.texcoords); std::free(m.normals);
        std::free(m.colors); std::free(m.indices);
    }
    t.raw.clear();
    for (auto& v : t.inst) v.clear();
    t.hasRoad = false;
    t.hasBldg = false;
    t.roadMin = t.roadMax = t.bldgMin = t.bldgMax = Vector3{ 0.0f, 0.0f, 0.0f };
}

// Frees GPU resources too (main thread only).
void City::DestroyTile(Tile& t) {
    bool hadGpu = false;
    for (auto& m : t.roadModels) {
        if (m.meshCount > 0) { UnloadModel(m); hadGpu = true; }
    }
    t.roadModels.clear();
    for (int s = 0; s < kBuildingShapes; s++) {
        if (t.instVbo[s] != 0) { gfx::DestroyInstanceBuffer(t.instVbo[s]); hadGpu = true; }
        t.instVbo[s] = 0;
        t.instVboCount[s] = 0;
    }
    if (hadGpu) gfx::MarkShadowsDirty();
    ResetTileCPU(t);
}

void City::DestroyAllTiles() {
    for (auto& kv : tiles) DestroyTile(kv.second);
    tiles.clear();
}

void City::ClearGeometry() {
    CancelRebuild();
    DestroyAllTiles();
    nodeTile.clear();
    edgeTile.clear();
    blockTile.clear();
    nodeEdges.clear();
    nodeBlocks.clear();
    nodeRing.clear();
    edgeSpur.clear();
    hasGeometry = false;
}

void City::ClearGraph() {
    nodes.clear();
    edges.clear();
    blocks.clear();
    buildingOverrides.clear();
    nextBlockId = 1;
    ClearGeometry();
}

Vector2 City::NodePos(int i) const {
    static const Vector2 kZero{};
    if (i < 0 || (size_t)i >= nodes.size()) return kZero;
    return nodes[i].pos;
}

Vector2 City::EdgeMidpoint(int i) const {
    static const Vector2 kZero{};
    if (i < 0 || (size_t)i >= edges.size()) return kZero;
    return Vector2Scale(Vector2Add(nodes[edges[i].a].pos, nodes[edges[i].b].pos), 0.5f);
}

void City::GenerateGrid(const Vector2& origin) {
    CityParams p = params;
    p.gridX = Clamp(p.gridX, 2, 999);
    p.gridZ = Clamp(p.gridZ, 2, 999);
    p.lanes = Clamp(p.lanes, 1, 8);
    p.cornerRadius = std::max(p.cornerRadius, 0.0f);
    params = p;

    const int nx = p.gridX + 2;
    const int nz = p.gridZ + 2;
    const float spacing = std::max(p.cellSize, 5.0f);

    // Regular grid centered on the origin of the (nx-1, nz-1) cell box.
    std::vector<std::vector<Vector2>> grid(nx, std::vector<Vector2>(nz));
    const float x0 = -((nx - 1) * spacing) * 0.5f;
    const float z0 = -((nz - 1) * spacing) * 0.5f;
    for (int ix = 0; ix < nx; ix++)
        for (int iz = 0; iz < nz; iz++)
            grid[ix][iz] = { x0 + ix * spacing, z0 + iz * spacing };

    if (p.organic) {
        const float warp = p.organicStrength * spacing * 0.6f;
        for (int ix = 0; ix < nx; ix++)
            for (int iz = 0; iz < nz; iz++)
                grid[ix][iz] = citygeom::OrganicWarp(grid[ix][iz], warp,
                                                     std::max(p.organicScale, 8.0f),
                                                     std::max(p.noiseOctaves, 1), p.seed);
    }

    ClearGraph();
    std::vector<std::vector<int>> nodeIdx(nx, std::vector<int>(nz, -1));
    for (int ix = 0; ix < nx; ix++)
        for (int iz = 0; iz < nz; iz++) {
            RoadNode n;
            n.pos = grid[ix][iz];
            n.boundary = (ix == 0 || ix == nx - 1 || iz == 0 || iz == nz - 1);
            nodeIdx[ix][iz] = (int)nodes.size();
            nodes.push_back(n);
        }
    for (int ix = 0; ix < nx; ix++)
        for (int iz = 0; iz < nz; iz++) {
            int idx = nodeIdx[ix][iz];
            if (ix + 1 < nx) edges.push_back({ idx, nodeIdx[ix + 1][iz], 0 });
            if (iz + 1 < nz) edges.push_back({ idx, nodeIdx[ix][iz + 1], 0 });
        }

    // Reposition so the mean of the interior nodes lands exactly on `origin`
    // (the requested spawn point).
    Vector2 center{};
    int inner = 0;
    for (int ix = 1; ix < nx - 1; ix++)
        for (int iz = 1; iz < nz - 1; iz++) {
            center = Vector2Add(center, grid[ix][iz]);
            inner++;
        }
    if (inner > 0) {
        Vector2 delta = Vector2Subtract(origin, Vector2Scale(center, 1.0f / (float)inner));
        for (auto& n : nodes) n.pos = Vector2Add(n.pos, delta);
    }

    RebuildAll();
}

void City::RebuildAll() {
    if (nodes.size() >= kAsyncRebuildNodes) {
        RequestRebuild();          // big city: build on a worker, keep drawing the old one
        return;
    }
    CancelRebuild();
    DestroyAllTiles();
    ComputeAllCPU();
    UploadAllTiles();
    hasGeometry = true;
}

// Graph -> junction flags -> blocks -> buildings -> tiles (CPU data only).
// Touches nothing but this City's own members, so it can run on a worker
// thread against a private City holding a copy of the graph.
void City::ComputeAllCPU() {
    BuildAdjacency();
    ComputeJunctionFlags();
    ComputeBlocks();
    LayoutBuildings();
    AssignTiles();
    for (auto& kv : tiles) ComputeTileCPU(kv.second);
}

void City::UploadAllTiles() {
    for (auto& kv : tiles) UploadTile(kv.second);
}

void City::RequestRebuild() {
    rebuildRequestId++;
    if (!rebuild) StartRebuildJob();
}

void City::StartRebuildJob() {
    auto job = std::make_unique<RebuildJob>();
    job->work = std::make_unique<City>();
    job->work->params = params;
    job->work->nodes = nodes;
    job->work->edges = edges;
    // Carried over so the worker's ComputeBlocks() can match its freshly built
    // blocks back to these ids (by node overlap) and so its building overrides
    // pass has the same map AdoptRebuild will keep.
    job->work->blocks = blocks;
    job->work->nextBlockId = nextBlockId;
    job->work->buildingOverrides = buildingOverrides;
    job->requestId = rebuildRequestId;
    City* w = job->work.get();
    job->fut = std::async(std::launch::async, [w]() { w->ComputeAllCPU(); });
    rebuild = std::move(job);
}

void City::CancelRebuild() {
    rebuildRequestId++;
    rebuild.reset();   // joins the worker (future dtor) then frees the private City
}

// Called every frame: when the worker is done, upload its tiles a few per frame
// (old geometry keeps drawing), then swap the finished result in atomically.
void City::PumpRebuild() {
    if (!rebuild) return;
    RebuildJob& j = *rebuild;
    if (j.computing) {
        if (j.fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        j.fut.get();
        j.computing = false;
        if (j.requestId == rebuildRequestId && j.work->nodes.size() == nodes.size()) {
            for (const auto& kv : j.work->tiles) j.keys.push_back(kv.first);
        }
    }
    // Graph changed while building/uploading: throw the result away and rebuild latest.
    if (j.requestId != rebuildRequestId || j.work->nodes.size() != nodes.size()) {
        rebuild.reset();
        StartRebuildJob();
        return;
    }

    const auto t0 = std::chrono::steady_clock::now();
    while (j.next < j.keys.size()) {
        j.work->UploadTile(j.work->tiles[j.keys[j.next++]]);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > kUploadBudgetMs) break;
    }
    if (j.next >= j.keys.size()) {
        AdoptRebuild(*j.work);
        rebuild.reset();
    }
}

// Swap a finished, fully uploaded private City's derived data into this one.
void City::AdoptRebuild(City& w) {
    DestroyAllTiles();
    for (size_t i = 0; i < nodes.size(); i++) nodes[i].junction = w.nodes[i].junction;
    blocks = std::move(w.blocks);
    nextBlockId = std::max(nextBlockId, w.nextBlockId);
    spurSegs = std::move(w.spurSegs);
    nodeEdges = std::move(w.nodeEdges);
    nodeBlocks = std::move(w.nodeBlocks);
    nodeRing = std::move(w.nodeRing);
    edgeSpur = std::move(w.edgeSpur);
    nodeTile = std::move(w.nodeTile);
    edgeTile = std::move(w.edgeTile);
    blockTile = std::move(w.blockTile);
    tiles = std::move(w.tiles);
    w.tiles.clear();
    hasGeometry = true;
    gfx::MarkShadowsDirty();
}

void City::BuildAdjacency() {
    nodeEdges.assign(nodes.size(), {});
    for (int i = 0; i < (int)edges.size(); i++) {
        nodeEdges[(size_t)edges[i].a].push_back(i);
        nodeEdges[(size_t)edges[i].b].push_back(i);
    }
}

// A node is a junction when its incident edges don't all share one direction.
// Distinct directions are collected from unit edge normals and deduped by
// |dot| > 0.999 (opposite parallel edges are the same street).
bool City::JunctionFlagFor(int ni) const {
    std::vector<Vector2> normals;
    for (int ei : nodeEdges[(size_t)ni]) {
        const RoadEdge& e = edges[(size_t)ei];
        const int other = (e.a == ni) ? e.b : e.a;
        Vector2 d = Vector2Normalize(Vector2Subtract(nodes[(size_t)other].pos, nodes[(size_t)ni].pos));
        if (d.x == 0.0f && d.y == 0.0f) continue;
        const Vector2 n = { -d.y, d.x };
        bool dup = false;
        for (const Vector2& u : normals)
            if (fabsf(Vector2DotProduct(u, n)) > 0.999f) { dup = true; break; }
        if (!dup) normals.push_back(n);
    }
    return normals.size() >= 2;
}

void City::ComputeJunctionFlags() {
    for (int i = 0; i < (int)nodes.size(); i++) nodes[(size_t)i].junction = JunctionFlagFor(i);
}

// Neighbour node ids of `v` sorted by outgoing angle. The face structure of the
// planar graph depends only on this cyclic order at every vertex.
std::vector<int> City::AngularRing(int v) const {
    std::vector<std::pair<float, int>> a;
    for (int ei : nodeEdges[(size_t)v]) {
        const RoadEdge& e = edges[(size_t)ei];
        const int o = (e.a == v) ? e.b : e.a;
        const Vector2 d = Vector2Subtract(nodes[(size_t)o].pos, nodes[(size_t)v].pos);
        a.push_back({ atan2f(d.y, d.x), o });
    }
    std::sort(a.begin(), a.end());
    std::vector<int> ring;
    ring.reserve(a.size());
    for (const auto& p : a) ring.push_back(p.second);
    return ring;
}

void City::ComputeBlocks() {
    // Match new blocks back to old ones by majority node-index overlap, so a
    // block's persistent id (and therefore its building overrides) survives a
    // rebuild as long as that block's node set is still mostly the same --
    // even though `blocks` itself is rebuilt from scratch below. A node index
    // stays meaningful here because ComputeBlocks never mutates `nodes` itself.
    std::unordered_map<int, std::vector<uint64_t>> nodeToOldBlock;
    std::unordered_map<uint64_t, int> oldBlockSize;
    for (const Block& ob : blocks) {
        oldBlockSize[ob.id] = (int)ob.nodes.size();
        for (int idx : ob.nodes) nodeToOldBlock[idx].push_back(ob.id);
    }
    blocks.clear();
    std::vector<Vector2> pos;
    pos.reserve(nodes.size());
    for (const auto& n : nodes) pos.push_back(n.pos);
    // Dead-end spurs (degree-1 chains) bound no face; feeding them to the face
    // walker makes the block polygon run down both sides of the spur (a
    // zero-width slit) and buildings then straddle the road. Prune them here and
    // keep them in spurSegs so the layout can steer around them.
    spurSegs.clear();
    std::vector<int> deg(nodes.size(), 0);
    std::vector<char> live(edges.size(), 1);
    for (int i = 0; i < (int)edges.size(); i++) {
        deg[edges[i].a]++; deg[edges[i].b]++;
    }
    std::vector<int> pending;
    for (int i = 0; i < (int)nodes.size(); i++) if (deg[i] == 1) pending.push_back(i);
    while (!pending.empty()) {
        const int v = pending.back(); pending.pop_back();
        if (deg[v] != 1) continue;
        for (int ei : nodeEdges[(size_t)v]) {
            if (!live[ei]) continue;
            live[ei] = 0;
            const int o = (edges[ei].a == v) ? edges[ei].b : edges[ei].a;
            deg[v]--; deg[o]--;
            spurSegs.push_back({ nodes[edges[ei].a].pos, nodes[edges[ei].b].pos });
            if (deg[o] == 1) pending.push_back(o);
            break;
        }
    }

    edgeSpur.assign(edges.size(), 0);
    for (int i = 0; i < (int)edges.size(); i++) edgeSpur[(size_t)i] = live[(size_t)i] ? 0 : 1;

    std::vector<std::pair<int, int>> ed;
    ed.reserve(edges.size());
    for (int i = 0; i < (int)edges.size(); i++)
        if (live[i]) ed.push_back({ edges[i].a, edges[i].b });

    auto faces = citygeom::ExtractFaces(pos, ed);
    blocks.reserve(faces.size());
    for (auto& face : faces) {
        Block b;
        b.nodes = face;
        std::vector<Vector2> poly;
        for (int idx : face) poly.push_back(nodes[idx].pos);
        b.area = fabsf(citygeom::PolygonArea(poly));

        // Best-overlap old block, if any: tally candidate old ids seen through
        // this block's own nodes, then require a majority overlap both ways so
        // a block that has genuinely changed shape doesn't inherit a stale id
        // (and with it, overrides meant for a differently-shaped block).
        std::unordered_map<uint64_t, int> overlap;
        for (int idx : face) {
            auto it = nodeToOldBlock.find(idx);
            if (it == nodeToOldBlock.end()) continue;
            for (uint64_t oid : it->second) overlap[oid]++;
        }
        uint64_t bestId = 0; int bestCount = 0;
        for (const auto& kv : overlap) if (kv.second > bestCount) { bestCount = kv.second; bestId = kv.first; }
        const int newSize = (int)face.size();
        const int oldSize = bestId ? oldBlockSize[bestId] : 0;
        if (bestId != 0 && bestCount * 2 >= newSize && bestCount * 2 >= oldSize) {
            b.id = bestId;
        } else {
            b.id = nextBlockId++;
        }
        blocks.push_back(std::move(b));
    }

    // Lookup tables used by the incremental rebuild.
    nodeBlocks.assign(nodes.size(), {});
    for (int bi = 0; bi < (int)blocks.size(); bi++)
        for (int idx : blocks[(size_t)bi].nodes) {
            auto& v = nodeBlocks[(size_t)idx];
            if (v.empty() || v.back() != bi) v.push_back(bi);
        }
    nodeRing.assign(nodes.size(), {});
    for (int v = 0; v < (int)nodes.size(); v++) nodeRing[(size_t)v] = AngularRing(v);
}


// Records outlines whose triangulation does not cover their area (a visible hole or
// a partly filled park), with the exact vertices, so the bad case can be replayed.
static void ReportIncompleteFill(const char* what, const std::vector<Vector2>& poly,
                                 const std::vector<int>& tris) {
    double covered = 0.0;
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        const Vector2 &a = poly[tris[i]], &b = poly[tris[i + 1]], &c = poly[tris[i + 2]];
        covered += 0.5 * std::fabs((double)(b.x - a.x) * (c.y - a.y) - (double)(c.x - a.x) * (b.y - a.y));
    }
    const double want = std::fabs((double)citygeom::PolygonArea(poly));
    if (want < 1e-3 || covered >= want * 0.99) return;
    static int reported = 0;
    if (reported++ >= 20) return;
    TraceLog(LOG_WARNING, "CITY: %s outline only %.0f%% filled (%zu verts); dumped to city_geometry_warnings.txt",
             what, 100.0 * covered / want, poly.size());
    if (FILE* f = fopen("city_geometry_warnings.txt", "a")) {
        fprintf(f, "%s covered=%.3f area=%.3f verts=%zu:", what, covered, want, poly.size());
        for (const Vector2& v : poly) fprintf(f, " (%.4f,%.4f)", v.x, v.y);
        fputc('\n', f);
        fclose(f);
    }
}

namespace {
// Liang-Barsky: does segment a-b touch the axis-aligned rect?
bool SegHitsRect(Vector2 a, Vector2 b, float minx, float miny, float maxx, float maxy) {
    float t0 = 0.0f, t1 = 1.0f;
    const float dx = b.x - a.x, dy = b.y - a.y;
    auto clip = [&](float p, float q) {
        if (fabsf(p) < 1e-9f) return q >= 0.0f;
        const float r = q / p;
        if (p < 0.0f) { if (r > t1) return false; if (r > t0) t0 = r; }
        else          { if (r < t0) return false; if (r < t1) t1 = r; }
        return true;
    };
    return clip(-dx, a.x - minx) && clip(dx, maxx - a.x) &&
           clip(-dy, a.y - miny) && clip(dy, maxy - a.y);
}
} // namespace

void City::LayoutBuildings() {
    for (auto& block : blocks) LayoutBlock(block);
}

// Lays out one block (park or parcelled buildings). A pure function of the
// block's node polygon, the params and the dead-end spurs, which is what lets
// the incremental rebuild redo just the blocks touched by an edit.
void City::LayoutBlock(Block& block) {
    const CityParams& p = params;
    const float inset = p.InsetDist();
    const float roadW = p.RoadWidth();
    {
        // Block-local identity mix: every block hashes its parcels with a value
        // derived from its own node set, so neighboring blocks never share the
        // same height/tint/orientation pattern (no endless repeated tile look,
        // even for 999x999 grids).
        uint32_t blockMix = 2166136261u;
        for (int bidx : block.nodes) blockMix = (blockMix ^ (uint32_t)(bidx + 1)) * 16777619u;
        block.park = false;
        block.inset.clear();
        block.parkPoly.clear();
        block.buildings.clear();

        std::vector<Vector2> poly;
        poly.reserve(block.nodes.size());
        for (int idx : block.nodes) poly.push_back(nodes[idx].pos);

        // A block becomes a park when it is clearly oversized relative to the
        // road grid (~2.2x a regular cell) ??? the parkThreshold slider is the
        // absolute floor users can raise to force more parks.
        const float regularCellArea = p.cellSize * p.cellSize;
        const bool isPark = block.area >= std::max(p.parkThreshold, regularCellArea * 2.2f);

        if (isPark) {
            block.park = true;
            if (!citygeom::InsetPolygon(poly, std::min(p.parkInset, roadW * 0.4f), block.parkPoly)) {
                block.parkPoly = poly;
            }
            return;
        }

        if (!citygeom::InsetPolygon(poly, inset, block.inset)) {
            // Block too small to fit parking setbacks cleanly; draw nothing on it.
            return;
        }

        // Block orientation: buildings follow the street formed by the block's
        // longest edge, so angled roads produce angled buildings that stay put
        // (and resize) instead of vanishing when the road is moved.
        float ux = 1.0f, uz = 0.0f;
        float bestLen = 0.0f;
        for (size_t i = 0; i < poly.size(); i++) {
            const Vector2& A = poly[i];
            const Vector2& B = poly[(i + 1) % poly.size()];
            const float lx = B.x - A.x, lz = B.y - A.y;
            const float len = lx * lx + lz * lz;
            if (len > bestLen) { bestLen = len; ux = lx; uz = lz; }
        }
        if (bestLen <= 0.0f) return;
        const float invLen = 1.0f / sqrtf(bestLen);
        ux *= invLen; uz *= invLen;
        float theta = atan2f(-uz, ux); // rotate +Y so the box's local x = street dir

        const float cth = cosf(theta), sth = sinf(theta);
        const auto toLocal = [cth, sth](const Vector2& v) {
            return Vector2{ v.x * cth - v.y * sth, v.x * sth + v.y * cth };
        };
        const auto toWorld = [cth, sth](const Vector2& v) {
            return Vector2{ v.x * cth + v.y * sth, -v.x * sth + v.y * cth };
        };

        // Buildable polygon in the block's own frame.
        std::vector<Vector2> linset;
        linset.reserve(block.inset.size());
        for (const auto& v : block.inset) linset.push_back(toLocal(v));

        // Dead-end spurs crossing this block, in the block frame.
        std::vector<std::pair<Vector2, Vector2>> lspurs;
        if (!spurSegs.empty()) {
            float bx0 = 1e9f, bz0 = 1e9f, bx1 = -1e9f, bz1 = -1e9f;
            for (const auto& v : poly) {
                bx0 = fminf(bx0, v.x); bx1 = fmaxf(bx1, v.x);
                bz0 = fminf(bz0, v.y); bz1 = fmaxf(bz1, v.y);
            }
            for (const auto& sg : spurSegs) {
                if (fmaxf(sg.first.x, sg.second.x) < bx0 || fminf(sg.first.x, sg.second.x) > bx1 ||
                    fmaxf(sg.first.y, sg.second.y) < bz0 || fminf(sg.first.y, sg.second.y) > bz1) continue;
                lspurs.push_back({ toLocal(sg.first), toLocal(sg.second) });
            }
        }

        float minU = 1e9f, minV = 1e9f, maxU = -1e9f, maxV = -1e9f;
        for (const auto& v : linset) {
            minU = fminf(minU, v.x); maxU = fmaxf(maxU, v.x);
            minV = fminf(minV, v.y); maxV = fmaxf(maxV, v.y);
        }
        const float spanU = std::max(maxU - minU, 2.0f);
        const float spanV = std::max(maxV - minV, 2.0f);

        // Parcel the block into a connected grid; the count adapts to the span so
        // buildings never hit a hard snap grid and get culled when roads move.
        const float targetCell = p.buildingSize + p.buildingGap;
        const int kMinCells = 1, kMaxCells = 6;
        int nU = (int)roundf(spanU / targetCell);
        nU = std::max(kMinCells, std::min(kMaxCells, nU));
        int nV = (int)roundf(spanV / targetCell);
        nV = std::max(kMinCells, std::min(kMaxCells, nV));
        const float cellW = spanU / nU, cellD = spanV / nV;

        for (int i = 0; i < nU; i++) {
            for (int j = 0; j < nV; j++) {
                float hlu = minU + (i + 0.5f) * cellW;
                float hlv = minV + (j + 0.5f) * cellD;

                // Footprint shrinks in steps until all four corners fit the
                // buildable polygon, so irregular/organic blocks still yield
                // buildings instead of dropping them when roads move.
                const auto cornersIn = [&](float cu, float cv, float su, float sv) {
                    const float hx = su * 0.5f, hz = sv * 0.5f;
                    Vector2 cs[4] = {
                        { cu - hx, cv - hz }, { cu + hx, cv - hz },
                        { cu + hx, cv + hz }, { cu - hx, cv + hz },
                    };
                    for (const auto& cp : cs) {
                        if (!citygeom::PointInPolygon(cp, linset)) return false;
                    }
                    for (const auto& sg : lspurs) {
                        if (SegHitsRect(sg.first, sg.second, cu - hx - inset, cv - hz - inset,
                                        cu + hx + inset, cv + hz + inset)) return false;
                    }
                    return true;
                };

                const float sizeCap = std::max(p.buildingSize * 1.15f, 2.0f);
                const float baseFootU = std::max(1.2f, std::min(sizeCap, std::min(cellW - 0.15f, cellW * 3.0f)));
                const float baseFootV = std::max(1.2f, std::min(sizeCap, std::min(cellD - 0.15f, cellD * 3.0f)));
                const float frc[5] = { 1.0f, 0.72f, 0.5f, 0.33f, 0.22f };
                float sx = 0, sz = 0;
                for (int f = 0; f < 5 && sx == 0.0f; f++) {
                    const float cu = std::max(1.2f, baseFootU * frc[f]);
                    const float cv = std::max(1.2f, baseFootV * frc[f]);
                    if (cornersIn(hlu, hlv, cu, cv)) { sx = cu; sz = cv; }
                }
                if (sx == 0.0f) {
                    // Last resort: a minimal footprint at the buildable centroid.
                    if (linset.size() < 3) continue;
                    Vector2 cent{ 0.0f, 0.0f };
                    for (const auto& v : linset) { cent.x += v.x; cent.y += v.y; }
                    cent.x /= (float)linset.size();
                    cent.y /= (float)linset.size();
                    const float cu = std::min(p.buildingSize, fmaxf(spanU * 0.18f, 1.2f));
                    const float cv = std::min(p.buildingSize, fmaxf(spanV * 0.18f, 1.2f));
                    if (!cornersIn(cent.x, cent.y, cu, cv)) continue;
                    hlu = cent.x; hlv = cent.y;
                    sx = cu; sz = cv;
                }

                const Vector2 wc = toWorld({ hlu, hlv });

                int shape = kBuildingBox;
                float rotDelta = 0.0f;

                float height = p.avgHeight * (1.0f - p.heightVariance + 2.0f * p.heightVariance * CoordHash01(i, j, p.seed ^ (int)blockMix));
                height = Clamp(height, 1.0f, 220.0f);

                // Shape + wedge orientation. Parcels near a block corner (where
                // two streets meet) get a wedge whose flat hypotenuse faces the
                // intersection and whose apex tapers back into the block, so the
                // building reads as connected to both street frontages instead of
                // throwing a stray point out into the road.
                const float spanMin = std::min(spanU, spanV);
                const float cornerThresh = 0.30f * spanMin;
                const float nearU = std::min(hlu - minU, maxU - hlu);
                const float nearV = std::min(hlv - minV, maxV - hlv);
                const bool corner = nearU < cornerThresh;
                const bool edge = !corner && (nearU < cornerThresh || nearV < cornerThresh);

                uint32_t h = CoordHash(i, j, p.seed ^ (int)blockMix);
                    rotDelta = 0.0f;
                    if (corner) {
                        const int kind = (int)(h % 3u);
                        shape = kBuildingBox;
                        // Wrap the wedge fl at around whichever street runs past the
                        // parcel edge it hugs: the hypotenuse is made PARALLEL to
                        // that street line (never diagonal across the parcel, which
                        // is what previously let a corner poke past the parcel edge
                        // and read as "clipped" on angled/organic blocks). The apex
                        // then points into the block interior, so the flat face is
                        // the street frontage and the point tucks away from it.
                        const bool nearMinU = (hlu - minU) < (maxU - hlu);
                        const bool nearMinV = (hlv - minV) < (maxV - hlv);
                        const bool hugsU = ((h & 1u) == 0u); // hypotenuse parallel to a U-run street
                        // Apex must point straight away from the street the parcel
                        // hugs (into the block interior), never toward it. The wedge
                        // mesh's apex is the local (0.5,0.5) corner and its
                        // hypotenuse is the diagonal facing -x,-z; rotating by R
                        // maps the apex to:
                        //   R=-PI/4 -> +v?    R=3PI/4 -> -v     (apex along V, hyp || U)
                        //   R= PI/4 -> +u     R=5PI/4 -> -u     (apex along U, hyp || V)
                        // so the flat hypotenuse stays flush with the street frontage
                        // and the point tucks into the block (not into the road).
                        if (hugsU) {
                            rotDelta = 0.0f;
                        } else {
                            rotDelta = 0.0f;
                        }
                    } else if (edge || (h % 7u) == 3u) {
                        (void)(h % 4u);
                        shape = kBuildingBox;
                    }

                Building b;
                b.size = { sx, height, sz };
                b.center = { wc.x, height * 0.5f + kRoadElevation + 0.06f, wc.y }; // sit on the pad surface
                b.angleY = theta + rotDelta;
                b.shape = shape;
                b.colorBucket = (int)(h % (uint32_t)kBuildingColorBuckets);
                block.buildings.push_back(b);
            }
        }
    }
    ApplyBuildingOverrides(block);
}

// Reapplies any per-building edits on top of block.buildings, which LayoutBlock
// just filled from scratch. `slot` is each building's index in that vector, so
// this only does anything for a block whose overrides map still has entries at
// slots that exist -- an override pointing past the end (the block now has
// fewer buildings) is left in the map rather than dropped, in case a later
// node move restores the earlier shape.
void City::ApplyBuildingOverrides(Block& block) {
    if (block.id == 0 || buildingOverrides.empty()) return;
    for (int slot = 0; slot < (int)block.buildings.size(); slot++) {
        auto it = buildingOverrides.find(OverrideKey(block.id, slot));
        if (it == buildingOverrides.end()) continue;
        const BuildingOverride& ov = it->second;
        Building& b = block.buildings[(size_t)slot];
        if (ov.hasHeight) {
            const float baseY = b.center.y - b.size.y * 0.5f; // pad surface, unaffected by height
            b.size.y = std::max(ov.height, 0.5f);
            b.center.y = baseY + b.size.y * 0.5f;
        }
        if (ov.hasOffset) {
            b.center.x += ov.posOffset.x;
            b.center.z += ov.posOffset.y;
        }
    }
}

// Rebuilds the ONE tile that block `blockId` lives in, right after its
// buildings vector was touched by an override -- otherwise a change here would
// sit in `blocks` only, invisible, since the tile's road mesh and (crucially)
// its GPU instance transforms are a separate baked copy that only Draw() reads
// and only a tile rebuild refreshes. This is why overrides weren't previewing
// or, less obviously, weren't the geometry actually being saved either: a
// save writes `blocks`/overrides directly, so that part was always correct --
// only the rendered/exported tile mesh was stale until the next full rebuild.
void City::RefreshBuildingTile(uint64_t blockId) {
    for (int bi = 0; bi < (int)blocks.size(); bi++) {
        if (blocks[(size_t)bi].id != blockId) continue;
        if ((size_t)bi >= blockTile.size()) return;
        const int64_t key = blockTile[(size_t)bi];
        auto it = tiles.find(key);
        if (it != tiles.end()) BuildTile(it->second);
        return;
    }
}

void City::SetBuildingHeightOverride(uint64_t blockId, int slot, float height) {
    if (blockId == 0 || slot < 0) return;
    BuildingOverride& ov = buildingOverrides[OverrideKey(blockId, slot)];
    ov.hasHeight = true;
    ov.height = std::max(height, 0.5f);
    for (Block& b : blocks) if (b.id == blockId) { ApplyBuildingOverrides(b); break; }
    RefreshBuildingTile(blockId);
}

void City::SetBuildingOffsetOverride(uint64_t blockId, int slot, Vector2 offset) {
    if (blockId == 0 || slot < 0) return;
    BuildingOverride& ov = buildingOverrides[OverrideKey(blockId, slot)];
    ov.hasOffset = true;
    ov.posOffset = offset;
    for (Block& b : blocks) if (b.id == blockId) { ApplyBuildingOverrides(b); break; }
    RefreshBuildingTile(blockId);
}

void City::ClearBuildingOverride(uint64_t blockId, int slot) {
    buildingOverrides.erase(OverrideKey(blockId, slot));
    // Relay out the block from scratch so the cleared building goes back to its
    // procedural default instead of keeping the last-applied override values.
    for (Block& b : blocks) if (b.id == blockId) { LayoutBlock(b); break; }
    RefreshBuildingTile(blockId);
}

bool City::HasBuildingOverride(uint64_t blockId, int slot) const {
    return buildingOverrides.count(OverrideKey(blockId, slot)) != 0;
}

Vector2 City::GetBuildingOffsetOverride(uint64_t blockId, int slot) const {
    auto it = buildingOverrides.find(OverrideKey(blockId, slot));
    if (it == buildingOverrides.end() || !it->second.hasOffset) return Vector2{ 0.0f, 0.0f };
    return it->second.posOffset;
}

bool City::PickBuilding(const Ray& ray, int& outBlock, int& outSlot, float* outDist) const {
    int bestBlock = -1, bestSlot = -1;
    float bestDist = 1e30f;
    for (int bi = 0; bi < (int)blocks.size(); bi++) {
        const Block& block = blocks[(size_t)bi];
        for (int si = 0; si < (int)block.buildings.size(); si++) {
            const Building& b = block.buildings[(size_t)si];
            const BoundingBox box{
                { b.center.x - b.size.x * 0.5f, b.center.y - b.size.y * 0.5f, b.center.z - b.size.z * 0.5f },
                { b.center.x + b.size.x * 0.5f, b.center.y + b.size.y * 0.5f, b.center.z + b.size.z * 0.5f },
            };
            RayCollision col = GetRayCollisionBox(ray, box);
            if (col.hit && col.distance < bestDist) {
                bestDist = col.distance;
                bestBlock = bi;
                bestSlot = si;
            }
        }
    }
    if (bestBlock < 0) return false;
    outBlock = bestBlock;
    outSlot = bestSlot;
    if (outDist) *outDist = bestDist;
    return true;
}

// Builds (or rebuilds) one tile: road strips / junction plates for its edges and
// nodes, pads + parks for its blocks, and the instance lists for their buildings.
void City::ComputeTileCPU(Tile& t) {
    ResetTileCPU(t);
    MeshBuilder mb;

const float roadW = params.RoadWidth();
    const float asphaltW = params.lanes * params.laneWidth;
// Roads: a wide dark slab (sidewalk band) plus a centered asphalt ribbon.
    // At a junction the incident strips overlap coplanar in a small region
    // (grid, T, L or organic), which z-fights, so the strips are trimmed flush
    // at each junction end and one junction plate per layer fills the exact
    // band-intersection region. The old trim was a fixed half-width measured
    // ALONG the edge, which only matches the plate on 90-degree junctions; on
    // angled/organic layouts the plate sides sit further out (w/sin(angle)),
    // so strips stopped short and left the "awkward space" gap. Now every strip
    // corner is cut exactly where its half-width side line LEAVES the junction
    // plate polygon, so plate and strips meet flush at any angle. The old
    // "half-width offset in-place" trick (offsetting the endpoint sideways) is
    // NOT used -- it throws pokes out past the intersection on angled roads.
    {
        const std::vector<std::vector<int>>& incident = nodeEdges;

        const float slabHalf = roadW * 0.5f;
        const float aspHalf = asphaltW * 0.5f;
        const float cornerR = std::max(params.cornerRadius, 0.0f);

        // One junction (node with 2+ distinct road directions) at a given band
        // half-width w. The arms are sorted CCW by outward angle; between every
        // pair of neighbouring arms the two facing side lines meet at a corner C
        // which is filleted with a circular arc. Each arm's strip ends exactly at
        // the arc's tangent points, and the plate polygon is the loop
        //   right[i], left[i], arc(gap i)..., right[i+1], ...
        // so strips and plate always meet flush at any angle, and everything is a
        // pure function of the graph: move a node and the curves follow.
        struct JunctionGeom {
            bool valid = false;
            std::vector<int> edge;            // incident edge ids, CCW by outward angle
            std::vector<Vector2> left, right; // strip end corners per arm (w.r.t. outward dir)
            std::vector<Vector2> poly;        // plate boundary, CCW
        };

        auto buildJunction = [&](int ni, float w) {
            JunctionGeom g;
            if (!nodes[ni].junction) return g;
            const Vector2 c = nodes[ni].pos;
            struct Arm { int ei; Vector2 u; float ang; float len; };
            std::vector<Arm> arms;
            for (int ei : incident[ni]) {
                const RoadEdge& e = edges[ei];
                const int other = (e.a == ni) ? e.b : e.a;
                Vector2 dv = Vector2Subtract(nodes[other].pos, c);
                const float len = Vector2Length(dv);
                if (len < 1e-4f) continue;
                dv = Vector2Scale(dv, 1.0f / len);
                arms.push_back({ ei, dv, atan2f(dv.y, dv.x), len });
            }
            const int k = (int)arms.size();
            if (k < 2) return g;
            std::sort(arms.begin(), arms.end(), [](const Arm& x, const Arm& y) { return x.ang < y.ang; });

            g.edge.resize(k);
            g.left.resize(k);
            g.right.resize(k);
            std::vector<std::vector<Vector2>> arcs(k);

            for (int i = 0; i < k; i++) {
                const Arm& A = arms[i];
                const Arm& B = arms[(i + 1) % k];
                g.edge[i] = A.ei;
                const Vector2 nA = { -A.u.y, A.u.x }, nB = { -B.u.y, B.u.x };
                const Vector2 oA = Vector2Add(c, Vector2Scale(nA, w));
                const Vector2 oB = Vector2Subtract(c, Vector2Scale(nB, w));
                const float D = A.u.x * B.u.y - A.u.y * B.u.x; // > 0: concave gap (< 180 deg)
                Vector2 TA = oA, TB = oB;

                if (fabsf(D) > 1e-3f) {
                    const float phi = acosf(Clamp(Vector2DotProduct(A.u, B.u), -1.0f, 1.0f));
                    const float th = std::max(tanf(phi * 0.5f), 1e-4f);
                    const float sh = std::max(sinf(phi * 0.5f), 1e-4f);
                    const bool concave = D > 0.0f;
                    const float dmax = 0.5f * std::min(A.len, B.len);

                    // Corner param along the arms for band half-width ww.
                    auto cornerA = [&](float ww) {
                        const Vector2 pa = Vector2Add(c, Vector2Scale(nA, ww));
                        const Vector2 pb = Vector2Subtract(c, Vector2Scale(nB, ww));
                        const Vector2 dw = Vector2Subtract(pb, pa);
                        return (dw.x * B.u.y - dw.y * B.u.x) / D;
                    };

                    // Radius is decided on the asphalt band so slab and asphalt stay concentric.
                    float rAsp = concave ? cornerR : (cornerR > 0.0f ? cornerR + 2.0f * aspHalf : 0.0f);
                    if (rAsp > 0.0f) rAsp = std::min(rAsp, std::max(dmax - cornerA(aspHalf), 0.0f) * th);

                    const float a = cornerA(w);
                    if (a > dmax) {
                        // Sharp corner lies beyond the arm: cut straight across.
                        TA = Vector2Add(oA, Vector2Scale(A.u, dmax));
                        TB = Vector2Add(oB, Vector2Scale(B.u, dmax));
                    } else {
                        const Vector2 C = Vector2Add(oA, Vector2Scale(A.u, a));
                        float rr = 0.0f;
                        if (rAsp > 0.0f) {
                            const float delta = w - aspHalf;
                            rr = concave ? rAsp - delta : rAsp + delta;
                            if (rr < 0.05f) rr = 0.0f;
                        }
                        float t = rr / th;
                        if (a + t > dmax) { t = std::max(dmax - a, 0.0f); rr = t * th; }
                        TA = Vector2Add(C, Vector2Scale(A.u, t));
                        TB = Vector2Add(C, Vector2Scale(B.u, t));
                        if (rr > 0.05f) {
                            Vector2 bis = Vector2Add(A.u, B.u);
                            bis = Vector2Normalize(bis);
                            const Vector2 ctr = Vector2Add(C, Vector2Scale(bis, rr / sh));
                            const float a0 = atan2f(TA.y - ctr.y, TA.x - ctr.x);
                            const float a1 = atan2f(TB.y - ctr.y, TB.x - ctr.x);
                            float dA = a1 - a0;
                            while (dA > PI) dA -= 2.0f * PI;
                            while (dA <= -PI) dA += 2.0f * PI;
                            const int segs = std::max(2, (int)ceilf(fabsf(dA) / 0.3f));
                            for (int sIdx = 1; sIdx < segs; sIdx++) {
                                const float ang = a0 + dA * ((float)sIdx / (float)segs);
                                arcs[i].push_back({ ctr.x + cosf(ang) * rr, ctr.y + sinf(ang) * rr });
                            }
                        }
                    }
                }
                g.left[i] = TA;
                g.right[(i + 1) % k] = TB;
            }

            auto pushPoly = [&](const Vector2& p) {
                if (g.poly.empty() || Vector2Distance(g.poly.back(), p) > 1e-4f) g.poly.push_back(p);
            };
            for (int i = 0; i < k; i++) {
                pushPoly(g.right[i]);
                pushPoly(g.left[i]);
                for (const Vector2& p : arcs[i]) pushPoly(p);
            }
            g.valid = g.poly.size() >= 3;
            return g;
        };

        // Junction geometry is computed on demand (only for the nodes this tile
        // touches) and memoised for the duration of the tile build.
        std::unordered_map<int, JunctionGeom> slabC, aspC;
        auto J = [&](std::unordered_map<int, JunctionGeom>& cache, int ni, float w) -> const JunctionGeom& {
            auto it = cache.find(ni);
            if (it == cache.end()) it = cache.emplace(ni, buildJunction(ni, w)).first;
            return it->second;
        };

        // Strip end corners for arm `ei` at node ni (flush at the node when the
        // node is not a junction).
        auto endCorners = [&](const JunctionGeom& g, int ni, int ei, float w,
                              const Vector2& outward, Vector2& left, Vector2& right) {
            if (g.valid) {
                for (size_t j = 0; j < g.edge.size(); j++) {
                    if (g.edge[j] == ei) { left = g.left[j]; right = g.right[j]; return; }
                }
            }
            const Vector2 nn = { -outward.y, outward.x };
            left = Vector2Add(nodes[ni].pos, Vector2Scale(nn, w));
            right = Vector2Subtract(nodes[ni].pos, Vector2Scale(nn, w));
        };

        for (int ei : t.edges) {
            const RoadEdge& e = edges[(size_t)ei];
            const Vector2& A = nodes[e.a].pos;
            const Vector2& B = nodes[e.b].pos;
            const Vector2 d = Vector2Normalize(Vector2Subtract(B, A));
            if (d.x == 0.0f && d.y == 0.0f) continue;
            const Vector2 dRev = { -d.x, -d.y };

            auto emitStrip = [&](std::unordered_map<int, JunctionGeom>& cache, float w, float y, Color col) {
                Vector2 lA, rA, lB, rB;
                endCorners(J(cache, e.a, w), e.a, ei, w, d, lA, rA);
                endCorners(J(cache, e.b, w), e.b, ei, w, dRev, lB, rB);
                auto ok = [&]() {
                    return Vector2DotProduct(Vector2Subtract(lB, rA), d) >= 1e-3f &&
                           Vector2DotProduct(Vector2Subtract(rB, lA), d) >= 1e-3f;
                };
                if (!ok()) {
                    // Junctions so close their trimmed ends cross: use flush ends.
                    const Vector2 nn = { -d.y, d.x };
                    lA = Vector2Add(A, Vector2Scale(nn, w));
                    rA = Vector2Subtract(A, Vector2Scale(nn, w));
                    lB = Vector2Subtract(B, Vector2Scale(nn, w));
                    rB = Vector2Add(B, Vector2Scale(nn, w));
                    if (!ok()) return;
                }
                mb.Quad(Vector3{ rA.x, y, rA.y }, Vector3{ lA.x, y, lA.y },
                        Vector3{ rB.x, y, rB.y }, Vector3{ lB.x, y, lB.y }, col);
            };
            emitStrip(slabC, slabHalf, 0.02f + kRoadElevation, Color{ 62, 62, 66, 255 });
            emitStrip(aspC, aspHalf, 0.10f + kRoadElevation, Color{ 82, 82, 88, 255 });
        }

        // Junction plates (rounded corners live here): fan from the node centre.
        const float capSlabY = 0.04f + kRoadElevation; // above the slab strips
        const float capAspY = 0.12f + kRoadElevation; // above the asphalt strips
        auto emitPlate = [&](const JunctionGeom& g, int ni, float y, Color col) {
            if (!g.valid) return;
            std::vector<Vector3> pts;
            pts.reserve(g.poly.size() + 2);
            pts.push_back({ nodes[ni].pos.x, y, nodes[ni].pos.y });
            for (const Vector2& p : g.poly) pts.push_back({ p.x, y, p.y });
            pts.push_back({ g.poly[0].x, y, g.poly[0].y });
            mb.Fan(pts, col);
        };
        for (int ni : t.nodes) {
            emitPlate(J(slabC, ni, slabHalf), ni, capSlabY, Color{ 62, 62, 66, 255 });
            emitPlate(J(aspC, ni, aspHalf), ni, capAspY, Color{ 82, 82, 88, 255 });
        }
    }

    // Block pads (concrete) under the buildings.
    for (int bi : t.blocks) {
        const Block& block = blocks[(size_t)bi];
        std::vector<Vector2> poly;
        poly.reserve(block.nodes.size());
        for (int idx : block.nodes) poly.push_back(nodes[idx].pos);

        std::vector<int> tris;
        citygeom::TriangulateSimple(poly, tris);
        ReportIncompleteFill("pad", poly, tris);
        const Color padColor = block.park ? Color{ 108, 158, 94, 255 } : Color{ 158, 158, 162, 255 };
        for (size_t t = 0; t + 2 < tris.size(); t += 3) {
            int base = (int)(mb.verts.size() / 3);
            mb.Vertex({ poly[tris[t]].x, 0.06f + kRoadElevation, poly[tris[t]].y }, padColor);
            mb.Vertex({ poly[tris[t + 1]].x, 0.06f + kRoadElevation, poly[tris[t + 1]].y }, padColor);
            mb.Vertex({ poly[tris[t + 2]].x, 0.06f + kRoadElevation, poly[tris[t + 2]].y }, padColor);
            // Same up-facing winding as the strips (see Fan): base,base+2,base+1.
            mb.Triangle(base, base + 2, base + 1);
        }

        // Park grass sits above the pad (and below the roads).
        if (block.park && !block.parkPoly.empty()) {
            std::vector<int> ptris;
            citygeom::TriangulateSimple(block.parkPoly, ptris);
            ReportIncompleteFill("park", block.parkPoly, ptris);
            {
                // The grass should be roughly the pad minus an inset margin; far less means
                // the inset polygon itself is wrong. Dump both outlines for replay.
                const double padA = std::fabs((double)citygeom::PolygonArea(poly));
                const double grassA = std::fabs((double)citygeom::PolygonArea(block.parkPoly));
                double per = 0; for (size_t k = 0; k < poly.size(); k++) per += Vector2Distance(poly[k], poly[(k + 1) % poly.size()]);
                const double insetUsed = std::min(params.parkInset, params.RoadWidth() * 0.4f);
                const double expect = padA - per * insetUsed;
                static int rep = 0;
                if (grassA < 0.8 * expect && rep++ < 20) {
                    TraceLog(LOG_WARNING, "CITY: park grass %.0f vs expected %.0f (pad %.0f); dumped", grassA, expect, padA);
                    if (FILE* f = fopen("city_geometry_warnings.txt", "a")) {
                        fprintf(f, "PARKPAD:"); for (const Vector2& v : poly) fprintf(f, " (%.4f,%.4f)", v.x, v.y);
                        fprintf(f, "\nPARKGRASS:"); for (const Vector2& v : block.parkPoly) fprintf(f, " (%.4f,%.4f)", v.x, v.y);
                        fprintf(f, "\ninset=%.4f\n", insetUsed); fclose(f);
                    }
                }
            }
            const Color grassColor{ 108, 158, 94, 255 };
            for (size_t t = 0; t + 2 < ptris.size(); t += 3) {
                int base = (int)(mb.verts.size() / 3);
                mb.Vertex({ block.parkPoly[ptris[t]].x, 0.08f + kRoadElevation, block.parkPoly[ptris[t]].y }, grassColor);
                mb.Vertex({ block.parkPoly[ptris[t + 1]].x, 0.08f + kRoadElevation, block.parkPoly[ptris[t + 1]].y }, grassColor);
                mb.Vertex({ block.parkPoly[ptris[t + 2]].x, 0.08f + kRoadElevation, block.parkPoly[ptris[t + 2]].y }, grassColor);
                mb.Triangle(base, base + 2, base + 1);
            }
        }
    }

    // Buildings of this tile's blocks. Each instance carries its tint in the
    // otherwise-unused bottom row of its transform (m3,m7,m11), which the
    // instanced shader reads back -- so all colours of a shape share ONE draw call.
    {
        Vector3 bmin{ 1e30f, 1e30f, 1e30f }, bmax{ -1e30f, -1e30f, -1e30f };
        for (int bi : t.blocks) {
            for (const Building& b : blocks[(size_t)bi].buildings) {
                const int shape = (b.shape >= 0 && b.shape < kBuildingShapes) ? b.shape : kBuildingBox;
                const int color = (b.colorBucket >= 0 && b.colorBucket < kBuildingColorBuckets)
                                  ? b.colorBucket : 0;
                // raylib's DrawModelEx convention: Scale * Rotate * Translate. The unit
                // building mesh grows to b.size, rotates to follow the block street,
                // and moves so its footprint center sits at b.center.
                Matrix m = MatrixMultiply(
                    MatrixScale(b.size.x, b.size.y, b.size.z),
                    MatrixMultiply(MatrixRotateY(b.angleY),
                                   MatrixTranslate(b.center.x, b.center.y, b.center.z)));
                const Color tint = kBuildingTints[color];
                m.m3 = tint.r / 255.0f;
                m.m7 = tint.g / 255.0f;
                m.m11 = tint.b / 255.0f;
                t.inst[shape].push_back(m);

                const float r = 0.5f * sqrtf(b.size.x * b.size.x + b.size.z * b.size.z) * 1.3f + 0.5f;
                bmin.x = fminf(bmin.x, b.center.x - r); bmax.x = fmaxf(bmax.x, b.center.x + r);
                bmin.z = fminf(bmin.z, b.center.z - r); bmax.z = fmaxf(bmax.z, b.center.z + r);
                bmin.y = fminf(bmin.y, b.center.y - b.size.y * 0.5f - 0.5f);
                bmax.y = fmaxf(bmax.y, b.center.y + b.size.y * 0.5f + 0.5f);
                t.hasBldg = true;
            }
        }
        if (t.hasBldg) { t.bldgMin = bmin; t.bldgMax = bmax; }
    }

    if (mb.verts.empty()) return;

    // Tight bounds of the road mesh for culling.
    {
        Vector3 rmin{ 1e30f, 1e30f, 1e30f }, rmax{ -1e30f, -1e30f, -1e30f };
        for (size_t i = 0; i + 2 < mb.verts.size(); i += 3) {
            rmin.x = fminf(rmin.x, mb.verts[i]);     rmax.x = fmaxf(rmax.x, mb.verts[i]);
            rmin.y = fminf(rmin.y, mb.verts[i + 1]); rmax.y = fmaxf(rmax.y, mb.verts[i + 1]);
            rmin.z = fminf(rmin.z, mb.verts[i + 2]); rmax.z = fmaxf(rmax.z, mb.verts[i + 2]);
        }
        // Tiny pad: the road bias pulls the surface a few cm toward the camera.
        t.roadMin = { rmin.x - 0.5f, rmin.y - 0.5f, rmin.z - 0.5f };
        t.roadMax = { rmax.x + 0.5f, rmax.y + 0.5f, rmax.z + 0.5f };
        t.hasRoad = true;
    }

    // Give the model raylib-owned copies of the CPU buffers. raylib's
    // UnloadMesh() frees mesh.vertices/texcoords/normals/colors/indices and
    // the GPU side, so they must NOT point into the (short-lived) builder ???
    // the next rebuild would then double-free them and corrupt the heap.
    auto takeCopy = [](const std::vector<float>& src, std::size_t elemSize) -> void* {
        if (src.empty()) return nullptr;
        void* p = std::malloc(src.size() * elemSize);
        if (p) std::memcpy(p, src.data(), src.size() * elemSize);
        return p;
    };
    auto takeCopyUc = [](const std::vector<unsigned char>& src) -> void* {
        if (src.empty()) return nullptr;
        void* p = std::malloc(src.size() * sizeof(unsigned char));
        if (p) std::memcpy(p, src.data(), src.size() * sizeof(unsigned char));
        return p;
    };
    auto takeCopyU16 = [](const std::vector<unsigned short>& src) -> void* {
        if (src.empty()) return nullptr;
        void* p = std::malloc(src.size() * sizeof(unsigned short));
        if (p) std::memcpy(p, src.data(), src.size() * sizeof(unsigned short));
        return p;
    };

    // raylib 6 renders indexed meshes with 16-bit indices (GL_UNSIGNED_SHORT),
    // which caps a single mesh at 65535 vertices; past grid ~46 the mesh
    // builder exceeded that. The builder used to store unsigned short indices,
    // so vertex ids >= 65536 silently wrapped at build time and the roads on
    // the far side of the grid folded back onto the near side (roads only on
    // one side). The builder now keeps full-width indices; split the geometry
    // into several 16-bit-safe chunks and rebase each chunk's vertex ids back
    // to local u16 ids. Indexed drawing (glDrawElements) keeps the uploads
    // small -- roughly 1/3 of the old GL_SIGNED soup at equal triangle counts --
    // while every chunk stays within the index type at any grid size.
    const int kMaxChunkVerts = 60000; // leave headroom below the 65535 cap
    std::vector<Mesh>& rawMeshes = t.raw;
    {
        // remap is sized once and grows monotonically; beginChunk clears only
        // the verts the previous chunk touched (not the whole array), which
        // avoids an O(all verts) memset per chunk at big grid sizes.
        std::vector<int> remap(mb.verts.size() / 3, (int)-1);
        std::vector<int> touched;
        std::vector<float> cv, ct, cn;
        std::vector<unsigned char> cc;
        std::vector<unsigned short> ci;

        auto beginChunk = [&]() {
            for (int id : touched) remap[id] = -1;
            touched.clear();
            cv.clear(); ct.clear(); cn.clear(); cc.clear(); ci.clear();
        };
        auto finishChunk = [&]() {
            if (ci.empty()) return; // nothing accumulated
            Mesh m = { 0 };
            m.vertexCount = (int)(cv.size() / 3);
            m.triangleCount = (int)(ci.size() / 3);
            m.vertices = (float*)takeCopy(cv, sizeof(float));
            m.texcoords = (float*)takeCopy(ct, sizeof(float));
            m.normals = (float*)takeCopy(cn, sizeof(float));
            m.colors = (unsigned char*)takeCopyUc(cc);
            m.indices = (unsigned short*)takeCopyU16(ci);
            rawMeshes.push_back(m);
        };
        auto addVert = [&](int id) {
            if (remap[id] < 0) {
                remap[id] = (int)cv.size() / 3;
                touched.push_back(id);
                cv.push_back(mb.verts[(size_t)id * 3 + 0]);
                cv.push_back(mb.verts[(size_t)id * 3 + 1]);
                cv.push_back(mb.verts[(size_t)id * 3 + 2]);
                ct.push_back(mb.texcoords[(size_t)id * 2 + 0]);
                ct.push_back(mb.texcoords[(size_t)id * 2 + 1]);
                cn.push_back(mb.normals[(size_t)id * 3 + 0]);
                cn.push_back(mb.normals[(size_t)id * 3 + 1]);
                cn.push_back(mb.normals[(size_t)id * 3 + 2]);
                cc.push_back(mb.colors[(size_t)id * 4 + 0]);
                cc.push_back(mb.colors[(size_t)id * 4 + 1]);
                cc.push_back(mb.colors[(size_t)id * 4 + 2]);
                cc.push_back(mb.colors[(size_t)id * 4 + 3]);
            }
            ci.push_back((unsigned short)remap[id]);
        };

        beginChunk();
        for (size_t t = 0; t + 2 < mb.indices.size(); t += 3) {
            const int ids[3] = { mb.indices[t], mb.indices[t + 1], mb.indices[t + 2] };
            int newVerts = 0;
            for (int k = 0; k < 3; k++) if (remap[ids[k]] < 0) newVerts++;
            if (newVerts > 0 && (int)(cv.size() / 3) + newVerts > kMaxChunkVerts) {
                finishChunk();
                beginChunk();
            }
            for (int k = 0; k < 3; k++) addVert(ids[k]);
        }
        finishChunk();
    }

}

// Uploads a tile's CPU data to the GPU (main thread only): road chunks become
// Models, building instances go into persistent instance buffers.
void City::UploadTile(Tile& t) {
    t.roadModels.reserve(t.raw.size());
    for (Mesh& m : t.raw) {
        UploadMesh(&m, false);
        Model mdl = LoadModelFromMesh(m);
        mdl.materials[0].shader = gfx::GetRoadShader();
        mdl.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = gfx::GetDefaultTexture();
        t.roadModels.push_back(mdl);
    }
    t.raw.clear();
    for (int s = 0; s < kBuildingShapes; s++) {
        if (t.inst[s].empty()) continue;
        t.instVbo[s] = gfx::CreateInstanceBuffer(t.inst[s]);
        t.instVboCount[s] = (int)t.inst[s].size();
    }
    gfx::MarkShadowsDirty();
}

// Synchronous single-tile rebuild (used by the incremental path).
void City::BuildTile(Tile& t) {
    DestroyTile(t);
    ComputeTileCPU(t);
    UploadTile(t);
}

int64_t City::TileKeyOf(const Vector2& p) const {
    const int tx = (int)floorf(p.x / kTileSize);
    const int tz = (int)floorf(p.y / kTileSize);
    return (int64_t)(((uint64_t)(uint32_t)tx << 32) | (uint64_t)(uint32_t)tz);
}

Vector2 City::BlockCenter(const Block& b) const {
    Vector2 c{ 0.0f, 0.0f };
    if (b.nodes.empty()) return c;
    for (int idx : b.nodes) c = Vector2Add(c, nodes[(size_t)idx].pos);
    return Vector2Scale(c, 1.0f / (float)b.nodes.size());
}

// Put every node/edge/block into its tile (tiles must be empty).
void City::AssignTiles() {
    tiles.clear();
    nodeTile.assign(nodes.size(), kNoTile);
    edgeTile.assign(edges.size(), kNoTile);
    blockTile.assign(blocks.size(), kNoTile);

    for (int i = 0; i < (int)nodes.size(); i++) {
        const int64_t k = TileKeyOf(nodes[(size_t)i].pos);
        tiles[k].nodes.push_back(i);
        nodeTile[(size_t)i] = k;
    }
    for (int i = 0; i < (int)edges.size(); i++) {
        const int64_t k = TileKeyOf(EdgeMidpoint(i));
        tiles[k].edges.push_back(i);
        edgeTile[(size_t)i] = k;
    }
    for (int i = 0; i < (int)blocks.size(); i++) {
        const int64_t k = TileKeyOf(BlockCenter(blocks[(size_t)i]));
        tiles[k].blocks.push_back(i);
        blockTile[(size_t)i] = k;
    }
}

// Incremental rebuild after node `ni` moved (graph topology unchanged). Only the
// elements that depend on that node are regenerated:
//   * plates + strips of the node and of its neighbours (a junction's fillets
//     depend on the direction/length of every arm), and
//   * pads, parks and buildings of the blocks that contain the node.
// Anything that could change the graph's face structure (angular order at a
// vertex flips, a face degenerates, dead-end spurs move) falls back to the full
// rebuild, so the result is always identical to RebuildAll().
void City::RebuildAfterNodeMove(int ni) {
    if (ni < 0 || (size_t)ni >= nodes.size()) return;
    if (rebuild) {            // a full rebuild is in flight: just restart it on the newest graph
        RequestRebuild();
        return;
    }
    if (!hasGeometry || nodeEdges.size() != nodes.size() || nodeBlocks.size() != nodes.size() ||
        nodeRing.size() != nodes.size() || nodeTile.size() != nodes.size() ||
        edgeTile.size() != edges.size() || blockTile.size() != blocks.size() ||
        edgeSpur.size() != edges.size()) {
        RebuildAll();
        return;
    }

    // Affected nodes: the moved node and its neighbours.
    std::vector<int> S;
    S.push_back(ni);
    for (int ei : nodeEdges[(size_t)ni]) {
        const RoadEdge& e = edges[(size_t)ei];
        if (edgeSpur[(size_t)ei]) { RebuildAll(); return; } // spur geometry feeds every nearby layout
        const int o = (e.a == ni) ? e.b : e.a;
        if (std::find(S.begin(), S.end(), o) == S.end()) S.push_back(o);
    }

    // Same cyclic edge order at every affected vertex => same faces.
    std::vector<std::vector<int>> newRings;
    newRings.reserve(S.size());
    for (int v : S) {
        std::vector<int> ring = AngularRing(v);
        const std::vector<int>& old = nodeRing[(size_t)v];
        bool same = ring.size() == old.size();
        if (same && !ring.empty()) {
            const auto it = std::find(old.begin(), old.end(), ring[0]);
            if (it == old.end()) same = false;
            else {
                const size_t off = (size_t)(it - old.begin());
                for (size_t k = 0; k < ring.size() && same; k++)
                    if (ring[k] != old[(off + k) % old.size()]) same = false;
            }
        }
        if (!same) { RebuildAll(); return; }
        newRings.push_back(std::move(ring));
    }

    // Faces must stay valid (positive CCW area).
    for (int bi : nodeBlocks[(size_t)ni]) {
        std::vector<Vector2> poly;
        for (int idx : blocks[(size_t)bi].nodes) poly.push_back(nodes[(size_t)idx].pos);
        if (citygeom::PolygonArea(poly) < 1e-3f) { RebuildAll(); return; }
    }

    // Commit ring caches + junction flags.
    for (size_t k = 0; k < S.size(); k++) {
        nodeRing[(size_t)S[k]] = std::move(newRings[k]);
        nodes[(size_t)S[k]].junction = JunctionFlagFor(S[k]);
    }

    // Re-lay out the blocks that contain the node.
    const std::vector<int>& dirtyBlocks = nodeBlocks[(size_t)ni];
    for (int bi : dirtyBlocks) {
        Block& b = blocks[(size_t)bi];
        std::vector<Vector2> poly;
        poly.reserve(b.nodes.size());
        for (int idx : b.nodes) poly.push_back(nodes[(size_t)idx].pos);
        b.area = fabsf(citygeom::PolygonArea(poly));
        LayoutBlock(b);
    }

    // Move elements between tiles and collect the tiles to regenerate.
    std::vector<int64_t> dirty;
    auto markDirty = [&](int64_t k) {
        if (k != kNoTile && std::find(dirty.begin(), dirty.end(), k) == dirty.end()) dirty.push_back(k);
    };
    auto retile = [&](std::vector<int64_t>& tileOf, int idx, int64_t nk, std::vector<int> Tile::*list) {
        const int64_t ok = tileOf[(size_t)idx];
        if (ok != nk) {
            if (ok != kNoTile) {
                auto it = tiles.find(ok);
                if (it != tiles.end()) {
                    std::vector<int>& v = it->second.*list;
                    v.erase(std::remove(v.begin(), v.end(), idx), v.end());
                }
                markDirty(ok);
            }
            (tiles[nk].*list).push_back(idx);
            tileOf[(size_t)idx] = nk;
        }
        markDirty(nk);
    };

    for (int v : S) retile(nodeTile, v, TileKeyOf(nodes[(size_t)v].pos), &Tile::nodes);
    std::vector<int> dirtyEdges;
    for (int v : S)
        for (int ei : nodeEdges[(size_t)v])
            if (std::find(dirtyEdges.begin(), dirtyEdges.end(), ei) == dirtyEdges.end()) dirtyEdges.push_back(ei);
    for (int ei : dirtyEdges) retile(edgeTile, ei, TileKeyOf(EdgeMidpoint(ei)), &Tile::edges);
    for (int bi : dirtyBlocks) retile(blockTile, bi, TileKeyOf(BlockCenter(blocks[(size_t)bi])), &Tile::blocks);

    for (int64_t k : dirty) {
        auto it = tiles.find(k);
        if (it == tiles.end()) continue;
        Tile& t = it->second;
        if (t.nodes.empty() && t.edges.empty() && t.blocks.empty()) {
            DestroyTile(t);
            tiles.erase(it);
        } else {
            BuildTile(t);
        }
    }
}

void City::Draw() {
    if (!hasGeometry || tiles.empty()) return;

    const bool shadowPass = gfx::IsInShadowPass();
    // The shadow map is being kept (nothing changed): skip submitting the casters.
    if (shadowPass && gfx::IsShadowPassReused()) return;

    // Cull whole tiles against the frustum of the pass being rendered (camera,
    // mirrored reflection camera, or the light's box during the shadow pass).
    const Frustum fr = ExtractFrustum(rlGetMatrixModelview(), rlGetMatrixProjection());

    visRoad.clear();
    visBldg.clear();
    for (const auto& kv : tiles) {
        const Tile& t = kv.second;
        // The road/pad/parks mesh is flat, so it casts no shadow.
        if (!shadowPass && t.hasRoad && fr.Intersects(t.roadMin, t.roadMax)) visRoad.push_back(&t);
        if (t.hasBldg && fr.Intersects(t.bldgMin, t.bldgMax)) visBldg.push_back(&t);
    }

    for (const Tile* t : visRoad)
        for (const Model& m : t->roadModels) DrawModel(m, Vector3Zero(), 1.0f, WHITE);

    if (gfx::InstanceBuffersActive()) {
        // Persistent per-tile GPU buffers: nothing is uploaded per frame.
        for (const Tile* t : visBldg) {
            for (int s = 0; s < kBuildingShapes; s++) {
                if (t->instVbo[s] != 0)
                    gfx::DrawCityInstancesBuffered(gfx::GetCityShapeMesh(s), t->instVbo[s], t->instVboCount[s], WHITE);
                else if (!t->inst[s].empty())   // buffer creation failed: let raylib upload this one
                    gfx::DrawCityInstances(gfx::GetCityShapeMesh(s), t->inst[s], 0, (int)t->inst[s].size(), WHITE);
            }
        }
        return;
    }

    // Fallback: merge the visible tiles' instances and let raylib upload them.
    for (auto& v : visInst) v.clear();
    for (const Tile* t : visBldg)
        for (int s = 0; s < kBuildingShapes; s++)
            visInst[s].insert(visInst[s].end(), t->inst[s].begin(), t->inst[s].end());
    for (int s = 0; s < kBuildingShapes; s++) {
        if (visInst[s].empty()) continue;
        gfx::DrawCityInstances(gfx::GetCityShapeMesh(s), visInst[s], 0, (int)visInst[s].size(), WHITE);
    }
}

void City::DrawOverlay3D() {
    if (ui::IsPlayActive()) return;
    CityEditorState& state = GetCityEditorState();

    const float h = std::max(params.RoadWidth() * 0.3f, 0.55f);

    // Marker cubes used to be a DrawCube call per node -- at a 99x99 grid that
    // is ~10,000 draw calls/frame, i.e. the dominant editor cost. Instance them
    // instead: one draw call per color bucket (interior/boundary, plus the
    // current hover/selection as their own 1-item buckets).
    nodeMarkerTransforms[0].clear();
    nodeMarkerTransforms[1].clear();
    Matrix selXf{}, hoverXf{};
    bool hasSel = false, hasHover = false;
    const bool active = (state.activeCity == this);
    for (int i = 0; i < (int)nodes.size(); i++) {
        Matrix m = MatrixMultiply(
            MatrixScale(h, h, h),
            MatrixTranslate(nodes[i].pos.x, h * 0.5f, nodes[i].pos.y));
        if (active && state.selectedNode == i) {
            selXf = m;
            hasSel = true;
            continue;
        }
        if (active && state.hoveredNode == i) {
            hoverXf = m;
            hasHover = true;
            continue;
        }
        if (nodes[i].boundary) nodeMarkerTransforms[1].push_back(m);
        else nodeMarkerTransforms[0].push_back(m);
    }

    const Mesh markerMesh = gfx::GetCityShapeMesh(kBuildingBox);
    if (!nodeMarkerTransforms[0].empty())
        gfx::DrawCityInstances(markerMesh, nodeMarkerTransforms[0], 0,
                               (int)nodeMarkerTransforms[0].size(), Color{ 214, 162, 92, 255 });
    if (!nodeMarkerTransforms[1].empty())
        gfx::DrawCityInstances(markerMesh, nodeMarkerTransforms[1], 0,
                               (int)nodeMarkerTransforms[1].size(), Color{ 92, 205, 226, 255 });
    if (hasHover)
        gfx::DrawCityInstances(markerMesh, { &hoverXf, &hoverXf + 1 }, 0, 1, Color{ 255, 160, 60, 255 });
    if (hasSel)
        gfx::DrawCityInstances(markerMesh, { &selXf, &selXf + 1 }, 0, 1,
                               (state.draggingNode && state.dragBlocked) ? Color{ 235, 70, 60, 255 }
                                                                          : Color{ 255, 220, 80, 255 });

    if (state.activeCity == this && state.selectedEdge >= 0 &&
        (size_t)state.selectedEdge < edges.size()) {
        const RoadEdge& e = edges[state.selectedEdge];
        Vector3 a = { nodes[e.a].pos.x, 0.35f, nodes[e.a].pos.y };
        Vector3 b = { nodes[e.b].pos.x, 0.35f, nodes[e.b].pos.y };
        DrawLine3D(a, b, Color{ 255, 220, 80, 255 });
    }
}

City::GeometryHashes City::DebugGeometryHashes() const {
    GeometryHashes h;
    auto mix = [](uint64_t x) {
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
        return x;
    };
    auto q = [](float v) { return (uint64_t)(int64_t)llround((double)v * 256.0); };   // 1/256 m
    for (const auto& kv : tiles) {
        const Tile& t = kv.second;
        for (const Model& mdl : t.roadModels) {
            for (int mi = 0; mi < mdl.meshCount; ++mi) {
                const Mesh& m = mdl.meshes[mi];
                const int tris = m.triangleCount;
                for (int tr = 0; tr < tris; ++tr) {
                    uint64_t th = 1469598103934665603ULL;
                    for (int c = 0; c < 3; ++c) {
                        const int vi = m.indices ? m.indices[tr * 3 + c] : tr * 3 + c;
                        for (int k = 0; k < 3; ++k) th = mix(th ^ q(m.vertices[vi * 3 + k]));
                        if (m.colors) for (int k = 0; k < 4; ++k) th = mix(th ^ (uint64_t)m.colors[vi * 4 + k]);
                    }
                    h.road += th;          // sum: independent of chunking/ordering
                    ++h.roadTris;
                }
            }
        }
        for (int s2 = 0; s2 < kBuildingShapes; ++s2) {
            for (const Matrix& m : t.inst[s2]) {
                const float f[16] = { m.m0, m.m4, m.m8, m.m12, m.m1, m.m5, m.m9, m.m13,
                                      m.m2, m.m6, m.m10, m.m14, m.m3, m.m7, m.m11, m.m15 };
                uint64_t ih = (uint64_t)s2 + 77;
                for (float v : f) ih = mix(ih ^ q(v));
                h.buildings += ih;
                ++h.instances;
            }
        }
    }
    return h;
}

void City::MoveNode(int index, const Vector2& pos, bool rebuildAll) {
    if (index < 0 || (size_t)index >= nodes.size()) return;
    nodes[index].pos = pos;
    if (rebuildAll) RebuildAfterNodeMove(index);
}

bool City::MoveWouldCross(int index, const Vector2& pos) const {
    if (index < 0 || (size_t)index >= nodes.size()) return false;
    auto orient = [](const Vector2& a, const Vector2& b, const Vector2& c) {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    for (size_t i = 0; i < edges.size(); i++) {
        const RoadEdge& e = edges[i];
        if (e.a != index && e.b != index) continue;
        const int other = (e.a == index) ? e.b : e.a;
        const Vector2& A = pos;
        const Vector2& B = nodes[(size_t)other].pos;
        for (size_t j = 0; j < edges.size(); j++) {
            const RoadEdge& f = edges[j];
            if (j == i || f.a == index || f.b == index || f.a == other || f.b == other) continue;
            const Vector2& C = nodes[(size_t)f.a].pos;
            const Vector2& D = nodes[(size_t)f.b].pos;
            const float o1 = orient(A, B, C), o2 = orient(A, B, D), o3 = orient(C, D, A), o4 = orient(C, D, B);
            if (((o1 > 0) != (o2 > 0)) && ((o3 > 0) != (o4 > 0)) &&
                std::fabs(o1) > 1e-4f && std::fabs(o2) > 1e-4f && std::fabs(o3) > 1e-4f && std::fabs(o4) > 1e-4f)
                return true;
        }
    }
    return false;
}

int City::AddNode(const Vector2& pos, bool boundary) {
    nodes.push_back(RoadNode{ pos, boundary });
    RebuildAll();
    return (int)nodes.size() - 1;
}

int City::SplitEdge(int edgeIndex, const Vector2& pos) {
    const RoadEdge e = edges[edgeIndex];
    const bool boundary = nodes[e.a].boundary && nodes[e.b].boundary;
    const int n = (int)nodes.size();
    nodes.push_back(RoadNode{ pos, boundary });
    edges[edgeIndex].b = n;
    edges.push_back(RoadEdge{ n, e.b, e.lanes });
    return n;
}

void City::ConnectWithSplits(int a, int b) {
    if (a == b) return;
    const Vector2 A = nodes[a].pos, B = nodes[b].pos;
    const Vector2 r = Vector2Subtract(B, A);
    struct Hit { float t; int edge; Vector2 p; };
    std::vector<Hit> hits;
    for (int ei = 0; ei < (int)edges.size(); ei++) {
        const RoadEdge& e = edges[ei];
        if (e.a == a || e.a == b || e.b == a || e.b == b) continue;
        const Vector2 P = nodes[e.a].pos;
        const Vector2 s = Vector2Subtract(nodes[e.b].pos, P);
        const float den = r.x * s.y - r.y * s.x;
        if (fabsf(den) < 1e-6f) continue;
        const Vector2 qp = Vector2Subtract(P, A);
        const float t = (qp.x * s.y - qp.y * s.x) / den;
        const float u = (qp.x * r.y - qp.y * r.x) / den;
        if (t > 1e-4f && t < 1.0f - 1e-4f && u > 1e-4f && u < 1.0f - 1e-4f)
            hits.push_back({ t, ei, Vector2Add(A, Vector2Scale(r, t)) });
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) { return x.t < y.t; });

    auto addEdge = [&](int u, int v) {
        for (const auto& e : edges)
            if ((e.a == u && e.b == v) || (e.a == v && e.b == u)) return;
        edges.push_back(RoadEdge{ u, v, 0 });
    };
    int prev = a;
    for (const Hit& h : hits) {
        const int n = SplitEdge(h.edge, h.p);
        addEdge(prev, n);
        prev = n;
    }
    addEdge(prev, b);
}

int City::AddNodeConnected(const Vector2& pos) {
    int nn = -1; float nd = 1e30f;
    for (int i = 0; i < (int)nodes.size(); i++) {
        const float d = Vector2Distance(nodes[i].pos, pos);
        if (d < nd) { nd = d; nn = i; }
    }
    if (nn >= 0 && nd < params.RoadWidth()) return nn; // too close to spawn a new node
    int ne = -1; float ed = 1e30f; Vector2 eq{};
    for (int i = 0; i < (int)edges.size(); i++) {
        const Vector2 P = nodes[edges[i].a].pos, Q = nodes[edges[i].b].pos;
        const Vector2 s = Vector2Subtract(Q, P);
        const float l2 = s.x * s.x + s.y * s.y;
        if (l2 < 1e-8f) continue;
        const float t = Clamp(Vector2DotProduct(Vector2Subtract(pos, P), s) / l2, 0.0f, 1.0f);
        const Vector2 q = Vector2Add(P, Vector2Scale(s, t));
        const float d = Vector2Distance(q, pos);
        if (d < ed) { ed = d; ne = i; eq = q; }
    }

    // Choose the attach point: an existing node when it is closer than any road
    // (or the road point is practically on a node), else split the nearest road.
    int target = nn;
    if (ne >= 0 && ed < nd) {
        const float snap = std::max(params.RoadWidth() * 0.5f, 1.0f);
        const int ea = edges[ne].a, eb = edges[ne].b;
        if (Vector2Distance(eq, nodes[ea].pos) < snap) target = ea;
        else if (Vector2Distance(eq, nodes[eb].pos) < snap) target = eb;
        else target = SplitEdge(ne, eq);
    }

    const int n = (int)nodes.size();
    nodes.push_back(RoadNode{ pos, false });
    if (target >= 0) ConnectWithSplits(target, n);
    RebuildAll();
    return n;
}

int City::InsertNodeOnEdge(int edgeIndex, const Vector2& pos) {
    if (edgeIndex < 0 || (size_t)edgeIndex >= edges.size()) return -1;
    const RoadEdge e = edges[edgeIndex];
    const bool boundary = nodes[e.a].boundary && nodes[e.b].boundary;
    const int n = (int)nodes.size();
    nodes.push_back(RoadNode{ pos, boundary });
    edges[edgeIndex].b = n;
    edges.push_back(RoadEdge{ n, e.b, e.lanes });
    RebuildAll();
    return n;
}

void City::DeleteNode(int index) {
    const int n = (int)nodes.size();
    if (index < 0 || index >= n) return;

    std::vector<int> incident;
    for (int i = 0; i < (int)edges.size(); i++)
        if (edges[i].a == index || edges[i].b == index) incident.push_back(i);

    // Degree-2 node: bridge its two neighbors so the road remains connected.
    int bridgeA = -1, bridgeB = -1;
    if (incident.size() == 2) {
        auto other = [&](int e, int node) {
            return edges[e].a == node ? edges[e].b : edges[e].a;
        };
        bridgeA = other(incident[0], index);
        bridgeB = other(incident[1], index);
        if (bridgeA == bridgeB) { bridgeA = -1; bridgeB = -1; }
        if (bridgeA >= 0 && bridgeB >= 0) {
            // Drop duplicate edges connecting the same pair.
            bool exists = false;
            for (const auto& e : edges)
                if ((e.a == bridgeA && e.b == bridgeB) || (e.a == bridgeB && e.b == bridgeA)) { exists = true; break; }
            if (exists) { bridgeA = -1; bridgeB = -1; }
        }
    }

    std::sort(incident.begin(), incident.end(), std::greater<int>());
    for (int e : incident) edges.erase(edges.begin() + e);
    if (bridgeA >= 0 && bridgeB >= 0) edges.push_back(RoadEdge{ bridgeA, bridgeB, 0 });

    nodes.erase(nodes.begin() + index);
    for (auto& e : edges) {
        if (e.a > index) e.a--;
        if (e.b > index) e.b--;
    }
    RebuildAll();
}

void City::SetEdgeLaneOverride(int edgeIndex, int lanes) {
    if (edgeIndex < 0 || (size_t)edgeIndex >= edges.size()) return;
    edges[edgeIndex].lanes = Clamp(lanes, 0, 8);
    RebuildAll();
}

int City::PickNode(const Ray& ray, float tolerance, float* outDist) const {
    int best = -1;
    float bestDist = 1e30f;
    for (int i = 0; i < (int)nodes.size(); i++) {
        Vector3 c = { nodes[i].pos.x, 0.04f, nodes[i].pos.y };
        RayCollision col = GetRayCollisionSphere(ray, c, tolerance);
        if (col.hit && col.distance < bestDist) {
            bestDist = col.distance;
            best = i;
        }
    }
    if (outDist) *outDist = bestDist;
    return best;
}

int City::PickRoad(const Ray& ray, float tolerance, float* outParam) const {
    int best = -1;
    float bestDist = tolerance;
    for (int i = 0; i < (int)edges.size(); i++) {
Vector3 a = { nodes[edges[i].a].pos.x, 0.13f + kRoadElevation, nodes[edges[i].a].pos.y };
        Vector3 b = { nodes[edges[i].b].pos.x, 0.13f + kRoadElevation, nodes[edges[i].b].pos.y };
        Vector3 d = Vector3Normalize(Vector3Subtract(b, a));
        // Approximate ray-to-segment distance by sampling the segment.
        float minD = 1e30f;
        for (int s = 0; s <= 12; s++) {
            Vector3 p = Vector3Add(a, Vector3Scale(d, Vector3Length(Vector3Subtract(b, a)) * s / 12.0f));
            Vector3 w = Vector3Subtract(p, ray.position);
            float t = Vector3DotProduct(w, ray.direction);
            float dist;
            if (t < 0.0f) dist = Vector3Length(w);
            else {
                Vector3 proj = Vector3Add(ray.position, Vector3Scale(ray.direction, t));
                dist = Vector3Length(Vector3Subtract(p, proj));
            }
            minD = fminf(minD, dist);
        }
        if (minD < bestDist) {
            bestDist = minD;
            best = i;
        }
    }
    if (outParam) *outParam = bestDist;
    return best;
}

bool City::ContainsPoint(const Vector2& p) const {
    for (const auto& block : blocks) {
        std::vector<Vector2> poly;
        for (int idx : block.nodes) poly.push_back(nodes[idx].pos);
        if (citygeom::PointInPolygon(p, poly)) return true;
    }
    const float reach = params.RoadWidth() * 0.6f;
    for (const auto& e : edges) {
        const Vector2 a = nodes[e.a].pos;
        const Vector2 b = nodes[e.b].pos;
        Vector2 ab = Vector2Subtract(b, a);
        float len2 = Vector2DotProduct(ab, ab);
        if (len2 <= 0.0f) continue;
        float t = Clamp(Vector2DotProduct(Vector2Subtract(p, a), ab) / len2, 0.0f, 1.0f);
        Vector2 closest = Vector2Add(a, Vector2Scale(ab, t));
        Vector2 diff = Vector2Subtract(p, closest);
        if (Vector2DotProduct(diff, diff) <= reach * reach) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------
bool City::WriteToStream(std::ostream& out) const {
    // Full float precision: the default 6 significant digits shifted saved node
    // positions by a few millimetres, so a reloaded city wasn't quite the one saved.
    out << std::setprecision(9);
    out << name.size() << '\n';
    out.write(name.data(), (std::streamsize)name.size());
    out << '\n';
    out << params.gridX << ' ' << params.gridZ << ' ' << params.cellSize << '\n';
    out << (params.organic ? 1 : 0) << ' ' << params.organicStrength << ' '
        << params.organicScale << ' ' << params.noiseOctaves << ' ' << params.seed << '\n';
    out << params.lanes << ' ' << params.laneWidth << ' ' << params.sidewalk << '\n';
    out << params.avgHeight << ' ' << params.heightVariance << ' '
        << params.buildingSize << ' ' << params.buildingGap << '\n';
    out << params.parkThreshold << ' ' << params.parkInset << '\n';
    out << "R " << params.cornerRadius << '\n';

    out << edges.size() << '\n';
    for (const auto& e : edges) out << e.a << ' ' << e.b << ' ' << e.lanes << '\n';
    out << nodes.size() << '\n';
    for (const auto& n : nodes)
        out << n.pos.x << ' ' << n.pos.y << ' ' << (n.boundary ? 1 : 0) << '\n';

    // Block ids + building overrides. Persisted so ComputeBlocks() can match
    // its freshly extracted faces back to these ids by node overlap on load
    // (see ComputeBlocks), which is what lets an override still find "building
    // 7 of block 12" after the city round-trips through a file. Block ids
    // themselves are otherwise only ever assigned in memory.
    out << "BLOCKS " << blocks.size() << ' ' << nextBlockId << '\n';
    for (const Block& b : blocks) {
        out << b.id << ' ' << b.nodes.size();
        for (int idx : b.nodes) out << ' ' << idx;
        out << '\n';
    }
    out << "OVERRIDES " << buildingOverrides.size() << '\n';
    for (const auto& kv : buildingOverrides) {
        const uint64_t blockId = kv.first >> 20;
        const int slot = (int)(kv.first & 0xFFFFF);
        const BuildingOverride& ov = kv.second;
        out << blockId << ' ' << slot << ' '
            << (ov.hasHeight ? 1 : 0) << ' ' << ov.height << ' '
            << (ov.hasOffset ? 1 : 0) << ' ' << ov.posOffset.x << ' ' << ov.posOffset.y << '\n';
    }
    return out.good();
}

bool City::ReadFromStream(std::istream& in) {
    size_t nameLen = 0;
    if (!(in >> nameLen)) return false;
    in.ignore();
    if (nameLen > 0) {
        name.resize(nameLen);
        in.read(&name[0], (std::streamsize)nameLen);
    }
    in.ignore();
    if (!in) return false;

    CityParams p;
    if (!(in >> p.gridX >> p.gridZ >> p.cellSize)) return false;
    int organic = 0;
    if (!(in >> organic >> p.organicStrength >> p.organicScale >> p.noiseOctaves >> p.seed)) return false;
    p.organic = (organic != 0);
    if (!(in >> p.lanes >> p.laneWidth >> p.sidewalk)) return false;
    if (!(in >> p.avgHeight >> p.heightVariance >> p.buildingSize >> p.buildingGap)) return false;
    if (!(in >> p.parkThreshold >> p.parkInset)) return false;

    // Optional "R <cornerRadius>" tag (absent in files saved before rounded corners).
    std::string tok;
    if (!(in >> tok)) return false;
    size_t edgeCount = 0;
    if (tok == "R") {
        if (!(in >> p.cornerRadius)) return false;
        if (!(in >> edgeCount)) return false;
    } else {
        try { edgeCount = (size_t)std::stoull(tok); } catch (...) { return false; }
    }
    params = p;

    edges.resize(edgeCount);
    for (size_t i = 0; i < edgeCount; i++)
        if (!(in >> edges[i].a >> edges[i].b >> edges[i].lanes)) return false;

    size_t nodeCount = 0;
    if (!(in >> nodeCount)) return false;
    nodes.resize(nodeCount);
    for (size_t i = 0; i < nodeCount; i++) {
        int boundary = 0;
        if (!(in >> nodes[i].pos.x >> nodes[i].pos.y >> boundary)) return false;
        nodes[i].boundary = (boundary != 0);
    }

    // Optional BLOCKS/OVERRIDES sections (absent in files saved before per-
    // building overrides). Seed `blocks` with just enough (id + node list) for
    // ComputeBlocks()'s matching pass to recover the same ids below; every
    // other Block field is filled in by the rebuild that follows.
    blocks.clear();
    buildingOverrides.clear();
    nextBlockId = 1;
    std::streampos beforeTag = in.tellg();
    std::string tag;
    if (in >> tag && tag == "BLOCKS") {
        size_t blockCount = 0;
        if (!(in >> blockCount >> nextBlockId)) return false;
        blocks.resize(blockCount);
        for (size_t i = 0; i < blockCount; i++) {
            size_t nc = 0;
            if (!(in >> blocks[i].id >> nc)) return false;
            blocks[i].nodes.resize(nc);
            for (size_t k = 0; k < nc; k++) if (!(in >> blocks[i].nodes[k])) return false;
        }
        if (!(in >> tag) || tag != "OVERRIDES") return false;
        size_t ovCount = 0;
        if (!(in >> ovCount)) return false;
        for (size_t i = 0; i < ovCount; i++) {
            uint64_t blockId = 0; int slot = 0, hasH = 0, hasP = 0;
            BuildingOverride ov;
            if (!(in >> blockId >> slot >> hasH >> ov.height >> hasP >> ov.posOffset.x >> ov.posOffset.y))
                return false;
            ov.hasHeight = (hasH != 0);
            ov.hasOffset = (hasP != 0);
            buildingOverrides[OverrideKey(blockId, slot)] = ov;
        }
    } else {
        in.clear();
        in.seekg(beforeTag); // pre-override file: nothing to seed, rewind harmlessly
    }

    RebuildAll();
    return true;
}

// ---------------------------------------------------------------------------
// Registry + sidecar
// ---------------------------------------------------------------------------
struct CityFileHeader {
    char magic[8];
    uint32_t version;
    uint32_t cityCount;
};
struct CityBlockHeader {
    uint32_t nameLen;
    uint32_t dataSize;
};

bool CityRegistry::WriteFile(const std::string& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    CityFileHeader fh;
    std::memset(&fh, 0, sizeof(fh));
    std::memcpy(fh.magic, "FLYCITY1", 8);
    fh.version = 1;
    fh.cityCount = (uint32_t)cities.size();
    out.write(reinterpret_cast<const char*>(&fh), sizeof(fh));

    for (auto* c : cities) {
        if (!c) continue;
        const std::string& nm = c->GetName();
        std::ostringstream payload(std::ios::binary);
        if (!c->WriteToStream(payload)) return false;
        std::string data = payload.str();

        CityBlockHeader bh;
        bh.nameLen = (uint32_t)nm.size();
        bh.dataSize = (uint32_t)data.size();
        out.write(reinterpret_cast<const char*>(&bh), sizeof(bh));
        out.write(nm.data(), (std::streamsize)nm.size());
        out.write(data.data(), (std::streamsize)data.size());
    }
    return out.good();
}

int CityRegistry::ReadFile(const std::string& path, const std::function<City*()>& create) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;

    CityFileHeader fh;
    in.read(reinterpret_cast<char*>(&fh), sizeof(fh));
    if (!in) return 0;
    if (std::memcmp(fh.magic, "FLYCITY1", 8) != 0) return 0;

    int loaded = 0;
    for (uint32_t i = 0; i < fh.cityCount; i++) {
        CityBlockHeader bh;
        in.read(reinterpret_cast<char*>(&bh), sizeof(bh));
        if (!in) return loaded;

        std::string name(bh.nameLen, '\0');
        if (bh.nameLen) in.read(&name[0], (std::streamsize)bh.nameLen);
        std::string data(bh.dataSize, '\0');
        if (bh.dataSize) in.read(&data[0], (std::streamsize)bh.dataSize);
        if (!in) return loaded;

        City* c = create();
        if (!c) return loaded;
        c->SetName(name);
        std::istringstream payload(data, std::ios::binary);
        if (!c->ReadFromStream(payload)) break;
        Register(c);
        loaded++;
    }
    return loaded;
}

// ---------------------------------------------------------------------------
// Undo integration
// ---------------------------------------------------------------------------
static std::function<void()> g_cityEditCb;

void SetCityEditCallback(const std::function<void()>& cb) { g_cityEditCb = cb; }
void NotifyCityEdit() {
    if (g_cityEditCb) g_cityEditCb();
}

std::string SerializeCitiesSnapshot() {
    auto& reg = GetCityRegistry();
    std::ostringstream out;
    out << "\nCITY " << reg.Count() << "\n";
    for (auto* c : reg.GetCities()) {
        if (!c) return "";
        if (!c->WriteToStream(out)) return "";
    }
    return out.str();
}

int RestoreCitiesFromSnapshot(std::istream& in, Engine& engine,
                              const std::function<City*()>& create) {
    // Optional trailing section; a snapshot without one restores fine.
    const std::streampos base = in.tellg();
    std::string marker;
    if (!(in >> marker) || marker != "CITY" || !create) {
        in.clear();
        if (base != std::streampos(-1)) in.seekg(base);
        return 0;
    }
    int count = 0;
    if (!(in >> count) || count < 0) {
        in.clear();
        if (base != std::streampos(-1)) in.seekg(base);
        return 0;
    }

    // Destroy the current city entities; the snapshot's versions come next.
    auto& reg = GetCityRegistry();
    for (auto* c : reg.GetCities()) {
        if (!c) continue;
        engine.RemoveEntity(c);
    }
    reg.GetCities().clear();

    int loaded = 0;
    for (int i = 0; i < count; i++) {
        City* c = create();
        if (!c) break;
        if (!c->ReadFromStream(in)) {
            engine.RemoveEntity(c);
            break;
        }
        reg.Register(c);
        loaded++;
    }
    return loaded;
}

} // namespace city