// Sidewalk furniture: every prop type gets placed in a generated city, and a close-up screenshot of one of
// each is written to the working directory (city_prop_<name>.png) for a visual check. Opens a window, so it is
// not registered with ctest: build/tests/city_props_test
#include "raylib.h"
#include "raymath.h"
#include "Engine.hpp"
#include "Engine/Graphics.hpp"
#include "../include/CityGen/City.hpp"

#include <cmath>
#include <cstdio>
#include <memory>

using namespace city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
    Engine engine(640, 360, "city_props_test", 60);
    engine.SetPlayerBuild(true);   // no editor UI
    auto cityOwner = std::make_unique<City>();
    City& c = *cityOwner;
    c.GetParams().gridX = 10; c.GetParams().gridZ = 10;
    c.GetParams().furniture = true;
    engine.AddEntity(std::move(cityOwner));
    c.GenerateGrid({ 0.0f, 0.0f });

    struct P { int shape; const char* name; };
    const P props[] = { { kPropBench, "bench" }, { kPropHydrant, "hydrant" }, { kPropBollard, "bollard" },
                        { kPropSign, "sign" }, { kPropLamp, "lamp" } };
    for (const P& p : props) {
        const int n = c.CountInstances(p.shape);
        std::printf("%-8s %d placed\n", p.name, n);
        CHECK(n > 0);
        Vector3 pos; float yaw;
        if (!c.FindInstance(p.shape, n / 2, pos, yaw)) continue;
        const Vector3 toRoad = { sinf(yaw), 0.0f, cosf(yaw) };            // local +z points toward the road
        Camera3D& cam = engine.GetCamera();
        const float d = p.shape == kPropBusStop ? 6.5f : 3.2f;
        cam.position = { pos.x + toRoad.x * d + toRoad.z * 0.8f, pos.y + 1.5f, pos.z + toRoad.z * d - toRoad.x * 0.8f };
        cam.target = { pos.x, pos.y + (p.shape == kPropBusStop ? 1.2f : 0.7f), pos.z };
        cam.up = { 0.0f, 1.0f, 0.0f };
        cam.fovy = 55.0f;
        cam.projection = CAMERA_PERSPECTIVE;
        for (int f = 0; f < 8; f++) engine.StepFrame(1.0f / 60.0f);
        char path[128];
        std::snprintf(path, sizeof path, "city_prop_%s.png", p.name);
        TakeScreenshot(path);
    }

    // Night: street lamps light the road and sidewalk around them (light pools); screenshot for a visual check.
    {
        gfx::SetTimeOfDay(21.0f);
        Vector3 pos; float yaw;
        if (c.FindInstance(kPropLamp, c.CountInstances(kPropLamp) / 3, pos, yaw)) {
            const Vector3 toRoad = { sinf(yaw), 0.0f, cosf(yaw) };
            Camera3D& cam = engine.GetCamera();
            cam.position = { pos.x + toRoad.x * 9.0f, pos.y + 2.2f, pos.z + toRoad.z * 9.0f };
            cam.target = { pos.x - toRoad.x * 2.0f, pos.y + 0.5f, pos.z - toRoad.z * 2.0f };
            cam.up = { 0.0f, 1.0f, 0.0f };
            cam.fovy = 70.0f;
            for (int f = 0; f < 8; f++) engine.StepFrame(1.0f / 60.0f);
            TakeScreenshot("city_night_lamp.png");
        }
    }

    // Slope: cars tilt with the road. A steep road, traffic, and a close-up of a car on the hill.
    {
        c.ClearTransit();
        c.GetParams().cars = 80;
        c.GetParams().trafficDetailDistance = 0.0f;
        const int edge = (int)c.GetEdges().size() / 2;
        c.SetRoadGrade(edge, c.GetEdges()[(size_t)edge].a, 12.0f);
        SetTrafficRunning(true);
        Vector3 pos; float yaw = 0.0f, pitch = 0.0f;
        bool found = false;
        for (int i = 0; i < 4000 && !found; i++) {
            c.TrafficStepForTest(0.05f);
            if (i % 20 == 0) found = c.FindSlopedCar(pos, yaw, pitch);
        }
        std::printf("sloped car: %s (pitch %.1f deg)\n", found ? "found" : "none", pitch * 57.2958f);
        CHECK(found);
        if (found) {
            const Vector3 fwd = { cosf(yaw), 0.0f, -sinf(yaw) };
            Camera3D& cam = engine.GetCamera();
            cam.position = { pos.x + fwd.z * 7.0f, pos.y + 1.3f, pos.z - fwd.x * 7.0f };   // from the side of the road
            cam.target = { pos.x, pos.y + 0.6f, pos.z };
            cam.up = { 0.0f, 1.0f, 0.0f };
            cam.fovy = 40.0f;
            for (int f = 0; f < 8; f++) engine.StepFrame(1.0f / 60.0f);
            TakeScreenshot("city_car_slope.png");
        }
        // Its turn signal must sit on the tilted body too: wait for a sloped car with a lit indicator.
        found = false;
        for (int i = 0; i < 6000 && !found; i++) {
            c.TrafficStepForTest(0.05f);
            found = c.FindSlopedCar(pos, yaw, pitch, true);
        }
        std::printf("sloped car with lit indicator: %s\n", found ? "found" : "none");
        CHECK(found);
        if (found) {
            const Vector3 fwd = { cosf(yaw), 0.0f, -sinf(yaw) };
            Camera3D& cam = engine.GetCamera();
            cam.position = { pos.x + fwd.z * 6.5f + fwd.x * 2.0f, pos.y + 1.3f, pos.z - fwd.x * 6.5f + fwd.z * 2.0f };
            cam.target = { pos.x, pos.y + 0.7f, pos.z };
            cam.up = { 0.0f, 1.0f, 0.0f };
            cam.fovy = 40.0f;
            for (int f = 0; f < 3; f++) engine.StepFrame(1.0f / 60.0f);
            TakeScreenshot("city_car_blink.png");
        }
        c.GetParams().cars = 0;
    }

    // Buses: auto-generated transit, run the sim for a while, then a close-up of a moving bus and of a stop.
    {
        gfx::SetTimeOfDay(12.0f);
        c.GetParams().cars = 0;
        c.AutoTransit(3, 6);
        CHECK(c.CountInstances(kPropBusStop) > 0);     // stops are data now: generated transit places the shelters
        SetTrafficRunning(true);                      // keep the agents alive through the engine updates
        for (int i = 0; i < 1200; i++) c.TrafficStepForTest(0.05f);
        Vector3 pos; float yaw;
        CHECK(c.FindBus(1, pos, yaw));
        if (c.FindBus(1, pos, yaw)) {
            const Vector3 fwd = { cosf(yaw), 0.0f, -sinf(yaw) };            // the bus drives along local +x
            Camera3D& cam = engine.GetCamera();
            std::printf("bus 1 at %.1f %.1f %.1f yaw %.2f\n", pos.x, pos.y, pos.z, yaw);
            cam.position = { pos.x + fwd.x * 12.0f + fwd.z * 4.0f, pos.y + 7.0f, pos.z + fwd.z * 12.0f - fwd.x * 4.0f };
            cam.target = { pos.x, pos.y + 1.2f, pos.z };
            cam.up = { 0.0f, 1.0f, 0.0f };
            cam.fovy = 60.0f;
            for (int f = 0; f < 8; f++) engine.StepFrame(1.0f / 60.0f);
            TakeScreenshot("city_bus.png");
        }
    }

    // Pedestrians: people walk, queue at bus stops and ride; a close-up of a queue at a stop.
    {
        c.GetParams().pedestrians = 140;
        for (int i = 0; i < 12000; i++) c.TrafficStepForTest(0.05f);
        const City::TrafficStats ps = c.GetTrafficStats();
        std::printf("pedestrians: %d walking, %d queued, %d on buses, %d trips\n", ps.peds, ps.queued, ps.riders, ps.pedTrips);
        CHECK(ps.pedTrips > 100);
        Vector3 pos; float yaw;
        bool found = false;
        for (int i = 0; i < 3000 && !found; i++) { c.TrafficStepForTest(0.05f); found = c.FindQueuedPed(0, pos, yaw); }
        CHECK(found);
        // Queued pedestrians must stand at a bus shelter.
        {
            const int shelters = c.CountInstances(kPropBusStop);
            int queuedSeen = 0, atShelter = 0;
            float worst = 0.0f;
            for (int q = 0; q < 40; q++) {
                Vector3 qp; float qy;
                if (!c.FindQueuedPed(q, qp, qy)) break;
                queuedSeen++;
                float best = 1e30f;
                for (int k = 0; k < shelters; k++) { Vector3 sp; float sy; if (c.FindInstance(kPropBusStop, k, sp, sy)) best = std::min(best, Vector2Distance({ qp.x, qp.z }, { sp.x, sp.z })); }
                worst = std::max(worst, best);
                if (best < 4.0f) atShelter++;
            }
            std::printf("queued pedestrians: %d, %d within 4 m of a shelter (worst %.1f m), %d shelters\n", queuedSeen, atShelter, worst, shelters);
            CHECK(queuedSeen == 0 || atShelter == queuedSeen);
        }
        if (found) {
            Camera3D& cam = engine.GetCamera();
            const Vector3 fwd = { cosf(yaw), 0.0f, -sinf(yaw) };
            cam.position = { pos.x + fwd.x * -5.0f - fwd.z * 7.0f, pos.y + 2.2f, pos.z + fwd.z * -5.0f + fwd.x * 7.0f };   // from the road side
            cam.target = { pos.x, pos.y + 1.0f, pos.z };
            cam.up = { 0.0f, 1.0f, 0.0f };
            cam.fovy = 55.0f;
            for (int f = 0; f < 8; f++) engine.StepFrame(1.0f / 60.0f);
            TakeScreenshot("city_ped_queue.png");
        }
    }

    std::printf(g_fail ? "city_props_test: %d FAILED\n" : "city_props_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
