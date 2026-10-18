#include "../../include/Terrain/Foliage.hpp"
#include "../../include/Terrain/BasicTerrain.hpp"
#include "../../include/Engine/Graphics.hpp"
#include "../../include/Engine/Lighting.hpp"
#include "../Engine/GraphicsInternal.hpp"
#include "../../include/Engine/Platform/Platform.hpp"
#include "raymath.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <fstream>

namespace foliage {

namespace {

constexpr int kMaxPerCell = 30000;
constexpr double kBuildSeconds = 0.004;   // time spent placing cells per frame, so a fast turn never hitches

const char* const kKindNames[(int)Kind::Count] = { "Grass", "Flowers", "Bush", "Tree", "Pine", "Rock" };

// How tall the unit mesh of each kind is at scale 1, in metres.
const float kBaseHeight[(int)Kind::Count] = { 0.7f, 0.55f, 1.3f, 7.0f, 9.0f, 0.9f };

bool CastsShadow(Kind k) { return k == Kind::Bush || k == Kind::Tree || k == Kind::Pine || k == Kind::Rock; }
// Plants are placed and culled in squares: small for the dense low plants, big for the sparse tall ones.
float CellSize(const Rule& r) { return r.kind == Kind::Grass || r.kind == Kind::Flowers ? 12.0f : 32.0f; }
bool Thins(Kind k) { return k == Kind::Grass || k == Kind::Flowers; }   // blades, not solid shapes

// ---- hashing ---------------------------------------------------------------

uint32_t Hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
uint32_t Mix(uint32_t a, uint32_t b) { return Hash(a * 0x9e3779b1U ^ (b + 0x7f4a7c15U)); }

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    float Next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s & 0xffffff) / 16777216.0f; }
    float Range(float a, float b) { return a + (b - a) * Next(); }
};

// ---- meshes ----------------------------------------------------------------

Color LerpColor(Color a, Color b, float t) {
    t = Clamp(t, 0.0f, 1.0f);
    return { (unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t),
             (unsigned char)(a.b + (b.b - a.b) * t), 255 };
}

// Collects a mesh. Every triangle is emitted facing both ways with the same (outward) normal, so a plant is lit
// right from any side and can't vanish to back-face culling. The vertex colour is the plant's own shading (dark at
// the roots), the instance tint multiplies it, and texcoord.y carries how free a vertex is to sway in the wind
// (0 = rooted, 1 = tip); the shape-instanced shader reads it.
struct MeshBuilder {
    std::vector<float> pos, nrm, uv;
    std::vector<unsigned char> col;
    std::vector<unsigned short> idx;

    int Vertex(Vector3 p, Vector3 n, Color c, float sway) {
        n = Vector3Normalize(n);
        pos.insert(pos.end(), { p.x, p.y, p.z });
        nrm.insert(nrm.end(), { n.x, n.y, n.z });
        uv.insert(uv.end(), { 0.0f, Clamp(sway, 0.0f, 1.0f) });
        col.insert(col.end(), { c.r, c.g, c.b, 255 });
        return (int)pos.size() / 3 - 1;
    }
    void Tri(int a, int b, int c) {
        if (idx.size() + 6 > 65000) return;
        idx.insert(idx.end(), { (unsigned short)a, (unsigned short)b, (unsigned short)c,
                                (unsigned short)a, (unsigned short)c, (unsigned short)b });
    }
    void Quad(int a, int b, int c, int d) { Tri(a, b, c); Tri(a, c, d); }

    Mesh Build() {
        Mesh m = {};
        m.vertexCount = (int)pos.size() / 3;
        m.triangleCount = (int)idx.size() / 3;
        m.vertices = (float*)RL_MALLOC(pos.size() * sizeof(float));
        std::memcpy(m.vertices, pos.data(), pos.size() * sizeof(float));
        m.normals = (float*)RL_MALLOC(nrm.size() * sizeof(float));
        std::memcpy(m.normals, nrm.data(), nrm.size() * sizeof(float));
        m.texcoords = (float*)RL_MALLOC(uv.size() * sizeof(float));
        std::memcpy(m.texcoords, uv.data(), uv.size() * sizeof(float));
        m.colors = (unsigned char*)RL_MALLOC(col.size());
        std::memcpy(m.colors, col.data(), col.size());
        m.indices = (unsigned short*)RL_MALLOC(idx.size() * sizeof(unsigned short));
        std::memcpy(m.indices, idx.data(), idx.size() * sizeof(unsigned short));
        UploadMesh(&m, false);
        return m;
    }
};

// A tapering strip of grass bending away along `lean`: three rings up to a point.
void AddBlade(MeshBuilder& mb, Vector3 root, float yaw, float width, float height, float bend, Color base, Color tip) {
    const Vector3 along = { std::cos(yaw), 0.0f, std::sin(yaw) };          // across the blade's width
    const Vector3 lean = { -std::sin(yaw), 0.0f, std::cos(yaw) };          // the way it leans
    const Vector3 up = { 0.0f, 1.0f, 0.0f };
    const Vector3 face = Vector3CrossProduct(along, up);
    const Vector3 n = Vector3Add(Vector3Scale(up, 0.7f), Vector3Scale(face, 0.3f));   // soft: mostly facing the sky
    const float hs[3] = { 0.0f, 0.5f, 1.0f };
    const float ws[3] = { 1.0f, 0.72f, 0.0f };
    int ring[3][2];
    for (int i = 0; i < 3; i++) {
        const float h = hs[i];
        const Vector3 centre = Vector3Add(root, Vector3Add(Vector3Scale(up, h * height), Vector3Scale(lean, bend * h * h * height)));
        const Vector3 off = Vector3Scale(along, width * 0.5f * ws[i]);
        const Color c = LerpColor(base, tip, h);
        ring[i][0] = mb.Vertex(Vector3Subtract(centre, off), n, c, h);
        ring[i][1] = mb.Vertex(Vector3Add(centre, off), n, c, h);
    }
    mb.Quad(ring[0][0], ring[0][1], ring[1][1], ring[1][0]);
    mb.Tri(ring[1][0], ring[1][1], ring[2][0]);
}

void AddFlower(MeshBuilder& mb, Vector3 root, float height, float petal, Color stemBase, Color stemTip, Color centre, int petals = 6) {
    const Vector3 up = { 0.0f, 1.0f, 0.0f };
    // stem: a thin blade
    AddBlade(mb, root, 0.0f, 0.025f, height, 0.0f, stemBase, stemTip);
    AddBlade(mb, root, PI * 0.5f, 0.025f, height, 0.0f, stemBase, stemTip);
    // head: a fan of petals round a centre, tilted a little
    const Vector3 c = Vector3Add(root, Vector3Scale(up, height));
    const int cv = mb.Vertex(Vector3Add(c, { 0.0f, 0.012f, 0.0f }), up, centre, 1.0f);
    int first = -1, prev = -1;
    for (int i = 0; i <= petals * 2; i++) {
        const float a = (float)i / (petals * 2) * 2.0f * PI;
        const float r = (i % 2 == 0) ? petal : petal * 0.45f;
        const int v = mb.Vertex(Vector3Add(c, { std::cos(a) * r, 0.0f, std::sin(a) * r }), up, WHITE, 1.0f);
        if (prev >= 0) mb.Tri(cv, prev, v);
        else first = v;
        prev = v;
    }
    (void)first;
}

// A lumpy ball (an icosphere with its points pushed in and out): bushes, crowns, rocks.
void AddBlob(MeshBuilder& mb, Vector3 centre, Vector3 radii, int subdiv, float lumpy, uint32_t seed,
             Color low, Color high, float swayLow, float swayHigh) {
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    std::vector<Vector3> v = { { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 }, { 0, -1, t }, { 0, 1, t },
                               { 0, -1, -t }, { 0, 1, -t }, { t, 0, -1 }, { t, 0, 1 }, { -t, 0, -1 }, { -t, 0, 1 } };
    for (Vector3& p : v) p = Vector3Normalize(p);
    std::vector<std::array<int, 3>> f = { { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 },
                                          { 1, 5, 9 }, { 5, 11, 4 }, { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 },
                                          { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 },
                                          { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } };
    for (int s = 0; s < subdiv; s++) {
        std::map<std::pair<int, int>, int> mid;
        auto midpoint = [&](int a, int b) {
            const auto key = std::make_pair(std::min(a, b), std::max(a, b));
            auto it = mid.find(key);
            if (it != mid.end()) return it->second;
            v.push_back(Vector3Normalize(Vector3Scale(Vector3Add(v[(size_t)a], v[(size_t)b]), 0.5f)));
            return mid[key] = (int)v.size() - 1;
        };
        std::vector<std::array<int, 3>> next;
        for (const auto& tri : f) {
            const int a = midpoint(tri[0], tri[1]), b = midpoint(tri[1], tri[2]), c = midpoint(tri[2], tri[0]);
            next.push_back({ tri[0], a, c }); next.push_back({ tri[1], b, a });
            next.push_back({ tri[2], c, b }); next.push_back({ a, b, c });
        }
        f = std::move(next);
    }
    std::vector<int> index(v.size());
    for (size_t i = 0; i < v.size(); i++) {
        const Vector3 d = v[i];
        // The same point always gets the same push, so the surface has no cracks.
        const float h = (float)(Mix(seed, (uint32_t)(int)std::lround(d.x * 997.0f) * 31u + (uint32_t)(int)std::lround(d.y * 991.0f) * 17u +
                                         (uint32_t)(int)std::lround(d.z * 983.0f)) & 0xffff) / 65535.0f;
        const float push = 1.0f + lumpy * (h - 0.5f) * 2.0f;
        const Vector3 p = { centre.x + d.x * radii.x * push, centre.y + d.y * radii.y * push, centre.z + d.z * radii.z * push };
        const Vector3 n = { d.x / radii.x, d.y / radii.y, d.z / radii.z };
        const float k = d.y * 0.5f + 0.5f;
        index[i] = mb.Vertex(p, n, LerpColor(low, high, k), swayLow + (swayHigh - swayLow) * k);
    }
    for (const auto& tri : f) mb.Tri(index[(size_t)tri[0]], index[(size_t)tri[1]], index[(size_t)tri[2]]);
}

// A tapering tube from y0 to y1.
void AddTrunk(MeshBuilder& mb, float y0, float y1, float r0, float r1, int sides, Color low, Color high, float sway0, float sway1) {
    std::vector<int> a, b;
    for (int i = 0; i < sides; i++) {
        const float ang = (float)i / sides * 2.0f * PI;
        const Vector3 n = { std::cos(ang), 0.0f, std::sin(ang) };
        a.push_back(mb.Vertex({ n.x * r0, y0, n.z * r0 }, n, low, sway0));
        b.push_back(mb.Vertex({ n.x * r1, y1, n.z * r1 }, n, high, sway1));
    }
    for (int i = 0; i < sides; i++) {
        const int j = (i + 1) % sides;
        mb.Quad(a[(size_t)i], b[(size_t)i], b[(size_t)j], a[(size_t)j]);
    }
}

// A cone: the stacked tiers of a pine.
void AddCone(MeshBuilder& mb, float y0, float height, float radius, int sides, Color low, Color high, float sway0, float sway1) {
    const float len = std::sqrt(height * height + radius * radius);
    std::vector<int> ring;
    for (int i = 0; i < sides; i++) {
        const float ang = (float)i / sides * 2.0f * PI;
        const Vector3 n = { std::cos(ang) * height / len, radius / len, std::sin(ang) * height / len };
        ring.push_back(mb.Vertex({ std::cos(ang) * radius, y0, std::sin(ang) * radius }, n, low, sway0));
    }
    const int tip = mb.Vertex({ 0.0f, y0 + height, 0.0f }, { 0.0f, 1.0f, 0.0f }, high, sway1);
    for (int i = 0; i < sides; i++) mb.Tri(ring[(size_t)i], tip, ring[(size_t)((i + 1) % sides)]);
}

constexpr int kLods = 3;
Mesh g_mesh[(int)Kind::Count][kLods];   // each kind at full detail, about half and the barest silhouette
bool g_meshReady = false;
Texture2D g_white = {};

// The mesh of one kind at one level of detail (0 = full). Farther levels use fewer, bigger parts, so a far field keeps
// its shape and colour with a fraction of the triangles.
Mesh BuildMesh(Kind kind, int lod) {
    MeshBuilder mb;
    switch (kind) {
        case Kind::Grass: {   // a tuft of blades
            const int blades[kLods] = { 9, 5, 3 };
            const float wide[kLods] = { 1.0f, 1.5f, 2.2f };
            Rng r(11);
            for (int i = 0; i < blades[lod]; i++) {
                const float a = r.Range(0.0f, 2.0f * PI), d = r.Range(0.0f, 0.22f);
                AddBlade(mb, { std::cos(a) * d, 0.0f, std::sin(a) * d }, r.Range(0.0f, PI), r.Range(0.05f, 0.09f) * wide[lod], r.Range(0.6f, 1.0f),
                         r.Range(0.15f, 0.45f), { 52, 78, 30, 255 }, { 150, 188, 78, 255 });
            }
            break;
        }
        case Kind::Flowers: {   // a little grass and a few heads on stems
            const int grass[kLods] = { 4, 3, 2 }, heads[kLods] = { 3, 2, 2 };
            Rng r(23);
            for (int i = 0; i < grass[lod]; i++) {
                const float a = r.Range(0.0f, 2.0f * PI), d = r.Range(0.0f, 0.2f);
                AddBlade(mb, { std::cos(a) * d, 0.0f, std::sin(a) * d }, r.Range(0.0f, PI), lod == 2 ? 0.14f : 0.07f, r.Range(0.35f, 0.6f), 0.3f, { 55, 85, 35, 255 }, { 110, 150, 60, 255 });
            }
            for (int i = 0; i < heads[lod]; i++) {
                const float a = r.Range(0.0f, 2.0f * PI), d = r.Range(0.0f, 0.2f);
                AddFlower(mb, { std::cos(a) * d, 0.0f, std::sin(a) * d }, r.Range(0.7f, 1.0f), lod == 0 ? 0.085f : 0.11f, { 60, 95, 40, 255 }, { 100, 140, 60, 255 }, { 255, 215, 60, 255 }, lod == 0 ? 6 : 3);
            }
            break;
        }
        case Kind::Bush:   // a few overlapping lumps, then one, then a rough one
            if (lod == 0) {
                AddBlob(mb, { 0.0f, 0.38f, 0.0f }, { 0.55f, 0.38f, 0.55f }, 1, 0.22f, 5, { 28, 58, 26, 255 }, { 84, 128, 50, 255 }, 0.1f, 0.45f);
                AddBlob(mb, { 0.32f, 0.3f, 0.14f }, { 0.34f, 0.28f, 0.34f }, 1, 0.22f, 6, { 30, 60, 28, 255 }, { 80, 124, 48, 255 }, 0.1f, 0.4f);
                AddBlob(mb, { -0.28f, 0.33f, -0.1f }, { 0.34f, 0.3f, 0.34f }, 1, 0.22f, 7, { 28, 56, 26, 255 }, { 86, 130, 52, 255 }, 0.1f, 0.42f);
            } else {
                AddBlob(mb, { 0.0f, 0.36f, 0.0f }, { 0.66f, 0.38f, 0.62f }, lod == 1 ? 1 : 0, 0.22f, 5, { 28, 58, 26, 255 }, { 84, 128, 50, 255 }, 0.1f, 0.45f);
            }
            break;
        case Kind::Tree:   // a trunk and a crown of lumps
            AddTrunk(mb, 0.0f, 0.55f, 0.038f, 0.02f, lod == 0 ? 7 : (lod == 1 ? 5 : 4), { 70, 52, 36, 255 }, { 96, 74, 52, 255 }, 0.0f, 0.05f);
            if (lod == 0) {
                AddBlob(mb, { 0.0f, 0.7f, 0.0f }, { 0.27f, 0.25f, 0.27f }, 2, 0.16f, 31, { 30, 66, 28, 255 }, { 96, 148, 58, 255 }, 0.3f, 0.7f);
                AddBlob(mb, { 0.13f, 0.6f, 0.07f }, { 0.2f, 0.18f, 0.2f }, 1, 0.18f, 32, { 30, 62, 28, 255 }, { 88, 140, 54, 255 }, 0.3f, 0.65f);
                AddBlob(mb, { -0.12f, 0.8f, -0.06f }, { 0.2f, 0.18f, 0.2f }, 1, 0.18f, 33, { 34, 70, 30, 255 }, { 100, 152, 60, 255 }, 0.4f, 0.8f);
            } else {
                AddBlob(mb, { 0.0f, 0.72f, 0.0f }, { 0.34f, 0.3f, 0.34f }, lod == 1 ? 1 : 0, 0.16f, 31, { 30, 66, 28, 255 }, { 96, 148, 58, 255 }, 0.3f, 0.7f);
            }
            break;
        case Kind::Pine:   // a trunk and stacked tiers, then fewer fatter ones, then one cone
            if (lod == 0) {
                AddTrunk(mb, 0.0f, 0.3f, 0.03f, 0.02f, 6, { 66, 48, 34, 255 }, { 84, 62, 44, 255 }, 0.0f, 0.0f);
                const float y[5] = { 0.1f, 0.28f, 0.46f, 0.62f, 0.76f };
                const float h[5] = { 0.4f, 0.36f, 0.32f, 0.28f, 0.24f };
                const float r[5] = { 0.22f, 0.18f, 0.14f, 0.1f, 0.06f };
                for (int i = 0; i < 5; i++)
                    AddCone(mb, y[i], h[i], r[i], 8, { 24, 52, 36, 255 }, { 54, 104, 62, 255 }, 0.1f + 0.15f * (float)i, 0.35f + 0.13f * (float)i);
            } else if (lod == 1) {
                AddTrunk(mb, 0.0f, 0.3f, 0.03f, 0.02f, 4, { 66, 48, 34, 255 }, { 84, 62, 44, 255 }, 0.0f, 0.0f);
                AddCone(mb, 0.1f, 0.5f, 0.24f, 6, { 24, 52, 36, 255 }, { 54, 104, 62, 255 }, 0.1f, 0.5f);
                AddCone(mb, 0.4f, 0.45f, 0.16f, 6, { 24, 52, 36, 255 }, { 54, 104, 62, 255 }, 0.4f, 0.75f);
            } else {
                AddCone(mb, 0.1f, 0.78f, 0.24f, 5, { 24, 52, 36, 255 }, { 54, 104, 62, 255 }, 0.1f, 0.85f);
            }
            break;
        case Kind::Rock:   // one lumpy squashed ball
            AddBlob(mb, { 0.0f, 0.27f, 0.0f }, { 0.5f, 0.32f, 0.42f }, lod == 0 ? 1 : 0, 0.3f, 91, { 88, 85, 81, 255 }, { 150, 146, 140, 255 }, 0.0f, 0.0f);
            break;
        default: break;
    }
    return mb.Build();
}

void BuildMeshes() {
    if (g_meshReady) return;
    g_meshReady = true;
    for (int k = 0; k < (int)Kind::Count; k++)
        for (int l = 0; l < kLods; l++) g_mesh[k][l] = BuildMesh((Kind)k, l);

    Image white = GenImageColor(2, 2, WHITE);
    g_white = LoadTextureFromImage(white);
    UnloadImage(white);
}

// ---- settings and caches ---------------------------------------------------

struct Cell {
    unsigned vbo = 0;
    int count = 0;
    Vector2 min{}, max{};
    float top = 0.0f, bottom = 0.0f;   // the ground's lowest and highest plant, for culling
    float builtRank = 2.0f;            // only plants ranked below this were placed (far cells keep just the ones that survive the fade)
    std::vector<float> rank;           // each plant's fade rank, ascending (the instances are in this order)
};

// Plants are given a rank in [0, kRankMax]; at distance d a plant stays while its rank is below this threshold, so the
// ones with the lowest ranks last longest. The shader tests the same thing per instance.
constexpr float kRankMax = 0.9f;
float RankThreshold(const Rule& r, float dist, float range) {
    const float start = Clamp(r.fadeStart, 0.0f, 0.98f) * range;
    const float t = Clamp((dist - start) / std::max(range - start, 0.01f), 0.0f, 1.0f);
    return r.farKeep + (1.0f - r.farKeep) * (1.0f - t);
}

struct TerrainState {
    std::vector<std::unordered_map<int64_t, Cell>> cells;   // one map per rule
    uint32_t seenRevision = 0;
    float seenSignature = 0.0f;
    double changedAt = 0.0;
    bool pending = false;
    bool primed = false;
    std::set<std::array<int, 3>> dirty;   // (rule, cx, cz): cells the brush painted, placed again before the next draw
};

std::unordered_map<const BasicTerrain*, Settings>& SettingsMap() { static std::unordered_map<const BasicTerrain*, Settings> m; return m; }
std::unordered_map<const BasicTerrain*, TerrainState>& StateMap() { static std::unordered_map<const BasicTerrain*, TerrainState> m; return m; }
int g_instances = 0;
int g_drawn = 0;
int g_windLoc = -2;
int g_fadeLoc = -2;
int g_camLoc = -2;

int64_t CellKey(int cx, int cz) { return ((int64_t)cx << 32) ^ (int64_t)(uint32_t)cz; }

void FreeCells(TerrainState& st) {
    for (auto& map : st.cells)
        for (auto& [key, cell] : map) if (cell.vbo) gfx::DestroyInstanceBuffer(cell.vbo);
    st.cells.clear();
    st.dirty.clear();
    g_instances = 0;
    for (auto& [t, s] : StateMap()) for (auto& map : s.cells) for (auto& [k, c] : map) g_instances += c.count;
}

struct Plant { Vector3 pos; float scale; float yaw; float tintT; float heightVar; float rank; };

float Signature(const BasicTerrain& t) {
    return t.position.x * 1.3f + t.position.y * 2.9f + t.position.z * 4.1f + (float)t.GetWidth() * 0.37f + (float)t.GetDepth() * 0.73f + t.GetScale() * 5.3f;
}

void ScatterCell(const BasicTerrain& t, const Rule& r, int cx, int cz, float densityScale, float maxRank, std::vector<Plant>& out) {
    const float halfW = t.GetWidth() * t.GetScale() * 0.5f, halfD = t.GetDepth() * t.GetScale() * 0.5f;
    const float kCell = CellSize(r);
    const float x0 = (float)cx * kCell, z0 = (float)cz * kCell;
    if (x0 > t.position.x + halfW || x0 + kCell < t.position.x - halfW || z0 > t.position.z + halfD || z0 + kCell < t.position.z - halfD) return;
    const int want = std::min(kMaxPerCell, (int)std::lround(r.density * densityScale / 100.0f * kCell * kCell));
    if (want <= 0 || (r.painted && !r.mask)) return;
    Rng rng(Mix(Mix((uint32_t)r.seed * 7919u + (uint32_t)r.kind, (uint32_t)cx), (uint32_t)cz));
    const uint8_t* splat = t.GetSplatData();
    const int sw = t.GetSplatWidth(), sd = t.GetSplatDepth();
    const float extentX = t.GetWidth() * t.GetScale(), extentZ = t.GetDepth() * t.GetScale();
    const float cosMaxSlope = std::cos(Clamp(r.maxSlope, 0.0f, 90.0f) * DEG2RAD);
    for (int i = 0; i < want; i++) {
        const float x = x0 + rng.Next() * kCell, z = z0 + rng.Next() * kCell;
        const float chance = rng.Next(), s = rng.Next(), yaw = rng.Next() * 2.0f * PI, tint = rng.Next(), hv = rng.Next();
        const float u = (x - t.position.x) / extentX + 0.5f, w = (z - t.position.z) / extentZ + 0.5f;
        if (u < 0.0f || u >= 1.0f || w < 0.0f || w >= 1.0f) continue;
        if (r.painted) {
            const int ix = std::min(kMaskRes - 1, (int)(u * (float)kMaskRes)), iz = std::min(kMaskRes - 1, (int)(w * (float)kMaskRes));
            const float weight = (*r.mask)[(size_t)iz * kMaskRes + (size_t)ix] / 255.0f;
            if (weight <= 0.02f || chance > weight) continue;   // a light brush stroke makes it sparse
        } else if (r.layer >= 0 && r.layer < 4 && splat) {
            const int ix = std::min(sw - 1, (int)(u * (float)sw)), iz = std::min(sd - 1, (int)(w * (float)sd));
            const float weight = splat[((size_t)iz * (size_t)sw + (size_t)ix) * 4 + (size_t)r.layer] / 255.0f;
            if (weight < r.minWeight) continue;
            // Thin out towards the edge of a painted patch.
            if (chance > Clamp((weight - r.minWeight) / 0.25f + 0.2f, 0.0f, 1.0f)) continue;
        }
        const float ground = t.position.y + t.GetHeightAt(x, z);
        if (ground < r.minAltitude || ground > r.maxAltitude) continue;
        if (t.GetNormalAt(x, z).y < cosMaxSlope) continue;
        Plant p;
        p.pos = { x, ground, z };
        p.scale = r.minScale + (r.maxScale - r.minScale) * s;
        p.yaw = yaw;
        p.tintT = tint;
        p.heightVar = 0.85f + 0.3f * hv;
        uint32_t xb, zb;
        std::memcpy(&xb, &x, 4); std::memcpy(&zb, &z, 4);
        p.rank = (float)(Mix(xb, zb) & 0xffffff) / 16777216.0f * kRankMax;
        if (p.rank >= maxRank) continue;
        out.push_back(p);
    }
}

Matrix PlantMatrix(const Rule& r, const Plant& p) {
    const float base = kBaseHeight[(int)r.kind] * p.scale;
    const bool grassy = Thins(r.kind);
    const float width = grassy ? p.scale * 0.9f : base;
    const Vector3 scale = { width, grassy ? base * p.heightVar : base, width };
    Matrix m = gfx::ComposeTRS(scale, MatrixRotateY(p.yaw), p.pos);
    const Color tint = LerpColor(r.tintA, r.tintB, p.tintT);
    m.m3 = tint.r / 255.0f; m.m7 = tint.g / 255.0f; m.m11 = tint.b / 255.0f; m.m15 = p.rank;   // tint and fade rank ride in the w of the columns
    return m;
}

void BuildCell(const BasicTerrain& t, const Rule& r, float densityScale, int cx, int cz, float maxRank, Cell& cell) {
    std::vector<Plant> plants;
    ScatterCell(t, r, cx, cz, densityScale, maxRank, plants);
    std::sort(plants.begin(), plants.end(), [](const Plant& a, const Plant& b) { return a.rank < b.rank; });
    const float kCell = CellSize(r);
    cell.builtRank = maxRank;
    cell.min = { (float)cx * kCell, (float)cz * kCell };
    cell.max = { cell.min.x + kCell, cell.min.y + kCell };
    cell.count = (int)plants.size();
    if (plants.empty()) return;
    std::vector<Matrix> xf;
    xf.reserve(plants.size());
    cell.bottom = 1e9f; cell.top = -1e9f;
    cell.rank.clear();
    cell.rank.reserve(plants.size());
    for (const Plant& p : plants) {
        xf.push_back(PlantMatrix(r, p));
        cell.rank.push_back(p.rank);
        cell.bottom = std::min(cell.bottom, p.pos.y);
        cell.top = std::max(cell.top, p.pos.y + kBaseHeight[(int)r.kind] * p.scale);
    }
    cell.vbo = gfx::CreateInstanceBuffer(xf);
    if (!cell.vbo) cell.count = 0;
}

// Is any of the cell inside the view? A cone round the view direction, wide enough for the whole frame.
bool CellInView(const Camera3D& cam, const Cell& c, float fwdX, float fwdY, float fwdZ, float tanHalf) {
    const float cx = (c.min.x + c.max.x) * 0.5f, cz = (c.min.y + c.max.y) * 0.5f, cy = (c.bottom + c.top) * 0.5f;
    const float radius = (c.max.x - c.min.x) * 0.71f + (c.top - c.bottom) * 0.5f;
    const float vx = cx - cam.position.x, vy = cy - cam.position.y, vz = cz - cam.position.z;
    const float ahead = vx * fwdX + vy * fwdY + vz * fwdZ;
    if (ahead < -radius) return false;
    const float lx = vx - fwdX * ahead, ly = vy - fwdY * ahead, lz = vz - fwdZ * ahead;
    const float lateral = std::sqrt(lx * lx + ly * ly + lz * lz);
    return lateral <= tanHalf * std::max(ahead, 0.0f) + radius;
}

float DistanceToCell(const Camera3D& cam, const Cell& c) {
    const float dx = std::max({ c.min.x - cam.position.x, 0.0f, cam.position.x - c.max.x });
    const float dz = std::max({ c.min.y - cam.position.z, 0.0f, cam.position.z - c.max.y });
    return std::sqrt(dx * dx + dz * dz);
}

float DistanceToRect(const Camera3D& cam, float x0, float z0, float kCell) {
    const float dx = std::max({ x0 - cam.position.x, 0.0f, cam.position.x - (x0 + kCell) });
    const float dz = std::max({ z0 - cam.position.z, 0.0f, cam.position.z - (z0 + kCell) });
    return std::sqrt(dx * dx + dz * dz);
}

} // namespace

// ---- public ----------------------------------------------------------------

const char* KindName(Kind k) { return (int)k >= 0 && (int)k < (int)Kind::Count ? kKindNames[(int)k] : ""; }

bool KindFromName(const char* name, Kind& out) {
    if (!name) return false;
    for (int i = 0; i < (int)Kind::Count; i++) {
        const char* a = name; const char* b = kKindNames[i];
        while (*a && *b && std::tolower((unsigned char)*a) == std::tolower((unsigned char)*b)) { ++a; ++b; }
        if (!*a && !*b) { out = (Kind)i; return true; }
    }
    return false;
}

bool SameRule(const Rule& a, const Rule& b) {
    auto same = [](Color x, Color y) { return x.r == y.r && x.g == y.g && x.b == y.b; };
    return a.kind == b.kind && a.enabled == b.enabled && a.layer == b.layer && a.minWeight == b.minWeight && a.density == b.density &&
           a.minScale == b.minScale && a.maxScale == b.maxScale && a.maxSlope == b.maxSlope && a.minAltitude == b.minAltitude &&
           a.maxAltitude == b.maxAltitude && a.viewDistance == b.viewDistance && a.seed == b.seed && same(a.tintA, b.tintA) && same(a.tintB, b.tintB) && a.painted == b.painted;
}

Rule DefaultRule(Kind kind) {
    Rule r;
    r.kind = kind;
    switch (kind) {
        case Kind::Grass:   r.density = 600.0f; r.minScale = 0.7f; r.maxScale = 1.3f; r.maxSlope = 40.0f; r.viewDistance = 250.0f;
                            r.tintA = { 255, 255, 235, 255 }; r.tintB = { 215, 255, 200, 255 }; r.fadeStart = 0.2f; r.farKeep = 0.1f; break;
        case Kind::Flowers: r.density = 40.0f;  r.minScale = 0.8f; r.maxScale = 1.2f; r.maxSlope = 30.0f; r.viewDistance = 150.0f;
                            r.tintA = { 255, 120, 140, 255 }; r.tintB = { 255, 235, 90, 255 }; r.seed = 2; r.fadeStart = 0.35f; break;
        case Kind::Bush:    r.density = 1.5f;   r.minScale = 0.7f; r.maxScale = 1.5f; r.maxSlope = 35.0f; r.viewDistance = 300.0f;
                            r.tintA = { 230, 255, 220, 255 }; r.tintB = { 255, 255, 230, 255 }; r.seed = 3; r.fadeStart = 0.6f; r.farKeep = 0.5f; break;
        case Kind::Tree:    r.density = 0.6f;   r.minScale = 0.8f; r.maxScale = 1.4f; r.maxSlope = 30.0f; r.viewDistance = 600.0f;
                            r.tintA = { 235, 255, 225, 255 }; r.tintB = { 255, 250, 215, 255 }; r.seed = 4; r.fadeStart = 0.7f; r.farKeep = 0.7f; break;
        case Kind::Pine:    r.density = 0.7f;   r.minScale = 0.8f; r.maxScale = 1.5f; r.maxSlope = 38.0f; r.viewDistance = 600.0f;
                            r.tintA = { 235, 245, 235, 255 }; r.tintB = { 255, 255, 255, 255 }; r.seed = 5; r.fadeStart = 0.7f; r.farKeep = 0.7f; break;
        case Kind::Rock:    r.density = 0.5f;   r.minScale = 0.5f; r.maxScale = 2.2f; r.maxSlope = 60.0f; r.viewDistance = 400.0f; r.layer = -1;
                            r.tintA = { 230, 225, 220, 255 }; r.tintB = { 255, 255, 255, 255 }; r.seed = 6; r.fadeStart = 0.6f; r.farKeep = 0.5f; break;
        default: break;
    }
    return r;
}

Settings& For(BasicTerrain* terrain) { return SettingsMap()[terrain]; }

bool Has(const BasicTerrain* terrain) {
    auto it = SettingsMap().find(terrain);
    return it != SettingsMap().end() && !it->second.rules.empty();
}

void Forget(const BasicTerrain* terrain) {
    auto st = StateMap().find(terrain);
    if (st != StateMap().end()) { FreeCells(st->second); StateMap().erase(st); }
    SettingsMap().erase(terrain);
}

void Invalidate(const BasicTerrain* terrain) {
    auto st = StateMap().find(terrain);
    if (st != StateMap().end()) FreeCells(st->second);
}

void Scatter(const BasicTerrain& terrain, const Rule& rule, float minX, float minZ, float maxX, float maxZ, std::vector<Vector3>& positions) {
    std::vector<Plant> plants;
    const float kCell = CellSize(rule);
    auto sit = SettingsMap().find(&terrain);
    const float densityScale = sit != SettingsMap().end() ? sit->second.density : 1.0f;
    for (int cz = (int)std::floor(minZ / kCell); cz <= (int)std::floor(maxZ / kCell); cz++)
        for (int cx = (int)std::floor(minX / kCell); cx <= (int)std::floor(maxX / kCell); cx++) {
            plants.clear();
            ScatterCell(terrain, rule, cx, cz, densityScale, 2.0f, plants);
            for (const Plant& p : plants)
                if (p.pos.x >= minX && p.pos.x <= maxX && p.pos.z >= minZ && p.pos.z <= maxZ) positions.push_back(p.pos);
        }
}

// ---- the foliage brush -----------------------------------------------------

namespace {
const BasicTerrain* g_paintTerrain = nullptr;
int g_paintRule = -1;

Rule* PaintedRule(const BasicTerrain* t, int rule) {
    auto it = SettingsMap().find(t);
    if (it == SettingsMap().end() || rule < 0 || rule >= (int)it->second.rules.size()) return nullptr;
    return &it->second.rules[(size_t)rule];
}

// Marks the cells of one rule that touch a rectangle: they are placed again, in place, before the next draw (so a
// painted patch never blinks out while it waits).
void MarkDirty(const BasicTerrain* t, int rule, float x0, float z0, float x1, float z1) {
    const Rule* rr = PaintedRule(t, rule);
    auto st = StateMap().find(t);
    if (!rr || st == StateMap().end()) return;
    const float kCell = CellSize(*rr);
    for (int cz = (int)std::floor(z0 / kCell); cz <= (int)std::floor(z1 / kCell); cz++)
        for (int cx = (int)std::floor(x0 / kCell); cx <= (int)std::floor(x1 / kCell); cx++)
            st->second.dirty.insert({ rule, cx, cz });
}

// Drops one rule's cells that touch a rectangle, so they are placed again from the new mask.
void DropCells(const BasicTerrain* t, int rule, float x0, float z0, float x1, float z1) {
    const Rule* rr = PaintedRule(t, rule);
    if (!rr) return;
    const float kCell = CellSize(*rr);
    auto st = StateMap().find(t);
    if (st == StateMap().end() || rule < 0 || rule >= (int)st->second.cells.size()) return;
    auto& cells = st->second.cells[(size_t)rule];
    for (int cz = (int)std::floor(z0 / kCell); cz <= (int)std::floor(z1 / kCell); cz++)
        for (int cx = (int)std::floor(x0 / kCell); cx <= (int)std::floor(x1 / kCell); cx++) {
            auto it = cells.find(CellKey(cx, cz));
            if (it == cells.end()) continue;
            if (it->second.vbo) gfx::DestroyInstanceBuffer(it->second.vbo);
            g_instances -= it->second.count;
            cells.erase(it);
        }
}

} // namespace

void SetPaintTarget(const BasicTerrain* terrain, int rule) {
    g_paintTerrain = rule >= 0 ? terrain : nullptr;
    g_paintRule = g_paintTerrain ? rule : -1;
}

bool PaintTarget(const BasicTerrain* terrain, int* rule) {
    if (!terrain || terrain != g_paintTerrain || !PaintedRule(terrain, g_paintRule)) return false;
    if (rule) *rule = g_paintRule;
    return true;
}

void Paint(BasicTerrain* t, float worldX, float worldZ, float radius, float amount, float hardness, bool erase) {
    Rule* r = PaintedRule(t, g_paintTerrain == t ? g_paintRule : -1);
    if (!r) return;
    r->painted = true;
    if (!r->mask) r->mask = std::make_shared<std::vector<uint8_t>>((size_t)kMaskRes * kMaskRes, (uint8_t)0);
    std::vector<uint8_t>& m = *r->mask;
    const float extentX = t->GetWidth() * t->GetScale(), extentZ = t->GetDepth() * t->GetScale();
    const float texX = extentX / kMaskRes, texZ = extentZ / kMaskRes;
    const float cx = (worldX - t->position.x) / texX + kMaskRes * 0.5f, cz = (worldZ - t->position.z) / texZ + kMaskRes * 0.5f;
    const float rx = radius / texX, rz = radius / texZ;
    const int minX = std::max(0, (int)std::floor(cx - rx)), maxX = std::min(kMaskRes - 1, (int)std::ceil(cx + rx));
    const int minZ = std::max(0, (int)std::floor(cz - rz)), maxZ = std::min(kMaskRes - 1, (int)std::ceil(cz + rz));
    const float rate = Clamp(amount, 0.0f, 1.0f);   // share of full coverage this stamp adds at its middle
    for (int z = minZ; z <= maxZ; z++)
        for (int x = minX; x <= maxX; x++) {
            const float dx = ((float)x + 0.5f - cx) / rx, dz = ((float)z + 0.5f - cz) / rz;
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d > 1.0f) continue;
            const float edge = d <= hardness ? 1.0f : 1.0f - (d - hardness) / std::max(1.0f - hardness, 0.01f);
            const float delta = rate * edge * 255.0f;
            uint8_t& v = m[(size_t)z * kMaskRes + (size_t)x];
            v = (uint8_t)Clamp(std::round((float)v + (erase ? -delta : delta)), 0.0f, 255.0f);
        }
    MarkDirty(t, g_paintRule, worldX - radius - 1.0f, worldZ - radius - 1.0f, worldX + radius + 1.0f, worldZ + radius + 1.0f);
}

void FillMask(BasicTerrain* t, int rule, bool filled) {
    Rule* r = PaintedRule(t, rule);
    if (!r) return;
    r->painted = true;
    if (!r->mask) r->mask = std::make_shared<std::vector<uint8_t>>((size_t)kMaskRes * kMaskRes, (uint8_t)0);
    std::fill(r->mask->begin(), r->mask->end(), filled ? (uint8_t)255 : (uint8_t)0);
    const float hx = t->GetWidth() * t->GetScale() * 0.5f, hz = t->GetDepth() * t->GetScale() * 0.5f;
    DropCells(t, rule, t->position.x - hx, t->position.z - hz, t->position.x + hx, t->position.z + hz);
}

void Draw(const Camera3D& camera, bool shadowPass) {
    if (SettingsMap().empty() || !gfx::detail::ShapeInstancedReady() || gfx::IsInReflectionPass()) return;
    BuildMeshes();
    if (g_white.id == 0) return;

    Material& mat = gfx::detail::ShapeInstancedMaterial();
    mat.maps[MATERIAL_MAP_DIFFUSE].texture = g_white;
    mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    if (g_windLoc == -2) g_windLoc = GetShaderLocation(mat.shader, "foliageWind");
    if (g_fadeLoc == -2) g_fadeLoc = GetShaderLocation(mat.shader, "foliageFade");
    if (g_camLoc == -2) g_camLoc = GetShaderLocation(mat.shader, "foliageCam");
    bool fadeSet = false;

    const double now = GetTime();
    if (!shadowPass) {
        g_drawn = 0;
        gfx::detail::SetupShapeInstancedLighting();
    }

    const Vector3 fwd = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    const float aspect = (float)GetScreenWidth() / (float)std::max(1, GetScreenHeight());
    const float tanHalf = std::tan(camera.fovy * 0.5f * DEG2RAD) * std::sqrt(1.0f + aspect * aspect);
    int builds = 0;
    bool windSet = false;

    for (BasicTerrain* t : BasicTerrain::GetInstances()) {
        auto sit = SettingsMap().find(t);
        if (sit == SettingsMap().end() || !sit->second.enabled || sit->second.rules.empty()) continue;
        Settings& set = sit->second;
        TerrainState& st = StateMap()[t];

        // The ground changed (sculpting, painting, moving): put the plants again once it settles.
        const float sig = Signature(*t);
        if (!st.primed) { st.seenRevision = t->GetRevision(); st.seenSignature = sig; st.primed = true; }
        if (t->GetRevision() != st.seenRevision || sig != st.seenSignature) {
            st.seenRevision = t->GetRevision(); st.seenSignature = sig;
            st.changedAt = now; st.pending = true;
        }
        if (st.pending && now - st.changedAt > 0.4) { FreeCells(st); st.pending = false; }
        if (st.cells.size() != set.rules.size()) { FreeCells(st); st.cells.resize(set.rules.size()); }

        if (!windSet && g_windLoc >= 0) {
            const gfx::LightingSettings& L = gfx::Lighting();
            const float yaw = L.windDirection * DEG2RAD;   // compass degrees: 0 = north (-z), 90 = east (+x)
            const float strength = Clamp(L.windSpeed / 18.0f, 0.15f, 1.6f) * set.wind;
            const Vector4 wind = { (float)now, strength, std::sin(yaw), -std::cos(yaw) };
            SetShaderValue(mat.shader, g_windLoc, &wind, SHADER_UNIFORM_VEC4);
            windSet = true;
        }

        for (size_t ri = 0; ri < set.rules.size(); ri++) {
            const Rule& r = set.rules[ri];
            if (!r.enabled || r.density <= 0.0f) continue;
            if (shadowPass && !CastsShadow(r.kind)) continue;
            const float kCell = CellSize(r);
            const float full = r.viewDistance * set.distance;
            const float range = shadowPass ? std::min(full, 90.0f) : full;
            auto& cells = st.cells[ri];
            if (g_fadeLoc >= 0 && g_camLoc >= 0) {
                const Vector4 fade = { Clamp(r.fadeStart, 0.0f, 0.98f) * range, range, r.farKeep, 1.0f };
                const Vector4 cam = { camera.position.x, camera.position.z, Thins(r.kind) ? 2.2f : 1.0f, 0.0f };   // z: how much the thinning plants grow to hold the ground
                SetShaderValue(mat.shader, g_fadeLoc, &fade, SHADER_UNIFORM_VEC4);
                SetShaderValue(mat.shader, g_camLoc, &cam, SHADER_UNIFORM_VEC4);
                fadeSet = true;
            }

            const int minCx = (int)std::floor((camera.position.x - range) / kCell), maxCx = (int)std::floor((camera.position.x + range) / kCell);
            const int minCz = (int)std::floor((camera.position.z - range) / kCell), maxCz = (int)std::floor((camera.position.z + range) / kCell);
            if (!shadowPass) {
                for (auto d = st.dirty.begin(); d != st.dirty.end();) {
                    if ((*d)[0] != (int)ri) { ++d; continue; }
                    const int cx = (*d)[1], cz = (*d)[2];
                    d = st.dirty.erase(d);
                    const float dist = DistanceToRect(camera, (float)cx * kCell, (float)cz * kCell, kCell);
                    Cell& cell = cells[CellKey(cx, cz)];
                    if (cell.vbo) gfx::DestroyInstanceBuffer(cell.vbo);
                    g_instances -= cell.count;
                    cell = Cell{};
                    if (dist > range) { cells.erase(CellKey(cx, cz)); continue; }
                    BuildCell(*t, r, set.density, cx, cz, std::min(RankThreshold(r, dist, range) + 0.35f, 2.0f), cell);
                    g_instances += cell.count;
                }
            }
            struct Todo { float dist; int cx, cz; };
            std::vector<Todo> todo;   // cells to place (or to fill in with more plants as the camera nears), nearest first
            for (int cz = minCz; cz <= maxCz; cz++) {
                for (int cx = minCx; cx <= maxCx; cx++) {
                    const float dist = DistanceToRect(camera, (float)cx * kCell, (float)cz * kCell, kCell);
                    if (dist > range) continue;
                    auto it = cells.find(CellKey(cx, cz));
                    // A far cell holds only the plants that survive the fade there; nearer, it needs more.
                    const float need = std::min(RankThreshold(r, dist, range) + 0.2f, 2.0f);
                    if (!shadowPass && (it == cells.end() || it->second.builtRank < std::min(need, 1.0f) - 1e-3f)) {
                        // Look before building: cells behind the camera wait.
                        Cell probe;
                        probe.min = { (float)cx * kCell, (float)cz * kCell };
                        probe.max = { probe.min.x + kCell, probe.min.y + kCell };
                        probe.bottom = t->position.y + t->GetMinHeight(); probe.top = t->position.y + t->GetMaxHeight() + kBaseHeight[(int)r.kind] * r.maxScale;
                        if (CellInView(camera, probe, fwd.x, fwd.y, fwd.z, tanHalf) || dist <= kCell) todo.push_back({ dist, cx, cz });
                    }
                    if (it == cells.end()) continue;
                    Cell& cell = it->second;
                    if (cell.count <= 0 || !cell.vbo) continue;
                    if (!shadowPass && !CellInView(camera, cell, fwd.x, fwd.y, fwd.z, tanHalf)) continue;   // nothing behind or beside you is drawn
                    // Only the plants that survive the fade at the cell's nearest edge (they come first); the shader
                    // fades each one at its own distance.
                    const float keepBelow = RankThreshold(r, DistanceToCell(camera, cell), range);
                    const int n = (int)(std::lower_bound(cell.rank.begin(), cell.rank.end(), keepBelow) - cell.rank.begin());
                    if (n <= 0) continue;
                    // Farther cells use a lower-detail mesh (the distances are the rule's, as shares of its draw distance).
                    const float cellDist = DistanceToCell(camera, cell);
                    const int lod = cellDist >= r.lod2 * range ? 2 : (cellDist >= r.lod1 * range ? 1 : 0);
                    const Mesh& mesh = g_mesh[(int)r.kind][lod];
                    DrawMeshInstancedBuffer(mesh, mat, cell.vbo, n);
                    gfx::IncrementDrawCallCount(1);
                    gfx::AddMeshCount(1);
                    gfx::AddTriangleCount(mesh.triangleCount * n);
                    if (!shadowPass) g_drawn += n;
                }
            }
            std::sort(todo.begin(), todo.end(), [](const Todo& a, const Todo& b) { return a.dist < b.dist; });
            for (const Todo& job : todo) {
                if (builds >= 1 && GetTime() - now > kBuildSeconds) break;
                Cell& cell = cells[CellKey(job.cx, job.cz)];
                if (cell.vbo) gfx::DestroyInstanceBuffer(cell.vbo);
                g_instances -= cell.count;
                cell = Cell{};
                BuildCell(*t, r, set.density, job.cx, job.cz, std::min(RankThreshold(r, job.dist, range) + 0.35f, 2.0f), cell);
                g_instances += cell.count;
                builds++;
            }

            // Free what the camera has left far behind.
            for (auto it = cells.begin(); it != cells.end();) {
                if (DistanceToCell(camera, it->second) > range * 1.4f + kCell) {
                    if (it->second.vbo) gfx::DestroyInstanceBuffer(it->second.vbo);
                    g_instances -= it->second.count;
                    it = cells.erase(it);
                } else ++it;
            }
        }
    }

    if (fadeSet) {   // and keeps its full size
        const Vector4 off = { 0.0f, 0.0f, 0.0f, 0.0f };
        SetShaderValue(mat.shader, g_fadeLoc, &off, SHADER_UNIFORM_VEC4);
    }
    if (windSet) {   // every other instanced shape sits still
        const Vector4 off = { 0.0f, 0.0f, 0.0f, 0.0f };
        SetShaderValue(mat.shader, g_windLoc, &off, SHADER_UNIFORM_VEC4);
    }
}

void Shutdown() {
    for (auto& [t, st] : StateMap()) FreeCells(st);
    StateMap().clear();
    SettingsMap().clear();
    if (g_meshReady) {
        for (auto& kind : g_mesh) for (Mesh& m : kind) { UnloadMesh(m); m = {}; }
        g_meshReady = false;
    }
    if (g_white.id) { UnloadTexture(g_white); g_white = {}; }
    g_instances = g_drawn = 0;
    g_windLoc = -2;
    g_fadeLoc = -2;
    g_camLoc = -2;
}

int MeshTriangles(Kind kind, int lod) {
    BuildMeshes();
    return g_mesh[(int)kind][std::clamp(lod, 0, kLods - 1)].triangleCount;
}

int InstanceCount() { return g_instances; }
int DrawnCount() { return g_drawn; }

// ---- persistence -----------------------------------------------------------

bool Write(std::ostream& out) {
    const auto& terrains = BasicTerrain::GetInstances();
    out << "FOLIAGE 3\n";
    for (size_t i = 0; i < terrains.size(); i++) {
        auto it = SettingsMap().find(terrains[i]);
        if (it == SettingsMap().end() || it->second.rules.empty()) continue;
        const Settings& s = it->second;
        out << "TERRAIN " << i << ' ' << (s.enabled ? 1 : 0) << ' ' << s.wind << ' ' << s.rules.size() << ' ' << s.density << ' ' << s.distance << '\n';
        for (const Rule& r : s.rules) {
            out << "RULE " << KindName(r.kind) << ' ' << (r.enabled ? 1 : 0) << ' ' << r.layer << ' ' << r.minWeight << ' ' << r.density << ' '
                << r.minScale << ' ' << r.maxScale << ' ' << r.maxSlope << ' ' << r.minAltitude << ' ' << r.maxAltitude << ' ' << r.viewDistance << ' '
                << r.seed << ' ' << (int)r.tintA.r << ' ' << (int)r.tintA.g << ' ' << (int)r.tintA.b << ' '
                << (int)r.tintB.r << ' ' << (int)r.tintB.g << ' ' << (int)r.tintB.b << ' '
                << r.fadeStart << ' ' << r.farKeep << ' ' << (r.painted ? 1 : 0) << ' ' << r.lod1 << ' ' << r.lod2 << ' ';
            // The paint mask, run-length encoded as (count, value) pairs.
            std::vector<std::pair<int, int>> runs;
            if (r.painted && r.mask)
                for (uint8_t v : *r.mask) {
                    if (!runs.empty() && runs.back().second == v) runs.back().first++;
                    else runs.push_back({ 1, v });
                }
            out << runs.size();
            for (auto& [n, v] : runs) out << ' ' << n << ' ' << v;
            out << '\n';
        }
    }
    return (bool)out;
}

int Read(std::istream& in) {
    const auto& terrains = BasicTerrain::GetInstances();
    std::string tag;
    int version = 0;
    if (!(in >> tag >> version) || tag != "FOLIAGE" || version < 1 || version > 3) return 0;
    int configured = 0;
    BasicTerrain* current = nullptr;
    int remaining = 0;
    while (in >> tag) {
        if (tag == "TERRAIN") {
            size_t index = 0; int enabled = 1; float wind = 0.6f;
            if (!(in >> index >> enabled >> wind >> remaining)) break;
            float density = 1.0f, distance = 1.0f;
            if (version >= 2 && !(in >> density >> distance)) break;
            current = index < terrains.size() ? terrains[index] : nullptr;
            if (current) {
                Settings& s = For(current);
                s = Settings{};
                s.enabled = enabled != 0;
                s.wind = Clamp(wind, 0.0f, 3.0f);
                s.density = Clamp(density, 0.05f, 4.0f);
                s.distance = Clamp(distance, 0.2f, 4.0f);
                configured++;
                Invalidate(current);
            }
        } else if (tag == "RULE") {
            std::string kindName; Rule r; int enabled = 1, ar, ag, ab, br, bg, bb;
            if (!(in >> kindName >> enabled >> r.layer >> r.minWeight >> r.density >> r.minScale >> r.maxScale >> r.maxSlope >> r.minAltitude
                     >> r.maxAltitude >> r.viewDistance >> r.seed >> ar >> ag >> ab >> br >> bg >> bb)) break;
            if (version >= 2) {
                int painted = 0; size_t runs = 0;
                if (!(in >> r.fadeStart >> r.farKeep >> painted)) break;
                if (version >= 3 && !(in >> r.lod1 >> r.lod2)) break;
                if (!(in >> runs)) break;
                r.lod1 = std::clamp(r.lod1, 0.0f, 1.0f);
                r.lod2 = std::clamp(r.lod2, r.lod1, 1.0f);
                r.fadeStart = std::clamp(r.fadeStart, 0.0f, 0.98f);
                r.farKeep = std::clamp(r.farKeep, 0.0f, 1.0f);
                r.painted = painted != 0;
                if (runs > 0) {
                    auto mask = std::make_shared<std::vector<uint8_t>>((size_t)kMaskRes * kMaskRes, (uint8_t)0);
                    size_t at = 0;
                    for (size_t i = 0; i < runs; i++) {
                        int n = 0, v = 0;
                        if (!(in >> n >> v)) return configured;
                        for (int k = 0; k < n && at < mask->size(); k++) (*mask)[at++] = (uint8_t)std::clamp(v, 0, 255);
                    }
                    r.mask = mask;
                }
            }
            if (!KindFromName(kindName.c_str(), r.kind) || !current) continue;
            r.enabled = enabled != 0;
            r.tintA = { (unsigned char)Clamp((float)ar, 0, 255), (unsigned char)Clamp((float)ag, 0, 255), (unsigned char)Clamp((float)ab, 0, 255), 255 };
            r.tintB = { (unsigned char)Clamp((float)br, 0, 255), (unsigned char)Clamp((float)bg, 0, 255), (unsigned char)Clamp((float)bb, 0, 255), 255 };
            r.layer = std::clamp(r.layer, -1, 3);
            r.density = std::clamp(r.density, 0.0f, 2000.0f);
            r.viewDistance = std::clamp(r.viewDistance, 5.0f, 3000.0f);
            For(current).rules.push_back(r);
        } else break;
    }
    return configured;
}

bool SaveFile(const std::string& path) {
    bool any = false;
    for (BasicTerrain* t : BasicTerrain::GetInstances()) any = any || Has(t);
    if (!any) return false;
    std::ofstream out(path);
    return out && Write(out);
}

int LoadFile(const std::string& path) {
    platform::InputStream in(path);
    if (!in) return 0;
    return Read(in);
}

} // namespace foliage
