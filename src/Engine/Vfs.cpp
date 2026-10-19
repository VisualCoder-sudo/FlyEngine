#include "../../include/Engine/Vfs.hpp"
#include "../../include/Engine/PackArchive.hpp"
#include "../../include/Engine/Platform/Platform.hpp"
#include "flyvfs.h"
#include "miniz.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include "Engine/U8Path.hpp"

#if defined(_WIN32)
#define FLY_FSEEK _fseeki64
#define FLY_FTELL _ftelli64
#else
#define FLY_FSEEK fseeko
#define FLY_FTELL ftello
#endif

namespace vfs {
namespace {

struct IndexEntry {
    uint64_t offset = 0, stored = 0, original = 0;
    uint32_t crc = 0;
    uint8_t method = 0;
    std::string external;    // when set: the file lives on disk at this path, not in the archive
};

struct State {
    FILE* file = nullptr;
    uint64_t archiveStart = 0;
    std::unordered_map<std::string, IndexEntry> files;
    std::map<std::string, std::set<std::string>> dirs;   // directory ("" = root) -> child names
    std::mutex ioMutex;                                   // serialises fseek+fread; decompression is outside it
    bool active = false;
};
State g;

uint64_t Rd64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | p[i]; return v; }
uint32_t Rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t Rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

std::string Normalize(std::string p) {
    for (char& c : p) if (c == '\\') c = '/';
    return p;
}

// Archive-relative path for a virtual or relative one ("" if it points outside the root).
bool ToRelative(const std::string& path, std::string& rel) {
    std::string p = Normalize(path);
    const std::string root = kRoot;
    if (p.compare(0, root.size(), root) == 0 && (p.size() == root.size() || p[root.size()] == '/')) {
        p = p.substr(root.size());
    }
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    while (!p.empty() && p.back() == '/') p.pop_back();
    // Collapse "./" and "a/../b" so scene paths that were joined with ".." still resolve.
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        const std::string part = p.substr(i, j - i);
        if (part == "..") { if (parts.empty()) return false; parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        i = j + 1;
    }
    rel.clear();
    for (size_t k = 0; k < parts.size(); ++k) { if (k) rel += '/'; rel += parts[k]; }
    return true;
}

// Registers every parent directory of `path`.
void AddDirs(const std::string& path) {
    std::string dir;
    size_t start = 0;
    while (true) {
        const size_t slash = path.find('/', start);
        const std::string name = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        g.dirs[dir].insert(name);
        if (slash == std::string::npos) break;
        dir += (dir.empty() ? "" : "/") + name;
        start = slash + 1;
    }
}

bool ReadAt(uint64_t absOffset, void* dst, size_t size) {
    std::lock_guard<std::mutex> lock(g.ioMutex);
    if (FLY_FSEEK(g.file, (int64_t)absOffset, SEEK_SET) != 0) return false;
    return std::fread(dst, 1, size, g.file) == size;
}

bool Mount(FILE* f) {
    Unmount();
    if (!f) return false;
    g.file = f;
    FLY_FSEEK(f, 0, SEEK_END);
    const int64_t fileSize = FLY_FTELL(f);
    if (fileSize < (int64_t)pack::kFooterSize) { Unmount(); return false; }

    uint8_t footer[pack::kFooterSize];
    if (!ReadAt((uint64_t)fileSize - pack::kFooterSize, footer, sizeof footer) ||
        std::memcmp(footer + pack::kFooterSize - 8, pack::kMagic, 8) != 0) { Unmount(); return false; }

    const uint64_t indexOffset = Rd64(footer), indexSize = Rd64(footer + 8), archiveSize = Rd64(footer + 16);
    const uint32_t indexCrc = Rd32(footer + 24), version = Rd32(footer + 28);
    if (version != pack::kVersion || archiveSize > (uint64_t)fileSize || archiveSize < pack::kFooterSize ||
        indexOffset + indexSize + pack::kFooterSize != archiveSize) { Unmount(); return false; }
    g.archiveStart = (uint64_t)fileSize - archiveSize;

    std::vector<uint8_t> index((size_t)indexSize);
    if (!ReadAt(g.archiveStart + indexOffset, index.data(), index.size()) ||
        (uint32_t)mz_crc32(MZ_CRC32_INIT, index.data(), index.size()) != indexCrc) { Unmount(); return false; }

    size_t pos = 0;
    if (index.size() < 4) { Unmount(); return false; }
    const uint32_t count = Rd32(index.data());
    pos = 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 2 > index.size()) { Unmount(); return false; }
        const uint16_t len = Rd16(&index[pos]); pos += 2;
        if (pos + len + 8 + 8 + 8 + 4 + 1 > index.size()) { Unmount(); return false; }
        std::string path((const char*)&index[pos], len); pos += len;
        IndexEntry e;
        e.offset = Rd64(&index[pos]); pos += 8;
        e.stored = Rd64(&index[pos]); pos += 8;
        e.original = Rd64(&index[pos]); pos += 8;
        e.crc = Rd32(&index[pos]); pos += 4;
        e.method = index[pos++];
        g.files[path] = e;
        AddDirs(path);
    }
    g.active = true;
    return true;
}

// --- raylib hooks (see rl/flyvfs.h) ---
int HookHandles(const char* path) { return g.active && Handles(path); }
int HookFileExists(const char* path) { return Exists(path); }
int HookDirExists(const char* path) { return IsDir(path); }
unsigned char* HookRead(const char* path, int* size) {
    std::vector<uint8_t> data;
    if (!Read(path, data)) return nullptr;
    unsigned char* out = (unsigned char*)std::malloc(data.empty() ? 1 : data.size());
    if (!data.empty()) std::memcpy(out, data.data(), data.size());
    if (size) *size = (int)data.size();
    return out;
}
const FlyVfsHooks kHooks = { HookHandles, HookFileExists, HookDirExists, HookRead };

} // namespace

bool MountFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    if (!Mount(f)) return false;
    FlySetVfsHooks(&kHooks);
    return true;
}

bool MountSelfExecutable() {
    const std::string exe = platform::ExecutablePath();
    return !exe.empty() && MountFile(exe);
}

void Unmount() {
    FlySetVfsHooks(nullptr);
    g.active = false;
    if (g.file) std::fclose(g.file);
    g.file = nullptr;
    g.files.clear();
    g.dirs.clear();
}

bool Active() { return g.active; }

bool Handles(const std::string& path) {
    if (!g.active) return false;
    const std::string p = Normalize(path);
    const std::string root = kRoot;
    return p.compare(0, root.size(), root) == 0 && (p.size() == root.size() || p[root.size()] == '/');
}

bool Exists(const std::string& path) {
    std::string rel;
    return g.active && ToRelative(path, rel) && g.files.count(rel) != 0;
}

bool IsDir(const std::string& path) {
    std::string rel;
    return g.active && ToRelative(path, rel) && g.dirs.count(rel) != 0 && g.files.count(rel) == 0;
}

bool Read(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    std::string rel;
    if (!g.active || !ToRelative(path, rel)) return false;
    auto it = g.files.find(rel);
    if (it == g.files.end()) return false;
    const IndexEntry& e = it->second;

    if (!e.external.empty()) {   // shipped beside the executable
        std::ifstream in(U8Path(e.external), std::ios::binary);
        if (!in) return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (out.size() != (size_t)e.original) { out.clear(); return false; }
        return true;
    }

    std::vector<uint8_t> stored((size_t)e.stored);
    if (e.stored && !ReadAt(g.archiveStart + e.offset, stored.data(), stored.size())) return false;
    if (e.method == 0) {
        out = std::move(stored);
    } else {
        out.resize((size_t)e.original);
        if (e.original) {
            const size_t got = tinfl_decompress_mem_to_mem(out.data(), out.size(), stored.data(), stored.size(), 0);
            if (got != (size_t)e.original) { out.clear(); return false; }
        }
    }
    if ((uint32_t)mz_crc32(MZ_CRC32_INIT, out.data(), out.size()) != e.crc) { out.clear(); return false; }
    return true;
}

std::vector<std::string> List(const std::string& dirPath) {
    std::vector<std::string> names;
    std::string rel;
    if (!g.active || !ToRelative(dirPath, rel)) return names;
    auto it = g.dirs.find(rel);
    if (it != g.dirs.end()) names.assign(it->second.begin(), it->second.end());
    return names;
}

std::string VirtualPath(const std::string& relative) {
    std::string r = Normalize(relative);
    while (!r.empty() && r.front() == '/') r.erase(r.begin());
    return r.empty() ? std::string(kRoot) : std::string(kRoot) + "/" + r;
}

ExternalReport MountExternal(const std::string& folder, const std::string& prefix, const std::vector<ExternalFile>& files) {
    namespace fs = std::filesystem;
    ExternalReport report;
    report.expected = files.size();
    std::error_code ec;
    report.folderFound = fs::is_directory(U8Path(folder), ec);
    for (const ExternalFile& f : files) {
        std::string rel = Normalize(f.path);
        const std::string under = rel.compare(0, prefix.size(), prefix) == 0 ? rel.substr(prefix.size()) : rel;
        const fs::path disk = U8Path(folder) / U8Path(under);
        std::error_code e2;
        const bool climbs = ("/" + under + "/").find("/../") != std::string::npos;   // never read outside the folder
        if (climbs || !g.active || !fs::is_regular_file(disk, e2) || fs::file_size(disk, e2) != f.size) {
            report.missing.push_back(rel);
            continue;
        }
        IndexEntry e;
        e.original = f.size;
        e.external = disk.string();
        g.files[rel] = e;
        AddDirs(rel);
    }
    return report;
}

} // namespace vfs
