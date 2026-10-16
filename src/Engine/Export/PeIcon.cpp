#include "../../../include/Engine/PeIcon.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include "Engine/U8Path.hpp"

namespace peicon {
namespace {

uint16_t R16(const std::vector<uint8_t>& b, size_t o) { return (uint16_t)(b[o] | (b[o + 1] << 8)); }
uint32_t R32(const std::vector<uint8_t>& b, size_t o) { return (uint32_t)(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24)); }
void W16(std::string& s, uint16_t v) { s.push_back((char)(v & 0xFF)); s.push_back((char)(v >> 8)); }
void W32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back((char)((v >> (8 * i)) & 0xFF)); }

// Area-averaged resize in premultiplied alpha (so transparent pixels do not bleed colour into the edges).
std::vector<uint8_t> Resize(const uint8_t* src, int sw, int sh, int dw, int dh) {
    std::vector<uint8_t> dst((size_t)dw * dh * 4);
    for (int y = 0; y < dh; ++y) {
        const double y0 = (double)y * sh / dh, y1 = (double)(y + 1) * sh / dh;
        for (int x = 0; x < dw; ++x) {
            const double x0 = (double)x * sw / dw, x1 = (double)(x + 1) * sw / dw;
            double r = 0, g = 0, b = 0, a = 0, wsum = 0;
            for (int sy = (int)y0; sy < std::min(sh, (int)std::ceil(y1)); ++sy) {
                const double wy = std::min<double>(sy + 1, y1) - std::max<double>(sy, y0);
                for (int sx = (int)x0; sx < std::min(sw, (int)std::ceil(x1)); ++sx) {
                    const double w = wy * (std::min<double>(sx + 1, x1) - std::max<double>(sx, x0));
                    const uint8_t* p = src + ((size_t)sy * sw + sx) * 4;
                    const double pa = p[3] / 255.0;
                    r += w * p[0] * pa; g += w * p[1] * pa; b += w * p[2] * pa; a += w * pa; wsum += w;
                }
            }
            uint8_t* d = dst.data() + ((size_t)y * dw + x) * 4;
            if (wsum <= 0 || a <= 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
            d[0] = (uint8_t)std::clamp(r / a + 0.5, 0.0, 255.0);
            d[1] = (uint8_t)std::clamp(g / a + 0.5, 0.0, 255.0);
            d[2] = (uint8_t)std::clamp(b / a + 0.5, 0.0, 255.0);
            d[3] = (uint8_t)std::clamp(a / wsum * 255.0 + 0.5, 0.0, 255.0);
        }
    }
    return dst;
}

} // namespace

void EncodeIconDib(const uint8_t* rgba, int srcW, int srcH, int size, std::string& out) {
    const std::vector<uint8_t> px = Resize(rgba, srcW, srcH, size, size);
    out.clear();
    W32(out, 40); W32(out, (uint32_t)size); W32(out, (uint32_t)size * 2);   // BITMAPINFOHEADER; height counts the mask
    W16(out, 1); W16(out, 32); W32(out, 0); W32(out, (uint32_t)size * size * 4);
    W32(out, 0); W32(out, 0); W32(out, 0); W32(out, 0);
    for (int y = size - 1; y >= 0; --y) {                                     // bottom-up BGRA
        for (int x = 0; x < size; ++x) {
            const uint8_t* p = px.data() + ((size_t)y * size + x) * 4;
            out.push_back((char)p[2]); out.push_back((char)p[1]); out.push_back((char)p[0]); out.push_back((char)p[3]);
        }
    }
    out.append((size_t)((size + 31) / 32) * 4 * size, '\0');                  // AND mask: all 0 (alpha does the work)
}

int ReplaceExeIcon(const std::string& exePath, const uint8_t* rgba, int width, int height, std::string& error) {
    if (!rgba || width <= 0 || height <= 0) { error = "No icon image."; return 0; }
    std::fstream f(U8Path(exePath), std::ios::in | std::ios::out | std::ios::binary);
    if (!f) { error = "Cannot open " + exePath; return 0; }

    auto readAt = [&](std::streamoff off, size_t n, std::vector<uint8_t>& out) {
        out.assign(n, 0);
        f.clear(); f.seekg(off);
        return (bool)f.read((char*)out.data(), (std::streamsize)n);
    };

    std::vector<uint8_t> hdr;
    if (!readAt(0, 0x40, hdr) || hdr[0] != 'M' || hdr[1] != 'Z') { error = "Not a Windows executable."; return 0; }
    const uint32_t pe = R32(hdr, 0x3C);
    std::vector<uint8_t> nt;
    if (pe > (1u << 20) || !readAt(pe, 24 + 240, nt) || std::memcmp(nt.data(), "PE\0\0", 4) != 0) { error = "Not a PE file."; return 0; }
    const uint16_t sections = R16(nt, 6), optSize = R16(nt, 20);
    if (R16(nt, 24) != 0x20B || optSize < 112 + 8 * 3) { error = "Not a 64-bit PE file."; return 0; }
    const uint32_t rsrcRva = R32(nt, 24 + 112 + 16), rsrcSize = R32(nt, 24 + 112 + 20);
    if (rsrcRva == 0 || rsrcSize == 0) { error = "The player has no resource section (build it with resources/player.rc)."; return 0; }

    std::vector<uint8_t> sec;
    if (!readAt((std::streamoff)pe + 24 + optSize, (size_t)sections * 40, sec)) { error = "Truncated section table."; return 0; }
    uint32_t rawPtr = 0, rawSize = 0, secRva = 0;
    for (uint16_t i = 0; i < sections; ++i) {
        const size_t o = (size_t)i * 40;
        const uint32_t va = R32(sec, o + 12), vsize = R32(sec, o + 8), rsize = R32(sec, o + 16), ptr = R32(sec, o + 20);
        if (rsrcRva >= va && rsrcRva < va + std::max(vsize, rsize)) { secRva = va; rawPtr = ptr; rawSize = rsize; break; }
    }
    if (rawPtr == 0) { error = "Resource section not found."; return 0; }
    const uint32_t dirOff = rsrcRva - secRva;                 // directory start inside the section
    if (dirOff >= rawSize) { error = "Corrupt resource directory."; return 0; }

    std::vector<uint8_t> rs;
    if (!readAt(rawPtr, rawSize, rs)) { error = "Truncated resource section."; return 0; }

    // Resource tree walk: dir at `base` (relative to the section) -> entries (id/name, offset).
    struct Entry { uint32_t id; uint32_t offset; bool isDir; };
    auto entries = [&](uint32_t base) {
        std::vector<Entry> v;
        if (base + 16 > rs.size()) return v;
        const uint32_t n = (uint32_t)R16(rs, base + 12) + R16(rs, base + 14);
        for (uint32_t i = 0; i < n && base + 16 + (i + 1) * 8 <= rs.size(); ++i) {
            const uint32_t id = R32(rs, base + 16 + i * 8), off = R32(rs, base + 20 + i * 8);
            v.push_back({ id, (off & 0x7FFFFFFFu) , (off & 0x80000000u) != 0 });
        }
        return v;
    };
    // First data entry under a (type, id) path, descending through language dirs: returns file offset + size of its data.
    struct Leaf { size_t fileOff = 0; uint32_t size = 0; bool ok = false; };
    auto leafOf = [&](const Entry& e) {
        Leaf leaf;
        uint32_t off = dirOff + e.offset;
        bool isDir = e.isDir;
        for (int depth = 0; isDir && depth < 4; ++depth) {
            const std::vector<Entry> sub = entries(off);
            if (sub.empty()) return leaf;
            isDir = sub[0].isDir;
            off = dirOff + sub[0].offset;
        }
        if (isDir || off + 16 > rs.size()) return leaf;
        const uint32_t dataRva = R32(rs, off), size = R32(rs, off + 4);
        if (dataRva < secRva || dataRva - secRva + size > rs.size()) return leaf;
        leaf.fileOff = dataRva - secRva; leaf.size = size; leaf.ok = true;   // offset inside `rs`
        return leaf;
    };

    std::vector<Entry> iconEntries, groupEntries;
    for (const Entry& e : entries(dirOff)) {
        if (e.id == 3 && e.isDir) iconEntries = entries(dirOff + e.offset);          // RT_ICON
        if (e.id == 14 && e.isDir) groupEntries = entries(dirOff + e.offset);        // RT_GROUP_ICON
    }
    if (iconEntries.empty() || groupEntries.empty()) { error = "The player has no icon resources (build it with resources/player.rc)."; return 0; }

    int replaced = 0;
    bool dirty = false;
    for (const Entry& g : groupEntries) {
        const Leaf grp = leafOf(g);
        if (!grp.ok || grp.size < 6) continue;
        const uint16_t count = R16(rs, grp.fileOff + 4);
        for (uint16_t i = 0; i < count && 6u + (i + 1) * 14u <= grp.size; ++i) {
            const size_t e = grp.fileOff + 6 + (size_t)i * 14;
            const int size = rs[e] == 0 ? 256 : rs[e];
            const uint16_t bits = R16(rs, e + 6);
            const uint32_t bytes = R32(rs, e + 8);
            const uint16_t iconId = R16(rs, e + 12);
            if (bits != 32) continue;
            for (const Entry& ie : iconEntries) {
                if (ie.id != iconId) continue;
                const Leaf slot = leafOf(ie);
                if (!slot.ok || slot.size != bytes) continue;
                std::string dib;
                EncodeIconDib(rgba, width, height, size, dib);
                if (dib.size() != slot.size) continue;       // not the uncompressed 32-bit layout we expected
                std::memcpy(rs.data() + slot.fileOff, dib.data(), dib.size());
                ++replaced; dirty = true;
            }
        }
    }
    if (!dirty) { error = "The player's icon slots are not replaceable 32-bit images."; return 0; }
    f.clear(); f.seekp(rawPtr);
    f.write((const char*)rs.data(), (std::streamsize)rs.size());
    f.flush();
    if (!f) { error = "Could not write " + exePath; return 0; }
    return replaced;
}

} // namespace peicon
