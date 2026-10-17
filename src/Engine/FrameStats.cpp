#include "../../include/Engine/Graphics.hpp"

// Per-frame render statistics. Reset once per frame by ResetFrameStats() and
// read by the player debug overlay.
static int g_drawCallCount = 0;
static int g_triangleCount = 0;
static int g_meshCount = 0;

// Frustum culling statistics (for debug overlay)
static int g_culledEntityCount = 0;
static int g_renderedEntityCount = 0;

// Global frustum culling toggle (for A/B comparison)
static bool g_frustumCullingEnabled = true;

// Last frame's per-pass wall times, published by Engine::Draw(). Not reset by
// ResetFrameStats(): these describe the frame that just finished, so zeroing
// them there would blank the overlay for the frame currently being built.
static gfx::FrameTimings g_frameTimings;

namespace {

// Fixed-capacity distinct-id set for the texture/material counters. A linear
// scan is fine at this size and keeps the frame allocation-free, which matters
// because this runs inside the render loop we are trying to measure. The cap is
// generous for a debug overlay; past it we record that we saturated instead of
// growing, so a wildly-textured frame degrades to an undercount rather than to
// a stutter.
constexpr int kDistinctIdCap = 1024;

struct DistinctIdSet {
    int ids[kDistinctIdCap];
    int count = 0;
    bool saturated = false;

    // Returns true if id was already present. Ids <= 0 are not resources and
    // are reported as already-seen so callers never have to guard.
    bool Add(int id) {
        if (id <= 0) return true;
        for (int i = 0; i < count; ++i) {
            if (ids[i] == id) return true;
        }
        if (count >= kDistinctIdCap) { saturated = true; return false; }
        ids[count++] = id;
        return false;
    }

    void Reset() { count = 0; saturated = false; }
};

DistinctIdSet g_textures;
DistinctIdSet g_materials;

} // namespace

int gfx::GetDrawCallCount() { return g_drawCallCount; }
void gfx::IncrementDrawCallCount(int count) { g_drawCallCount += count; }

int gfx::GetTriangleCount() { return g_triangleCount; }
void gfx::AddTriangleCount(int count) { g_triangleCount += count; }
void gfx::AddModelTriangles(const Model& model) {
    for (int i = 0; i < model.meshCount; ++i) g_triangleCount += model.meshes[i].triangleCount;
}

int gfx::GetMeshCount() { return g_meshCount; }
void gfx::AddMeshCount(int count) { g_meshCount += count; }

int gfx::GetCulledEntityCount() { return g_culledEntityCount; }
int gfx::GetRenderedEntityCount() { return g_renderedEntityCount; }
void gfx::IncrementCulledEntityCount(int count) { g_culledEntityCount += count; }
void gfx::IncrementRenderedEntityCount(int count) { g_renderedEntityCount += count; }

int gfx::GetTextureCount() { return g_textures.count; }
int gfx::GetMaterialCount() { return g_materials.count; }
bool gfx::GetStatsSaturated() { return g_textures.saturated || g_materials.saturated; }

void gfx::NoteTextureBound(int id) { g_textures.Add(id); }
void gfx::NoteMaterialUsed(int id) { g_materials.Add(id); }

void gfx::ResetFrameStats() {
    g_drawCallCount = 0;
    g_triangleCount = 0;
    g_meshCount = 0;
    g_culledEntityCount = 0;
    g_renderedEntityCount = 0;
    g_textures.Reset();
    g_materials.Reset();
}

bool gfx::GetFrustumCullingEnabled() { return g_frustumCullingEnabled; }
void gfx::SetFrustumCullingEnabled(bool enabled) { g_frustumCullingEnabled = enabled; }

void gfx::SetFrameTimings(const FrameTimings& timings) { g_frameTimings = timings; }
gfx::FrameTimings gfx::GetFrameTimings() { return g_frameTimings; }