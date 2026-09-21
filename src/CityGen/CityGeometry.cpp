#include "../../include/CityGen/CityGeometry.hpp"
#include "../../include/Terrain/Water/WaterNoise.hpp"
#include "raymath.h"

#include <algorithm>
#include <cmath>

namespace citygeom {

namespace {

constexpr float kEps = 1e-5f;

float Cross(const Vector2& a, const Vector2& b, const Vector2& c) {
    // 2D cross of (b-a) x (c-b) treated in (x, z) plane.
    return (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
}

bool SegmentsProperlyIntersect(const Vector2& p1, const Vector2& p2,
                               const Vector2& q1, const Vector2& q2) {
    auto orient = [](const Vector2& a, const Vector2& b, const Vector2& c) {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    float d1 = orient(p1, p2, q1);
    float d2 = orient(p1, p2, q2);
    float d3 = orient(q1, q2, p1);
    float d4 = orient(q1, q2, p2);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
        ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) {
        return true;
    }
    return false;
}

bool IsPolygonSimple(const std::vector<Vector2>& poly) {
    const int n = (int)poly.size();
    if (n < 3) return false;
    for (int i = 0; i < n; i++) {
        const Vector2& a1 = poly[i];
        const Vector2& b1 = poly[(i + 1) % n];
        for (int j = i + 1; j < n; j++) {
            // Skip adjacent segments.
            if (j == i || j == (i + n - 1) % n) continue;
            const Vector2& a2 = poly[j];
            const Vector2& b2 = poly[(j + 1) % n];
            if (SegmentsProperlyIntersect(a1, b1, a2, b2)) return false;
        }
    }
    return true;
}

} // namespace

Vector2 OrganicWarp(const Vector2& p, float strength, float scale,
                    int octaves, int seed) {
    if (strength <= 0.0f || scale <= 0.0f) return p;
    Vector2 q = Vector2Scale(p, 1.0f / scale);
    float nx = WaterNoise::FBM2D(q.x, q.y, octaves, 0.5f, 2.0f, seed);
    float nz = WaterNoise::FBM2D(q.y, q.x, octaves, 0.5f, 2.0f, seed ^ 0x5bf03635);
    return { p.x + nx * strength, p.y + nz * strength };
}

float PolygonArea(const std::vector<Vector2>& poly) {
    float a = 0.0f;
    const int n = (int)poly.size();
    for (int i = 0; i < n; i++) {
        const Vector2& p = poly[i];
        const Vector2& q = poly[(i + 1) % n];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5f * a;
}

void EnsureCCW(std::vector<Vector2>& poly) {
    if (PolygonArea(poly) < 0.0f) std::reverse(poly.begin(), poly.end());
}

bool PointInPolygon(const Vector2& p, const std::vector<Vector2>& poly) {
    const int n = (int)poly.size();
    if (n < 3) return false;
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const Vector2& a = poly[i];
        const Vector2& b = poly[j];
        if (((a.y > p.y) != (b.y > p.y)) &&
            (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)) {
            inside = !inside;
        }
    }
    return inside;
}

std::vector<std::vector<int>> ExtractFaces(
    const std::vector<Vector2>& positions,
    const std::vector<std::pair<int, int>>& edges) {

    const int V = (int)positions.size();
    const int E = (int)edges.size();
    std::vector<std::vector<int>> result;

    if (V == 0 || E == 0) return result;

    // Directed half-edges: 2E of them.
    std::vector<int> from(2 * E), to(2 * E);
    for (int e = 0; e < E; e++) {
        from[2 * e] = edges[e].first;
        to[2 * e] = edges[e].second;
        from[2 * e + 1] = edges[e].second;
        to[2 * e + 1] = edges[e].first;
    }

    // Outgoing list per vertex, sorted by angle.
    std::vector<std::vector<int>> outgoing(V);
    std::vector<float> ang(2 * E);
    for (int d = 0; d < 2 * E; d++) {
        const Vector2& a = positions[from[d]];
        const Vector2& b = positions[to[d]];
        ang[d] = atan2f(b.y - a.y, b.x - a.x);
        outgoing[from[d]].push_back(d);
    }
    for (int v = 0; v < V; v++) {
        std::sort(outgoing[v].begin(), outgoing[v].end(),
                  [&](int x, int y) { return ang[x] < ang[y]; });
    }

    // next[d] = directed edge after d (turn right around to[d]).
    std::vector<int> next(2 * E, -1);
    for (int d = 0; d < 2 * E; d++) {
        int t = to[d];
        const auto& out = outgoing[t];
        // Find the edge going back toward from[d].
        int back = -1;
        for (int k = 0; k < (int)out.size(); k++) {
            if (to[out[k]] == from[d]) { back = k; break; }
        }
        if (back < 0) continue;
        int idx = back - 1;
        if (idx < 0) idx = (int)out.size() - 1;
        next[d] = out[idx];
    }

    std::vector<bool> visited(2 * E, false);
    std::vector<std::vector<int>> loops;
    for (int d = 0; d < 2 * E; d++) {
        if (visited[d] || next[d] < 0) continue;
        std::vector<int> loop;
        int cur = d;
        while (!visited[cur]) {
            visited[cur] = true;
            loop.push_back(from[cur]);
            cur = next[cur];
            if (cur == d) break;
        }
        if (loop.size() >= 3) loops.push_back(std::move(loop));
    }

    // Drop the outer (unbounded) face: it has the minority winding sign, or on
    // a sign tie it is the loop with the largest absolute area.
    auto areaOf = [&](const std::vector<int>& l) {
        float a = 0.0f;
        const int n = (int)l.size();
        for (int i = 0; i < n; i++) {
            const Vector2& p = positions[l[i]];
            const Vector2& q = positions[l[(i + 1) % n]];
            a += p.x * q.y - q.x * p.y;
        }
        return 0.5f * a;
    };

    std::vector<std::vector<int>> kept;
    std::vector<int> sink; // canonical indexes in loops to keep
    if (loops.size() == 1) {
        sink.push_back(0);
    } else if (loops.size() > 1) {
        int posCount = 0, negCount = 0;
        float maxAbs = 0.0f;
        int maxIdx = 0;
        for (int k = 0; k < (int)loops.size(); k++) {
            float a = areaOf(loops[k]);
            if (a > 0) posCount++; else negCount++;
            if (fabsf(a) > maxAbs) { maxAbs = fabsf(a); maxIdx = k; }
        }
        if (posCount != negCount) {
            bool minorityPositive = posCount < negCount;
            for (int k = 0; k < (int)loops.size(); k++) {
                if ((areaOf(loops[k]) > 0) != minorityPositive) sink.push_back(k);
            }
        } else {
            for (int k = 0; k < (int)loops.size(); k++) if (k != maxIdx) sink.push_back(k);
        }
    }

    for (int k : sink) {
        std::vector<int> loop = loops[k];
        // Ensure CCW (positive signed area) in the (x, z) plane.
        if (areaOf(loop) < 0.0f) {
            std::reverse(loop.begin(), loop.end());
        }
        result.push_back(std::move(loop));
    }
    return result;
}

bool InsetPolygon(const std::vector<Vector2>& poly, float d,
                  std::vector<Vector2>& out) {
    out.clear();
    const int n = (int)poly.size();
    if (n < 3 || d < 0.0f) return false;

    auto dir = [](const Vector2& a, const Vector2& b) {
        Vector2 v = Vector2Subtract(b, a);
        float len = Vector2Length(v);
        if (len < kEps) return Vector2{};
        return Vector2Scale(v, 1.0f / len);
    };
    // Left normal of a direction (interior side for CCW polygons).
    auto leftNormal = [](const Vector2& dv) {
        return Vector2{ -dv.y, dv.x };
    };

    // Offset each original edge line toward the interior by d.
    std::vector<Vector2> edgeOrig(n), edgeDir(n), edgeNorm(n);
    for (int i = 0; i < n; i++) {
        const Vector2& a = poly[i];
        const Vector2& b = poly[(i + 1) % n];
        edgeDir[i] = dir(a, b);
        if (edgeDir[i].x == 0.0f && edgeDir[i].y == 0.0f) return false;
        edgeNorm[i] = leftNormal(edgeDir[i]);
        edgeOrig[i] = Vector2Add(a, Vector2Scale(edgeNorm[i], d));
    }

    out.resize(n);
    for (int i = 0; i < n; i++) {
        const Vector2& o1 = edgeOrig[i];
        const Vector2& d1 = edgeDir[i];
        const Vector2& o2 = edgeOrig[(i + 1) % n];
        const Vector2& d2 = edgeDir[(i + 1) % n];
        float cr = d1.x * d2.y - d1.y * d2.x;
        if (fabsf(cr) < 1e-4f) {
            // Nearly parallel: average the two single-edge offsets.
            out[i] = Vector2Scale(Vector2Add(Vector2Add(poly[i], Vector2Scale(edgeNorm[i], d)),
                                             Vector2Scale(edgeNorm[(i + 1) % n], d)),
                                  0.5f);
        } else {
            Vector2 w = Vector2Subtract(o2, o1);
            float t1 = (w.x * d2.y - w.y * d2.x) / cr;
            out[i] = Vector2Add(o1, Vector2Scale(d1, t1));
        }
    }

    if (PolygonArea(out) < 1e-3f || !IsPolygonSimple(out)) {
        out.clear();
        return false;
    }
    return true;
}

void TriangulateSimple(const std::vector<Vector2>& poly, std::vector<int>& tris) {
    const int n = (int)poly.size();
    if (n < 3) return;

    std::vector<int> idx(n);
    for (int i = 0; i < n; i++) idx[i] = i;

    auto pointInTri = [](const Vector2& p, const Vector2& a, const Vector2& b,
                         const Vector2& c) {
        auto sgn = [](const Vector2& q, const Vector2& r, const Vector2& s) {
            return (r.x - q.x) * (s.y - q.y) - (r.y - q.y) * (s.x - q.x);
        };
        float d2 = sgn(p, a, b), d3 = sgn(p, b, c), d4 = sgn(p, c, a);
        bool hasNeg = (d2 < 0) || (d3 < 0) || (d4 < 0);
        bool hasPos = (d2 > 0) || (d3 > 0) || (d4 > 0);
        return !(hasNeg && hasPos);
    };

    std::vector<int> work = idx;
    while ((int)work.size() > 3) {
        bool clipped = false;
        const int m = (int)work.size();
        for (int i = 0; i < m && !clipped; i++) {
            int pi = work[(i + m - 1) % m], qi = work[i], ri = work[(i + 1) % m];
            const Vector2& a = poly[pi];
            const Vector2& b = poly[qi];
            const Vector2& c = poly[ri];
            // Convex (CCW) reflex check done below: for a CCW polygon an ear
            // is convex when the cross of (b-a) x (c-b) is positive, proven by
            // PointInPolygon-compatible ordering used above.
            float cr = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
            if (cr <= kEps) continue;
            // No other vertex may lie inside the candidate triangle.
            bool occupied = false;
            for (int k = 0; k < m; k++) {
                if (work[k] == pi || work[k] == qi || work[k] == ri) continue;
                if (pointInTri(poly[work[k]], a, b, c)) { occupied = true; break; }
            }
            if (!occupied) {
                tris.push_back(pi);
                tris.push_back(qi);
                tris.push_back(ri);
                work.erase(work.begin() + i);
                clipped = true;
            }
        }
        if (!clipped) break; // degenerate polygon; stop and keep what we have
    }
    if (work.size() == 3) {
        tris.push_back(work[0]);
        tris.push_back(work[1]);
        tris.push_back(work[2]);
    }
}

} // namespace citygeom