#pragma once

#include "Terrain.hpp"
#include "raylib.h"

namespace terrain {

// ============================================================================
// Physics Interface
// ============================================================================

struct PhysicsSimInterface {
    void* world = nullptr;
    void* (*createStaticBody)(void* world, const void* shape) = nullptr;
    void (*destroyBody)(void* world, void* body) = nullptr;
    void (*setBodyTransform)(void* body, const float* pos, const float* rot) = nullptr;
};

// Set the physics interface (called by engine during initialization)
void SetPhysicsInterface(PhysicsSimInterface* iface);

// ============================================================================
// Physics Creation
// ============================================================================

// Create heightfield collision shape (fast, 2.5D)
void CreatePhysicsHeightfield(Terrain& terrain);

// Create triangle mesh collision shape (precise, matches visual)
void CreatePhysicsTriangleMesh(Terrain& terrain);

// Destroy physics body
void DestroyPhysics(Terrain& terrain);

// Update physics body transform to match terrain entity
void UpdatePhysicsTransform(Terrain& terrain);

// ============================================================================
// Raycasting
// ============================================================================

// Raycast against terrain physics (uses physics engine if available)
bool RaycastHeightfield(const Terrain& terrain, const Ray& ray, float* outDistance, Vector3* outPoint, Vector3* outNormal);

} // namespace terrain