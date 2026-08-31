#include "TerrainPhysics.hpp"
#include "Terrain.hpp"
#include "TerrainTypes.hpp"
#include "raylib.h"
#include "raymath.h"
#include <vector>

namespace terrain {

// ============================================================================
// Box3D Integration
// ============================================================================

// Forward declare Box3D types
struct b3HeightField;
struct b3MeshShape;
struct b3Body;
struct b3ShapeDef;

extern "C" {
    // Box3D heightfield functions
    b3HeightField* b3CreateHeightField(int width, int depth, const float* heights, float minHeight, float maxHeight);
    void b3DestroyHeightField(b3HeightField* hf);
    
    // Box3D mesh shape functions
    b3MeshShape* b3CreateMeshShape(const float* vertices, int vertexCount, const int* indices, int indexCount);
    void b3DestroyMeshShape(b3MeshShape* shape);
    
    // Body functions
    b3Body* b3CreateStaticBody(void* world, const void* shape);
    void b3DestroyBody(void* world, b3Body* body);
    void b3SetBodyTransform(b3Body* body, const float* position, const float* rotation);
}

// Physics simulation interface (from engine)
static PhysicsSimInterface* g_physicsInterface = nullptr;

void SetPhysicsInterface(PhysicsSimInterface* iface) {
    g_physicsInterface = iface;
}

// ============================================================================
// Heightfield Physics
// ============================================================================

void CreatePhysicsHeightfield(Terrain& terrain) {
    if (!g_physicsInterface || !g_physicsInterface->world) return;
    
    // Combine all chunks into single heightfield
    int gridW = terrain.gridWidth;
    int gridH = terrain.gridDepth;
    int res = terrain.chunkResolution;
    int totalW = gridW * (res - 1) + 1;
    int totalH = gridH * (res - 1) + 1;
    
    std::vector<float> fullHeightmap(totalW * totalH);
    
    for (int gz = 0; gz < gridH; gz++) {
        for (int gx = 0; gx < gridW; gx++) {
            const TerrainChunk& chunk = terrain.chunks[gz * gridW + gx];
            
            for (int z = 0; z < res; z++) {
                for (int x = 0; x < res; x++) {
                    int dstX = gx * (res - 1) + x;
                    int dstZ = gz * (res - 1) + z;
                    if (dstX < totalW && dstZ < totalH) {
                        fullHeightmap[dstZ * totalW + dstX] = chunk.heightmap[z * res + x];
                    }
                }
            }
        }
    }
    
    // Create heightfield
    b3HeightField* hf = b3CreateHeightField(totalW, totalH, fullHeightmap.data(), terrain.minHeight, terrain.maxHeight);
    
    if (hf && g_physicsInterface->createStaticBody) {
        terrain.physicsBody = g_physicsInterface->createStaticBody(g_physicsInterface->world, hf);
        // Store heightfield pointer for cleanup
        // In practice, you'd need a way to track this
    }
}

void CreatePhysicsTriangleMesh(Terrain& terrain) {
    if (!g_physicsInterface || !g_physicsInterface->world) return;
    
    // Combine all LOD0 chunk meshes
    std::vector<float> allVertices;
    std::vector<int> allIndices;
    int vertexOffset = 0;
    
    for (const auto& chunk : terrain.chunks) {
        if (!chunk.cpuMesh.vertices) continue;
        
        const Mesh& mesh = chunk.cpuMesh;
        
        // Transform vertices to world space
        for (int i = 0; i < mesh.vertexCount; i++) {
            Vector3 v = { mesh.vertices[i * 3], mesh.vertices[i * 3 + 1], mesh.vertices[i * 3 + 2] };
            v = Vector3Transform(v, chunk.transform);
            allVertices.push_back(v.x);
            allVertices.push_back(v.y);
            allVertices.push_back(v.z);
        }
        
        // Add indices with offset
        for (int i = 0; i < mesh.triangleCount * 3; i++) {
            allIndices.push_back(mesh.indices[i] + vertexOffset);
        }
        
        vertexOffset += mesh.vertexCount;
    }
    
    if (allVertices.empty()) return;
    
    b3MeshShape* shape = b3CreateMeshShape(allVertices.data(), allVertices.size() / 3, allIndices.data(), allIndices.size());
    
    if (shape && g_physicsInterface->createStaticBody) {
        terrain.physicsBody = g_physicsInterface->createStaticBody(g_physicsInterface->world, shape);
    }
}

void DestroyPhysics(Terrain& terrain) {
    if (terrain.physicsBody && g_physicsInterface && g_physicsInterface->destroyBody) {
        g_physicsInterface->destroyBody(g_physicsInterface->world, (b3Body*)terrain.physicsBody);
        terrain.physicsBody = nullptr;
    }
}

void UpdatePhysicsTransform(Terrain& terrain) {
    if (terrain.physicsBody && g_physicsInterface && g_physicsInterface->setBodyTransform) {
        float pos[3] = { terrain.center.x, terrain.center.y, terrain.center.z };
        float rot[4] = { 0, 0, 0, 1 }; // Quaternion from Euler
        // Convert Euler to quaternion
        Quaternion q = QuaternionFromEuler(terrain.rotation.x * DEG2RAD, terrain.rotation.y * DEG2RAD, terrain.rotation.z * DEG2RAD);
        rot[0] = q.x; rot[1] = q.y; rot[2] = q.z; rot[3] = q.w;
        
        g_physicsInterface->setBodyTransform((b3Body*)terrain.physicsBody, pos, rot);
    }
}

// ============================================================================
// Raycasting Helpers
// ============================================================================

bool RaycastHeightfield(const Terrain& terrain, const Ray& ray, float* outDistance, Vector3* outPoint, Vector3* outNormal) {
    // This would use Box3D raycast against heightfield
    // For now, use the CPU fallback in Terrain::Raycast
    return false;
}

} // namespace terrain