// Property test for the city's polygon helpers: park outlines are triangulated
// by TriangulateSimple, and every triangle must be counter-clockwise and the
// triangles together must cover exactly the outline's area. A missing ear leaves
// a hole in the grass that shows the pad underneath.
#include "CityGen/CityGeometry.hpp"
#include "raymath.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static float TriArea(const Vector2& a, const Vector2& b, const Vector2& c) {
    return 0.5f * ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
}

static unsigned g_seed = 12345;
static float Rnd() { g_seed = g_seed * 1664525u + 1013904223u; return (float)(g_seed >> 8) / 16777216.0f; }

// A rough block outline: a ring of nodes at uneven radii/angles, optionally with
// extra nodes inserted on edges (what the road tool creates).
static std::vector<Vector2> RandomBlock(int n, float unevenness, bool tJunctions) {
    std::vector<Vector2> p;
    for (int i = 0; i < n; ++i) {
        const float a = 6.2831853f * ((float)i + (Rnd() - 0.5f) * 0.6f) / (float)n;
        const float r = 40.0f * (1.0f + unevenness * (Rnd() - 0.5f));
        p.push_back({ std::cos(a) * r, std::sin(a) * r });
    }
    if (tJunctions) {
        std::vector<Vector2> q;
        for (size_t i = 0; i < p.size(); ++i) {
            q.push_back(p[i]);
            if (Rnd() < 0.5f) {
                const Vector2& a = p[i]; const Vector2& b = p[(i + 1) % p.size()];
                const float t = 0.3f + 0.4f * Rnd();
                q.push_back({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t });
            }
        }
        p = q;
    }
    citygeom::EnsureCCW(p);
    return p;
}

// A block outline that walks out along a dead-end road spur and back, so two
// nodes appear twice. Captured from the editor; it used to be triangulated into
// only ~26% of its area, leaving a hole in the block's pad.
static int SpurOutlineRegression() {
    const std::vector<Vector2> poly = {
        { 231.00f, -61.11f }, { 288.70f, -64.81f }, { 322.71f, -21.39f }, { 314.61f, -27.29f }, { 386.52f, -12.15f },
        { 359.76f, 11.27f }, { 314.14f, 35.49f }, { 314.61f, -27.29f }, { 322.71f, -21.39f }, { 231.85f, -16.25f } };
    std::vector<int> tris;
    citygeom::TriangulateSimple(poly, tris);
    float sum = 0.0f;
    for (size_t t = 0; t + 2 < tris.size(); t += 3) sum += TriArea(poly[tris[t]], poly[tris[t + 1]], poly[tris[t + 2]]);
    const float want = citygeom::PolygonArea(poly);
    if (std::fabs(sum - want) > 0.01f * want) {
        std::printf("FAIL spur outline: triangles cover %.1f of %.1f m^2\n", sum, want);
        return 1;
    }
    return 0;
}

int main() {
    int failures = SpurOutlineRegression(), polys = 0, insetFailed = 0;
    for (int iter = 0; iter < 20000; ++iter) {
        const int n = 4 + (int)(Rnd() * 9.0f);
        const float uneven = 0.1f + Rnd() * 1.2f;
        std::vector<Vector2> block = RandomBlock(n, uneven, (iter & 1) != 0);
        if (!citygeom::IsSimplePolygonForTest(block)) continue;   // only simple outlines are valid blocks
        std::vector<Vector2> park;
        if (!citygeom::InsetPolygon(block, 3.0f, park)) { park = block; ++insetFailed; }
        ++polys;
        std::vector<int> tris;
        citygeom::TriangulateSimple(park, tris);
        float sum = 0.0f;
        bool badTri = false;
        for (size_t t = 0; t + 2 < tris.size(); t += 3) {
            const float a = TriArea(park[tris[t]], park[tris[t + 1]], park[tris[t + 2]]);
            sum += a;
            if (a < -1e-3f) badTri = true;
        }
        // Every triangle must lie inside the outline: a triangle spanning a concave pocket would
        // fill ground that is not part of the park.
        for (size_t t = 0; t + 2 < tris.size() && !badTri; t += 3) {
            const Vector2 &a = park[tris[t]], &b = park[tris[t + 1]], &c = park[tris[t + 2]];
            if (std::fabs(TriArea(a, b, c)) < 0.5f) continue;
            const Vector2 g{ (a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f };
            if (!citygeom::PointInPolygon(g, park)) badTri = true;
        }
        const float want = citygeom::PolygonArea(park);
        if (badTri || std::fabs(sum - want) > 0.01f * want + 0.05f) {
            if (failures < 5)
                std::printf("FAIL iter %d (n=%zu): triangles cover %.2f of %.2f%s\n", iter, park.size(), sum, want,
                            badTri ? " (has clockwise triangle)" : "");
            ++failures;
        }
    }
    // Regression: an outline with a vertex resting exactly on another edge (no clean
    // ear at that corner) must still be filled completely, not abandoned part-way.
    {
        std::vector<Vector2> sq = { {0,0}, {4,0}, {4,2}, {2,2}, {2,2}, {2,2}, {4,2}, {4,4}, {0,4} };
        std::vector<int> tr;
        citygeom::TriangulateSimple(sq, tr);
        double cov = 0;
        for (size_t i = 0; i + 2 < tr.size(); i += 3) {
            const Vector2 &a = sq[tr[i]], &b = sq[tr[i+1]], &c = sq[tr[i+2]];
            cov += 0.5 * std::fabs((b.x-a.x)*(c.y-a.y) - (c.x-a.x)*(b.y-a.y));
        }
        if (cov < 15.9) { std::printf("degenerate outline only %.2f of 16 filled\n", cov); ++failures; }
    }
    std::printf("%d outlines (%d fell back to the uninset outline), %d failed\n", polys, insetFailed, failures);
    std::printf(failures ? "city_geometry_test: FAILED\n" : "city_geometry_test: all passed\n");
    return failures ? 1 : 0;
}
