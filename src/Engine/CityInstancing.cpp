#include "../../include/Graphics/Graphics.hpp"
#include "GraphicsInternal.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <vector>

namespace {

// Persistent instance buffers.
bool instanceBuffersEnabled = true;

}

namespace gfx {

void DrawCityInstances(Mesh mesh, const std::vector<Matrix>& transforms, int start, int count, Color tint) {
    if (!detail::CityInstancedReady() || count <= 0) return;
    if (start < 0 || (size_t)(start + count) > transforms.size()) return;
    if (!IsInShadowPass()) detail::SetupInstancedLighting();
    detail::CityInstancedMaterial().maps[MATERIAL_MAP_DIFFUSE].color = tint;
    DrawMeshInstanced(mesh, detail::CityInstancedMaterial(), transforms.data() + start, count);
    IncrementDrawCallCount(1);
    AddMeshCount(1);
    AddTriangleCount(mesh.triangleCount * count);
    NoteMaterialUsed(detail::CityInstancedMaterial().shader.id);
    if (detail::CityInstancedMaterial().maps) {
        for (int m = 0; m < kMaterialMapCount; ++m) {
            NoteTextureBound(detail::CityInstancedMaterial().maps[m].texture.id);
        }
    }
}

unsigned int CreateInstanceBuffer(const std::vector<Matrix>& transforms) {
    if (transforms.empty()) return 0;
    // Column-major float16 (m0,m1,m2,...), NOT the Matrix struct's memory order.
    std::vector<float16> data(transforms.size());
    for (size_t i = 0; i < transforms.size(); i++) data[i] = MatrixToFloatV(transforms[i]);
    return rlLoadVertexBuffer(data.data(), (int)(data.size() * sizeof(float16)), false);
}

void DestroyInstanceBuffer(unsigned int id) {
    if (id != 0) rlUnloadVertexBuffer(id);
}

void SetInstanceBuffersEnabled(bool enabled) { instanceBuffersEnabled = enabled; }
bool GetInstanceBuffersEnabled() { return instanceBuffersEnabled; }

bool InstanceBuffersActive() {
    return instanceBuffersEnabled && detail::CityInstancedReady() && detail::CityInstancedShader().locs != nullptr &&
           detail::CityInstancedShader().locs[SHADER_LOC_VERTEX_INSTANCETRANSFORM] != -1;
}

// Same as DrawCityInstances(), but the per-instance transforms come from a
// persistent buffer instead of being uploaded on every call.
void DrawCityInstancesBuffered(Mesh mesh, unsigned int instanceVbo, int count, Color tint) {
    if (!InstanceBuffersActive() || count <= 0 || instanceVbo == 0 || mesh.vaoId == 0) return;
    if (!IsInShadowPass()) detail::SetupInstancedLighting();
    detail::CityInstancedMaterial().maps[MATERIAL_MAP_DIFFUSE].color = tint;
    DrawMeshInstancedBuffer(mesh, detail::CityInstancedMaterial(), instanceVbo, count);
    IncrementDrawCallCount(1);
    AddMeshCount(1);
    AddTriangleCount(mesh.triangleCount * count);
    const Texture2D& tex = detail::CityInstancedMaterial().maps[MATERIAL_MAP_DIFFUSE].texture;
    if (tex.id > 0) {
        NoteTextureBound(tex.id);
        NoteMaterialUsed(detail::CityInstancedShader().id);
    }
}

} // namespace gfx
