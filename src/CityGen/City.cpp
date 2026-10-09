#include "../../include/CityGen/City.hpp"
#include "../../include/CityGen/CityEditor.hpp"
#include "../../include/CityGen/CityGeometry.hpp"
#include "../../include/CityGen/CityPhysics.hpp"
#include "../../include/Engine/Backend/Box3DWrapper.hpp"
#include "../../include/Engine.hpp"
#include "../../include/Engine/Graphics.hpp"
#include "../../include/Engine/Frontend/ui.hpp"

#include "rlgl.h"

#include <array>
#include <chrono>
#include <climits>
#include <limits>
#include <queue>
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
constexpr float kProfileStep = 6.0f; // sloped roads/pads are tessellated about this often (m)

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

// Per-style colour buckets (same 8 slots as kBuildingTints; style 0 uses kBuildingTints itself).
const Color kStyleTints[4][kBuildingColorBuckets] = {
    { { 210, 205, 195, 255 }, { 160, 158, 158, 255 }, { 190, 180, 160, 255 }, { 130, 140, 150, 255 },
      { 175, 150, 130, 255 }, { 120, 125, 135, 255 }, { 205, 190, 175, 255 }, { 150, 165, 180, 255 } },
    { { 176, 96, 76, 255 }, { 156, 84, 66, 255 }, { 190, 120, 92, 255 }, { 140, 76, 62, 255 },
      { 168, 108, 84, 255 }, { 120, 70, 60, 255 }, { 196, 140, 110, 255 }, { 150, 92, 74, 255 } },
    { { 128, 130, 132, 255 }, { 104, 108, 112, 255 }, { 146, 148, 140, 255 }, { 90, 96, 104, 255 },
      { 160, 150, 128, 255 }, { 112, 116, 120, 255 }, { 136, 138, 130, 255 }, { 98, 104, 98, 255 } },
    { { 232, 224, 204, 255 }, { 210, 200, 178, 255 }, { 196, 214, 200, 255 }, { 220, 196, 176, 255 },
      { 226, 210, 190, 255 }, { 190, 204, 216, 255 }, { 236, 218, 196, 255 }, { 206, 186, 166, 255 } },
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
           a.cornerRadius == b.cornerRadius &&
           a.style == b.style && a.shapeVariety == b.shapeVariety &&
           a.shortChance == b.shortChance && a.footprintVariety == b.footprintVariety &&
           a.furniture == b.furniture && a.cars == b.cars && a.pedestrians == b.pedestrians &&
           a.trafficDetailDistance == b.trafficDetailDistance && a.leftHandTraffic == b.leftHandTraffic;
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
    std::vector<unsigned char> fixedNormal; // 1: normal set explicitly (walls); skipped by RecomputeNormals
    std::vector<unsigned char> wallTri;     // per triangle: 1 = collision-only-as-is (walls, bridge decks)
    bool wallMode = false;
    float curLayer = 0.0f;                  // stack layer (m above the road surface) for the road shader's depth bias

    void Vertex(const Vector3& p, Color c) {
        verts.push_back(p.x); verts.push_back(p.y); verts.push_back(p.z);
        texcoords.push_back(curLayer); texcoords.push_back(0.0f);
        normals.push_back(0.0f); normals.push_back(1.0f); normals.push_back(0.0f);
        colors.push_back(c.r); colors.push_back(c.g); colors.push_back(c.b); colors.push_back(c.a);
        fixedNormal.push_back(0);
    }

    // Vertex with an explicit normal (vertical faces: walls, curbs).
    void VertexN(const Vector3& p, Color c, const Vector3& n) {
        Vertex(p, c);
        const size_t i = normals.size() - 3;
        normals[i] = n.x; normals[i + 1] = n.y; normals[i + 2] = n.z;
        fixedNormal.back() = 1;
    }

    // Surface normals from the triangles (area weighted, always facing up) so sloped roads and
    // pads are lit as slopes. Flat geometry comes out as exactly (0,1,0).
    void RecomputeNormals() {
        const size_t vc = verts.size() / 3;
        std::vector<Vector3> acc(vc, Vector3{ 0.0f, 0.0f, 0.0f });
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            const size_t i0 = (size_t)indices[t], i1 = (size_t)indices[t + 1], i2 = (size_t)indices[t + 2];
            const Vector3 a = { verts[i0 * 3], verts[i0 * 3 + 1], verts[i0 * 3 + 2] };
            const Vector3 b = { verts[i1 * 3], verts[i1 * 3 + 1], verts[i1 * 3 + 2] };
            const Vector3 c = { verts[i2 * 3], verts[i2 * 3 + 1], verts[i2 * 3 + 2] };
            Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a));
            const float nl = Vector3Length(n);
            if (nl < 1e-12f) continue;
            if (n.y < 0.0f) n = Vector3Negate(n);
            n = Vector3Scale(n, 1.0f / nl);
            // A near-vertical triangle that is not a deliberate wall (those have explicit normals) would drag the
            // normals of the flat surface it touches sideways.
            if (n.y < 0.2f && !(t / 3 < wallTri.size() && wallTri[t / 3])) continue;
            // Weight by the angle at each corner, not by triangle area: area weights let the long thin triangles of
            // a fan-split sloped pad dominate, which showed as radial shading wedges that depended on the split.
            const Vector3 P[3] = { a, b, c };
            const size_t I[3] = { i0, i1, i2 };
            for (int k = 0; k < 3; k++) {
                const Vector3 e1 = Vector3Normalize(Vector3Subtract(P[(k + 1) % 3], P[k])), e2 = Vector3Normalize(Vector3Subtract(P[(k + 2) % 3], P[k]));
                const float ang = acosf(Clamp(Vector3DotProduct(e1, e2), -1.0f, 1.0f));
                acc[I[k]] = Vector3Add(acc[I[k]], Vector3Scale(n, ang));
            }
        }
        // Vertices are unshared (every quad has its own), so each quad would be shaded flat: a sloped road, whose profile
        // curves along its length, then shows as steps of dark and light rectangles. Weld the accumulated normals of
        // vertices at the same position (to the centimetre) so the surface shades continuously across quad borders.
        {
            auto key = [&](size_t i) {
                const int64_t x = (int64_t)llroundf(verts[i * 3] * 100.0f), y = (int64_t)llroundf(verts[i * 3 + 1] * 100.0f), z = (int64_t)llroundf(verts[i * 3 + 2] * 100.0f);
                return (uint64_t)(x * 73856093LL) ^ (uint64_t)(y * 19349663LL) ^ (uint64_t)(z * 83492791LL);
            };
            std::unordered_map<uint64_t, Vector3> sum;
            sum.reserve(vc);
            for (size_t i = 0; i < vc; i++) {
                if (fixedNormal[i]) continue;
                const float l = Vector3Length(acc[i]);
                if (l < 1e-9f) continue;
                Vector3& dst = sum[key(i)];
                dst = Vector3Add(dst, Vector3Scale(acc[i], 1.0f / l));   // unit normals: every quad weighs the same at the shared corner
            }
            for (size_t i = 0; i < vc; i++) {
                if (fixedNormal[i]) continue;
                const auto it = sum.find(key(i));
                if (it != sum.end()) acc[i] = it->second;
            }
        }
        for (size_t i = 0; i < vc; i++) {
            if (fixedNormal[i]) continue;
            const float l = Vector3Length(acc[i]);
            if (l < 1e-9f) continue;
            normals[i * 3] = acc[i].x / l; normals[i * 3 + 1] = acc[i].y / l; normals[i * 3 + 2] = acc[i].z / l;
        }
    }

    void Triangle(int a, int b, int c) {
        wallTri.push_back(wallMode ? 1 : 0);
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

    // A quad that faces up whichever way round its corners were given.
    void QuadUp(const Vector3& a, const Vector3& b, const Vector3& c, const Vector3& d, Color col) {
        const Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a));
        if (n.y >= 0.0f) Quad(a, b, c, d, col);
        else Quad(a, d, c, b, col);
    }

    // A flat-shaded quad with an explicit normal (vertical faces); wound to face `n`.
    void WallQuad(const Vector3& a, const Vector3& b, const Vector3& c, const Vector3& d, const Vector3& n, Color col) {
        const bool flip = Vector3DotProduct(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a)), n) < 0.0f;
        const Vector3 p[4] = { a, flip ? d : b, c, flip ? b : d };
        const int base = (int)(verts.size() / 3);
        for (const Vector3& v : p) VertexN(v, col, n);
        const bool was = wallMode;
        wallMode = true;
        Triangle(base, base + 1, base + 2);
        Triangle(base, base + 2, base + 3);
        wallMode = was;
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

namespace { bool g_simActive = false; }   // Play mode (editor Play or the standalone player): traffic runs

void City::Update(float dt) {
    PumpRebuild();
    trafficClock += dt;   // signal clock runs in the editor too (heads animate while editing)
    if (g_simActive && hasGeometry && !rebuild) StepTraffic(std::min(dt, 0.1f));
    else if (!g_simActive && !agents.empty()) agents.clear();
}

// ---------------------------------------------------------------------------
// Traffic and pedestrians (visual agents; Play mode only)
// ---------------------------------------------------------------------------
namespace {
uint32_t Lcg(uint32_t& s) { s = s * 1664525u + 1013904223u; return s >> 8; }
float Lcg01(uint32_t& s) { return (float)(Lcg(s) & 0xFFFFu) / 65535.0f; }
}

// A road allows travel from `from` along edge e (respecting one-way).
static bool EdgeAllowsFrom(const RoadEdge& e, int from) {
    if (e.oneWay == 0) return true;
    return (e.oneWay == 1 && e.a == from) || (e.oneWay == 2 && e.b == from);
}

// ---------------------------------------------------------------------------
// Routing: cars head for destination nodes along the fastest allowed route.
// ---------------------------------------------------------------------------
void City::PickDestinationPool() {
    if (routes.version == graphVersion && !routes.pool.empty()) return;
    routes.version = graphVersion;
    routes.pool.clear();
    routes.dist.clear();
    if (nodeEdges.size() != nodes.size()) return;
    std::vector<int> cand;
    std::vector<float> w;
    float total = 0.0f;
    for (int i = 0; i < (int)nodes.size(); i++) {
        bool drivable = false;
        for (int ei : nodeEdges[(size_t)i]) if (edges[(size_t)ei].type != (int)RoadType::Pedestrian) { drivable = true; break; }
        if (!drivable) continue;
        float mul = 1.0f; int st = -1;
        DistrictAt(nodes[(size_t)i].pos, 0, mul, st);       // downtown districts attract more trips
        mul = std::max(mul, 0.25f);
        cand.push_back(i); w.push_back(mul); total += mul;
    }
    uint32_t r = 0x9E3779B9u ^ (uint32_t)params.seed;
    for (int k = 0; k < 64 && routes.pool.size() < 24 && !cand.empty(); k++) {
        float x = Lcg01(r) * total;
        size_t pick = cand.size() - 1;
        for (size_t j = 0; j < cand.size(); j++) { x -= w[j]; if (x <= 0.0f) { pick = j; break; } }
        if (std::find(routes.pool.begin(), routes.pool.end(), cand[pick]) == routes.pool.end()) routes.pool.push_back(cand[pick]);
    }
}

int City::PickDestination(uint32_t& rng) {
    PickDestinationPool();
    if (routes.pool.empty()) return -1;
    return routes.pool[Lcg(rng) % (uint32_t)routes.pool.size()];
}

const std::vector<float>& City::RouteField(int dest, int avoidEdge) {
    PickDestinationPool();
    const int64_t key = ((int64_t)dest << 32) | (int64_t)(uint32_t)(avoidEdge + 1);
    const auto it = routes.dist.find(key);
    if (it != routes.dist.end()) return it->second;
    const float kInf = std::numeric_limits<float>::infinity();
    std::vector<float> d(nodes.size(), kInf);
    if (dest >= 0 && (size_t)dest < nodes.size() && nodeEdges.size() == nodes.size()) {
        using Item = std::pair<float, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
        d[(size_t)dest] = 0.0f;
        pq.push({ 0.0f, dest });
        while (!pq.empty()) {
            const auto [dv, v] = pq.top(); pq.pop();
            if (dv > d[(size_t)v]) continue;
            for (int ei : nodeEdges[(size_t)v]) {
                const RoadEdge& e = edges[(size_t)ei];
                if (e.type == (int)RoadType::Pedestrian || ei == avoidEdge) continue;
                const int u = e.a == v ? e.b : e.a;
                if (!EdgeAllowsFrom(e, u)) continue;                  // u -> v must be drivable
                const float len = Vector2Distance(nodes[(size_t)e.a].pos, nodes[(size_t)e.b].pos);
                const float nd = dv + len / std::max(EdgeSpeedLimit(ei), 1.0f);
                if (nd < d[(size_t)u]) { d[(size_t)u] = nd; pq.push({ nd, u }); }
            }
        }
    }
    return routes.dist.emplace(key, std::move(d)).first->second;
}

// ---------------------------------------------------------------------------
// Public transit: stops, lines, auto generation.
// ---------------------------------------------------------------------------
namespace {
// Closest point of segment ab to p, as a fraction 0..1 along it.
float SegFraction(const Vector2& a, const Vector2& b, const Vector2& p) {
    const Vector2 ab = Vector2Subtract(b, a);
    const float l2 = Vector2DotProduct(ab, ab);
    return l2 > 1e-6f ? Clamp(Vector2DotProduct(Vector2Subtract(p, a), ab) / l2, 0.0f, 1.0f) : 0.0f;
}
}

void City::ResolveBusStops() {
    for (BusStop& bs : busStops) {
        bs.edge = -1;
        float best = 15.0f;
        for (int ei = 0; ei < (int)edges.size(); ei++) {
            const RoadEdge& e = edges[(size_t)ei];
            if (e.type == (int)RoadType::Pedestrian || e.type == (int)RoadType::Highway) continue;
            const Vector2 a = nodes[(size_t)e.a].pos, b = nodes[(size_t)e.b].pos;
            const float f = SegFraction(a, b, bs.pos);
            const Vector2 q = Vector2Add(a, Vector2Scale(Vector2Subtract(b, a), f));
            const float d = Vector2Distance(q, bs.pos);
            if (d < best) {
                best = d;
                bs.edge = ei;
                bs.s = f * Vector2Distance(a, b);
                const Vector2 ab = Vector2Subtract(b, a);
                bs.fwd = Vector2DotProduct(bs.heading, ab) >= 0.0f;
            }
        }
        if (bs.edge >= 0) {   // one-way roads only carry traffic one way
            const RoadEdge& e = edges[(size_t)bs.edge];
            if (e.oneWay == 1) bs.fwd = true;
            else if (e.oneWay == 2) bs.fwd = false;
        }
    }
}

bool City::SnapBusStop(const Vector2& cursor, Vector2& pos, Vector2& heading) const {
    float best = 14.0f;
    int bestEdge = -1;
    float bestF = 0.0f;
    for (int ei = 0; ei < (int)edges.size(); ei++) {
        const RoadEdge& e = edges[(size_t)ei];
        if (e.type == (int)RoadType::Pedestrian || e.type == (int)RoadType::Highway) continue;
        const Vector2 a = nodes[(size_t)e.a].pos, b = nodes[(size_t)e.b].pos;
        const float f = SegFraction(a, b, cursor);
        const float d = Vector2Distance(Vector2Add(a, Vector2Scale(Vector2Subtract(b, a), f)), cursor);
        if (d < best) { best = d; bestEdge = ei; bestF = f; }
    }
    if (bestEdge < 0) return false;
    const RoadEdge& e = edges[(size_t)bestEdge];
    const Vector2 a = nodes[(size_t)e.a].pos, b = nodes[(size_t)e.b].pos;
    const Vector2 ab = Vector2Normalize(Vector2Subtract(b, a));
    pos = Vector2Add(a, Vector2Scale(Vector2Subtract(b, a), bestF));
    // The stop is on the kerb the cursor is on: with right-hand traffic that is the right of the direction served.
    const float side = ab.x * (cursor.y - pos.y) - ab.y * (cursor.x - pos.x);   // > 0: left of a -> b
    bool fwd = side <= 0.0f;
    if (params.leftHandTraffic) fwd = !fwd;
    if (e.oneWay == 1) fwd = true;
    else if (e.oneWay == 2) fwd = false;
    heading = fwd ? ab : Vector2Negate(ab);
    return true;
}

int City::AddBusStop(const Vector2& pos, const Vector2& heading) {
    BusStop bs;
    bs.pos = pos;
    bs.heading = heading;
    bs.name = "Stop " + std::to_string(busStops.size() + 1);
    busStops.push_back(bs);
    ResolveBusStops();
    if (busStops.back().edge < 0) { busStops.pop_back(); return -1; }
    RebuildAll();
    return (int)busStops.size() - 1;
}

void City::RemoveBusStop(int index) {
    if (index < 0 || (size_t)index >= busStops.size()) return;
    busStops.erase(busStops.begin() + index);
    for (BusLine& l : busLines) {
        std::vector<int> kept;
        for (int s : l.stops) { if (s == index) continue; kept.push_back(s > index ? s - 1 : s); }
        l.stops = kept;
    }
    RebuildAll();
}

int City::AddBusLine(const BusLine& line) {
    busLines.push_back(line);
    RebuildAll();
    return (int)busLines.size() - 1;
}

void City::UpdateBusLine(int index, const BusLine& line) {
    if (index < 0 || (size_t)index >= busLines.size()) return;
    busLines[(size_t)index] = line;
    busLines[(size_t)index].buses = std::clamp(line.buses, 0, 20);
}

void City::RemoveBusLine(int index) {
    if (index < 0 || (size_t)index >= busLines.size()) return;
    busLines.erase(busLines.begin() + index);
}

void City::AddStopToLine(int line, int stop) {
    if (line < 0 || (size_t)line >= busLines.size() || stop < 0 || (size_t)stop >= busStops.size()) return;
    busLines[(size_t)line].stops.push_back(stop);
}

void City::RemoveStopFromLine(int line, int position) {
    if (line < 0 || (size_t)line >= busLines.size()) return;
    auto& v = busLines[(size_t)line].stops;
    if (position >= 0 && (size_t)position < v.size()) v.erase(v.begin() + position);
}

void City::ClearTransit() {
    busStops.clear();
    busLines.clear();
    RebuildAll();
}

int City::PickBusStop(const Ray& ray) const {
    int best = -1;
    float bestT = 1e30f;
    const float top = BusStopMarkerHeight();
    for (int i = 0; i < (int)busStops.size(); i++) {
        const BusStop& bs = busStops[(size_t)i];
        const float y0 = bs.edge >= 0 ? EdgeProfileY(bs.edge, bs.edge >= 0 ? bs.s / std::max(Vector2Distance(nodes[(size_t)edges[(size_t)bs.edge].a].pos, nodes[(size_t)edges[(size_t)bs.edge].b].pos), 1e-3f) : 0.0f) : 0.0f;
        const Vector3 p0 = { bs.pos.x, y0 + 0.5f, bs.pos.y }, p1 = { bs.pos.x, y0 + top, bs.pos.y };
        const Vector3 u = Vector3Subtract(p1, p0), w0 = Vector3Subtract(ray.position, p0);
        const float a = Vector3DotProduct(u, u), b = Vector3DotProduct(u, ray.direction), c = Vector3DotProduct(ray.direction, ray.direction);
        const float dd = Vector3DotProduct(u, w0), e2 = Vector3DotProduct(ray.direction, w0);
        const float den = a * c - b * b;
        float sc = den > 1e-6f ? (c * dd - b * e2) / den : 0.0f;
        sc = Clamp(sc, 0.0f, 1.0f);
        const float tc = std::max(0.0f, (b * sc - e2) / c);
        const Vector3 onP = Vector3Add(p0, Vector3Scale(u, sc)), onR = Vector3Add(ray.position, Vector3Scale(ray.direction, tc));
        const float tol = (sc > 0.9f ? 2.2f : 1.0f) + tc * 0.012f;
        if (Vector3Distance(onP, onR) < tol && tc < bestT) { bestT = tc; best = i; }
    }
    return best;
}

// Node path along the fastest drivable route (greedy descent of the travel-time field).
std::vector<int> City::RouteNodes(int from, int to) {
    std::vector<int> path;
    if (from < 0 || to < 0 || (size_t)from >= nodes.size() || (size_t)to >= nodes.size() || nodeEdges.size() != nodes.size()) return path;
    const std::vector<float>& field = RouteField(to);
    if (!std::isfinite(field[(size_t)from])) return path;
    int cur = from;
    path.push_back(cur);
    for (size_t guard = 0; cur != to && guard < nodes.size() + 4; guard++) {
        int bestNode = -1;
        float best = std::numeric_limits<float>::infinity();
        for (int ei : nodeEdges[(size_t)cur]) {
            const RoadEdge& e = edges[(size_t)ei];
            if (e.type == (int)RoadType::Pedestrian || !EdgeAllowsFrom(e, cur)) continue;
            const int other = e.a == cur ? e.b : e.a;
            const float cost = Vector2Distance(nodes[(size_t)e.a].pos, nodes[(size_t)e.b].pos) / std::max(EdgeSpeedLimit(ei), 1.0f) + field[(size_t)other];
            if (cost < best) { best = cost; bestNode = other; }
        }
        if (bestNode < 0) return {};
        cur = bestNode;
        path.push_back(cur);
    }
    return cur == to ? path : std::vector<int>{};
}

void City::AutoTransit(int lineCount, int stopsPerLine) {
    busStops.clear();
    busLines.clear();
    PickDestinationPool();
    lineCount = std::clamp(lineCount, 1, 12);
    stopsPerLine = std::clamp(stopsPerLine, 2, 24);
    const std::vector<int> hubs = routes.pool;
    static const Color kPalette[8] = { { 40, 110, 200, 255 }, { 210, 70, 60, 255 }, { 50, 160, 90, 255 }, { 230, 170, 40, 255 },
                                       { 150, 80, 170, 255 }, { 40, 170, 180, 255 }, { 220, 110, 50, 255 }, { 120, 130, 140, 255 } };
    if (hubs.size() >= 2) {
        for (int L = 0; L < lineCount; L++) {
            const int A = hubs[(size_t)L % hubs.size()];
            // The terminal: the hub farthest from A that A can reach and that can reach back.
            int B = -1;
            float far = 0.0f;
            for (int h : hubs) {
                if (h == A) continue;
                const float d = Vector2Distance(nodes[(size_t)A].pos, nodes[(size_t)h].pos);
                if (d > far && !RouteNodes(A, h).empty() && !RouteNodes(h, A).empty()) { far = d; B = h; }
            }
            if (B < 0) continue;
            std::vector<int> loop = RouteNodes(A, B);
            const std::vector<int> back = RouteNodes(B, A);
            loop.insert(loop.end(), back.begin() + 1, back.end());
            // Total drivable length, to spread the stops evenly.
            float total = 0.0f;
            for (size_t i = 0; i + 1 < loop.size(); i++) total += Vector2Distance(nodes[(size_t)loop[i]].pos, nodes[(size_t)loop[i + 1]].pos);
            const float spacing = std::max(total / (float)stopsPerLine, 30.0f);
            BusLine line;
            line.name = "Line " + std::to_string(L + 1);
            line.color = kPalette[L % 8];
            line.buses = 2;
            float since = spacing * 0.5f;
            for (size_t i = 0; i + 1 < loop.size(); i++) {
                const int u = loop[i], v = loop[i + 1];
                int ei = -1;
                for (int cand : nodeEdges[(size_t)u]) if ((edges[(size_t)cand].a == u && edges[(size_t)cand].b == v) || (edges[(size_t)cand].a == v && edges[(size_t)cand].b == u)) { ei = cand; break; }
                if (ei < 0) continue;
                const RoadEdge& e = edges[(size_t)ei];
                const Vector2 pu = nodes[(size_t)u].pos, pv = nodes[(size_t)v].pos;
                const float len = Vector2Distance(pu, pv);
                since += len;
                if (since < spacing || len < 26.0f || e.type == (int)RoadType::Highway || e.type == (int)RoadType::Path) continue;
                since = 0.0f;
                const Vector2 pos = Vector2Scale(Vector2Add(pu, pv), 0.5f);
                const Vector2 heading = Vector2Normalize(Vector2Subtract(pv, pu));
                int idx = -1;                                          // share a stop already there (lines can meet at stops)
                for (int k = 0; k < (int)busStops.size(); k++)
                    if (Vector2Distance(busStops[(size_t)k].pos, pos) < 6.0f && Vector2DotProduct(busStops[(size_t)k].heading, heading) > 0.9f) { idx = k; break; }
                if (idx < 0) {
                    BusStop bs;
                    bs.pos = pos; bs.heading = heading; bs.name = "Stop " + std::to_string(busStops.size() + 1);
                    busStops.push_back(bs);
                    idx = (int)busStops.size() - 1;
                }
                if (line.stops.empty() || line.stops.back() != idx) line.stops.push_back(idx);
            }
            if (line.stops.size() >= 2) busLines.push_back(line);
        }
    }
    RebuildAll();
}

void City::SpawnAgent(Agent& a, bool car) {
    a.car = car;
    a.bus = -1; a.length = 4.2f; a.dwell = 0.0f; a.busSkip = -1; a.busTarget = 0;
    for (int tries = 0; tries < 20; tries++) {
        a.edge = (int)(Lcg(a.rng) % (uint32_t)std::max<size_t>(edges.size(), 1));
        const RoadEdge& e = edges[(size_t)a.edge];
        if (car && e.type == (int)RoadType::Pedestrian) continue;
        if (!car && e.type == (int)RoadType::Highway) continue;
        a.fwd = e.oneWay == 0 ? (Lcg(a.rng) & 1u) != 0 : e.oneWay == 1;
        if (!car) a.fwd = (Lcg(a.rng) & 1u) != 0;
        break;
    }
    const float len = Vector2Distance(nodes[(size_t)edges[(size_t)a.edge].a].pos, nodes[(size_t)edges[(size_t)a.edge].b].pos);
    a.s = Lcg01(a.rng) * std::max(len - 2.0f, 0.0f);
    // Personality: every car has its own desired speed, acceleration, braking and following distance.
    static const float kLimit[5] = { 11.0f, 14.0f, 25.0f, 5.0f, 3.0f };   // m/s by road type
    const float limit = kLimit[std::clamp(edges[(size_t)a.edge].type, 0, 4)];
    a.speedFactor = 0.75f + Lcg01(a.rng) * 0.4f;
    a.maxSpeed = car ? limit * a.speedFactor : 1.1f + Lcg01(a.rng) * 0.6f;
    a.accel = 1.4f + Lcg01(a.rng) * 1.6f;
    a.decel = 2.2f + Lcg01(a.rng) * 2.0f;
    a.headway = 1.0f + Lcg01(a.rng) * 1.0f;
    a.minGap = 1.8f + Lcg01(a.rng) * 1.4f;
    a.speed = a.maxSpeed * (0.5f + Lcg01(a.rng) * 0.5f);
    a.wait = 0.0f; a.released = false; a.placed = false; a.inJ = false; a.nextEdge = -1;
    a.dest = car && params.routedTraffic ? PickDestination(a.rng) : -1;
    const int lanes = std::max(edges[(size_t)a.edge].lanes > 0 ? edges[(size_t)a.edge].lanes : params.lanes, 1);
    a.lane = car ? (int)(Lcg(a.rng) % (uint32_t)std::max(edges[(size_t)a.edge].oneWay ? lanes : lanes / 2, 1)) : (int)(Lcg(a.rng) & 1u);
    static const Color kCar[8] = { {200,40,40,255}, {40,90,200,255}, {230,230,230,255}, {30,30,34,255},
                                   {220,180,40,255}, {50,150,90,255}, {150,150,158,255}, {180,100,40,255} };
    static const Color kPed[6] = { {200,60,60,255}, {60,120,200,255}, {230,200,60,255}, {70,170,100,255}, {200,200,200,255}, {160,80,160,255} };
    a.laneF = (float)a.lane; a.turn = 0;
    a.colorRoll = Lcg01(a.rng); a.colorRoll2 = Lcg01(a.rng);
    a.color = car ? kCar[Lcg(a.rng) % 8u] : kPed[Lcg(a.rng) % 6u];
}

// Car colour from the table: weighted by chance, or uniform in "all" mode. "Other" entries use a vivid palette.
Color City::PickCarColor(const Agent& a) const {
    static const Color kVivid[8] = { {200,40,40,255}, {40,90,200,255}, {230,230,230,255}, {30,30,34,255},
                                     {220,180,40,255}, {50,150,90,255}, {150,150,158,255}, {180,100,40,255} };
    const auto& tab = params.carColors;
    if (tab.empty()) return Color{ 128, 131, 138, 255 };
    size_t pick = tab.size() - 1;
    if (params.carColorAll) pick = std::min((size_t)(a.colorRoll * (float)tab.size()), tab.size() - 1);
    else {
        float total = 0.0f;
        for (const CarColor& c : tab) total += std::max(c.weight, 0.0f);
        if (total <= 0.0f) pick = std::min((size_t)(a.colorRoll * (float)tab.size()), tab.size() - 1);
        else {
            float r = a.colorRoll * total;
            for (size_t i = 0; i < tab.size(); i++) {
                r -= std::max(tab[i].weight, 0.0f);
                if (r < 0.0f) { pick = i; break; }
            }
        }
    }
    const CarColor& c = tab[pick];
    if (!c.other) return c.color;
    // Vivid palette without the neutral entries (those have their own chances in the table).
    static const int kIdx[5] = { 0, 1, 4, 5, 7 };
    return kVivid[kIdx[std::min((int)(a.colorRoll2 * 5.0f), 4)]];
}

// Lane position and heading on a road at distance s (from the start node of the travel direction).
void City::LanePose(int ei, bool fwd, float s, float lane, bool car, Vector3& pos, Vector2& heading) const {
    const RoadEdge& e = edges[(size_t)ei];
    const Vector2 A = nodes[(size_t)e.a].pos, B = nodes[(size_t)e.b].pos;
    const float len = std::max(Vector2Distance(A, B), 1e-3f);
    const Vector2 d = Vector2Scale(Vector2Subtract(B, A), 1.0f / len);
    const float sNorm = Clamp((fwd ? s : len - s) / len, 0.0f, 1.0f);
    const Vector2 along = fwd ? d : Vector2Scale(d, -1.0f);
    const Vector2 right = { along.y, -along.x };          // right-hand side of travel in (x,z)
    float lat;
    if (car) {
        const int lanes = std::max(e.lanes > 0 ? e.lanes : params.lanes, 1);
        const float lw = e.width > 0.0f ? e.width : params.laneWidth;
        // Right-hand traffic: two-way roads use the right half; one-way roads spread across all lanes.
        lat = e.oneWay ? -(lane + 0.5f - (float)lanes * 0.5f) * lw * (fwd ? 1.0f : -1.0f)
                       : (lane + 0.5f) * lw * (params.leftHandTraffic ? -1.0f : 1.0f);
        if (e.oneWay) lat = -lat * (fwd ? -1.0f : -1.0f);
    } else {
        // Pedestrians keep to one sidewalk of the road (lane 1 = left of A->B, 0 = right), whichever way they walk.
        const float aHalf = EdgeAsphaltHalf(ei), sHalf = EdgeSlabHalf(ei);
        const Vector2 nLeft = { -d.y, d.x };
        const Vector2 c = Vector2Add(Vector2Lerp(A, B, sNorm), Vector2Scale(nLeft, (aHalf + (sHalf - aHalf) * 0.5f) * (lane > 0.5f ? 1.0f : -1.0f)));
        pos = { c.x, EdgeSurfaceY(ei, sNorm) + kRoadElevation + 0.1f + e.curbH, c.y };
        heading = along;
        return;
    }
    const Vector2 c = Vector2Add(Vector2Lerp(A, B, sNorm), Vector2Scale(right, lat));
    pos = { c.x, EdgeSurfaceY(ei, sNorm) + kRoadElevation + (car ? 0.14f : 0.1f + e.curbH), c.y };
    heading = along;
}

float City::ArmClear(int edge) const {
    if (edge < 0 || (size_t)edge >= edges.size()) return 8.0f;
    const RoadEdge& e = edges[(size_t)edge];
    const float len = Vector2Distance(nodes[(size_t)e.a].pos, nodes[(size_t)e.b].pos);
    float clear = EdgeAsphaltHalf(edge) + std::max(params.cornerRadius, 0.0f) + 0.5f;
    for (int nd : { e.a, e.b })   // a roundabout at either end: the road starts at the ring's outer edge
        if (nodes[(size_t)nd].junction && nodes[(size_t)nd].jkind == (int)JunctionKind::Roundabout) {
            const float R = RoundaboutOuterRadius(nd, true), w = EdgeAsphaltHalf(edge);
            clear = std::max(clear, sqrtf(std::max(R * R - w * w, 1.0f)));
        }
    return std::min(clear, len * 0.48f);
}

float City::RoundaboutRingWidth(int node) const {
    float meanHalf = 0.0f; int cnt = 0;
    if ((size_t)node < nodeEdges.size())
        for (int ei : nodeEdges[(size_t)node]) { meanHalf += EdgeAsphaltHalf(ei); cnt++; }
    meanHalf = cnt ? meanHalf / (float)cnt : params.lanes * params.laneWidth * 0.5f;
    if ((size_t)node < nodes.size() && nodes[(size_t)node].rbRing > 0.0f) return nodes[(size_t)node].rbRing;
    return std::max(meanHalf * 2.0f, 4.0f);
}

float City::RoundaboutRadius(int node) const {
    if ((size_t)node < nodes.size() && nodes[(size_t)node].rbIsland > 0.0f) return std::max(nodes[(size_t)node].rbIsland, 1.5f);
    return std::max(RoundaboutRingWidth(node) * 0.9f, 4.5f);
}

void City::SetRoundaboutProps(int node, float island, float ring, bool splitters, bool concrete) {
    if (node < 0 || (size_t)node >= nodes.size()) return;
    RoadNode& n = nodes[(size_t)node];
    n.rbIsland = island <= 0.0f ? 0.0f : Clamp(island, 1.5f, 30.0f);
    n.rbRing = ring <= 0.0f ? 0.0f : Clamp(ring, 3.0f, 20.0f);
    n.rbSplitters = splitters;
    n.rbConcrete = concrete;
    RebuildAll();
}

bool City::TouchesRoundabout(const Vector2& p, float r) const {
    for (int i = 0; i < (int)nodes.size(); i++) {
        const RoadNode& n = nodes[(size_t)i];
        if (!n.junction || n.jkind != (int)JunctionKind::Roundabout) continue;
        if (Vector2Distance(p, n.pos) < RoundaboutOuterRadius(i, false) + r) return true;
    }
    return false;
}

float City::RoundaboutOuterRadius(int node, bool asphalt) const {
    const float r = RoundaboutRadius(node) + RoundaboutRingWidth(node);
    return asphalt ? r : r + params.sidewalk;
}

// Plans the path through the junction from the end of the current road onto the next one: a curve for
// ordinary junctions, an arc around the central island for roundabouts (counter-clockwise when
// driving on the right, clockwise on the left).
void City::PlanJunction(Agent& a) {
    a.jpath.clear(); a.jcum.clear();
    if (a.nextEdge < 0 || (size_t)a.nextEdge >= edges.size()) return;
    const RoadEdge& eA = edges[(size_t)a.edge];
    const RoadEdge& eB = edges[(size_t)a.nextEdge];
    const int node = a.fwd ? eA.b : eA.a;
    const float lenA = Vector2Distance(nodes[(size_t)eA.a].pos, nodes[(size_t)eA.b].pos);
    const float JA = ArmClear(a.edge), JB = ArmClear(a.nextEdge);
    const int lanesB = std::max(eB.lanes > 0 ? eB.lanes : params.lanes, 1);
    a.jLaneB = std::min(a.lane, std::max(eB.oneWay ? lanesB : lanesB / 2, 1) - 1);
    Vector3 p0, p2; Vector2 h0, h2;
    // Start where the car actually is if it is already past the nominal entry (e.g. it just switched from the far model).
    LanePose(a.edge, a.fwd, std::clamp(a.s, lenA - JA, lenA - 0.3f), a.laneF, true, p0, h0);
    LanePose(a.nextEdge, a.nextFwd, JB, (float)a.jLaneB, true, p2, h2);
    const Vector2 P0 = { p0.x, p0.z }, P2 = { p2.x, p2.z };
    std::vector<Vector2> pts;
    const bool rb = nodes[(size_t)node].junction && nodes[(size_t)node].jkind == (int)JunctionKind::Roundabout;
    if (rb) {
        const Vector2 c = nodes[(size_t)node].pos;
        const float Rc = RoundaboutRadius(node) + RoundaboutRingWidth(node) * 0.5f;
        const float f0 = atan2f(P0.y - c.y, P0.x - c.x), f1 = atan2f(P2.y - c.y, P2.x - c.x);
        const float dir = params.leftHandTraffic ? -1.0f : 1.0f;      // +1: counter-clockwise (right-hand traffic)
        float d = dir > 0 ? fmodf(f1 - f0, 2.0f * PI) : fmodf(f0 - f1, 2.0f * PI);
        if (d < 0.0f) d += 2.0f * PI;
        if (d < 0.35f) d += 2.0f * PI;                                // always go round at least a little
        // Join and leave the ring tangentially, over a stretch of arc, so the car swings on and off instead of turning 90 degrees.
        const float lead = std::min(0.75f, d * 0.4f);
        const float fa = f0 + dir * lead, fb = f0 + dir * (d - lead);
        auto ringPt = [&](float f) { return Vector2{ c.x + cosf(f) * Rc, c.y + sinf(f) * Rc }; };
        auto tangent = [&](float f) { return Vector2{ -sinf(f) * dir, cosf(f) * dir }; };
        auto cubic = [&](Vector2 b0, Vector2 b1, Vector2 b2, Vector2 b3, int n, bool skipFirst) {
            for (int i = skipFirst ? 1 : 0; i <= n; i++) {
                const float t = (float)i / (float)n, u = 1.0f - t;
                pts.push_back({ u*u*u*b0.x + 3*u*u*t*b1.x + 3*u*t*t*b2.x + t*t*t*b3.x,
                                u*u*u*b0.y + 3*u*u*t*b1.y + 3*u*t*t*b2.y + t*t*t*b3.y });
            }
        };
        {   // entry
            const Vector2 ra = ringPt(fa), ta = tangent(fa);
            const float k = Vector2Distance(P0, ra) * 0.55f;
            cubic(P0, Vector2Add(P0, Vector2Scale(h0, k)), Vector2Subtract(ra, Vector2Scale(ta, k)), ra, 10, false);
        }
        {   // arc between the join and leave points
            const float span = fb - fa == 0.0f ? 0.0f : dir * (d - 2.0f * lead);
            const int n = std::max(2, (int)ceilf(fabsf(span) / 0.15f));
            for (int k = 1; k <= n; k++) pts.push_back(ringPt(fa + span * (float)k / (float)n));
        }
        {   // exit
            const Vector2 rb = ringPt(fb), tb = tangent(fb);
            const float k = Vector2Distance(rb, P2) * 0.55f;
            cubic(rb, Vector2Add(rb, Vector2Scale(tb, k)), Vector2Subtract(P2, Vector2Scale(h2, k)), P2, 10, true);
        }
        a.jVMax = 5.5f;
    } else {
        // Control point: where the entry and exit lane lines meet (a smooth fillet through the corner).
        const float cr = h0.x * h2.y - h0.y * h2.x;
        Vector2 P1 = Vector2Scale(Vector2Add(P0, P2), 0.5f);
        if (fabsf(cr) > 0.05f) {
            const Vector2 dp = Vector2Subtract(P2, P0);
            const float t0 = (dp.x * h2.y - dp.y * h2.x) / cr;
            const float chord = Vector2Distance(P0, P2);
            if (t0 > 0.0f && t0 < chord * 2.0f) P1 = Vector2Add(P0, Vector2Scale(h0, t0));
        }
        for (int i = 0; i <= 12; i++) {
            const float t = (float)i / 12.0f, u = 1.0f - t;
            pts.push_back({ u * u * P0.x + 2 * u * t * P1.x + t * t * P2.x, u * u * P0.y + 2 * u * t * P1.y + t * t * P2.y });
        }
        const float turn = fabsf(atan2f(h0.x * h2.y - h0.y * h2.x, h0.x * h2.x + h0.y * h2.y));
        a.jVMax = 99.0f;
        if (turn > 0.15f) {
            const float chord = Vector2Distance(P0, P2);
            const float R = std::max(chord / std::max(2.0f * sinf(std::min(turn, 1.5f) * 0.5f), 0.2f), 2.0f);
            a.jVMax = std::max(sqrtf(2.6f * R), 2.5f);
        }
    }
    a.jpath = pts;
    a.jcum.assign(pts.size(), 0.0f);
    for (size_t i = 1; i < pts.size(); i++) a.jcum[i] = a.jcum[i - 1] + Vector2Distance(pts[i - 1], pts[i]);
    a.jlen = std::max(a.jcum.back(), 0.5f);
    a.jy0 = p0.y; a.jy2 = p2.y; a.jExit = JB; a.jFrom = a.edge; a.jTo = a.nextEdge; a.jNode = node; a.jt = 0.0f;
}

int City::ApproachSector(int node, int edge) const {
    const RoadEdge& e = edges[(size_t)edge];
    const int other = e.a == node ? e.b : e.a;
    const Vector2 d = Vector2Subtract(nodes[(size_t)other].pos, nodes[(size_t)node].pos);
    const float ang = atan2f(d.y, d.x);
    return ((int)roundf(ang / (PI / 4.0f)) + 8) & 7;
}

int City::SignalState(int node, int edge) const {
    const RoadNode& nd = nodes[(size_t)node];
    const int bit = ApproachSector(node, edge);
    static const SignalPhase kDefault[2] = { { 9.0f, 0x00 }, { 9.0f, 0x00 } };
    // Default programme: east/west roads, then north/south roads.
    SignalPhase def[2] = { { 9.0f, (uint8_t)((1 << 0) | (1 << 1) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 7)) }, { 9.0f, (uint8_t)((1 << 2) | (1 << 6)) } };
    (void)kDefault;
    const SignalPhase* ph = nd.phases.empty() ? def : nd.phases.data();
    const size_t n = nd.phases.empty() ? 2 : nd.phases.size();
    float total = 0.0f;
    for (size_t i = 0; i < n; i++) total += std::max(ph[i].duration, 1.0f) + nd.yellow + nd.allRed;
    if (total < 1.0f) return 2;
    float t = fmodf(trafficClock + nd.sigOffset, total);
    if (t < 0.0f) t += total;
    for (size_t i = 0; i < n; i++) {
        const float dur = std::max(ph[i].duration, 1.0f);
        const bool mine = (ph[i].mask >> bit) & 1;
        if (t < dur) return mine ? 2 : 0;
        t -= dur;
        if (t < nd.yellow) return mine ? 1 : 0;
        t -= nd.yellow + nd.allRed;
        if (t < 0.0f) return 0;
    }
    return 0;
}

float City::EdgeSpeedLimit(int edge) const {
    if (edge < 0 || (size_t)edge >= edges.size()) return 11.0f;
    const RoadEdge& e = edges[(size_t)edge];
    if (e.speedLimit > 0.0f) return e.speedLimit;
    static const float kLimit[5] = { 11.0f, 14.0f, 25.0f, 5.0f, 3.0f };
    return kLimit[std::clamp(e.type, 0, 4)];
}

void City::SetEdgeTurnLanes(int edge, uint16_t turnA, uint16_t turnB, float speedLimit) {
    if (edge < 0 || (size_t)edge >= edges.size()) return;
    edges[(size_t)edge].turnA = turnA;
    edges[(size_t)edge].turnB = turnB;
    edges[(size_t)edge].speedLimit = Clamp(speedLimit, 0.0f, 60.0f);
    RebuildAll();   // turn arrows are part of the road mesh
}

void City::StepTraffic(float dt) {
    const auto stepT0 = std::chrono::steady_clock::now();
    float statMinGap = 1e9f;
    if (edges.empty() || nodeEdges.size() != nodes.size()) { agents.clear(); return; }
    trafficFrame++;
    simTime += dt;
    // Keep the agent counts in sync with the params.
    size_t wantCars = (size_t)std::max(params.cars, 0), wantPeds = (size_t)std::max(params.pedestrians, 0);
    int carChangeBudget = 1 << 20;   // cars added/removed this step (rush hours change the count gradually)
    if (params.rushHours) {
        static const float kPts[10][2] = { {0,0.2f},{5,0.2f},{7,1.0f},{9.5f,1.0f},{11,0.6f},{15.5f,0.6f},{17,1.0f},{19.5f,1.0f},{22,0.25f},{24,0.2f} };
        const float h = fmodf(gfx::GetTimeOfDay(), 24.0f);   // the global game time
        float density = 1.0f;
        for (int k = 0; k < 9; k++)
            if (h >= kPts[k][0] && h <= kPts[k + 1][0]) {
                const float t = (h - kPts[k][0]) / std::max(kPts[k + 1][0] - kPts[k][0], 1e-3f);
                density = kPts[k][1] + (kPts[k + 1][1] - kPts[k][1]) * t;
                break;
            }
        wantCars = (size_t)lroundf((float)wantCars * density);
        carChangeBudget = 2;
    }
    size_t haveCars = 0, havePeds = 0;
    for (const Agent& a : agents) { if (a.bus >= 0) continue; (a.car ? haveCars : havePeds)++; }   // buses have their own fleets
    auto trim = [&](bool car, size_t want, size_t have) {
        int budget = car ? carChangeBudget : (1 << 20);
        while (have > want && budget-- > 0) {
            size_t victim = agents.size();
            for (size_t i = agents.size(); i-- > 0;) if (agents[i].bus < 0 && agents[i].car == car && (!car || agents[i].far)) { victim = i; break; }   // out of sight first
            if (victim == agents.size()) for (size_t i = agents.size(); i-- > 0;) if (agents[i].bus < 0 && agents[i].car == car) { victim = i; break; }
            if (victim == agents.size()) break;
            agents.erase(agents.begin() + (long)victim);
            have--;
        }
    };
    trim(true, wantCars, haveCars);
    trim(false, wantPeds, havePeds);
    auto overlapsExisting = [&](const Agent& a) {
        for (const Agent& o : agents)
            if (o.car && o.edge == a.edge && o.fwd == a.fwd && o.lane == a.lane && fabsf(o.s - a.s) < 7.0f) return true;
        return false;
    };
    for (size_t i = haveCars; i < wantCars && carChangeBudget-- > 0; i++) {
        Agent a; a.rng = (uint32_t)(0x9E3779B9u * (uint32_t)(agents.size() + 1)) ^ (uint32_t)params.seed;
        SpawnAgent(a, true);
        for (int tries = 0; tries < 30 && overlapsExisting(a); tries++) SpawnAgent(a, true);
        agents.push_back(a);
    }
    for (size_t i = havePeds; i < wantPeds; i++) { Agent a; a.rng = (uint32_t)(0x85EBCA6Bu * (uint32_t)(agents.size() + 1)) ^ (uint32_t)params.seed; SpawnAgent(a, false); agents.push_back(a); }

    // Buses: one fleet per line, respawned whenever the lines, stops or roads change; topped up one bus per step.
    {
        uint64_t sig = graphVersion * 1315423911ull + busLines.size() * 2654435761ull;
        for (const BusStop& bs : busStops) sig = sig * 1099511628211ull ^ (uint64_t)(int64_t)(bs.pos.x * 16.0f) ^ ((uint64_t)(int64_t)(bs.pos.y * 16.0f) << 20) ^ (bs.fwd ? 1u : 0u);
        for (const BusLine& l : busLines) {
            sig = sig * 1099511628211ull ^ (uint64_t)l.buses;
            for (int s : l.stops) sig = sig * 1099511628211ull ^ (uint64_t)(s + 1);
        }
        if (sig != busSig) {
            agents.erase(std::remove_if(agents.begin(), agents.end(), [](const Agent& a) { return a.bus >= 0; }), agents.end());
            busSig = sig;
        }
        bool spawnedOne = false;
        for (size_t li = 0; li < busLines.size(); li++) {
            const BusLine& L = busLines[li];
            bool ok = L.stops.size() >= 2;
            for (int s : L.stops) if ((size_t)s >= busStops.size() || busStops[(size_t)s].edge < 0) ok = false;
            int have = 0;
            for (const Agent& a : agents) if (a.bus == (int)li) have++;
            if (!ok) {
                if (have) agents.erase(std::remove_if(agents.begin(), agents.end(), [li](const Agent& a) { return a.bus == (int)li; }), agents.end());
                continue;
            }
            while (have > L.buses) {
                for (size_t i = agents.size(); i-- > 0;) if (agents[i].bus == (int)li) { agents.erase(agents.begin() + (long)i); break; }
                have--;
            }
            if (have < L.buses && !spawnedOne) {
                spawnedOne = true;
                Agent a; a.rng = (uint32_t)(0xC2B2AE35u * (uint32_t)(li * 31 + have + 1)) ^ (uint32_t)params.seed;
                SpawnAgent(a, true);
                a.bus = (int)li;
                a.length = 11.0f;
                a.dest = -1;
                a.busTarget = (have * (int)L.stops.size() / std::max(L.buses, 1)) % (int)L.stops.size();
                const BusStop& stp = busStops[(size_t)L.stops[(size_t)a.busTarget]];
                const RoadEdge& se = edges[(size_t)stp.edge];
                a.edge = stp.edge; a.fwd = stp.fwd;
                const float elen = Vector2Distance(nodes[(size_t)se.a].pos, nodes[(size_t)se.b].pos);
                a.s = std::max((stp.fwd ? stp.s : elen - stp.s) - 30.0f, 1.0f);
                const int lanes = std::max(se.lanes > 0 ? se.lanes : params.lanes, 1);
                a.lane = std::max(se.oneWay ? lanes : lanes / 2, 1) - 1;       // the kerb lane
                a.laneF = (float)a.lane;
                a.speedFactor = 0.85f; a.maxSpeed = std::min(EdgeSpeedLimit(a.edge), 10.0f) * a.speedFactor;
                a.accel = 1.2f; a.decel = 2.0f; a.headway = 1.6f; a.minGap = 3.0f; a.speed = 0.0f;
                a.color = L.color;
                agents.push_back(a);
            }
        }
    }

    // Per-edge/direction lists of cars, for following distance (near cars only matter, far ones are free-flowing).
    std::unordered_map<int, std::vector<int>> onEdge;
    for (int i = 0; i < (int)agents.size(); i++) if (agents[(size_t)i].car) onEdge[agents[(size_t)i].edge * 2 + (agents[(size_t)i].fwd ? 1 : 0)].push_back(i);

    // Cars in or approaching each junction, for right-of-way decisions.
    std::unordered_map<int, std::vector<int>> nodeCars;
    for (int i = 0; i < (int)agents.size(); i++) {
        const Agent& o = agents[(size_t)i];
        if (!o.car || o.far || o.edge < 0 || (size_t)o.edge >= edges.size()) continue;
        if (o.inJ && o.jNode >= 0) nodeCars[o.jNode].push_back(i);
        else if (o.nextEdge >= 0 && !o.jpath.empty()) nodeCars[o.fwd ? edges[(size_t)o.edge].b : edges[(size_t)o.edge].a].push_back(i);
    }
    // Two planned paths conflict when they come closer than a car width plus margin.
    auto PathsConflict = [](const std::vector<Vector2>& pa, size_t fromA, const std::vector<Vector2>& pb, size_t fromB) {
        for (size_t i = fromA; i < pa.size(); i++)
            for (size_t j = fromB; j < pb.size(); j++)
                if (Vector2DistanceSqr(pa[i], pb[j]) < 2.7f * 2.7f) return true;
        return false;
    };

    const float D = params.trafficDetailDistance;
    for (int i = 0; i < (int)agents.size(); i++) {
        Agent& a = agents[(size_t)i];
        if (a.edge < 0 || (size_t)a.edge >= edges.size() || (a.inJ && (a.nextEdge < 0 || (size_t)a.nextEdge >= edges.size()))) { SpawnAgent(a, a.car); continue; }

        // A planned junction path only belongs to the road pair it was made for; drop it otherwise (e.g. after a
        // far-model hand-over) so a car never rides a path from a different junction.
        if (!a.jpath.empty() && !a.inJ && (a.jFrom != a.edge || a.jTo != a.nextEdge)) a.jpath.clear();
        if (a.inJ && (a.jpath.size() < 2 || a.jTo != a.nextEdge)) { a.inJ = false; a.jpath.clear(); }

        // Detail level by distance to the camera (with hysteresis).
        if (a.car) {
            if (D > 0.0f && hasTrafficFocus && a.placed) {
                const float dist = Vector2Distance({ a.pos.x, a.pos.z }, { trafficFocus.x, trafficFocus.z });
                if (!a.far && dist > D * 1.1f) a.far = true;
                else if (a.far && dist < D * 0.9f) a.far = false;
            } else a.far = false;
        }
        if (a.bus >= 0) a.far = false;
        a.nearTime = a.far ? 0.0f : a.nearTime + dt;
        float step = dt;
        if (a.car && !a.far) a.wheelRot = fmodf(a.wheelRot + a.speed * dt / 0.32f, 2.0f * PI);
        if (a.far) {                                         // far agents tick every 4th frame with a 4x step
            if (((trafficFrame + (unsigned)i) & 3u) != 0u) continue;
            step = dt * 4.0f;
        }

        const RoadEdge& e = edges[(size_t)a.edge];
        const float len = std::max(Vector2Distance(nodes[(size_t)e.a].pos, nodes[(size_t)e.b].pos), 1e-3f);
        const int toNode = a.fwd ? e.b : e.a;
        Vector2 hdTmp{};
        const float JA = ArmClear(a.edge);

        // ---- pedestrians: walk the sidewalk, round corners, wait for the light and use the crossing ----
        if (!a.car) {
            const float Jp = std::min(ArmClear(a.edge) + 2.0f, len * 0.48f);
            auto sideSign = [](int lane) { return lane > 0 ? 1.0f : -1.0f; };
            // Sidewalk point on edge ei at distance sEdge from node a, on `lane` side.
            auto walkPoint = [&](int ei, float sEdge, int lane) {
                Vector3 p; Vector2 h;
                LanePose(ei, true, sEdge, (float)lane, false, p, h);
                return p;
            };
            Vector3 tp; float ty = a.yaw;
            if (a.inJ) {
                if (a.wait > 0.5f) {
                    // Standing at the kerb until traffic on the road being crossed has a red light.
                    if (SignalState(toNode, a.edge) == 0) a.wait = 0.0f;
                    tp = { a.jc0.x, a.jy0, a.jc0.y };
                } else {
                    a.jt += a.maxSpeed * step / a.jlen;
                    if (a.jt >= 1.0f) {
                        a.edge = a.nextEdge; a.fwd = a.nextFwd; a.s = a.jExit; a.lane = a.turn > 0 ? 1 : 0;
                        a.inJ = false; a.nextEdge = -1; a.turn = 0;
                    }
                    const float t = Clamp(a.jt, 0.0f, 1.0f);
                    const Vector2 q = Vector2Lerp(a.jc0, a.jc2, t);
                    tp = { q.x, a.jy0 + (a.jy2 - a.jy0) * t, q.y };
                    const Vector2 dir = Vector2Subtract(a.jc2, a.jc0);
                    if (dir.x != 0.0f || dir.y != 0.0f) ty = atan2f(-dir.y, dir.x);
                }
                if (a.inJ) {
                    if (!a.placed) { a.pos = tp; a.yaw = ty; a.placed = true; }
                    else {
                        a.pos = tp;
                        float dy = ty - a.yaw;
                        while (dy > PI) dy -= 2.0f * PI;
                        while (dy < -PI) dy += 2.0f * PI;
                        a.yaw += dy * (1.0f - expf(-step * 10.0f));
                    }
                    continue;
                }
            } else {
                a.s += a.maxSpeed * step;
                if (a.s >= len - Jp) {
                    // Reached the corner: choose to turn onto another road or cross this one.
                    const float sEnd = a.fwd ? len - Jp : Jp;                 // end point in edge coordinates
                    const Vector3 p0 = walkPoint(a.edge, sEnd, a.lane);
                    std::vector<int> others;
                    for (int ei2 : nodeEdges[(size_t)toNode])
                        if (ei2 != a.edge && edges[(size_t)ei2].type != (int)RoadType::Highway) others.push_back(ei2);
                    const bool cross = others.empty() || (Lcg01(a.rng) < 0.35f && nodes[(size_t)toNode].junction);
                    Vector3 p2; int newSide;
                    if (cross) {
                        newSide = a.lane > 0 ? 0 : 1;
                        p2 = walkPoint(a.edge, sEnd, newSide);
                        a.nextEdge = a.edge; a.nextFwd = !a.fwd; a.jExit = Jp;
                        const bool signalled = nodes[(size_t)toNode].junction && nodes[(size_t)toNode].jkind == (int)JunctionKind::TrafficLight;
                        a.wait = signalled ? 1.0f : 0.0f;
                    } else {
                        const int e2 = others[Lcg(a.rng) % (uint32_t)others.size()];
                        const float len2 = std::max(Vector2Distance(nodes[(size_t)edges[(size_t)e2].a].pos, nodes[(size_t)edges[(size_t)e2].b].pos), 1e-3f);
                        const float J2 = std::min(ArmClear(e2) + 2.0f, len2 * 0.48f);
                        const bool fwd2 = edges[(size_t)e2].a == toNode;      // walking away from the junction
                        const float s2 = fwd2 ? J2 : len2 - J2;
                        const Vector3 q0 = walkPoint(e2, s2, 0), q1 = walkPoint(e2, s2, 1);
                        const float d0 = Vector3Distance(p0, q0), d1 = Vector3Distance(p0, q1);
                        newSide = d0 <= d1 ? 0 : 1;
                        p2 = newSide == 0 ? q0 : q1;
                        a.nextEdge = e2; a.nextFwd = fwd2; a.jExit = J2;
                        a.wait = 0.0f;
                    }
                    a.turn = newSide > 0 ? 1 : 0;                                // reused: side to adopt on arrival
                    a.jc0 = { p0.x, p0.z }; a.jc2 = { p2.x, p2.z }; a.jy0 = p0.y; a.jy2 = p2.y;
                    a.jlen = std::max(Vector2Distance(a.jc0, a.jc2), 0.3f);
                    a.jt = 0.0f; a.inJ = true;
                    a.pos = p0;
                    a.placed = true;
                    continue;
                }
            }
            LanePose(a.edge, a.fwd, a.s, (float)a.lane, false, tp, hdTmp);
            ty = atan2f(-hdTmp.y, hdTmp.x);
            if (!a.placed) { a.pos = tp; a.yaw = ty; a.placed = true; }
            else {
                a.pos = tp;
                float dy = ty - a.yaw;
                while (dy > PI) dy -= 2.0f * PI;
                while (dy < -PI) dy += 2.0f * PI;
                a.yaw += dy * (1.0f - expf(-step * 10.0f));
            }
            continue;
        }

        // ---- cars ----
        a.laneF += Clamp((float)a.lane - a.laneF, -0.7f * step, 0.7f * step);
        // Choose the next road early so the junction curve and the lane are known on approach.
        auto chooseNext = [&]() {
            std::vector<int> options;
            for (int ei2 : nodeEdges[(size_t)toNode]) {
                if (ei2 == a.edge) continue;
                const RoadEdge& e2 = edges[(size_t)ei2];
                if (e2.type == (int)RoadType::Pedestrian || !EdgeAllowsFrom(e2, toNode)) continue;
                options.push_back(ei2);
            }
            if (options.empty()) { a.nextEdge = -2; a.turn = 0; return; }          // dead end: turn around
            // Prefer a road reachable from a lane this car may use (turn-lane markings), else any.
            const uint16_t mask = toNode == e.b ? e.turnB : e.turnA;
            const Vector2 hA = Vector2Normalize(Vector2Subtract(nodes[(size_t)toNode].pos, nodes[(size_t)(a.fwd ? e.a : e.b)].pos));
            auto manoeuvre = [&](int ei2) {
                const RoadEdge& e2 = edges[(size_t)ei2];
                const int other = e2.a == toNode ? e2.b : e2.a;
                const Vector2 hB = Vector2Normalize(Vector2Subtract(nodes[(size_t)other].pos, nodes[(size_t)toNode].pos));
                const float cross = hA.x * hB.y - hA.y * hB.x, dotv = hA.x * hB.x + hA.y * hB.y;
                const float ang = atan2f(cross, dotv);
                if (ang > 0.5f) return 0;      // left
                if (ang < -0.5f) return 2;     // right
                return 1;                      // straight
            };
            const int lanesHere = std::max(e.lanes > 0 ? e.lanes : params.lanes, 1);
            const int perDir = e.oneWay ? lanesHere : std::max(lanesHere / 2, 1);
            auto laneOk = [&](int lane, int m) {
                if (lane >= 4) return true;
                const int bits = (mask >> (4 * lane)) & 7;
                return bits == 0 || ((bits >> m) & 1);
            };
            std::vector<int> usable;
            for (int ei2 : options) {
                const int m = manoeuvre(ei2);
                for (int l = 0; l < perDir; l++) if (laneOk(l, m)) { usable.push_back(ei2); break; }
            }
            const std::vector<int>& pool = usable.empty() ? options : usable;
            a.nextEdge = pool[Lcg(a.rng) % (uint32_t)pool.size()];
            if (params.routedTraffic && a.bus < 0) {
                if (a.dest < 0 || a.dest == toNode) {                              // trip finished (or none yet): pick the next one
                    if (a.dest == toNode) tripsCompleted++;
                    a.dest = PickDestination(a.rng);
                    for (int tries = 0; tries < 4 && a.dest == toNode; tries++) a.dest = PickDestination(a.rng);
                }
                if (a.dest >= 0) {
                    const std::vector<float>& field = RouteField(a.dest);
                    float best = std::numeric_limits<float>::infinity();
                    for (int ei2 : pool) {
                        const RoadEdge& e2 = edges[(size_t)ei2];
                        const int other = e2.a == toNode ? e2.b : e2.a;
                        const float len2 = Vector2Distance(nodes[(size_t)e2.a].pos, nodes[(size_t)e2.b].pos);
                        const float cost = len2 / std::max(EdgeSpeedLimit(ei2), 1.0f) + field[(size_t)other];
                        if (cost < best) { best = cost; a.nextEdge = ei2; }
                    }
                }
            }
            if (a.bus >= 0 && (size_t)a.bus < busLines.size() && !busLines[(size_t)a.bus].stops.empty()) {
                // Buses follow their line: turn onto the stop's road when at its start, else take the fastest way there.
                const BusLine& L = busLines[(size_t)a.bus];
                const int sidx = L.stops[(size_t)a.busTarget % L.stops.size()];
                if ((size_t)sidx < busStops.size() && busStops[(size_t)sidx].edge >= 0) {
                    const BusStop& stp = busStops[(size_t)sidx];
                    const RoadEdge& se = edges[(size_t)stp.edge];
                    const int fromNode = stp.fwd ? se.a : se.b;
                    bool forced = false;
                    if (toNode == fromNode) for (int ei2 : options) if (ei2 == stp.edge) { a.nextEdge = ei2; forced = true; break; }
                    for (int pass = 0; pass < 2 && !forced; pass++) {
                        const std::vector<float>& field = RouteField(fromNode, pass == 0 ? stp.edge : -1);
                        float best = std::numeric_limits<float>::infinity();
                        int pick = -1;
                        for (int ei2 : pool) {
                            if (pass == 0 && ei2 == stp.edge) continue;   // arriving along the stop road does not help
                            const RoadEdge& e2 = edges[(size_t)ei2];
                            const int other = e2.a == toNode ? e2.b : e2.a;
                            const float len2 = Vector2Distance(nodes[(size_t)e2.a].pos, nodes[(size_t)e2.b].pos);
                            const float cost = len2 / std::max(EdgeSpeedLimit(ei2), 1.0f) + field[(size_t)other];
                            if (cost < best) { best = cost; pick = ei2; }
                        }
                        if (pick >= 0 && std::isfinite(best)) { a.nextEdge = pick; break; }
                    }
                }
            }
            a.nextFwd = edges[(size_t)a.nextEdge].a == toNode;
            const int m = manoeuvre(a.nextEdge);
            a.turn = m == 0 ? -1 : (m == 2 ? 1 : 0);
            if (!laneOk(a.lane, m)) {                // move to the nearest lane that allows this manoeuvre
                int best = a.lane, bd = 99;
                for (int l = 0; l < perDir; l++) if (laneOk(l, m) && abs(l - a.lane) < bd) { bd = abs(l - a.lane); best = l; }
                a.lane = best;
            }
            if (!a.far) PlanJunction(a);
        };
        if (!a.inJ && a.nextEdge == -1 && len - a.s < 70.0f) chooseNext();

        if (a.far) {
            if (!a.inJ && !a.jpath.empty()) a.jpath.clear();   // plans are only valid in the detailed model
            // Cheap model: constant desired speed, no following, no signals, snap turns.
            a.speed = EdgeSpeedLimit(a.edge) * a.speedFactor;
            // Still never drive through the car ahead (so far cars arrive at the camera without overlapping): cheap same-lane check.
            for (int j : onEdge[a.edge * 2 + (a.fwd ? 1 : 0)]) {
                if (j == i) continue;
                const Agent& o = agents[(size_t)j];
                if (o.lane != a.lane || o.inJ || o.s <= a.s) continue;
                if (o.s - a.s < 9.0f) a.speed = std::min(a.speed, o.speed);
            }
            a.s += a.speed * step;
            if (a.inJ) { a.edge = a.nextEdge; a.fwd = a.nextFwd; a.s = a.jExit; a.lane = a.jLaneB; a.laneF = (float)a.lane; a.inJ = false; a.nextEdge = -1; a.jpath.clear(); }
            else if (a.s >= len) {
                if (a.nextEdge == -1) chooseNext();
                const float over = a.s - len;
                if (a.nextEdge < 0) { a.fwd = !a.fwd; a.s = over; }
                else {
                    a.edge = a.nextEdge; a.fwd = a.nextFwd; a.s = over;
                    const int lanes2 = std::max(edges[(size_t)a.edge].lanes > 0 ? edges[(size_t)a.edge].lanes : params.lanes, 1);
                    a.lane = std::min(a.lane, std::max(edges[(size_t)a.edge].oneWay ? lanes2 : lanes2 / 2, 1) - 1);
                }
                a.nextEdge = -1; a.released = false; a.wait = 0.0f; a.jpath.clear();
            }
            Vector3 tp; Vector2 hd;
            LanePose(a.edge, a.fwd, a.s, a.laneF, true, tp, hd);
            a.pos = tp; a.yaw = atan2f(-hd.y, hd.x); a.placed = true;
            continue;
        }

        // Speed limit: the road's, and the planned turn (or roundabout) as it comes up.
        float v0 = EdgeSpeedLimit(a.edge) * a.speedFactor;
        if (a.nextEdge >= 0 && !a.jpath.empty() && (a.inJ || len - a.s < 25.0f)) v0 = std::min(v0, a.jVMax);
        if (a.nextEdge >= 0 && a.jpath.empty() && !a.inJ) PlanJunction(a);

        // Leader: nearest car ahead in the same lane (on this edge, or just beyond the junction).
        float gap = 1e9f, leaderV = a.speed;
        if (!a.inJ) {
            for (int j : onEdge[a.edge * 2 + (a.fwd ? 1 : 0)]) {
                if (j == i) continue;
                const Agent& o = agents[(size_t)j];
                if (o.lane != a.lane || o.inJ) continue;
                if (o.s <= a.s) continue;
                const float g = std::max(o.s - a.s - 0.5f * (a.length + o.length), 0.05f);   // bumper to bumper (never negative: an overlapped car still brakes hard)
                if (g < gap) { gap = g; leaderV = o.speed; }
            }
            if (a.nextEdge >= 0 && len - a.s < 40.0f) {
                for (int j : onEdge[a.nextEdge * 2 + (a.nextFwd ? 1 : 0)]) {
                    const Agent& o = agents[(size_t)j];
                    if (o.lane != a.lane) continue;
                    const float g = std::max((len - a.s) + o.s - 0.5f * (a.length + o.length), 0.05f);
                    if (g < gap) { gap = g; leaderV = o.speed; }
                }
            }
        }
        const float entryGap = (len - JA) - a.s;   // distance to the start of the junction
        auto nodeIt = nodeCars.find(toNode);
        if (!a.inJ) {
            // Cars already crossing the junction ahead of us in the same stream are leaders too.
            if (nodeIt != nodeCars.end())
                for (int j : nodeIt->second) {
                    const Agent& o = agents[(size_t)j];
                    if (!o.inJ || o.jFrom != a.edge || o.lane != a.lane) continue;
                    const float g = std::max(entryGap + o.jt * o.jlen - 0.5f * (a.length + o.length), 0.05f);
                    if (g < gap) { gap = g; leaderV = o.speed; }
                }
        } else {
            for (int j : nodeIt != nodeCars.end() ? nodeIt->second : std::vector<int>{}) {
                if (j == i) continue;
                const Agent& o = agents[(size_t)j];
                if (!o.inJ || o.jFrom != a.jFrom || o.nextEdge != a.nextEdge || o.jt <= a.jt) continue;
                const float g = std::max((o.jt - a.jt) * a.jlen - 0.5f * (a.length + o.length), 0.05f);
                if (g < gap) { gap = g; leaderV = o.speed; }
            }
            if (a.nextEdge >= 0)
                for (int j : onEdge[a.nextEdge * 2 + (a.nextFwd ? 1 : 0)]) {
                    const Agent& o = agents[(size_t)j];
                    if (o.lane != a.jLaneB || o.s < a.jExit - 1.0f) continue;
                    const float g = std::max((1.0f - a.jt) * a.jlen + (o.s - a.jExit) - 0.5f * (a.length + o.length), 0.05f);
                    if (g < gap) { gap = g; leaderV = o.speed; }
                }
        }
        a.stuck = a.speed < 0.3f ? a.stuck + step : 0.0f;
        if (a.stuck > 30.0f) {    // true gridlock (everyone waiting on everyone): remove the car and put it back on a free road
            for (int tries = 0; tries < 30; tries++) { SpawnAgent(a, true); if (!overlapsExisting(a)) break; }
            a.stuck = 0.0f;
            continue;
        }
        const bool desperate = a.stuck > 9.0f;     // waited ages: stop yielding so jams (and mutual blocking) resolve
        // Merging / crossing traffic inside the junction: brake for any car directly ahead (within a car-length of our heading),
        // whichever road it came from.
        if (a.inJ && nodeIt != nodeCars.end()) {
            const Vector2 f = { cosf(a.yaw), -sinf(a.yaw) };
            for (int j : nodeIt->second) {
                if (j == i) continue;
                const Agent& o = agents[(size_t)j];
                if (!o.inJ) continue;   // cars still queuing outside never hold up a car already crossing
                const Vector2 rel = { o.pos.x - a.pos.x, o.pos.z - a.pos.z };
                const float along = rel.x * f.x + rel.y * f.y, lat = fabsf(rel.x * f.y - rel.y * f.x);
                if (along <= 0.2f || along > 10.0f || lat > 2.0f) continue;
                const float g = std::max(along - 4.4f, 0.05f);
                if (g < gap) { gap = g; leaderV = o.speed; }
            }
        }
        // Do not block the junction: only enter if the road we are turning onto has room for us.
        if (!a.inJ && a.nextEdge >= 0 && !a.jpath.empty() && entryGap < 10.0f)
            for (int j : onEdge[a.nextEdge * 2 + (a.nextFwd ? 1 : 0)]) {
                const Agent& o = agents[(size_t)j];
                if (o.lane != a.jLaneB || o.inJ || o.s > a.jExit + 7.5f) continue;
                const float g = std::max(entryGap - 0.5f, 0.05f);
                if (g < gap) { gap = g; leaderV = 0.0f; }
                break;
            }
        // Right of way: do not enter the junction while a conflicting car is in it, or is going to get there first.
        if (!a.inJ && a.nextEdge >= 0 && !a.jpath.empty() && entryGap < 12.0f && nodeIt != nodeCars.end() && !desperate) {
            const JunctionKind jk0 = (JunctionKind)nodes[(size_t)toNode].jkind;
            const bool controlled = nodes[(size_t)toNode].junction && (jk0 == JunctionKind::TrafficLight || jk0 == JunctionKind::Stop);
            const float myArr = std::max(entryGap, 0.0f) / std::max(a.speed, 1.5f);
            bool blocked = false;
            for (int j : nodeIt->second) {
                if (j == i) continue;
                const Agent& o = agents[(size_t)j];
                if (o.jpath.empty()) continue;
                if (o.inJ) {
                    if (o.jFrom == a.edge && o.lane == a.lane) continue;               // same stream: just follow it
                    const size_t from = (size_t)std::max(0.0f, o.jt * (float)(o.jpath.size() - 1) - 1.0f);
                    if (PathsConflict(a.jpath, 0, o.jpath, from)) { blocked = true; break; }
                } else {
                    const float lenO = std::max(Vector2Distance(nodes[(size_t)edges[(size_t)o.edge].a].pos, nodes[(size_t)edges[(size_t)o.edge].b].pos), 1e-3f);
                    const float oEntry = (lenO - ArmClear(o.edge)) - o.s;
                    if (oEntry > 14.0f || oEntry < -0.5f) continue;
                    if (o.edge == a.edge && o.fwd == a.fwd) continue;                   // same road: the leader logic handles it
                    if (controlled && !o.released) continue;                            // held at its stop line: no claim yet
                    const float oArr = std::max(oEntry, 0.0f) / std::max(o.speed, 1.5f);
                    const bool wins = oArr < myArr || (oArr == myArr && j < i);
                    if (wins && PathsConflict(a.jpath, 0, o.jpath, 0)) { blocked = true; break; }
                }
            }
            if (blocked) { const float g = std::max(entryGap - 1.0f, 0.05f); if (g < gap) { gap = g; leaderV = 0.0f; } }
        }
        // Bus stop: a stationary obstacle at the stop; wait there, then head for the next stop of the line.
        if (a.bus >= 0 && !a.inJ && (size_t)a.bus < busLines.size() && !busLines[(size_t)a.bus].stops.empty()) {
            const BusLine& L = busLines[(size_t)a.bus];
            const int sidx = L.stops[(size_t)a.busTarget % L.stops.size()];
            const int key = a.edge * 2 + (a.fwd ? 1 : 0);
            if (a.busSkip >= 0 && key != a.busSkip) a.busSkip = -1;
            if ((size_t)sidx < busStops.size() && busStops[(size_t)sidx].edge == a.edge && busStops[(size_t)sidx].fwd == a.fwd && key != a.busSkip) {
                const BusStop& stp = busStops[(size_t)sidx];
                const float dist = (a.fwd ? stp.s : len - stp.s) - a.s;
                if (dist > -1.0f) {
                    if (dist < gap) { gap = std::max(dist, 0.0f); leaderV = 0.0f; }
                    if (dist < a.minGap + 2.5f && a.speed < 0.4f) {
                        a.dwell += step;
                        if (a.dwell >= 7.0f + Lcg01(a.rng) * 4.0f) {
                            a.dwell = 0.0f;
                            a.busTarget = (a.busTarget + 1) % (int)L.stops.size();
                            a.busSkip = key;
                            busStopsServedCount++;
                            a.nextEdge = -1; a.jpath.clear(); a.turn = 0;      // the turn planned for the old target is stale: re-plan toward the next stop
                        }
                    }
                }
            }
        }
        if (gap < 1e8f) statMinGap = std::min(statMinGap, gap);   // car-to-car gaps only
        // Junction control acts as a stationary obstacle at the stop line.
        const float stopAt = len - JA - 5.0f;   // stop line, before the crosswalk
        const JunctionKind jk = (JunctionKind)nodes[(size_t)toNode].jkind;
        if (!a.inJ && nodes[(size_t)toNode].junction && (jk == JunctionKind::TrafficLight || jk == JunctionKind::Stop) && !a.released) {
            const float distToLine = stopAt - a.s;
            if (distToLine > -1.5f) {
                bool go = true;
                if (jk == JunctionKind::TrafficLight) {
                    const int st = SignalState(toNode, a.edge);
                    // Amber: cars that cannot stop comfortably before the line carry on.
                    go = st == 2 || (st == 1 && distToLine < a.speed * a.speed / (2.0f * a.decel) + 1.0f);
                } else {
                    go = a.wait > 1.2f + Lcg01(a.rng) * 0.6f;
                }
                if (!go) {
                    if (distToLine < gap) { gap = std::max(distToLine, 0.0f); leaderV = 0.0f; }
                    if (distToLine < 1.2f && a.speed < 0.4f && jk == JunctionKind::Stop) a.wait += step;
                } else if (jk == JunctionKind::Stop || distToLine < 1.0f) a.released = true;
            }
        }

        // Intelligent Driver Model: smooth acceleration toward the desired speed, braking for the leader.
        const float v = a.speed;
        const float dv = v - leaderV;
        const float sStar = a.minGap + std::max(0.0f, v * a.headway + v * dv / (2.0f * sqrtf(a.accel * a.decel)));
        float acc = a.accel * (1.0f - powf(v / std::max(v0, 0.5f), 4.0f) - (gap < 1e8f ? (sStar / std::max(gap, 0.3f)) * (sStar / std::max(gap, 0.3f)) : 0.0f));
        acc = std::max(acc, -9.0f);
        a.speed = std::max(0.0f, v + acc * step);
        if (v0 < 3.0f && !a.inJ && a.speed < 0.2f && gap > 1e8f) a.speed = 0.2f;   // never dawdle to a halt in a corner

        if (a.inJ) {
            a.jt += a.speed * step / a.jlen;
            if (a.jt >= 1.0f) {
                a.edge = a.nextEdge; a.fwd = a.nextFwd; a.s = a.jExit; a.inJ = false; a.nextEdge = -1; a.turn = 0;
                a.released = false; a.wait = 0.0f;
                a.lane = a.jLaneB; a.laneF = (float)a.lane;
                a.jpath.clear();
            }
        } else {
            a.s += a.speed * step;
            if (a.nextEdge >= 0 && a.s >= len - JA && len - JA > 0.0f) {
                if (a.jpath.empty()) PlanJunction(a);
                if (!a.jpath.empty()) { a.inJ = true; a.jt = 0.0f; }
                else { a.edge = a.nextEdge; a.fwd = a.nextFwd; a.s = 0.0f; a.nextEdge = -1; }
            } else if (a.s >= len) {
                // Dead end (or no junction curve possible): turn around.
                if (a.nextEdge == -1) chooseNext();
                const float over = a.s - len;
                if (a.nextEdge < 0) { a.fwd = !a.fwd; a.s = over; }
                else { a.edge = a.nextEdge; a.fwd = a.nextFwd; a.s = over; }
                a.nextEdge = -1; a.released = false; a.wait = 0.0f; a.jpath.clear();
            }
        }

        // Pose.
        Vector3 tp; float ty;
        if (a.inJ && a.jpath.size() >= 2) {
            const float t = Clamp(a.jt, 0.0f, 1.0f);
            const float dist = t * a.jlen;
            size_t k = 0;
            while (k + 2 < a.jpath.size() && a.jcum[k + 1] < dist) k++;
            const float segLen = std::max(a.jcum[k + 1] - a.jcum[k], 1e-4f);
            const float f = Clamp((dist - a.jcum[k]) / segLen, 0.0f, 1.0f);
            const Vector2 q = Vector2Lerp(a.jpath[k], a.jpath[k + 1], f);
            tp = { q.x, a.jy0 + (a.jy2 - a.jy0) * t, q.y };
            // Heading from the path a car-length ahead and behind (a smooth tangent, not the current segment's).
            auto at = [&](float dd) {
                dd = Clamp(dd, 0.0f, a.jlen);
                size_t kk = 0;
                while (kk + 2 < a.jpath.size() && a.jcum[kk + 1] < dd) kk++;
                const float sl = std::max(a.jcum[kk + 1] - a.jcum[kk], 1e-4f);
                return Vector2Lerp(a.jpath[kk], a.jpath[kk + 1], Clamp((dd - a.jcum[kk]) / sl, 0.0f, 1.0f));
            };
            Vector2 dq = Vector2Subtract(at(dist + 1.6f), at(dist - 1.6f));
            if (dq.x == 0.0f && dq.y == 0.0f) dq = Vector2Subtract(a.jpath[k + 1], a.jpath[k]);
            ty = atan2f(-dq.y, dq.x);
        } else {
            Vector2 hd;
            LanePose(a.edge, a.fwd, a.s, a.laneF, true, tp, hd);
            ty = atan2f(-hd.y, hd.x);
        }
        if (!a.placed) { a.pos = tp; a.yaw = ty; a.placed = true; }
        else {
            // Light smoothing only (the path itself is already smooth); hides lane-change/edge hand-over steps.
            const float k = 1.0f - expf(-step * 30.0f);
            a.pos = Vector3Lerp(a.pos, tp, k);
            float dy = ty - a.yaw;
            while (dy > PI) dy -= 2.0f * PI;
            while (dy < -PI) dy += 2.0f * PI;
            // Turn rate limit (steering): a car swings round at most ~1.6 rad/s, so heading never snaps.
            const float kY = 1.0f - expf(-step * 9.0f);
            const float maxTurn = 1.6f * step + 0.002f;
            a.yaw += Clamp(dy * kY, -maxTurn, maxTurn);
        }
    }
    {
        TrafficStats st;
        float sum = 0.0f;
        for (Agent& a : agents) {
            if (!a.car) {
                st.peds++;
                if (a.placed && (a.lastPos.x != 0.0f || a.lastPos.z != 0.0f)) { const float v = Vector3Distance(a.pos, a.lastPos) / std::max(dt, 1e-4f); if (v < 1000.0f) st.maxWalkerSpeed = std::max(st.maxWalkerSpeed, v); }
                a.lastPos = a.pos;
                continue;
            }
            if (a.bus >= 0) st.buses++;
            else st.cars++;
            (a.far ? st.farCars : st.nearCars)++;
            sum += a.speed;
            if (a.speed < 0.2f) st.stopped++;
        }
        // Pose jumps: a detailed car that moved more than 3 m in one step (teleport). Cumulative.
        st.jumps = trafficStats.jumps;
        for (Agent& a : agents) {
            if (!a.car || !a.placed) continue;
            if (!a.far && a.nearTime >= 1.0f && (a.lastPos.x != 0.0f || a.lastPos.z != 0.0f) && Vector3Distance(a.pos, a.lastPos) > 3.0f) st.jumps++;
            a.lastPos = a.pos;
        }
        // Cars occupying (almost) the same spot, i.e. a collision that was not avoided (cumulative frame count).
        st.overlaps = trafficStats.overlaps;
        for (size_t i = 0; simTime > 8.0f && i < agents.size(); i++) {   // ignore the first seconds (cars start anywhere)
            if (!agents[i].car || agents[i].nearTime < 2.0f) continue;
            for (size_t j = i + 1; j < agents.size(); j++)
                if (agents[j].car && agents[j].nearTime >= 2.0f && Vector3DistanceSqr(agents[i].pos, agents[j].pos) < 2.2f * 2.2f) st.overlaps++;
        }
        st.maxWalkerSpeed = std::max(st.maxWalkerSpeed, trafficStats.maxWalkerSpeed * (1.0f - 0.3f * dt));   // slowly decaying peak
        st.trips = tripsCompleted;
        st.busStopsServed = busStopsServedCount;
        st.avgSpeed = st.cars ? sum / (float)st.cars : 0.0f;
        st.minGap = statMinGap < 1e8f ? statMinGap : -1.0f;
        st.stepMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - stepT0).count();
        trafficStats = st;
    }
}

// CPU-side reset only (safe on a worker thread: touches no GPU state).
void City::ResetTileCPU(Tile& t) {
    for (Mesh& m : t.raw) {
        std::free(m.vertices); std::free(m.texcoords); std::free(m.normals);
        std::free(m.colors); std::free(m.indices);
    }
    t.raw.clear();
    for (auto& v : t.inst) v.clear();
    t.coll = TileCollision{};
    t.hasRoad = false;
    t.hasBldg = false;
    t.roadMin = t.roadMax = t.bldgMin = t.bldgMax = Vector3{ 0.0f, 0.0f, 0.0f };
}

// Frees GPU resources too (main thread only).
void City::DestroyTile(Tile& t) {
    DestroyTilePhysics(t);
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
    blockKinds.clear();
    districts.clear();
    blockDistricts.clear();
    busStops.clear();
    busLines.clear();
    placed.clear();
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
    graphVersion++;
    ResolveBusStops();
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
    job->work->blockKinds = blockKinds;
    job->work->districts = districts;
    job->work->busStops = busStops;
    job->work->blockDistricts = blockDistricts;
    job->work->placed = placed;
    job->work->collisionEnabled = collisionEnabled;
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
    for (int i = 0; i < (int)nodes.size(); i++) { nodes[(size_t)i].junction = JunctionFlagFor(i); }
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

// World-space corners of a placed footprint grown by `grow` on every side.
std::array<Vector2, 4> PlacedCorners(const PlacedBuilding& pb, float grow) {
    const float hx = pb.sizeX * 0.5f + grow, hz = pb.sizeZ * 0.5f + grow;
    const float c = cosf(pb.angleY), s = sinf(pb.angleY);
    const Vector2 loc[4] = { { -hx, -hz }, { hx, -hz }, { hx, hz }, { -hx, hz } };
    std::array<Vector2, 4> out;
    for (int i = 0; i < 4; i++)
        out[(size_t)i] = { pb.center.x + loc[i].x * c + loc[i].y * s, pb.center.y - loc[i].x * s + loc[i].y * c };
    return out;
}

// Separating-axis test for two convex quads.
bool QuadsOverlap(const Vector2* a, const Vector2* b) {
    for (const Vector2* q : { a, b }) {
        for (int i = 0; i < 4; i++) {
            const Vector2 e = { q[(i + 1) % 4].x - q[i].x, q[(i + 1) % 4].y - q[i].y };
            const Vector2 n = { -e.y, e.x };
            float a0 = 1e30f, a1 = -1e30f, b0 = 1e30f, b1 = -1e30f;
            for (int k = 0; k < 4; k++) {
                const float da = a[k].x * n.x + a[k].y * n.y, db = b[k].x * n.x + b[k].y * n.y;
                a0 = fminf(a0, da); a1 = fmaxf(a1, da); b0 = fminf(b0, db); b1 = fmaxf(b1, db);
            }
            if (a1 < b0 || b1 < a0) return false;
        }
    }
    return true;
}
} // namespace

void City::LayoutBuildings() {
    for (auto& block : blocks) LayoutBlock(block);
}

// Lays out one block (park or parcelled buildings). A pure function of the
// block's node polygon, the params and the dead-end spurs, which is what lets
// the incremental rebuild redo just the blocks touched by an edit.
void City::LayoutBlockProcedural(Block& block) {
    const CityParams& p = params;
    const float roadHalf = BlockRoadHalf(block);
    const float inset = roadHalf + p.buildingGap * 0.5f;
    const float roadW = roadHalf * 2.0f;
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
        float distMul = 1.0f; int bstyle = p.style;
        {
            int ds = -1;
            DistrictAt(BlockCenter(block), block.id, distMul, ds);
            if (ds >= 0) bstyle = ds;
        }

        std::vector<Vector2> poly;
        poly.reserve(block.nodes.size());
        for (int idx : block.nodes) poly.push_back(nodes[idx].pos);

        // A block becomes a park when it is clearly oversized relative to the
        // road grid (~2.2x a regular cell) ??? the parkThreshold slider is the
        // absolute floor users can raise to force more parks.
        const float regularCellArea = p.cellSize * p.cellSize;
        const auto kindIt = blockKinds.find(block.id);
        const BlockKind kind = kindIt == blockKinds.end() ? BlockKind::Auto : kindIt->second;
        if (kind == BlockKind::Concrete) return; // bare pad: no grass, no buildings
        const bool isPark = kind == BlockKind::Park ||
            (kind == BlockKind::Auto &&
             block.area >= std::max(p.parkThreshold, regularCellArea * 2.2f));

        const auto makePark = [&]() {
            block.park = true;
            if (!citygeom::InsetPolygon(poly, std::min(p.parkInset, roadW * 0.4f), block.parkPoly)) {
                block.parkPoly = poly;
            }
        };
        if (isPark) {
            makePark();
            return;
        }

        if (!citygeom::InsetPolygon(poly, inset, block.inset)) {
            // Too small for building setbacks (e.g. a sliver left by a tight curve):
            // grass it over instead of leaving bare concrete, unless painted otherwise.
            if (kind == BlockKind::Auto) makePark();
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

        // User-placed footprints near this block: parcels must clear them.
        std::vector<std::array<Vector2, 4>> obst;
        if (!placed.empty()) {
            float bx0 = 1e9f, bz0 = 1e9f, bx1 = -1e9f, bz1 = -1e9f;
            for (const auto& v : poly) {
                bx0 = fminf(bx0, v.x); bx1 = fmaxf(bx1, v.x);
                bz0 = fminf(bz0, v.y); bz1 = fmaxf(bz1, v.y);
            }
            for (const PlacedBuilding& pb : placed) {
                const auto q = PlacedCorners(pb, p.buildingGap * 0.5f);
                float x0 = 1e9f, z0 = 1e9f, x1 = -1e9f, z1 = -1e9f;
                for (const auto& v : q) { x0 = fminf(x0, v.x); x1 = fmaxf(x1, v.x); z0 = fminf(z0, v.y); z1 = fmaxf(z1, v.y); }
                if (x1 < bx0 || x0 > bx1 || z1 < bz0 || z0 > bz1) continue;
                obst.push_back(q);
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
                float bx0 = 1e9f, bz0 = 1e9f, bx1 = -1e9f, bz1 = -1e9f;
                for (const auto& v : poly) { bx0 = fminf(bx0, v.x); bx1 = fmaxf(bx1, v.x); bz0 = fminf(bz0, v.y); bz1 = fmaxf(bz1, v.y); }
                std::vector<std::pair<Vector2, float>> rbCircles;   // roundabouts near this block: parcels keep clear of them
                for (int ni2 = 0; ni2 < (int)nodes.size(); ni2++) {
                    const RoadNode& rn = nodes[(size_t)ni2];
                    if (!rn.junction || rn.jkind != (int)JunctionKind::Roundabout) continue;
                    const float rr = RoundaboutOuterRadius(ni2, false) + p.buildingGap;
                    if (rn.pos.x < bx0 - rr || rn.pos.x > bx1 + rr || rn.pos.y < bz0 - rr || rn.pos.y > bz1 + rr) continue;
                    rbCircles.push_back({ rn.pos, rr });
                }
                const auto cornersIn = [&](float cu, float cv, float su, float sv) {
                    const float hx = su * 0.5f, hz = sv * 0.5f;
                    Vector2 cs[4] = {
                        { cu - hx, cv - hz }, { cu + hx, cv - hz },
                        { cu + hx, cv + hz }, { cu - hx, cv + hz },
                    };
                    for (const auto& cp : cs) {
                        if (!citygeom::PointInPolygon(cp, linset)) return false;
                    }
                    if (!rbCircles.empty()) {
                        const Vector2 w[4] = { toWorld(cs[0]), toWorld(cs[1]), toWorld(cs[2]), toWorld(cs[3]) };
                        const Vector2 mid = { (w[0].x + w[2].x) * 0.5f, (w[0].y + w[2].y) * 0.5f };
                        for (const auto& rc : rbCircles) {
                            if (Vector2Distance(mid, rc.first) < rc.second + std::max(su, sv) * 0.75f) return false;   // clear of the ring
                            for (int q = 0; q < 4; q++) if (Vector2Distance(w[q], rc.first) < rc.second) return false;
                        }
                    }
                    if (!obst.empty()) {
                        const Vector2 w[4] = { toWorld(cs[0]), toWorld(cs[1]), toWorld(cs[2]), toWorld(cs[3]) };
                        for (const auto& o : obst) if (QuadsOverlap(w, o.data())) return false;
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
                // Style: suburbs are low, industrial blocks squat, brick mid-rise.
                static const float kStyleHeight[4] = { 1.0f, 0.75f, 0.55f, 0.28f };
                height *= kStyleHeight[std::clamp(bstyle, 0, 3)] * distMul;
                height = Clamp(height, bstyle == 3 ? 4.5f : 1.0f, 220.0f);
                // Whole storeys: some buildings are much shorter than the rest, then the height snaps to the floor count.
                int floors = std::max(1, (int)lroundf(height / kFloorHeight));
                {
                    const uint32_t hs = CoordHash(i + 91, j + 17, p.seed ^ (int)blockMix ^ 0x5407);
                    if ((float)(hs & 0xFFFFu) / 65535.0f < p.shortChance) floors = 1 + (int)((hs >> 16) % 2u);
                }
                height = (float)floors * kFloorHeight;
                // Footprint variety: buildings do not always fill their whole parcel.
                {
                    const uint32_t hf = CoordHash(i + 5, j + 211, p.seed ^ (int)blockMix ^ 0x70F1);
                    sx *= 1.0f - p.footprintVariety * ((float)(hf & 0xFFu) / 255.0f);
                    sz *= 1.0f - p.footprintVariety * ((float)((hf >> 8) & 0xFFu) / 255.0f);
                }

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
                // Shape variety: gabled houses and stepped towers on parcels that stayed plain boxes.
                if (shape == kBuildingBox) {
                    const float roll = (float)((h >> 8) & 0xFFFFu) / 65535.0f;
                    const float want = p.shapeVariety * (bstyle == 3 ? 1.6f : 1.0f);
                    if (roll < want) {
                        const uint32_t pick = (h >> 4) % 3u;
                        if (bstyle == 3) shape = kBuildingGable;
                        else if (bstyle == 2) shape = kBuildingShed;
                        else if (floors >= 5 && pick == 0u) shape = kBuildingTower;
                        else shape = pick == 1u ? kBuildingShed : kBuildingGable;
                    }
                }

                Building b;
                b.size = { sx, height, sz };
                b.floors = floors;
                b.center = { wc.x, height * 0.5f + kRoadElevation + 0.06f, wc.y }; // sit on the pad surface
                b.angleY = theta + rotDelta;
                b.shape = shape;
                b.colorBucket = (int)(h % (uint32_t)kBuildingColorBuckets);
                b.style = bstyle;
                block.buildings.push_back(b);
            }
        }
        // Nothing fit (tiny/thin block): grass it rather than leave an empty pad.
        if (block.buildings.empty() && kind == BlockKind::Auto) {
            block.inset.clear();
            makePark();
            return;
        }
    }
    ApplyBuildingOverrides(block);
}

// ---------------------------------------------------------------------------
// Block surface: the pad height field. A block whose road nodes sit at different heights
// gets a boundary ring that follows the roads' height profiles, a flat apron out to the
// edge of the road slab (so the pad never rises over the asphalt), and an inner polygon
// that slopes between those boundary heights.
// ---------------------------------------------------------------------------
struct City::BlockSurface {
    bool flat = true;           // all boundary nodes at one height: the pad is a flat plane
    float flatY = 0.0f;
    std::vector<Vector3> B;     // block boundary (subdivided along sloped roads), with heights
    std::vector<char> corner;   // per B vertex: 1 = an original block node (0 = subdivision point)
    std::vector<int> bEdge;     // per B vertex: index of the block edge (node i -> i+1) it lies on
    std::vector<Vector3> R;     // inner ring, one per B vertex (valid when ringOk)
    bool ringOk = false;
    std::vector<int> tris;      // triangulation of R (ringOk) or B

    float HeightAt(float x, float z) const {
        if (flat) return flatY;
        const std::vector<Vector3>& V = ringOk ? R : B;
        for (size_t t = 0; t + 2 < tris.size(); t += 3) {
            const Vector3 &a = V[(size_t)tris[t]], &b = V[(size_t)tris[t + 1]], &c = V[(size_t)tris[t + 2]];
            const float d = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
            if (fabsf(d) < 1e-9f) continue;
            const float l1 = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / d;
            const float l2 = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / d;
            const float l3 = 1.0f - l1 - l2;
            if (l1 >= -1e-4f && l2 >= -1e-4f && l3 >= -1e-4f) return l1 * a.y + l2 * b.y + l3 * c.y;
        }
        // Outside the inner polygon (the road-side apron): height of the nearest boundary point.
        float best = 1e30f, y = B.empty() ? 0.0f : B[0].y;
        for (size_t i = 0; i < B.size(); i++) {
            const Vector3 &p = B[i], &q = B[(i + 1) % B.size()];
            const float dx = q.x - p.x, dz = q.z - p.z, l2 = dx * dx + dz * dz;
            const float t = l2 > 1e-9f ? Clamp(((x - p.x) * dx + (z - p.z) * dz) / l2, 0.0f, 1.0f) : 0.0f;
            const float ex = p.x + dx * t - x, ez = p.z + dz * t - z, d2 = ex * ex + ez * ez;
            if (d2 < best) { best = d2; y = p.y + (q.y - p.y) * t; }
        }
        return y;
    }
};

void City::BuildBlockSurface(const Block& block, BlockSurface& out) const {
    out = BlockSurface{};
    const int n = (int)block.nodes.size();
    if (n < 3) return;
    const float h0 = nodes[(size_t)block.nodes[0]].h;
    out.flatY = h0;
    bool flat = true;
    for (int idx : block.nodes) if (fabsf(nodes[(size_t)idx].h - h0) > 1e-4f) flat = false;
    if (flat) return;
    out.flat = false;

    for (int i = 0; i < n; i++) {
        const int ni = block.nodes[(size_t)i], nj = block.nodes[(size_t)((i + 1) % n)];
        const int ei = EdgeBetween(ni, nj);
        const Vector2 P = nodes[(size_t)ni].pos, Q = nodes[(size_t)nj].pos;
        // Boundary points at the edge's shared stations (oriented ni -> nj), heights from its profile.
        std::vector<float> fr;
        if (ei >= 0) {
            fr = EdgeStations(ei);
            if (edges[(size_t)ei].a != ni) { std::reverse(fr.begin(), fr.end()); for (float& v : fr) v = 1.0f - v; }
        } else {
            fr = { 0.0f, 1.0f };
        }
        for (size_t k = 0; k + 1 < fr.size(); k++) {
            const float f = fr[k];
            float y;
            if (ei >= 0) y = EdgeProfileY(ei, edges[(size_t)ei].a == ni ? f : 1.0f - f);
            else y = nodes[(size_t)ni].h + (nodes[(size_t)nj].h - nodes[(size_t)ni].h) * f;
            out.B.push_back({ P.x + (Q.x - P.x) * f, y, P.y + (Q.y - P.y) * f });
            out.corner.push_back(k == 0 ? 1 : 0);
            out.bEdge.push_back(i);
        }
    }
    // Inner ring. It is built from the corner polygon (the block's own nodes), where InsetPolygon is
    // reliable, and the subdivision points are placed on it by projecting their perpendicular
    // offsets onto the ring edge (clamped, so points near a corner collapse onto the mitre corner).
    // The apron reaches past the slab edge by the corner fillet's overshoot: the rounded junction
    // plate is flat at the node height and extends a little beyond the slab corner, so the sloping
    // inner surface must start beyond it or the plate would poke through.
    const float apron = BlockRoadHalf(block) + std::max(params.cornerRadius, 0.0f) * 0.5f + 0.5f;
    {
        std::vector<Vector2> coarse, ringC;
        for (int idx : block.nodes) coarse.push_back(nodes[(size_t)idx].pos);
        if (citygeom::InsetPolygon(coarse, apron, ringC) && ringC.size() == coarse.size()) {
            const size_t nc = coarse.size();
            // ringC[i] is the mitre corner of source edges i and i+1, i.e. of vertex i+1.
            auto rc = [&](size_t v) { return ringC[(v + nc - 1) % nc]; };
            out.R.resize(out.B.size());
            std::vector<size_t> cornerAt(nc, 0);
            for (size_t j = 0; j < out.B.size(); j++) if (out.corner[j]) cornerAt[(size_t)out.bEdge[j]] = j;
            for (size_t j = 0; j < out.B.size(); j++) {
                const size_t v = (size_t)out.bEdge[j];
                Vector2 r;
                float ry = out.B[j].y;
                if (out.corner[j]) {
                    r = rc(v);
                } else {
                    const Vector2 P = coarse[v], Q = coarse[(v + 1) % nc];
                    Vector2 dir = Vector2Subtract(Q, P);
                    const float dl = Vector2Length(dir);
                    dir = dl > 1e-6f ? Vector2Scale(dir, 1.0f / dl) : Vector2{ 1.0f, 0.0f };
                    const Vector2 nrm = { -dir.y, dir.x };
                    const Vector2 q = Vector2Add({ out.B[j].x, out.B[j].z }, Vector2Scale(nrm, apron));
                    const Vector2 a0 = rc(v), a1 = rc((v + 1) % nc), ab = Vector2Subtract(a1, a0);
                    const float l2 = ab.x * ab.x + ab.y * ab.y;
                    const float t = l2 > 1e-9f ? Clamp(Vector2DotProduct(Vector2Subtract(q, a0), ab) / l2, 0.0f, 1.0f) : 0.0f;
                    r = Vector2Add(a0, Vector2Scale(ab, t));
                    (void)cornerAt;
                }
                // An acute or reflex corner of an irregular block gives a mitre point far outside the block, which
                // showed as a long thin spike across the road: keep every ring point near its boundary point.
                {
                    Vector2 d = Vector2Subtract(r, { out.B[j].x, out.B[j].z });
                    const float lim = apron * 1.8f, dl = Vector2Length(d);
                    if (dl > lim) r = Vector2Add({ out.B[j].x, out.B[j].z }, Vector2Scale(d, lim / dl));
                    // A ring point that fell outside the block (the inset of an acute or reflex corner can fold over)
                    // would stretch the apron strip across the road: collapse it onto its boundary point instead.
                    if (!citygeom::PointInPolygon(r, coarse)) r = { out.B[j].x, out.B[j].z };
                }
                out.R[j] = { r.x, ry, r.y };
            }
            out.ringOk = true;
        }
    }

    // Triangulate the corner polygon only: runs of collinear subdivision points make ear clipping
    // drop or sliver the ear at a corner. The subdivision points are then re-attached by fanning
    // each boundary triangle edge that had some, so the surface has no T-junction cracks.
    const std::vector<Vector3>& V = out.ringOk ? out.R : out.B;
    const int m = (int)V.size();
    std::vector<int> cIdx;
    for (int i = 0; i < m; i++) if (out.corner[(size_t)i]) cIdx.push_back(i);
    if (cIdx.size() < 3) return;
    std::vector<Vector2> cxz;
    for (int ci : cIdx) cxz.push_back({ V[(size_t)ci].x, V[(size_t)ci].z });
    std::vector<int> ctris;
    citygeom::TriangulateSimple(cxz, ctris);
    std::vector<int> nextCorner((size_t)m, -1);
    for (size_t k = 0; k < cIdx.size(); k++) nextCorner[(size_t)cIdx[k]] = cIdx[(k + 1) % cIdx.size()];
    std::vector<std::array<int, 3>> work;
    for (size_t t = 0; t + 2 < ctris.size(); t += 3)
        work.push_back({ cIdx[(size_t)ctris[t]], cIdx[(size_t)ctris[t + 1]], cIdx[(size_t)ctris[t + 2]] });
    while (!work.empty()) {
        const std::array<int, 3> tr = work.back();
        work.pop_back();
        bool split = false;
        for (int e = 0; e < 3 && !split; e++) {
            const int x = tr[(size_t)e], y = tr[(size_t)((e + 1) % 3)], r = tr[(size_t)((e + 2) % 3)];
            if (nextCorner[(size_t)x] == y && (x + 1) % m != y) {
                for (int cur = x; cur != y; cur = (cur + 1) % m) work.push_back({ cur, (cur + 1) % m, r });
                split = true;
            }
        }
        if (!split) { out.tris.push_back(tr[0]); out.tris.push_back(tr[1]); out.tris.push_back(tr[2]); }
    }
}

// Sits the block's buildings on the sloped pad: the box starts at the lowest ground under its
// footprint and is made taller by the drop, so it never floats above or sinks into the slope.
void City::FitBuildingsToSurface(Block& block) const {
    BlockSurface sf;
    BuildBlockSurface(block, sf);
    for (Building& b : block.buildings) {
        if (b.placedIndex >= 0 && (size_t)b.placedIndex < placed.size() && placed[(size_t)b.placedIndex].free) continue;   // set from the road/ground directly
        PlacedBuilding tmp;
        tmp.center = { b.center.x, b.center.z };
        tmp.sizeX = b.size.x; tmp.sizeZ = b.size.z; tmp.angleY = b.angleY;
        const auto q = PlacedCorners(tmp, 0.0f);
        float lo = sf.HeightAt(b.center.x, b.center.z), hi = lo;
        for (const Vector2& c : q) {
            const float y = sf.HeightAt(c.x, c.y);
            lo = std::min(lo, y); hi = std::max(hi, y);
        }
        b.size.y += hi - lo;
        b.center.y = lo + kRoadElevation + 0.06f + b.size.y * 0.5f;
    }
}

// Ground height under a free-placed building: the road surface when it is over one, else 0.
float City::FreeGroundY(const Vector2& p) const {
    float best = 1e30f, y = 0.0f;
    for (int i = 0; i < (int)edges.size(); i++) {
        const Vector2 P = nodes[(size_t)edges[(size_t)i].a].pos, Q = nodes[(size_t)edges[(size_t)i].b].pos;
        const Vector2 d = Vector2Subtract(Q, P);
        const float l2 = d.x * d.x + d.y * d.y;
        if (l2 < 1e-8f) continue;
        const float t = Clamp(Vector2DotProduct(Vector2Subtract(p, P), d) / l2, 0.0f, 1.0f);
        const float dist = Vector2Distance(Vector2Add(P, Vector2Scale(d, t)), p);
        if (dist < best) { best = dist; y = EdgeSurfaceY(i, t); }
    }
    return best <= EdgeSlabHalf(0) + 2.0f ? y : 0.0f;
}

// One block: procedural park/parcels, then the user-placed buildings whose
// centre lies in it.
void City::LayoutBlock(Block& block) {
    LayoutBlockProcedural(block);
    bool elevated = false;
    for (int idx : block.nodes) if (nodes[(size_t)idx].h != 0.0f) { elevated = true; break; }
    if (!placed.empty()) {
    std::vector<Vector2> poly;
    poly.reserve(block.nodes.size());
    for (int idx : block.nodes) poly.push_back(nodes[(size_t)idx].pos);
    for (int i = 0; i < (int)placed.size(); i++) {
        const PlacedBuilding& pb = placed[(size_t)i];
        if (pb.free) {
            // NoCollision buildings may lie off every block: the nearest block owns (draws) them.
            int owner = -1; float bd = 1e30f;
            for (int k = 0; k < (int)blocks.size(); k++) {
                const float d = Vector2Distance(BlockCenter(blocks[(size_t)k]), pb.center);
                if (d < bd) { bd = d; owner = k; }
            }
            if (owner < 0 || blocks[(size_t)owner].id != block.id) continue;
        } else if (!citygeom::PointInPolygon(pb.center, poly)) continue;
        Building b;
        b.floors = std::max(1, (int)lroundf(pb.height / kFloorHeight));
        b.size = { pb.sizeX, (float)b.floors * kFloorHeight, pb.sizeZ };
        b.center = { pb.center.x, b.size.y * 0.5f + kRoadElevation + 0.06f, pb.center.y };
        if (pb.free) b.center.y += FreeGroundY(pb.center);
        b.angleY = pb.angleY;
        b.shape = (pb.shape >= 0 && pb.shape < kBuildingShapes && pb.shape != kBuildingWedge && pb.shape != kBuildingSlant) ? pb.shape : kBuildingBox;
        b.colorBucket = ((pb.colorBucket % kBuildingColorBuckets) + kBuildingColorBuckets) % kBuildingColorBuckets;
        b.placedIndex = i;
        b.style = params.style;
        block.buildings.push_back(b);
    }
    }
    if (elevated) FitBuildingsToSurface(block);
}

int City::AddPlacedBuilding(const PlacedBuilding& pb) {
    placed.push_back(pb);
    RebuildAll();
    return (int)placed.size() - 1;
}

void City::UpdatePlacedBuilding(int index, const PlacedBuilding& pb) {
    if (index < 0 || (size_t)index >= placed.size()) return;
    placed[(size_t)index] = pb;
    RebuildAll();
}

void City::RemovePlacedBuilding(int index) {
    if (index < 0 || (size_t)index >= placed.size()) return;
    placed.erase(placed.begin() + index);
    RebuildAll();
}

bool City::CanPlaceBuilding(const PlacedBuilding& pb, int ignoreIndex) const {
    const int bi = PickBlockAt(pb.center);
    if (bi < 0) return false;
    std::vector<Vector2> poly, safe;
    for (int idx : blocks[(size_t)bi].nodes) poly.push_back(nodes[(size_t)idx].pos);
    // Keep clear of the asphalt: corners must be inside the block shrunk by half a road.
    if (!citygeom::InsetPolygon(poly, BlockRoadHalf(blocks[(size_t)bi]), safe)) return false;
    const auto q = PlacedCorners(pb, 0.0f);
    for (const Vector2& c : q) if (!citygeom::PointInPolygon(c, safe) || TouchesRoundabout(c, params.buildingGap)) return false;
    for (int i = 0; i < (int)placed.size(); i++) {
        if (i == ignoreIndex) continue;
        const auto o = PlacedCorners(placed[(size_t)i], 0.0f);
        if (QuadsOverlap(q.data(), o.data())) return false;
    }
    return true;
}

bool City::SnapBuildingToRoad(const Vector2& cursor, PlacedBuilding& pb) const {
    const float reach = params.RoadWidth() * 0.5f + std::max(pb.sizeX, pb.sizeZ) * 1.5f + 8.0f;
    int best = -1; float bd = reach; Vector2 bq{}, bdir{};
    for (int i = 0; i < (int)edges.size(); i++) {
        const Vector2 P = nodes[(size_t)edges[(size_t)i].a].pos, Q = nodes[(size_t)edges[(size_t)i].b].pos;
        const Vector2 s = Vector2Subtract(Q, P);
        const float l2 = s.x * s.x + s.y * s.y;
        if (l2 < 1e-8f) continue;
        const float t = Clamp(Vector2DotProduct(Vector2Subtract(cursor, P), s) / l2, 0.0f, 1.0f);
        const Vector2 q = Vector2Add(P, Vector2Scale(s, t));
        const float d = Vector2Distance(q, cursor);
        if (d < bd) { bd = d; best = i; bq = q; bdir = Vector2Scale(s, 1.0f / sqrtf(l2)); }
    }
    if (best < 0) return false;
    Vector2 n = { -bdir.y, bdir.x };
    if (Vector2DotProduct(n, Vector2Subtract(cursor, bq)) < 0.0f) n = Vector2Scale(n, -1.0f);
    pb.center = Vector2Add(bq, Vector2Scale(n, EdgeSlabHalf(best) + params.buildingGap * 0.5f + pb.sizeZ * 0.5f));
    pb.angleY = atan2f(-bdir.y, bdir.x); // local x runs along the street
    return true;
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

        // `asphalt` selects the band: the asphalt ribbon, or the full slab with sidewalks. Each arm
        // uses its own road's width, so mixed widths meet at one junction.
        auto buildJunction = [&](int ni, bool asphalt) {
            JunctionGeom g;
            if (!nodes[ni].junction) return g;
            const Vector2 c = nodes[ni].pos;
            struct Arm { int ei; Vector2 u; float ang; float len; float w; float asp; };
            std::vector<Arm> arms;
            for (int ei : incident[ni]) {
                const RoadEdge& e = edges[ei];
                const int other = (e.a == ni) ? e.b : e.a;
                Vector2 dv = Vector2Subtract(nodes[other].pos, c);
                const float len = Vector2Length(dv);
                if (len < 1e-4f) continue;
                dv = Vector2Scale(dv, 1.0f / len);
                arms.push_back({ ei, dv, atan2f(dv.y, dv.x), len, asphalt ? EdgeAsphaltHalf(ei) : EdgeSlabHalf(ei), EdgeAsphaltHalf(ei) });
            }
            const int k = (int)arms.size();
            if (k < 2) return g;
            std::sort(arms.begin(), arms.end(), [](const Arm& x, const Arm& y) { return x.ang < y.ang; });

            g.edge.resize(k);
            g.left.resize(k);
            g.right.resize(k);
            if (nodes[(size_t)ni].jkind == (int)JunctionKind::Roundabout) {
                // Roundabout: a circular roadway (and sidewalk ring) instead of the corner-filleted plate; each arm's
                // strip ends where its sides meet the circle.
                const float R = RoundaboutOuterRadius(ni, asphalt);
                for (int i = 0; i < k; i++) {
                    const Arm& A = arms[i];
                    const float d = std::min(sqrtf(std::max(R * R - A.w * A.w, 0.25f)), A.len * 0.48f);
                    const Vector2 nA = { -A.u.y, A.u.x };
                    const Vector2 tip = Vector2Add(c, Vector2Scale(A.u, d));
                    g.edge[i] = A.ei;
                    g.left[i] = Vector2Add(tip, Vector2Scale(nA, A.w));
                    g.right[i] = Vector2Subtract(tip, Vector2Scale(nA, A.w));
                }
                for (int s = 0; s < 48; s++) {
                    const float ang = 2.0f * PI * (float)s / 48.0f;
                    g.poly.push_back({ c.x + cosf(ang) * R, c.y + sinf(ang) * R });
                }
                g.valid = true;
                return g;
            }
            std::vector<std::vector<Vector2>> arcs(k);

            for (int i = 0; i < k; i++) {
                const Arm& A = arms[i];
                const Arm& B = arms[(i + 1) % k];
                g.edge[i] = A.ei;
                const Vector2 nA = { -A.u.y, A.u.x }, nB = { -B.u.y, B.u.x };
                const Vector2 oA = Vector2Add(c, Vector2Scale(nA, A.w));
                const Vector2 oB = Vector2Subtract(c, Vector2Scale(nB, B.w));
                const float D = A.u.x * B.u.y - A.u.y * B.u.x; // > 0: concave gap (< 180 deg)
                Vector2 TA = oA, TB = oB;

                if (fabsf(D) > 1e-3f) {
                    const float phi = acosf(Clamp(Vector2DotProduct(A.u, B.u), -1.0f, 1.0f));
                    const float th = std::max(tanf(phi * 0.5f), 1e-4f);
                    const float sh = std::max(sinf(phi * 0.5f), 1e-4f);
                    const bool concave = D > 0.0f;
                    const float dmax = 0.5f * std::min(A.len, B.len);

                    // Corner param along the arms for band half-width ww.
                    auto cornerA = [&](float wa, float wb) {
                        const Vector2 pa = Vector2Add(c, Vector2Scale(nA, wa));
                        const Vector2 pb = Vector2Subtract(c, Vector2Scale(nB, wb));
                        const Vector2 dw = Vector2Subtract(pb, pa);
                        return (dw.x * B.u.y - dw.y * B.u.x) / D;
                    };

                    // Radius is decided on the asphalt band so slab and asphalt stay concentric.
                    float rAsp = concave ? cornerR : (cornerR > 0.0f ? cornerR + A.asp + B.asp : 0.0f);
                    if (rAsp > 0.0f) rAsp = std::min(rAsp, std::max(dmax - cornerA(A.asp, B.asp), 0.0f) * th);

                    const float a = cornerA(A.w, B.w);
                    if (a > dmax) {
                        // Sharp corner lies beyond the arm: cut straight across.
                        TA = Vector2Add(oA, Vector2Scale(A.u, dmax));
                        TB = Vector2Add(oB, Vector2Scale(B.u, dmax));
                    } else {
                        const Vector2 C = Vector2Add(oA, Vector2Scale(A.u, a));
                        float rr = 0.0f;
                        if (rAsp > 0.0f) {
                            const float delta = 0.5f * ((A.w - A.asp) + (B.w - B.asp));
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
        auto J = [&](std::unordered_map<int, JunctionGeom>& cache, int ni, bool asphalt) -> const JunctionGeom& {
            auto it = cache.find(ni);
            if (it == cache.end()) it = cache.emplace(ni, buildJunction(ni, asphalt)).first;
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
            const float elen = Vector2Distance(A, B);
            const Vector2 nL = { -d.y, d.x };                 // left of the edge direction
            const float bankTan = tanf(e.bank * DEG2RAD);
            const bool sloped = nodes[e.a].h != nodes[e.b].h || e.bank != 0.0f;

            // One road band (slab or asphalt): trimmed corners plus the cross-section stations.
            // Stations are the same along-the-edge positions the block pads use, so a sloped
            // road and the pads beside it share the same heights.
            struct Strip {
                bool ok = false;
                Vector2 lA{}, rA{}, lB{}, rB{};
                float w = 0.0f, s0 = 0.0f, s1 = 1.0f;
                std::vector<float> ss, yc, tp;   // stations, centreline height, bank taper (0..1)
            };
            auto makeStrip = [&](std::unordered_map<int, JunctionGeom>& cache, bool asphalt) {
                Strip sp;
                sp.w = asphalt ? EdgeAsphaltHalf(ei) : EdgeSlabHalf(ei);
                const float w = sp.w;
                endCorners(J(cache, e.a, asphalt), e.a, ei, w, d, sp.lA, sp.rA);
                endCorners(J(cache, e.b, asphalt), e.b, ei, w, dRev, sp.lB, sp.rB);
                auto ok = [&]() {
                    return Vector2DotProduct(Vector2Subtract(sp.lB, sp.rA), d) >= 1e-3f &&
                           Vector2DotProduct(Vector2Subtract(sp.rB, sp.lA), d) >= 1e-3f;
                };
                if (!ok()) {
                    // Junctions so close their trimmed ends cross: use flush ends.
                    sp.lA = Vector2Add(A, Vector2Scale(nL, w));
                    sp.rA = Vector2Subtract(A, Vector2Scale(nL, w));
                    sp.lB = Vector2Subtract(B, Vector2Scale(nL, w));
                    sp.rB = Vector2Add(B, Vector2Scale(nL, w));
                    if (!ok()) return sp;
                }
                sp.ok = true;
                const Vector2 mA = { (sp.lA.x + sp.rA.x) * 0.5f, (sp.lA.y + sp.rA.y) * 0.5f };
                const Vector2 mB = { (sp.lB.x + sp.rB.x) * 0.5f, (sp.lB.y + sp.rB.y) * 0.5f };
                sp.s0 = Vector2DotProduct(Vector2Subtract(mA, A), d) / elen;
                sp.s1 = Vector2DotProduct(Vector2Subtract(mB, A), d) / elen;
                if (sp.s1 <= sp.s0 + 1e-4f) { sp.s0 = 0.0f; sp.s1 = 1.0f; }
                sp.ss.push_back(sp.s0);
                if (sloped) {
                    for (float sk : EdgeStations(ei))
                        if (sk > sp.s0 + 1e-3f && sk < sp.s1 - 1e-3f) sp.ss.push_back(sk);
                }
                sp.ss.push_back(sp.s1);
                for (float sv : sp.ss) {
                    sp.yc.push_back(EdgeSurfaceY(ei, sv));
                    sp.tp.push_back(e.bank != 0.0f ? sinf(PI * EdgeRampU(ei, sv)) : 0.0f);
                }
                return sp;
            };
            // Height of a strip's centreline / bank taper at position s (linear between stations).
            auto stripLerp = [&](const Strip& sp, const std::vector<float>& arr, float sv) {
                if (sp.ss.size() < 2) return arr.empty() ? 0.0f : arr[0];
                size_t i = 0;
                while (i + 2 < sp.ss.size() && sv > sp.ss[i + 1]) i++;
                const float span = sp.ss[i + 1] - sp.ss[i];
                const float f = span > 1e-6f ? Clamp((sv - sp.ss[i]) / span, 0.0f, 1.0f) : 0.0f;
                return arr[i] + (arr[i + 1] - arr[i]) * f;
            };
            auto centreAt = [&](float sv) { return Vector2Add(A, Vector2Scale(d, sv * elen)); };
            // Surface height of a strip's plane at (s, lateral) before the layer offset.
            auto planeY = [&](const Strip& sp, float sv, float lat) {
                return stripLerp(sp, sp.yc, sv) + bankTan * stripLerp(sp, sp.tp, sv) * lat;
            };
            auto emitStrip = [&](const Strip& sp, float y, Color col) {
                if (!sp.ok) return;
                const float hA = nodes[(size_t)e.a].h;
                if (!sloped) {
                    mb.Quad(Vector3{ sp.rA.x, y + hA, sp.rA.y }, Vector3{ sp.lA.x, y + hA, sp.lA.y },
                            Vector3{ sp.rB.x, y + hA, sp.rB.y }, Vector3{ sp.lB.x, y + hA, sp.lB.y }, col);
                    return;
                }
                auto side = [&](const Vector2& p, const Vector2& q, float f, float yy) {
                    return Vector3{ p.x + (q.x - p.x) * f, yy, p.y + (q.y - p.y) * f };
                };
                // Each vertex takes the surface height at ITS OWN position along the road. The two side edges of a
                // strip are stepped by the same fraction, but when the strip's end cuts are skewed (a junction that
                // is not square) the left and right points of one fraction are at different distances along the
                // road; giving both the same centre-line height twisted the surface sideways (e.g. 10 degrees on a
                // 5 m ramp), so a pad that is level across the road showed above or below it.
                auto along = [&](const Vector3& pt) { return Clamp(Vector2DotProduct(Vector2Subtract({ pt.x, pt.z }, A), d) / elen, 0.0f, 1.0f); };
                // The two end lines stay level (both corners at the end station's height) so they meet the junction
                // plate, which is flat at the node height, exactly; only the interior follows the true position.
                auto vtx = [&](const Vector2& p0, const Vector2& p1, float f, float lat, size_t station) {
                    const Vector3 xz = side(p0, p1, f, 0.0f);
                    const bool endLine = station == 0 || station + 1 == sp.ss.size();
                    const float sv = endLine ? sp.ss[station] : along(xz);
                    return Vector3{ xz.x, y + planeY(sp, sv, lat), xz.z };
                };
                for (size_t i = 0; i + 1 < sp.ss.size(); i++) {
                    const float f0 = (sp.ss[i] - sp.s0) / (sp.s1 - sp.s0), f1 = (sp.ss[i + 1] - sp.s0) / (sp.s1 - sp.s0);
                    // P side = right of travel (lateral -w), Q side = left (+w).
                    mb.Quad(vtx(sp.rA, sp.lB, f0, -sp.w, i), vtx(sp.lA, sp.rB, f0, sp.w, i),
                            vtx(sp.lA, sp.rB, f1, sp.w, i + 1), vtx(sp.rA, sp.lB, f1, -sp.w, i + 1), col);
                }
            };

            const Strip slabS = makeStrip(slabC, false);
            const Strip aspS = makeStrip(aspC, true);
            // Surface colours by road type: asphalt, dirt track (Path), pale paving (Pedestrian).
            Color slabCol{ 62, 62, 66, 255 }, aspCol{ 82, 82, 88, 255 };
            if (e.type == (int)RoadType::Path) { slabCol = Color{ 104, 88, 66, 255 }; aspCol = Color{ 128, 108, 80, 255 }; }
            else if (e.type == (int)RoadType::Pedestrian) { slabCol = Color{ 170, 168, 160, 255 }; aspCol = Color{ 196, 192, 182, 255 }; }
            mb.curLayer = 0.02f;
            emitStrip(slabS, 0.02f + kRoadElevation, slabCol);
            mb.curLayer = 0.10f;
            emitStrip(aspS, 0.10f + kRoadElevation, aspCol);

            // A thin ribbon on the asphalt (lane lines): follows the asphalt plane + a small lift.
            auto ribbon = [&](float sa, float sb, float lat, float halfW, Color col) {
                if (!aspS.ok || sb - sa < 1e-3f) return;
                std::vector<float> br{ sa };
                for (float sv : aspS.ss) if (sv > sa + 1e-4f && sv < sb - 1e-4f) br.push_back(sv);
                br.push_back(sb);
                const float lift = 0.10f + kRoadElevation + 0.045f;
                for (size_t i = 0; i + 1 < br.size(); i++) {
                    auto pt = [&](float sv, float lt) {
                        const Vector2 c = Vector2Add(centreAt(sv), Vector2Scale(nL, lt));
                        return Vector3{ c.x, planeY(aspS, sv, lt) + lift, c.y };
                    };
                    mb.QuadUp(pt(br[i], lat - halfW), pt(br[i], lat + halfW), pt(br[i + 1], lat + halfW), pt(br[i + 1], lat - halfW), col);
                }
            };
            mb.curLayer = 0.145f;
            const int Lanes = e.lanes > 0 ? e.lanes : params.lanes;
            const float laneW = e.width > 0.0f ? e.width : params.laneWidth;
            const Color kWhite{ 232, 232, 238, 255 }, kYellow{ 232, 198, 58, 255 };
            if (e.markings && aspS.ok) {
                const float lo = aspS.s0 * elen + 2.0f, hi = aspS.s1 * elen - 2.0f;   // stay off the junctions
                for (int k = 1; k < Lanes; k++) {
                    const float lat = ((float)k - (float)Lanes * 0.5f) * laneW;
                    if (Lanes % 2 == 0 && k == Lanes / 2 && e.oneWay == 0) {
                        ribbon(lo / elen, hi / elen, lat, 0.09f, kYellow);   // centre line (two-way only)
                    } else {
                        for (float a0 = lo; a0 + 2.5f <= hi; a0 += 6.0f)
                            ribbon(a0 / elen, (a0 + 2.5f) / elen, lat, 0.07f, kWhite);
                    }
                }
            }

            // A flat strip across the asphalt (stop lines): lateral range [lat0, lat1], along [sa, sb].
            auto crossBand = [&](float sa, float sb, float lat0, float lat1, Color col) {
                if (!aspS.ok || sb - sa < 1e-3f) return;
                const float lift = 0.10f + kRoadElevation + 0.045f;
                auto pt = [&](float sv, float lt) {
                    const Vector2 c = Vector2Add(centreAt(sv), Vector2Scale(nL, lt));
                    return Vector3{ c.x, planeY(aspS, sv, lt) + lift, c.y };
                };
                mb.QuadUp(pt(sa, lat0), pt(sa, lat1), pt(sb, lat1), pt(sb, lat0), col);
            };
            // Crosswalks / stop lines at the junction ends of this road.
            if (aspS.ok) {
                const float aH = EdgeAsphaltHalf(ei);
                const float spanLen = (aspS.s1 - aspS.s0) * elen;
                for (int end = 0; end < 2; end++) {
                    const int ni = end == 0 ? e.a : e.b;
                    const int kind = nodes[(size_t)ni].jkind;
                    if (!nodes[(size_t)ni].junction || kind == (int)JunctionKind::Plain) continue;
                    // Distance from the trimmed road end measured along the road, then mapped to s.
                    // Where this road's asphalt actually starts at lateral position lt: junction corners on angled /
                    // organic layouts are cut diagonally, so the start differs across the road.
                    const float sPos = Vector2DotProduct(Vector2Subtract(end == 0 ? aspS.lA : aspS.rB, A), d) / elen;   // lateral +w
                    const float sNeg = Vector2DotProduct(Vector2Subtract(end == 0 ? aspS.rA : aspS.lB, A), d) / elen;   // lateral -w
                    auto sStart = [&](float lt) { const float f = Clamp(lt / std::max(aH, 1e-3f) * 0.5f + 0.5f, 0.0f, 1.0f); return sNeg + (sPos - sNeg) * f; };
                    const float sFar = end == 0 ? std::max(sPos, sNeg) : std::min(sPos, sNeg);
                    auto sAt = [&](float dist) { return end == 0 ? sFar + dist / elen : sFar - dist / elen; };
                    auto band = [&](float d0, float d1, float lat0, float lat1, Color col) {
                        const float x = sAt(d0), y2 = sAt(d1);
                        crossBand(std::min(x, y2), std::max(x, y2), lat0, lat1, col);
                    };
                    // Traffic arrives at this end if the road allows travel toward it.
                    const bool arrives = e.oneWay == 0 || (e.oneWay == 1 && end == 1) || (e.oneWay == 2 && end == 0);
                    // Right-hand traffic: arriving lane is on the right of travel direction.
                    float latLo = -aH, latHi = aH;
                    if (e.oneWay == 0) { if ((end == 1) != params.leftHandTraffic) latHi = 0.0f; else latLo = 0.0f; }
                    const bool zebra = kind == (int)JunctionKind::Crosswalks || kind == (int)JunctionKind::TrafficLight || kind == (int)JunctionKind::Roundabout;
                    if (zebra && spanLen > 7.0f) {
                        // One straight band, square to the road, set back past the farthest corner of the (diagonally cut) road end.
                        const float zl = std::min(2.8f, spanLen * 0.3f);
                        const float sgn = end == 0 ? 1.0f : -1.0f;
                        const float zoff = nodes[(size_t)ni].jkind == (int)JunctionKind::Roundabout ? 3.4f : 1.0f;   // past the splitter island
                        const float s0v = sFar + sgn * zoff / elen, s1v = s0v + sgn * zl / elen;
                        for (float lt = -aH + 0.6f; lt <= aH - 0.6f; lt += 1.0f)
                            ribbon(std::min(s0v, s1v), std::max(s0v, s1v), lt, 0.27f, kWhite);
                    }
                    if (arrives && spanLen > 10.0f && (kind == (int)JunctionKind::TrafficLight || kind == (int)JunctionKind::Stop))
                        band(zebra ? 4.4f + (kind == (int)JunctionKind::Roundabout ? 2.4f : 0.0f) : 1.0f, zebra ? 5.0f + (kind == (int)JunctionKind::Roundabout ? 2.4f : 0.0f) : 1.6f, latLo + 0.1f, latHi - 0.1f, kWhite);
                }
            }

            // One-way arrows: a shaft and a head every ~24 m, one per lane, pointing along travel.
            if (e.oneWay != 0 && aspS.ok) {
                const float dirSign = e.oneWay == 1 ? 1.0f : -1.0f;
                const float lo = aspS.s0 * elen + 5.0f, hi = aspS.s1 * elen - 5.0f;
                const float lift = 0.10f + kRoadElevation + 0.045f;
                auto pt = [&](float dist, float lt) {
                    const float sv = dist / elen;
                    const Vector2 c = Vector2Add(centreAt(sv), Vector2Scale(nL, lt));
                    return Vector3{ c.x, planeY(aspS, sv, lt) + lift, c.y };
                };
                for (float a0 = lo; a0 + 4.0f <= hi; a0 += 24.0f) {
                    for (int k = 0; k < Lanes; k++) {
                        const float lat = ((float)k + 0.5f - (float)Lanes * 0.5f) * laneW;
                        const float tail = dirSign > 0 ? a0 : a0 + 3.6f, neck = tail + dirSign * 2.4f, tip = tail + dirSign * 3.6f;
                        mb.QuadUp(pt(tail, lat - 0.12f), pt(tail, lat + 0.12f), pt(neck, lat + 0.12f), pt(neck, lat - 0.12f), kWhite);
                        mb.QuadUp(pt(neck, lat - 0.55f), pt(neck, lat + 0.55f), pt(tip, lat), pt(tip, lat), kWhite);
                    }
                }
            }

            // Turn-lane arrows painted on the approach to each junction (lane use set per road end).
            if (aspS.ok && (e.turnA || e.turnB)) {
                mb.curLayer = 0.145f;
                const float lift = 0.10f + kRoadElevation + 0.045f;
                for (int end = 0; end < 2; end++) {
                    const uint16_t mask = end == 0 ? e.turnA : e.turnB;
                    if (!mask) continue;
                    if (e.oneWay != 0 && !((e.oneWay == 1 && end == 1) || (e.oneWay == 2 && end == 0))) continue;
                    const float dirSign = end == 1 ? 1.0f : -1.0f;
                    const float leftSign = dirSign;                  // left of travel in nL coordinates
                    const int perDir = e.oneWay ? Lanes : std::max(Lanes / 2, 1);
                    const float span = (aspS.s1 - aspS.s0) * elen;
                    if (span < 13.0f) continue;
                    const float inset = std::min(8.5f, span * 0.45f);
                    const float baseDist = end == 1 ? aspS.s1 * elen - inset : aspS.s0 * elen + inset;
                    for (int l = 0; l < std::min(perDir, 4); l++) {
                        const int bits = (mask >> (4 * l)) & 7;
                        if (!bits) continue;
                        float lat = e.oneWay ? -(((float)l + 0.5f - (float)Lanes * 0.5f) * laneW * (end == 1 ? 1.0f : -1.0f))
                                             : ((float)l + 0.5f) * laneW;
                        const float latNL = ((e.oneWay == 0 && params.leftHandTraffic) ? dirSign : -dirSign) * lat;
                        auto P = [&](float u, float v) {
                            const float sv = (baseDist + dirSign * u) / elen;
                            const float lt = latNL + leftSign * v;
                            const Vector2 c = Vector2Add(centreAt(sv), Vector2Scale(nL, lt));
                            return Vector3{ c.x, planeY(aspS, sv, lt) + lift, c.y };
                        };
                        auto tri = [&](Vector3 a0, Vector3 b0, Vector3 c0) { mb.QuadUp(a0, b0, c0, c0, kWhite); };
                        if (bits & 2) {            // straight
                            mb.QuadUp(P(0, -0.12f), P(0, 0.12f), P(1.8f, 0.12f), P(1.8f, -0.12f), kWhite);
                            tri(P(1.8f, -0.5f), P(1.8f, 0.5f), P(2.9f, 0.0f));
                        }
                        for (int side = -1; side <= 1; side += 2) {   // +1 left, -1 right
                            if (!(bits & (side > 0 ? 1 : 4))) continue;
                            const float sd = (float)side;
                            mb.QuadUp(P(0, -0.12f), P(0, 0.12f), P(1.0f, 0.12f), P(1.0f, -0.12f), kWhite);
                            mb.QuadUp(P(1.0f, -0.12f), P(1.0f, 0.12f), P(1.9f, sd * 0.75f + 0.12f), P(1.9f, sd * 0.75f - 0.12f), kWhite);
                            tri(P(1.55f, sd * 1.05f), P(2.15f, sd * 0.45f), P(2.4f, sd * 1.15f));
                        }
                    }
                }
            }

            // Street furniture: streetlights (alternating sides) and trees along ordinary sidewalks.
            if (params.furniture && slabS.ok && !e.bridge && e.type != (int)RoadType::Highway && e.type != (int)RoadType::Path) {
                const float aH2 = EdgeAsphaltHalf(ei), sH2 = EdgeSlabHalf(ei);
                if (sH2 - aH2 >= 0.9f) {
                    const float lat = aH2 + (sH2 - aH2) * 0.5f;
                    const float lo = slabS.s0 * elen + 5.0f, hi = slabS.s1 * elen - 5.0f;
                    auto place = [&](int shape, float dist, int sd, float yawBase, float scale) {
                        const float sv = dist / elen;
                        const Vector2 c = Vector2Add(centreAt(sv), Vector2Scale(nL, lat * (float)sd));
                        const float y = planeY(slabS, sv, lat * (float)sd) + kRoadElevation + 0.07f + e.curbH;
                        // Local +z (the lamp arm) points back toward the road.
                        const float yaw = yawBase + atan2f(-nL.x * (float)sd, -nL.y * (float)sd);
                        Matrix m = MatrixMultiply(MatrixScale(scale, scale, scale),
                                   MatrixMultiply(MatrixRotateY(yaw), MatrixTranslate(c.x, y, c.y)));
                        m.m3 = -1.0f; m.m7 = -1.0f; m.m11 = -1.0f;   // negative tint = prop (vertex colours)
                        t.inst[shape].push_back(m);
                    };
                    int n = 0;
                    for (float dist = lo; dist <= hi; dist += 11.0f, n++) {
                        const uint32_t hv = CoordHash(ei, n, params.seed ^ 0x51ED);
                        if ((n & 1) == 0) place(kPropLamp, dist, ((n >> 1) & 1) ? 1 : -1, 0.0f, 1.0f);
                        else place(kPropTree, dist, ((n >> 1) & 1) ? -1 : 1, (float)(hv % 628) * 0.01f, 0.85f + (float)(hv % 40) * 0.01f);
                        // Occasional extras beside the lamps and trees: hydrants, signs, benches, bollards.
                        const uint32_t hx = CoordHash(ei, n + 977, params.seed ^ 0x7B33) % 100u;
                        if ((n & 1) == 0) {
                            const int sdLamp = ((n >> 1) & 1) ? 1 : -1;
                            if (hx < 12u && dist + 1.6f <= hi) place(kPropHydrant, dist + 1.6f, sdLamp, 0.0f, 1.0f);
                            else if (hx < 22u && dist + 0.9f <= hi) place(kPropSign, dist + 0.9f, sdLamp, 0.0f, 1.0f);
                        } else {
                            const int sdTree = ((n >> 1) & 1) ? -1 : 1;
                            if (hx < 30u && dist + 3.2f <= hi) place(kPropBench, dist + 3.2f, sdTree, 0.0f, 1.0f);
                            else if (hx < 42u)
                                for (int k = 0; k < 3; k++) if (dist + 3.0f + 1.4f * (float)k <= hi) place(kPropBollard, dist + 3.0f + 1.4f * (float)k, sdTree, 0.0f, 1.0f);
                        }
                    }
                    // Bus stops (data: placed by hand or generated by AutoTransit), on the kerb of the direction they serve.
                    for (const BusStop& bs : busStops) {
                        if (bs.edge != ei) continue;
                        const int sdStop = (bs.fwd ? -1 : 1) * (params.leftHandTraffic ? -1 : 1);
                        place(kPropBusStop, std::clamp(bs.s, lo, hi), sdStop, 0.0f, 1.0f);
                    }
                }
            }

            // Highway: a concrete barrier down the middle.
            if (e.type == (int)RoadType::Highway && aspS.ok) {
                const float bh = 0.85f, bw = 0.3f;
                const Color barCol{ 176, 176, 180, 255 }, barSide{ 150, 150, 156, 255 };
                mb.curLayer = 0.2f;
                for (size_t i = 0; i + 1 < aspS.ss.size(); i++) {
                    const float a0 = aspS.ss[i], a1 = aspS.ss[i + 1];
                    auto pt = [&](float sv, float lt, float up) {
                        const Vector2 c = Vector2Add(centreAt(sv), Vector2Scale(nL, lt));
                        return Vector3{ c.x, planeY(aspS, sv, lt) + 0.10f + kRoadElevation + up, c.y };
                    };
                    mb.QuadUp(pt(a0, -bw, bh), pt(a0, bw, bh), pt(a1, bw, bh), pt(a1, -bw, bh), barCol);
                    mb.WallQuad(pt(a0, bw, 0.0f), pt(a1, bw, 0.0f), pt(a1, bw, bh), pt(a0, bw, bh), Vector3{ nL.x, 0.0f, nL.y }, barSide);
                    mb.WallQuad(pt(a0, -bw, 0.0f), pt(a1, -bw, 0.0f), pt(a1, -bw, bh), pt(a0, -bw, bh), Vector3{ -nL.x, 0.0f, -nL.y }, barSide);
                }
            }

            // Raised sidewalks: top surface + the curb face toward the asphalt.
            mb.curLayer = 0.07f + e.curbH;
            if (e.curbH > 0.001f && slabS.ok) {
                const float aH = EdgeAsphaltHalf(ei), sH = EdgeSlabHalf(ei);
                if (sH > aH + 0.05f) {
                    const Color swCol{ 172, 172, 176, 255 }, curbCol{ 150, 150, 156, 255 };
                    for (int sd = -1; sd <= 1; sd += 2) {
                        for (size_t i = 0; i + 1 < slabS.ss.size(); i++) {
                            const float s0v = slabS.ss[i], s1v = slabS.ss[i + 1];
                            auto top = [&](float sv, float lat) {
                                const Vector2 c = Vector2Add(centreAt(sv), Vector2Scale(nL, lat * (float)sd));
                                return Vector3{ c.x, planeY(slabS, sv, lat * (float)sd) + kRoadElevation + 0.07f + e.curbH, c.y };
                            };
                            mb.QuadUp(top(s0v, aH), top(s0v, sH), top(s1v, sH), top(s1v, aH), swCol);
                            auto face = [&](float sv, float yy) {
                                const Vector2 c = Vector2Add(centreAt(sv), Vector2Scale(nL, aH * (float)sd));
                                return Vector3{ c.x, planeY(slabS, sv, aH * (float)sd) + kRoadElevation + yy, c.y };
                            };
                            const Vector3 inward = { -nL.x * (float)sd, 0.0f, -nL.y * (float)sd };
                            mb.WallQuad(face(s0v, 0.10f), face(s1v, 0.10f), face(s1v, 0.07f + e.curbH), face(s0v, 0.07f + e.curbH), inward, curbCol);
                        }
                    }
                }
            }

            // Bridge: deck with side walls, a parapet along each edge, and support pillars.
            mb.curLayer = 0.04f;
            if (e.bridge && slabS.ok) {
                const float sH = EdgeSlabHalf(ei);
                const float T = 0.7f, parapet = 1.0f, pw = 0.3f;
                const Color deckCol{ 140, 140, 146, 255 }, pillarCol{ 118, 118, 124, 255 };
                auto edgePt = [&](float sv, float lat, float yy) {
                    const Vector2 c = Vector2Add(centreAt(sv), Vector2Scale(nL, lat));
                    return Vector3{ c.x, planeY(slabS, sv, lat) + kRoadElevation + 0.02f + yy, c.y };
                };
                for (size_t i = 0; i + 1 < slabS.ss.size(); i++) {
                    const float a0 = slabS.ss[i], a1 = slabS.ss[i + 1];
                    for (int sd = -1; sd <= 1; sd += 2) {
                        const float lat = sH * (float)sd;
                        const Vector3 out = { nL.x * (float)sd, 0.0f, nL.y * (float)sd };
                        const Vector3 in = { -out.x, 0.0f, -out.z };
                        // Outer face: deck bottom up to the top of the parapet.
                        mb.WallQuad(edgePt(a0, lat, -T), edgePt(a1, lat, -T), edgePt(a1, lat, parapet), edgePt(a0, lat, parapet), out, deckCol);
                        // Parapet inner face and top cap.
                        mb.WallQuad(edgePt(a0, lat - pw * (float)sd, 0.0f), edgePt(a1, lat - pw * (float)sd, 0.0f),
                                    edgePt(a1, lat - pw * (float)sd, parapet), edgePt(a0, lat - pw * (float)sd, parapet), in, deckCol);
                        mb.WallQuad(edgePt(a0, lat - pw * (float)sd, parapet), edgePt(a1, lat - pw * (float)sd, parapet),
                                    edgePt(a1, lat, parapet), edgePt(a0, lat, parapet), Vector3{ 0.0f, 1.0f, 0.0f }, deckCol);
                    }
                    // Underside.
                    mb.WallQuad(edgePt(a0, sH, -T), edgePt(a0, -sH, -T), edgePt(a1, -sH, -T), edgePt(a1, sH, -T), Vector3{ 0.0f, -1.0f, 0.0f }, deckCol);
                }
                // Pillars down to the ground plane every ~14 m along the span.
                const float pillar = 0.7f;
                for (float pd = slabS.s0 * elen + 7.0f; pd < slabS.s1 * elen - 5.0f; pd += 14.0f) {
                    const float sv = pd / elen;
                    const Vector2 c = centreAt(sv);
                    const float top = planeY(slabS, sv, 0.0f) + kRoadElevation + 0.02f - T;
                    if (top < 1.0f) continue;
                    const Vector2 cs[4] = { Vector2Add(Vector2Add(c, Vector2Scale(d, pillar)), Vector2Scale(nL, pillar)),
                                            Vector2Add(Vector2Subtract(c, Vector2Scale(d, pillar)), Vector2Scale(nL, pillar)),
                                            Vector2Subtract(Vector2Subtract(c, Vector2Scale(d, pillar)), Vector2Scale(nL, pillar)),
                                            Vector2Subtract(Vector2Add(c, Vector2Scale(d, pillar)), Vector2Scale(nL, pillar)) };
                    for (int k = 0; k < 4; k++) {
                        const Vector2 &p0 = cs[k], &p1 = cs[(k + 1) % 4];
                        const Vector2 mid = Vector2Scale(Vector2Add(p0, p1), 0.5f);
                        Vector2 nrm = Vector2Normalize(Vector2Subtract(mid, c));
                        mb.WallQuad(Vector3{ p0.x, 0.0f, p0.y }, Vector3{ p1.x, 0.0f, p1.y }, Vector3{ p1.x, top, p1.y }, Vector3{ p0.x, top, p0.y },
                                    Vector3{ nrm.x, 0.0f, nrm.y }, pillarCol);
                    }
                }
            }
        }

        mb.curLayer = 0.0f;
        // Junction plates (rounded corners live here): fan from the node centre.
        const float capSlabY = 0.04f + kRoadElevation; // above the slab strips
        const float capAspY = 0.12f + kRoadElevation; // above the asphalt strips
        auto emitPlate = [&](const JunctionGeom& g, int ni, float y, Color col) {
            if (!g.valid) return;
            std::vector<Vector3> pts;
            pts.reserve(g.poly.size() + 2);
            const float yy = y + nodes[(size_t)ni].h;
            pts.push_back({ nodes[ni].pos.x, yy, nodes[ni].pos.y });
            for (const Vector2& p : g.poly) pts.push_back({ p.x, yy, p.y });
            pts.push_back({ g.poly[0].x, yy, g.poly[0].y });
            mb.Fan(pts, col);
        };
        for (int ni : t.nodes) {
            mb.curLayer = 0.04f;
            emitPlate(J(slabC, ni, false), ni, capSlabY, Color{ 62, 62, 66, 255 });
            mb.curLayer = 0.12f;
            emitPlate(J(aspC, ni, true), ni, capAspY, Color{ 82, 82, 88, 255 });
            if (nodes[(size_t)ni].junction && nodes[(size_t)ni].jkind == (int)JunctionKind::Roundabout) {
                // Central island: a raised concrete kerb ring around a grass top, plus a concrete splitter island on each arm.
                const float Ri = RoundaboutRadius(ni);
                const float baseY = nodes[(size_t)ni].h + capAspY;
                const float top = baseY + 0.2f;
                const Vector2 c = nodes[(size_t)ni].pos;
                const int seg = 40;
                const float kerb = 1.4f;
                mb.curLayer = 0.2f;
                std::vector<Vector3> grass;
                grass.push_back({ c.x, top, c.y });
                for (int k = 0; k <= seg; k++) {
                    const float ang = 2.0f * PI * (float)k / (float)seg;
                    grass.push_back({ c.x + cosf(ang) * (Ri - kerb), top, c.y + sinf(ang) * (Ri - kerb) });
                }
                mb.Fan(grass, nodes[(size_t)ni].rbConcrete ? Color{ 168, 168, 172, 255 } : Color{ 108, 158, 94, 255 });
                auto at = [&](Vector3 v, float y) { v.y = y; return v; };
                for (int k = 0; k < seg; k++) {
                    const float a0 = 2.0f * PI * (float)k / (float)seg, a1 = 2.0f * PI * (float)(k + 1) / (float)seg;
                    const Vector3 o0 = { c.x + cosf(a0) * Ri, 0.0f, c.y + sinf(a0) * Ri }, o1 = { c.x + cosf(a1) * Ri, 0.0f, c.y + sinf(a1) * Ri };
                    const Vector3 i0 = { c.x + cosf(a0) * (Ri - kerb), 0.0f, c.y + sinf(a0) * (Ri - kerb) }, i1 = { c.x + cosf(a1) * (Ri - kerb), 0.0f, c.y + sinf(a1) * (Ri - kerb) };
                    mb.QuadUp(at(o0, top), at(o1, top), at(i1, top), at(i0, top), Color{ 176, 176, 180, 255 });   // concrete kerb top
                    const Vector3 nrm = { cosf((a0 + a1) * 0.5f), 0.0f, sinf((a0 + a1) * 0.5f) };
                    mb.WallQuad(at(o0, baseY), at(o1, baseY), at(o1, top), at(o0, top), nrm, Color{ 150, 150, 156, 255 });
                }
                // Concrete splitter islands where each road meets the ring.
                if ((size_t)ni < nodeEdges.size() && nodes[(size_t)ni].rbSplitters)
                    for (int ei2 : nodeEdges[(size_t)ni]) {
                        const RoadEdge& e2 = edges[(size_t)ei2];
                        const int other = e2.a == ni ? e2.b : e2.a;
                        const Vector2 u = Vector2Normalize(Vector2Subtract(nodes[(size_t)other].pos, c));
                        if (u.x == 0.0f && u.y == 0.0f) continue;
                        const Vector2 nn = { -u.y, u.x };
                        const float r0 = RoundaboutOuterRadius(ni, true) - 0.6f, r1 = r0 + 3.4f;
                        const float hw0 = 0.1f, hw1 = 0.9f;
                        const float sy = baseY + 0.12f;
                        const Vector3 a0 = { c.x + u.x * r0 - nn.x * hw0, sy, c.y + u.y * r0 - nn.y * hw0 }, a1 = { c.x + u.x * r0 + nn.x * hw0, sy, c.y + u.y * r0 + nn.y * hw0 };
                        const Vector3 b0 = { c.x + u.x * r1 - nn.x * hw1, sy, c.y + u.y * r1 - nn.y * hw1 }, b1 = { c.x + u.x * r1 + nn.x * hw1, sy, c.y + u.y * r1 + nn.y * hw1 };
                        mb.QuadUp(a0, a1, b1, b0, Color{ 176, 176, 180, 255 });
                    }
            }
        }
    }

    // Vertex ranges of sloped pads/grass: their normals are smoothed toward the pad's average below.
    std::vector<std::pair<size_t, size_t>> slopedRanges;

    // Block pads (concrete) under the buildings.
    for (int bi : t.blocks) {
        const Block& block = blocks[(size_t)bi];
        const size_t padRangeStart = mb.verts.size() / 3;
        std::vector<Vector2> poly;
        poly.reserve(block.nodes.size());
        for (int idx : block.nodes) poly.push_back(nodes[idx].pos);

        mb.curLayer = 0.06f;
        BlockSurface sf;
        BuildBlockSurface(block, sf);
        const Color padColor = block.park ? Color{ 108, 158, 94, 255 } : Color{ 158, 158, 162, 255 };
        const float padY = 0.06f + kRoadElevation;
        if (sf.flat) {
            std::vector<int> tris;
            citygeom::TriangulateSimple(poly, tris);
            ReportIncompleteFill("pad", poly, tris);
            for (size_t t = 0; t + 2 < tris.size(); t += 3) {
                int base = (int)(mb.verts.size() / 3);
                mb.Vertex({ poly[tris[t]].x, padY + sf.flatY, poly[tris[t]].y }, padColor);
                mb.Vertex({ poly[tris[t + 1]].x, padY + sf.flatY, poly[tris[t + 1]].y }, padColor);
                mb.Vertex({ poly[tris[t + 2]].x, padY + sf.flatY, poly[tris[t + 2]].y }, padColor);
                // Same up-facing winding as the strips (see Fan): base,base+2,base+1.
                mb.Triangle(base, base + 2, base + 1);
            }
        } else {
            // Shared vertices, so RecomputeNormals smooths the slope (no flat-shaded slivers).
            const std::vector<Vector3>& V = sf.ringOk ? sf.R : sf.B;
            const int baseV = (int)(mb.verts.size() / 3);
            for (const Vector3& v : V) mb.Vertex({ v.x, v.y + padY, v.z }, padColor);
            int baseB = baseV;
            if (sf.ringOk) {
                baseB = (int)(mb.verts.size() / 3);
                // The outer edge of the apron lies on the road centre line, under the road. Sunk below the road surface so
                // an apron triangle never pokes through it where the road and the sloped pad edge differ (a thin spike).
                for (const Vector3& v : sf.B) mb.Vertex({ v.x, v.y + padY - 0.06f, v.z }, padColor);
            }
            // Every pad triangle is emitted facing up whatever order its corners came in (a concave or acute block
            // corner can flip the ring/apron strip, and a flipped triangle is culled and leaves a hole).
            auto upTri = [&](int a, int b, int c) {
                const float ax = mb.verts[(size_t)a * 3], az = mb.verts[(size_t)a * 3 + 2];
                const float ux = mb.verts[(size_t)b * 3] - ax, uz = mb.verts[(size_t)b * 3 + 2] - az;
                const float vx = mb.verts[(size_t)c * 3] - ax, vz = mb.verts[(size_t)c * 3 + 2] - az;
                // Collinear in (x,z) but at different heights = a vertical curtain: it is invisible from above, adds a
                // sideways normal to the vertices it touches (dark smears) and an invisible wall to the collision surface.
                if (fabsf(uz * vx - ux * vz) < 2e-3f) return;
                {   // Nearly vertical too (a sliver whose points are collinear to a few centimetres): same problem.
                    const float uy = mb.verts[(size_t)b * 3 + 1] - mb.verts[(size_t)a * 3 + 1], vy = mb.verts[(size_t)c * 3 + 1] - mb.verts[(size_t)a * 3 + 1];
                    const float nx = uy * vz - uz * vy, nz = ux * vy - uy * vx, ny = fabsf(uz * vx - ux * vz);
                    if (ny < 0.35f * sqrtf(nx * nx + ny * ny + nz * nz)) return;
                }
                if (uz * vx - ux * vz >= 0.0f) mb.Triangle(a, b, c);
                else mb.Triangle(a, c, b);
            };
            for (size_t t = 0; t + 2 < sf.tris.size(); t += 3)
                upTri(baseV + sf.tris[t], baseV + sf.tris[t + 2], baseV + sf.tris[t + 1]);
            if (sf.ringOk) {
                const int m = (int)sf.B.size();
                for (int j = 0; j < m; j++) {   // flat apron out to the road slab edge
                    const int k = (j + 1) % m;
                    upTri(baseB + j, baseV + k, baseB + k);
                    upTri(baseB + j, baseV + j, baseV + k);
                }
            }
        }

        if (!sf.flat) slopedRanges.push_back({ padRangeStart, mb.verts.size() / 3 });
        const size_t grassStart = mb.verts.size() / 3;
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
                const double insetUsed = std::min(params.parkInset, BlockRoadHalf(block) * 0.8f);
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
            mb.curLayer = 0.08f;
            const Color grassColor{ 108, 158, 94, 255 };
            for (size_t t = 0; t + 2 < ptris.size(); t += 3) {
                int base = (int)(mb.verts.size() / 3);
                for (int k = 0; k < 3; k++) {
                    const Vector2& gp = block.parkPoly[ptris[t + (size_t)k]];
                    mb.Vertex({ gp.x, 0.08f + kRoadElevation + sf.HeightAt(gp.x, gp.y), gp.y }, grassColor);
                }
                mb.Triangle(base, base + 2, base + 1);
            }
            if (!sf.flat) slopedRanges.push_back({ grassStart, mb.verts.size() / 3 });
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
                // Anti z-fighting: neighbouring or overlapping buildings often share wall/roof planes
                // exactly. Each instance gets a tiny deterministic size difference (footprint -0..6 cm,
                // height +0..8 cm, bottom fixed) so coincident faces never land on the same depth.
                const uint32_t jh = CoordHash((int)floorf(b.center.x * 4.0f), (int)floorf(b.center.z * 4.0f), params.seed ^ 0x2F1B);
                const float jx = (float)(jh & 0xFFu) / 255.0f * 0.06f, jz = (float)((jh >> 8) & 0xFFu) / 255.0f * 0.06f;
                const float jy = (float)((jh >> 16) & 0xFFu) / 255.0f * 0.08f;
                Matrix m = MatrixMultiply(
                    MatrixScale(std::max(b.size.x - jx, 0.1f), b.size.y + jy, std::max(b.size.z - jz, 0.1f)),
                    MatrixMultiply(MatrixRotateY(b.angleY),
                                   MatrixTranslate(b.center.x, b.center.y + jy * 0.5f, b.center.z)));
                const Color tint = kStyleTints[std::clamp(b.style, 0, 3)][color];
                m.m3 = tint.r / 255.0f;
                m.m7 = tint.g / 255.0f;
                m.m11 = tint.b / 255.0f;
                t.inst[shape].push_back(m);
                if (collisionEnabled)
                    t.coll.buildings.push_back({ b.center, Vector3Scale(b.size, 0.5f), b.angleY, shape });

                const float r = 0.5f * sqrtf(b.size.x * b.size.x + b.size.z * b.size.z) * 1.3f + 0.5f;
                bmin.x = fminf(bmin.x, b.center.x - r); bmax.x = fmaxf(bmax.x, b.center.x + r);
                bmin.z = fminf(bmin.z, b.center.z - r); bmax.z = fmaxf(bmax.z, b.center.z + r);
                bmin.y = fminf(bmin.y, b.center.y - b.size.y * 0.5f - 0.5f);
                bmax.y = fmaxf(bmax.y, b.center.y + b.size.y * 0.5f + 0.5f);
                t.hasBldg = true;
            }
        }
        for (int sp : { (int)kPropLamp, (int)kPropTree, (int)kPropBench, (int)kPropHydrant, (int)kPropBollard, (int)kPropBusStop, (int)kPropSign })
            for (const Matrix& pm : t.inst[sp]) {
                bmin.x = fminf(bmin.x, pm.m12 - 3.0f); bmax.x = fmaxf(bmax.x, pm.m12 + 3.0f);
                bmin.y = fminf(bmin.y, pm.m13 - 0.5f); bmax.y = fmaxf(bmax.y, pm.m13 + 8.0f);
                bmin.z = fminf(bmin.z, pm.m14 - 3.0f); bmax.z = fmaxf(bmax.z, pm.m14 + 3.0f);
                t.hasBldg = true;
            }
        if (t.hasBldg) { t.bldgMin = bmin; t.bldgMax = bmax; }
    }

    if (mb.verts.empty()) return;
    mb.RecomputeNormals();
    // A sloped pad is warped (its edge heights follow the roads' eased profiles) and split into fans, so per-vertex
    // normals differ a lot between neighbouring vertices and show as radial shading wedges. Pull them toward the
    // average normal of the whole pad (the grass, flat-shaded per triangle, gets the same treatment).
    for (const auto& r : slopedRanges) {
        if (r.second <= r.first) continue;
        Vector3 avg = { 0.0f, 0.0f, 0.0f };
        for (size_t i = r.first; i < r.second; i++) avg = Vector3Add(avg, { mb.normals[i * 3], mb.normals[i * 3 + 1], mb.normals[i * 3 + 2] });
        if (Vector3Length(avg) < 1e-6f) continue;
        avg = Vector3Normalize(avg);
        for (size_t i = r.first; i < r.second; i++) {
            if (mb.fixedNormal[i]) continue;
            const Vector3 n = Vector3Normalize(Vector3Add(Vector3Scale({ mb.normals[i * 3], mb.normals[i * 3 + 1], mb.normals[i * 3 + 2] }, 0.3f), Vector3Scale(avg, 0.7f)));
            mb.normals[i * 3] = n.x; mb.normals[i * 3 + 1] = n.y; mb.normals[i * 3 + 2] = n.z;
        }
    }

    // Collision surface: every road/pad/park triangle, wound so its normal faces up.
    if (collisionEnabled) {
        t.coll.surfVerts.reserve(mb.verts.size() / 3);
        for (size_t i = 0; i + 2 < mb.verts.size(); i += 3)
            t.coll.surfVerts.push_back({ mb.verts[i], mb.verts[i + 1], mb.verts[i + 2] });
        t.coll.surfLayer.reserve(mb.verts.size() / 3);
        for (size_t i = 0; i + 1 < mb.texcoords.size(); i += 2) t.coll.surfLayer.push_back(mb.texcoords[i]);
        t.coll.surfIdx.reserve(mb.indices.size());
        for (size_t i = 0; i + 2 < mb.indices.size(); i += 3) {
            int a = mb.indices[i], b = mb.indices[i + 1], c = mb.indices[i + 2];
            if (i / 3 < mb.wallTri.size() && mb.wallTri[i / 3]) {   // walls/decks: keep as built
                t.coll.surfIdx.push_back(a); t.coll.surfIdx.push_back(b); t.coll.surfIdx.push_back(c);
                continue;
            }
            const Vector3 &pa = t.coll.surfVerts[(size_t)a], &pb = t.coll.surfVerts[(size_t)b], &pc = t.coll.surfVerts[(size_t)c];
            const float ny = (pb.z - pa.z) * (pc.x - pa.x) - (pb.x - pa.x) * (pc.z - pa.z);
            if (fabsf(ny) < 1e-6f) continue; // degenerate
            if (ny < 0.0f) std::swap(b, c);
            t.coll.surfIdx.push_back(a); t.coll.surfIdx.push_back(b); t.coll.surfIdx.push_back(c);
        }
    }

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
    CreateTilePhysics(t); // no-op unless a play-mode physics world is attached
    gfx::MarkShadowsDirty();
}

// ---------------------------------------------------------------------------
// Play-mode collision (one static Box3D body per tile)
// ---------------------------------------------------------------------------
struct City::TilePhysics {
    b3BodyId body{};
    std::vector<b3HullData*> hulls;
    std::vector<b3MeshData*> meshes;
};

namespace {
struct PhysCtx { bool active = false; b3WorldId world{}; float friction = 0.4f; float restitution = 0.0f; };
PhysCtx g_phys;

// Unit-mesh points of a non-box building shape, deduplicated.
const std::vector<b3Vec3>& ShapeUnitPoints(int shape) {
    static std::vector<b3Vec3> cache[kBuildingShapes];
    std::vector<b3Vec3>& pts = cache[shape];
    if (pts.empty()) {
        const Mesh m = gfx::GetCityShapeMesh(shape);
        for (int i = 0; i < m.vertexCount; i++) {
            const b3Vec3 v = { m.vertices[i * 3], m.vertices[i * 3 + 1], m.vertices[i * 3 + 2] };
            bool dup = false;
            for (const b3Vec3& q : pts)
                if (fabsf(q.x - v.x) + fabsf(q.y - v.y) + fabsf(q.z - v.z) < 1e-4f) { dup = true; break; }
            if (!dup) pts.push_back(v);
        }
    }
    return pts;
}
} // namespace

void City::DestroyTilePhysics(Tile& t) {
    if (!t.phys) return;
    if (g_phys.active && b3Body_IsValid(t.phys->body)) b3DestroyBody(t.phys->body);
    for (b3HullData* h : t.phys->hulls) b3DestroyHull(h);
    for (b3MeshData* m : t.phys->meshes) b3DestroyMesh(m);
    t.phys.reset();
}

void City::CreateTilePhysics(Tile& t) {
    DestroyTilePhysics(t);
    if (!g_phys.active || !collisionEnabled) return;
    if (t.coll.buildings.empty() && t.coll.surfIdx.empty()) return;

    auto phys = std::make_shared<TilePhysics>();
    phys->body = b3wrap::CreateBody(g_phys.world, Vector3{ 0.0f, 0.0f, 0.0f }, QuaternionIdentity(), b3_staticBody);

    for (const CollBox& cb : t.coll.buildings) {
        const Quaternion q = QuaternionFromAxisAngle({ 0.0f, 1.0f, 0.0f }, cb.angleY);
        if (cb.shape == kBuildingBox) {
            // Pure rectangular building: an exact (rotated) box.
            b3BoxHull box = b3MakeTransformedBoxHull(cb.half.x, cb.half.y, cb.half.z,
                                                     b3wrap::ToB3Transform(cb.center, q));
            b3wrap::AddHullShape(phys->body, &box.base, 0.0f, g_phys.friction, g_phys.restitution);
            continue;
        }
        // Wedge / slant: convex hull of the unit mesh scaled, rotated and placed like the instance.
        const std::vector<b3Vec3>& unit = ShapeUnitPoints(cb.shape);
        std::vector<b3Vec3> pts;
        pts.reserve(unit.size());
        for (const b3Vec3& u : unit) {
            const Vector3 s = { u.x * cb.half.x * 2.0f, u.y * cb.half.y * 2.0f, u.z * cb.half.z * 2.0f };
            const Vector3 w = Vector3Add(Vector3RotateByQuaternion(s, q), cb.center);
            pts.push_back({ w.x, w.y, w.z });
        }
        b3HullData* hull = pts.size() >= 4 ? b3CreateHull(pts.data(), (int)pts.size(), 64) : nullptr;
        if (hull) {
            b3wrap::AddHullShape(phys->body, hull, 0.0f, g_phys.friction, g_phys.restitution);
            phys->hulls.push_back(hull);
        } else {
            b3BoxHull box = b3MakeTransformedBoxHull(cb.half.x, cb.half.y, cb.half.z,
                                                     b3wrap::ToB3Transform(cb.center, q));
            b3wrap::AddHullShape(phys->body, &box.base, 0.0f, g_phys.friction, g_phys.restitution);
        }
    }

    if (!t.coll.surfIdx.empty()) {
        std::vector<b3Vec3> verts;
        verts.reserve(t.coll.surfVerts.size());
        for (const Vector3& v : t.coll.surfVerts) verts.push_back({ v.x, v.y, v.z });
        b3MeshDef md{};
        md.vertices = verts.data();
        md.indices = t.coll.surfIdx.data();
        md.vertexCount = (int)verts.size();
        md.triangleCount = (int)t.coll.surfIdx.size() / 3;
        md.weldTolerance = 0.001f;
        md.weldVertices = true;
        md.identifyEdges = true;
        if (b3MeshData* mesh = b3CreateMesh(&md, nullptr, 0)) {
            b3wrap::AddMeshShape(phys->body, mesh, b3Vec3_one, 0.0f, g_phys.friction, g_phys.restitution);
            phys->meshes.push_back(mesh);
        }
    }
    t.phys = std::move(phys);
}

void City::CreateAllTilePhysics() {
    for (auto& kv : tiles) CreateTilePhysics(kv.second);
}

void City::DestroyAllTilePhysics() {
    for (auto& kv : tiles) DestroyTilePhysics(kv.second);
}

void City::SetCollisionEnabled(bool on) {
    if (collisionEnabled == on) return;
    collisionEnabled = on;
    RebuildAll();
}

void AttachPhysicsWorld(b3WorldId world, float friction, float restitution) {
    g_phys.active = true;
    g_simActive = true;
    g_phys.world = world;
    g_phys.friction = friction;
    g_phys.restitution = restitution;
    for (City* c : GetCityRegistry().GetCities())
        if (c && c->alive) c->CreateAllTilePhysics();
}

void DetachPhysicsWorld() {
    for (City* c : GetCityRegistry().GetCities())
        if (c) c->DestroyAllTilePhysics();
    g_phys.active = false;
    g_simActive = false;
    for (City* c : GetCityRegistry().GetCities()) if (c) c->ClearAgents();
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

    // Moving agents (cars, pedestrians): dynamic instances rebuilt every frame. Far cars are plain boxes.
    if (!shadowPass) {
        const Matrix inv = MatrixInvert(rlGetMatrixModelview());
        trafficFocus = { inv.m12, inv.m13, inv.m14 };
        hasTrafficFocus = true;

        // Night light pools: the nearest street lamps (up to 24) and car headlights (the rest) light the
        // road, sidewalks and facades around them.
        std::vector<Vector4> lights;
        if (gfx::GetNightAmount() > 0.03f) {
            struct Cand { float d; Vector4 l; };
            std::vector<Cand> lamps;
            for (const Tile* t : visBldg)
                for (const Matrix& m : t->inst[kPropLamp]) {
                    const Vector3 head = Vector3Transform(Vector3{ 0.0f, 5.35f, 0.95f }, m);
                    const float d = Vector3Distance(head, trafficFocus);
                    if (d < 120.0f) lamps.push_back({ d, Vector4{ head.x, head.y, head.z, 15.0f } });
                }
            const size_t nl = std::min<size_t>(lamps.size(), 24);
            std::partial_sort(lamps.begin(), lamps.begin() + (long)nl, lamps.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
            for (size_t i = 0; i < nl; i++) lights.push_back(lamps[i].l);
            std::vector<Cand> cars;
            for (const Agent& a : agents) {
                if (!a.car || a.far || !a.placed) continue;
                const float d = Vector3Distance(a.pos, trafficFocus);
                if (d > 90.0f) continue;
                cars.push_back({ d, Vector4{ a.pos.x + cosf(a.yaw) * 3.6f, a.pos.y + 0.8f, a.pos.z - sinf(a.yaw) * 3.6f, 9.0f } });
            }
            const size_t nc = std::min<size_t>(cars.size(), 32 - lights.size());
            std::partial_sort(cars.begin(), cars.begin() + (long)nc, cars.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
            for (size_t i = 0; i < nc; i++) lights.push_back(cars[i].l);
        }
        gfx::SetNightLights(lights.data(), (int)lights.size());
    }
    // Traffic-signal heads (animated by the signal clock, also in the editor).
    {
        std::vector<Matrix> poles, lamps;
        for (int ni = 0; ni < (int)nodes.size(); ni++) {
            const RoadNode& nd = nodes[(size_t)ni];
            if (!nd.junction || nd.jkind != (int)JunctionKind::TrafficLight || (size_t)ni >= nodeEdges.size()) continue;
            for (int ei : nodeEdges[(size_t)ni]) {
                const RoadEdge& e = edges[(size_t)ei];
                const int other = e.a == ni ? e.b : e.a;
                const Vector2 armDir = Vector2Normalize(Vector2Subtract(nodes[(size_t)other].pos, nd.pos));
                if (armDir.x == 0.0f && armDir.y == 0.0f) continue;
                if (e.oneWay != 0 && !EdgeAllowsFrom(e, other)) continue;                 // no traffic arrives along this arm
                const Vector2 right = { -armDir.y, armDir.x };                              // right of an approaching driver
                const float sideW = std::max(EdgeSlabHalf(ei) - EdgeAsphaltHalf(ei), 1.2f) * 0.5f;
                const Vector2 pp = Vector2Add(Vector2Add(nd.pos, Vector2Scale(armDir, ArmClear(ei) + 4.8f)), Vector2Scale(right, (EdgeAsphaltHalf(ei) + sideW) * (params.leftHandTraffic ? -1.0f : 1.0f)));
                const float y = nd.h + kRoadElevation + 0.1f + e.curbH;
                const float yaw = atan2f(armDir.x, armDir.y);
                Matrix pm = MatrixMultiply(MatrixRotateY(yaw), MatrixTranslate(pp.x, y, pp.y));
                pm.m3 = -1.0f; pm.m7 = -1.0f; pm.m11 = -1.0f;
                poles.push_back(pm);
                const int st = SignalState(ni, ei);
                const Color on[3] = { { 255, 40, 40, 255 }, { 255, 190, 30, 255 }, { 50, 230, 80, 255 } };
                const float ly[3] = { 4.2f, 3.75f, 3.3f };
                for (int k = 0; k < 3; k++) {
                    const bool lit = (k == 0 && st == 0) || (k == 1 && st == 1) || (k == 2 && st == 2);
                    const Color c = lit ? on[k] : Color{ (unsigned char)(on[k].r / 7), (unsigned char)(on[k].g / 7), (unsigned char)(on[k].b / 7), 255 };
                    Matrix lm = MatrixMultiply(MatrixScale(0.26f, 0.26f, 0.12f),
                                MatrixMultiply(MatrixRotateY(yaw), MatrixTranslate(pp.x + sinf(yaw) * 0.24f, y + ly[k], pp.y + cosf(yaw) * 0.24f)));
                    lm.m3 = -std::max(c.r / 255.0f, 0.03f); lm.m7 = -std::max(c.g / 255.0f, 0.03f); lm.m11 = -std::max(c.b / 255.0f, 0.03f);
                    lamps.push_back(lm);
                }
            }
        }
        if (!poles.empty()) gfx::DrawCityInstances(gfx::GetCityShapeMesh(kPropSignal), poles, 0, (int)poles.size(), WHITE);
        if (!lamps.empty()) gfx::DrawCityInstances(gfx::GetCityShapeMesh(kBuildingBox), lamps, 0, (int)lamps.size(), WHITE);
    }

    if (!agents.empty()) {
        std::vector<Matrix> carM, boxM, pedM, blinkM, glassM, wheelM, busM, busGlassM;
        for (const Agent& a : agents) {
            if (!a.placed) continue;
            const Color shown = (a.car && a.bus < 0) ? PickCarColor(a) : a.color;
            const float cr = -std::max(shown.r / 255.0f, 0.05f), cg = -std::max(shown.g / 255.0f, 0.05f), cb = -std::max(shown.b / 255.0f, 0.05f);
            Matrix m;
            // Cars and buses tilt with the road (pitch about the car's own z axis, then turn to the heading).
            const Matrix carBase = MatrixMultiply(MatrixMultiply(MatrixRotateZ(CarPitch(a)), MatrixRotateY(a.yaw)), MatrixTranslate(a.pos.x, a.pos.y, a.pos.z));
            if (a.car && a.far) m = MatrixMultiply(MatrixScale(4.2f, 1.4f, 1.8f), MatrixMultiply(MatrixRotateY(a.yaw), MatrixTranslate(a.pos.x, a.pos.y + 0.7f, a.pos.z)));
            else m = carBase;
            m.m3 = cr; m.m7 = cg; m.m11 = cb;
            if (a.bus >= 0) {
                busM.push_back(m);
                Matrix gm = m; gm.m3 = -1.0f; gm.m7 = -1.0f; gm.m11 = -1.0f;
                busGlassM.push_back(gm);
                for (float lx : { -3.5f, 3.4f }) for (float lz : { -1.12f, 1.12f }) {
                    Matrix wm = MatrixMultiply(MatrixMultiply(MatrixMultiply(MatrixScale(1.4f, 1.4f, 1.4f), MatrixRotateZ(-a.wheelRot)), MatrixTranslate(lx, 0.45f, lz)), carBase);
                    wm.m3 = -1.0f; wm.m7 = -1.0f; wm.m11 = -1.0f;
                    wheelM.push_back(wm);
                }
                continue;
            }
            (a.car ? (a.far ? boxM : carM) : pedM).push_back(m);
            if (a.car && !a.far) {
                Matrix gm = m; gm.m3 = -1.0f; gm.m7 = -1.0f; gm.m11 = -1.0f;
                glassM.push_back(gm);
                for (float lx : { -1.3f, 1.3f }) for (float lz : { -0.92f, 0.92f }) {
                    Matrix wm = MatrixMultiply(MatrixMultiply(MatrixRotateZ(-a.wheelRot), MatrixTranslate(lx, 0.32f, lz)), carBase);
                    wm.m3 = -1.0f; wm.m7 = -1.0f; wm.m11 = -1.0f;
                    wheelM.push_back(wm);
                }
            }
            // Turn indicators: orange lamps at the front and rear corner on the turning side, blinking.
            if (a.car && !a.far && a.turn != 0 && fmodf(trafficClock, 0.7f) < 0.35f) {
                const float side = a.turn > 0 ? -0.86f : 0.86f;   // right of travel is -z in the car mesh
                for (float lx : { 2.0f, -2.0f }) {
                    // In the car's own (tilted) frame, so the lamps stay on the body on slopes.
                    Matrix bm = MatrixMultiply(MatrixMultiply(MatrixScale(0.22f, 0.2f, 0.34f), MatrixTranslate(lx, 0.75f, side)), carBase);
                    bm.m3 = -1.0f; bm.m7 = -0.6f; bm.m11 = -0.05f;
                    blinkM.push_back(bm);
                }
            }
        }
        if (!busM.empty()) gfx::DrawCityInstances(gfx::GetCityShapeMesh(kPropBus), busM, 0, (int)busM.size(), WHITE);
        if (!busGlassM.empty()) gfx::DrawCityInstances(gfx::GetCityShapeMesh(kPropBusGlass), busGlassM, 0, (int)busGlassM.size(), WHITE);
        if (!carM.empty()) gfx::DrawCityInstances(gfx::GetCityShapeMesh(kPropCar), carM, 0, (int)carM.size(), WHITE);
        if (!glassM.empty()) gfx::DrawCityInstances(gfx::GetCityShapeMesh(kPropCarGlass), glassM, 0, (int)glassM.size(), WHITE);
        if (!wheelM.empty()) gfx::DrawCityInstances(gfx::GetCityShapeMesh(kPropWheel), wheelM, 0, (int)wheelM.size(), WHITE);
        if (!boxM.empty()) gfx::DrawCityInstances(gfx::GetCityShapeMesh(kBuildingBox), boxM, 0, (int)boxM.size(), WHITE);
        if (!blinkM.empty()) gfx::DrawCityInstances(gfx::GetCityShapeMesh(kBuildingBox), blinkM, 0, (int)blinkM.size(), WHITE);
        if (!pedM.empty()) gfx::DrawCityInstances(gfx::GetCityShapeMesh(kPropPerson), pedM, 0, (int)pedM.size(), WHITE);
    }

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
    if (!state.panelOpen) return; // editor overlays (node markers etc.) only while the City panel is open

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
            MatrixTranslate(nodes[i].pos.x, nodes[i].h + h * 0.5f, nodes[i].pos.y));
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
        // Follow the road's height profile.
        const int selSeg = 12;
        for (int k = 0; k < selSeg; k++) {
            const float f0 = (float)k / selSeg, f1 = (float)(k + 1) / selSeg;
            const Vector2 pa = Vector2Lerp(nodes[e.a].pos, nodes[e.b].pos, f0), pb2 = Vector2Lerp(nodes[e.a].pos, nodes[e.b].pos, f1);
            DrawLine3D({ pa.x, EdgeProfileY(state.selectedEdge, f0) + 0.35f, pa.y },
                       { pb2.x, EdgeProfileY(state.selectedEdge, f1) + 0.35f, pb2.y }, Color{ 255, 220, 80, 255 });
        }
    }

    // Road tool: snap marker, start/handle, and the curve being drawn.
    if (state.activeCity == this && state.tool == CityTool::DrawRoad) {
        const float y = 0.45f;
        if (state.roadSnapValid)
            DrawSphere({ state.roadSnapPos.x, state.roadEndH + y, state.roadSnapPos.y }, std::max(params.RoadWidth() * 0.25f, 0.4f),
                       state.roadErase ? Color{ 235, 70, 60, 255 } : Color{ 255, 220, 80, 255 });
        if (state.roadActive) {
            const Color ok = state.roadPreviewOk ? Color{ 90, 220, 120, 255 } : Color{ 235, 70, 60, 255 };
            const float r = std::max(params.RoadWidth() * 0.25f, 0.4f);
            DrawSphere({ state.roadStart.x, state.roadStartH + y, state.roadStart.y }, r, ok);
            // Ramp from the start height to the end height along the sampled curve.
            float total = 0.0f;
            for (size_t i = 0; i + 1 < state.roadPreview.size(); i++)
                total += Vector2Distance(state.roadPreview[i], state.roadPreview[i + 1]);
            float run = 0.0f;
            auto yAt = [&](float d) { return state.roadStartH + (state.roadEndH - state.roadStartH) * (total > 1e-4f ? d / total : 0.0f) + y; };
            for (size_t i = 0; i + 1 < state.roadPreview.size(); i++) {
                const float seg = Vector2Distance(state.roadPreview[i], state.roadPreview[i + 1]);
                DrawLine3D({ state.roadPreview[i].x, yAt(run), state.roadPreview[i].y },
                           { state.roadPreview[i + 1].x, yAt(run + seg), state.roadPreview[i + 1].y }, ok);
                run += seg;
            }
            if (state.roadHasHandle) {
                DrawLine3D({ state.roadStart.x, state.roadStartH + y, state.roadStart.y },
                           { state.roadHandle.x, state.roadStartH + y, state.roadHandle.y }, Color{ 200, 200, 255, 255 });
                DrawSphere({ state.roadHandle.x, state.roadStartH + y, state.roadHandle.y }, r * 0.6f, Color{ 200, 200, 255, 255 });
            }
        }
    }

    // Insert tool: ghost footprint (green = can place, red = blocked).
    if (state.activeCity == this && state.tool == CityTool::InsertBuilding && state.insertHasPreview) {
        const PlacedBuilding& pb = state.insertPreview;
        const auto q = PlacedCorners(pb, 0.0f);
        const Color col = state.insertPreviewOk ? Color{ 90, 220, 120, 255 } : Color{ 235, 70, 60, 255 };
        for (int i = 0; i < 4; i++) {
            const Vector2 a = q[(size_t)i], b = q[(size_t)((i + 1) % 4)];
            DrawLine3D({ a.x, 0.5f, a.y }, { b.x, 0.5f, b.y }, col);
            DrawLine3D({ a.x, pb.height, a.y }, { b.x, pb.height, b.y }, col);
            DrawLine3D({ a.x, 0.5f, a.y }, { a.x, pb.height, a.y }, col);
        }
    }

    // Geometry problems: red outlines on steep / thin triangles, red vertical lines on steps and cracks.
    if (state.activeCity == this && state.showProblems && collisionEnabled) {
        const GeometryProblems& gp = GetGeometryProblemsCached();
        const Color red{ 255, 40, 40, 255 };
        for (const auto& t : gp.steep) for (int i = 0; i < 3; i++) DrawLine3D(Vector3Add(t[(size_t)i], { 0, 0.15f, 0 }), Vector3Add(t[(size_t)((i + 1) % 3)], { 0, 0.15f, 0 }), red);
        const Color orange{ 255, 150, 30, 255 };
        for (const auto& t : gp.thin) for (int i = 0; i < 3; i++) DrawLine3D(Vector3Add(t[(size_t)i], { 0, 0.15f, 0 }), Vector3Add(t[(size_t)((i + 1) % 3)], { 0, 0.15f, 0 }), orange);
        const Color magenta{ 255, 40, 255, 255 };
        for (const Vector3& p : gp.padOverRoad) {
            DrawLine3D(Vector3Add(p, { -0.8f, 0.4f, 0 }), Vector3Add(p, { 0.8f, 0.4f, 0 }), magenta);
            DrawLine3D(Vector3Add(p, { 0, 0.4f, -0.8f }), Vector3Add(p, { 0, 0.4f, 0.8f }), magenta);
        }
        for (const auto& st : gp.steps) {
            DrawLine3D(st.first, Vector3Add(st.second, { 0, 1.0f, 0 }), red);
            DrawCube(Vector3Add(st.second, { 0, 1.0f, 0 }), 0.5f, 0.5f, 0.5f, red);
        }
    }

    // Transit tool: stop pillars (coloured by the selected line) and the selected line's route as a polyline.
    if (state.activeCity == this && state.tool == CityTool::Transit) {
        const float top = BusStopMarkerHeight();
        std::vector<char> inLine(busStops.size(), 0);
        Color lc{ 255, 255, 255, 255 };
        if (state.transitLine >= 0 && (size_t)state.transitLine < busLines.size()) {
            const BusLine& L = busLines[(size_t)state.transitLine];
            lc = L.color;
            for (int si : L.stops) if ((size_t)si < busStops.size()) inLine[(size_t)si] = 1;
            for (size_t k = 0; k < L.stops.size(); k++) {
                const int a0 = L.stops[k], b0 = L.stops[(k + 1) % L.stops.size()];
                if ((size_t)a0 >= busStops.size() || (size_t)b0 >= busStops.size() || L.stops.size() < 2) continue;
                const Vector2 pa = busStops[(size_t)a0].pos, pb = busStops[(size_t)b0].pos;
                DrawLine3D({ pa.x, top * 0.55f, pa.y }, { pb.x, top * 0.55f, pb.y }, lc);
            }
        }
        for (int i = 0; i < (int)busStops.size(); i++) {
            const BusStop& bs = busStops[(size_t)i];
            Color c = bs.edge < 0 ? Color{ 200, 60, 60, 255 } : (inLine[(size_t)i] ? lc : Color{ 60, 200, 220, 255 });
            if (i == state.transitStop) c = Color{ 255, 230, 70, 255 };
            DrawLine3D({ bs.pos.x, 0.5f, bs.pos.y }, { bs.pos.x, top, bs.pos.y }, c);
            DrawCube({ bs.pos.x, top, bs.pos.y }, 1.8f, 1.8f, 1.8f, c);
            DrawLine3D({ bs.pos.x, top, bs.pos.y }, { bs.pos.x + bs.heading.x * 4.0f, top, bs.pos.y + bs.heading.y * 4.0f }, c);   // direction served
        }
    }

    // District tool: a ring per district at its radius, a pillar at its centre.
    if (state.activeCity == this && state.tool == CityTool::Districts) {
        for (int i = 0; i < (int)districts.size(); i++) {
            const District& d = districts[(size_t)i];
            static const Color kc[3] = { { 255, 170, 60, 255 }, { 110, 220, 120, 255 }, { 150, 170, 200, 255 } };
            Color c = kc[std::clamp(d.kind, 0, 2)];
            if (i != state.districtSel) c.a = 150;
            // Rises above the tallest buildings it creates (they would hide a short marker).
            const float top = DistrictMarkerHeight(d);
            DrawLine3D({ d.pos.x, 0.5f, d.pos.y }, { d.pos.x, top, d.pos.y }, c);
            DrawCube({ d.pos.x, top, d.pos.y }, 6.0f, 6.0f, 6.0f, c);
            for (int k = 0; k < 64; k++) {
                const float a0 = (float)k / 64.0f * 6.2831853f, a1 = (float)(k + 1) / 64.0f * 6.2831853f;
                DrawLine3D({ d.pos.x + cosf(a0) * d.radius, 0.6f, d.pos.y + sinf(a0) * d.radius },
                           { d.pos.x + cosf(a1) * d.radius, 0.6f, d.pos.y + sinf(a1) * d.radius }, c);
            }
        }
    }

    // Paint tool: outline the block under the cursor.
    if (state.activeCity == this && (state.tool == CityTool::PaintBlock || (state.tool == CityTool::Districts && state.districtPaintMode)) &&
        state.hoveredBlock >= 0 && (size_t)state.hoveredBlock < blocks.size()) {
        const auto& bn = blocks[(size_t)state.hoveredBlock].nodes;
        for (size_t i = 0; i < bn.size(); i++) {
            const Vector2 a = nodes[(size_t)bn[i]].pos, b = nodes[(size_t)bn[(i + 1) % bn.size()]].pos;
            DrawLine3D({ a.x, 0.4f, a.y }, { b.x, 0.4f, b.y }, Color{ 255, 220, 80, 255 });
        }
    }
}

// Tilt of a vehicle along the road: the slope between points a little ahead of and behind its centre, so the
// body follows the surface and the wheels meet it (inside a junction: the slope of its planned path).
float City::CarPitch(const Agent& a) const {
    if (!a.car || a.far || !a.placed) return 0.0f;
    if (a.inJ) return a.jlen > 0.5f ? atan2f(a.jy2 - a.jy0, a.jlen) : 0.0f;
    if (a.edge < 0 || (size_t)a.edge >= edges.size()) return 0.0f;
    const RoadEdge& e = edges[(size_t)a.edge];
    const float len = Vector2Distance(nodes[(size_t)e.a].pos, nodes[(size_t)e.b].pos);
    const float half = 0.45f * a.length;
    Vector3 p0, p1; Vector2 h0, h1;
    LanePose(a.edge, a.fwd, Clamp(a.s - half, 0.0f, len), a.laneF, true, p0, h0);
    LanePose(a.edge, a.fwd, Clamp(a.s + half, 0.0f, len), a.laneF, true, p1, h1);
    const float d = sqrtf((p1.x - p0.x) * (p1.x - p0.x) + (p1.z - p0.z) * (p1.z - p0.z));
    return d > 0.2f ? atan2f(p1.y - p0.y, d) : 0.0f;
}

bool City::FindSlopedCar(Vector3& pos, float& yaw, float& pitch, bool needBlinker) const {
    for (const Agent& a : agents) {
        if (!a.car || a.bus >= 0 || a.far || !a.placed) continue;
        if (needBlinker && !(a.turn != 0 && fmodf(trafficClock, 0.7f) < 0.35f)) continue;   // its turn signal is lit right now
        const float p = CarPitch(a);
        if (fabsf(p) > 0.05f) { pos = a.pos; yaw = a.yaw; pitch = p; return true; }
    }
    return false;
}

bool City::FindBus(int index, Vector3& pos, float& yaw) const {
    for (const Agent& a : agents) {
        if (a.bus < 0 || !a.placed) continue;
        if (index-- == 0) { pos = a.pos; yaw = a.yaw; return true; }
    }
    return false;
}

std::string City::DebugBuses() const {
    std::ostringstream o;
    for (size_t i = 0; i < agents.size(); i++) {
        const Agent& a = agents[i];
        if (a.bus < 0) continue;
        float len = 0.0f;
        if (a.edge >= 0 && (size_t)a.edge < edges.size()) len = Vector2Distance(nodes[(size_t)edges[(size_t)a.edge].a].pos, nodes[(size_t)edges[(size_t)a.edge].b].pos);
        o << "bus line " << a.bus << " edge " << a.edge << (a.fwd ? " fwd" : " bwd") << " s " << a.s << "/" << len << " v " << a.speed
          << " target " << a.busTarget << " dwell " << a.dwell << " stuck " << a.stuck << " inJ " << a.inJ << " next " << a.nextEdge << " lane " << a.lane << "\n";
    }
    return o.str();
}

void City::ComputeGeometryProblems(GeometryProblems& out) const {
    out = GeometryProblems{};
    std::vector<Vector3> v;
    DebugSurfaceTriangles(v);
    struct Key { int64_t x, y, z; bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; } };
    struct KeyHash { size_t operator()(const Key& k) const { return (size_t)(k.x * 73856093LL) ^ (size_t)(k.y * 19349663LL) ^ (size_t)(k.z * 83492791LL); } };
    auto q = [](const Vector3& p) { return Key{ (int64_t)llroundf(p.x * 50.0f), (int64_t)llroundf(p.y * 100.0f), (int64_t)llroundf(p.z * 50.0f) }; };   // 2 cm in x,z; 1 cm in y
    // Open edges: used by exactly one triangle.
    struct EKey { Key a, b; bool operator==(const EKey& o) const { return a == o.a && b == o.b; } };
    struct EHash { size_t operator()(const EKey& e) const { KeyHash h; return h(e.a) * 31u ^ h(e.b); } };
    std::unordered_map<EKey, int, EHash> edgeUse;
    std::unordered_map<EKey, std::pair<Vector3, Vector3>, EHash> edgePts;
    auto addEdge = [&](const Vector3& a, const Vector3& b) {
        Key ka = q(a), kb = q(b);
        if (kb.x < ka.x || (kb.x == ka.x && (kb.y < ka.y || (kb.y == ka.y && kb.z < ka.z)))) std::swap(ka, kb);
        const EKey ek{ ka, kb };
        edgeUse[ek]++;
        edgePts[ek] = { a, b };
    };
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        const Vector3 &p = v[i], &r = v[i + 1], &s = v[i + 2];
        const Vector3 n = Vector3CrossProduct(Vector3Subtract(r, p), Vector3Subtract(s, p));
        const float area3 = Vector3Length(n);
        if (area3 < 1e-6f) continue;
        const float ny = fabsf(n.y) / area3;
        const float areaXZ = 0.5f * fabsf((r.x - p.x) * (s.z - p.z) - (r.z - p.z) * (s.x - p.x));
        const float L = std::max({ Vector3Distance(p, r), Vector3Distance(r, s), Vector3Distance(p, s) });
        if (ny < 0.5f && 0.5f * area3 > 0.5f) out.steep.push_back({ p, r, s });
        if (L > 6.0f && areaXZ > 0.2f && areaXZ < 0.012f * L * L) out.thin.push_back({ p, r, s });
        addEdge(p, r); addEdge(r, s); addEdge(s, p);
    }
    // Steps: open-edge vertices at the same x,z with clearly different heights (layers sit within ~10 cm of each other).
    struct XZ { int64_t x, z; bool operator==(const XZ& o) const { return x == o.x && z == o.z; } };
    struct XZHash { size_t operator()(const XZ& k) const { return (size_t)(k.x * 73856093LL) ^ (size_t)(k.z * 83492791LL); } };
    std::unordered_map<XZ, std::pair<Vector3, Vector3>, XZHash> col;   // lowest and highest open-edge vertex per column
    for (const auto& kv : edgeUse) {
        if (kv.second != 1) continue;
        const auto& pts = edgePts[kv.first];
        for (const Vector3& p : { pts.first, pts.second }) {
            const XZ k{ (int64_t)llroundf(p.x * 50.0f), (int64_t)llroundf(p.z * 50.0f) };
            auto it = col.find(k);
            if (it == col.end()) col[k] = { p, p };
            else { if (p.y < it->second.first.y) it->second.first = p; if (p.y > it->second.second.y) it->second.second = p; }
        }
    }
    for (const auto& kv : col)
        if (kv.second.second.y - kv.second.first.y > 0.15f) out.steps.push_back(kv.second);

    // Pad over road: the road shader biases every layer toward the camera (about 2 m per layer unit), so a pad
    // (layer 0.06/0.08) hides the asphalt (layer 0.10/0.12) wherever it is more than ~8 cm higher than the asphalt.
    // Sample each pad triangle and look up the asphalt triangles over the same x,z.
    struct Tri { Vector3 a, b, c; float layer; };
    std::vector<Tri> pads, roads;
    for (const auto& kv : tiles) {
        const TileCollision& cl = kv.second.coll;
        if (cl.surfLayer.size() != cl.surfVerts.size()) continue;
        for (size_t i = 0; i + 2 < cl.surfIdx.size(); i += 3) {
            const size_t ia = (size_t)cl.surfIdx[i], ib = (size_t)cl.surfIdx[i + 1], ic = (size_t)cl.surfIdx[i + 2];
            const float lay = cl.surfLayer[ia];
            Tri t{ cl.surfVerts[ia], cl.surfVerts[ib], cl.surfVerts[ic], lay };
            if (fabsf(lay - 0.06f) < 0.011f || fabsf(lay - 0.08f) < 0.011f) pads.push_back(t);
            else if (fabsf(lay - 0.10f) < 0.011f || fabsf(lay - 0.12f) < 0.011f) roads.push_back(t);
        }
    }
    const float cell = 4.0f;
    std::unordered_map<int64_t, std::vector<int>> grid;
    auto cellKey = [&](int cx, int cz) { return ((int64_t)cx << 32) ^ (int64_t)(uint32_t)cz; };
    for (size_t i = 0; i < roads.size(); i++) {
        const Tri& t = roads[i];
        const int x0 = (int)floorf(std::min({ t.a.x, t.b.x, t.c.x }) / cell), x1 = (int)floorf(std::max({ t.a.x, t.b.x, t.c.x }) / cell);
        const int z0 = (int)floorf(std::min({ t.a.z, t.b.z, t.c.z }) / cell), z1 = (int)floorf(std::max({ t.a.z, t.b.z, t.c.z }) / cell);
        for (int cx = x0; cx <= x1; cx++) for (int cz = z0; cz <= z1; cz++) grid[cellKey(cx, cz)].push_back((int)i);
    }
    auto heightIn = [](const Tri& t, float x, float z, float& y) {
        const float d = (t.b.z - t.c.z) * (t.a.x - t.c.x) + (t.c.x - t.b.x) * (t.a.z - t.c.z);
        if (fabsf(d) < 1e-9f) return false;
        const float l1 = ((t.b.z - t.c.z) * (x - t.c.x) + (t.c.x - t.b.x) * (z - t.c.z)) / d;
        const float l2 = ((t.c.z - t.a.z) * (x - t.c.x) + (t.a.x - t.c.x) * (z - t.c.z)) / d;
        const float l3 = 1.0f - l1 - l2;
        if (l1 < 0.02f || l2 < 0.02f || l3 < 0.02f) return false;   // clearly inside, not on an edge
        y = l1 * t.a.y + l2 * t.b.y + l3 * t.c.y;
        return true;
    };
    for (const Tri& pt : pads) {
        const float areaXZ = 0.5f * fabsf((pt.b.x - pt.a.x) * (pt.c.z - pt.a.z) - (pt.b.z - pt.a.z) * (pt.c.x - pt.a.x));
        if (areaXZ < 0.5f) continue;
        const float w[4][3] = { { 1 / 3.f, 1 / 3.f, 1 / 3.f }, { 0.6f, 0.2f, 0.2f }, { 0.2f, 0.6f, 0.2f }, { 0.2f, 0.2f, 0.6f } };
        for (int k = 0; k < 4; k++) {
            const float x = pt.a.x * w[k][0] + pt.b.x * w[k][1] + pt.c.x * w[k][2];
            const float z = pt.a.z * w[k][0] + pt.b.z * w[k][1] + pt.c.z * w[k][2];
            const float yPad = pt.a.y * w[k][0] + pt.b.y * w[k][1] + pt.c.y * w[k][2];
            const auto it = grid.find(cellKey((int)floorf(x / cell), (int)floorf(z / cell)));
            if (it == grid.end()) continue;
            bool hit = false;
            for (int ri : it->second) {
                float yRoad;
                if (!heightIn(roads[(size_t)ri], x, z, yRoad)) continue;
                // effective drawn height = surface + ~2 m per layer unit of bias
                if (yPad + 2.0f * pt.layer > yRoad + 2.0f * roads[(size_t)ri].layer + 0.02f) { out.padOverRoad.push_back({ x, yPad, z }); out.padOverDelta.push_back((yPad + 2.0f * pt.layer) - (yRoad + 2.0f * roads[(size_t)ri].layer)); hit = true; break; }
            }
            if (hit) break;
        }
    }
}

const City::GeometryProblems& City::GetGeometryProblemsCached() {
    const double now = GetTime();
    if (now - problemsTime > 0.5) {
        ComputeGeometryProblems(problemsCache);
        problemsTime = now;
    }
    return problemsCache;
}

void City::DebugSurfaceLayers(std::vector<float>& out) const {
    out.clear();
    for (const auto& kv : tiles) {
        const TileCollision& c = kv.second.coll;
        for (size_t i = 0; i + 2 < c.surfIdx.size(); i += 3) out.push_back(c.surfLayer.size() == c.surfVerts.size() ? c.surfLayer[(size_t)c.surfIdx[i]] : -1.0f);
    }
}

void City::DebugSurfaceTriangles(std::vector<Vector3>& out) const {
    out.clear();
    for (const auto& kv : tiles) {
        const TileCollision& c = kv.second.coll;
        for (size_t i = 0; i + 2 < c.surfIdx.size(); i += 3) {
            out.push_back(c.surfVerts[(size_t)c.surfIdx[i]]);
            out.push_back(c.surfVerts[(size_t)c.surfIdx[i + 1]]);
            out.push_back(c.surfVerts[(size_t)c.surfIdx[i + 2]]);
        }
    }
}

int City::CountInstances(int shape) const {
    if (shape < 0 || shape >= kBuildingShapes) return 0;
    int n = 0;
    for (const auto& kv : tiles) n += (int)kv.second.inst[shape].size();
    return n;
}

bool City::FindInstance(int shape, int index, Vector3& pos, float& yaw) const {
    if (shape < 0 || shape >= kBuildingShapes || index < 0) return false;
    for (const auto& kv : tiles) {
        const auto& v = kv.second.inst[shape];
        if (index < (int)v.size()) {
            const Matrix& m = v[(size_t)index];
            pos = { m.m12, m.m13, m.m14 };
            yaw = atan2f(m.m8, m.m0);
            return true;
        }
        index -= (int)v.size();
    }
    return false;
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
    RoadNode nn{ pos, boundary };
    {   // keep the road's height profile exactly: the new node sits on it
        const Vector2 P = nodes[e.a].pos, Q = nodes[e.b].pos, d = Vector2Subtract(Q, P);
        const float l2 = d.x * d.x + d.y * d.y;
        const float t = l2 > 1e-8f ? Clamp(Vector2DotProduct(Vector2Subtract(pos, P), d) / l2, 0.0f, 1.0f) : 0.0f;
        nn.h = EdgeProfileY(edgeIndex, t);
    }
    nodes.push_back(nn);
    edges[edgeIndex].b = n;
    RoadEdge tail = e;
    tail.a = n;
    edges.push_back(tail);
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

City::RoadSnap City::SnapRoadPoint(const Vector2& p) const {
    RoadSnap r; r.pos = p;
    const float tol = std::max(params.RoadWidth() * 0.6f, 1.0f);
    int nn = -1; float nd = tol;
    for (int i = 0; i < (int)nodes.size(); i++) {
        const float d = Vector2Distance(nodes[(size_t)i].pos, p);
        if (d < nd) { nd = d; nn = i; }
    }
    if (nn >= 0) { r.node = nn; r.pos = nodes[(size_t)nn].pos; return r; }
    int ne = -1; float ed = tol; Vector2 eq{};
    for (int i = 0; i < (int)edges.size(); i++) {
        const Vector2 P = nodes[(size_t)edges[(size_t)i].a].pos, Q = nodes[(size_t)edges[(size_t)i].b].pos;
        const Vector2 s = Vector2Subtract(Q, P);
        const float l2 = s.x * s.x + s.y * s.y;
        if (l2 < 1e-8f) continue;
        const float t = Clamp(Vector2DotProduct(Vector2Subtract(p, P), s) / l2, 0.0f, 1.0f);
        const Vector2 q = Vector2Add(P, Vector2Scale(s, t));
        const float d = Vector2Distance(q, p);
        if (d < ed) { ed = d; ne = i; eq = q; }
    }
    if (ne >= 0) {
        // On a road, but practically at its end: use that node.
        const int ea = edges[(size_t)ne].a, eb = edges[(size_t)ne].b;
        if (Vector2Distance(eq, nodes[(size_t)ea].pos) < tol) { r.node = ea; r.pos = nodes[(size_t)ea].pos; }
        else if (Vector2Distance(eq, nodes[(size_t)eb].pos) < tol) { r.node = eb; r.pos = nodes[(size_t)eb].pos; }
        else { r.edge = ne; r.pos = eq; }
    }
    return r;
}

int City::AddRoadPath(const std::vector<Vector2>& pts, float hStart, float hEnd) {
    if (pts.size() < 2) return -1;
    const float minSep = params.RoadWidth();

    // Drop interior samples too close to the previous kept point or the end, so
    // junction plates of neighbouring nodes never overlap.
    std::vector<Vector2> path;
    path.push_back(pts.front());
    for (size_t i = 1; i + 1 < pts.size(); i++)
        if (Vector2Distance(pts[i], path.back()) >= minSep &&
            Vector2Distance(pts[i], pts.back()) >= minSep) path.push_back(pts[i]);
    path.push_back(pts.back());
    if (Vector2Distance(path.front(), path.back()) < 1e-3f) return -1;

    // Resolve an endpoint to a node (splitting a road or creating a node as needed).
    // A node that already exists keeps its own height; a new one takes `h`.
    auto resolve = [&](const Vector2& p, float h) {
        const RoadSnap s = SnapRoadPoint(p);
        if (s.node >= 0) return s.node;
        if (s.edge >= 0) return SplitEdge(s.edge, s.pos);
        RoadNode nn{ p, false };
        nn.h = h;
        nodes.push_back(nn);
        return (int)nodes.size() - 1;
    };
    const int first = resolve(path.front(), hStart);
    const int last = resolve(path.back(), hEnd);
    const float h0 = nodes[(size_t)first].h, h1 = nodes[(size_t)last].h;
    float total = 0.0f;
    for (size_t i = 1; i < path.size(); i++) total += Vector2Distance(path[i - 1], path[i]);
    int prev = first;
    float run = 0.0f;
    for (size_t i = 1; i < path.size(); i++) {
        run += Vector2Distance(path[i - 1], path[i]);
        int cur;
        if (i + 1 == path.size()) cur = last;
        else {
            RoadNode nn{ path[i], false };
            nn.h = h0 + (h1 - h0) * (total > 1e-4f ? run / total : 0.0f);
            nodes.push_back(nn);
            cur = (int)nodes.size() - 1;
        }
        if (cur == prev) continue;
        ConnectWithSplits(prev, cur);
        prev = cur;
    }
    RebuildAll();
    return prev;
}

void City::DeleteRoadChain(int edgeIndex) {
    if (edgeIndex < 0 || (size_t)edgeIndex >= edges.size()) return;
    std::vector<int> deg(nodes.size(), 0);
    for (const auto& e : edges) { deg[(size_t)e.a]++; deg[(size_t)e.b]++; }
    std::vector<char> kill(edges.size(), 0);
    kill[(size_t)edgeIndex] = 1;
    // Walk outward from each end through degree-2 nodes.
    for (int dir = 0; dir < 2; dir++) {
        int node = dir == 0 ? edges[(size_t)edgeIndex].a : edges[(size_t)edgeIndex].b;
        int via = edgeIndex;
        while (deg[(size_t)node] == 2) {
            int next = -1;
            for (int i = 0; i < (int)edges.size(); i++)
                if (i != via && (edges[(size_t)i].a == node || edges[(size_t)i].b == node)) { next = i; break; }
            if (next < 0 || kill[(size_t)next]) break;
            kill[(size_t)next] = 1;
            node = edges[(size_t)next].a == node ? edges[(size_t)next].b : edges[(size_t)next].a;
            via = next;
        }
    }
    std::vector<RoadEdge> kept;
    for (int i = 0; i < (int)edges.size(); i++) if (!kill[(size_t)i]) kept.push_back(edges[(size_t)i]);
    edges = std::move(kept);

    // Remove nodes with no roads left and remap indices.
    std::vector<int> remap(nodes.size(), -1), used(nodes.size(), 0);
    for (const auto& e : edges) { used[(size_t)e.a] = 1; used[(size_t)e.b] = 1; }
    std::vector<RoadNode> newNodes;
    for (size_t i = 0; i < nodes.size(); i++)
        if (used[i]) { remap[i] = (int)newNodes.size(); newNodes.push_back(nodes[i]); }
    nodes = std::move(newNodes);
    for (auto& e : edges) { e.a = remap[(size_t)e.a]; e.b = remap[(size_t)e.b]; }
    RebuildAll();
}

int City::InsertNodeOnEdge(int edgeIndex, const Vector2& pos) {
    if (edgeIndex < 0 || (size_t)edgeIndex >= edges.size()) return -1;
    const RoadEdge e = edges[edgeIndex];
    const bool boundary = nodes[e.a].boundary && nodes[e.b].boundary;
    const int n = SplitEdge(edgeIndex, pos);
    (void)boundary; (void)e;
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

float City::EdgeAsphaltHalf(int ei) const {
    if (ei < 0 || (size_t)ei >= edges.size()) return params.lanes * params.laneWidth * 0.5f;
    const RoadEdge& e = edges[(size_t)ei];
    const int lanes = e.lanes > 0 ? e.lanes : params.lanes;
    const float lw = e.width > 0.0f ? e.width : params.laneWidth;
    return lanes * lw * 0.5f;
}

float City::EdgeSlabHalf(int ei) const {
    const float sw = (ei >= 0 && (size_t)ei < edges.size() && edges[(size_t)ei].sidewalk >= 0.0f)
                         ? edges[(size_t)ei].sidewalk : params.sidewalk;
    return EdgeAsphaltHalf(ei) + sw;
}

int City::EdgeBetween(int a, int b) const {
    if (a >= 0 && (size_t)a < nodeEdges.size()) {
        for (int ei : nodeEdges[(size_t)a]) {
            const RoadEdge& e = edges[(size_t)ei];
            if ((e.a == a && e.b == b) || (e.a == b && e.b == a)) return ei;
        }
        return -1;
    }
    for (int k = 0; k < (int)edges.size(); k++)
        if ((edges[(size_t)k].a == a && edges[(size_t)k].b == b) || (edges[(size_t)k].a == b && edges[(size_t)k].b == a)) return k;
    return -1;
}

float City::BlockRoadHalf(const Block& block) const {
    float half = params.RoadWidth() * 0.5f;
    bool any = false;
    float widest = 0.0f;
    const int n = (int)block.nodes.size();
    for (int i = 0; i < n; i++) {
        const int ei = EdgeBetween(block.nodes[(size_t)i], block.nodes[(size_t)((i + 1) % n)]);
        if (ei < 0) continue;
        any = true;
        widest = std::max(widest, EdgeSlabHalf(ei));
    }
    return any ? widest : half;
}

void City::EdgePlateau(int ei, float len, float& atA, float& atB) const {
    const RoadEdge& e = edges[(size_t)ei];
    const float slabHalf = EdgeSlabHalf(ei);
    const float base = slabHalf + std::max(params.cornerRadius, 0.0f) + 1.5f;
    float lenAt[2] = { base, base };
    for (int end = 0; end < 2 && nodeEdges.size() == nodes.size(); end++) {
        const int n = end == 0 ? e.a : e.b, other = end == 0 ? e.b : e.a;
        const Vector2 mine = Vector2Normalize(Vector2Subtract(nodes[(size_t)other].pos, nodes[(size_t)n].pos));
        float minAngle = PI;
        for (int ej : nodeEdges[(size_t)n]) {
            if (ej == ei) continue;
            const RoadEdge& f = edges[(size_t)ej];
            const int o2 = f.a == n ? f.b : f.a;
            const Vector2 theirs = Vector2Normalize(Vector2Subtract(nodes[(size_t)o2].pos, nodes[(size_t)n].pos));
            minAngle = std::min(minAngle, acosf(Clamp(Vector2DotProduct(mine, theirs), -1.0f, 1.0f)));
        }
        // Two roads meeting at angle a: the corner of their edges is slabHalf / tan(a/2) from the node along the arm,
        // so the strip's end cut reaches that far when the junction is sharp.
        if (minAngle < PI * 0.5f) lenAt[end] = std::max(base, slabHalf / std::max(tanf(std::max(minAngle, 0.35f) * 0.5f), 0.05f) + 1.5f);
    }
    atA = std::min(0.45f, lenAt[0] / len);
    atB = std::min(0.45f, lenAt[1] / len);
}

float City::EdgeRampU(int ei, float s) const {
    if (ei < 0 || (size_t)ei >= edges.size()) return 0.0f;
    const RoadEdge& e = edges[(size_t)ei];
    const float len = Vector2Distance(nodes[(size_t)e.a].pos, nodes[(size_t)e.b].pos);
    if (len < 1e-3f) return 0.0f;
    // Flat across each junction (plates and the strip ends meet at exactly the node height),
    // eased ramp in between.
    float pa, pb;
    EdgePlateau(ei, len, pa, pb);
    const float u = Clamp((s - pa) / std::max(1.0f - pa - pb, 1e-3f), 0.0f, 1.0f);
    return u * u * (3.0f - 2.0f * u);
}

float City::EdgeProfileY(int ei, float s) const {
    if (ei < 0 || (size_t)ei >= edges.size()) return 0.0f;
    const RoadEdge& e = edges[(size_t)ei];
    const float hA = nodes[(size_t)e.a].h, hB = nodes[(size_t)e.b].h;
    if (hA == hB) return hA;
    return hA + (hB - hA) * EdgeRampU(ei, s);
}

std::vector<float> City::EdgeStations(int ei) const {
    std::vector<float> st{ 0.0f };
    if (ei >= 0 && (size_t)ei < edges.size()) {
        const RoadEdge& e = edges[(size_t)ei];
        const float len = Vector2Distance(nodes[(size_t)e.a].pos, nodes[(size_t)e.b].pos);
        if ((nodes[(size_t)e.a].h != nodes[(size_t)e.b].h || e.bank != 0.0f) && len > 1e-3f) {
            const int steps = std::max(1, (int)ceilf(len / kProfileStep));
            float plateauA, plateauB;
            EdgePlateau(ei, len, plateauA, plateauB);
            std::vector<float> v;
            for (int k = 1; k < steps; k++) v.push_back((float)k / (float)steps);
            v.push_back(plateauA);          // the surface is exactly level up to here (junction plateau)
            v.push_back(1.0f - plateauB);
            std::sort(v.begin(), v.end());
            for (float x : v)
                if (x > st.back() + 1e-3f && x < 1.0f - 1e-3f) st.push_back(x);
        }
    }
    st.push_back(1.0f);
    return st;
}

float City::EdgeSurfaceY(int ei, float s) const {
    const std::vector<float> st = EdgeStations(ei);
    size_t i = 0;
    while (i + 2 < st.size() && s > st[i + 1]) i++;
    const float span = st[i + 1] - st[i];
    const float f = span > 1e-6f ? Clamp((s - st[i]) / span, 0.0f, 1.0f) : 0.0f;
    const float y0 = EdgeProfileY(ei, st[i]), y1 = EdgeProfileY(ei, st[i + 1]);
    return y0 + (y1 - y0) * f;
}

void City::SetNodeJunctionKind(int index, int kind) {
    if (index < 0 || (size_t)index >= nodes.size()) return;
    nodes[(size_t)index].jkind = Clamp(kind, 0, 4);
    RebuildAll();
}

void City::SetNodeHeight(int index, float h) {
    if (index < 0 || (size_t)index >= nodes.size()) return;
    nodes[(size_t)index].h = Clamp(h, -200.0f, 500.0f);
    RebuildAll();
}

void City::SetEdgeProps(int edgeIndex, const RoadEdge& pr) {
    if (edgeIndex < 0 || (size_t)edgeIndex >= edges.size()) return;
    RoadEdge& e = edges[(size_t)edgeIndex];
    e.lanes = Clamp(pr.lanes, 0, 8);
    e.width = Clamp(pr.width, 0.0f, 12.0f);
    e.sidewalk = pr.sidewalk < 0.0f ? -1.0f : Clamp(pr.sidewalk, 0.0f, 12.0f);
    e.type = Clamp(pr.type, 0, 4);
    e.bridge = pr.bridge;
    e.curbH = Clamp(pr.curbH, 0.0f, 1.0f);
    e.markings = pr.markings;
    e.bank = Clamp(pr.bank, -20.0f, 20.0f);
    e.oneWay = Clamp(pr.oneWay, 0, 2);
    e.speedLimit = Clamp(pr.speedLimit, 0.0f, 60.0f);
    RebuildAll();
}

void City::GetRoadChain(int edgeIndex, std::vector<int>& outEdges, std::vector<int>& outNodes) const {
    outEdges.clear();
    outNodes.clear();
    if (edgeIndex < 0 || (size_t)edgeIndex >= edges.size()) return;
    std::vector<int> deg(nodes.size(), 0);
    for (const auto& e : edges) { deg[(size_t)e.a]++; deg[(size_t)e.b]++; }
    auto otherEnd = [&](int ei, int node) { return edges[(size_t)ei].a == node ? edges[(size_t)ei].b : edges[(size_t)ei].a; };
    // Walks outward from `node` (reached via edge `via`) through degree-2 nodes.
    auto walk = [&](int node, int via, std::vector<int>& out) {
        for (int guard = 0; guard < (int)edges.size() && deg[(size_t)node] == 2; guard++) {
            int next = -1;
            for (int i = 0; i < (int)edges.size(); i++)
                if (i != via && (edges[(size_t)i].a == node || edges[(size_t)i].b == node)) { next = i; break; }
            if (next < 0 || next == edgeIndex) break;
            out.push_back(next);
            node = otherEnd(next, node);
            via = next;
        }
        return node;
    };
    std::vector<int> back, fwd;
    const int endA = walk(edges[(size_t)edgeIndex].a, edgeIndex, back);
    walk(edges[(size_t)edgeIndex].b, edgeIndex, fwd);
    std::reverse(back.begin(), back.end());
    outEdges = back;
    outEdges.push_back(edgeIndex);
    outEdges.insert(outEdges.end(), fwd.begin(), fwd.end());
    int cur = endA;
    outNodes.push_back(cur);
    for (int ei : outEdges) { cur = otherEnd(ei, cur); outNodes.push_back(cur); }
}

void City::SmoothRoadHeights(int edgeIndex, int iterations) {
    std::vector<int> ce, cn;
    GetRoadChain(edgeIndex, ce, cn);
    if (cn.size() < 3) return;
    for (int it = 0; it < iterations; it++) {
        std::vector<float> nh(cn.size());
        for (size_t i = 0; i < cn.size(); i++) nh[i] = nodes[(size_t)cn[i]].h;
        for (size_t i = 1; i + 1 < cn.size(); i++)
            nh[i] = 0.25f * nodes[(size_t)cn[i - 1]].h + 0.5f * nodes[(size_t)cn[i]].h + 0.25f * nodes[(size_t)cn[i + 1]].h;
        for (size_t i = 1; i + 1 < cn.size(); i++) nodes[(size_t)cn[i]].h = nh[i];
    }
    RebuildAll();
}

void City::SetRoadGrade(int edgeIndex, int fromNode, float gradePercent) {
    std::vector<int> ce, cn;
    GetRoadChain(edgeIndex, ce, cn);
    if (cn.size() < 2) return;
    if (cn.back() == fromNode) std::reverse(cn.begin(), cn.end());
    float h = nodes[(size_t)cn[0]].h;
    for (size_t i = 1; i < cn.size(); i++) {
        const float len = Vector2Distance(nodes[(size_t)cn[i - 1]].pos, nodes[(size_t)cn[i]].pos);
        h += len * gradePercent * 0.01f;
        nodes[(size_t)cn[i]].h = Clamp(h, -200.0f, 500.0f);
    }
    RebuildAll();
}

int City::PickNode(const Ray& ray, float tolerance, float* outDist) const {
    int best = -1;
    float bestDist = 1e30f;
    for (int i = 0; i < (int)nodes.size(); i++) {
        Vector3 c = { nodes[i].pos.x, nodes[i].h + 0.04f, nodes[i].pos.y };
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
Vector3 a = { nodes[edges[i].a].pos.x, nodes[edges[i].a].h + 0.13f + kRoadElevation, nodes[edges[i].a].pos.y };
        Vector3 b = { nodes[edges[i].b].pos.x, nodes[edges[i].b].h + 0.13f + kRoadElevation, nodes[edges[i].b].pos.y };
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

void City::SetBlockKind(uint64_t blockId, BlockKind kind) {
    if (blockId == 0) return;
    if (GetBlockKind(blockId) == kind) return;
    if (kind == BlockKind::Auto) blockKinds.erase(blockId);
    else blockKinds[blockId] = kind;
    RebuildAll();
}

District City::MakeDistrict(DistrictKind kind, const Vector2& pos) {
    District d;
    d.pos = pos;
    d.kind = (int)kind;
    switch (kind) {
        case DistrictKind::Downtown:   d.peak = 3.0f; d.style = 0; d.radius = 120.0f; d.name = "Downtown"; break;
        case DistrictKind::Suburb:     d.peak = 1.0f; d.style = 3; d.radius = 140.0f; d.name = "Suburb"; break;
        case DistrictKind::Industrial: d.peak = 1.0f; d.style = 2; d.radius = 100.0f; d.name = "Industrial"; break;
    }
    return d;
}

float City::DistrictMarkerHeight(const District& d) const {
    return params.avgHeight * (1.0f + std::max(0.0f, d.peak)) * 1.3f + 25.0f;
}

int City::PickDistrict(const Ray& ray) const {
    int best = -1;
    float bestT = 1e30f;
    for (int i = 0; i < (int)districts.size(); i++) {
        const District& d = districts[(size_t)i];
        const float top = DistrictMarkerHeight(d);
        // Closest approach of the ray to the pillar (a vertical segment from the ground to the top cube).
        const Vector3 p0 = { d.pos.x, 0.5f, d.pos.y }, p1 = { d.pos.x, top, d.pos.y };
        const Vector3 u = Vector3Subtract(p1, p0), w0 = Vector3Subtract(ray.position, p0);
        const float a = Vector3DotProduct(u, u), b = Vector3DotProduct(u, ray.direction), c = Vector3DotProduct(ray.direction, ray.direction);
        const float dd = Vector3DotProduct(u, w0), e = Vector3DotProduct(ray.direction, w0);
        const float den = a * c - b * b;
        // w0 = ray origin - pillar base, so: s = (c*dd - b*e) / den, t = (b*s - e) / c.
        float sc = den > 1e-6f ? (c * dd - b * e) / den : 0.0f;   // along the pillar
        sc = Clamp(sc, 0.0f, 1.0f);
        const float tc = std::max(0.0f, (b * sc - e) / c);       // along the ray
        const Vector3 onPillar = Vector3Add(p0, Vector3Scale(u, sc));
        const Vector3 onRay = Vector3Add(ray.position, Vector3Scale(ray.direction, tc));
        const float dist = Vector3Distance(onPillar, onRay);
        const float tol = 2.0f + tc * 0.012f;                    // a little forgiving farther away
        const float topTol = 6.0f + tc * 0.015f;                 // the cube at the top is the easy target
        const bool hit = dist < (sc > 0.95f ? topTol : tol);
        if (hit && tc < bestT) { bestT = tc; best = i; }
    }
    return best;
}

int City::AddDistrict(const District& d) {
    districts.push_back(d);
    districts.back().radius = std::max(districts.back().radius, 1.0f);
    RebuildAll();
    return (int)districts.size() - 1;
}

void City::UpdateDistrict(int index, const District& d) {
    if (index < 0 || (size_t)index >= districts.size()) return;
    districts[(size_t)index] = d;
    districts[(size_t)index].radius = std::max(d.radius, 1.0f);
    RebuildAll();
}

void City::MoveDistrict(int index, const Vector2& pos, bool rebuild) {
    if (index < 0 || (size_t)index >= districts.size()) return;
    districts[(size_t)index].pos = pos;
    if (rebuild) RebuildAll();
}

void City::RemoveDistrict(int index) {
    if (index < 0 || (size_t)index >= districts.size()) return;
    districts.erase(districts.begin() + index);
    for (auto it = blockDistricts.begin(); it != blockDistricts.end();) {
        if (it->second == index) it = blockDistricts.erase(it);
        else { if (it->second > index) it->second--; ++it; }
    }
    RebuildAll();
}

void City::AutoDistricts(float peak) {
    districts.clear();
    blockDistricts.clear();
    if (nodes.empty()) { RebuildAll(); return; }
    Vector2 lo = nodes[0].pos, hi = nodes[0].pos;
    for (const RoadNode& n : nodes) {
        lo.x = std::min(lo.x, n.pos.x); lo.y = std::min(lo.y, n.pos.y);
        hi.x = std::max(hi.x, n.pos.x); hi.y = std::max(hi.y, n.pos.y);
    }
    District d = MakeDistrict(DistrictKind::Downtown, { (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f });
    d.peak = peak;
    d.radius = std::max(40.0f, 0.5f * std::max(hi.x - lo.x, hi.y - lo.y) * 1.1f);   // fades out near the city edge
    d.style = -1;                                                                   // keep the city's own style
    districts.push_back(d);
    RebuildAll();
}

void City::PaintBlockDistrict(uint64_t blockId, int district) {
    if (district < 0 || (size_t)district >= districts.size()) blockDistricts.erase(blockId);
    else blockDistricts[blockId] = district;
    RebuildAll();
}

int City::GetBlockDistrict(uint64_t blockId) const {
    const auto it = blockDistricts.find(blockId);
    return it == blockDistricts.end() ? -1 : it->second;
}

void City::DistrictAt(const Vector2& p, uint64_t blockId, float& heightMul, int& style) const {
    heightMul = 1.0f;
    style = -1;
    float best = 0.0f;
    const auto apply = [&](const District& d, float w) {
        const float t = w * w * (3.0f - 2.0f * w);   // smoothstep: flat at the centre and at the rim
        heightMul += (d.peak - 1.0f) * t;
        if (d.style >= 0 && w > best) { best = w; style = d.style; }
    };
    const auto paint = blockDistricts.find(blockId);
    if (paint != blockDistricts.end() && (size_t)paint->second < districts.size()) {
        // Painted: the block takes the district's peak and style outright.
        apply(districts[(size_t)paint->second], 1.0f);
        heightMul = std::max(heightMul, 0.15f);
        return;
    }
    for (const District& d : districts) {
        const float w = 1.0f - Vector2Distance(p, d.pos) / std::max(d.radius, 1.0f);
        if (w > 0.0f) apply(d, std::min(w, 1.0f));
    }
    heightMul = std::max(heightMul, 0.15f);
}

BlockKind City::GetBlockKind(uint64_t blockId) const {
    const auto it = blockKinds.find(blockId);
    return it == blockKinds.end() ? BlockKind::Auto : it->second;
}

int City::PickBlockAt(const Vector2& p) const {
    for (int bi = 0; bi < (int)blocks.size(); bi++) {
        std::vector<Vector2> poly;
        poly.reserve(blocks[(size_t)bi].nodes.size());
        for (int idx : blocks[(size_t)bi].nodes) poly.push_back(nodes[(size_t)idx].pos);
        if (citygeom::PointInPolygon(p, poly)) return bi;
    }
    return -1;
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
    // Painted block kinds (optional trailing section; older readers/files skip it).
    out << "KINDS " << blockKinds.size() << '\n';
    for (const auto& kv : blockKinds) out << kv.first << ' ' << (int)kv.second << '\n';
    out << "COLL " << (collisionEnabled ? 1 : 0) << '\n';
    out << "PLACED " << placed.size() << '\n';
    for (const auto& pb : placed)
        out << pb.center.x << ' ' << pb.center.y << ' ' << pb.sizeX << ' ' << pb.sizeZ << ' '
            << pb.height << ' ' << pb.angleY << ' ' << pb.colorBucket << '\n';
    {
        size_t shapeCount = 0;
        for (const auto& pb : placed) if (pb.shape != 0) shapeCount++;
        if (shapeCount) {
            out << "PSHAPE " << shapeCount << '\n';
            for (size_t i = 0; i < placed.size(); i++) if (placed[i].shape != 0) out << i << ' ' << placed[i].shape << '\n';
        }
    }
    {
        size_t freeCount = 0;
        for (const auto& pb : placed) if (pb.free) freeCount++;
        if (freeCount) {
            out << "FREEP " << freeCount;
            for (size_t i = 0; i < placed.size(); i++) if (placed[i].free) out << ' ' << i;
            out << '\n';
        }
    }
    // Road elevation + per-road properties (optional trailing sections; only non-default data).
    bool anyH = false;
    for (const auto& nd : nodes) if (nd.h != 0.0f) { anyH = true; break; }
    if (anyH) {
        out << "NHEIGHT " << nodes.size() << '\n';
        for (const auto& nd : nodes) out << nd.h << '\n';
    }
    size_t propCount = 0;
    auto hasProps = [](const RoadEdge& e) {
        return e.width != 0.0f || e.sidewalk >= 0.0f || e.type != 0 || e.bridge || e.curbH != 0.0f || e.markings || e.bank != 0.0f;
    };
    for (const auto& e : edges) if (hasProps(e)) propCount++;
    if (propCount) {
        out << "EDGEP " << propCount << '\n';
        for (size_t i = 0; i < edges.size(); i++) {
            const RoadEdge& e = edges[i];
            if (!hasProps(e)) continue;
            out << i << ' ' << e.width << ' ' << e.sidewalk << ' ' << e.type << ' ' << (e.bridge ? 1 : 0) << ' '
                << e.curbH << ' ' << (e.markings ? 1 : 0) << ' ' << e.bank << '\n';
        }
    }
    {
        size_t ns = 0;
        for (const auto& nd : nodes) if (!nd.phases.empty() || nd.yellow != 1.5f || nd.allRed != 1.0f || nd.sigOffset != 0.0f) ns++;
        if (ns) {
            out << "SIGNALS " << ns << '\n';
            for (size_t i = 0; i < nodes.size(); i++) {
                const RoadNode& nd = nodes[i];
                if (nd.phases.empty() && nd.yellow == 1.5f && nd.allRed == 1.0f && nd.sigOffset == 0.0f) continue;
                out << i << ' ' << nd.yellow << ' ' << nd.allRed << ' ' << nd.sigOffset << ' ' << nd.phases.size();
                for (const auto& ph : nd.phases) out << ' ' << ph.duration << ' ' << (int)ph.mask;
                out << '\n';
            }
        }
        size_t nt = 0;
        for (const auto& e : edges) if (e.speedLimit != 0.0f || e.turnA != 0 || e.turnB != 0) nt++;
        if (nt) {
            out << "EDGET " << nt << '\n';
            for (size_t i = 0; i < edges.size(); i++) {
                const RoadEdge& e = edges[i];
                if (e.speedLimit == 0.0f && e.turnA == 0 && e.turnB == 0) continue;
                out << i << ' ' << e.speedLimit << ' ' << e.turnA << ' ' << e.turnB << '\n';
            }
        }
    }
    {
        size_t nr = 0;
        for (const auto& nd : nodes) if (nd.rbIsland != 0.0f || nd.rbRing != 0.0f || !nd.rbSplitters || nd.rbConcrete) nr++;
        if (nr) {
            out << "RBP " << nr << '\n';
            for (size_t i = 0; i < nodes.size(); i++) {
                const RoadNode& nd = nodes[i];
                if (nd.rbIsland == 0.0f && nd.rbRing == 0.0f && nd.rbSplitters && !nd.rbConcrete) continue;
                out << i << ' ' << nd.rbIsland << ' ' << nd.rbRing << ' ' << (nd.rbSplitters ? 1 : 0) << ' ' << (nd.rbConcrete ? 1 : 0) << '\n';
            }
        }
    }
    if (params.shortChance != 0.15f || params.footprintVariety != 0.15f)
        out << "VARY " << params.shortChance << ' ' << params.footprintVariety << '\n';
    if (params.style != 0 || params.shapeVariety != 0.5f)
        out << "STYLE " << params.style << ' ' << params.shapeVariety << '\n';
    if (!params.furniture || params.cars != 0 || params.pedestrians != 0 || params.trafficDetailDistance != 150.0f || params.leftHandTraffic)
        out << "LIFE " << (params.furniture ? 1 : 0) << ' ' << params.cars << ' ' << params.pedestrians << ' ' << params.trafficDetailDistance << ' ' << (params.leftHandTraffic ? 1 : 0) << '\n';
    {
        const CityParams def;
        bool same = params.carColorAll == def.carColorAll && params.carColors.size() == def.carColors.size();
        for (size_t i = 0; same && i < def.carColors.size(); i++) {
            const CarColor &a = params.carColors[i], &b = def.carColors[i];
            same = a.name == b.name && a.weight == b.weight && a.other == b.other && a.color.r == b.color.r && a.color.g == b.color.g && a.color.b == b.color.b;
        }
        if (!same) {
            out << "CARCOL " << (params.carColorAll ? 1 : 0) << ' ' << params.carColors.size() << '\n';
            for (const CarColor& c : params.carColors) {
                std::string nm = c.name.empty() ? "-" : c.name;
                for (char& ch : nm) if (ch == ' ' || ch == '\n' || ch == '\t') ch = '_';
                out << (int)c.color.r << ' ' << (int)c.color.g << ' ' << (int)c.color.b << ' ' << c.weight << ' ' << (c.other ? 1 : 0) << ' ' << nm << '\n';
            }
        }
    }
    {
        size_t nk = 0;
        for (const auto& nd : nodes) if (nd.jkind != 0) nk++;
        if (nk) {
            out << "NODEP " << nk << '\n';
            for (size_t i = 0; i < nodes.size(); i++) if (nodes[i].jkind != 0) out << i << ' ' << nodes[i].jkind << '\n';
        }
        size_t ne = 0;
        for (const auto& e : edges) if (e.oneWay != 0) ne++;
        if (ne) {
            out << "EDGEQ " << ne << '\n';
            for (size_t i = 0; i < edges.size(); i++) if (edges[i].oneWay != 0) out << i << ' ' << edges[i].oneWay << '\n';
        }
    }
    if (!busStops.empty() || !busLines.empty()) {
        auto tok = [](std::string n) { if (n.empty()) n = "-"; for (char& ch : n) if (ch == ' ' || ch == '\n' || ch == '\t') ch = '_'; return n; };
        out << "TRANSIT " << busStops.size() << ' ' << busLines.size() << '\n';
        for (const BusStop& bs : busStops)
            out << bs.pos.x << ' ' << bs.pos.y << ' ' << bs.heading.x << ' ' << bs.heading.y << ' ' << tok(bs.name) << '\n';
        for (const BusLine& l : busLines) {
            out << tok(l.name) << ' ' << (int)l.color.r << ' ' << (int)l.color.g << ' ' << (int)l.color.b << ' ' << l.buses << ' ' << l.stops.size();
            for (int s : l.stops) out << ' ' << s;
            out << '\n';
        }
    }
    if (!params.routedTraffic || params.rushHours)
        out << "TRAFFIC " << (params.routedTraffic ? 1 : 0) << ' ' << (params.rushHours ? 1 : 0) << '\n';
    // Last on purpose: a reader that predates districts stops at the first tag it does not know,
    // so everything it understands has to come before this.
    if (!districts.empty()) {
        out << "DISTRICTS " << districts.size() << '\n';
        for (const District& d : districts) {
            std::string nm = d.name.empty() ? "-" : d.name;
            for (char& ch : nm) if (ch == ' ' || ch == '\n' || ch == '\t') ch = '_';
            out << d.pos.x << ' ' << d.pos.y << ' ' << d.radius << ' ' << d.peak << ' ' << d.kind << ' ' << d.style << ' ' << nm << '\n';
        }
        out << "DPAINT " << blockDistricts.size() << '\n';
        for (const auto& kv : blockDistricts) out << kv.first << ' ' << kv.second << '\n';
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
    for (size_t i = 0; i < edgeCount; i++) {
        edges[i] = RoadEdge{};
        if (!(in >> edges[i].a >> edges[i].b >> edges[i].lanes)) return false;
    }

    size_t nodeCount = 0;
    if (!(in >> nodeCount)) return false;
    nodes.resize(nodeCount);
    for (size_t i = 0; i < nodeCount; i++) {
        int boundary = 0;
        if (!(in >> nodes[i].pos.x >> nodes[i].pos.y >> boundary)) return false;
        nodes[i].boundary = (boundary != 0);
        nodes[i].h = 0.0f;
        nodes[i].jkind = 0;
        nodes[i].phases.clear(); nodes[i].yellow = 1.5f; nodes[i].allRed = 1.0f; nodes[i].sigOffset = 0.0f;
        nodes[i].rbIsland = 0.0f; nodes[i].rbRing = 0.0f; nodes[i].rbSplitters = true; nodes[i].rbConcrete = false;
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
        blockKinds.clear();
        districts.clear();
        blockDistricts.clear();
        busStops.clear();
        busLines.clear();
        placed.clear();
        collisionEnabled = true;
        params.carColorAll = false;
        params.carColors = CityParams{}.carColors;
        params.routedTraffic = true;
        params.rushHours = false;
        // Optional trailing sections in any order: KINDS, COLL, PLACED.
        for (;;) {
            const std::streampos before = in.tellg();
            tag.clear();
            if (!(in >> tag)) { in.clear(); in.seekg(before); break; }
            if (tag == "KINDS") {
                size_t kc = 0;
                if (!(in >> kc)) return false;
                for (size_t i = 0; i < kc; i++) {
                    uint64_t id = 0; int k = 0;
                    if (!(in >> id >> k)) return false;
                    if (k > 0 && k <= (int)BlockKind::Concrete) blockKinds[id] = (BlockKind)k;
                }
            } else if (tag == "COLL") {
                int c = 1;
                if (!(in >> c)) return false;
                collisionEnabled = (c != 0);
            } else if (tag == "PLACED") {
                size_t pc = 0;
                if (!(in >> pc)) return false;
                placed.resize(pc);
                for (size_t i = 0; i < pc; i++) {
                    PlacedBuilding& pb = placed[i];
                    if (!(in >> pb.center.x >> pb.center.y >> pb.sizeX >> pb.sizeZ >> pb.height >> pb.angleY >> pb.colorBucket))
                        return false;
                }
            } else if (tag == "SIGNALS") {
                size_t sc = 0;
                if (!(in >> sc)) return false;
                for (size_t i = 0; i < sc; i++) {
                    size_t idx = 0, pc = 0; float ye = 1.5f, ar = 1.0f, of = 0.0f;
                    if (!(in >> idx >> ye >> ar >> of >> pc)) return false;
                    std::vector<SignalPhase> phs;
                    for (size_t k = 0; k < pc; k++) {
                        SignalPhase ph; int mk = 0;
                        if (!(in >> ph.duration >> mk)) return false;
                        ph.mask = (uint8_t)mk;
                        phs.push_back(ph);
                    }
                    if (idx < nodes.size()) { nodes[idx].yellow = ye; nodes[idx].allRed = ar; nodes[idx].sigOffset = of; nodes[idx].phases = phs; }
                }
            } else if (tag == "EDGET") {
                size_t ec = 0;
                if (!(in >> ec)) return false;
                for (size_t i = 0; i < ec; i++) {
                    size_t idx = 0; float sl = 0; unsigned ta = 0, tb = 0;
                    if (!(in >> idx >> sl >> ta >> tb)) return false;
                    if (idx < edges.size()) { edges[idx].speedLimit = sl; edges[idx].turnA = (uint16_t)ta; edges[idx].turnB = (uint16_t)tb; }
                }
            } else if (tag == "LIFE") {
                std::string rest;
                std::getline(in, rest);
                std::istringstream ls(rest);
                int fu = 1, ca = 0, pe = 0; float td = 150.0f;
                if (!(ls >> fu >> ca >> pe)) return false;
                int lht = 0;
                ls >> td;   // optional (files saved before the detail distance existed)
                ls >> lht;
                params.furniture = fu != 0;
                params.cars = std::clamp(ca, 0, 500);
                params.pedestrians = std::clamp(pe, 0, 1000);
                params.trafficDetailDistance = std::max(td, 0.0f);
                params.leftHandTraffic = lht != 0;
            } else if (tag == "CARCOL") {
                int all = 0; size_t nc = 0;
                if (!(in >> all >> nc)) return false;
                std::vector<CarColor> tab;
                for (size_t i = 0; i < nc; i++) {
                    int r = 0, g = 0, b = 0, ot = 0; float w = 0; std::string nm;
                    if (!(in >> r >> g >> b >> w >> ot >> nm)) return false;
                    for (char& ch : nm) if (ch == '_') ch = ' ';
                    CarColor c;
                    c.name = nm == "-" ? "" : nm;
                    c.color = Color{ (unsigned char)std::clamp(r, 0, 255), (unsigned char)std::clamp(g, 0, 255), (unsigned char)std::clamp(b, 0, 255), 255 };
                    c.weight = std::max(w, 0.0f); c.other = ot != 0;
                    tab.push_back(c);
                }
                params.carColorAll = all != 0;
                params.carColors = tab;
            } else if (tag == "RBP") {
                size_t nr = 0;
                if (!(in >> nr)) return false;
                for (size_t i = 0; i < nr; i++) {
                    size_t idx = 0; float isl = 0, rg = 0; int sp = 1, co = 0;
                    if (!(in >> idx >> isl >> rg >> sp >> co)) return false;
                    if (idx < nodes.size()) { nodes[idx].rbIsland = isl; nodes[idx].rbRing = rg; nodes[idx].rbSplitters = sp != 0; nodes[idx].rbConcrete = co != 0; }
                }
            } else if (tag == "TRANSIT") {
                size_t ns = 0, nl = 0;
                if (!(in >> ns >> nl)) return false;
                busStops.assign(ns, BusStop{});
                for (BusStop& bs : busStops) {
                    std::string nm;
                    if (!(in >> bs.pos.x >> bs.pos.y >> bs.heading.x >> bs.heading.y >> nm)) return false;
                    if (nm == "-") nm.clear();
                    for (char& ch : nm) if (ch == '_') ch = ' ';
                    bs.name = nm;
                }
                busLines.assign(nl, BusLine{});
                for (BusLine& l : busLines) {
                    std::string nm; int r = 0, g = 0, b = 0, buses = 2; size_t k = 0;
                    if (!(in >> nm >> r >> g >> b >> buses >> k)) return false;
                    if (nm == "-") nm.clear();
                    for (char& ch : nm) if (ch == '_') ch = ' ';
                    l.name = nm;
                    l.color = Color{ (unsigned char)std::clamp(r, 0, 255), (unsigned char)std::clamp(g, 0, 255), (unsigned char)std::clamp(b, 0, 255), 255 };
                    l.buses = std::clamp(buses, 0, 20);
                    l.stops.resize(k);
                    for (size_t i = 0; i < k; i++) {
                        if (!(in >> l.stops[i])) return false;
                        if (l.stops[i] < 0 || (size_t)l.stops[i] >= ns) l.stops[i] = 0;
                    }
                }
            } else if (tag == "TRAFFIC") {
                int routed = 1, rush = 0;
                if (!(in >> routed >> rush)) return false;
                params.routedTraffic = routed != 0;
                params.rushHours = rush != 0;
            } else if (tag == "DAY") {
                float tod = 12.0f, len = 0.0f;       // older files kept the time per city: it is a scene setting now
                if (!(in >> tod >> len)) return false;
            } else if (tag == "DISTRICTS") {
                size_t dc = 0;
                if (!(in >> dc)) return false;
                districts.assign(dc, District{});
                for (District& d : districts) {
                    std::string nm;
                    if (!(in >> d.pos.x >> d.pos.y >> d.radius >> d.peak >> d.kind >> d.style >> nm)) return false;
                    if (nm == "-") nm.clear();
                    for (char& ch : nm) if (ch == '_') ch = ' ';
                    d.name = nm;
                    d.radius = std::max(d.radius, 1.0f);
                    d.style = std::clamp(d.style, -1, 3);
                }
            } else if (tag == "DPAINT") {
                size_t pc = 0;
                if (!(in >> pc)) return false;
                for (size_t i = 0; i < pc; i++) {
                    uint64_t id = 0; int di = 0;
                    if (!(in >> id >> di)) return false;
                    if (di >= 0 && (size_t)di < districts.size()) blockDistricts[id] = di;
                }
            } else if (tag == "VARY") {
                float sc = 0.15f, fp = 0.15f;
                if (!(in >> sc >> fp)) return false;
                params.shortChance = Clamp(sc, 0.0f, 1.0f);
                params.footprintVariety = Clamp(fp, 0.0f, 0.6f);
            } else if (tag == "STYLE") {
                int st = 0; float sv = 0.5f;
                if (!(in >> st >> sv)) return false;
                params.style = std::clamp(st, 0, 3);
                params.shapeVariety = Clamp(sv, 0.0f, 1.0f);
            } else if (tag == "NODEP") {
                size_t nc = 0;
                if (!(in >> nc)) return false;
                for (size_t i = 0; i < nc; i++) {
                    size_t idx = 0; int kind = 0;
                    if (!(in >> idx >> kind)) return false;
                    if (idx < nodes.size()) nodes[idx].jkind = Clamp(kind, 0, 4);
                }
            } else if (tag == "EDGEQ") {
                size_t ec = 0;
                if (!(in >> ec)) return false;
                for (size_t i = 0; i < ec; i++) {
                    size_t idx = 0; int ow = 0;
                    if (!(in >> idx >> ow)) return false;
                    if (idx < edges.size()) edges[idx].oneWay = Clamp(ow, 0, 2);
                }
            } else if (tag == "PSHAPE") {
                size_t sc = 0;
                if (!(in >> sc)) return false;
                for (size_t i = 0; i < sc; i++) {
                    size_t idx = 0; int sh = 0;
                    if (!(in >> idx >> sh)) return false;
                    if (idx < placed.size()) placed[idx].shape = std::clamp(sh, 0, kBuildingShapes - 1);
                }
            } else if (tag == "FREEP") {
                size_t fc = 0;
                if (!(in >> fc)) return false;
                for (size_t i = 0; i < fc; i++) {
                    size_t idx = 0;
                    if (!(in >> idx)) return false;
                    if (idx < placed.size()) placed[idx].free = true;
                }
            } else if (tag == "NHEIGHT") {
                size_t hc = 0;
                if (!(in >> hc)) return false;
                for (size_t i = 0; i < hc; i++) {
                    float h = 0.0f;
                    if (!(in >> h)) return false;
                    if (i < nodes.size()) nodes[i].h = h;
                }
            } else if (tag == "EDGEP") {
                size_t pc = 0;
                if (!(in >> pc)) return false;
                for (size_t i = 0; i < pc; i++) {
                    size_t idx = 0; int type = 0, br = 0, mk = 0; float w = 0, sw = -1, cb = 0, bk = 0;
                    if (!(in >> idx >> w >> sw >> type >> br >> cb >> mk >> bk)) return false;
                    if (idx >= edges.size()) continue;
                    RoadEdge& e = edges[idx];
                    e.width = w; e.sidewalk = sw; e.type = type; e.bridge = br != 0; e.curbH = cb; e.markings = mk != 0; e.bank = bk;
                }
            } else {
                in.clear();
                in.seekg(before);
                break;
            }
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
void SetTrafficRunning(bool on) { g_simActive = on; }
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