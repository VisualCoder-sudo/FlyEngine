// BasicTerrainErode.cpp - grid-based erosion for BasicTerrain.
//
// Two cooperating passes, both operating on a snapshot of a rect and writing
// to a scratch buffer, so a single pass is free of cell-ordering bias:
//
//  * Thermal: mass-conserving talus relaxation. Any slope steeper than the
//    repose angle sheds material to its lower neighbours until the whole
//    region rests at or below the angle of repose. This is what rounds off
//    jagged peaks and widens valleys.
//
//  * Hydraulic: a rain/flow/sediment model. Each pass rains, water runs to the
//    steepest downhill neighbour, scours soil off the uphill cell (so it
//    carves channels and gullies rather than just smoothing), and re-deposits
//    a tunable fraction at the outlet - the rest is washed out of the system,
//    so hydraulic passes slowly lower the terrain, like rainfall wash.
//
// The brush path (ApplyBrush) runs these on a padded snapshot and blends the
// resulting height deltas by the brush falloff; ErodeRegion() writes the
// result back uniformly over a cell rectangle.

#include "../../include/Terrain/BasicTerrain.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

struct NeighborOffset { int dx, dz; };

constexpr NeighborOffset kNeighbors4[4] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };

// One mass-conserving talus pass. Reads `src`, writes `dst` (dst starts as a
// copy of src). `talusSlope` is the maximum height difference allowed between
// adjacent cells in world units (repose angle converted by cellSize).
void ThermalPass(float* dst, const float* src, int w, int d, float strength, float talusSlope) {
    for (int z = 0; z < d; z++) {
        for (int x = 0; x < w; x++) {
            const int i = z * w + x;
            const float hi = src[i];
            for (const NeighborOffset& nb : kNeighbors4) {
                const int nx = x + nb.dx, nz = z + nb.dz;
                if (nx < 0 || nz < 0 || nx >= w || nz >= d) continue;
                const int n = nz * w + nx;
                const float diff = hi - src[n];
                if (diff > talusSlope) {
                    // Split the excess between erosion (moved off) and the
                    // neighbour (deposited). Strength scales the amount moved.
                    const float amount = (diff - talusSlope) * strength;
                    dst[i] -= amount;
                    dst[n] += amount;
                }
            }
        }
    }
}

// One hydraulic iteration inside RunErosion. Reads `cur` (current heights,
// do not modify) and the persistent `water` film; writes the new heights and
// water into out/waterOut. Order: rain, outlet/outflow computation, then a
// single write-back from snapshots (no ordering bias).
void HydraulicIteration(float* out, const float* cur, int w, int d, float cellSize,
                        std::vector<float>& water, std::vector<float>& waterOut,
                        std::vector<float>& outflow, std::vector<float>& inflow,
                        std::vector<float>& deposit, std::vector<float>& erosion,
                        const BasicTerrain::ErosionSettings& s) {
    const size_t count = (size_t)w * d;
    const float hydraulic = std::clamp(s.hydraulic, 0.0f, 2.0f);
    const float hydroK = 0.06f;   // base carving rate (tuned for scale ~8 world units)

    for (size_t i = 0; i < count; i++) {
        water[i] += s.rain;
        outflow[i] = 0.0f;
        inflow[i] = 0.0f;
        deposit[i] = 0.0f;
        erosion[i] = 0.0f;
    }

    // Pass 1: compute outlets/outflows from snapshot water levels. Only the
    // single steepest downhill neighbour receives water, which concentrates
    // flow along gullies instead of spreading it evenly.
    for (int z = 0; z < d; z++) {
        for (int x = 0; x < w; x++) {
            const int i = z * w + x;
            float bestLvl = cur[i] + water[i];
            int outlet = -1;
            for (const NeighborOffset& nb : kNeighbors4) {
                const int nx = x + nb.dx, nz = z + nb.dz;
                if (nx < 0 || nz < 0 || nx >= w || nz >= d) continue;
                const int n = nz * w + nx;
                const float lvl = cur[n] + water[n];
                if (lvl < bestLvl) { bestLvl = lvl; outlet = n; }
            }
            if (outlet < 0) continue;

            const float dh = (cur[i] + water[i]) - bestLvl;
            if (dh <= 1e-4f) continue;

            // Steeper water-level gradient -> a higher fraction of the water
            // film moves per pass.
            const float k = std::min(1.0f, dh / (cellSize * 0.5f));
            const float flow = water[i] * k;
            if (flow <= 0.0f) continue;

            outflow[i] = flow;
            inflow[outlet] += flow;

            // Scour the uphill cell; cap so one pass can never overshoot the
            // current water-level difference (keeps the pass stable).
            float erode = flow * std::min(1.0f, dh / cellSize) * hydraulic * hydroK;
            erode = std::min(erode, dh * 0.5f);
            erosion[i] = erode;
            deposit[outlet] += erode * std::clamp(s.deposit, 0.0f, 1.0f);
        }
    }

    // Pass 2: apply water transport + height changes, reading only snapshots.
    for (size_t i = 0; i < count; i++) {
        waterOut[i] = water[i] - outflow[i] + inflow[i];
        out[i] = cur[i] - erosion[i] + deposit[i];
    }

    for (size_t i = 0; i < count; i++) {
        water[i] = waterOut[i] * (1.0f - std::clamp(s.evaporation, 0.0f, 1.0f));
    }
}

} // namespace

void BasicTerrain::RunErosion(float* heights, int w, int d, float cellSize,
                              const ErosionSettings& s, int iterations) {
    if (heights == nullptr || w < 2 || d < 2 || iterations <= 0) return;

    const size_t count = (size_t)w * d;
    std::vector<float> cur(heights, heights + count);
    std::vector<float> out(count);
    std::vector<float> water(count, 0.0f);
    std::vector<float> waterOut(count, 0.0f);
    std::vector<float> outflow(count, 0.0f);
    std::vector<float> inflow(count, 0.0f);
    std::vector<float> deposit(count, 0.0f);
    std::vector<float> erosion(count, 0.0f);

    const float talusSlope = std::tan(s.talusDeg * 3.14159265f / 180.0f) * cellSize;
    const float thermal = std::clamp(s.thermal, 0.0f, 2.0f);

    for (int iter = 0; iter < iterations; iter++) {
        if (thermal > 0.0f) {
            for (size_t i = 0; i < count; i++) out[i] = cur[i];
            ThermalPass(out.data(), cur.data(), w, d, thermal * 0.25f, talusSlope);
            cur.swap(out);
        }
        if (s.hydraulic > 0.0f) {
            HydraulicIteration(out.data(), cur.data(), w, d, cellSize,
                               water, waterOut, outflow, inflow, deposit, erosion, s);
            cur.swap(out);
        }
    }

    for (size_t i = 0; i < count; i++) heights[i] = cur[i];
}

void BasicTerrain::ErodeRegion(int x0, int z0, int x1, int z1,
                               const ErosionSettings& s, float dt) {
    x0 = std::clamp(x0, 0, width - 1);
    x1 = std::clamp(x1, 0, width - 1);
    z0 = std::clamp(z0, 0, depth - 1);
    z1 = std::clamp(z1, 0, depth - 1);
    if (x1 < x0 || z1 < z0) return;

    // Pad by one cell so the boundary cells see correct neighbours during the
    // passes. Only the inner (requested) rectangle is written back.
    const int px0 = std::max(0, x0 - 1), px1 = std::min(width - 1, x1 + 1);
    const int pz0 = std::max(0, z0 - 1), pz1 = std::min(depth - 1, z1 + 1);
    const int rw = px1 - px0 + 1;
    const int rd = pz1 - pz0 + 1;

    std::vector<float> buf((size_t)rw * rd);
    for (int z = pz0; z <= pz1; z++)
        for (int x = px0; x <= px1; x++)
            buf[(size_t)(z - pz0) * rw + (x - px0)] = heightmap[(size_t)z * width + x];

    const int iterations = std::max(1, (int)std::lroundf(s.iterations * std::max(dt, 0.05f)));
    RunErosion(buf.data(), rw, rd, scale, s, iterations);

    for (int z = z0; z <= z1; z++)
        for (int x = x0; x <= x1; x++)
            heightmap[(size_t)z * width + x] = std::clamp(
                buf[(size_t)(z - pz0) * rw + (x - px0)], minHeight, maxHeight);

    meshDirty = true;
}