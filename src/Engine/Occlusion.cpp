#include "../../include/Engine/Occlusion.hpp"
#include "rlgl.h"
#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace occlusion {
namespace {

constexpr int kW = 192;
constexpr int kH = 108;
constexpr int kMaxOccluders = 48;
constexpr float kNear = 0.1f;   // clip-space w below this is treated as "behind the camera"

float g_depth[kW * kH];          // view depth (clip w) of the nearest occluder surface per texel
bool g_active = false;
Matrix g_view{}, g_proj{};

// Corner indices of the six faces, as two triangles each. Corner i has bit0 = +x, bit1 = +y, bit2 = +z.
constexpr int kBoxTris[12][3] = {
    { 0, 1, 3 }, { 0, 3, 2 },   // -z
    { 4, 6, 7 }, { 4, 7, 5 },   // +z
    { 0, 4, 5 }, { 0, 5, 1 },   // -y
    { 2, 3, 7 }, { 2, 7, 6 },   // +y
    { 0, 2, 6 }, { 0, 6, 4 },   // -x
    { 1, 5, 7 }, { 1, 7, 3 },   // +x
};

struct Clip { float x, y, w; };

inline Clip ToClip(const Matrix& vp, const Vector3& p) {
    return {
        vp.m0 * p.x + vp.m4 * p.y + vp.m8  * p.z + vp.m12,
        vp.m1 * p.x + vp.m5 * p.y + vp.m9  * p.z + vp.m13,
        vp.m3 * p.x + vp.m7 * p.y + vp.m11 * p.z + vp.m15,
    };
}

struct Screen { float x, y, invW; };

inline Screen ToScreen(const Clip& c) {
    const float iw = 1.0f / c.w;
    return { (c.x * iw * 0.5f + 0.5f) * kW, (c.y * iw * 0.5f + 0.5f) * kH, iw };
}

void RasterizeTriangle(const Screen& a, const Screen& b, const Screen& c) {
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (fabsf(area) < 1e-6f) return;
    const float invArea = 1.0f / area;

    const int x0 = std::max(0, (int)floorf(std::min({ a.x, b.x, c.x })));
    const int x1 = std::min(kW - 1, (int)ceilf(std::max({ a.x, b.x, c.x })));
    const int y0 = std::max(0, (int)floorf(std::min({ a.y, b.y, c.y })));
    const int y1 = std::min(kH - 1, (int)ceilf(std::max({ a.y, b.y, c.y })));

    for (int y = y0; y <= y1; ++y) {
        const float py = y + 0.5f;
        for (int x = x0; x <= x1; ++x) {
            const float px = x + 0.5f;
            const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * invArea;
            const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * invArea;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            // 1/w is linear in screen space; the view depth is its reciprocal.
            const float invW = w0 * a.invW + w1 * b.invW + w2 * c.invW;
            if (invW <= 0.0f) continue;
            const float depth = 1.0f / invW;
            float& cell = g_depth[y * kW + x];
            if (depth < cell) cell = depth;
        }
    }
}

} // namespace

void BeginFrame(std::vector<OccluderBox>& occluders) {
    g_active = false;
    if (occluders.empty()) return;

    g_view = rlGetMatrixModelview();
    g_proj = rlGetMatrixProjection();
    // Perspective only: an orthographic camera has no depth in w.
    if (g_proj.m11 == 0.0f && g_proj.m15 != 0.0f) return;

    if ((int)occluders.size() > kMaxOccluders) {
        std::partial_sort(occluders.begin(), occluders.begin() + kMaxOccluders, occluders.end(),
                          [](const OccluderBox& a, const OccluderBox& b) { return a.volume > b.volume; });
        occluders.resize(kMaxOccluders);
    }

    const Matrix vp = MatrixMultiply(g_view, g_proj);
    std::fill(std::begin(g_depth), std::end(g_depth), 1e30f);

    bool any = false;
    for (const OccluderBox& box : occluders) {
        Screen s[8];
        bool ok = true;
        for (int i = 0; i < 8 && ok; ++i) {
            const Clip c = ToClip(vp, box.corners[i]);
            // A box that reaches the camera plane would need clipping: just do not use it.
            if (c.w < kNear) ok = false;
            else s[i] = ToScreen(c);
        }
        if (!ok) continue;
        for (const auto& t : kBoxTris) RasterizeTriangle(s[t[0]], s[t[1]], s[t[2]]);
        any = true;
    }
    g_active = any;
}

void EndFrame() { g_active = false; }

bool IsOccluded(const BoundingBox& box) {
    if (!g_active) return false;
    // Only valid for the camera the buffer was made with (not the shadow or reflection passes).
    const Matrix view = rlGetMatrixModelview();
    const Matrix proj = rlGetMatrixProjection();
    if (std::memcmp(&view, &g_view, sizeof(Matrix)) != 0 || std::memcmp(&proj, &g_proj, sizeof(Matrix)) != 0) return false;

    const Matrix vp = MatrixMultiply(g_view, g_proj);
    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f, nearest = 1e30f;
    for (int i = 0; i < 8; ++i) {
        const Vector3 p = { (i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y, (i & 4) ? box.max.z : box.min.z };
        const Clip c = ToClip(vp, p);
        if (c.w < kNear) return false;   // reaches the camera plane: leave it to the frustum test
        const Screen s = ToScreen(c);
        minX = std::min(minX, s.x); maxX = std::max(maxX, s.x);
        minY = std::min(minY, s.y); maxY = std::max(maxY, s.y);
        nearest = std::min(nearest, c.w);
    }

    // Every texel the box touches, plus a one-texel border: a texel the occluder's edge only partly covers
    // is sampled at its centre, so its uncovered neighbour must be required to be covered too.
    const int x0 = (int)floorf(minX) - 1, x1 = (int)ceilf(maxX) + 1;
    const int y0 = (int)floorf(minY) - 1, y1 = (int)ceilf(maxY) + 1;
    if (x1 < 0 || y1 < 0 || x0 >= kW || y0 >= kH) return false;   // off screen: the frustum test handles it
    // Partly off screen: the part outside has no data, so only the on-screen part can be judged; the
    // off-screen part cannot make the object visible, so judging the rest is still safe.
    const int cx0 = std::max(0, x0), cx1 = std::min(kW - 1, x1);
    const int cy0 = std::max(0, y0), cy1 = std::min(kH - 1, y1);

    // Occluder depth is sampled at texel centres and interpolated: allow for its slope across a texel.
    const float margin = 0.25f + 0.02f * nearest;
    for (int y = cy0; y <= cy1; ++y) {
        for (int x = cx0; x <= cx1; ++x) {
            if (nearest <= g_depth[y * kW + x] + margin) return false;
        }
    }
    return true;
}

} // namespace occlusion
