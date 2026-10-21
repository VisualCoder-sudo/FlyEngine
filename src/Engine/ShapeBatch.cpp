#include "../../include/Graphics/Graphics.hpp"
#include "GraphicsInternal.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

struct Item {
    int shape;
    unsigned int texture;
    gfx::SurfaceMaterial surface;
    Matrix transform;   // tint already packed into m3/m7/m11
};

// Instances share one draw only when their material matches too (it is a uniform, not per instance).
bool SurfaceLess(const gfx::SurfaceMaterial& a, const gfx::SurfaceMaterial& b) {
    if (a.metallic != b.metallic) return a.metallic < b.metallic;
    if (a.roughness != b.roughness) return a.roughness < b.roughness;
    if (a.emissiveStrength != b.emissiveStrength) return a.emissiveStrength < b.emissiveStrength;
    const unsigned ca = (a.emissive.r << 16) | (a.emissive.g << 8) | a.emissive.b;
    const unsigned cb = (b.emissive.r << 16) | (b.emissive.g << 8) | b.emissive.b;
    return ca < cb;
}

bool batching = false;
std::vector<Item> items;
std::vector<Matrix> group;

// Hover outlines wait until the instanced draws are in, so the hull is depth-tested against the shape's own faces.
struct Outline { int shape; Matrix transform; };
std::vector<Outline> outlines;

}

namespace gfx {

void BeginShapeBatch() {
    // FLY_NO_SHAPE_BATCH=1 draws every shape on its own again (debugging aid).
    static const bool off = std::getenv("FLY_NO_SHAPE_BATCH") != nullptr;
    batching = !off && detail::ShapeInstancedReady();
    items.clear();
    outlines.clear();
}

bool QueueShapeOutline(ShapeType shape, const Matrix& transform) {
    if (!batching) return false;
    outlines.push_back({ (int)shape, transform });
    return true;
}

void ApplySurfaceMaterial(Material& material, const SurfaceMaterial& surface) {
    if (!material.maps) return;
    material.maps[MATERIAL_MAP_METALNESS].value = surface.metallic;
    material.maps[MATERIAL_MAP_ROUGHNESS].value = surface.roughness;
    material.maps[MATERIAL_MAP_EMISSION].color = surface.emissive;
    material.maps[MATERIAL_MAP_EMISSION].value = surface.emissiveStrength;
}

bool QueueShape(ShapeType shape, Texture2D texture, const Matrix& transform, Color tint, const SurfaceMaterial& surface) {
    if (!batching || tint.a != 255) return false;
    Item it;
    it.shape = (int)shape;
    it.texture = texture.id;
    it.surface = surface;
    it.transform = transform;
    // The shader reads the tint from the w of the first three columns (m3, m7, m11).
    it.transform.m3 = tint.r / 255.0f;
    it.transform.m7 = tint.g / 255.0f;
    it.transform.m11 = tint.b / 255.0f;
    it.transform.m15 = 1.0f;
    items.push_back(it);
    return true;
}

void EndShapeBatch() {
    if (!batching) return;
    batching = false;
    struct FlushOutlines {
        ~FlushOutlines() {
            for (const Outline& o : outlines) DrawModelOutline(GetShapeModel((ShapeType)o.shape), o.transform);
            outlines.clear();
        }
    } flushOutlines;
    if (items.empty()) return;

    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.shape != b.shape) return a.shape < b.shape;
        if (a.texture != b.texture) return a.texture < b.texture;
        return SurfaceLess(a.surface, b.surface);
    });
    if (!IsInShadowPass()) detail::SetupShapeInstancedLighting();

    Material& mat = detail::ShapeInstancedMaterial();
    mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    for (size_t i = 0; i < items.size();) {
        size_t j = i;
        group.clear();
        while (j < items.size() && items[j].shape == items[i].shape && items[j].texture == items[i].texture &&
               items[j].surface == items[i].surface) {
            group.push_back(items[j].transform);
            ++j;
        }
        Model& model = GetShapeModel((ShapeType)items[i].shape);
        Texture2D tex = model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture;
        tex.id = items[i].texture;
        mat.maps[MATERIAL_MAP_DIFFUSE].texture = tex;
        ApplySurfaceMaterial(mat, items[i].surface);
        DrawMeshInstanced(model.meshes[0], mat, group.data(), (int)group.size());
        IncrementDrawCallCount(1);
        AddMeshCount(1);
        AddTriangleCount(model.meshes[0].triangleCount * (int)group.size());
        NoteMaterialUsed(mat.shader.id);
        NoteTextureBound(items[i].texture);
        i = j;
    }
    items.clear();
}

} // namespace gfx
