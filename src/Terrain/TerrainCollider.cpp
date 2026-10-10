#include "../../include/Terrain/TerrainCollider.hpp"
#include "../../include/Terrain/BasicTerrain.hpp"

#include <algorithm>
#include <cmath>

namespace terrain {

b3HeightFieldData* BuildHeightField(const float* heights, int countX, int countZ, float cellSize) {
    if (!heights || countX < 2 || countZ < 2 || !(cellSize > 0.0f)) return nullptr;

    const size_t n = (size_t)countX * (size_t)countZ;
    float lo = heights[0];
    float hi = heights[0];
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(heights[i])) return nullptr;
        lo = std::min(lo, heights[i]);
        hi = std::max(hi, heights[i]);
    }

    // The 16-bit quantisation spans exactly the sampled range, so a 100 m tall
    // terrain resolves heights to ~1.5 mm and a flat one is stored exactly.
    b3HeightFieldDef def{};
    def.heights = const_cast<float*>(heights);   // Box3D only reads and compresses it
    def.materialIndices = nullptr;
    def.scale = b3Vec3{ cellSize, 1.0f, cellSize };
    def.countX = countX;
    def.countZ = countZ;
    def.globalMinimumHeight = lo;
    def.globalMaximumHeight = hi;
    def.clockwiseWinding = false;
    return b3CreateHeightField(&def);
}

bool CreateTerrainCollider(b3WorldId world, const BasicTerrain& t, float friction, float restitution,
                           TerrainCollider& out) {
    const int w = t.GetWidth();
    const int d = t.GetDepth();
    b3HeightFieldData* hf = BuildHeightField(t.GetHeightData(), w, d, t.GetScale());
    if (!hf) return false;

    // BasicTerrain's mesh puts sample (x, z) at position + (x - w/2, z - d/2) * scale
    // (see BuildDenseMesh), and the height field's local origin is its first sample.
    b3BodyDef bodyDef = b3DefaultBodyDef();
    bodyDef.type = b3_staticBody;
    bodyDef.position = b3Vec3{ t.position.x - w * t.GetScale() * 0.5f,
                               t.position.y,
                               t.position.z - d * t.GetScale() * 0.5f };
    b3BodyId body = b3CreateBody(world, &bodyDef);

    b3ShapeDef shapeDef = b3DefaultShapeDef();
    shapeDef.density = 0.0f;
    shapeDef.baseMaterial.friction = friction;
    shapeDef.baseMaterial.restitution = restitution;
    b3CreateHeightFieldShape(body, &shapeDef, hf);

    out.body = body;
    out.heightField = hf;
    return true;
}

void ReleaseTerrainCollider(TerrainCollider& c) {
    if (c.heightField) b3DestroyHeightField(c.heightField);
    c = TerrainCollider{};
}

} // namespace terrain
