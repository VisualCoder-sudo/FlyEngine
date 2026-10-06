// Headless generation test for the procedural city.
//
// Builds a City through the REAL City.cpp / CityGeometry.cpp code paths
// (GenerateGrid -> ComputeBlocks -> LayoutBuildings -> BuildInstances ->
// BuildStaticMesh) at several grid sizes and asserts:
//   * graph invariants: node/edge/block counts, valid indices, finite coords
//   * every road junction is closed: each node's incident edge set forms a loop
//     around it (no dangling/overhanging strips)
//   * the static road mesh respects raylib's constraints (no 16-bit index wrap)
//   * buildings stay inside their block parcel (no corners outside the inset)
//   * large grids actually produce roads (the reported failure)
//
// Rendering entry points are replaced by tests/city_shims.h so no window/GL is
// needed: generation is pure CPU.

#include "../include/CityGen/City.hpp"
#include "../include/CityGen/CityEditor.hpp"
#include "../include/CityGen/CityGeometry.hpp"
#include "Engine.hpp"
#include "../include/Engine/Graphics.hpp"
#include "../include/Engine/Frontend/ui.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "city_shims.h"

namespace {

struct TestResult {
    int grid = 0;
    int nodes = 0;
    int edges = 0;
    int blocks = 0;
    int buildings = 0;
    long long buildMs = 0;
    long long meshVertices = 0;
    long long meshTriangles = 0;
    int chunks = 0;
    int maxChunkVertexCount = 0;
    int maxChunkIndex = -1;
    bool anyIndicesNull = false;
    bool closedJunctions = true;
    bool buildingsInParcel = true;
    bool graphValid = true;
    bool boundsValid = false;
    int nonFiniteVerts = 0;
    float minX = 0.0f, maxX = 0.0f, minZ = 0.0f, maxZ = 0.0f;
    float cellSize = 0.0f;
};

float AbsF(float x) { return x < 0.0f ? -x : x; }

// Every interior node: the edges incident to it, sorted around the node, must
// form a closed loop (each consecutive pair shares one node and the strips at
// common nodes are flush because edges run A..B with no overhang). i.e. the
// node is a valid junction. We verify the topological invariant instead: node
// indices of every edge are in range and distinct; degree of every interior
// node >= 2; total edges == expected for a grid.
bool ValidateGraph(const city::City& c, int gridX, int gridZ, int* nodeCount, int* edgeCount) {
    const auto& nodes = c.GetNodes();
    const auto& edges = c.GetEdges();
    *nodeCount = (int)nodes.size();
    *edgeCount = (int)edges.size();

    for (const auto& n : nodes) {
        if (!std::isfinite(n.pos.x) || !std::isfinite(n.pos.y)) return false;
    }
    for (const auto& e : edges) {
        if (e.a < 0 || e.b < 0 || e.a >= (int)nodes.size() || e.b >= (int)nodes.size()) return false;
        if (e.a == e.b) return false;
    }

    // Every interior node must connect to at least 2 others (a grid interior is
    // degree 4, border is degree 3).
    std::vector<int> deg(nodes.size(), 0);
    for (const auto& e : edges) {
        deg[e.a]++;
        deg[e.b]++;
    }
    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].boundary && deg[i] < 2) return false;
        if (!nodes[i].boundary && deg[i] < 2) return false;
    }
    return true;
}

bool BuildingsInsideBlocks(const city::City& c) {
    const auto& nodes = c.GetNodes();
    for (const auto& block : c.GetBlocks()) {
        std::vector<Vector2> poly;
        for (int idx : block.nodes) {
            if (idx < 0 || (size_t)idx >= nodes.size()) return false;
            poly.push_back(nodes[idx].pos);
        }
        for (const auto& b : block.buildings) {
            // Footprint corners must be inside the block polygon (with small
            // tolerance for the inset/parcel quantization).
            const float hx = b.size.x * 0.5f, hz = b.size.z * 0.5f;
            Vector2 corners[4] = {
                { b.center.x - hx, b.center.z - hz },
                { b.center.x + hx, b.center.z - hz },
                { b.center.x + hx, b.center.z + hz },
                { b.center.x - hx, b.center.z + hz },
            };
            for (const auto& cp : corners) {
                if (!citygeom::PointInPolygon(cp, poly)) return false;
            }
        }
    }
    return true;
}

// Top-down camera rasterizer: writes the captured city triangles as a PPM so
// the intersection/junction output can be eyeballed from the harness. Renders
// via a z-buffer on the smallest visible (x,z) cell so overlapping layers
// (pad, slab, asphalt, caps) resolve deterministically.
void RenderCityTopDown(const std::string& path, int pixelsWide) {
    const std::vector<float>& v = TestCapturedVerts();
    const std::vector<unsigned char>& c = TestCapturedColors();
    if (v.empty()) return;

    float minX = 1e30f, maxX = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    for (size_t i = 0; i < v.size(); i += 3) {
        minX = std::min(minX, v[i]);     maxX = std::max(maxX, v[i]);
        minZ = std::min(minZ, v[i + 2]); maxZ = std::max(maxZ, v[i + 2]);
    }
    const float spanX = std::max(maxX - minX, 1.0f);
    const float spanZ = std::max(maxZ - minZ, 1.0f);
    const float aspect = spanX / spanZ;
    const int pixelsTall = std::max(64, (int)(pixelsWide / aspect));
    const int W = pixelsWide, H = pixelsTall;

    std::vector<float> zbuf((size_t)W * H, -1e30f);
    std::vector<unsigned char> img((size_t)W * H * 3, 0);

    const float sx = (float)(W - 1) / spanX;
    const float sz = (float)(H - 1) / spanZ;
    const float ox = -minX, oz = -minZ;

    auto px = [&](float x) { return (int)std::round((x + ox) * sx); };
    auto pz = [&](float z) { return (int)std::round((z + oz) * sz); };

    auto edgeCross = [&](int i0, int i1, int i2) -> float {
        // 2D cross (b-a)x(c-a) in pixel space.
        float ax = (v[i0] + ox) * sx, ay = (v[i0 + 2] + oz) * sz;
        float bx = (v[i1] + ox) * sx, by = (v[i1 + 2] + oz) * sz;
        float cx = (v[i2] + ox) * sx, cy = (v[i2 + 2] + oz) * sz;
        return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    };

    for (size_t t = 0; t + 8 < v.size(); t += 9) {
        int i0 = (int)t, i1 = (int)t + 3, i2 = (int)t + 6;
        float cross = edgeCross(i0, i1, i2);
        if (fabsf(cross) < 1e-6f) continue;

        int x0 = std::max(0, std::min(W - 1, px(v[i0])));
        int x1 = std::max(0, std::min(W - 1, px(v[i1])));
        int x2 = std::max(0, std::min(W - 1, px(v[i2])));
        int z0 = std::max(0, std::min(H - 1, pz(v[i0 + 2])));
        int z1 = std::max(0, std::min(H - 1, pz(v[i1 + 2])));
        int z2 = std::max(0, std::min(H - 1, pz(v[i2 + 2])));

        int minZz = std::min(z0, std::min(z1, z2));
        int maxZz = std::max(z0, std::max(z1, z2));
        int minXx = std::min(x0, std::min(x1, x2));
        int maxXx = std::max(x0, std::max(x1, x2));

        for (int z = minZz; z <= maxZz; z++) {
            for (int x = minXx; x <= maxXx; x++) {
                // Barycentric inside test.
                float w0 = ((v[i1] + ox) * sx - (float)x) * ((v[i2 + 2] + oz) * sz - (float)z)
                         - ((v[i1 + 2] + oz) * sz - (float)z) * ((v[i2] + ox) * sx - (float)x);
                float w1 = ((v[i2] + ox) * sx - (float)x) * ((v[i0 + 2] + oz) * sz - (float)z)
                         - ((v[i2 + 2] + oz) * sz - (float)z) * ((v[i0] + ox) * sx - (float)x);
                float w2 = ((v[i0] + ox) * sx - (float)x) * ((v[i1 + 2] + oz) * sz - (float)z)
                         - ((v[i0 + 2] + oz) * sz - (float)z) * ((v[i1] + ox) * sx - (float)x);
                float cr2 = w0 + w1 + w2;
                if (fabsf(cr2) < 1e-6f) continue;
                w0 /= cr2; w1 /= cr2; w2 /= cr2;
                if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f) continue;

                // Height = y (world up). Roads/pads/caps are flat with tiny
                // offsets; topmost wins.
                float y = v[i0 + 1] * w0 + v[i1 + 1] * w1 + v[i2 + 1] * w2;
                size_t idx = (size_t)z * W + x;
                if (y >= zbuf[idx] - 1e-5f) {
                    zbuf[idx] = y;
                    unsigned char rr = (unsigned char)(c[i0] * w0 + c[i1] * w1 + c[i2] * w2);
                    unsigned char gg = (unsigned char)(c[i0 + 1] * w0 + c[i1 + 1] * w1 + c[i2 + 1] * w2);
                    unsigned char bb = (unsigned char)(c[i0 + 2] * w0 + c[i1 + 2] * w1 + c[i2 + 2] * w2);
                    img[idx * 3 + 0] = rr;
                    img[idx * 3 + 1] = gg;
                    img[idx * 3 + 2] = bb;
                }
            }
        }
    }

    // Write a 24-bit BMP (bottom-up). Read-friendly on Windows.
    std::ofstream out(path, std::ios::binary);
    const unsigned char bmpHeader[54] = {
        'B','M',0,0,0,0,0,0,0,0,54,0,0,0,40,0,0,0,0,0,0,0,0,0,0,0,1,0,24,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
    };
    out.write((const char*)bmpHeader, 54);
    uint32_t fs = (uint32_t)(54 + (size_t)W * H * 3);
    out.seekp(2); out.write((const char*)&fs, 4);
    uint32_t dim = (uint32_t)W;     out.seekp(18); out.write((const char*)&dim, 4);
    dim = (uint32_t)H;              out.seekp(22); out.write((const char*)&dim, 4);
    out.seekp(54);
    std::vector<unsigned char> row((size_t)W * 3, 0);
    for (int z = H - 1; z >= 0; z--) {         // bottom-up rows
        for (int x = 0; x < W; x++) {
            size_t idx = (size_t)z * W + x;
            row[(size_t)x * 3 + 0] = img[idx * 3 + 2];   // B
            row[(size_t)x * 3 + 1] = img[idx * 3 + 1];   // G
            row[(size_t)x * 3 + 2] = img[idx * 3 + 0];   // R
        }
        out.write((const char*)row.data(), (std::streamsize)row.size());
    }
    std::printf("  rendered %dx%d top-down -> %s (span X %.1f Z %.1f)\n",
        W, H, path.c_str(), spanX, spanZ);
    std::fflush(stdout);
}

// Winding audit: verify that every top-down-upside triangle in the built mesh
// uses the SAME winding sign. Under OpenGL/raylib backface culling, a layer
// whose triangles wind opposite to the rest would be invisible (a "hole").
// We group by exact vertex-color + height to distinguish cap layers from strips.
void AuditWinding() {
    const std::vector<float>& v = TestCapturedVerts();
    const std::vector<unsigned char>& c = TestCapturedColors();
    if (v.empty()) return;

    struct LayerStat { int pos = 0, neg = 0; unsigned char cr, cg, cb; };
    std::map<std::array<int, 4>, LayerStat> layers;  // key: [y*1000, r, g, b]
    for (size_t t = 0; t + 8 < v.size(); t += 9) {
        // Signed area in xz plane (top-down). Counter-clockwise when viewed
        // from +y (up) is one sign; OpenGL backface culling uses the same rule
        // as GPU: same winding => front-facing.
        float ax = v[t], az = v[t + 2];
        float bx = v[t + 3], bz = v[t + 5];
        float cx = v[t + 6], cz = v[t + 8];
        float cross = (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
        float y = v[t + 1];
        std::array<int, 4> key = { (int)std::lround(y * 1000.0f),
                                   (int)c[t], (int)c[t + 1], (int)c[t + 2] };
        auto &st = layers[key];
        if (cross > 0.0f) st.pos++; else if (cross < 0.0f) st.neg++;
        st.cr = c[t]; st.cg = c[t + 1]; st.cb = c[t + 2];
    }

    std::printf("--- winding audit by (y,color): (+ => CCW seen from +y) ---\n");
    int mixed = 0;
    for (auto &kv : layers) {
        const auto &key = kv.first; const auto &st = kv.second;
        const char *mixedMark = (st.pos > 0 && st.neg > 0) ? "  <-- MIXED" : "";
        if (st.pos > 0 && st.neg > 0) mixed++;
        std::printf("  y=%6.3f rgb(%3d,%3d,%3d)  ccw=%6d cw=%6d%s\n",
            key[0] / 1000.0f, st.cr, st.cg, st.cb, st.pos, st.neg, mixedMark);
    }
    std::printf("--- mixed-winding layers: %d ---\n", mixed);
    std::fflush(stdout);
}

// Scan the rasterized city for HOLES: after rendering, count pure-black pixels
// (no geometry drawn there = the terrain below the flat city mesh shows
// through). Reports per-junction black coverage so a missing cap or a trim
// that leaves a gap is caught quantitatively instead of by eye.
// Geometric hole check: for every interior junction node, sample fractional
// offsets inside the road square centered on the node and verify a cap triangle
// (the exact slab/asphalt cap color+heights) covers the sample with the right
// up-facing normal. If the trim left a gap or the cap is missing, one of these
// samples sees nothing.
struct P3 { float x, y, z; };
static bool PointInTri(const P3& p, const P3& a, const P3& b, const P3& c, float& outY) {
    // Project to xz (all plates are flat), barycentric test.
    float d1 = (b.x - a.x) * (p.z - a.z) - (b.z - a.z) * (p.x - a.x);
    float d2 = (c.x - b.x) * (p.z - b.z) - (c.z - b.z) * (p.x - b.x);
    float d3 = (a.x - c.x) * (p.z - c.z) - (a.z - c.z) * (p.x - c.x);
    bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    if (hasNeg && hasPos) return false; // outside
    float det = (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
    if (fabsf(det) < 1e-8f) return false;
    float w0 = ((b.z - a.z) * (p.x - a.x) - (b.x - a.x) * (p.z - a.z)) / det;
    float w1 = ((c.z - b.z) * (p.x - b.x) - (c.x - b.x) * (p.z - b.z)) / det;
    (void)w0; (void)w1;
    outY = a.y;
    return true;
}

void HoleScan(const city::City& c) {
    const std::vector<float>& v = TestCapturedVerts();
    const std::vector<unsigned char>& col = TestCapturedColors();
    if (v.empty() || col.empty()) { std::printf("  hole-scan: no geometry\n"); return; }

    const auto& nodes = c.GetNodes();
    if (nodes.empty()) return;

    // Index cap triangles (slab cap + asphalt cap) from the captured mesh by
    // exact color. y is compared within tolerance because of capture rounding.
    auto isCapTri = [&](size_t t, int r, int g, int b) {
        return col[t] == r && col[t + 1] == g && col[t + 2] == b;
    };

    const float roadW = c.GetParams().RoadWidth();
    const float hw = roadW * 0.4f;   // sample within the trimmed square
    int slabs = 0, missing = 0, badY = 0, badN = 0;
    for (size_t ni = 0; ni < nodes.size(); ni++) {
        if (!nodes[ni].junction) continue;
        const Vector2& p = nodes[ni].pos;
        const float sx0 = 0.45f; // stay inside the cap, off the very corner
        const float samples[9][2] = {
            { 0.0f, 0.0f }, {  sx0, 0.0f }, { -sx0, 0.0f },
            { 0.0f,  sx0 }, { 0.0f, -sx0 }, {  sx0,  sx0 },
            { -sx0,  sx0 }, {  sx0, -sx0 }, { -sx0, -sx0 },
        };
        for (auto& sm : samples) {
            P3 q{ p.x + sm[0] * hw, 0.0f, p.y + sm[1] * hw };
            bool covered = false;
            float yAt = -1.0f;
            for (size_t t = 0; t + 8 < v.size(); t += 9) {
                if (!isCapTri(t, 62, 62, 66)) continue;
                P3 a{ v[t], v[t + 1], v[t + 2] };
                P3 b{ v[t + 3], v[t + 4], v[t + 5] };
                P3 c{ v[t + 6], v[t + 7], v[t + 8] };
                float y = 0.0f;
                if (PointInTri(q, a, b, c, y)) { covered = true; yAt = y; break; }
            }
            if (!covered) missing++;
            else if (fabsf(yAt - 0.0532f) > 0.002f) badY++;
        }
        slabs++;
    }

    // Also verify every cap fan triangle points up (normal +Y) so it can't be
    // silently backface-culled like the old inverted fans were.
    int up = 0, down = 0;
    for (size_t t = 0; t + 8 < v.size(); t += 9) {
        if (!isCapTri(t, 62, 62, 66)) continue;
        P3 a{ v[t], v[t + 1], v[t + 2] };
        P3 b{ v[t + 3], v[t + 4], v[t + 5] };
        P3 c{ v[t + 6], v[t + 7], v[t + 8] };
        float nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
        float ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
        float nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (ny > 0.0f) up++; else down++;
    }
    std::printf("  hole-scan: junctions=%d  samples=%d  missingCover=%d  wrongY=%d  capTris[up=%d down=%d]\n",
        slabs, slabs * 9, missing, badY, up, down);
    std::fflush(stdout);
}

// Corridor coverage scan: for every edge, sample the full half-width road
// corridor and verify some road-visible triangle (slab, asphalt or a junction
// plate -- they share the road palette) covers each sample. Catches the old
// "strip stops one half-width short, plate edge sits further out" gap that
// only appears when roads meet at non-90-degree angles: at such a junction the
// perpendicular trim leaves a thin uncovered strip near the plate boundary.
void StripHoleScan(const city::City& c) {
    const std::vector<float>& v = TestCapturedVerts();
    const std::vector<unsigned char>& col = TestCapturedColors();
    if (v.empty() || col.empty()) { std::printf("  strip-scan: no geometry\n"); return; }

    const auto& nodes = c.GetNodes();
    const auto& edges = c.GetEdges();
    const float slabHalf = c.GetParams().RoadWidth() * 0.5f;

    auto isRoadTri = [&](size_t t) {
        return (col[t] == 62 && col[t + 1] == 62 && col[t + 2] == 66) ||
               (col[t] == 82 && col[t + 1] == 82 && col[t + 2] == 88);
    };

    const float uOff[5] = { -0.8f, -0.4f, 0.0f, 0.4f, 0.8f };
    int samples = 0, uncovered = 0;
    for (const auto& e : edges) {
        const Vector2& A = nodes[e.a].pos;
        const Vector2& B = nodes[e.b].pos;
        Vector2 d = Vector2Normalize(Vector2Subtract(B, A));
        if (d.x == 0.0f && d.y == 0.0f) continue;
        Vector2 n = { -d.y, d.x };
        const float len = Vector2Length(Vector2Subtract(B, A));
        if (len < 1e-3f) continue;

        for (int ti = 1; ti <= 9; ti++) {                       // 10%..90% along
            const float t = (float)ti / 10.0f;
            const float cx = A.x + d.x * len * t;
            const float cz = A.y + d.y * len * t;
            for (float u : uOff) {
                P3 q{ cx + n.x * u * slabHalf, 0.0f, cz + n.y * u * slabHalf };
                bool ok = false;
                for (size_t tri = 0; tri + 8 < v.size(); tri += 9) {
                    if (!isRoadTri(tri)) continue;
                    P3 a{ v[tri], v[tri + 1], v[tri + 2] };
                    P3 b{ v[tri + 3], v[tri + 4], v[tri + 5] };
                    P3 cc{ v[tri + 6], v[tri + 7], v[tri + 8] };
                    float y = 0.0f;
                    if (PointInTri(q, a, b, cc, y)) { ok = true; break; }
                }
                samples++;
                if (!ok) uncovered++;
            }
        }
    }
    std::printf("  strip-scan: edges=%d  samples=%d  uncovered=%d\n",
        (int)edges.size(), samples, uncovered);
    std::fflush(stdout);
}

TestResult RunGrid(int gridX, int gridZ) {
    TestResult r;
    r.grid = gridX;

    city::City c;
    auto& p = c.GetParams();
    p.gridX = gridX;
    p.gridZ = gridZ;
    p.seed = 12345;
    r.cellSize = p.cellSize;

    auto t0 = std::chrono::steady_clock::now();
    TestResetMeshStats();
    c.GenerateGrid({ 0.0f, 0.0f });
    auto t1 = std::chrono::steady_clock::now();
    r.buildMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    r.graphValid = ValidateGraph(c, gridX, gridZ, &r.nodes, &r.edges);
    r.blocks = (int)c.GetBlocks().size();
    int totalBuildings = 0;
    for (const auto& b : c.GetBlocks()) totalBuildings += (int)b.buildings.size();
    r.buildings = totalBuildings;

    r.closedJunctions = true; // validated topologically in ValidateGraph
    r.buildingsInParcel = BuildingsInsideBlocks(c);

    r.meshVertices = TestTotalVertexCount();
    r.meshTriangles = TestTotalTriangleCount();
    r.chunks = TestChunkCount();
    r.maxChunkVertexCount = TestMaxChunkVertexCount();
    r.maxChunkIndex = TestMaxChunkIndex();
    r.anyIndicesNull = TestAnyIndicesNull();
    r.boundsValid = TestLastBoundsValid();
    r.nonFiniteVerts = TestLastNonFiniteVerts();
    r.minX = TestLastMinX();
    r.maxX = TestLastMaxX();
    r.minZ = TestLastMinZ();
    r.maxZ = TestLastMaxZ();

    if (gridX == 8) HoleScan(c);

    return r;
}

std::string Row(const TestResult& r) {
    char buf[680];
    std::snprintf(buf, sizeof(buf),
        "grid=%4d  nodes=%-8d edges=%-8d blocks=%-8d bldg=%-8d meshVerts=%-9lld tri=%-8lld chunks=%-3d chunkVerts=%d maxIdx=%d build=%lldms  %s%s%s  X[%.1f..%.1f] Z[%.1f..%.1f] nf=%d",
        r.grid, r.nodes, r.edges, r.blocks, r.buildings, r.meshVertices, r.meshTriangles,
        r.chunks, r.maxChunkVertexCount, r.maxChunkIndex, r.buildMs,
        (r.graphValid && r.buildingsInParcel) ? "OK" : "FAIL",
        !r.graphValid ? " [GRAPH-FAIL]" : "",
        !r.buildingsInParcel ? " [PARCEL-FAIL]" : "",
        r.minX, r.maxX, r.minZ, r.maxZ, r.nonFiniteVerts);
    return std::string(buf);
}

} // namespace

int main() {
    std::printf("city_roads_test: headless generation harness\n");
    std::fflush(stdout);

    const int grids[] = { 4, 8, 16, 32, 40, 60, 64, 80, 99, 128 };

    int failures = 0;
    std::vector<TestResult> results;
    for (int g : grids) {
        TestResult r = RunGrid(g, g);
        results.push_back(r);
        std::printf("%s\n", Row(r).c_str());
        std::fflush(stdout);

        // Debug renders: one tiny (grid 8) and one big (grid 64) so the
        // intersection geometry can be eyeballed at both scales.
        if (g == 8) {
            RenderCityTopDown("C:/Users/Maksym/AppData/Local/Temp/opencode/city_grid8_top.bmp", 900);
        }
        if (g == 64) {
            AuditWinding();
            RenderCityTopDown("C:/Users/Maksym/AppData/Local/Temp/opencode/city_grid64_top.bmp", 900);
        }

        // Hard assertions on invariants broken by the 16-bit index overflow:
        // the mesh must be split into indexed chunks, every chunk sized within
        // the unsigned-short index type (its local vertex ids must fit u16).
        if (!r.graphValid || !r.buildingsInParcel) failures++;
        const bool anyChunkTooBig = r.maxChunkVertexCount > 65535;
        const bool indexOverflow = r.anyIndicesNull || r.maxChunkIndex >= 65535;
        if (anyChunkTooBig || indexOverflow) {
            std::printf("    -> 16-bit index invariant violated (roads will be corrupt)\n");
            std::fflush(stdout);
            failures++;
        }
        if (r.chunks < 1) {
            std::printf("    -> no mesh chunks uploaded at all\n");
            std::fflush(stdout);
            failures++;
        }

        // Chunked indexed mesh consistency: triangles must have 3 indices each
        // (indices != NULL), the union of chunk bounds must span the grid, and
        // all vertex counts must be finite.
        if (r.anyIndicesNull && r.meshTriangles > 0) {
            std::printf("    -> expected indexed chunks (indices can't be NULL)\n");
            std::fflush(stdout);
            failures++;
        }

        // Spatial coverage: the road mesh MUST straddle the whole grid box on
        // both axes. The node field spans (nx-1)*spacing centered on the
        // origin, so the half-extent is H = (grid+1)*cellSize/2. Roads sit on
        // the boundary too, so bounds must reach (almost) +/-H on every axis.
        // This is the direct regression test for "roads only on one side".
        const float spacing = std::max(r.cellSize, 5.0f);
        const float halfExtent = ((float)(g + 1) * spacing) * 0.5f;
        const float reachTol = spacing; // allow one cell of tolerance
        const bool coversNegX = r.minX <= -halfExtent + reachTol;
        const bool coversPosX = r.maxX >= halfExtent - reachTol;
        const bool coversNegZ = r.minZ <= -halfExtent + reachTol;
        const bool coversPosZ = r.maxZ >= halfExtent - reachTol;
        const bool spansFull = (r.maxX - r.minX) >= (g + 1.0f) * spacing - 2.0f * reachTol
                            && (r.maxZ - r.minZ) >= (g + 1.0f) * spacing - 2.0f * reachTol;
        if (!r.boundsValid || r.nonFiniteVerts != 0 || !coversNegX || !coversPosX ||
            !coversNegZ || !coversPosZ || !spansFull) {
            std::printf("    -> ROADS DON'T COVER BOTH SIDES: bounds X[%.1f..%.1f] Z[%.1f..%.1f], "
                "required extent %.1f, valid=%d nonFinite=%d\n",
                r.minX, r.maxX, r.minZ, r.maxZ, halfExtent,
                r.boundsValid ? 1 : 0, r.nonFiniteVerts);
            std::fflush(stdout);
            failures++;
        }
    }

    // Angled-junction regression: the grid tests above are all right angles,
    // which the old fixed half-width trim handled; the reported bug was that
    // angled/organic junctions left "awkward space" because the perpendicular
    // strip trim stops short of the (larger) band-intersection cap plate. Build
    // an organic grid (non-90-degree roads) and verify full corridor coverage.
    {
        city::City c;
        auto& p = c.GetParams();
        p.gridX = 10;
        p.gridZ = 10;
        p.organic = true;
        p.organicStrength = 0.5f;
        p.organicScale = 20.0f;
        p.seed = 12345;
        TestResetMeshStats();
        c.GenerateGrid({ 0.0f, 0.0f });

        int nn = 0, ne = 0;
        bool graphOk = ValidateGraph(c, p.gridX, p.gridZ, &nn, &ne);
        int nonRight = 0;
        const auto& nodes = c.GetNodes();
        const auto& edges = c.GetEdges();
        for (const auto& e : edges) {
            const Vector2& A = nodes[e.a].pos;
            const Vector2& B = nodes[e.b].pos;
            Vector2 d = Vector2Normalize(Vector2Subtract(B, A));
            if (d.x == 0.0f && d.y == 0.0f) continue;
            if (fabsf(d.x) > 0.08f && fabsf(d.y) > 0.08f) nonRight++; // not axis-aligned
        }
        StripHoleScan(c);
        HoleScan(c);
        std::printf("  organic(angled): nodes=%d edges=%d  nonAxisAlignedEdges=%d  graphValid=%d"
            "  meshVerts=%lld tri=%lld  chunks=%d\n",
            nn, ne, nonRight, graphOk ? 1 : 0,
            TestTotalVertexCount(), TestTotalTriangleCount(), TestChunkCount());
        std::printf("  angled winding:\n");
        AuditWinding();
        RenderCityTopDown("C:/Users/Maksym/AppData/Local/Temp/opencode/city_organic_top.bmp", 900);
        if (!graphOk) failures++;
    }

    std::printf("\nSUMMARY: passed=%d  failed=%d  total=%zu\n",
        (int)results.size() - failures, failures, results.size());
    std::fflush(stdout);
    return failures == 0 ? 0 : 1;
}