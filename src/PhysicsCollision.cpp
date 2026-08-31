#include "PhysicsCollision.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
#include <string>

namespace pcoll {

namespace {

static const float kEps = 1e-6f;
static const int kDefaultMaxVerts = 128;
static const int kTriWarningCount = 30000;

inline Vector3 Sub3(Vector3 a, Vector3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vector3 Scale3(Vector3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline Vector3 Add3(Vector3 a, Vector3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vector3 Cross3(Vector3 a, Vector3 b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline float Dot3(Vector3 a, Vector3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float LenSqr3(Vector3 a) { return Dot3(a, a); }
inline float Len3(Vector3 a) { return std::sqrt(Dot3(a, a)); }
inline float DistSqr3(Vector3 a, Vector3 b) { return LenSqr3(Sub3(a, b)); }
inline Vector3 Norm3(Vector3 a) {
    float l = Len3(a);
    if (l < 1e-12f) return { 0, 0, 0 };
    return Scale3(a, 1.0f / l);
}

void AABBOfVerts(const std::vector<Vector3>& pts, Vector3& mn, Vector3& mx) {
    mn = { FLT_MAX, FLT_MAX, FLT_MAX };
    mx = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (auto& p : pts) {
        mn.x = std::fminf(mn.x, p.x); mn.y = std::fminf(mn.y, p.y); mn.z = std::fminf(mn.z, p.z);
        mx.x = std::fmaxf(mx.x, p.x); mx.y = std::fmaxf(mx.y, p.y); mx.z = std::fmaxf(mx.z, p.z);
    }
}

void DedupeVerts(std::vector<Vector3>& pts) {
    if (pts.size() < 2) {
        for (auto& p : pts) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) { pts.clear(); break; }
        }
        return;
    }
    Vector3 mn, mx;
    AABBOfVerts(pts, mn, mx);
    Vector3 ext = Sub3(mx, mn);
    float maxExt = std::fmaxf(ext.x, std::fmaxf(ext.y, ext.z));
    if (maxExt < 1e-7f) {
        for (auto& p : pts) {
            if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) {
                pts.clear(); pts.push_back(p); return;
            }
        }
        pts.clear(); return;
    }
    float cell = std::fmaxf(maxExt / 65536.0f, 1e-6f);
    struct GridKey { int x, y, z; };
    struct GridLess {
        bool operator()(const GridKey& a, const GridKey& b) const {
            if (a.x != b.x) return a.x < b.x;
            if (a.y != b.y) return a.y < b.y;
            return a.z < b.z;
        }
    };
    std::map<GridKey, int, GridLess> cells;
    std::vector<Vector3> out;
    out.reserve(pts.size());
    for (auto& p : pts) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
        int ix = std::clamp((int)((p.x - mn.x) / cell), 0, 65535);
        int iy = std::clamp((int)((p.y - mn.y) / cell), 0, 65535);
        int iz = std::clamp((int)((p.z - mn.z) / cell), 0, 65535);
        GridKey k{ ix, iy, iz };
        if (cells.insert(std::make_pair(k, (int)out.size())).second) out.push_back(p);
    }
    pts.swap(out);
}

void CollectUniqueVerts(const std::vector<Triangle>& tris, std::vector<Vector3>& out) {
    for (auto& t : tris) { out.push_back(t.a); out.push_back(t.b); out.push_back(t.c); }
    DedupeVerts(out);
}

void Decimate(std::vector<Vector3>& pts, int maxVerts) {
    if ((int)pts.size() <= maxVerts || maxVerts < 2) return;
    Vector3 mn, mx;
    AABBOfVerts(pts, mn, mx);
    Vector3 ext = Sub3(mx, mn);
    float extent = std::fmaxf(ext.x, std::fmaxf(ext.y, ext.z));
    if (extent < 1e-7f) return;
    int res = (int)std::floor(std::cbrt((float)maxVerts));
    if (res < 2) res = 2;
    float cell = extent / (float)res;
    if (cell < 1e-8f) return;
    std::map<std::pair<int, std::pair<int, int>>, Vector3> cells;
    for (auto& p : pts) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
        int ix = std::clamp((int)((p.x - mn.x) / cell), 0, res - 1);
        int iy = std::clamp((int)((p.y - mn.y) / cell), 0, res - 1);
        int iz = std::clamp((int)((p.z - mn.z) / cell), 0, res - 1);
        auto key = std::make_pair(ix, std::make_pair(iy, iz));
        if (cells.find(key) == cells.end()) cells[key] = p;
    }
    pts.clear();
    pts.reserve(cells.size());
    for (auto& kv : cells) pts.push_back(kv.second);
}

struct HFace {
    unsigned int a, b, c;
    Vector3 normal;
    float d;
    std::vector<int> outside;
};

bool AbovePlane(const HFace& f, const Vector3& p) {
    return Dot3(f.normal, p) - f.d > kEps;
}

bool BuildHullImpl(const std::vector<Vector3>& points, int maxVerts, ConvexShape& out) {
    std::vector<Vector3> pts = points;
    DedupeVerts(pts);
    Decimate(pts, maxVerts);
    if (pts.size() < 4) return false;

    std::vector<Vector3> work = pts;
    int extreme[6] = { 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < (int)pts.size(); ++i) {
        if (pts[i].x < pts[extreme[0]].x) extreme[0] = i;
        if (pts[i].x > pts[extreme[1]].x) extreme[1] = i;
        if (pts[i].y < pts[extreme[2]].y) extreme[2] = i;
        if (pts[i].y > pts[extreme[3]].y) extreme[3] = i;
        if (pts[i].z < pts[extreme[4]].z) extreme[4] = i;
        if (pts[i].z > pts[extreme[5]].z) extreme[5] = i;
    }
    int i0 = -1, i1 = -1;
    float best = -1.0f;
    for (int a = 0; a < 6; ++a) {
        for (int b = a + 1; b < 6; ++b) {
            float d2 = DistSqr3(pts[extreme[a]], pts[extreme[b]]);
            if (d2 > best) { best = d2; i0 = extreme[a]; i1 = extreme[b]; }
        }
    }
    Vector3 d01 = Sub3(pts[i1], pts[i0]);
    float len01sq = LenSqr3(d01);
    if (len01sq < 1e-16f) return false;
    int i2 = -1; best = -1.0f;
    for (int i = 0; i < (int)pts.size(); ++i) {
        if (i == i0 || i == i1) continue;
        float d2 = DistSqr3(pts[i], pts[i0]) - Dot3(Sub3(pts[i], pts[i0]), d01) * Dot3(Sub3(pts[i], pts[i0]), d01) / len01sq;
        if (d2 > best) { best = d2; i2 = i; }
    }
    if (i2 < 0) return false;
    Vector3 n01 = Norm3(Cross3(d01, Sub3(pts[i2], pts[i0])));
    int i3 = -1; best = -1.0f;
    for (int i = 0; i < (int)pts.size(); ++i) {
        if (i == i0 || i == i1 || i == i2) continue;
        float d = std::fabsf(Dot3(n01, Sub3(pts[i], pts[i0])));
        if (d > best) { best = d; i3 = i; }
    }
    if (i3 < 0 || best < 1e-9f) return false;

    out.verts.clear();
    out.verts.push_back(pts[i0]);
    out.verts.push_back(pts[i1]);
    out.verts.push_back(pts[i2]);
    out.verts.push_back(pts[i3]);
    Vector3 interior = Scale3(Add3(Add3(out.verts[0], out.verts[1]), Add3(out.verts[2], out.verts[3])), 0.25f);

    std::vector<HFace> faces;
    unsigned int seed[4][3] = { { 0, 1, 2 }, { 0, 3, 1 }, { 1, 3, 2 }, { 2, 3, 0 } };
    for (auto& tri : seed) {
        HFace f;
        f.a = tri[0]; f.b = tri[1]; f.c = tri[2];
        Vector3 n = Norm3(Cross3(Sub3(out.verts[f.b], out.verts[f.a]), Sub3(out.verts[f.c], out.verts[f.a])));
        if (Dot3(n, Sub3(out.verts[f.a], interior)) < 0.0f) {
            std::swap(f.b, f.c);
            n = Norm3(Cross3(Sub3(out.verts[f.b], out.verts[f.a]), Sub3(out.verts[f.c], out.verts[f.a])));
        }
        f.normal = n;
        f.d = Dot3(n, out.verts[f.a]);
        faces.push_back(f);
    }

    std::vector<bool> used(pts.size(), false);
    used[i0] = used[i1] = used[i2] = used[i3] = true;
    for (int i = 0; i < (int)pts.size(); ++i) {
        if (used[i]) continue;
        int bestF = -1; float bestD = kEps;
        for (int fi = 0; fi < (int)faces.size(); ++fi) {
            float d = Dot3(faces[fi].normal, pts[i]) - faces[fi].d;
            if (d > bestD) { bestD = d; bestF = fi; }
        }
        if (bestF >= 0) faces[bestF].outside.push_back(i);
    }

    for (int guard = 0; guard < 1000000; ++guard) {
        int bestF = -1, bestP = -1; float bestD = kEps;
        for (int fi = 0; fi < (int)faces.size(); ++fi) {
            for (int pi : faces[fi].outside) {
                float d = Dot3(faces[fi].normal, pts[pi]) - faces[fi].d;
                if (d > bestD) { bestD = d; bestF = fi; bestP = pi; }
            }
        }
        if (bestF < 0) break;
        Vector3 p = pts[bestP];

        std::vector<bool> vis(faces.size(), false);
        std::vector<int> visible;
        for (int fi = 0; fi < (int)faces.size(); ++fi) {
            if (AbovePlane(faces[fi], p)) { vis[fi] = true; visible.push_back(fi); }
        }
        if (visible.empty()) break;

        std::map<std::pair<unsigned int, unsigned int>, int> edgeCount;
        for (int fi : visible) {
            unsigned int v[3] = { faces[fi].a, faces[fi].b, faces[fi].c };
            for (int e = 0; e < 3; ++e) edgeCount[std::make_pair(v[e], v[(e + 1) % 3])]++;
        }
        std::vector<std::pair<unsigned int, unsigned int>> horizon;
        for (auto& kv : edgeCount) {
            if (kv.second == 1) horizon.push_back(kv.first);
        }

        std::vector<int> freed;
        for (int fi : visible) {
            for (int pi : faces[fi].outside) {
                if (pi != bestP) freed.push_back(pi);
            }
        }

        std::vector<HFace> kept;
        kept.reserve(faces.size());
        for (int fi = 0; fi < (int)faces.size(); ++fi) {
            if (!vis[fi]) kept.push_back(faces[fi]);
        }
        faces.swap(kept);

        int newVert = (int)out.verts.size();
        out.verts.push_back(p);

        std::vector<int> newFaceIdx;
        newFaceIdx.reserve(horizon.size());
        for (auto& h : horizon) {
            HFace f;
            f.a = h.first; f.b = h.second; f.c = (unsigned int)newVert;
            Vector3 n = Norm3(Cross3(Sub3(out.verts[f.b], out.verts[f.a]), Sub3(out.verts[f.c], out.verts[f.a])));
            if (Dot3(n, Sub3(out.verts[f.a], interior)) < 0.0f) {
                std::swap(f.b, f.c);
                n = Norm3(Cross3(Sub3(out.verts[f.b], out.verts[f.a]), Sub3(out.verts[f.c], out.verts[f.a])));
            }
            f.normal = n;
            f.d = Dot3(n, out.verts[f.a]);
            newFaceIdx.push_back((int)faces.size());
            faces.push_back(f);
        }

        for (int pi : freed) {
            int bestN = -1; float bestNd = kEps;
            for (int ni : newFaceIdx) {
                float d = Dot3(faces[ni].normal, pts[pi]) - faces[ni].d;
                if (d > bestNd) { bestNd = d; bestN = ni; }
            }
            if (bestN >= 0) faces[bestN].outside.push_back(pi);
        }
    }

    out.faces.clear();
    out.faces.reserve(faces.size());
    for (auto& f : faces) {
        HullFace hf;
        hf.verts.push_back(f.a);
        hf.verts.push_back(f.b);
        hf.verts.push_back(f.c);
        hf.normal = f.normal;
        hf.d = f.d;
        out.faces.push_back(hf);
    }
    return out.verts.size() >= 4 && out.faces.size() >= 4;
}

void KMeans(const std::vector<Vector3>& points, int k, int maxIters, std::vector<int>& labels) {
    int n = (int)points.size();
    labels.assign(n, 0);
    if (n == 0 || k <= 0) return;
    if (k > n) k = n;
    std::vector<Vector3> means(k);
    Vector3 sum = { 0, 0, 0 };
    for (auto& p : points) sum = Add3(sum, p);
    means[0] = Scale3(sum, 1.0f / (float)n);
    for (int i = 1; i < k; ++i) {
        float best = -1.0f; int bestJ = 0;
        for (int j = 0; j < n; ++j) {
            float md = FLT_MAX;
            for (int t = 0; t < i; ++t) { float d = DistSqr3(points[j], means[t]); if (d < md) md = d; }
            if (md > best) { best = md; bestJ = j; }
        }
        means[i] = points[bestJ];
    }
    for (int iter = 0; iter < maxIters; ++iter) {
        bool changed = false;
        std::vector<Vector3> acc(k, { 0, 0, 0 });
        std::vector<int> cnt(k, 0);
        for (int j = 0; j < n; ++j) {
            int best = 0; float bd = FLT_MAX;
            for (int t = 0; t < k; ++t) { float d = DistSqr3(points[j], means[t]); if (d < bd) { bd = d; best = t; } }
            if (labels[j] != best) { labels[j] = best; changed = true; }
            acc[best] = Add3(acc[best], points[j]);
            cnt[best]++;
        }
        for (int t = 0; t < k; ++t) {
            if (cnt[t] > 0) means[t] = Scale3(acc[t], 1.0f / (float)cnt[t]);
        }
        if (!changed) break;
    }
}

void AddTriVerts(const Triangle& t, std::vector<Vector3>& out) {
    out.push_back(t.a); out.push_back(t.b); out.push_back(t.c);
}

AABB ComputeAABBFromVerts(const std::vector<Vector3>& verts) {
    AABB r;
    r.min = { FLT_MAX, FLT_MAX, FLT_MAX };
    r.max = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (auto& v : verts) {
        r.min.x = std::fminf(r.min.x, v.x); r.min.y = std::fminf(r.min.y, v.y); r.min.z = std::fminf(r.min.z, v.z);
        r.max.x = std::fmaxf(r.max.x, v.x); r.max.y = std::fmaxf(r.max.y, v.y); r.max.z = std::fmaxf(r.max.z, v.z);
    }
    return r;
}

} // namespace

const char* CollisionAccuracyName(CollisionAccuracy accuracy) {
    switch (accuracy) {
        case CollisionAccuracy::Box: return "Box";
        case CollisionAccuracy::Hull: return "Hull";
        case CollisionAccuracy::Default: return "Default";
        case CollisionAccuracy::Precise: return "Precise";
    }
    return "Default";
}

bool ParseCollisionAccuracy(const std::string& text, CollisionAccuracy& out) {
    std::string s = text;
    for (auto& ch : s) { if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + ('a' - 'A')); }
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a != std::string::npos) {
        size_t b = s.find_last_not_of(" \t\r\n");
        s = s.substr(a, b - a + 1);
    }
    static const char* kPrefix = "enum.collisionaccuracy.";
    size_t plen = std::string(kPrefix).size();
    if (s.size() >= plen && s.compare(0, plen, kPrefix) == 0) s = s.substr(plen);
    if (s == "box") out = CollisionAccuracy::Box;
    else if (s == "hull") out = CollisionAccuracy::Hull;
    else if (s == "default") out = CollisionAccuracy::Default;
    else if (s == "precise") out = CollisionAccuracy::Precise;
    else return false;
    return true;
}

bool BuildHull(const std::vector<Vector3>& points, int maxVerts, ConvexShape& out) {
    return BuildHullImpl(points, maxVerts, out);
}

Collider BuildCollider(CollisionAccuracy accuracy, const std::vector<Triangle>& unitTris) {
    Collider c;
    c.accuracy = accuracy;
    c.unitHalfExtents = { 0.5f, 0.5f, 0.5f };
    c.unitAABB = { { FLT_MAX, FLT_MAX, FLT_MAX }, { -FLT_MAX, -FLT_MAX, -FLT_MAX } };

    auto unionAABB = [&](const AABB& r) {
        c.unitAABB.min = { std::fminf(c.unitAABB.min.x, r.min.x), std::fminf(c.unitAABB.min.y, r.min.y), std::fminf(c.unitAABB.min.z, r.min.z) };
        c.unitAABB.max = { std::fmaxf(c.unitAABB.max.x, r.max.x), std::fmaxf(c.unitAABB.max.y, r.max.y), std::fmaxf(c.unitAABB.max.z, r.max.z) };
    };

    switch (accuracy) {
        case CollisionAccuracy::Box:
            c.isBox = true;
            c.unitAABB = { { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f } };
            return c;

        case CollisionAccuracy::Precise:
            c.tris = unitTris;
            if (unitTris.empty()) {
                c.isBox = true;
                c.unitAABB = { { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f } };
            } else {
                for (auto& t : unitTris) {
                    AABB ta{ { std::fminf(t.a.x, std::fminf(t.b.x, t.c.x)), std::fminf(t.a.y, std::fminf(t.b.y, t.c.y)), std::fminf(t.a.z, std::fminf(t.b.z, t.c.z)) },
                             { std::fmaxf(t.a.x, std::fmaxf(t.b.x, t.c.x)), std::fmaxf(t.a.y, std::fmaxf(t.b.y, t.c.y)), std::fmaxf(t.a.z, std::fmaxf(t.b.z, t.c.z)) } };
                    unionAABB(ta);
                }
            }
            return c;

        case CollisionAccuracy::Hull: {
            std::vector<Vector3> pts;
            CollectUniqueVerts(unitTris, pts);
            if (pts.size() >= 4) {
                ConvexShape h;
                if (BuildHullImpl(pts, kDefaultMaxVerts, h)) {
                    c.hulls.push_back(h);
                    unionAABB(ComputeAABBFromVerts(h.verts));
                    return c;
                }
            }
            c.isBox = true;
            c.unitAABB = { { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f } };
            return c;
        }

        case CollisionAccuracy::Default:
        default: {
            if (unitTris.empty()) {
                c.isBox = true;
                c.unitAABB = { { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f } };
                return c;
            }
            if ((int)unitTris.size() > kTriWarningCount) c.warned = true;
            int k = 1 + (int)unitTris.size() / 4000;
            if (k < 2) k = 2;
            if (k > 6) k = 6;
            std::vector<Vector3> centroids;
            centroids.reserve(unitTris.size());
            for (auto& t : unitTris) centroids.push_back(Scale3(Add3(t.a, Add3(t.b, t.c)), 1.0f / 3.0f));
            std::vector<int> labels;
            KMeans(centroids, k, 16, labels);
            for (int i = 0; i < k; ++i) {
                std::vector<Vector3> pts;
                for (size_t j = 0; j < unitTris.size(); ++j) {
                    if (labels[j] == i) AddTriVerts(unitTris[j], pts);
                }
                DedupeVerts(pts);
                if (pts.size() < 4) continue;
                ConvexShape h;
                if (BuildHullImpl(pts, kDefaultMaxVerts, h)) {
                    c.hulls.push_back(h);
                    unionAABB(ComputeAABBFromVerts(h.verts));
                    continue;
                }
                Vector3 mn, mx;
                AABBOfVerts(pts, mn, mx);
                AABB box{ mn, mx };
                c.hulls.push_back({}); // empty hull
                unionAABB(box);
            }
            if (c.hulls.empty()) c.isBox = true;
            if (c.isBox) c.unitAABB = { { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f } };
            return c;
        }
    }
}

} // namespace pcoll
