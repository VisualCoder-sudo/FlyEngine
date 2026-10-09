// Numeric audit of the generated road / pad / park surface on random irregular cities, flat and on uneven ground:
// counts cliffs (nearly vertical surface triangles), spikes (triangles that are long and thin seen from above),
// and non-finite vertices, and prints the worst offenders. Opens a window (mesh upload), so it is not registered
// with ctest: build/tests/city_surface_audit
#include "raylib.h"
#include "raymath.h"
#include "../include/CityGen/City.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace city;

struct Audit { int tris = 0, cliffs = 0, spikes = 0, bad = 0; float worstSpike = 0, wx = 0, wz = 0, worstCliff = 0, cx = 0, cz = 0; };

static Audit Run(const City& c) {
    Audit a;
    std::vector<Vector3> v;
    c.DebugSurfaceTriangles(v);
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        const Vector3 &p = v[i], &q = v[i + 1], &r = v[i + 2];
        a.tris++;
        if (!std::isfinite(p.x + p.y + p.z + q.x + q.y + q.z + r.x + r.y + r.z)) { a.bad++; continue; }
        const Vector3 n = Vector3CrossProduct(Vector3Subtract(q, p), Vector3Subtract(r, p));
        const float area3 = Vector3Length(n);
        if (area3 < 1e-6f) continue;
        const float ny = fabsf(n.y) / area3;
        const float areaXZ = 0.5f * fabsf((q.x - p.x) * (r.z - p.z) - (q.z - p.z) * (r.x - p.x));
        const float L = std::max({ Vector3Distance(p, q), Vector3Distance(q, r), Vector3Distance(p, r) });
        if (ny < 0.5f && 0.5f * area3 > 0.5f) {
            a.cliffs++;
            const float steep = 1.0f - ny;
            if (getenv("AUDIT_DUMP") && a.cliffs <= 8)
                std::printf("  cliff tri: (%.2f,%.2f,%.2f) (%.2f,%.2f,%.2f) (%.2f,%.2f,%.2f) areaXZ %.3f\n", p.x, p.y, p.z, q.x, q.y, q.z, r.x, r.y, r.z, areaXZ);
            if (steep > a.worstCliff) { a.worstCliff = steep; a.cx = (p.x + q.x + r.x) / 3; a.cz = (p.z + q.z + r.z) / 3; }
        }
        // Long and thin seen from above (xz area small against length^2), and big enough to see.
        if (L > 6.0f && areaXZ > 0.2f && areaXZ < 0.012f * L * L) {
            a.spikes++;
            if (getenv("AUDIT_DUMP") && a.spikes <= 6)
                std::printf("  spike tri: (%.2f,%.2f,%.2f) (%.2f,%.2f,%.2f) (%.2f,%.2f,%.2f) areaXZ %.3f L %.1f\n", p.x, p.y, p.z, q.x, q.y, q.z, r.x, r.y, r.z, areaXZ, L);
            const float s = L * L / std::max(areaXZ, 1e-3f);
            if (s > a.worstSpike) { a.worstSpike = s; a.wx = (p.x + q.x + r.x) / 3; a.wz = (p.z + q.z + r.z) / 3; }
        }
    }
    return a;
}

static uint32_t Hash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }

int main() {
    InitWindow(256, 256, "city_surface_audit");
    int fail = 0;
    long sumFlat = 0, sumHill = 0, cliffFlat = 0, cliffHill = 0;
    const int first = getenv("AUDIT_DUMP") ? atoi(getenv("AUDIT_DUMP")) : 1, last = getenv("AUDIT_DUMP") ? first : 12;
    for (int seed = first; seed <= last; seed++) {
        for (int mode = 0; mode < 2; mode++) {
            City c;
            c.GetParams().gridX = 5; c.GetParams().gridZ = 5;
            c.GetParams().organic = true;
            c.GetParams().organicStrength = 0.3f + 0.1f * (float)(seed % 6);
            c.GetParams().seed = seed * 101;
            c.GetParams().cars = 0;
            c.GenerateGrid({ 0.0f, 0.0f });
            if (mode == 1) {
                for (int i = 0; i < (int)c.GetNodes().size(); i++) {
                    // Rolling ground: smooth hills (up to ~12 m over 100 m of road) plus a little per-node noise.
                    const Vector2 pp = c.GetNodes()[(size_t)i].pos;
                    const float ph = (float)(seed % 7);
                    const float h = 6.0f + 3.5f * sinf(pp.x * 0.05f + ph) + 3.0f * cosf(pp.y * 0.055f + ph * 0.7f) +
                                    0.4f * ((float)(Hash((uint32_t)(i * 31 + seed)) % 1000u) / 1000.0f - 0.5f);
                    c.SetNodeHeight(i, h);
                }
            }
            if (getenv("AUDIT_DUMP") && mode == 1) {
                for (int i = 0; i < (int)c.GetNodes().size(); i++) {
                    const RoadNode& nd = c.GetNodes()[(size_t)i];
                    if (Vector2Distance(nd.pos, { 78.0f, 102.0f }) < 45.0f) std::printf("  node %d at (%.1f,%.1f) h %.2f junction %d\n", i, nd.pos.x, nd.pos.y, nd.h, (int)nd.junction);
                }
                for (int e = 0; e < (int)c.GetEdges().size(); e++) {
                    const RoadEdge& ed = c.GetEdges()[(size_t)e];
                    const Vector2 pa = c.GetNodes()[(size_t)ed.a].pos, pb = c.GetNodes()[(size_t)ed.b].pos;
                    if (Vector2Distance(pa, { 78.0f, 102.0f }) < 40.0f && Vector2Distance(pb, { 78.0f, 102.0f }) < 60.0f) std::printf("  edge %d: node %d -> %d\n", e, ed.a, ed.b);
                }
            }
            const Audit a = Run(c);
            std::printf("seed %2d %-4s: %5d tris, cliffs %3d (worst %.2f @ %.0f,%.0f), spikes %3d (worst %.0f @ %.0f,%.0f), bad %d\n", seed, mode ? "hill" : "flat",
                        a.tris, a.cliffs, a.worstCliff, a.cx, a.cz, a.spikes, a.worstSpike, a.wx, a.wz, a.bad);
            (mode ? sumHill : sumFlat) += a.spikes;
            (mode ? cliffHill : cliffFlat) += a.cliffs;
            if (a.bad) fail++;
        }
    }
    // Curtains (zero-area vertical triangles) and sideways normals used to make thousands of cliff triangles on
    // uneven ground; what is left is the steep middle of a short road ramp.
    if (cliffFlat != 0 || cliffHill > 40) { std::printf("FAIL: cliffs flat %ld uneven %ld\n", cliffFlat, cliffHill); fail++; }
    std::printf("spikes: flat total %ld, uneven-ground total %ld; cliffs: flat %ld, uneven %ld\n", sumFlat, sumHill, cliffFlat, cliffHill);
    std::printf(fail ? "city_surface_audit: FAILED\n" : "city_surface_audit: ok\n");
    return fail ? 1 : 0;
}
