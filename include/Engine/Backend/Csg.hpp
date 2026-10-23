#pragma once
// Solid-modelling booleans (union / subtract / intersect) on closed polygon meshes, using a BSP tree per solid.
// Every polygon carries a colour, so a result keeps the colour of the part each face came from.
// Pure geometry: no raylib, so it can be tested on its own.
#include <cstdint>
#include <vector>

namespace csg {

struct V3 {
    double x = 0, y = 0, z = 0;
};

struct Vertex {
    V3 pos;
    V3 normal;   // shading normal, interpolated when a polygon is split
};

// A convex polygon, counter-clockwise seen from outside the solid.
struct Polygon {
    std::vector<Vertex> verts;
    uint32_t color = 0xFFFFFFFFu;   // 0xAABBGGRR (r in the low byte)
};

using Polys = std::vector<Polygon>;

// Inputs must be closed, consistently wound solids. Results are closed too (up to the epsilon).
Polys Union(const Polys& a, const Polys& b);
Polys Subtract(const Polys& a, const Polys& b);
Polys Intersect(const Polys& a, const Polys& b);

// A whole union in one pass, for convex parts only (every engine primitive is convex). The solid is
//   (inside any Add part, or every Intersect part when there are no Add parts) and outside every Negate part
//   and inside every Intersect part.
// Each part's faces are cut only by the parts they overlap, so this splits far less than folding the parts in
// one boolean at a time, and the result does not depend on the order of the parts. Where two parts have a face
// in the same place, the earlier part's face is kept.
enum class Mode { Add, Negate, Intersect };
struct ConvexPart {
    Polys polys;
    Mode mode = Mode::Add;
};
Polys Evaluate(const std::vector<ConvexPart>& parts);

// Flat triangle list for upload: 3 vertices per triangle.
struct TriSoup {
    std::vector<float> pos;        // xyz per vertex
    std::vector<float> nrm;        // xyz per vertex
    std::vector<uint32_t> color;   // per vertex
    size_t TriangleCount() const { return color.size() / 3; }
};
// Fan-triangulates the polygons; slivers with (almost) no area are dropped.
TriSoup Triangulate(const Polys& polys);

struct Bounds {
    V3 min, max;
    bool valid = false;
};
Bounds BoundsOf(const Polys& polys);

// Signed volume of a closed polygon mesh (positive when wound outward). Used by tests and sanity checks.
double Volume(const Polys& polys);

}   // namespace csg
