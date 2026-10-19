// Adaptive BasicTerrain mesh must be watertight: every interior edge is shared by two triangles (no T-junction cracks).
// Opens a window, so it is not registered with ctest: build/tests/terrain_crack_test [terrain.bt]
#include "raylib.h"
#include "Engine.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#define private public
#include "../include/Terrain/BasicTerrain.hpp"
#undef private

static int g_fail = 0;

// Counts interior edges used by exactly one triangle, welding vertices by position.
static int CountCracks(const Mesh& m, int& tris) {
    typedef std::pair<int, int> Key;
    std::map<Key, int> weld; // (x,z) in mm
    std::vector<int> id(m.vertexCount);
    std::vector<Key> pos;
    int minX = 1 << 30, maxX = -(1 << 30), minZ = 1 << 30, maxZ = -(1 << 30);
    for (int i = 0; i < m.vertexCount; i++) {
        Key k((int)std::lround(m.vertices[i * 3] * 1000.0f), (int)std::lround(m.vertices[i * 3 + 2] * 1000.0f));
        auto it = weld.find(k);
        if (it == weld.end()) { it = weld.emplace(k, (int)pos.size()).first; pos.push_back(k); }
        id[i] = it->second;
        minX = std::min(minX, k.first); maxX = std::max(maxX, k.first);
        minZ = std::min(minZ, k.second); maxZ = std::max(maxZ, k.second);
    }
    std::map<Key, int> edges;
    tris = m.triangleCount;
    for (int t = 0; t < m.triangleCount; t++) {
        int v[3] = { id[m.indices[t * 3]], id[m.indices[t * 3 + 1]], id[m.indices[t * 3 + 2]] };
        for (int e = 0; e < 3; e++) {
            int a = v[e], b = v[(e + 1) % 3];
            if (a == b) continue;
            edges[Key(std::min(a, b), std::max(a, b))]++;
        }
    }
    int cracks = 0;
    for (auto& [k, n] : edges) {
        if (n != 1) continue;
        Key a = pos[k.first], b = pos[k.second];
        bool border = (a.first == minX && b.first == minX) || (a.first == maxX && b.first == maxX) ||
                      (a.second == minZ && b.second == minZ) || (a.second == maxZ && b.second == maxZ);
        if (!border) cracks++;
    }
    return cracks;
}

static void Check(const char* name, BasicTerrain& t) {
    if (!t.BuildAdaptiveMesh()) { std::printf("%s: adaptive fell back to dense (too many verts)\n", name); g_fail++; return; }
    int tris = 0;
    int cracks = CountCracks(t.mesh, tris);
    std::printf("%s: verts=%d tris=%d (dense tris=%d) interior-open-edges=%d\n", name, t.mesh.vertexCount, tris,
                (t.width - 1) * (t.depth - 1) * 2, cracks);
    if (cracks) g_fail++;
}

int main(int argc, char** argv) {
    Engine engine(640, 360, "terrain_crack_test", 60);
    engine.SetPlayerBuild(true);

    if (argc > 1) {
        BasicTerrain t;
        if (!t.LoadFromFile(argv[1])) { std::printf("FAIL load %s\n", argv[1]); return 1; }
        Check(argv[1], t);
    }
    for (int i = 0; i < 3; i++) {
        BasicTerrain t(128, 128, 2.0f, 40.0f, 16.0f);
        t.GenerateFlat(0.0f);
        t.genBoxPos = { 0.0f, 12.0f, 0.0f };
        t.genBoxSize = { 256.0f, 30.0f, 256.0f };
        t.genHills = true;
        t.GenerateTerrainInBox();
        Check(("hills" + std::to_string(i)).c_str(), t);
    }
    std::printf(g_fail ? "FAILED\n" : "OK\n");
    return g_fail ? 1 : 0;
}
