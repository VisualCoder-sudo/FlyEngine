// Unions (solid modelling): the CSG core on its own (union / subtract / intersect volumes, colours kept per face),
// and a ScatteredObject union: it keeps its parts' colours, honours the global colour, survives a scene save/load
// and a clone, and Separate hands the parts back where they were. Opens a window (it needs a GPU for the mesh),
// so ctest needs a display (label needs_display): build/tests/union_test
#include "raylib.h"
#include "Engine.hpp"
#include "Graphics/Graphics.hpp"
#include "Engine/Backend/Csg.hpp"
#include "Engine/Backend/ScenePersistence.hpp"
#include "../include/Terrain/BasicTerrain.hpp"
#include "Terrain/Water/WaterBody.hpp"
#define private public
#include "../include/Terrain/Terrain.hpp"
#undef private
#include "Engine/Frontend/ui.hpp"

#include <cmath>
#include <cstdio>
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

    // --- the union is drawn and is a solid in physics terms ---------------------------------------------------------
    for (int f = 0; f < 4; f++) engine.StepFrame(1.0f / 60.0f);
    CHECK(uo->GetCollider().hulls.size() > 0 || uo->GetCollider().tris.size() > 0);

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
