#include "../../include/Terrain/TerrainMesh.hpp"
#include "raylib.h"
#include "raymath.h"
#include <vector>
#include <cmath>

namespace terrain {

// ============================================================================
// Mesh Generation
// ============================================================================

Mesh GenerateTerrainMesh(const float* heightmap, int resolution, float scale, float minHeight, float maxHeight) {
    Mesh mesh = {0};
    
    int vertexCount = resolution * resolution;
    int triangleCount = (resolution - 1) * (resolution - 1) * 2;
    int indexCount = triangleCount * 3;
    
    // Allocate vertex data
    mesh.vertexCount = vertexCount;
    mesh.triangleCount = triangleCount;
    
    mesh.vertices = (float*)MemAlloc(vertexCount * 3 * sizeof(float));
    mesh.normals = (float*)MemAlloc(vertexCount * 3 * sizeof(float));
    mesh.texcoords = (float*)MemAlloc(vertexCount * 2 * sizeof(float));
    mesh.tangents = (float*)MemAlloc(vertexCount * 4 * sizeof(float)); // Tangents for normal mapping
    mesh.indices = (unsigned short*)MemAlloc(indexCount * sizeof(unsigned short));
    
    float halfSize = (resolution - 1) * scale * 0.5f;
    float heightRange = maxHeight - minHeight;
    
    // Generate vertices
    for (int z = 0; z < resolution; z++) {
        for (int x = 0; x < resolution; x++) {
            int idx = (z * resolution + x) * 3;
            int uvIdx = (z * resolution + x) * 2;
            int tanIdx = (z * resolution + x) * 4;
            
            float wx = (x * scale) - halfSize;
            float wz = (z * scale) - halfSize;
            float h = heightmap[z * resolution + x];
            
            // Position
            mesh.vertices[idx + 0] = wx;
            mesh.vertices[idx + 1] = h;
            mesh.vertices[idx + 2] = wz;
            
            // UV
            mesh.texcoords[uvIdx + 0] = (float)x / (resolution - 1);
            mesh.texcoords[uvIdx + 1] = (float)z / (resolution - 1);
        }
    }
    
    // Compute normals and tangents
    for (int z = 0; z < resolution; z++) {
        for (int x = 0; x < resolution; x++) {
            int idx = (z * resolution + x) * 3;
            int tanIdx = (z * resolution + x) * 4;
            
            // Normal from heightmap neighbors
            float hL = (x > 0) ? heightmap[z * resolution + (x - 1)] : heightmap[z * resolution + x];
            float hR = (x < resolution - 1) ? heightmap[z * resolution + (x + 1)] : heightmap[z * resolution + x];
            float hD = (z > 0) ? heightmap[(z - 1) * resolution + x] : heightmap[z * resolution + x];
            float hU = (z < resolution - 1) ? heightmap[(z + 1) * resolution + x] : heightmap[z * resolution + x];
            
            Vector3 normal = {
                (hL - hR) * scale,
                2.0f * scale,
                (hD - hU) * scale
            };
            normal = Vector3Normalize(normal);
            
            mesh.normals[idx + 0] = normal.x;
            mesh.normals[idx + 1] = normal.y;
            mesh.normals[idx + 2] = normal.z;
            
            // Tangent (for normal mapping) - along X axis
            Vector3 tangent = { 1.0f, 0.0f, 0.0f };
            Vector3 bitangent = Vector3CrossProduct(normal, tangent);
            bitangent = Vector3Normalize(bitangent);
            tangent = Vector3CrossProduct(bitangent, normal);
            tangent = Vector3Normalize(tangent);
            
            mesh.tangents[tanIdx + 0] = tangent.x;
            mesh.tangents[tanIdx + 1] = tangent.y;
            mesh.tangents[tanIdx + 2] = tangent.z;
            mesh.tangents[tanIdx + 3] = (Vector3DotProduct(Vector3CrossProduct(normal, tangent), bitangent) < 0) ? -1.0f : 1.0f;
        }
    }
    
    // Generate indices (triangle strip would be more efficient, but use triangles for simplicity)
    int indexIdx = 0;
    for (int z = 0; z < resolution - 1; z++) {
        for (int x = 0; x < resolution - 1; x++) {
            int a = z * resolution + x;
            int b = z * resolution + x + 1;
            int c = (z + 1) * resolution + x;
            int d = (z + 1) * resolution + x + 1;
            
            // Triangle 1
            mesh.indices[indexIdx++] = a;
            mesh.indices[indexIdx++] = c;
            mesh.indices[indexIdx++] = b;
            
            // Triangle 2
            mesh.indices[indexIdx++] = b;
            mesh.indices[indexIdx++] = c;
            mesh.indices[indexIdx++] = d;
        }
    }
    
    // Upload to GPU
    UploadMesh(&mesh, false);
    
    return mesh;
}

Mesh GenerateTerrainMeshLOD(const float* heightmap, int resolution, int lod, float scale, float minHeight, float maxHeight) {
    // Generate lower LOD by sampling
    int lodRes = resolution;
    for (int i = 0; i < lod; i++) {
        lodRes = (lodRes - 1) / 2 + 1;
    }
    lodRes = std::max(lodRes, 2);
    
    std::vector<float> lodHeightmap(lodRes * lodRes);
    float sampleScale = (float)(resolution - 1) / (lodRes - 1);
    
    for (int z = 0; z < lodRes; z++) {
        for (int x = 0; x < lodRes; x++) {
            float srcX = x * sampleScale;
            float srcZ = z * sampleScale;
            int x0 = (int)srcX;
            int z0 = (int)srcZ;
            int x1 = std::min(x0 + 1, resolution - 1);
            int z1 = std::min(z0 + 1, resolution - 1);
            
            float fx = srcX - x0;
            float fz = srcZ - z0;
            
            float h00 = heightmap[z0 * resolution + x0];
            float h10 = heightmap[z0 * resolution + x1];
            float h01 = heightmap[z1 * resolution + x0];
            float h11 = heightmap[z1 * resolution + x1];
            
            float h0 = h00 + (h10 - h00) * fx;
            float h1 = h01 + (h11 - h01) * fx;
            lodHeightmap[z * lodRes + x] = h0 + (h1 - h0) * fz;
        }
    }
    
    float lodScale = scale * sampleScale;
    return GenerateTerrainMesh(lodHeightmap.data(), lodRes, lodScale, minHeight, maxHeight);
}

// ============================================================================
// Geomorphing (LOD transition)
// ============================================================================

void ApplyGeomorphing(Mesh& mesh, const float* heightmap, int resolution, int lod, float morphFactor) {
    // morphFactor: 0 = current LOD, 1 = next LOD
    // Morp vertices toward lower LOD positions to avoid popping
    
    int nextLodRes = resolution;
    for (int i = 0; i < lod + 1; i++) {
        nextLodRes = (nextLodRes - 1) / 2 + 1;
    }
    nextLodRes = std::max(nextLodRes, 2);
    
    if (morphFactor <= 0.0f || lod >= 2) return;
    
    // This is a simplified version - full geomorphing requires vertex shader support
    // For now, we just note that geomorphing is handled in the vertex shader
    // via a morph factor uniform
}

// ============================================================================
// Mesh Utilities
// ============================================================================

void UpdateMeshHeights(Mesh& mesh, const float* heightmap, int resolution, float scale, float minHeight) {
    if (!mesh.vertices || mesh.vertexCount != resolution * resolution) return;
    
    for (int z = 0; z < resolution; z++) {
        for (int x = 0; x < resolution; x++) {
            int idx = (z * resolution + x) * 3;
            mesh.vertices[idx + 1] = heightmap[z * resolution + x];
        }
    }
    
    // Recompute normals
    for (int z = 0; z < resolution; z++) {
        for (int x = 0; x < resolution; x++) {
            int idx = (z * resolution + x) * 3;
            
            float hL = (x > 0) ? heightmap[z * resolution + (x - 1)] : heightmap[z * resolution + x];
            float hR = (x < resolution - 1) ? heightmap[z * resolution + (x + 1)] : heightmap[z * resolution + x];
            float hD = (z > 0) ? heightmap[(z - 1) * resolution + x] : heightmap[z * resolution + x];
            float hU = (z < resolution - 1) ? heightmap[(z + 1) * resolution + x] : heightmap[z * resolution + x];
            
            Vector3 normal = {
                (hL - hR) * scale,
                2.0f * scale,
                (hD - hU) * scale
            };
            normal = Vector3Normalize(normal);
            
            mesh.normals[idx + 0] = normal.x;
            mesh.normals[idx + 1] = normal.y;
            mesh.normals[idx + 2] = normal.z;
        }
    }
    
    // Update GPU buffers
    UpdateMeshBuffer(mesh, 0, mesh.vertices, mesh.vertexCount * 3 * sizeof(float), 0);
    UpdateMeshBuffer(mesh, 2, mesh.normals, mesh.vertexCount * 3 * sizeof(float), 0);
}

void UpdateMeshSplatmap(Mesh& mesh, const uint8_t* splatmap, int resolution) {
    // Splatmap is typically passed as a texture, not vertex attribute
    // This would add a second UV set for splatmap sampling
    // For now, we use a texture bound to the shader
}

BoundingBox ComputeMeshBounds(const Mesh& mesh, float minHeight, float maxHeight) {
    BoundingBox bounds = {0};
    bounds.min = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    bounds.max = { FLT_MAX, FLT_MAX, FLT_MAX };
    
    if (mesh.vertices && mesh.vertexCount > 0) {
        bounds.min = { mesh.vertices[0], mesh.vertices[1], mesh.vertices[2] };
        bounds.max = bounds.min;
        
        for (int i = 1; i < mesh.vertexCount; i++) {
            Vector3 v = { mesh.vertices[i * 3], mesh.vertices[i * 3 + 1], mesh.vertices[i * 3 + 2] };
            bounds.min = Vector3Min(bounds.min, v);
            bounds.max = Vector3Max(bounds.max, v);
        }
    }
    
    bounds.min.y = minHeight;
    bounds.max.y = maxHeight;
    
    return bounds;
}

} // namespace terrain