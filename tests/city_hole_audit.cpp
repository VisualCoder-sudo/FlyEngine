// Holes in the ground of a city on hills: the engine's ground plane is hidden and the sky is magenta, so any gap in
// the road / sidewalk / pad surface shows as magenta from straight above. Counts those pixels over tiles of the city
// (flat, hills, graded, organic, random) and writes the pictures (city_hole_<name>_<n>.png). Opens a window, so it is
// not registered with ctest: build/tests/city_hole_audit
#include "raylib.h"
#include "raymath.h"
#include "Engine.hpp"
#include "Engine/Graphics.hpp"
#include "../include/CityGen/City.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// Counts magenta pixels in the picture (a hole) and returns the first one found.
static int Holes(const char* path, int* hx, int* hy) {
    Image img = LoadImage(path);
    int n = 0;
    *hx = *hy = -1;
    for (int y = 0; y < img.height; y++)
        for (int x = 0; x < img.width; x++) {
            const Color c = GetImageColor(img, x, y);
            if (c.r > 200 && c.g < 60 && c.b > 200) { if (n == 0) { *hx = x; *hy = y; } n++; }
        }
    UnloadImage(img);
    return n;
}

static int Audit(Engine& engine, City& c, const std::string& name) {
    float lo = 1e9f, hi = -1e9f, zlo = 1e9f, zhi = -1e9f;
    for (const RoadNode& n : c.GetNodes()) { lo = fminf(lo, n.pos.x); hi = fmaxf(hi, n.pos.x); zlo = fminf(zlo, n.pos.y); zhi = fmaxf(zhi, n.pos.y); }
    lo += 60.0f; zlo += 60.0f; hi -= 60.0f; zhi -= 60.0f;
    int total = 0;
    int idx = 0;
    for (float z = zlo; z <= zhi; z += 50.0f)
        for (float x = lo; x <= hi; x += 50.0f) {
            // Straight down from 62 m (at 45 degrees fov a tile of about 50 m): look at the ground below.
            float ground = 0.0f;
            for (const RoadNode& n : c.GetNodes()) if (Vector2Distance(n.pos, { x, z }) < 40.0f) { ground = n.h; break; }
            Camera3D& cam = engine.GetCamera();
            cam.position = { x, ground + 60.0f, z + 0.01f };
            cam.target = { x, ground, z };
            cam.up = { 0.0f, 0.0f, -1.0f };
            cam.fovy = 45.0f;
            cam.projection = CAMERA_PERSPECTIVE;
            for (int f = 0; f < 6; f++) engine.StepFrame(1.0f / 60.0f);
            const std::string pathS = "city_hole_" + name + "_" + std::to_string(idx++) + ".png";
            const char* path = pathS.c_str();
            TakeScreenshot(path);
            int hx, hy;
            const int n = Holes(path, &hx, &hy);
            if (n > 0) std::printf("  %s tile (%.0f, %.0f): %d hole pixels, first at (%d, %d) -> %s\n", name.c_str(), x, z, n, hx, hy, path);
            total += n;
        }
    std::printf("%s: %d hole pixels in %d tiles\n", name.c_str(), total, idx);
    return total;
}

int main() {
    Engine engine(700, 700, "city_hole_audit", 60);
    engine.SetPlayerBuild(true);
    gfx::SetTimeOfDay(12.0f);
    gfx::SetGridVisible(false);
    gfx::Lighting().hasSky = true;
    gfx::Lighting().skyColor[0] = 1.0f; gfx::Lighting().skyColor[1] = 0.0f; gfx::Lighting().skyColor[2] = 1.0f;
    auto owner = std::make_unique<City>();
    City& c = *owner;
    engine.AddEntity(std::move(owner));
    c.GetParams().gridX = 6; c.GetParams().gridZ = 6;
    c.GetParams().cars = 0;
    c.GenerateGrid({ 0.0f, 0.0f });
    const int flat = Audit(engine, c, std::string("flat"));
    for (int i = 0; i < (int)c.GetNodes().size(); i++) {
        const Vector2 p = c.GetNodes()[(size_t)i].pos;
        c.SetNodeHeight(i, 5.0f + 4.0f * sinf(p.x * 0.045f) + 4.0f * cosf(p.y * 0.05f + p.x * 0.02f));
    }
    const int hills = Audit(engine, c, std::string("hills"));
    c.LimitRoadGrades(8.0f);
    const int graded = Audit(engine, c, std::string("graded"));
    // An organic (irregular) layout on hills, and a bigger hill amplitude.
    int organic = 0, steep = 0;
    {
        auto o2 = std::make_unique<City>();
        City& oc = *o2;
        engine.AddEntity(std::move(o2));
        oc.GetParams().gridX = 7; oc.GetParams().gridZ = 7;
        oc.GetParams().organic = true; oc.GetParams().organicStrength = 0.6f;
        oc.GetParams().cars = 0;
        oc.GenerateGrid({ 400.0f, 0.0f });
        for (int i = 0; i < (int)oc.GetNodes().size(); i++) {
            const Vector2 p = oc.GetNodes()[(size_t)i].pos;
            oc.SetNodeHeight(i, 6.0f + 5.0f * sinf(p.x * 0.05f) + 5.0f * cosf(p.y * 0.06f + p.x * 0.02f));
        }
        organic = Audit(engine, oc, std::string("organic"));
        for (int i = 0; i < (int)oc.GetNodes().size(); i++) {
            const Vector2 p = oc.GetNodes()[(size_t)i].pos;
            oc.SetNodeHeight(i, 10.0f + 9.0f * sinf(p.x * 0.07f) + 9.0f * cosf(p.y * 0.08f + p.x * 0.03f));
        }
        steep = Audit(engine, oc, std::string("steep"));
    }
    // Random ground: several seeds, per-node noise on top of hills, with and without districts.
    int random = 0;
    for (int seed = 1; seed <= 6; seed++) {
        auto o4 = std::make_unique<City>();
        City& rc = *o4;
        engine.AddEntity(std::move(o4));
        rc.GetParams().gridX = 6; rc.GetParams().gridZ = 6; rc.GetParams().cars = 0; rc.GetParams().seed = (uint32_t)(seed * 7919);
        rc.GenerateGrid({ 1000.0f * (float)seed, 0.0f });
        if (seed % 2 == 0) rc.AutoDistricts(3.0f);
        uint32_t st = (uint32_t)seed * 2654435761u;
        auto rnd = [&]() { st = st * 1664525u + 1013904223u; return (float)(st >> 8) / 16777216.0f; };
        for (int i = 0; i < (int)rc.GetNodes().size(); i++) {
            const Vector2 p = rc.GetNodes()[(size_t)i].pos;
            rc.SetNodeHeight(i, 6.0f + 6.0f * sinf(p.x * 0.04f + (float)seed) + 5.0f * cosf(p.y * 0.05f) + 3.0f * rnd());
        }
        random += Audit(engine, rc, "random" + std::to_string(seed));
    }
    std::printf("holes in random cities: %d\n", random);
    // The engine's ground plane (a 40 m square at y = 0 around the origin) must not cover roads, pads and lamp bases of a
    // city on ground that dips below y = 0: the same close-up views with the plane on and off must be identical.
    int planeDiff = 0;
    {
        auto o5 = std::make_unique<City>();
        City& pc = *o5;
        engine.AddEntity(std::move(o5));
        pc.GetParams().gridX = 4; pc.GetParams().gridZ = 4; pc.GetParams().cars = 0;
        pc.GenerateGrid({ 0.0f, 0.0f });
        for (int i = 0; i < (int)pc.GetNodes().size(); i++) {
            const Vector2 p = pc.GetNodes()[(size_t)i].pos;
            pc.SetNodeHeight(i, fmaxf(0.0f, 0.06f * p.x + 0.04f * p.y + 1.0f));      // ground level near the origin, rising away
        }
        gfx::Lighting().hasSky = false;
        struct V { float x, y, z, tx, ty, tz; } views[] = { { 0, 4, 12, 0, 0, 0 }, { -10, 3, -10, 0, 0, 0 }, { 0, 6, 0.01f, 0, 0, 0 }, { -18, 2.5f, 15, -8, 0, 0 } };
        for (const V& v : views) {
            Camera3D& cam = engine.GetCamera();
            cam.position = { v.x, v.y, v.z }; cam.target = { v.tx, v.ty, v.tz }; cam.up = { 0, 1, 0 }; cam.fovy = 60.0f;
            if (fabsf(v.x - v.tx) + fabsf(v.z - v.tz) < 1.0f) cam.up = { 0, 0, -1 };
            gfx::SetGridVisible(true);
            for (int f = 0; f < 8; f++) engine.StepFrame(1.0f / 60.0f);
            TakeScreenshot("city_hole_plane_on.png");
            gfx::SetGridVisible(false);
            for (int f = 0; f < 8; f++) engine.StepFrame(1.0f / 60.0f);
            TakeScreenshot("city_hole_plane_off.png");
            Image a = LoadImage("city_hole_plane_on.png"), b = LoadImage("city_hole_plane_off.png");
            for (int y = 0; y < a.height; y++) for (int x = 0; x < a.width; x++) {
                const Color p = GetImageColor(a, x, y), q = GetImageColor(b, x, y);
                if (std::abs(p.r - q.r) + std::abs(p.g - q.g) + std::abs(p.b - q.b) > 3) planeDiff++;
            }
            UnloadImage(a); UnloadImage(b);
        }
        std::printf("ground plane changes %d pixels of the close-up views\n", planeDiff);
    }
    std::printf("holes: flat %d, hills %d, graded %d, organic %d, steep %d, random %d\n", flat, hills, graded, organic, steep, random);
    CHECK(flat == 0 && hills == 0 && graded == 0 && organic == 0 && steep == 0 && random == 0 && planeDiff == 0);
    std::printf(g_fail ? "city_hole_audit: %d FAILED (hills holes: %d)\n" : "city_hole_audit: ok (hills holes: %d)\n", g_fail ? g_fail : hills, hills);
    return g_fail ? 1 : 0;
}
