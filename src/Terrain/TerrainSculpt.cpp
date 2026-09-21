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
// Brush Operations
// ============================================================================

void ApplyBrush(Terrain& terrain, const TerrainBrush& brush, Vector2 center) {
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
                float dist = std::sqrt(dx * dx + dz * dz);
                
                float weight = BrushFalloff(dist, pixelRadius, hardness);
                if (weight <= 0.0f) continue;
                
                int idx = z * res + x;
                float& height = chunk->heightmap[idx];
                
                switch (brush.tool) {
                    case TerrainTool::Raise:
                        if (brush.addMode) height += strength * weight;
                        else height -= strength * weight;
                        break;
                        
                    case TerrainTool::Lower:
                        if (brush.addMode) height -= strength * weight;
                        else height += strength * weight;
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
                            height = height + (avg - height) * strength * weight;
                        }
                        break;
                    }
                    
                    case TerrainTool::Flatten: {
                        height = height + (brush.targetHeight - height) * strength * weight;
                        break;
                    }
                    
                    case TerrainTool::Ramp:
                        // Handled separately in RampTerrain
                        break;
                        
                    case TerrainTool::Noise: {
                        // Add noise based on position
                        float noise = (float)rand() / RAND_MAX * 2.0f - 1.0f;
                        height += noise * strength * weight;
                        break;
                    }
                    
                    case TerrainTool::Erosion:
                        // TODO: Thermal/hydraulic erosion
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