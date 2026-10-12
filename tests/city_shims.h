#pragma once
#include "raylib.h"
#include "raymath.h"

// Test-only shims: replace the raylib rendering entry points that City.cpp
// references so generation logic can be exercised headlessly (no window, no GL).
// Every stub is a plain definition (no GPU work); they only record what the
// production code handed to raylib so the test can assert invariants.

#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {

// --- recorded from UploadMesh() so the test can verify mesh invariants ---
// The static city may be split into several 16-bit-safe indexed chunks, so the
// shim accumulates across every UploadMesh() call in a build and additionally
// tracks per-chunk maxima (vertexCount, largest local index).
static int g_chunkCount = 0;
static long long g_totalVertexCount = 0;
static long long g_totalTriangleCount = 0;
static int g_maxChunkVertexCount = 0;
static int g_maxChunkIndex = -1;
static bool g_anyIndicesNull = false;
static float g_minX, g_maxX, g_minZ, g_maxZ;      // union across chunks
static bool g_boundsValid = false;
static int g_nonFiniteVerts = 0;
static std::vector<float> g_capVerts;    // x,y,z per corner, 9 floats / triangle
static std::vector<unsigned char> g_capColors; // r,g,b per corner, 9 bytes / triangle

void UploadMesh(Mesh *mesh, bool dynamic) {
    (void)dynamic;
    if (!mesh) return;
    // Capture every uploaded triangle (positions + vertex color) so the harness
    // can re-render the built city from CPU-side data for visual inspection.
    if (mesh->indices && mesh->vertices && mesh->colors) {
        for (int t = 0; t < mesh->triangleCount; t++) {
            for (int k = 0; k < 3; k++) {
                int id = mesh->indices[t * 3 + k];
                if (id < 0 || id >= mesh->vertexCount) continue;
                g_capVerts.push_back(mesh->vertices[(size_t)id * 3 + 0]);
                g_capVerts.push_back(mesh->vertices[(size_t)id * 3 + 1]);
                g_capVerts.push_back(mesh->vertices[(size_t)id * 3 + 2]);
                g_capColors.push_back(mesh->colors[(size_t)id * 4 + 0]);
                g_capColors.push_back(mesh->colors[(size_t)id * 4 + 1]);
                g_capColors.push_back(mesh->colors[(size_t)id * 4 + 2]);
            }
        }
    }
    g_chunkCount++;
    g_totalVertexCount += mesh->vertexCount;
    g_totalTriangleCount += mesh->triangleCount;
    if (mesh->vertexCount > g_maxChunkVertexCount) g_maxChunkVertexCount = mesh->vertexCount;
    if (mesh->indices == NULL) g_anyIndicesNull = true;
    else {
        for (int i = 0; i < mesh->triangleCount * 3; i++) {
            if ((int)mesh->indices[i] > g_maxChunkIndex) g_maxChunkIndex = (int)mesh->indices[i];
        }
    }
    // Spatial coverage: scan positions so the test can assert the union of all
    // chunk bounds truly spans the whole city (both sides), i.e. no
    // 16-bit-index-clamp folding anywhere.
    if (mesh->vertices && mesh->vertexCount > 0) {
        for (int i = 0; i < mesh->vertexCount; i++) {
            float px = mesh->vertices[i * 3 + 0];
            float pz = mesh->vertices[i * 3 + 2];
            if (!(px == px) || !(pz == pz)) { g_nonFiniteVerts++; continue; }
            if (!g_boundsValid) { g_minX = g_maxX = px; g_minZ = g_maxZ = pz; g_boundsValid = true; }
            else {
                if (px < g_minX) g_minX = px;
                if (px > g_maxX) g_maxX = px;
                if (pz < g_minZ) g_minZ = pz;
                if (pz > g_maxZ) g_maxZ = pz;
            }
        }
    }
}

static Material g_staticMaterial[1];
static MaterialMap g_staticMaterialMap[8];
static bool g_materialInit = false;

Model LoadModelFromMesh(Mesh mesh) {
    Model m = { 0 };
    m.meshCount = 1;
    m.meshes = &mesh; // points at the (temporary) heap copies; Draw() is never
                      // called in the harness so aliasing is harmless here.
    if (!g_materialInit) {
        std::memset(g_staticMaterial, 0, sizeof(g_staticMaterial));
        std::memset(g_staticMaterialMap, 0, sizeof(g_staticMaterialMap));
        g_staticMaterial[0].maps = g_staticMaterialMap; // raylib 6: maps is a pointer
        g_materialInit = true;
    }
    m.materialCount = 1;
    m.materials = g_staticMaterial;
    m.transform = MatrixIdentity();
    return m;
}

void UnloadModel(Model model) { (void)model; }
void DrawModel(Model model, Vector3 position, float scale, Color tint) {
    (void)model; (void)position; (void)scale; (void)tint;
}
void DrawCube(Vector3 position, float width, float height, float length, Color color) {
    (void)position; (void)width; (void)height; (void)length; (void)color;
}
void DrawLine3D(Vector3 startPos, Vector3 endPos, Color color) {
    (void)startPos; (void)endPos; (void)color;
}
RayCollision GetRayCollisionSphere(Ray ray, Vector3 center, float radius) {
    (void)ray; (void)center; (void)radius;
    RayCollision c = { 0 };
    return c;
}

} // extern "C"

namespace gfx {
Shader& GetLitShader() {
    static Shader s = { 0 };
    return s;
}
Shader& GetRoadShader() {
    static Shader s = { 0 };
    return s;
}
Texture2D GetDefaultTexture() {
    Texture2D t = { 0 };
    return t;
}
bool IsInShadowPass() { return false; }
void DrawCityInstances(Mesh mesh, const std::vector<Matrix>& transforms, int start, int count, Color tint) {
    (void)mesh; (void)transforms; (void)start; (void)count; (void)tint;
}
Mesh GetCityShapeMesh(int shape) {
    (void)shape;
    Mesh m = { 0 };
    return m;
}
} // namespace gfx

namespace ui {
bool IsPlayActive() { return false; }
} // namespace ui

namespace city {
CityEditorState& GetCityEditorState() {
    static CityEditorState s;
    return s;
}
} // namespace city

void Engine::RemoveEntity(Entity* e) { (void)e; }

// Expose the recorded values to the test body.
void TestResetMeshStats() {
    g_chunkCount = 0;
    g_totalVertexCount = 0;
    g_totalTriangleCount = 0;
    g_maxChunkVertexCount = 0;
    g_maxChunkIndex = -1;
    g_anyIndicesNull = false;
    g_boundsValid = false;
    g_nonFiniteVerts = 0;
    g_minX = g_maxX = g_minZ = g_maxZ = 0.0f;
    g_capVerts.clear();
    g_capColors.clear();
}
int TestChunkCount() { return g_chunkCount; }
long long TestTotalVertexCount() { return g_totalVertexCount; }
long long TestTotalTriangleCount() { return g_totalTriangleCount; }
int TestMaxChunkVertexCount() { return g_maxChunkVertexCount; }
int TestMaxChunkIndex() { return g_maxChunkIndex; }
bool TestAnyIndicesNull() { return g_anyIndicesNull; }
bool TestLastBoundsValid() { return g_boundsValid; }
int TestLastNonFiniteVerts() { return g_nonFiniteVerts; }
float TestLastMinX() { return g_minX; }
float TestLastMaxX() { return g_maxX; }
float TestLastMinZ() { return g_minZ; }
float TestLastMaxZ() { return g_maxZ; }
// Access to the captured triangles (x,y,z per vertex + r,g,b per vertex).
const std::vector<float>& TestCapturedVerts() { return g_capVerts; }
const std::vector<unsigned char>& TestCapturedColors() { return g_capColors; }