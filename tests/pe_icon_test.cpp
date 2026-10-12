// peicon::ReplaceExeIcon on a synthetic PE: a header plus a .rsrc section holding two 32-bit icon slots (16 and 32 px)
// and their group. Checks the slots are overwritten with the resized picture, nothing else in the file changes and the
// length is kept; and that files that are not usable players are refused. No window.
#include "Engine/PeIcon.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void P16(std::vector<uint8_t>& b, size_t o, unsigned v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); }
static void P32(std::vector<uint8_t>& b, size_t o, unsigned v) { P16(b, o, v & 0xFFFF); P16(b, o + 2, v >> 16); }

static const size_t kRaw = 0x400;
static const unsigned kVa = 0x1000;

// Section-relative layout (see the comments in the builder).
static void Dir(std::vector<uint8_t>& s, size_t at, unsigned nId) { P16(s, at + 14, nId); }
static void Ent(std::vector<uint8_t>& s, size_t at, unsigned i, unsigned id, unsigned off, bool dir) {
    P32(s, at + 16 + i * 8, id); P32(s, at + 20 + i * 8, off | (dir ? 0x80000000u : 0));
}

static std::vector<uint8_t> BuildPe(bool withResources, size_t& icon16Off, size_t& icon32Off) {
    const size_t dib16 = 40 + 16 * 16 * 4 + 4 * 16, dib32 = 40 + 32 * 32 * 4 + 4 * 32;
    std::vector<uint8_t> rs(0x108 + dib16 + dib32 + 0x10, 0);
    Dir(rs, 0x00, 2); Ent(rs, 0x00, 0, 3, 0x20, true); Ent(rs, 0x00, 1, 14, 0x80, true);        // root: RT_ICON, RT_GROUP_ICON
    Dir(rs, 0x20, 2); Ent(rs, 0x20, 0, 1, 0x50, true); Ent(rs, 0x20, 1, 2, 0x68, true);          // icons 1, 2
    Dir(rs, 0x50, 1); Ent(rs, 0x50, 0, 0x409, 0xB0, false);
    Dir(rs, 0x68, 1); Ent(rs, 0x68, 0, 0x409, 0xC0, false);
    Dir(rs, 0x80, 1); Ent(rs, 0x80, 0, 1, 0x98, true);                                          // group 1
    Dir(rs, 0x98, 1); Ent(rs, 0x98, 0, 0x409, 0xD0, false);
    icon16Off = 0x108; icon32Off = 0x108 + dib16;
    P32(rs, 0xB0, kVa + (unsigned)icon16Off); P32(rs, 0xB4, (unsigned)dib16);
    P32(rs, 0xC0, kVa + (unsigned)icon32Off); P32(rs, 0xC4, (unsigned)dib32);
    P32(rs, 0xD0, kVa + 0xE0);                P32(rs, 0xD4, 6 + 2 * 14);
    P16(rs, 0xE0 + 2, 1); P16(rs, 0xE0 + 4, 2);                                                  // GRPICONDIR: icon type, 2 images
    auto grp = [&](size_t e, unsigned size, size_t bytes, unsigned id) {
        rs[e] = (uint8_t)size; rs[e + 1] = (uint8_t)size; P16(rs, e + 4, 1); P16(rs, e + 6, 32); P32(rs, e + 8, (unsigned)bytes); P16(rs, e + 12, id);
    };
    grp(0xE0 + 6, 16, dib16, 1); grp(0xE0 + 6 + 14, 32, dib32, 2);
    std::memset(rs.data() + icon16Off, 0xAA, dib16 + dib32);                                     // placeholder pixels

    std::vector<uint8_t> f(kRaw + rs.size(), 0);
    f[0] = 'M'; f[1] = 'Z'; P32(f, 0x3C, 0x80);
    std::memcpy(&f[0x80], "PE\0\0", 4);
    P16(f, 0x84, 0x8664); P16(f, 0x86, 1); P16(f, 0x94, 240);
    P16(f, 0x98, 0x20B);
    if (withResources) { P32(f, 0x98 + 112 + 16, kVa); P32(f, 0x98 + 112 + 20, (unsigned)rs.size()); }
    const size_t sh = 0x98 + 240;
    std::memcpy(&f[sh], ".rsrc", 5);
    P32(f, sh + 8, (unsigned)rs.size()); P32(f, sh + 12, kVa); P32(f, sh + 16, (unsigned)rs.size()); P32(f, sh + 20, (unsigned)kRaw);
    std::memcpy(&f[kRaw], rs.data(), rs.size());
    icon16Off += kRaw; icon32Off += kRaw;
    return f;
}

static std::vector<uint8_t> Slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "fly_pe_icon_test";
    fs::remove_all(dir); fs::create_directories(dir);

    std::vector<uint8_t> src(64 * 64 * 4);
    for (size_t i = 0; i < src.size(); i += 4) { src[i] = 255; src[i + 1] = 0; src[i + 2] = 0; src[i + 3] = 255; }   // opaque red

    size_t o16 = 0, o32 = 0;
    const std::vector<uint8_t> original = BuildPe(true, o16, o32);
    const fs::path exe = dir / "p.exe";
    std::ofstream(exe, std::ios::binary).write((const char*)original.data(), (std::streamsize)original.size());

    std::string err;
    CHECK(peicon::ReplaceExeIcon(exe.string(), src.data(), 64, 64, err) == 2);
    const std::vector<uint8_t> patched = Slurp(exe);
    CHECK(patched.size() == original.size());
    std::string d16, d32;
    peicon::EncodeIconDib(src.data(), 64, 64, 16, d16);
    peicon::EncodeIconDib(src.data(), 64, 64, 32, d32);
    CHECK(std::memcmp(&patched[o16], d16.data(), d16.size()) == 0);
    CHECK(std::memcmp(&patched[o32], d32.data(), d32.size()) == 0);
    CHECK(patched[o16 + 40] == 0 && patched[o16 + 41] == 0 && patched[o16 + 42] == 255 && patched[o16 + 43] == 255);   // BGRA of red
    size_t diff = 0, inSlots = 0;
    for (size_t i = 0; i < original.size(); ++i) {
        if (original[i] != patched[i]) { ++diff; if ((i >= o16 && i < o16 + d16.size()) || (i >= o32 && i < o32 + d32.size())) ++inSlots; }
    }
    CHECK(diff == inSlots && diff > 0);   // every changed byte is inside an icon slot

    // Not usable: no resource directory; not a PE at all.
    const std::vector<uint8_t> bare = BuildPe(false, o16, o32);
    std::ofstream(dir / "bare.exe", std::ios::binary).write((const char*)bare.data(), (std::streamsize)bare.size());
    CHECK(peicon::ReplaceExeIcon((dir / "bare.exe").string(), src.data(), 64, 64, err) == 0 && !err.empty());
    std::ofstream(dir / "text.exe") << "not an executable at all, just text padding padding padding padding padding";
    CHECK(peicon::ReplaceExeIcon((dir / "text.exe").string(), src.data(), 64, 64, err) == 0);

    fs::remove_all(dir);
    if (g_fail == 0) std::printf("pe_icon_test: OK\n");
    return g_fail ? 1 : 0;
}
