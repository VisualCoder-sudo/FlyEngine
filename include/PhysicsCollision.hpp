#pragma once

#include "raylib.h"
#include "raymath.h"
#include <string>
#include <vector>

namespace pcoll {

enum class CollisionAccuracy {
    Box = 0,
    Hull = 1,
    Default = 2,
    Precise = 3,
};

const char* CollisionAccuracyName(CollisionAccuracy accuracy);
bool ParseCollisionAccuracy(const std::string& text, CollisionAccuracy& out);

struct Triangle {
    Vector3 a{};
    Vector3 b{};
    Vector3 c{};
};

struct HullFace {
    std::vector<unsigned int> verts;
    Vector3 normal = { 0.0f, 0.0f, 0.0f };
    float d = 0.0f;
};

struct ConvexShape {
    std::vector<Vector3> verts;
    std::vector<HullFace> faces;
};

struct AABB {
    Vector3 min = { 0.0f, 0.0f, 0.0f };
    Vector3 max = { 0.0f, 0.0f, 0.0f };
};

struct Collider {
    CollisionAccuracy accuracy = CollisionAccuracy::Box;
    std::vector<ConvexShape> hulls;
    std::vector<Triangle> tris;
    Vector3 unitHalfExtents = { 0.5f, 0.5f, 0.5f };
    bool isBox = false;
    bool warned = false;
    AABB unitAABB{};
};

Collider BuildCollider(CollisionAccuracy accuracy, const std::vector<Triangle>& unitTris);
bool BuildHull(const std::vector<Vector3>& points, int maxVerts, ConvexShape& out);

} // namespace pcoll
