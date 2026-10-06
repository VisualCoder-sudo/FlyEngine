// BasicTerrainIO.cpp -- project persistence for BasicTerrain.
//
// Why this file exists
// --------------------
// BasicTerrain used to have no persistence at all. It is an Entity but not a
// ScatteredObject, and it never called terrain::GetTerrainRegistry().Register(),
// so it appeared in none of the six counts that SaveSceneToStream writes. A
// project containing only a BasicTerrain serialized to all zeros and the
// terrain vanished on reopen. LoadHeightmap/SaveHeightmap existed but had zero
// callers, and they are 8-bit grayscale PNG-style exports -- fine for "save a
// heightmap for another tool", wrong for persistence because they quantise the
// height range to 256 steps and drop the splatmap entirely.
//
// Format: one sidecar holding every BasicTerrain in the scene, written as
//
//   "BTTR"  u32 version(=1)  u32 count
//   count x { header, deflated heightmap, deflated splatmap }
//
// The heightmap is stored as raw little-endian float32 rather than the 8-bit
// image Load/SaveHeightmap uses, so a save/load cycle is bit-exact. Both
// payloads are deflated with miniz (already vendored), which is lossless and
// compresses this data well: heights are locally smooth and the splatmap is
// mostly runs of a single layer. A block only stores the compressed form if it
// actually came out smaller than the raw bytes -- on incompressible data (a
// noise heightmap) we keep the raw bytes rather than inflate the file.
//
// Field order is explicit rather than a memcpy'd struct so the file does not
// depend on struct padding or the compiler's layout. Every scalar is
// little-endian, written byte by byte.

#include "../../include/Terrain/BasicTerrain.hpp"

#include "miniz.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <istream>
#include <ostream>
#include <vector>

namespace {

constexpr char     kMagic[4]   = {'B', 'T', 'T', 'R'};
constexpr uint32_t kVersion    = 1;

// Guards the decompressor against a corrupt or truncated file claiming an
// absurd uncompressed size. At kSplatMaxDim^2 * 4 bytes the splatmap is the
// largest legitimate payload (~16 MB), so anything past this is a bad header
// and we would rather fail than try to allocate it.
constexpr size_t kMaxPayloadBytes = 256u * 1024u * 1024u;

// ---------------------------------------------------------------- primitives

void WriteU32(std::ostream& os, uint32_t v) {
    const unsigned char b[4] = {
        (unsigned char)(v & 0xFFu),
        (unsigned char)((v >> 8) & 0xFFu),
        (unsigned char)((v >> 16) & 0xFFu),
        (unsigned char)((v >> 24) & 0xFFu)};
    os.write(reinterpret_cast<const char*>(b), 4);
}

void WriteI32(std::ostream& os, int32_t v) {
    WriteU32(os, static_cast<uint32_t>(v));
}

// Height/scale are stored as float32, so this is an exact round-trip for every
// value the editor can produce -- no text formatting, no 9-significant-digit
// truncation the way the text scene format would introduce.
void WriteF32(std::ostream& os, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    WriteU32(os, bits);
}

void WriteU8(std::ostream& os, uint8_t v) {
    os.write(reinterpret_cast<const char*>(&v), 1);
}

void WriteString(std::ostream& os, const std::string& s) {
    WriteU32(os, static_cast<uint32_t>(s.size()));
    os.write(s.data(), static_cast<std::streamsize>(s.size()));
}

bool ReadU32(std::istream& is, uint32_t& v) {
    unsigned char b[4] = {0, 0, 0, 0};
    if (!is.read(reinterpret_cast<char*>(b), 4)) return false;
    v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
        (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    return true;
}

bool ReadI32(std::istream& is, int32_t& v) {
    uint32_t u = 0;
    if (!ReadU32(is, u)) return false;
    v = static_cast<int32_t>(u);
    return true;
}

bool ReadF32(std::istream& is, float& f) {
    uint32_t bits = 0;
    if (!ReadU32(is, bits)) return false;
    std::memcpy(&f, &bits, sizeof(f));
    return true;
}

bool ReadU8(std::istream& is, uint8_t& v) {
    char c = 0;
    if (!is.read(&c, 1)) return false;
    v = static_cast<uint8_t>(c);
    return true;
}

bool ReadString(std::istream& is, std::string& s, size_t maxLen = 4096) {
    uint32_t len = 0;
    if (!ReadU32(is, len)) return false;
    if (len > maxLen) return false;
    s.resize(len);
    if (len == 0) return true;
    return static_cast<bool>(is.read(&s[0], static_cast<std::streamsize>(len)));
}

// ----------------------------------------------------------------- payloads
//
// Both blocks share one shape: a 1-byte flag saying raw or deflated, then the
// byte count, then the bytes. Only the destination differs.

void WriteBlock(std::ostream& os, const void* data, size_t size) {
    const unsigned char* bytes = static_cast<const unsigned char*>(data);

    const mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(size));
    std::vector<unsigned char> comp(bound ? bound : 1);
    mz_ulong compSize = bound;

    // Only keep the compressed form when it genuinely shrank the block. On
    // incompressible input deflate can expand by a few bytes, and storing that
    // would make the file larger for no gain.
    const bool compressible =
        size > 0 && mz_compress2(comp.data(), &compSize, bytes, static_cast<mz_ulong>(size),
                                 MZ_DEFAULT_COMPRESSION) == MZ_OK &&
        compSize < size;

    if (compressible) {
        WriteU8(os, 1);
        WriteU32(os, static_cast<uint32_t>(compSize));
        os.write(reinterpret_cast<const char*>(comp.data()), static_cast<std::streamsize>(compSize));
    } else {
        WriteU8(os, 0);
        WriteU32(os, static_cast<uint32_t>(size));
        if (size > 0) os.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    }
}

bool ReadBlock(std::istream& is, std::vector<unsigned char>& out, size_t expectedRawSize) {
    uint8_t compressed = 0;
    uint32_t stored = 0;
    if (!ReadU8(is, compressed)) return false;
    if (!ReadU32(is, stored)) return false;

    // A bad length here is the classic way a corrupt file turns into a huge
    // allocation, so validate before touching memory.
    const size_t bound = compressed ? mz_compressBound(static_cast<mz_ulong>(expectedRawSize))
                                    : expectedRawSize;
    if (bound > kMaxPayloadBytes) return false;

    out.assign(expectedRawSize, 0);
    if (expectedRawSize == 0) return true;

    if (!compressed) {
        if (stored != expectedRawSize) return false;
        std::vector<unsigned char> tmp(expectedRawSize);
        if (!is.read(reinterpret_cast<char*>(tmp.data()), static_cast<std::streamsize>(tmp.size()))) {
            return false;
        }
        std::memcpy(out.data(), tmp.data(), out.size());
        return true;
    }

    std::vector<unsigned char> comp(stored);
    if (!is.read(reinterpret_cast<char*>(comp.data()), static_cast<std::streamsize>(comp.size()))) {
        return false;
    }
    mz_ulong outSize = static_cast<mz_ulong>(expectedRawSize);
    if (mz_uncompress(out.data(), &outSize, comp.data(), static_cast<mz_ulong>(comp.size())) != MZ_OK) {
        return false;
    }
    // Deflate must produce exactly the declared size; a short result means the
    // header and payload disagree.
    return outSize == expectedRawSize;
}

// ------------------------------------------------------------ per-instance

void WriteInstance(std::ostream& os, const BasicTerrain& t) {
    WriteString(os, t.GetName());

    WriteI32(os, t.GetWidth());
    WriteI32(os, t.GetDepth());
    WriteF32(os, t.GetScale());
    WriteF32(os, t.GetMinHeight());
    WriteF32(os, t.GetMaxHeight());
    WriteF32(os, t.GetTextureTiling());

    WriteF32(os, t.position.x);
    WriteF32(os, t.position.y);
    WriteF32(os, t.position.z);

    // Procedural-generation parameters. These are editor state rather than
    // mesh data, but they are what the Generate panel shows when the terrain is
    // reselected, so a round-trip that dropped them would look like the panel
    // reset itself.
    WriteF32(os, t.genBoxPos.x);
    WriteF32(os, t.genBoxPos.y);
    WriteF32(os, t.genBoxPos.z);
    WriteF32(os, t.genBoxSize.x);
    WriteF32(os, t.genBoxSize.y);
    WriteF32(os, t.genBoxSize.z);
    WriteI32(os, t.genSeed);
    WriteU8(os, t.genHills ? 1 : 0);
    WriteU8(os, t.genMountains ? 1 : 0);
    WriteU8(os, t.genTextures ? 1 : 0);
    WriteU8(os, t.showWireframe ? 1 : 0);

    WriteBlock(os, t.GetHeightData(), t.GetHeightBytes());
    WriteBlock(os, t.GetSplatData(), t.GetSplatBytes());
}

} // namespace

// ------------------------------------------------------- instance entry points

bool BasicTerrain::SaveToFile(const std::string& path) const {
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) return false;

    os.write(kMagic, 4);
    WriteU32(os, kVersion);
    WriteU32(os, 1); // count
    WriteInstance(os, *this);

    os.flush();
    return os.good();
}

bool BasicTerrain::LoadFromFile(const std::string& path) {
    std::ifstream is(path, std::ios::binary);
    if (!is) return false;

    char magic[4] = {0, 0, 0, 0};
    if (!is.read(magic, 4)) return false;
    if (std::memcmp(magic, kMagic, 4) != 0) return false;

    uint32_t version = 0, count = 0;
    if (!ReadU32(is, version) || !ReadU32(is, count)) return false;
    if (version != kVersion || count == 0) return false;

    return ReadInstance(is, *this);
}

bool BasicTerrain::WriteCollection(const std::string& path) {
    const std::vector<BasicTerrain*>& all = s_instances;

    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) return false;

    os.write(kMagic, 4);
    WriteU32(os, kVersion);
    WriteU32(os, static_cast<uint32_t>(all.size()));
    for (BasicTerrain* t : all) {
        if (t) WriteInstance(os, *t);
    }

    os.flush();
    return os.good();
}

int BasicTerrain::ReadCollection(const std::string& path,
                                 const std::function<BasicTerrain*()>& makeInstance) {
    if (!makeInstance) return 0;

    std::ifstream is(path, std::ios::binary);
    if (!is) return 0;

    char magic[4] = {0, 0, 0, 0};
    if (!is.read(magic, 4)) return 0;
    if (std::memcmp(magic, kMagic, 4) != 0) return 0;

    uint32_t version = 0, count = 0;
    if (!ReadU32(is, version) || !ReadU32(is, count)) return 0;
    if (version != kVersion) return 0;
    // Each terrain costs at least a few bytes on disk; a count far above that
    // is a corrupt header rather than a real scene.
    if (count > 4096) return 0;

    // A truncated file is not fatal: keep whatever parsed cleanly and report the
    // count. That matches how the city loader behaves (it keeps going and logs),
    // and it means one bad record does not throw away the terrains before it.
    int loaded = 0;
    for (uint32_t i = 0; i < count; ++i) {
        BasicTerrain* raw = makeInstance();
        if (!raw) break;
        if (!ReadInstance(is, *raw)) break;
        ++loaded;
    }
    return loaded;
}

void BasicTerrain::Reshape(int newWidth, int newDepth, float newScale,
                           float newMinHeight, float newMaxHeight, float newTiling) {
    // Tear down any GPU state built for the previous dimensions. Loader target
    // instances are freshly constructed (mesh == {0}), but being safe here
    // costs one null check and makes Reshape callable on a live terrain.
    if (mesh.vertices) { UnloadMesh(mesh); mesh = {0}; }
    if (model.meshes != NULL) { RL_FREE(model.meshes); model.meshes = NULL; }
    if (terrainTexture.id > 0) { UnloadTexture(terrainTexture); terrainTexture = {0}; }
    if (splatmapTexture.id > 0) { UnloadTexture(splatmapTexture); splatmapTexture = {0}; }
    if (paintColorTexture.id > 0) { UnloadTexture(paintColorTexture); paintColorTexture = {0}; }

    width = newWidth;
    depth = newDepth;
    scale = newScale;
    maxHeight = newMaxHeight;
    minHeight = newMinHeight;
    textureTiling = newTiling;

    heightmap.assign((size_t)width * depth, 0.0f);
    splatWidth = std::min(width * kSplatSupersample, kSplatMaxDim);
    splatDepth = std::min(depth * kSplatSupersample, kSplatMaxDim);
    splatmap.resize((size_t)splatWidth * splatDepth * 4, 0);
    MarkAllDirty();
}

bool BasicTerrain::ReadInstance(std::istream& is, BasicTerrain& t) {
    std::string name;
    if (!ReadString(is, name)) return false;
    t.SetName(name);

    int32_t w = 0, d = 0;
    float scale = 0.0f, minH = 0.0f, maxH = 0.0f, tiling = 0.0f;
    if (!ReadI32(is, w) || !ReadI32(is, d)) return false;
    if (!ReadF32(is, scale) || !ReadF32(is, minH) || !ReadF32(is, maxH) || !ReadF32(is, tiling)) return false;

    // Bounds-check before allocating: a corrupt width/depth would otherwise ask
    // for an arbitrary reserve(). kSplatMaxDim already caps the splatmap, and
    // the heightmap gets the same treatment.
    if (w < 1 || d < 1 || w > 8192 || d > 8192) return false;
    if (!(scale > 0.0f)) return false;
    if (!(maxH > minH)) return false;

    Vector3 pos{};
    if (!ReadF32(is, pos.x) || !ReadF32(is, pos.y) || !ReadF32(is, pos.z)) return false;

    Vector3 boxPos{}, boxSize{};
    int32_t seed = 0;
    uint8_t hills = 0, mountains = 0, textures = 0, wireframe = 0;
    if (!ReadF32(is, boxPos.x) || !ReadF32(is, boxPos.y) || !ReadF32(is, boxPos.z)) return false;
    if (!ReadF32(is, boxSize.x) || !ReadF32(is, boxSize.y) || !ReadF32(is, boxSize.z)) return false;
    if (!ReadI32(is, seed)) return false;
    if (!ReadU8(is, hills) || !ReadU8(is, mountains) || !ReadU8(is, textures) || !ReadU8(is, wireframe)) {
        return false;
    }

    t.Reshape(w, d, scale, minH, maxH, tiling);

    t.genBoxPos = boxPos;
    t.genBoxSize = boxSize;
    t.genSeed = seed;
    t.genHills = hills != 0;
    t.genMountains = mountains != 0;
    t.genTextures = textures != 0;
    t.showWireframe = wireframe != 0;

    std::vector<unsigned char> buf;
    if (!ReadBlock(is, buf, (size_t)w * (size_t)d * sizeof(float))) return false;
    if (!buf.empty()) std::memcpy(t.GetHeightData(), buf.data(), buf.size());

    // The splatmap size follows from w/d via the supersample factor, so it is
    // not stored -- it cannot disagree with the header.
    if (!ReadBlock(is, buf, t.GetSplatBytes())) return false;
    if (!buf.empty()) std::memcpy(t.GetSplatData(), buf.data(), buf.size());

    t.position = pos;
    t.MarkAllDirty();
    return true;
}