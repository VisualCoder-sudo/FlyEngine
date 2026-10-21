// Physically based shading: the baked environment (src/Engine/RL/rl_ibl.cpp) exists and lights the lit shaders,
// metallic and roughness change the picture the way they should, and an object's material survives a scene
// save/load and a clone. Opens a window (it needs an Engine), so ctest needs a display (label needs_display):
// build/tests/pbr_test. The pictures are left as pbr_<api>_*.png.
#include "raylib.h"
#include "Engine.hpp"
#include "Graphics/Graphics.hpp"
#include "Graphics/PostFX.hpp"
#include "Engine/Backend/ScenePersistence.hpp"
#include "Engine/Frontend/ui.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static ScatteredObject* Make(Engine& engine, std::vector<ScatteredObject*>& objs, const char* name, ShapeType shape,
                             Vector3 pos, Vector3 size, Color color) {
    auto o = std::make_unique<ScatteredObject>(pos, size, color, shape);
    o->SetName(name);
    ScatteredObject* raw = o.get();
    objs.push_back(raw);
    engine.AddEntity(std::move(o));
    return raw;
}

// Colour statistics of the pixels within `radius` (fraction of the picture's height) of a world point.
struct Patch {
    float r = 0, g = 0, b = 0;   // mean, 0..255
    float lumaSpread = 0;        // standard deviation of the brightness
    float detail = 0;            // mean brightness step between pixels 2 apart (fine bumps, not broad shading)
};
static Patch Measure(const Image& img, const Camera3D& cam, Vector3 world, float radius) {
    const Vector2 s = GetWorldToScreen(world, cam);
    const float sx = s.x * img.width / (float)GetScreenWidth(), sy = s.y * img.height / (float)GetScreenHeight();
    const int rad = (int)(radius * img.height);
    Patch p;
    double sum = 0, sum2 = 0;
    long n = 0;
    for (int y = (int)sy - rad; y <= (int)sy + rad; y++)
        for (int x = (int)sx - rad; x <= (int)sx + rad; x++) {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height || (x - sx) * (x - sx) + (y - sy) * (y - sy) > rad * rad) continue;
            const Color c = GetImageColor(img, x, y);
            p.r += c.r; p.g += c.g; p.b += c.b;
            const double l = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
            sum += l; sum2 += l * l;
            if (x + 2 < img.width && y + 2 < img.height) {
                const Color cx = GetImageColor(img, x + 2, y), cy = GetImageColor(img, x, y + 2);
                const double lx = 0.2126 * cx.r + 0.7152 * cx.g + 0.0722 * cx.b, ly = 0.2126 * cy.r + 0.7152 * cy.g + 0.0722 * cy.b;
                p.detail += (float)(std::fabs(lx - l) + std::fabs(ly - l));
            }
            n++;
        }
    if (n == 0) return p;
    p.r /= n; p.g /= n; p.b /= n;
    p.detail /= n;
    const double mean = sum / n;
    p.lumaSpread = (float)std::sqrt(std::fmax(sum2 / n - mean * mean, 0.0));
    return p;
}

// File names carry the graphics API, so pbr_test and pbr_test_fallback can run side by side (ctest -j).
static std::string g_api;
static std::string Out(const std::string& name) { return "pbr_" + g_api + "_" + name; }

static Image Shoot(Engine& engine, const char* name) {
    for (int f = 0; f < 8; f++) engine.StepFrame(1.0f / 60.0f);
    char path[96];
    std::snprintf(path, sizeof path, "%s.png", Out(name).c_str());
    TakeScreenshot(path);
    return LoadImage(path);
}

int main() {
    // Instanced shape batches draw one instance per group in LTO builds (see object_nodes_test); draw each on its own.
    setenv("FLY_NO_SHAPE_BATCH", "1", 1);
    Engine engine(960, 540, "pbr_test", 60);
    for (const char* c = GetGraphicsBackendName(); *c; ++c) g_api += (char)std::tolower((unsigned char)*c);
    engine.SetPlayerBuild(true);
    std::vector<ScatteredObject*> objs;
    std::vector<std::unique_ptr<ModelGroup>> models;

    // --- the material is saved, loaded and cloned ------------------------------------------------------------
    {
        ScatteredObject* o = Make(engine, objs, "Brass", ShapeType::Sphere, { 0, 500, 0 }, { 1, 1, 1 }, { 220, 180, 90, 255 });
        o->surface.metallic = 1.0f;
        o->surface.roughness = 0.25f;
        o->surface.emissive = { 10, 200, 30, 255 };
        o->surface.emissiveStrength = 2.5f;
        o->useModelMaterial = false;
        std::stringstream ss;
        CHECK(SaveSceneToStream(ss, objs, models, "", nullptr));
        std::vector<ScatteredObject*> loaded;
        std::vector<std::unique_ptr<ModelGroup>> m2;
        CHECK(LoadSceneFromStream(ss, engine, loaded, m2, "", nullptr, nullptr));
        CHECK(loaded.size() == 1);
        if (loaded.size() == 1) {
            const gfx::SurfaceMaterial& m = loaded[0]->surface;
            CHECK(std::fabs(m.metallic - 1.0f) < 1e-4f && std::fabs(m.roughness - 0.25f) < 1e-4f);
            CHECK(m.emissive.r == 10 && m.emissive.g == 200 && m.emissive.b == 30 && std::fabs(m.emissiveStrength - 2.5f) < 1e-4f);
            CHECK(!loaded[0]->useModelMaterial);
            loaded[0]->alive = false;
        }
        auto copy = o->Clone();
        CHECK(copy->surface == o->surface && !copy->useModelMaterial);
        // A scene from before materials (v32: the same record without the material at its end) loads with the defaults.
        {
            std::vector<ScatteredObject*> plain;
            std::vector<std::unique_ptr<ModelGroup>> m4;
            auto p = std::make_unique<ScatteredObject>(Vector3{ 0, 0, 0 }, Vector3{ 1, 1, 1 }, WHITE, ShapeType::Cube);
            plain.push_back(p.get());
            std::stringstream v33;
            CHECK(SaveSceneToStream(v33, plain, m4, "", nullptr));
            std::string text = v33.str();
            const std::string material = " 0 0.600000024 255 255 255 0 1\n";
            const size_t at = text.find(material);
            CHECK(text.rfind("SIMPLE_ENGINE_BUILD 33", 0) == 0 && at != std::string::npos);
            if (at != std::string::npos) {
                text.replace(at, material.size(), "\n");
                text.replace(0, 22, "SIMPLE_ENGINE_BUILD 32");
                std::istringstream in(text);
                std::vector<ScatteredObject*> old;
                std::vector<std::unique_ptr<ModelGroup>> m3;
                CHECK(LoadSceneFromStream(in, engine, old, m3, "", nullptr, nullptr));
                CHECK(old.size() == 1 && old[0]->surface == gfx::SurfaceMaterial{} && old[0]->useModelMaterial);
                for (ScatteredObject* q : old) q->alive = false;
            }
        }
        o->alive = false;
        objs.clear();
    }

    // --- a row of non-metals and a row of metals, smooth to rough, under an afternoon sky ------------------
    gfx::Lighting() = gfx::NewSceneLighting();
    gfx::Lighting().hasClouds = false;          // nothing random in the picture
    gfx::SetTimeOfDay(15.0f);
    Make(engine, objs, "Floor", ShapeType::Cube, { 0, -0.5f, 0 }, { 60, 1, 60 }, { 120, 120, 120, 255 });
    const float rough[5] = { 0.05f, 0.25f, 0.5f, 0.75f, 1.0f };
    ScatteredObject* plastic[5];
    ScatteredObject* metal[5];
    // Plain white surfaces: the default checker texture would swamp the contrast measured below.
    {
        const Texture2D checker = gfx::GetDefaultTexture();
        std::vector<Color> white((size_t)checker.width * (size_t)checker.height, WHITE);
        UpdateTexture(checker, white.data());
    }
    // The metals float above the non-metals, so neither row hides the other.
    for (int i = 0; i < 5; i++) {
        const float x = -6.0f + 3.0f * i;
        plastic[i] = Make(engine, objs, "Plastic", ShapeType::Sphere, { x, 1.2f, 0.0f }, { 2.2f, 2.2f, 2.2f }, { 200, 40, 40, 255 });
        plastic[i]->surface.roughness = rough[i];
        metal[i] = Make(engine, objs, "Metal", ShapeType::Sphere, { x, 4.0f, 0.0f }, { 2.2f, 2.2f, 2.2f }, { 235, 235, 235, 255 });
        metal[i]->surface.metallic = 1.0f;
        metal[i]->surface.roughness = rough[i];
    }
    // Looking north, with the afternoon sun behind the camera.
    Camera3D& cam = engine.GetCamera();
    cam.position = { 0.0f, 3.0f, -14.0f };
    cam.target = { 0.0f, 2.6f, 0.0f };
    cam.up = { 0.0f, 1.0f, 0.0f };
    cam.fovy = 50.0f;
    cam.projection = CAMERA_PERSPECTIVE;

    gfx::Quality().environmentLighting = true;
    Image lit = Shoot(engine, "spheres");
    CHECK(IsEnvironmentBaked());
    CHECK(GetEnvSpecular().id != 0 && GetEnvIrradiance().id != 0 && GetEnvBrdfLut().id != 0);
    Patch p[5], m[5];
    for (int i = 0; i < 5; i++) {
        p[i] = Measure(lit, cam, *plastic[i]->GetPosPtr(), 0.05f);
        m[i] = Measure(lit, cam, *metal[i]->GetPosPtr(), 0.05f);
        std::printf("roughness %.2f: plastic rgb %.0f %.0f %.0f spread %.1f | metal rgb %.0f %.0f %.0f spread %.1f\n", rough[i],
                    p[i].r, p[i].g, p[i].b, p[i].lumaSpread, m[i].r, m[i].g, m[i].b, m[i].lumaSpread);
    }
    // A mirror-smooth metal shows its surroundings: the blue sky in its upper half, the olive ground in its lower
    // half. The fully rough one blurs the two into much the same shade.
    auto split = [&](int i) {
        const Vector3 c = *metal[i]->GetPosPtr();
        const Patch up = Measure(lit, cam, Vector3Add(c, { 0.0f, 0.5f, -0.7f }), 0.012f);
        const Patch down = Measure(lit, cam, Vector3Add(c, { 0.0f, -0.5f, -0.7f }), 0.012f);
        return (up.b - up.r) - (down.b - down.r);
    };
    std::printf("sky/ground split in the metal: smooth %.1f, rough %.1f\n", split(0), split(4));
    CHECK(split(0) > split(4) + 20.0f);
    // A white metal is coloured by what it reflects: the blue sky makes the top of the smooth ball bluer than red.
    const Patch top = Measure(lit, cam, Vector3Add(*metal[0]->GetPosPtr(), { 0.0f, 0.75f, -0.35f }), 0.012f);
    std::printf("smooth metal, upper part: rgb %.0f %.0f %.0f\n", top.r, top.g, top.b);
    CHECK(top.b > top.r + 5.0f);
    // A red non-metal stays red at every roughness (the reflection is only a sheen on top).
    for (int i = 0; i < 5; i++) CHECK(p[i].r > p[i].g + 40.0f);

    // Without the baked environment the metal falls back to the flat sky/ground colours: the smooth ball loses its detail.
    gfx::Quality().environmentLighting = false;
    Image flat = Shoot(engine, "spheres_flat");
    const Patch mf = Measure(flat, cam, *metal[0]->GetPosPtr(), 0.05f);
    std::printf("smooth metal without the environment: spread %.1f (with: %.1f)\n", mf.lumaSpread, m[0].lumaSpread);
    CHECK(mf.lumaSpread < m[0].lumaSpread);
    gfx::Quality().environmentLighting = true;

    // A normal map, through the real import path: an OBJ whose material names a bump map of tilted stripes. The
    // loader has no tangents for it, so they are generated; the stripes then show as bands of light and shade on a
    // ball that is otherwise as smooth as the plain one beside it.
    {
        Mesh sphere = GenMeshSphere(0.5f, 32, 32);
        CHECK(ExportMesh(sphere, Out("ball_mesh.obj").c_str()));
        UnloadMesh(sphere);
        std::ifstream meshIn(Out("ball_mesh.obj"));
        std::stringstream body;
        body << meshIn.rdbuf();
        std::ofstream(Out("ball.obj")) << "mtllib " << Out("ball.mtl") << "\nusemtl stripes\n" << body.str();
        std::ofstream(Out("ball.mtl")) << "newmtl stripes\nKd 1 1 1\nbump " << Out("stripes.png") << "\n";
        Image stripes = GenImageColor(64, 64, WHITE);
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                const bool left = (x / 4) % 2 == 0;   // normals leaning 45 degrees one way, then the other
                ImageDrawPixel(&stripes, x, y, left ? Color{ 38, 128, 218, 255 } : Color{ 218, 128, 218, 255 });
            }
        ExportImage(stripes, Out("stripes.png").c_str());
        UnloadImage(stripes);

        for (int i = 0; i < 5; i++) { plastic[i]->hidden = true; metal[i]->hidden = true; }
        ScatteredObject* bumpy = Make(engine, objs, "Bumpy", ShapeType::Sphere, { 2.0f, 3.0f, 0.0f }, { 3, 3, 3 }, WHITE);
        ScatteredObject* smooth = Make(engine, objs, "Smooth", ShapeType::Sphere, { -2.0f, 3.0f, 0.0f }, { 3, 3, 3 }, WHITE);
        CHECK(bumpy->SetModel(Out("ball.obj")));
        bumpy->NormalizeModelToUnitBox();
        const Model& model = bumpy->GetModel();
        bool hasNormalMap = false, hasTangents = model.meshCount > 0;
        for (int i = 0; i < model.materialCount; i++)
            if (model.materials[i].maps && model.materials[i].maps[MATERIAL_MAP_NORMAL].texture.id != 0) hasNormalMap = true;
        for (int i = 0; i < model.meshCount; i++) if (!model.meshes[i].tangents) hasTangents = false;
        CHECK(hasNormalMap);
        CHECK(hasTangents);
        for (ScatteredObject* o : { bumpy, smooth }) { o->useModelMaterial = false; o->surface.roughness = 1.0f; }
        Image img = Shoot(engine, "normal_map");
        const Patch b = Measure(img, cam, *bumpy->GetPosPtr(), 0.06f), sm = Measure(img, cam, *smooth->GetPosPtr(), 0.06f);
        std::printf("normal map: striped ball detail %.1f, plain ball %.1f\n", b.detail, sm.detail);
        CHECK(b.detail > sm.detail * 2.0f + 2.0f);
        UnloadImage(img);
        bumpy->alive = false;
        smooth->alive = false;
        for (int i = 0; i < 5; i++) { plastic[i]->hidden = false; metal[i]->hidden = false; }
    }

    // Glow: an emissive ball at night is far brighter than the same ball without it.
    gfx::SetTimeOfDay(0.5f);
    plastic[2]->surface.emissiveStrength = 0.0f;
    Image dark = Shoot(engine, "night");
    plastic[2]->surface.emissive = { 255, 120, 20, 255 };
    plastic[2]->surface.emissiveStrength = 3.0f;
    Image glow = Shoot(engine, "night_glow");
    const Patch d0 = Measure(dark, cam, *plastic[2]->GetPosPtr(), 0.04f), d1 = Measure(glow, cam, *plastic[2]->GetPosPtr(), 0.04f);
    std::printf("night ball: plain %.0f, glowing %.0f\n", d0.r + d0.g + d0.b, d1.r + d1.g + d1.b);
    CHECK(d1.r + d1.g + d1.b > d0.r + d0.g + d0.b + 120.0f);

    UnloadImage(lit);
    UnloadImage(flat);
    UnloadImage(dark);
    UnloadImage(glow);
    if (g_fail == 0) std::printf("pbr_test: all checks passed\n");
    else std::printf("pbr_test: %d FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
