// Terrain file readers/importers must reject corrupt or hostile input instead of crashing, dividing by zero or
// allocating gigabytes (corrupt headers, bad sizes, non-square RAW16, truncated blocks, NaN queries).
// Opens a window (engine ctor), so it is not registered with ctest: build/tests/terrain_io_fuzz_test
#include "raylib.h"
#include "Engine.hpp"
#include "../include/Terrain/BasicTerrain.hpp"
#include "../include/Terrain/Terrain.hpp"
#include "../include/Terrain/TerrainIO.hpp"
#include "../include/Terrain/TerrainRegistry.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

using namespace terrain;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void WriteBytes(const std::string& p, const std::string& b) { std::ofstream o(p, std::ios::binary); o.write(b.data(), (std::streamsize)b.size()); }
static void PutU32(std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); }
static void PutF32(std::string& s, float v) { s.append(reinterpret_cast<const char*>(&v), 4); }

int main() {
    Engine engine(320, 240, "terrain_io_fuzz_test", 60);
    engine.SetPlayerBuild(true);
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "flyengine_terrain_io_fuzz";
    std::filesystem::create_directories(dir);

    // --- Terrain payload: a valid round trip, then corrupt headers.
    {
        terrain::Terrain t({ 0, 0, 0 }, 128.0f, 128.0f, 64, 33);
        std::ostringstream good(std::ios::binary);
        CHECK(t.WriteToStream(good));
        const std::string bytes = good.str();
        { terrain::Terrain r({ 0, 0, 0 }, 128.0f, 128.0f, 64, 33); std::istringstream in(bytes, std::ios::binary); CHECK(r.ReadFromStream(in)); }

        TerrainHeader h;
        std::memcpy(&h, bytes.data(), sizeof(h));
        auto tryHeader = [&](TerrainHeader bad) {
            std::string b = bytes;
            std::memcpy(&b[0], &bad, sizeof(bad));
            terrain::Terrain r({ 0, 0, 0 }, 128.0f, 128.0f, 64, 33);
            std::istringstream in(b, std::ios::binary);
            return r.ReadFromStream(in);
        };
        TerrainHeader a = h; a.chunkResolution = 0;           CHECK(!tryHeader(a));
        a = h; a.chunkResolution = 1;                         CHECK(!tryHeader(a));
        a = h; a.chunkResolution = 0x7fffffffu;               CHECK(!tryHeader(a));
        a = h; a.chunkWorldSize = 0.0f;                       CHECK(!tryHeader(a));
        a = h; a.chunkWorldSize = NAN;                        CHECK(!tryHeader(a));
        a = h; a.sizeX = 1e30f;                               CHECK(!tryHeader(a));
        a = h; a.layerCount = 0xffffffffu;                    CHECK(!tryHeader(a));

        // Chunk record whose size disagrees with the chunk's resolution.
        std::string b = bytes;
        const size_t chunkAt = sizeof(TerrainHeader) + h.layerCount * sizeof(TerrainLayerHeader);
        TerrainChunkHeader ch;
        std::memcpy(&ch, b.data() + chunkAt, sizeof(ch));
        ch.originalSize = 0xfffffff0u;
        std::memcpy(&b[chunkAt], &ch, sizeof(ch));
        terrain::Terrain r({ 0, 0, 0 }, 128.0f, 128.0f, 64, 33);
        std::istringstream in(b, std::ios::binary);
        CHECK(!r.ReadFromStream(in));
    }

    // --- Heightmap importers.
    {
        terrain::Terrain t({ 0, 0, 0 }, 128.0f, 128.0f, 64, 33);
        HeightmapImportSettings s;
        const std::string raw = (dir / "h.r16").string();
        WriteBytes(raw, std::string(2 * 16 * 16, '\x40'));
        CHECK(terrain::ImportRaw16(t, raw, s));
        WriteBytes(raw, std::string(2 * 16 * 16 + 1, '\x40'));       // odd byte count
        (void)terrain::ImportRaw16(t, raw, s);                       // trailing byte ignored; must not overflow
        WriteBytes(raw, std::string(2 * 10 * 12, '\x40'));           // not square
        CHECK(!terrain::ImportRaw16(t, raw, s));
        CHECK(!terrain::LoadHeightmap(t, raw, s));                   // used to read past a 1024x1024 image
        WriteBytes(raw, "");                                         // empty
        CHECK(!terrain::ImportRaw16(t, raw, s));
        WriteBytes(raw, std::string(2 * 16 * 16, '\x40'));
        s.targetResolution = 1;                                      // divided by zero while resampling
        CHECK(!terrain::ImportRaw16(t, raw, s));
        s = HeightmapImportSettings{}; s.targetChunkSize = 0;
        CHECK(!terrain::ImportRaw16(t, raw, s));
        s = HeightmapImportSettings{}; s.worldWidth = NAN;
        CHECK(!terrain::ImportRaw16(t, raw, s));
    }

    // --- terrain.terrain block with an absurd size.
    {
        TerrainFileHeader fh; fh.terrainCount = 1;
        std::string f(reinterpret_cast<const char*>(&fh), sizeof(fh));
        TerrainBlockHeader bh; bh.terrainIndex = 1; bh.nameLen = 0xfffffff0u; bh.dataSize = 0xfffffff0u;
        f.append(reinterpret_cast<const char*>(&bh), sizeof(bh));
        const std::string p = (dir / "terrain.terrain").string();
        WriteBytes(p, f);
        int created = 0;
        CHECK(terrain::GetTerrainRegistry().ReadFile(p, [&]() -> terrain::Terrain* { created++; return nullptr; }) == 0);
        CHECK(created == 0);
    }

    // --- BasicTerrain: out-of-range queries, corrupt deflate block size.
    {
        BasicTerrain bt(8, 8, 1.0f, 10.0f, 1.0f);
        CHECK(std::isfinite(bt.GetHeightAt(NAN, 0.0f)));
        CHECK(std::isfinite(bt.GetHeightAt(0.0f, INFINITY)));

        std::string f("BTTR", 4);
        PutU32(f, 1); PutU32(f, 1);
        PutU32(f, 0);                                   // empty name
        PutU32(f, 8); PutU32(f, 8);
        PutF32(f, 1.0f); PutF32(f, 0.0f); PutF32(f, 10.0f); PutF32(f, 1.0f);
        for (int i = 0; i < 3; i++) PutF32(f, 0.0f);    // position
        for (int i = 0; i < 6; i++) PutF32(f, 0.0f);    // gen box
        PutU32(f, 0);                                   // seed
        f.append(4, '\0');                              // flags
        f.push_back(1); PutU32(f, 0xfffffff0u);         // deflated block claiming ~4 GB
        const std::string p = (dir / "bad.bt").string();
        WriteBytes(p, f);
        BasicTerrain victim(4, 4, 1.0f, 10.0f, 1.0f);
        CHECK(!victim.LoadFromFile(p));
    }

    std::filesystem::remove_all(dir);
    std::printf(g_fail ? "terrain_io_fuzz_test: %d FAILED\n" : "terrain_io_fuzz_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
