// Public transit: auto-generated lines (stops on roads, every stop resolved), manually placed stops and lines,
// buses that actually serve their stops while the traffic sim runs, and a save/load round trip. Runs the real
// City code; needs a display (mesh upload), so it is not registered with ctest: build/tests/city_transit_test
#include "raylib.h"
#include "../include/CityGen/City.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>

using namespace city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void Run(City& c, float seconds) {
    for (int i = 0; i < (int)(seconds / 0.05f); i++) c.TrafficStepForTest(0.05f);
}

int main() {
    InitWindow(320, 240, "city_transit_test");

    // ---- auto generation ----
    City a;
    a.GetParams().gridX = 10; a.GetParams().gridZ = 10;
    a.GetParams().cars = 0; a.GetParams().pedestrians = 0;
    a.GenerateGrid({ 0.0f, 0.0f });
    a.AutoTransit(3, 6);
    std::printf("auto: %zu stops, %zu lines\n", a.GetBusStops().size(), a.GetBusLines().size());
    CHECK(a.GetBusLines().size() >= 2);
    for (const BusLine& l : a.GetBusLines()) {
        CHECK(l.stops.size() >= 2);
        for (int s : l.stops) CHECK(s >= 0 && (size_t)s < a.GetBusStops().size());
    }
    for (const BusStop& s : a.GetBusStops()) CHECK(s.edge >= 0);
    int wantBuses = 0;
    for (const BusLine& l : a.GetBusLines()) wantBuses += l.buses;
    Run(a, 600.0f);
    const City::TrafficStats st = a.GetTrafficStats();
    std::printf("auto: %d buses running, %d stop visits in 600 s\n", st.buses, st.busStopsServed);
    CHECK(st.buses == wantBuses);
    CHECK(st.busStopsServed >= wantBuses * 3);          // every bus served several stops

    // ---- manual ----
    City m;
    m.GetParams().gridX = 6; m.GetParams().gridZ = 6;
    m.GetParams().cars = 0; m.GetParams().pedestrians = 0;
    m.GenerateGrid({ 0.0f, 0.0f });
    const Vector2 c0 = { m.GetNodes()[0].pos.x, m.GetNodes()[0].pos.y };
    (void)c0;
    // Two stops on the first road of the grid interior, both directions' kerbs by clicking each side.
    const RoadEdge& e = m.GetEdges()[m.GetEdges().size() / 2];
    const Vector2 pa = m.GetNodes()[(size_t)e.a].pos, pb = m.GetNodes()[(size_t)e.b].pos;
    const Vector2 mid = { (pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f };
    const Vector2 ab = { (pb.x - pa.x) / std::max(Vector2Distance(pa, pb), 1e-3f), (pb.y - pa.y) / std::max(Vector2Distance(pa, pb), 1e-3f) };
    const Vector2 rightOfAB = { ab.y, -ab.x };           // right of a -> b
    Vector2 pos, heading;
    CHECK(m.SnapBusStop({ mid.x + rightOfAB.x * 3.0f, mid.y + rightOfAB.y * 3.0f }, pos, heading));
    CHECK(Vector2DotProduct(heading, ab) > 0.9f);       // the right kerb serves travel a -> b (right-hand traffic)
    const int s0 = m.AddBusStop(pos, heading);
    CHECK(m.SnapBusStop({ mid.x - rightOfAB.x * 3.0f, mid.y - rightOfAB.y * 3.0f }, pos, heading));
    CHECK(Vector2DotProduct(heading, ab) < -0.9f);      // the other kerb serves b -> a
    const int s1 = m.AddBusStop(pos, heading);
    CHECK(s0 == 0 && s1 == 1);
    BusLine line; line.name = "Shuttle"; line.buses = 1;
    const int li = m.AddBusLine(line);
    m.AddStopToLine(li, s0);
    m.AddStopToLine(li, s1);
    CHECK(m.GetBusLines()[0].stops.size() == 2);
    Run(m, 400.0f);
    const City::TrafficStats ms = m.GetTrafficStats();
    std::printf("manual: %d bus, %d stop visits in 400 s\n", ms.buses, ms.busStopsServed);
    CHECK(ms.buses == 1);
    CHECK(ms.busStopsServed >= 2);

    // ---- save / load ----
    std::stringstream ss;
    CHECK(a.WriteToStream(ss));
    City r;
    CHECK(r.ReadFromStream(ss));
    CHECK(r.GetBusStops().size() == a.GetBusStops().size());
    CHECK(r.GetBusLines().size() == a.GetBusLines().size());
    if (r.GetBusLines().size() == a.GetBusLines().size() && !a.GetBusLines().empty()) {
        CHECK(r.GetBusLines()[0].stops == a.GetBusLines()[0].stops);
        CHECK(r.GetBusLines()[0].color.r == a.GetBusLines()[0].color.r);
    }
    for (const BusStop& s : r.GetBusStops()) CHECK(s.edge >= 0);   // re-attached to the roads after loading

    // ---- editing ----
    m.RemoveBusStop(0);
    CHECK(m.GetBusStops().size() == 1 && m.GetBusLines()[0].stops.size() == 1);

    std::printf(g_fail ? "city_transit_test: %d FAILED\n" : "city_transit_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
