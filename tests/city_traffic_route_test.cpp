// Routed traffic: cars pick destinations and reach them (trips complete), shortest-route fields are
// complete on a connected grid, and rush hours change the number of cars. Runs the real City traffic
// step; needs a display (mesh upload), so it is not registered with ctest:
//   build/tests/city_traffic_route_test
#include "raylib.h"
#include "../include/CityGen/City.hpp"

#include <cmath>
#include <cstdio>

using namespace city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static int CountCars(City& c) {
    c.TrafficStepForTest(0.0f);
    return c.GetTrafficStats().cars;
}

int main() {
    InitWindow(320, 240, "city_traffic_route_test");
    City c;
    CityParams& p = c.GetParams();
    p.gridX = 8; p.gridZ = 8;
    p.cars = 24; p.pedestrians = 0;
    p.trafficDetailDistance = 0.0f;       // everyone in the detailed model
    p.routedTraffic = true;
    c.GenerateGrid({ 0.0f, 0.0f });

    // Route fields: zero at the destination, finite everywhere on a connected grid.
    const std::vector<int> dests = c.RouteDestinations();
    CHECK(!dests.empty());
    if (!dests.empty()) {
        const std::vector<float>& f = c.RouteField(dests[0]);
        CHECK(f[(size_t)dests[0]] == 0.0f);
        int unreachable = 0;
        for (size_t i = 0; i < f.size(); i++) if (!std::isfinite(f[i])) unreachable++;
        std::printf("destinations: %zu, unreachable nodes from the first: %d of %zu\n", dests.size(), unreachable, f.size());
        CHECK(unreachable == 0);
    }

    // Drive for 5 simulated minutes: cars must complete trips.
    for (int i = 0; i < 6000; i++) c.TrafficStepForTest(0.05f);
    const City::TrafficStats st = c.GetTrafficStats();
    std::printf("cars %d, trips completed in 300 s: %d, avg speed %.1f m/s, stopped %d\n", st.cars, st.trips, st.avgSpeed, st.stopped);
    CHECK(st.cars == 24);
    CHECK(st.trips >= 24);                  // on average every car finished at least one trip

    // Rush hours: fewer cars at 03:00 than at 08:00, changing gradually.
    c.GetParams().rushHours = true;
    c.GetParams().timeOfDay = 3.0f;
    for (int i = 0; i < 100; i++) c.TrafficStepForTest(0.05f);
    const int night = CountCars(c);
    c.GetParams().timeOfDay = 8.0f;
    for (int i = 0; i < 100; i++) c.TrafficStepForTest(0.05f);
    const int rush = CountCars(c);
    std::printf("rush hours: %d cars at 03:00, %d at 08:00 (of 24)\n", night, rush);
    CHECK(night < rush);
    CHECK(rush == 24);

    std::printf(g_fail ? "city_traffic_route_test: %d FAILED\n" : "city_traffic_route_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
