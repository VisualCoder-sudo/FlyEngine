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
#include <string>
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

static std::vector<std::pair<std::string, int>> hist;
static uint32_t Hash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }

int main() {
    InitWindow(256, 256, "city_surface_audit");
    int fail = 0;
    long sumFlat = 0, sumHill = 0, cliffFlat = 0, cliffHill = 0, stepsFlat = 0, stepsHill = 0, overFlat = 0, overHill = 0;
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
                    if (Vector2Distance(nd.pos, { -5.0f, 74.0f }) < 45.0f) std::printf("  node %d at (%.1f,%.1f) h %.2f junction %d\n", i, nd.pos.x, nd.pos.y, nd.h, (int)nd.junction);
                }
                for (int e = 0; e < (int)c.GetEdges().size(); e++) {
                    const RoadEdge& ed = c.GetEdges()[(size_t)e];
                    const Vector2 pa = c.GetNodes()[(size_t)ed.a].pos, pb = c.GetNodes()[(size_t)ed.b].pos;
                    if (Vector2Distance(pa, { -5.0f, 74.0f }) < 40.0f && Vector2Distance(pb, { -5.0f, 74.0f }) < 60.0f) {
                        float fa, fb; const float el = Vector2Distance(pa, pb); c.EdgePlateau(e, el, fa, fb);
                        std::printf("  edge %d: node %d -> %d  len %.1f plateau %.1f m / %.1f m  slabHalf %.1f\n", e, ed.a, ed.b, el, fa * el, fb * el, c.EdgeSlabHalf(e));
                    }
                }
            }
            City::GeometryProblems gp;
            c.ComputeGeometryProblems(gp);
            if (mode == 1 && getenv("AUDIT_LAYERS")) {
                std::vector<Vector3> tv; std::vector<float> tl;
                c.DebugSurfaceTriangles(tv); c.DebugSurfaceLayers(tl);
                auto layerAt = [&](const Vector3& p) {
                    for (size_t i = 0; i < tv.size(); i++)
                        if (fabsf(tv[i].x - p.x) < 0.02f && fabsf(tv[i].y - p.y) < 0.02f && fabsf(tv[i].z - p.z) < 0.02f) return tl[i / 3];
                    return -1.0f;
                };
                for (const auto& st : gp.steps) {
                    char key[64]; std::snprintf(key, sizeof key, "L%.2f -> L%.2f", layerAt(st.first), layerAt(st.second));
                    bool found = false;
                    for (auto& h : hist) if (h.first == key) { h.second++; found = true; }
                    if (!found) hist.push_back({ key, 1 });
                }
            }
            (mode ? stepsHill : stepsFlat) += (long)gp.steps.size();
            (mode ? overHill : overFlat) += (long)gp.padOverRoad.size();
            if (getenv("AUDIT_DUMP") && mode == 1)
                for (size_t k = 0; k < gp.steps.size() && k < 12; k++)
                    std::printf("  step at (%.2f, %.2f): y %.2f -> %.2f\n", gp.steps[k].first.x, gp.steps[k].first.z, gp.steps[k].first.y, gp.steps[k].second.y);
            if (getenv("AUDIT_DUMP") && mode == 1) {
                float mx = 0; double avg = 0;
                for (float d : gp.padOverDelta) { mx = std::max(mx, d); avg += d; }
                std::printf("  pad-over-road: max excess %.3f m, mean %.3f m\n", mx, gp.padOverDelta.empty() ? 0.0 : avg / (double)gp.padOverDelta.size());
                // Worst cases first, with the nearest node (distance along the ground) so we know where they are.
                std::vector<size_t> order(gp.padOverRoad.size());
                for (size_t k = 0; k < order.size(); k++) order[k] = k;
                std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return gp.padOverDelta[x] > gp.padOverDelta[y]; });
                for (size_t oi = 0; oi < order.size() && oi < 10; oi++) {
                    const size_t k = order[oi];
                    int nn = 0; float bd = 1e30f;
                    for (int i = 0; i < (int)c.GetNodes().size(); i++) { const float d = Vector2Distance(c.GetNodes()[(size_t)i].pos, { gp.padOverRoad[k].x, gp.padOverRoad[k].z }); if (d < bd) { bd = d; nn = i; } }
                    std::printf("  over at (%.1f, %.1f) pad y %.2f excess %.3f; nearest node %d is %.1f m away (h %.2f)\n", gp.padOverRoad[k].x, gp.padOverRoad[k].z, gp.padOverRoad[k].y, gp.padOverDelta[k], nn, bd, c.GetNodes()[(size_t)nn].h);
                }
            }
            std::printf("  seed %d %s: steps/cracks %zu  pad-over-road %zu\n", seed, mode ? "hill" : "flat", gp.steps.size(), gp.padOverRoad.size());
            if (getenv("AUDIT_XS") && mode == 1) {
                // Cross-section of one edge at 55 % of its length: every surface layer's height at each lateral offset.
                const int ei = atoi(getenv("AUDIT_XS"));
                const RoadEdge& ed = c.GetEdges()[(size_t)ei];
                const Vector2 pa = c.GetNodes()[(size_t)ed.a].pos, pb = c.GetNodes()[(size_t)ed.b].pos;
                const Vector2 dir = Vector2Normalize(Vector2Subtract(pb, pa)), nrm = { -dir.y, dir.x };
                const Vector2 mid = Vector2Add(pa, Vector2Scale(Vector2Subtract(pb, pa), getenv("AUDIT_XSF") ? (float)atof(getenv("AUDIT_XSF")) : 0.55f));
                std::vector<Vector3> tv; std::vector<float> tl;
                c.DebugSurfaceTriangles(tv); c.DebugSurfaceLayers(tl);
                std::printf("  cross-section of edge %d (len %.1f, h %.2f -> %.2f), asphalt half %.1f, slab half %.1f\n", ei, Vector2Distance(pa, pb), c.GetNodes()[(size_t)ed.a].h, c.GetNodes()[(size_t)ed.b].h, c.EdgeAsphaltHalf(ei), c.EdgeSlabHalf(ei));
                for (int off = -14; off <= 14; off += 1) {
                    const float x = mid.x + nrm.x * (float)off, z = mid.y + nrm.y * (float)off;
                    std::printf("   lat %3d:", off);
                    for (size_t i = 0; i + 2 < tv.size(); i += 3) {
                        const Vector3 &A = tv[i], &B = tv[i + 1], &C = tv[i + 2];
                        const float d = (B.z - C.z) * (A.x - C.x) + (C.x - B.x) * (A.z - C.z);
                        if (fabsf(d) < 1e-9f) continue;
                        const float l1 = ((B.z - C.z) * (x - C.x) + (C.x - B.x) * (z - C.z)) / d, l2 = ((C.z - A.z) * (x - C.x) + (A.x - C.x) * (z - C.z)) / d, l3 = 1 - l1 - l2;
                        if (l1 < 0 || l2 < 0 || l3 < 0) continue;
                        std::printf(" [L%.2f y%.2f]", tl[i / 3], l1 * A.y + l2 * B.y + l3 * C.y);
                    }
                    std::printf("\n");
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
    // Regression bounds (uneven ground, 12 cities). Before the road-twist fix pad-over-road was 1187 and steps 21.
    if (cliffFlat != 0 || cliffHill > 80) { std::printf("FAIL: cliffs flat %ld uneven %ld\n", cliffFlat, cliffHill); fail++; }
    if (stepsFlat != 0 || stepsHill > 40) { std::printf("FAIL: steps flat %ld uneven %ld\n", stepsFlat, stepsHill); fail++; }
    if (overFlat != 0 || overHill > 700) { std::printf("FAIL: pad-over-road flat %ld uneven %ld\n", overFlat, overHill); fail++; }
    for (auto& h : hist) std::printf("  step layers %s : %d\n", h.first.c_str(), h.second);
    std::printf("steps/cracks: flat %ld, uneven %ld; pad-over-road: flat %ld, uneven %ld\n", stepsFlat, stepsHill, overFlat, overHill);
    std::printf("spikes: flat total %ld, uneven-ground total %ld; cliffs: flat %ld, uneven %ld\n", sumFlat, sumHill, cliffFlat, cliffHill);
    std::printf(fail ? "city_surface_audit: FAILED\n" : "city_surface_audit: ok\n");
    return fail ? 1 : 0;
}
