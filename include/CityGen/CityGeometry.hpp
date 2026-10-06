#pragma once

#include "raylib.h"
#include <vector>

// Flat, CPU-side polygon/planar-graph helpers for the procedural city.
// All polygons live in the world XZ plane (x, z) and are CCW when their
// signed area is positive.
namespace citygeom {

// Optional organic warp of a world-space point (used when widening the base
// grid). `strength` is the absolute displacement scale (world units).
Vector2 OrganicWarp(const Vector2& p, float strength, float scale,
                    int octaves, int seed);

// Signed area of a polygon in the (x, z) plane; positive = CCW.
float PolygonArea(const std::vector<Vector2>& poly);
void EnsureCCW(std::vector<Vector2>& poly);

bool PointInPolygon(const Vector2& p, const std::vector<Vector2>& poly);

// Extract the minimal faces of a planar graph as closed node-index loops.
// `positions[i]` is the coordinate of node i; each edge is (a, b). The outer
// boundary face is dropped (unless it is the only loop). Loops are oriented
// CCW in (x, z).
std::vector<std::vector<int>> ExtractFaces(
    const std::vector<Vector2>& positions,
    const std::vector<std::pair<int, int>>& edges);

// Shrink `poly` toward its interior by `d` using miter offsets (new polygon
// sides stay parallel to the originals). Returns false when the result
// degenerates/self-intersects; `out` is left valid-but-empty then.
bool InsetPolygon(const std::vector<Vector2>& poly, float d,
                  std::vector<Vector2>& out);

// Ear-clipping triangulation of a simple (possibly concave) polygon.
// Appends triangle triples (indices into `poly`) to `tris`.
void TriangulateSimple(const std::vector<Vector2>& poly, std::vector<int>& tris);

// True when no two non-adjacent edges of `poly` properly cross (tests).
bool IsSimplePolygonForTest(const std::vector<Vector2>& poly);

} // namespace citygeom