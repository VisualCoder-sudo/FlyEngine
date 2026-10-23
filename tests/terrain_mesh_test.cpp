// Terrain mesh / sculpt review pass: 16-bit index overflow on big grids, Terrain ctor sanitising, brush NaN guards,
// erosion keeping shared chunk edges identical. Opens a window (engine ctor), so it is not registered with ctest:
// build/tests/terrain_mesh_test
#include "raylib.h"
#include "Engine.hpp"
#define private public
#include "../include/Terrain/BasicTerrain.hpp"
#undef private
#include "../include/Terrain/Terrain.hpp"
#include "../include/Terrain/TerrainMesh.hpp"
#include "../include/Terrain/TerrainSculpt.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static bool MeshValid(const Mesh& m) {
    if (!m.vertices || !m.indices || m.vertexCount <= 0 || m.vertexCount > 65535) return false;
    for (int i = 0; i < m.triangleCount * 3; i++) if (m.indices[i] >= m.vertexCount) return false;
    for (int i = 0; i < m.vertexCount * 3; i++) if (!std::isfinite(m.vertices[i]) || !std::isfinite(m.normals[i])) return false;
    return true;
}

static bool AllFinite(terrain::Terrain& t) {
    for (auto& ch : t.chunks) for (float v : ch.heightmap) if (!std::isfinite(v)) return false;
    return true;
}

int main() {
    Engine engine(320, 240, "terrain_mesh_test", 60);
    engine.SetPlayerBuild(true);

    // BasicTerrain 600x600 rough grid: adaptive may bail, dense fallback must stay under 65535 vertices.
    {
        BasicTerrain t(600, 600, 1.0f, 100.0f, 8.0f);
        float* h = t.GetHeightData();
        for (int i = 0; i < 600 * 600; i++) h[i] = (float)((i * 2654435761u) % 1000) * 0.01f;
        t.BuildDenseMesh();
        CHECK(MeshValid(t.mesh));
        CHECK(t.mesh.vertexCount > 10000);
        t.RebuildMesh();
        CHECK(MeshValid(t.mesh));
        float minX = 1e30f, maxX = -1e30f;
        for (int i = 0; i < t.mesh.vertexCount; i++) { minX = std::fmin(minX, t.mesh.vertices[i * 3]); maxX = std::fmax(maxX, t.mesh.vertices[i * 3]); }
        CHECK(std::fabs((maxX - minX) - 599.0f) < 0.01f); // full extent preserved
    }
    // Degenerate sizes are clamped, not divided by zero.
    { BasicTerrain t(0, 1, 1.0f, 10.0f, 1.0f); CHECK(t.width >= 2 && t.depth >= 2); t.BuildDenseMesh(); CHECK(MeshValid(t.mesh)); }

    // GenerateTerrainMesh with a 513 chunk resolution.
    {
        std::vector<float> hm(513 * 513, 1.0f);
        Mesh m = terrain::GenerateTerrainMesh(hm.data(), 513, 0.5f, 0, 10);
        CHECK(MeshValid(m));
        UnloadMesh(m);
    }

    // Terrain ctor: resolution / size sanitising.
    {
        terrain::Terrain a({ 0, 0, 0 }, 128.0f, 128.0f, 64, 33); CHECK(a.GetChunkResolution() == 33);
        terrain::Terrain b({ 0, 0, 0 }, 128.0f, 128.0f, 64, 64); CHECK(b.GetChunkResolution() == 65);
        terrain::Terrain c({ 0, 0, 0 }, 128.0f, 128.0f, 0, 5000); CHECK(c.GetChunkResolution() == 65);
        terrain::Terrain d({ 0, 0, 0 }, NAN, -5.0f, 64, 65); CHECK(d.gridWidth >= 1);
        CHECK(a.GetChunkAt(NAN, 0.0f) == nullptr);
    }

    // Brush guards + erosion edge consistency on a 2x2 chunk terrain.
    {
        terrain::Terrain t({ 0, 0, 0 }, 128.0f, 128.0f, 64, 33);
        for (auto& ch : t.chunks)
            for (size_t i = 0; i < ch.heightmap.size(); i++) ch.heightmap[i] = (float)((i * 2654435761u) % 100) * 0.05f;
        terrain::TerrainBrush b;
        b.radius = 0.0f; b.tool = terrain::TerrainTool::Raise;
        t.ApplyBrush(b, { 0, 0 }, 0.016f);
        CHECK(AllFinite(t));
        b.radius = 20.0f; b.tool = terrain::TerrainTool::Noise; b.noiseScale = 0.0f;
        t.ApplyBrush(b, { 0, 0 }, 0.016f);
        CHECK(AllFinite(t));

        b.erosionIterations = 5; b.erosionThermal = 1.0f; b.erosionHydraulic = 1.0f;
        terrain::ApplyErosionToRegion(t, -64, -64, 64, 64, b);
        // Chunk (0,0) right edge == chunk (1,0) left edge ; chunk (0,0) bottom edge == chunk (0,1) top edge.
        const int r = 33; float maxDiff = 0.0f;
        for (int k = 0; k < r; k++) {
            maxDiff = std::fmax(maxDiff, std::fabs(t.chunks[0].heightmap[k * r + (r - 1)] - t.chunks[1].heightmap[k * r + 0]));
            maxDiff = std::fmax(maxDiff, std::fabs(t.chunks[0].heightmap[(r - 1) * r + k] - t.chunks[2].heightmap[0 * r + k]));
        }
        std::printf("max shared-edge diff after erosion: %g\n", maxDiff);
        CHECK(maxDiff < 1e-5f);
    }

    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
