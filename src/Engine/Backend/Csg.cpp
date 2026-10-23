#include "Engine/Backend/Csg.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace csg {
namespace {

constexpr double kEps = 1e-5;

V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(V3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
double Length(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Normalize(V3 a) {
    const double l = Length(a);
    return l > 1e-20 ? a * (1.0 / l) : V3{};
}
V3 Lerp(V3 a, V3 b, double t) { return a + (b - a) * t; }

Vertex Interpolate(const Vertex& a, const Vertex& b, double t) {
    Vertex v;
    v.pos = Lerp(a.pos, b.pos, t);
    v.normal = Normalize(Lerp(a.normal, b.normal, t));
    return v;
}

struct Plane {
    V3 n;
    double w = 0;
    bool Valid() const { return Length(n) > 0.5; }
    void Flip() { n = n * -1.0; w = -w; }
};

// Newell's method: stable for long thin or nearly degenerate polygons.
Plane PlaneOf(const std::vector<Vertex>& v) {
    Plane p;
    V3 n;
    for (size_t i = 0; i < v.size(); ++i) {
        const V3& a = v[i].pos;
        const V3& b = v[(i + 1) % v.size()].pos;
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    p.n = Normalize(n);
    V3 c;
    for (const Vertex& x : v) c = c + x.pos;
    p.w = v.empty() ? 0.0 : Dot(p.n, c * (1.0 / (double)v.size()));
    return p;
}

// A polygon with its plane cached.
struct Poly {
    std::vector<Vertex> v;
    uint32_t color = 0xFFFFFFFFu;
    Plane plane;

    void Flip() {
        std::reverse(v.begin(), v.end());
        for (Vertex& x : v) x.normal = x.normal * -1.0;
        plane.Flip();
    }
};
using PolyList = std::vector<Poly>;

PolyList ToInternal(const Polys& in) {
    PolyList out;
    out.reserve(in.size());
    for (const Polygon& p : in) {
        if (p.verts.size() < 3) continue;
        Poly q;
        q.v = p.verts;
        q.color = p.color;
        q.plane = PlaneOf(q.v);
        if (!q.plane.Valid()) continue;
        out.push_back(std::move(q));
    }
    return out;
}

Polys ToPublic(const PolyList& in) {
    Polys out;
    out.reserve(in.size());
    for (const Poly& p : in) {
        Polygon q;
        q.verts = p.v;
        q.color = p.color;
        out.push_back(std::move(q));
    }
    return out;
}

enum : int { kCoplanar = 0, kFront = 1, kBack = 2, kSpanning = 3 };

void SplitPolygon(const Plane& plane, const Poly& poly, PolyList& coplanarFront, PolyList& coplanarBack, PolyList& front, PolyList& back) {
    int polygonType = 0;
    std::vector<int> types;
    types.reserve(poly.v.size());
    for (const Vertex& x : poly.v) {
        const double t = Dot(plane.n, x.pos) - plane.w;
        const int type = t < -kEps ? kBack : (t > kEps ? kFront : kCoplanar);
        polygonType |= type;
        types.push_back(type);
    }
    switch (polygonType) {
        case kCoplanar:
            (Dot(plane.n, poly.plane.n) > 0 ? coplanarFront : coplanarBack).push_back(poly);
            break;
        case kFront: front.push_back(poly); break;
        case kBack: back.push_back(poly); break;
        default: {
            std::vector<Vertex> f, b;
            const size_t n = poly.v.size();
            for (size_t i = 0; i < n; ++i) {
                const size_t j = (i + 1) % n;
                const int ti = types[i], tj = types[j];
                const Vertex& vi = poly.v[i];
                const Vertex& vj = poly.v[j];
                if (ti != kBack) f.push_back(vi);
                if (ti != kFront) b.push_back(vi);
                if ((ti | tj) == kSpanning) {
                    const double t = (plane.w - Dot(plane.n, vi.pos)) / Dot(plane.n, vj.pos - vi.pos);
                    const Vertex v = Interpolate(vi, vj, t);
                    f.push_back(v);
                    b.push_back(v);
                }
            }
            auto emit = [&](std::vector<Vertex>& verts, PolyList& list) {
                if (verts.size() < 3) return;
                Poly q;
                q.v = std::move(verts);
                q.color = poly.color;
                q.plane = poly.plane;   // pieces of a planar polygon stay in its plane
                list.push_back(std::move(q));
            };
            emit(f, front);
            emit(b, back);
        }
    }
}

// BSP tree of a solid: the polygons on a node's plane, and what lies in front of and behind it.
class Node {
public:
    Node() = default;
    explicit Node(PolyList polys) { Build(std::move(polys)); }

    void Invert() {
        for (Poly& p : polygons) p.Flip();
        plane.Flip();
        std::swap(front, back);
        if (front) front->Invert();
        if (back) back->Invert();
    }

    // The parts of `list` outside the solid this tree describes.
    PolyList ClipPolygons(const PolyList& list) const {
        if (!hasPlane) return list;
        PolyList f, b;
        for (const Poly& p : list) SplitPolygon(plane, p, f, b, f, b);   // coplanar polygons go with their facing side
        if (front) f = front->ClipPolygons(f);
        if (back) b = back->ClipPolygons(b); else b.clear();
        f.insert(f.end(), std::make_move_iterator(b.begin()), std::make_move_iterator(b.end()));
        return f;
    }

    void ClipTo(const Node& other) {
        polygons = other.ClipPolygons(polygons);
        if (front) front->ClipTo(other);
        if (back) back->ClipTo(other);
    }

    PolyList AllPolygons() const {
        PolyList out = polygons;
        if (front) { PolyList f = front->AllPolygons(); out.insert(out.end(), f.begin(), f.end()); }
        if (back) { PolyList b = back->AllPolygons(); out.insert(out.end(), b.begin(), b.end()); }
        return out;
    }

    void Build(PolyList list) {
        if (list.empty()) return;
        if (!hasPlane) {
            plane = list[0].plane;
            hasPlane = true;
        }
        PolyList f, b;
        for (const Poly& p : list) SplitPolygon(plane, p, polygons, polygons, f, b);
        if (!f.empty()) {
            if (!front) front = std::make_unique<Node>();
            front->Build(std::move(f));
        }
        if (!b.empty()) {
            if (!back) back = std::make_unique<Node>();
            back->Build(std::move(b));
        }
    }

private:
    Plane plane;
    bool hasPlane = false;
    std::unique_ptr<Node> front, back;
    PolyList polygons;
};

}   // namespace

Polys Union(const Polys& pa, const Polys& pb) {
    if (pa.empty()) return pb;
    if (pb.empty()) return pa;
    Node a(ToInternal(pa)), b(ToInternal(pb));
    a.ClipTo(b);
    b.ClipTo(a);
    b.Invert();
    b.ClipTo(a);
    b.Invert();
    a.Build(b.AllPolygons());
    return ToPublic(a.AllPolygons());
}

Polys Subtract(const Polys& pa, const Polys& pb) {
    if (pa.empty() || pb.empty()) return pa;
    Node a(ToInternal(pa)), b(ToInternal(pb));
    a.Invert();
    a.ClipTo(b);
    b.ClipTo(a);
    b.Invert();
    b.ClipTo(a);
    b.Invert();
    a.Build(b.AllPolygons());
    a.Invert();
    return ToPublic(a.AllPolygons());
}

Polys Intersect(const Polys& pa, const Polys& pb) {
    if (pa.empty() || pb.empty()) return {};
    Node a(ToInternal(pa)), b(ToInternal(pb));
    a.Invert();
    b.ClipTo(a);
    b.Invert();
    a.ClipTo(b);
    b.ClipTo(a);
    a.Build(b.AllPolygons());
    a.Invert();
    return ToPublic(a.AllPolygons());
}

TriSoup Triangulate(const Polys& polys) {
    TriSoup out;
    for (const Polygon& p : polys) {
        const size_t n = p.verts.size();
        if (n < 3) continue;
        const Plane plane = PlaneOf(p.verts);
        for (size_t i = 1; i + 1 < n; ++i) {
            const Vertex* tri[3] = { &p.verts[0], &p.verts[i], &p.verts[i + 1] };
            const V3 c = Cross(tri[1]->pos - tri[0]->pos, tri[2]->pos - tri[0]->pos);
            if (Length(c) < 1e-9) continue;   // sliver left by a split
            for (const Vertex* v : tri) {
                V3 nrm = Length(v->normal) > 0.5 ? v->normal : plane.n;
                out.pos.push_back((float)v->pos.x); out.pos.push_back((float)v->pos.y); out.pos.push_back((float)v->pos.z);
                out.nrm.push_back((float)nrm.x); out.nrm.push_back((float)nrm.y); out.nrm.push_back((float)nrm.z);
                out.color.push_back(p.color);
            }
        }
    }
    return out;
}

Bounds BoundsOf(const Polys& polys) {
    Bounds b;
    for (const Polygon& p : polys) {
        for (const Vertex& v : p.verts) {
            if (!b.valid) { b.min = b.max = v.pos; b.valid = true; continue; }
            b.min = { std::min(b.min.x, v.pos.x), std::min(b.min.y, v.pos.y), std::min(b.min.z, v.pos.z) };
            b.max = { std::max(b.max.x, v.pos.x), std::max(b.max.y, v.pos.y), std::max(b.max.z, v.pos.z) };
        }
    }
    return b;
}

double Volume(const Polys& polys) {
    double sum = 0.0;
    for (const Polygon& p : polys) {
        for (size_t i = 1; i + 1 < p.verts.size(); ++i)
            sum += Dot(p.verts[0].pos, Cross(p.verts[i].pos, p.verts[i + 1].pos)) / 6.0;
    }
    return sum;
}

}   // namespace csg
