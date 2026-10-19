#include "../../../include/Engine/PackArchive.hpp"
#include "miniz.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "Engine/U8Path.hpp"

#if defined(_WIN32)
#define FLY_FSEEK _fseeki64
#define FLY_FTELL _ftelli64
#else
#define FLY_FSEEK fseeko
#define FLY_FTELL ftello
#endif

namespace fs = std::filesystem;

namespace pack {
namespace {

void Wr16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
void Wr32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (8 * i))); }
void Wr64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back((uint8_t)(x >> (8 * i))); }

// Formats that are already entropy coded: deflating them again wastes time for nothing.
bool AlreadyCompressed(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    static const char* kSkip[] = { ".png", ".jpg", ".jpeg", ".webp", ".ogg", ".mp3", ".mp4", ".zip", ".ktx2", ".flac", ".gz" };
    for (const char* e : kSkip) if (ext == e) return true;
    return false;
}

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(U8Path(path), std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    in.seekg(0);
    out.resize((size_t)size);
    return size == 0 || (bool)in.read((char*)out.data(), size);
}

struct IndexItem {
    std::string path;
    uint64_t offset = 0, stored = 0, original = 0;
    uint32_t crc = 0;
    uint8_t method = 0;
};

} // namespace

bool BuildExecutable(const std::string& srcExe, const std::string& dstExe, const std::vector<Entry>& entries,
                     std::string& error, const std::function<bool(size_t, size_t)>& progress) {
    error.clear();
    const fs::path src = U8Path(srcExe), dst = U8Path(dstExe);
    const fs::path tmp = U8Path(dstExe + ".part");
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::remove(tmp, ec);
    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) { error = "Could not copy the player executable: " + ec.message(); return false; }

    FILE* out = std::fopen(tmp.string().c_str(), "ab");
    if (!out) { error = "Could not open " + tmp.string() + " for writing"; fs::remove(tmp, ec); return false; }
    FLY_FSEEK(out, 0, SEEK_END);
    const int64_t archiveStart = FLY_FTELL(out);

    auto fail = [&](const std::string& why) {
        error = why;
        std::fclose(out);
        fs::remove(tmp, ec);
        return false;
    };

    std::vector<IndexItem> index;
    index.reserve(entries.size());
    uint64_t offset = 0;
    std::vector<uint8_t> data;
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        if (progress && !progress(i, entries.size())) return fail("Cancelled");
        if (e.path.empty() || e.path.size() > 0xFFFF) return fail("Bad archive path: " + e.path);
        if (!e.aliasOf.empty()) {
            // Identical to an earlier file: point at the same stored bytes.
            auto src = std::find_if(index.begin(), index.end(), [&](const IndexItem& it) { return it.path == e.aliasOf; });
            if (src == index.end()) return fail("Alias target not found: " + e.aliasOf);
            IndexItem item = *src;
            item.path = e.path;
            index.push_back(std::move(item));
            continue;
        }
        if (e.fromBytes) data = e.bytes;
        else if (!ReadWholeFile(e.diskPath, data)) return fail("Could not read " + e.diskPath);

        IndexItem item;
        item.path = e.path;
        item.offset = offset;
        item.original = data.size();
        item.crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, data.data(), data.size());

        const uint8_t* payload = data.data();
        size_t payloadSize = data.size();
        void* packed = nullptr;
        size_t packedSize = 0;
        if (!data.empty() && !AlreadyCompressed(e.path)) {
            const int flags = (int)tdefl_create_comp_flags_from_zip_params(6, -15, MZ_DEFAULT_STRATEGY);   // raw deflate
            packed = tdefl_compress_mem_to_heap(data.data(), data.size(), &packedSize, flags);
            if (packed && packedSize < data.size() - data.size() / 20) {   // worth it: at least 5% smaller
                payload = (const uint8_t*)packed;
                payloadSize = packedSize;
                item.method = 1;
            }
        }
        const bool ok = payloadSize == 0 || std::fwrite(payload, 1, payloadSize, out) == payloadSize;
        if (packed) mz_free(packed);
        if (!ok) return fail("Write failed (disk full?)");
        item.stored = payloadSize;
        offset += payloadSize;
        index.push_back(std::move(item));
    }

    std::vector<uint8_t> idx;
    Wr32(idx, (uint32_t)index.size());
    for (const IndexItem& it : index) {
        Wr16(idx, (uint16_t)it.path.size());
        idx.insert(idx.end(), it.path.begin(), it.path.end());
        Wr64(idx, it.offset); Wr64(idx, it.stored); Wr64(idx, it.original);
        Wr32(idx, it.crc);
        idx.push_back(it.method);
    }
    const uint64_t indexOffset = offset;
    if (std::fwrite(idx.data(), 1, idx.size(), out) != idx.size()) return fail("Write failed (disk full?)");

    std::vector<uint8_t> footer;
    Wr64(footer, indexOffset);
    Wr64(footer, idx.size());
    Wr64(footer, indexOffset + idx.size() + kFooterSize);
    Wr32(footer, (uint32_t)mz_crc32(MZ_CRC32_INIT, idx.data(), idx.size()));
    Wr32(footer, kVersion);
    footer.resize(kFooterSize - 8, 0);
    footer.insert(footer.end(), kMagic, kMagic + 8);
    if (std::fwrite(footer.data(), 1, footer.size(), out) != footer.size()) return fail("Write failed (disk full?)");
    if (std::fclose(out) != 0) { fs::remove(tmp, ec); error = "Write failed (disk full?)"; return false; }
    (void)archiveStart;

    fs::permissions(tmp, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                         fs::perms::others_read | fs::perms::others_exec, fs::perm_options::replace, ec);
    fs::rename(tmp, dst, ec);   // replaces dst atomically; a failure keeps the old file
    if (ec) { fs::remove(tmp, ec); error = "Could not move the result into place: " + ec.message(); return false; }
    if (progress) progress(entries.size(), entries.size());
    return true;
}

} // namespace pack
