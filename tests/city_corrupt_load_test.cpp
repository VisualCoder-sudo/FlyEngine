// Corrupt city data must be rejected, not crash or allocate gigabytes: huge counts, edges that name missing
// nodes, a lying sidecar block size. Also: destroying a City removes it from the registry. No window needed
// (every case is rejected before any mesh is built).
#include "../include/CityGen/City.hpp"
#include "../include/CityGen/CityGeometry.hpp"
#include <cmath>
#include <limits>

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static const char* kHead = "0\n\n2 2 10\n0 0.5 0.1 3 1\n2 3 2\n8 2 10 2\n0.5 3\n";

static bool Reads(const std::string& body) {
    City c;
    std::istringstream in(std::string(kHead) + body);
    return c.ReadFromStream(in);
}

static void WriteSidecar(const char* path, unsigned dataSize, const std::string& data) {
    std::ofstream o(path, std::ios::binary);
    o.write("FLYCITY1", 8);
    unsigned v[2] = { 1, 1 };
    o.write((const char*)v, 8);
    unsigned bh[2] = { 0, dataSize };
    o.write((const char*)bh, 8);
    o.write(data.data(), (std::streamsize)data.size());
}

int main() {
    CHECK(!Reads("99999999999\n"));                       // edge count far beyond the data
    CHECK(!Reads("18446744073709551615\n"));              // "-1" style wrap
    CHECK(!Reads("1\n0 5 1\n2\n0 0 0\n1 0 0\n"));         // edge b=5 but only 2 nodes
    CHECK(!Reads("1\n-1 0 1\n2\n0 0 0\n1 0 0\n"));        // negative node index
    CHECK(!Reads("0\n99999999999\n"));                    // node count
    CHECK(!Reads("1\n0 0 1\n2\n0 0 0\n1 0 0\n"));         // self-loop edge
    CHECK(!Reads("1\n0 1 1\n2\n0 0 0\n1e30 0 0\n"));      // absurd node coordinate
    CHECK(!Reads("1\n0 1 1\n2\n0 0 0\n-1e9 0 0\n"));
    {   // a failed parse leaves the city empty, not half-loaded
        City c;
        std::istringstream in(std::string(kHead) + "1\n0 1 1\n2\n0 0 0\n1 0 0\nBLOCKS 0 1\nOVERRIDES 0\nSIGNALS 1\n0 1.5 1 0 1\n");
        CHECK(!c.ReadFromStream(in));
        CHECK(c.GetNodes().empty() && c.GetEdges().empty());
    }
    {   // SanitizeParams: NaN/inf/huge values land in safe ranges
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        CityParams p;
        p.cellSize = nan; p.laneWidth = inf; p.sidewalk = -5; p.buildingSize = 0; p.buildingGap = nan;
        p.avgHeight = nan; p.organicStrength = nan; p.noiseOctaves = 1000000; p.gridX = -4; p.gridZ = 1 << 30;
        p.parkInset = -5.0f; p.heightVariance = 99;
        SanitizeParams(p);
        CHECK(std::isfinite(p.cellSize) && p.cellSize >= 5.0f);
        CHECK(std::isfinite(p.laneWidth) && p.laneWidth <= 8.0f);
        CHECK(p.sidewalk == 0.0f && p.buildingSize >= 1.0f && std::isfinite(p.buildingGap));
        CHECK(std::isfinite(p.avgHeight) && std::isfinite(p.organicStrength));
        CHECK(p.noiseOctaves <= 8 && p.gridX == 2 && p.gridZ == 999);
        CHECK(p.parkInset == 0.0f && p.heightVariance == 1.0f);
    }
    {   // geometry helpers reject NaN instead of returning NaN polygons / allocating gigabytes
        const float nan = std::numeric_limits<float>::quiet_NaN();
        std::vector<Vector2> sq = { {0,0}, {10,0}, {10,10}, {0,10} }, out;
        CHECK(!citygeom::InsetPolygon(sq, nan, out));
        CHECK(citygeom::InsetPolygon(sq, 1.0f, out));
        auto pts = citygeom::SampleQuadBezier({0,0}, {1e30f, 1e30f}, {10,0}, 1.0f);
        CHECK(pts.size() <= 2049);
        pts = citygeom::SampleQuadBezier({0,0}, {5,5}, {10,0}, 0.0f);
        CHECK(pts.size() <= 2049);
        Vector2 w = citygeom::OrganicWarp({1,1}, nan, 40.0f, 3, 1);
        CHECK(std::isfinite(w.x) && std::isfinite(w.y));
    }
    {
        City c;
        std::istringstream in("99999999999\nabc");        // name length
        CHECK(!c.ReadFromStream(in));
    }
    {   // sidecar whose block header claims ~4 GB of payload
        const char* path = "city_corrupt_load_test.city";
        WriteSidecar(path, 0xF0000000u, "xyz");
        City* keep = nullptr;
        int n = GetCityRegistry().ReadFile(path, [&]() -> City* { keep = new City(); return keep; });
        CHECK(n == 0);
        delete keep;
        std::remove(path);
    }
    {   // failed block leaves the half-built entity marked dead
        const char* path = "city_corrupt_load_test2.city";
        const std::string body = std::string(kHead) + "1\n0 5 1\n2\n0 0 0\n1 0 0\n";
        WriteSidecar(path, (unsigned)body.size(), body);
        City* keep = nullptr;
        int n = GetCityRegistry().ReadFile(path, [&]() -> City* { keep = new City(); return keep; });
        CHECK(n == 0);
        CHECK(keep && !keep->alive);
        CHECK(GetCityRegistry().Count() == 0);
        delete keep;
        std::remove(path);
    }
    {   // registry never keeps a destroyed City
        City* c = new City();
        GetCityRegistry().Register(c);
        CHECK(GetCityRegistry().Count() == 1);
        delete c;
        CHECK(GetCityRegistry().Count() == 0);
    }
    std::printf(g_fail ? "city_corrupt_load FAILED\n" : "city_corrupt_load OK\n");
    return g_fail ? 1 : 0;
}
