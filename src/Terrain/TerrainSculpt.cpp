#include "../../include/Terrain/TerrainSculpt.hpp"
#include "../../include/Terrain/Terrain.hpp"
#include "../../include/Terrain/TerrainTypes.hpp"
#include "raylib.h"
#include "raymath.h"
#include <algorithm>
#include <cmath>

namespace terrain {

// ============================================================================
// Brush Falloff Functions
// ============================================================================

inline float BrushFalloff(float distance, float radius, float hardness) {
    if (distance >= radius) return 0.0f;
    float t = distance / radius;
    // Smoothstep with hardness control
    float smooth = 1.0f - t * t * (3.0f - 2.0f * t); // Smoothstep
    return std::pow(smooth, 1.0f + hardness * 4.0f);
}

inline float BrushFalloffLinear(float distance, float radius) {
    if (distance >= radius) return 0.0f;
    return 1.0f - distance / radius;
}

// ============================================================================
// Deterministic Noise Functions (ported from BasicTerrain.cpp)
// ============================================================================

static uint32_t TerrainHash(int x, int y, int seed) {
    uint32_t h = (uint32_t)(x * 374761393 + y * 668265263) ^ (uint32_t)(seed * 2654435761u);
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= (h >> 16);
    return h;
}

static float TerrainValueNoise(float x, float y, int seed) {
    int ix = (int)std::floor(x);
    int iy = (int)std::floor(y);
    float fx = x - ix;
    float fy = y - iy;

    float v00 = (TerrainHash(ix,     iy,     seed) & 0xFFFFFF) / (float)0xFFFFFF;
    float v10 = (TerrainHash(ix + 1, iy,     seed) & 0xFFFFFF) / (float)0xFFFFFF;
    float v01 = (TerrainHash(ix,     iy + 1, seed) & 0xFFFFFF) / (float)0xFFFFFF;
    float v11 = (TerrainHash(ix + 1, iy + 1, seed) & 0xFFFFFF) / (float)0xFFFFFF;

    float sx = fx * fx * (3.0f - 2.0f * fx);
    float sy = fy * fy * (3.0f - 2.0f * fy);
    float top = v00 + (v10 - v00) * sx;
    float bot = v01 + (v11 - v01) * sx;
    return top + (bot - top) * sy; // [0,1)
}

static float TerrainFbm(float x, float y, int seed, int octaves, float persistence, float lacunarity) {
    float amp = 1.0f, freq = 1.0f, sum = 0.0f, norm = 0.0f;
    for (int o = 0; o < octaves; o++) {
        sum += TerrainValueNoise(x * freq, y * freq, seed + o * 101) * amp;
        norm += amp;
        amp *= persistence;
        freq *= lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f; // [0,1)
}

// ============================================================================
// Erosion Structures and Core Algorithm (adapted from BasicTerrainErode.cpp)
// ============================================================================

struct ErosionSettings {
    float thermal = 0.5f;       // Talus relaxation amount per pass
    float hydraulic = 0.8f;     // Rain-flow carving amount per pass
    float talusDeg = 3.0f;      // Repose angle in degrees
    float rain = 0.05f;         // Rainfall per pass
    float evaporation = 0.03f;  // Water evaporation fraction per pass
    float deposit = 0.6f;       // Fraction of eroded soil re-deposited
    int iterations = 6;         // Number of passes
};

struct NeighborOffset { int dx, dz; };
constexpr NeighborOffset kNeighbors4[4] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };

// One mass-conserving talus pass. Reads `src`, writes `dst` (dst starts as copy of src).
// `talusSlope` is max height difference allowed between adjacent cells in world units.
static void ThermalPass(float* dst, const float* src, int w, int d, float strength, float talusSlope) {
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
                    const float amount = (diff - talusSlope) * strength;
                    dst[i] -= amount;
                    dst[n] += amount;
                }
            }
        }
    }
}

// One hydraulic iteration. Reads `cur` (heights), `water` film; writes new heights/water to out/waterOut.
static void HydraulicIteration(float* out, const float* cur, int w, int d, float cellSize,
                               std::vector<float>& water, std::vector<float>& waterOut,
                               std::vector<float>& outflow, std::vector<float>& inflow,
                               std::vector<float>& deposit, std::vector<float>& erosion,
                               const ErosionSettings& s) {
    const size_t count = (size_t)w * d;
    const float hydraulic = std::clamp(s.hydraulic, 0.0f, 2.0f);
    const float hydroK = 0.06f;   // Base carving rate

    for (size_t i = 0; i < count; i++) {
        water[i] += s.rain;
        outflow[i] = 0.0f;
        inflow[i] = 0.0f;
        deposit[i] = 0.0f;
        erosion[i] = 0.0f;
    }

    // Pass 1: compute outlets/outflows from snapshot water levels
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

            const float k = std::min(1.0f, dh / (cellSize * 0.5f));
            const float flow = water[i] * k;
            if (flow <= 0.0f) continue;

            outflow[i] = flow;
            inflow[outlet] += flow;

            // Scour the uphill cell
            float erode = flow * std::min(1.0f, dh / cellSize) * hydraulic * hydroK;
            erode = std::min(erode, dh * 0.5f);
            erosion[i] = erode;
            deposit[outlet] += erode * std::clamp(s.deposit, 0.0f, 1.0f);
        }
    }

    // Pass 2: apply water transport + height changes
    for (size_t i = 0; i < count; i++) {
        waterOut[i] = water[i] - outflow[i] + inflow[i];
        out[i] = cur[i] - erosion[i] + deposit[i];
    }

    for (size_t i = 0; i < count; i++) {
        water[i] = waterOut[i] * (1.0f - std::clamp(s.evaporation, 0.0f, 1.0f));
    }
}

// Stateless erosion core: runs thermal + hydraulic passes on a height buffer.
static void RunErosionOnRegion(float* heights, int w, int d, float cellSize,
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

// Apply erosion to a world-space rectangular region across all chunks.
void ApplyErosionToRegion(Terrain& terrain, float minWX, float minWZ, float maxWX, float maxWZ,
                                  const TerrainBrush& brush) {
    // Convert world bounds to grid cell coordinates (uniform cell size across terrain)
    // Use the first chunk's resolution/cell size as reference
    auto& allChunks = TerrainSculptAccess::GetChunks(terrain);
    if (allChunks.empty()) return;

    const TerrainChunk& refChunk = allChunks[0];
    const float cellSize = refChunk.worldSize / (refChunk.resolution - 1);
    const float halfWorldW = terrain.size.x * 0.5f;
    const float halfWorldD = terrain.size.z * 0.5f;

    // World -> terrain-local grid coordinates
    int gx0 = (int)std::floor((minWX - (terrain.center.x - halfWorldW)) / cellSize);
    int gz0 = (int)std::floor((minWZ - (terrain.center.z - halfWorldD)) / cellSize);
    int gx1 = (int)std::ceil((maxWX - (terrain.center.x - halfWorldW)) / cellSize);
    int gz1 = (int)std::ceil((maxWZ - (terrain.center.z - halfWorldD)) / cellSize);

    // Clamp to terrain grid
    int totalW = terrain.gridWidth * (refChunk.resolution - 1);
    int totalD = terrain.gridDepth * (refChunk.resolution - 1);
    gx0 = std::clamp(gx0, 0, totalW - 1);
    gz0 = std::clamp(gz0, 0, totalD - 1);
    gx1 = std::clamp(gx1, 0, totalW - 1);
    gz1 = std::clamp(gz1, 0, totalD - 1);

    if (gx1 <= gx0 || gz1 <= gz0) return;

    // Pad by 1 cell for neighbor access during erosion
    const int pad = 1;
    int px0 = std::max(0, gx0 - pad);
    int pz0 = std::max(0, gz0 - pad);
    int px1 = std::min(totalW - 1, gx1 + pad);
    int pz1 = std::min(totalD - 1, gz1 + pad);

    const int rw = px1 - px0 + 1;
    const int rd = pz1 - pz0 + 1;
    if (rw < 3 || rd < 3) return;

    // Extract heightmap region into contiguous buffer
    std::vector<float> buf((size_t)rw * rd);
    for (int z = pz0; z <= pz1; z++) {
        for (int x = px0; x <= px1; x++) {
            // Find which chunk this grid cell belongs to
            int chunkGX = x / (refChunk.resolution - 1);
            int chunkGZ = z / (refChunk.resolution - 1);
            int localX = x % (refChunk.resolution - 1);
            int localZ = z % (refChunk.resolution - 1);

            // Find the chunk
            const TerrainChunk* chunk = nullptr;
            for (auto& c : allChunks) {
                if ((int)c.chunkCoord.x == chunkGX && (int)c.chunkCoord.y == chunkGZ) {
                    chunk = &c;
                    break;
                }
            }
            if (!chunk) continue;

            float h = chunk->heightmap[(size_t)localZ * chunk->resolution + localX];
            buf[(size_t)(z - pz0) * rw + (x - px0)] = h;
        }
    }

    // Convert brush erosion settings to ErosionSettings
    ErosionSettings es;
    es.thermal = brush.erosionThermal;
    es.hydraulic = brush.erosionHydraulic;
    es.talusDeg = brush.erosionTalusDeg;
    es.rain = brush.erosionRain;
    es.evaporation = brush.erosionEvaporation;
    es.deposit = brush.erosionDeposit;
    es.iterations = brush.erosionIterations;

    // Run erosion
    RunErosionOnRegion(buf.data(), rw, rd, cellSize, es, es.iterations);

    // Write back to chunks with brush falloff blending
    // Compute brush center in grid coordinates
    float brushCX = (brush.radius > 0) ? (minWX + maxWX) * 0.5f : 0; // fallback
    float brushCZ = (brush.radius > 0) ? (minWZ + maxWZ) * 0.5f : 0;
    // Actually use the brush center passed to the function... but we don't have it here.
    // For region erosion (Generate tab), we use uniform weight (no falloff).
    // For brush erosion, this function won't be called directly; instead the brush
    // path in ApplyBrush will handle falloff blending.

    // For now, write back uniformly (for Generate tab Erode Region/All)
    for (int z = gz0; z <= gz1; z++) {
        for (int x = gx0; x <= gx1; x++) {
            int chunkGX = x / (refChunk.resolution - 1);
            int chunkGZ = z / (refChunk.resolution - 1);
            int localX = x % (refChunk.resolution - 1);
            int localZ = z % (refChunk.resolution - 1);

            TerrainChunk* chunk = nullptr;
            for (auto& c : allChunks) {
                if ((int)c.chunkCoord.x == chunkGX && (int)c.chunkCoord.y == chunkGZ) {
                    chunk = &c;
                    break;
                }
            }
            if (!chunk) continue;

            float newH = buf[(size_t)(z - pz0) * rw + (x - px0)];
            float& h = chunk->heightmap[(size_t)localZ * chunk->resolution + localX];
            h = std::clamp(newH, TerrainSculptAccess::MinHeight(terrain), TerrainSculptAccess::MaxHeight(terrain));
            chunk->dirty = true;
            chunk->physicsDirty = true;
        }
    }

    TerrainSculptAccess::OnHeightmapChanged(terrain)();
    TerrainSculptAccess::NeedsPhysicsRebuild(terrain) = true;
}

// Apply erosion as a brush operation with falloff blending across chunks.
static void ApplyErosionBrush(Terrain& terrain,
                               const std::vector<TerrainChunk*>& chunks,
                               Vector2 center,
                               const TerrainBrush& brush,
                               float strength,
                               float dt) {
    if (chunks.empty()) return;

    // Find world-space bounds of all affected chunks (expanded by brush radius)
    float minWX = FLT_MAX, minWZ = FLT_MAX, maxWX = -FLT_MAX, maxWZ = -FLT_MAX;
    for (auto* chunk : chunks) {
        minWX = std::min(minWX, chunk->bounds.min.x);
        minWZ = std::min(minWZ, chunk->bounds.min.z);
        maxWX = std::max(maxWX, chunk->bounds.max.x);
        maxWZ = std::max(maxWZ, chunk->bounds.max.z);
    }

    // Expand by brush radius for falloff
    minWX -= brush.radius;
    minWZ -= brush.radius;
    maxWX += brush.radius;
    maxWZ += brush.radius;

    // Convert to grid coordinates
    auto& allChunks = TerrainSculptAccess::GetChunks(terrain);
    const TerrainChunk& refChunk = allChunks[0];
    const float cellSize = refChunk.worldSize / (refChunk.resolution - 1);
    const float halfWorldW = terrain.size.x * 0.5f;
    const float halfWorldD = terrain.size.z * 0.5f;

    int gx0 = (int)std::floor((minWX - (terrain.center.x - halfWorldW)) / cellSize);
    int gz0 = (int)std::floor((minWZ - (terrain.center.z - halfWorldD)) / cellSize);
    int gx1 = (int)std::ceil((maxWX - (terrain.center.x - halfWorldW)) / cellSize);
    int gz1 = (int)std::ceil((maxWZ - (terrain.center.z - halfWorldD)) / cellSize);

    int totalW = terrain.gridWidth * (refChunk.resolution - 1);
    int totalD = terrain.gridDepth * (refChunk.resolution - 1);
    gx0 = std::clamp(gx0, 0, totalW - 1);
    gz0 = std::clamp(gz0, 0, totalD - 1);
    gx1 = std::clamp(gx1, 0, totalW - 1);
    gz1 = std::clamp(gz1, 0, totalD - 1);

    if (gx1 <= gx0 || gz1 <= gz0) return;

    // Pad by 1 cell for neighbor access during erosion
    const int pad = 1;
    int px0 = std::max(0, gx0 - pad);
    int pz0 = std::max(0, gz0 - pad);
    int px1 = std::min(totalW - 1, gx1 + pad);
    int pz1 = std::min(totalD - 1, gz1 + pad);

    const int rw = px1 - px0 + 1;
    const int rd = pz1 - pz0 + 1;
    if (rw < 3 || rd < 3) return;

    // Extract heightmap region into contiguous buffer
    std::vector<float> buf((size_t)rw * rd);
    for (int z = pz0; z <= pz1; z++) {
        for (int x = px0; x <= px1; x++) {
            int chunkGX = x / (refChunk.resolution - 1);
            int chunkGZ = z / (refChunk.resolution - 1);
            int localX = x % (refChunk.resolution - 1);
            int localZ = z % (refChunk.resolution - 1);

            const TerrainChunk* chunk = nullptr;
            for (auto& c : allChunks) {
                if ((int)c.chunkCoord.x == chunkGX && (int)c.chunkCoord.y == chunkGZ) {
                    chunk = &c;
                    break;
                }
            }
            if (!chunk) continue;

            float h = chunk->heightmap[(size_t)localZ * chunk->resolution + localX];
            buf[(size_t)(z - pz0) * rw + (x - px0)] = h;
        }
    }

    // Convert brush erosion settings to ErosionSettings
    ErosionSettings es;
    es.thermal = brush.erosionThermal;
    es.hydraulic = brush.erosionHydraulic;
    es.talusDeg = brush.erosionTalusDeg;
    es.rain = brush.erosionRain;
    es.evaporation = brush.erosionEvaporation;
    es.deposit = brush.erosionDeposit;
    // Scale iterations by strength * dt * 60 for frame-rate independence (like BasicTerrain)
    es.iterations = std::clamp((int)(brush.erosionIterations * strength * dt * 60.0f * 0.05f + 0.5f), 1, 10);

    // Run erosion on the padded region
    RunErosionOnRegion(buf.data(), rw, rd, cellSize, es, es.iterations);

    // Write back to chunks with brush falloff blending
    // Convert brush center to grid coordinates
    float brushGX = (center.x - (terrain.center.x - halfWorldW)) / cellSize;
    float brushGZ = (center.y - (terrain.center.z - halfWorldD)) / cellSize;
    float pixelRadiusGrid = brush.radius / cellSize;

    for (int z = gz0; z <= gz1; z++) {
        for (int x = gx0; x <= gx1; x++) {
            // Compute brush falloff weight
            float ddx = x - brushGX;
            float ddz = z - brushGZ;
            float dist = std::sqrt(ddx * ddx + ddz * ddz);
            float n = dist / pixelRadiusGrid;
            if (n >= 1.0f) continue;

            float weight = 1.0f - n * n * (3.0f - 2.0f * n); // smoothstep
            weight = std::pow(weight, 1.0f + brush.hardness * 4.0f);
            if (weight <= 0.0f) continue;

            int chunkGX = x / (refChunk.resolution - 1);
            int chunkGZ = z / (refChunk.resolution - 1);
            int localX = x % (refChunk.resolution - 1);
            int localZ = z % (refChunk.resolution - 1);

            TerrainChunk* chunk = nullptr;
            for (auto& c : allChunks) {
                if ((int)c.chunkCoord.x == chunkGX && (int)c.chunkCoord.y == chunkGZ) {
                    chunk = &c;
                    break;
                }
            }
            if (!chunk) continue;

            float newH = buf[(size_t)(z - pz0) * rw + (x - px0)];
            float& h = chunk->heightmap[(size_t)localZ * chunk->resolution + localX];
            float before = h;
            h = std::clamp(before + (newH - before) * weight,
                          TerrainSculptAccess::MinHeight(terrain),
                          TerrainSculptAccess::MaxHeight(terrain));
            chunk->dirty = true;
            chunk->physicsDirty = true;
        }
    }

    // Update min/max height for all affected chunks
    for (auto* chunk : chunks) {
        for (float h : chunk->heightmap) {
            float& minH = TerrainSculptAccess::MinHeight(terrain);
            float& maxH = TerrainSculptAccess::MaxHeight(terrain);
            minH = std::min(minH, h);
            maxH = std::max(maxH, h);
        }
    }

    TerrainSculptAccess::OnHeightmapChanged(terrain)();
    TerrainSculptAccess::NeedsPhysicsRebuild(terrain) = true;
}

// ============================================================================
// Brush Operations
// ============================================================================

void ApplyBrush(Terrain& terrain, const TerrainBrush& brush, Vector2 center, float dt) {
    auto chunks = TerrainSculptAccess::GetChunksInRadius(terrain, center, brush.radius);
    
    // Push undo state for affected chunks
    for (auto* chunk : chunks) {
        int idx = -1;
        auto& allChunks = TerrainSculptAccess::GetChunks(terrain);
        for (int i = 0; i < (int)allChunks.size(); i++) {
            if (&allChunks[i] == chunk) { idx = i; break; }
        }
        if (idx >= 0) terrain.PushUndo(idx);
    }
    
    float strength = brush.strength;
    float radius = brush.radius;
    float hardness = brush.hardness;
    
    for (auto* chunk : chunks) {
        int res = chunk->resolution;
        float worldSize = chunk->worldSize;
        
        // Convert brush center to chunk-local coordinates
        Vector2 localCenter = WorldToChunkUV(center.x, center.y, *chunk);
        localCenter.x *= (res - 1);
        localCenter.y *= (res - 1);
        
        // Radius in heightmap pixels
        float pixelRadius = radius / worldSize * (res - 1);
        
        // Bounds of affected area
        int minX = std::max(0, (int)std::floor(localCenter.x - pixelRadius));
        int maxX = std::min(res - 1, (int)std::ceil(localCenter.x + pixelRadius));
        int minZ = std::max(0, (int)std::floor(localCenter.y - pixelRadius));
        int maxZ = std::min(res - 1, (int)std::ceil(localCenter.y + pixelRadius));
        
        for (int z = minZ; z <= maxZ; z++) {
            for (int x = minX; x <= maxX; x++) {
                float dx = x - localCenter.x;
                float dz = z - localCenter.y;
                
                // Normalized distance to brush edge: circle = Euclidean, square = Chebyshev
                float n;
                if (brush.shape == TerrainBrush::Shape::Square)
                    n = std::max(std::fabs(dx), std::fabs(dz)) / pixelRadius;
                else
                    n = std::sqrt(dx * dx + dz * dz) / pixelRadius;
                if (n >= 1.0f) continue;
                
                float weight = 1.0f - n * n * (3.0f - 2.0f * n);   // smoothstep falloff
                weight = std::pow(weight, 1.0f + hardness * 4.0f);
                if (weight <= 0.0f) continue;
                
                int idx = z * res + x;
                float& height = chunk->heightmap[idx];
                
                switch (brush.tool) {
                    case TerrainTool::Raise:
                        if (brush.addMode) height += strength * dt * 0.25f * weight;
                        else height -= strength * dt * 0.25f * weight;
                        break;
                        
                    case TerrainTool::Lower:
                        if (brush.addMode) height -= strength * dt * 0.25f * weight;
                        else height += strength * dt * 0.25f * weight;
                        break;
                        
                    case TerrainTool::Smooth: {
                        // Laplacian smoothing
                        float sum = 0.0f;
                        int count = 0;
                        if (x > 0) { sum += chunk->heightmap[idx - 1]; count++; }
                        if (x < res - 1) { sum += chunk->heightmap[idx + 1]; count++; }
                        if (z > 0) { sum += chunk->heightmap[idx - res]; count++; }
                        if (z < res - 1) { sum += chunk->heightmap[idx + res]; count++; }
                        if (count > 0) {
                            float avg = sum / count;
                            float factor = std::clamp(strength * 0.05f * dt, 0.0f, 1.0f);
                            height = height + (avg - height) * factor * weight;
                        }
                        break;
                    }
                    
                    case TerrainTool::Flatten: {
                        float factor = std::clamp(strength * 0.05f * dt, 0.0f, 1.0f);
                        height = height + (brush.targetHeight - height) * factor * weight;
                        break;
                    }
                    
                    case TerrainTool::Ramp:
                        // Handled separately in RampTerrain
                        break;
                        
                    case TerrainTool::Noise: {
                        // Deterministic FBM noise based on world position
                        Vector2 uv = { x / (float)(res - 1), z / (float)(res - 1) };
                        Vector2 worldPos = {
                            chunk->bounds.min.x + uv.x * worldSize,
                            chunk->bounds.min.z + uv.y * worldSize
                        };
                        float u = worldPos.x / brush.noiseScale;
                        float v = worldPos.y / brush.noiseScale;
                        float n = TerrainFbm(u, v, brush.noiseSeed, 3, 0.5f, 2.0f) * 2.0f - 1.0f; // [-1, 1]
                        height += n * strength * dt * 0.06f * weight;
                        break;
                    }
                    
                    case TerrainTool::Paint: {
                        // Paint tool: delegate to PaintLayer for proper splatmap handling
                        // We apply a small amount per frame for continuous painting
                        float paintStrength = std::clamp(strength * dt * 0.5f, 0.0f, 1.0f);
                        // Note: Actual painting is done via PaintLayer which handles splatmap properly
                        // This just provides visual feedback for the heightmap if needed
                        break;
                    }
                    
                    case TerrainTool::Erode:
                        // Erosion is handled as a neighborhood operation after the per-chunk loop
                        // We skip per-vertex processing here and do it regionally below
                        break;
    
                    case TerrainTool::None:
                        // No tool selected
                        break;
                }
                
                // Clamp to valid range
                float minH = TerrainSculptAccess::MinHeight(const_cast<Terrain&>(terrain));
                float maxH = TerrainSculptAccess::MaxHeight(const_cast<Terrain&>(terrain));
                height = std::clamp(height, minH, maxH);
            }
        }
        
        chunk->dirty = true;
        chunk->physicsDirty = true;
        
        // Update min/max height
        for (float h : chunk->heightmap) {
            float& minH = TerrainSculptAccess::MinHeight(terrain);
            float& maxH = TerrainSculptAccess::MaxHeight(terrain);
            minH = std::min(minH, h);
            maxH = std::max(maxH, h);
        }
    }
    
    // Handle Erosion tool as a neighborhood operation across all affected chunks
    if (brush.tool == TerrainTool::Erode) {
        ApplyErosionBrush(terrain, chunks, center, brush, strength, dt);
    }
    
    TerrainSculptAccess::OnHeightmapChanged(terrain)();
    TerrainSculptAccess::NeedsPhysicsRebuild(terrain) = true;
}

void RampTerrain(Terrain& terrain, Vector2 start, Vector2 end, float startHeight, float endHeight) {
    Vector2 dir = Vector2Subtract(end, start);
    float length = Vector2Length(dir);
    if (length < 0.001f) return;
    dir = Vector2Scale(dir, 1.0f / length);
    
    float radius = length * 0.5f + 5.0f; // Ramp width
    
    auto chunks = TerrainSculptAccess::GetChunksInRadius(terrain, Vector2Scale(Vector2Add(start, end), 0.5f), radius + length * 0.5f);
    
    for (auto* chunk : chunks) {
        int idx = -1;
        auto& allChunks = TerrainSculptAccess::GetChunks(terrain);
        for (int i = 0; i < (int)allChunks.size(); i++) {
            if (&allChunks[i] == chunk) { idx = i; break; }
        }
        if (idx >= 0) const_cast<Terrain&>(terrain).PushUndo(idx);
        
        int res = chunk->resolution;
        float worldSize = chunk->worldSize;
        
        for (int z = 0; z < res; z++) {
            for (int x = 0; x < res; x++) {
                Vector2 uv = { x / (float)(res - 1), z / (float)(res - 1) };
                Vector2 worldPos = {
                    chunk->bounds.min.x + uv.x * worldSize,
                    chunk->bounds.min.z + uv.y * worldSize
                };
                
                // Project onto ramp line
                Vector2 toPos = Vector2Subtract(worldPos, start);
                float t = Vector2DotProduct(toPos, dir);
                t = std::clamp(t / length, 0.0f, 1.0f);
                
                // Distance from ramp line
                Vector2 proj = Vector2Add(start, Vector2Scale(dir, t * length));
                float dist = Vector2Distance(worldPos, proj);
                
                if (dist > radius) continue;
                
                float weight = BrushFalloffLinear(dist, radius);
                float targetHeight = startHeight + (endHeight - startHeight) * t;
                
                int idx = z * res + x;
                float& height = chunk->heightmap[idx];
                height = height + (targetHeight - height) * weight;
                height = std::clamp(height, TerrainSculptAccess::MinHeight(terrain), TerrainSculptAccess::MaxHeight(terrain));
            }
        }
        
        chunk->dirty = true;
        chunk->physicsDirty = true;
    }
}

void PaintLayer(Terrain& terrain, Vector2 center, float radius, float strength, int layerIndex, bool erase) {
    if (layerIndex < 0 || layerIndex >= 4) return; // Max 4 layers (RGBA)
    
    auto chunks = TerrainSculptAccess::GetChunksInRadius(terrain, center, radius);
    
    for (auto* chunk : chunks) {
        int idx = -1;
        auto& allChunks = TerrainSculptAccess::GetChunks(terrain);
        for (int i = 0; i < (int)allChunks.size(); i++) {
            if (&allChunks[i] == chunk) { idx = i; break; }
        }
        if (idx >= 0) terrain.PushUndo(idx);
        
        int res = chunk->resolution;
        float worldSize = chunk->worldSize;
        
        Vector2 localCenter = WorldToChunkUV(center.x, center.y, *chunk);
        localCenter.x *= (res - 1);
        localCenter.y *= (res - 1);
        
        float pixelRadius = radius / worldSize * (res - 1);
        
        int minX = std::max(0, (int)std::floor(localCenter.x - pixelRadius));
        int maxX = std::min(res - 1, (int)std::ceil(localCenter.x + pixelRadius));
        int minZ = std::max(0, (int)std::floor(localCenter.y - pixelRadius));
        int maxZ = std::min(res - 1, (int)std::ceil(localCenter.y + pixelRadius));
        
        for (int z = minZ; z <= maxZ; z++) {
            for (int x = minX; x <= maxX; x++) {
                float dx = x - localCenter.x;
                float dz = z - localCenter.y;
                float dist = std::sqrt(dx * dx + dz * dz);
                
                float weight = BrushFalloff(dist, pixelRadius, 0.5f);
                if (weight <= 0.0f) continue;
                
                int splatIdx = (z * res + x) * 4;
                uint8_t& layerWeight = chunk->splatmap[splatIdx + layerIndex];
                
                if (erase) {
                    // Reduce this layer, redistribute to others
                    float reduce = std::min((float)layerWeight, strength * weight * 255.0f);
                    layerWeight = (uint8_t)std::max(0.0f, (float)layerWeight - reduce);
                } else {
                    // Increase this layer
                    float add = strength * weight * 255.0f;
                    layerWeight = (uint8_t)std::min(255.0f, (float)layerWeight + add);
                }
                
                // Renormalize to sum = 255
                int sum = 0;
                for (int i = 0; i < 4; i++) sum += chunk->splatmap[splatIdx + i];
                if (sum > 0) {
                    for (int i = 0; i < 4; i++) {
                        chunk->splatmap[splatIdx + i] = (uint8_t)((chunk->splatmap[splatIdx + i] * 255) / sum);
                    }
                }
            }
        }
        
        chunk->dirty = true;
    }
}

// ============================================================================
// Undo/Redo Support
// ============================================================================

void Terrain::PushUndo(int chunkIndex) {
    auto& undoStack = TerrainSculptAccess::UndoStack(*this);
    auto& redoStack = TerrainSculptAccess::RedoStack(*this);
    auto& chunks = TerrainSculptAccess::GetChunks(*this);
    
    if (chunkIndex < 0 || chunkIndex >= (int)chunks.size()) return;
    
    TerrainChunk& chunk = chunks[chunkIndex];
    
    Terrain::UndoEntry entry;
    entry.chunkIndex = chunkIndex;
    entry.previousHeightmap = chunk.heightmap;
    entry.previousSplatmap = chunk.splatmap;
    
    undoStack.push_back(entry);
    if ((int)undoStack.size() > MAX_UNDO_ENTRIES) {
        undoStack.erase(undoStack.begin());
    }
    redoStack.clear();
}

void Terrain::ClearRedo() {
    auto& redoStack = TerrainSculptAccess::RedoStack(*this);
    redoStack.clear();
}

bool Terrain::Undo() {
    auto& undoStack = TerrainSculptAccess::UndoStack(*this);
    auto& redoStack = TerrainSculptAccess::RedoStack(*this);
    auto& chunks = TerrainSculptAccess::GetChunks(*this);
    
    if (undoStack.empty()) return false;
    
    Terrain::UndoEntry entry = undoStack.back();
    undoStack.pop_back();
    
    if (entry.chunkIndex >= 0 && entry.chunkIndex < (int)chunks.size()) {
        TerrainChunk& chunk = chunks[entry.chunkIndex];
        
        // Save current state to redo
        Terrain::UndoEntry redoEntry;
        redoEntry.chunkIndex = entry.chunkIndex;
        redoEntry.previousHeightmap = chunk.heightmap;
        redoEntry.previousSplatmap = chunk.splatmap;
        redoStack.push_back(redoEntry);
        
        // Restore
        chunk.heightmap = entry.previousHeightmap;
        chunk.splatmap = entry.previousSplatmap;
        chunk.dirty = true;
        chunk.physicsDirty = true;
        
        TerrainSculptAccess::NeedsPhysicsRebuild(*this) = true;
        TerrainSculptAccess::OnHeightmapChanged(*this)();
    }
    
    return true;
}

bool Terrain::Redo() {
    auto& undoStack = TerrainSculptAccess::UndoStack(*this);
    auto& redoStack = TerrainSculptAccess::RedoStack(*this);
    auto& chunks = TerrainSculptAccess::GetChunks(*this);
    
    if (redoStack.empty()) return false;
    
    Terrain::UndoEntry entry = redoStack.back();
    redoStack.pop_back();
    
    if (entry.chunkIndex >= 0 && entry.chunkIndex < (int)chunks.size()) {
        TerrainChunk& chunk = chunks[entry.chunkIndex];
        
        // Save current state to undo
        Terrain::UndoEntry undoEntry;
        undoEntry.chunkIndex = entry.chunkIndex;
        undoEntry.previousHeightmap = chunk.heightmap;
        undoEntry.previousSplatmap = chunk.splatmap;
        undoStack.push_back(undoEntry);
        
        // Restore
        chunk.heightmap = entry.previousHeightmap;
        chunk.splatmap = entry.previousSplatmap;
        chunk.dirty = true;
        chunk.physicsDirty = true;
        
        TerrainSculptAccess::NeedsPhysicsRebuild(*this) = true;
        TerrainSculptAccess::OnHeightmapChanged(*this)();
    }
    
    return true;
}

void Terrain::ClearUndoRedo() {
    auto& undoStack = TerrainSculptAccess::UndoStack(*this);
    auto& redoStack = TerrainSculptAccess::RedoStack(*this);
    undoStack.clear();
    redoStack.clear();
}

} // namespace terrain