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
    Matrix transform;   // tint already packed into m3/m7/m11
};

bool batching = false;
std::vector<Item> items;
std::vector<Matrix> group;

}

namespace gfx {

void BeginShapeBatch() {
    // FLY_NO_SHAPE_BATCH=1 draws every shape on its own again (debugging aid).
    static const bool off = std::getenv("FLY_NO_SHAPE_BATCH") != nullptr;
    batching = !off && detail::ShapeInstancedReady();
    items.clear();
}

bool QueueShape(ShapeType shape, Texture2D texture, const Matrix& transform, Color tint) {
    if (!batching || tint.a != 255) return false;
    Item it;
    it.shape = (int)shape;
    it.texture = texture.id;
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
    if (items.empty()) return;

    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.shape != b.shape ? a.shape < b.shape : a.texture < b.texture;
    });
    if (!IsInShadowPass()) detail::SetupShapeInstancedLighting();

    Material& mat = detail::ShapeInstancedMaterial();
    mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    for (size_t i = 0; i < items.size();) {
        size_t j = i;
        group.clear();
        while (j < items.size() && items[j].shape == items[i].shape && items[j].texture == items[i].texture) {
            group.push_back(items[j].transform);
            ++j;
        }
        Model& model = GetShapeModel((ShapeType)items[i].shape);
        Texture2D tex = model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture;
        tex.id = items[i].texture;
        mat.maps[MATERIAL_MAP_DIFFUSE].texture = tex;
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
