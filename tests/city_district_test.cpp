// Districts: a downtown raises building heights towards its centre, an auto downtown
// sits in the middle, painting overrides a block, and everything survives a save/load.
#include "raylib.h"
#include "../include/CityGen/City.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>

using namespace city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static float MaxHeightNear(const City& c, Vector2 p, float r) {
    float h = 0.0f;
    for (const Block& b : c.GetBlocks())
        for (const Building& bl : b.buildings)
            if (Vector2Distance({ bl.center.x, bl.center.z }, p) < r) h = std::max(h, bl.size.y);
    return h;
}

static float AvgHeight(const City& c) {
    double s = 0; int n = 0;
    for (const Block& b : c.GetBlocks()) for (const Building& bl : b.buildings) { s += bl.size.y; n++; }
    return n ? (float)(s / n) : 0.0f;
}

int main() {
    InitWindow(256, 256, "city_district_test");
    City c;
    c.GetParams().gridX = 8; c.GetParams().gridZ = 8;
    c.GetParams().heightVariance = 0.0f;
    c.GetParams().shortChance = 0.0f;
    c.GenerateGrid({ 0.0f, 0.0f });
    const float base = AvgHeight(c);
    CHECK(base > 0.0f);
    CHECK(c.GetDistricts().empty());

    c.AutoDistricts(3.0f);
    CHECK(c.GetDistricts().size() == 1);
    const District d0 = c.GetDistricts()[0];
    const float centre = MaxHeightNear(c, d0.pos, d0.radius * 0.25f);
    const float rim = MaxHeightNear(c, { d0.pos.x + d0.radius * 0.95f, d0.pos.y }, d0.radius * 0.15f);
    std::printf("base avg %.1f  centre max %.1f  rim max %.1f  (district at %.1f,%.1f r=%.1f)\n", base, centre, rim, d0.pos.x, d0.pos.y, d0.radius);
    CHECK(centre > base * 1.8f);
    CHECK(centre > rim * 1.5f);

    // Dragging the downtown moves the tall buildings with it.
    c.MoveDistrict(0, { d0.pos.x + 100.0f, d0.pos.y });
    CHECK(MaxHeightNear(c, { d0.pos.x + 100.0f, d0.pos.y }, 20.0f) > MaxHeightNear(c, d0.pos, 20.0f));

    // A second district (suburb) switches style and lowers buildings.
    c.AddDistrict(City::MakeDistrict(DistrictKind::Suburb, { d0.pos.x - 100.0f, d0.pos.y }));
    CHECK(c.GetDistricts().size() == 2);
    bool sawSuburb = false;
    for (const Block& b : c.GetBlocks()) for (const Building& bl : b.buildings) if (bl.style == 3) sawSuburb = true;
    CHECK(sawSuburb);

    // Painting a block takes the district's style.
    const uint64_t id = c.GetBlocks()[0].id;
    c.PaintBlockDistrict(id, 1);
    CHECK(c.GetBlockDistrict(id) == 1);
    for (const Block& b : c.GetBlocks()) if (b.id == id) for (const Building& bl : b.buildings) CHECK(bl.style == 3);

    // Save / load round trip.
    std::stringstream ss;
    CHECK(c.WriteToStream(ss));
    City c2;
    CHECK(c2.ReadFromStream(ss));
    CHECK(c2.GetDistricts().size() == 2);
    CHECK(c2.GetBlockDistrict(id) == 1);
    CHECK(std::fabs(AvgHeight(c2) - AvgHeight(c)) < 0.01f);

    c.RemoveDistrict(1);
    CHECK(c.GetDistricts().size() == 1 && c.GetBlockDistrict(id) == -1);

    std::printf(g_fail ? "city_district_test: %d FAILED\n" : "city_district_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
