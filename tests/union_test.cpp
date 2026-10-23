// Unions (solid modelling): the CSG core on its own (union / subtract / intersect volumes, colours kept per face),
// and a ScatteredObject union: it keeps its parts' colours, honours the global colour, survives a scene save/load
// and a clone, and Separate hands the parts back where they were. Opens a window (it needs a GPU for the mesh),
// so ctest needs a display (label needs_display): build/tests/union_test
#include "raylib.h"
#include "Engine.hpp"
#include "Graphics/Graphics.hpp"
#include "Engine/Backend/Csg.hpp"
#include "Engine/Backend/ScenePersistence.hpp"
#include "Engine/Backend/PhysicsSimulation.hpp"
#include "../include/Terrain/BasicTerrain.hpp"
#include "Terrain/Water/WaterBody.hpp"
#define private public
#include "../include/Terrain/Terrain.hpp"
#undef private
#include "Engine/Frontend/ui.hpp"

#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
static bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// An axis-aligned box as outward-wound quads.
static csg::Polys Box(csg::V3 lo, csg::V3 hi, uint32_t color) {
    const csg::V3 p[8] = { { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, lo.z },
                           { lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } };
    const int f[6][4] = { { 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 4, 7, 3 }, { 1, 2, 6, 5 }, { 0, 1, 5, 4 }, { 3, 7, 6, 2 } };
    const csg::V3 n[6] = { { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 } };
    csg::Polys out;
    for (int i = 0; i < 6; ++i) {
        csg::Polygon poly;
        poly.color = color;
        for (int k = 0; k < 4; ++k) poly.verts.push_back({ p[f[i][k]], n[i] });
        out.push_back(poly);
    }
    return out;
}

static void TestCsg() {
    const uint32_t red = 0xFF0000FFu, blue = 0xFFFF0000u;
    const csg::Polys a = Box({ 0, 0, 0 }, { 2, 2, 2 }, red);
    const csg::Polys b = Box({ 1, 1, 1 }, { 3, 3, 3 }, blue);
    CHECK(Near(csg::Volume(a), 8.0, 1e-6));

    // Overlap is 1x1x1.
    const csg::Polys u = csg::Union(a, b);
    CHECK(Near(csg::Volume(u), 15.0, 1e-3));
    const csg::Polys d = csg::Subtract(a, b);
    CHECK(Near(csg::Volume(d), 7.0, 1e-3));
    const csg::Polys i = csg::Intersect(a, b);
    CHECK(Near(csg::Volume(i), 1.0, 1e-3));

    // Faces keep the colour of the part they came from; no polygon has a colour that is neither.
    bool sawRed = false, sawBlue = false, other = false;
    for (const csg::Polygon& p : u) {
        if (p.color == red) sawRed = true;
        else if (p.color == blue) sawBlue = true;
        else other = true;
    }
    CHECK(sawRed && sawBlue && !other);

    // A box through the middle leaves a hole: the cut walls are the cutter's faces, facing into the hole.
    const csg::Polys post = Box({ 0.5, -1, 0.5 }, { 1.5, 3, 1.5 }, blue);
    const csg::Polys holed = csg::Subtract(a, post);
    CHECK(Near(csg::Volume(holed), 8.0 - 2.0, 1e-3));
    bool wallBlue = false;
    for (const csg::Polygon& p : holed) wallBlue = wallBlue || p.color == blue;
    CHECK(wallBlue);

    // Disjoint solids: a union keeps both, an intersection is empty.
    const csg::Polys far = Box({ 10, 0, 0 }, { 11, 1, 1 }, blue);
    CHECK(Near(csg::Volume(csg::Union(a, far)), 9.0, 1e-3));
    CHECK(csg::Intersect(a, far).empty() || Near(csg::Volume(csg::Intersect(a, far)), 0.0, 1e-6));

    // The one-pass evaluator agrees with the booleans, and does not double or leave faces where parts touch.
    auto area = [](const csg::Polys& ps) {
        double sum = 0.0;
        for (const csg::Polygon& p : ps)
            for (size_t k = 1; k + 1 < p.verts.size(); ++k) {
                const csg::V3 a = p.verts[0].pos, b = p.verts[k].pos, c = p.verts[k + 1].pos;
                const csg::V3 e1 = { b.x - a.x, b.y - a.y, b.z - a.z }, e2 = { c.x - a.x, c.y - a.y, c.z - a.z };
                const csg::V3 x = { e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
                sum += 0.5 * std::sqrt(x.x * x.x + x.y * x.y + x.z * x.z);
            }
        return sum;
    };
    using M = csg::Mode;
    CHECK(Near(csg::Volume(csg::Evaluate({ { a, M::Add }, { b, M::Add } })), 15.0, 1e-6));
    CHECK(Near(csg::Volume(csg::Evaluate({ { a, M::Add }, { b, M::Negate } })), 7.0, 1e-6));
    CHECK(Near(csg::Volume(csg::Evaluate({ { a, M::Intersect }, { b, M::Intersect } })), 1.0, 1e-6));
    CHECK(Near(csg::Volume(csg::Evaluate({ { a, M::Add }, { post, M::Negate } })), 6.0, 1e-6));
    const csg::Polys side = Box({ 2, 0, 0 }, { 4, 2, 2 }, blue);   // touches `a` face to face
    const csg::Polys joined = csg::Evaluate({ { a, M::Add }, { side, M::Add } });
    CHECK(Near(csg::Volume(joined), 16.0, 1e-6));
    CHECK(Near(area(joined), 40.0, 1e-6));   // a 4 x 2 x 2 box: no wall left between them
    const csg::Polys twin = csg::Evaluate({ { a, M::Add }, { Box({ 0, 0, 0 }, { 2, 2, 2 }, blue), M::Add } });
    CHECK(Near(area(twin), 24.0, 1e-6));      // the same box twice: each face once
    bool twinRed = true;
    for (const csg::Polygon& p : twin) twinRed = twinRed && p.color == red;   // the earlier part's faces win
    CHECK(twinRed);
    const csg::Polys flush = csg::Evaluate({ { a, M::Add }, { Box({ 0, 2, 0 }, { 2, 4, 2 }, blue), M::Negate } });
    CHECK(Near(area(flush), 24.0, 1e-6) && Near(csg::Volume(flush), 8.0, 1e-6));   // a cutter resting on top changes nothing

    const csg::TriSoup soup = csg::Triangulate(u);
    CHECK(soup.TriangleCount() > 12);
    CHECK(soup.pos.size() == soup.TriangleCount() * 9);
}

static ScatteredObject* Add(Engine& engine, std::vector<ScatteredObject*>& objs, Vector3 pos, Vector3 size, Color color, ShapeType shape = ShapeType::Cube) {
    auto o = std::make_unique<ScatteredObject>(pos, size, color, shape);
    ScatteredObject* raw = o.get();
    objs.push_back(raw);
    engine.AddEntity(std::move(o));
    return raw;
}

static std::unique_ptr<ScatteredObject> Part(Vector3 pos, Vector3 size, Color color, ShapeType shape = ShapeType::Cube) {
    return std::make_unique<ScatteredObject>(pos, size, color, shape);
}

int main() {
    TestCsg();

    setenv("FLY_NO_SHAPE_BATCH", "1", 1);
    Engine engine(640, 360, "union_test", 60);
    engine.SetPlayerBuild(true);
    std::vector<ScatteredObject*> objs;
    std::vector<std::unique_ptr<ModelGroup>> models;

    // --- a union of two overlapping cubes ----------------------------------------------------------------------
    std::vector<std::unique_ptr<ScatteredObject>> parts;
    parts.push_back(Part({ 10, 5, 0 }, { 2, 2, 2 }, { 255, 0, 0, 255 }));
    parts.push_back(Part({ 11, 5, 0 }, { 2, 2, 2 }, { 0, 0, 255, 255 }));
    std::unique_ptr<ScatteredObject> u = ScatteredObject::MakeUnion(std::move(parts), { UnionMode::Add, UnionMode::Add });
    CHECK(u != nullptr);
    if (!u) { std::printf("union_test: FAILED early\n"); return 1; }
    CHECK(u->IsUnion());
    CHECK(u->HasModel());
    CHECK(u->UnionParts().size() == 2);
    // 3 wide, 2 tall, 2 deep, centred between the two cubes.
    CHECK(Near(u->GetSizePtr()->x, 3.0f, 1e-3) && Near(u->GetSizePtr()->y, 2.0f, 1e-3) && Near(u->GetSizePtr()->z, 2.0f, 1e-3));
    CHECK(Near(u->GetPosPtr()->x, 10.5f, 1e-3) && Near(u->GetPosPtr()->y, 5.0f, 1e-3));
    CHECK(Near(u->UnionParts()[0].obj->GetPosPtr()->x, -0.5f, 1e-3));   // parts are relative to the union
    CHECK(!u->GetCollider().isBox);
    CHECK(!u->useGlobalColor);

    // Vertex colours: both source colours are present in the mesh, and the global colour whitens them.
    auto countColours = [](const ScatteredObject& o, int& red, int& blue, int& white) {
        red = blue = white = 0;
        const Model& m = o.GetModel();
        for (int mi = 0; mi < m.meshCount; ++mi) {
            const Mesh& mesh = m.meshes[mi];
            for (int i = 0; i < mesh.vertexCount; ++i) {
                const unsigned char* c = &mesh.colors[i * 4];
                if (c[0] == 255 && c[1] == 0 && c[2] == 0) red++;
                else if (c[0] == 0 && c[1] == 0 && c[2] == 255) blue++;
                else if (c[0] == 255 && c[1] == 255 && c[2] == 255) white++;
            }
        }
    };
    int r, b, w;
    countColours(*u, r, b, w);
    CHECK(r > 0 && b > 0 && w == 0);
    u->SetUseGlobalColor(true);
    countColours(*u, r, b, w);
    CHECK(r == 0 && b == 0 && w > 0);
    u->SetUseGlobalColor(false);
    countColours(*u, r, b, w);
    CHECK(r > 0 && b > 0 && w == 0);

    // --- save / load / clone -----------------------------------------------------------------------------------
    u->SetName("Pair");
    *u->GetColorPtr() = { 10, 200, 30, 255 };
    u->SetUseGlobalColor(true);
    ScatteredObject* uo = u.get();
    objs.push_back(uo);
    engine.AddEntity(std::move(u));
    ScatteredObject* cutter = Add(engine, objs, { 0, 0, 0 }, { 1, 1, 1 }, { 255, 255, 0, 255 }, ShapeType::Sphere);
    cutter->negate = true;
    {
        std::stringstream ss;
        CHECK(SaveSceneToStream(ss, objs, models, "", nullptr));
        std::vector<ScatteredObject*> loaded;
        std::vector<std::unique_ptr<ModelGroup>> loadedModels;
        ss.seekg(0);
        CHECK(LoadSceneFromStream(ss, engine, loaded, loadedModels, "", nullptr));
        CHECK(loaded.size() == 2);
        if (loaded.size() == 2) {
            ScatteredObject* lu = loaded[0];
            CHECK(lu->IsUnion() && lu->HasModel());
            CHECK(lu->UnionParts().size() == 2);
            CHECK(lu->useGlobalColor);
            CHECK(lu->GetColorPtr()->g == 200);
            CHECK(Near(lu->GetSizePtr()->x, 3.0f, 1e-3));
            CHECK(lu->UnionParts()[1].obj->GetColorPtr()->b == 255);
            countColours(*lu, r, b, w);
            CHECK(w > 0 && r == 0);
            CHECK(loaded[1]->negate);
            CHECK(lu->GetName() == "Pair");
        }
    }
    {
        std::unique_ptr<ScatteredObject> copy = uo->Clone();
        CHECK(copy->IsUnion() && copy->UnionParts().size() == 2 && copy->useGlobalColor);
        CHECK(copy->GetModel().meshes != uo->GetModel().meshes);   // its own mesh
        copy.reset();   // frees only its own mesh
        CHECK(uo->HasModel());
    }

    // --- subtract / intersect through MakeUnion ------------------------------------------------------------------
    {
        std::vector<std::unique_ptr<ScatteredObject>> p;
        p.push_back(Part({ 0, 0, 0 }, { 4, 4, 4 }, { 200, 200, 200, 255 }));
        p.push_back(Part({ 0, 0, 0 }, { 1, 6, 1 }, { 255, 0, 255, 255 }));
        auto holed = ScatteredObject::MakeUnion(std::move(p), { UnionMode::Add, UnionMode::Negate });
        CHECK(holed && holed->HasModel());
        if (holed) {
            countColours(*holed, r, b, w);
            int magenta = 0;
            const Model& m = holed->GetModel();
            for (int mi = 0; mi < m.meshCount; ++mi)
                for (int i = 0; i < m.meshes[mi].vertexCount; ++i) {
                    const unsigned char* c = &m.meshes[mi].colors[i * 4];
                    if (c[0] == 255 && c[1] == 0 && c[2] == 255) magenta++;
                }
            CHECK(magenta > 0);   // the hole's walls
            CHECK(Near(holed->GetSizePtr()->y, 4.0f, 1e-3));   // the cutter sticks out, the solid does not
        }
        std::vector<std::unique_ptr<ScatteredObject>> q;
        q.push_back(Part({ 0, 0, 0 }, { 2, 2, 2 }, { 255, 0, 0, 255 }));
        q.push_back(Part({ 1, 0, 0 }, { 2, 2, 2 }, { 0, 255, 0, 255 }));
        auto both = ScatteredObject::MakeUnion(std::move(q), { UnionMode::Intersect, UnionMode::Intersect });
        CHECK(both && Near(both->GetSizePtr()->x, 1.0f, 1e-3) && Near(both->GetPosPtr()->x, 0.5f, 1e-3));
        std::vector<std::unique_ptr<ScatteredObject>> n;
        n.push_back(Part({ 0, 0, 0 }, { 2, 2, 2 }, { 255, 0, 0, 255 }));
        CHECK(ScatteredObject::MakeUnion(std::move(n), { UnionMode::Negate }) == nullptr);
    }

    // --- separate: parts go back where the union (moved, scaled, turned) puts them ------------------------------
    {
        std::vector<std::unique_ptr<ScatteredObject>> p;
        p.push_back(Part({ 2, 0, 0 }, { 2, 2, 2 }, { 255, 0, 0, 255 }));
        p.push_back(Part({ 4, 0, 0 }, { 2, 2, 2 }, { 0, 0, 255, 255 }));
        p[1]->script = "Spin";
        auto un = ScatteredObject::MakeUnion(std::move(p), { UnionMode::Add, UnionMode::Add });
        CHECK(un != nullptr);
        if (un) {
            auto back = un->TakeUnionParts();
            CHECK(back.size() == 2 && !un->IsUnion() && !un->HasModel());
            if (back.size() == 2) {
                CHECK(Near(back[0]->GetPosPtr()->x, 2.0f, 1e-3) && Near(back[1]->GetPosPtr()->x, 4.0f, 1e-3));
                CHECK(back[1]->script == "Spin");
            }
        }
        std::vector<std::unique_ptr<ScatteredObject>> p2;
        p2.push_back(Part({ 2, 0, 0 }, { 2, 2, 2 }, { 255, 0, 0, 255 }));
        p2.push_back(Part({ 4, 0, 0 }, { 2, 2, 2 }, { 0, 0, 255, 255 }));
        auto un2 = ScatteredObject::MakeUnion(std::move(p2), { UnionMode::Add, UnionMode::Add });
        if (un2) {
            // Centre (3,0,0); turn 90 degrees about Y, double the length, move up.
            *un2->GetRotationPtr() = { 0, 90, 0 };
            un2->GetSizePtr()->x *= 2.0f;
            un2->GetPosPtr()->y += 10.0f;
            auto back = un2->TakeUnionParts();
            if (back.size() == 2) {
                // Part 0 sat at x=-1 relative to the centre: doubled, then Y-90 sends +x to -z (and -x to +z).
                const Vector3 p0 = *back[0]->GetPosPtr();
                CHECK(Near(p0.y, 10.0f, 1e-3) && Near(std::fabs(p0.z), 2.0f, 1e-3) && Near(p0.x - 3.0f, 0.0f, 1e-3));
                CHECK(Near(back[0]->GetRotationPtr()->y, 90.0f, 1e-2));
                const Vector3 p1 = *back[1]->GetPosPtr();
                CHECK(Near(p0.z, -p1.z, 1e-3));
            }
        }
    }

    // --- curved parts: inclusion-exclusion on the meshes' volumes (A + B = A u B + A n B), rotated parts included ---
    {
        auto meshVolume = [](ScatteredObject& o) {
            const Model& m = o.GetModel();
            const Vector3 sz = *o.GetSizePtr();
            double v = 0.0;
            for (int mi = 0; mi < m.meshCount; ++mi) {
                const Mesh& mesh = m.meshes[mi];
                for (int t = 0; t + 2 < mesh.vertexCount; t += 3) {
                    const float* a = &mesh.vertices[t * 3];
                    const float* b = &mesh.vertices[(t + 1) * 3];
                    const float* c = &mesh.vertices[(t + 2) * 3];
                    v += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0;
                }
            }
            return v * sz.x * sz.y * sz.z;
        };
        auto pair = [&](UnionMode mode, bool curved) {
            std::vector<std::unique_ptr<ScatteredObject>> p;
            p.push_back(Part({ 0, 0, 0 }, { 3, 2, 3 }, { 200, 50, 50, 255 }, curved ? ShapeType::Cylinder : ShapeType::Cube));
            p.push_back(Part({ 0.8f, 0.3f, 0.4f }, { 2.5f, 2.5f, 2.5f }, { 50, 50, 200, 255 }, curved ? ShapeType::Sphere : ShapeType::Cube));
            *p[1]->GetRotationPtr() = { 20.0f, 35.0f, 10.0f };
            auto u = ScatteredObject::MakeUnion(std::move(p), { mode == UnionMode::Intersect ? UnionMode::Intersect : UnionMode::Add,
                                                                  mode == UnionMode::Negate ? UnionMode::Negate : (mode == UnionMode::Intersect ? UnionMode::Intersect : UnionMode::Add) });
            return u ? meshVolume(*u) : 0.0;
        };
        for (bool curved : { false, true }) {
            std::vector<std::unique_ptr<ScatteredObject>> a, b;
            a.push_back(Part({ 0, 0, 0 }, { 3, 2, 3 }, WHITE, curved ? ShapeType::Cylinder : ShapeType::Cube));
            b.push_back(Part({ 0, 0, 0 }, { 3, 2, 3 }, WHITE, curved ? ShapeType::Cylinder : ShapeType::Cube));
            std::vector<std::unique_ptr<ScatteredObject>> sphere;
            sphere.push_back(Part({ 0.8f, 0.3f, 0.4f }, { 2.5f, 2.5f, 2.5f }, WHITE, curved ? ShapeType::Sphere : ShapeType::Cube));
            *sphere[0]->GetRotationPtr() = { 20.0f, 35.0f, 10.0f };
            auto ua = ScatteredObject::MakeUnion(std::move(b), { UnionMode::Add });
            auto ub = ScatteredObject::MakeUnion(std::move(sphere), { UnionMode::Add });
            CHECK(ua && ub);
            if (!ua || !ub) continue;
            const double va = meshVolume(*ua), vb = meshVolume(*ub);
            const double vu = pair(UnionMode::Add, curved), vi = pair(UnionMode::Intersect, curved), vd = pair(UnionMode::Negate, curved);
            std::printf("union_test: %s A %.4f B %.4f AuB %.4f AnB %.4f A-B %.4f\n", curved ? "cylinder/sphere" : "cube/cube", va, vb, vu, vi, vd);
            CHECK(va > 0 && vb > 0 && vi > 0);
            CHECK(Near(va + vb, vu + vi, 0.02 * (va + vb)));   // inclusion-exclusion
            CHECK(Near(va - vi, vd, 0.02 * va));               // A minus B = A - (A n B)
            CHECK(vu >= std::max(va, vb) - 1e-3 && vu <= va + vb + 1e-3);
        }
    }

    // --- the union is drawn and is a solid in physics terms ---------------------------------------------------------
    for (int f = 0; f < 4; f++) engine.StepFrame(1.0f / 60.0f);
    CHECK(uo->GetCollider().hulls.size() > 0 || uo->GetCollider().tris.size() > 0);

    // --- a scene from before unions (v33: no negate/union fields) still loads ------------------------------------
    {
        std::vector<ScatteredObject*> plain;
        std::vector<std::unique_ptr<ModelGroup>> m;
        auto p = std::make_unique<ScatteredObject>(Vector3{ 0, 0, 0 }, Vector3{ 1, 1, 1 }, WHITE, ShapeType::Cube);
        plain.push_back(p.get());
        std::stringstream ss;
        CHECK(SaveSceneToStream(ss, plain, m, "", nullptr));
        std::string text = ss.str();
        const size_t at = text.find(" 0 0\n");
        CHECK(text.rfind("SIMPLE_ENGINE_BUILD 34", 0) == 0 && at != std::string::npos);
        if (at != std::string::npos) {
            text.replace(at, 5, "\n");
            text.replace(0, 22, "SIMPLE_ENGINE_BUILD 33");
            std::istringstream in(text);
            std::vector<ScatteredObject*> old;
            std::vector<std::unique_ptr<ModelGroup>> m2;
            CHECK(LoadSceneFromStream(in, engine, old, m2, "", nullptr, nullptr));
            CHECK(old.size() == 1 && !old[0]->negate && !old[0]->IsUnion());
            for (ScatteredObject* q : old) q->alive = false;
        }
    }

    // --- stress: many curved parts and cutters stay fast enough for an undo step (every undo rebuilds unions) ---
    {
        std::vector<std::unique_ptr<ScatteredObject>> p;
        std::vector<UnionMode> modes;
        for (int i = 0; i < 10; ++i) {
            p.push_back(Part({ i * 0.9f, std::sin(i * 0.7f) * 0.5f, std::cos(i * 0.5f) * 0.5f }, { 1.6f, 1.6f, 1.6f },
                             { (unsigned char)(i * 25), 120, 200, 255 }, ShapeType::Sphere));
            modes.push_back(UnionMode::Add);
        }
        for (int i = 0; i < 3; ++i) {
            p.push_back(Part({ 1.5f + i * 3.0f, 0, 0 }, { 0.6f, 4, 0.6f }, WHITE, ShapeType::Cylinder));
            *p.back()->GetRotationPtr() = { 0, 0, 30.0f * i };
            modes.push_back(UnionMode::Negate);
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto big = ScatteredObject::MakeUnion(std::move(p), modes);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        int tris = 0;
        if (big) for (int mi = 0; mi < big->GetModel().meshCount; ++mi) tris += big->GetModel().meshes[mi].triangleCount;
        std::printf("union_test: 10 spheres + 3 cutters -> %d triangles in %.0f ms\n", tris, ms);
        CHECK(big && tris > 0);
        CHECK(ms < 3000.0);
        if (big) {
            // Undo/redo path: the union goes through a snapshot and comes back the same.
            std::vector<ScatteredObject*> one{ big.get() };
            std::vector<std::unique_ptr<ModelGroup>> m;
            std::string bytes;
            CHECK(SnapshotSceneToMemory(one, m, "", bytes));
            std::istringstream in(bytes);
            std::vector<ScatteredObject*> back;
            std::vector<std::unique_ptr<ModelGroup>> m2;
            const auto t1 = std::chrono::steady_clock::now();
            CHECK(RestoreSceneFromMemory(in, engine, back, m2, ""));
            const double restoreMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
            std::printf("union_test: snapshot restore of it took %.0f ms\n", restoreMs);
            CHECK(back.size() == 1 && back[0]->IsUnion() && back[0]->UnionParts().size() == 13);
            if (back.size() == 1) {
                int t2 = 0;
                for (int mi = 0; mi < back[0]->GetModel().meshCount; ++mi) t2 += back[0]->GetModel().meshes[mi].triangleCount;
                CHECK(t2 == tris);   // deterministic rebuild
            }
            for (ScatteredObject* q : back) q->alive = false;
        }
    }

    // --- physics: a union falls and lands, a Negate hole is a real hole for rays, a cutter does not collide ------
    {
        std::vector<ScatteredObject*> world;
        auto addTo = [&](std::unique_ptr<ScatteredObject> o) { ScatteredObject* raw = o.get(); world.push_back(raw); engine.AddEntity(std::move(o)); return raw; };
        addTo(Part({ 200, -0.5f, 0 }, { 60, 1, 60 }, GRAY));   // floor (anchored)

        // Two cubes side by side: 3 x 1 x 1, dropped from 6 m; it should come to rest on the floor (centre at 0.5).
        std::vector<std::unique_ptr<ScatteredObject>> d;
        d.push_back(Part({ 190, 6, 0 }, { 2, 1, 1 }, RED));
        d.push_back(Part({ 191, 6, 0 }, { 2, 1, 1 }, BLUE));
        d[0]->anchored = false;
        ScatteredObject* drop = addTo(ScatteredObject::MakeUnion(std::move(d), { UnionMode::Add, UnionMode::Add }));
        CHECK(!drop->anchored);

        // An anchored 6 x 1 x 6 slab at y 3 with a 2 m round hole carved through its middle, precise collision.
        std::vector<std::unique_ptr<ScatteredObject>> h;
        h.push_back(Part({ 210, 3, 0 }, { 6, 1, 6 }, GRAY));
        h.push_back(Part({ 210, 1.5f, 0 }, { 2, 3, 2 }, WHITE, ShapeType::Cylinder));   // a cylinder's origin is its base: y 1.5 .. 4.5
        ScatteredObject* slab = addTo(ScatteredObject::MakeUnion(std::move(h), { UnionMode::Add, UnionMode::Negate }));
        slab->SetCollisionAccuracy(pcoll::CollisionAccuracy::Precise);

        // A free-standing cutter above the floor: rays and bodies go straight through it.
        ScatteredObject* cutter = addTo(Part({ 220, 2, 0 }, { 2, 2, 2 }, WHITE));
        cutter->negate = true;
        cutter->canCollide = false;

        auto simOwner = std::make_unique<phys::Simulation>(world);
        phys::Simulation* sim = simOwner.get();
        engine.AddEntity(std::move(simOwner));
        sim->StartPlay();
        for (int f = 0; f < 300; ++f) sim->Update(1.0f / 60.0f);
        std::printf("union_test: dropped union rests at y %.2f (expected 0.50)\n", drop->GetPosPtr()->y);
        CHECK(std::fabs(drop->GetPosPtr()->y - 0.5f) < 0.1f);

        const phys::RaycastHit solid = sim->RayCast({ 212.5f, 20, 0 }, { 0, -1, 0 }, 40.0f);
        const phys::RaycastHit hole = sim->RayCast({ 210, 20, 0 }, { 0, -1, 0 }, 40.0f);
        const phys::RaycastHit ghost = sim->RayCast({ 220, 20, 0 }, { 0, -1, 0 }, 40.0f);
        std::printf("union_test: ray on slab y %.2f, through hole y %.2f, through cutter y %.2f\n",
                    solid.hit ? solid.point.y : -99.0f, hole.hit ? hole.point.y : -99.0f, ghost.hit ? ghost.point.y : -99.0f);
        CHECK(solid.hit && std::fabs(solid.point.y - 3.5f) < 0.05f);   // slab top
        CHECK(hole.hit && std::fabs(hole.point.y) < 0.05f);             // the floor, through the hole
        CHECK(ghost.hit && std::fabs(ghost.point.y) < 0.05f);           // the floor, through the cutter
        sim->alive = false;
        for (ScatteredObject* o : world) o->alive = false;
    }

    // FLY_UNION_SHOT=<file.png>: render a union beside a plain cube for a look (not part of the pass/fail).
    if (const char* shot = std::getenv("FLY_UNION_SHOT")) {
        Add(engine, objs, { 0, -0.5f, 0 }, { 40, 1, 40 }, { 150, 150, 150, 255 });
        Add(engine, objs, { -4, 1, 0 }, { 2, 2, 2 }, { 40, 90, 200, 255 });
        std::vector<std::unique_ptr<ScatteredObject>> sp;
        sp.push_back(Part({ 3, 1, 0 }, { 2, 2, 2 }, { 40, 90, 200, 255 }));
        sp.push_back(Part({ 4, 2, 1 }, { 2, 2, 2 }, { 200, 60, 40, 255 }));
        sp.push_back(Part({ 4.5f, 1, -1 }, { 1.5f, 3, 1.5f }, { 60, 200, 80, 255 }, ShapeType::Cylinder));
        auto su = ScatteredObject::MakeUnion(std::move(sp), { UnionMode::Add, UnionMode::Add, UnionMode::Add });
        if (su) { objs.push_back(su.get()); engine.AddEntity(std::move(su)); }
        for (int f = 0; f < 8; f++) engine.StepFrame(1.0f / 60.0f);
        TakeScreenshot(shot);
    }

    std::printf(g_fail ? "union_test: %d FAILED\n" : "union_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
