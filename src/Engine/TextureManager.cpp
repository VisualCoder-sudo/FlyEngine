#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CloseWindow Win32CloseWindow
#define ShowCursor Win32ShowCursor
#define Rectangle Win32Rectangle
#include <windows.h>
#undef CloseWindow
#undef ShowCursor
#undef Rectangle
#undef LoadImage
#undef DrawText
#undef DrawTextEx
#undef PlaySound

#include "../../include/Engine/Backend/TextureManager.hpp"
#include "../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../include/Engine/Frontend/ProjectManager.hpp"
#include "raylib.h"
#include "raymath.h"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <chrono>

namespace fs = std::filesystem;

namespace textureManager {

std::string g_projectDir;
std::unordered_map<std::string, int> g_refCount;
std::unordered_map<std::string, std::string> g_hashToPath;
std::unordered_map<std::string, Texture2D> g_gpuTextures;

struct TextureCacheEntry {
    std::string hash;
    fs::file_time_type mtime;
    uintmax_t size;
};
std::unordered_map<std::string, TextureCacheEntry> g_textureCache;
std::string g_cacheFilePath;
bool g_cacheLoaded = false;

std::string g_gpuCacheFilePath;
bool g_gpuCacheLoaded = false;
std::unordered_map<std::string, std::string> g_gpuTexturePaths;

static const std::unordered_set<std::string> s_compressedExts = {
    ".dds", ".ktx", ".pkm", ".astc", ".basis",
    ".DDS", ".KTX", ".PKM", ".ASTC", ".BASIS"
};

struct SHA256 {
    uint32_t h[8];
    uint32_t buf[16];
    uint64_t bytes;
    uint8_t buffer[64];
    size_t bufferLen;

    SHA256() { reset(); }

    void reset() {
        h[0] = 0x6a09e667; h[1] = 0xbb67ae85; h[2] = 0x3c6ef372; h[3] = 0xa54ff53a;
        h[4] = 0x510e527f; h[5] = 0x9b05688c; h[6] = 0x1f83d9ab; h[7] = 0x5be0cd19;
        bytes = 0; bufferLen = 0;
    }

    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
    static uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
    static uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
    static uint32_t ep0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
    static uint32_t ep1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
    static uint32_t sig0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
    static uint32_t sig1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }
    static const uint32_t k[64];

    void transform() {
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + ep1(e) + ch(e, f, g) + k[i] + buf[i];
            uint32_t t2 = ep0(a) + maj(a, b, c);
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const uint8_t* data, size_t len) {
        size_t i = 0;
        while (i < len) {
            if (bufferLen == 64) { transform(); bufferLen = 0; }
            size_t take = std::min(len - i, 64 - bufferLen);
            memcpy(buffer + bufferLen, data + i, take);
            bufferLen += take;
            i += take;
            bytes += take;
        }
    }

    std::string final() {
        uint64_t bits = bytes * 8;
        update((const uint8_t*)"\x80", 1);
        while (bufferLen != 56) update((const uint8_t*)"", 1);
        for (int i = 7; i >= 0; --i) {
            update((const uint8_t*)&bits + i, 1);
        }
        transform();

        std::ostringstream oss;
        for (int i = 0; i < 8; ++i) {
            oss << std::hex << std::setw(8) << std::setfill('0') << h[i];
        }
        return oss.str();
    }
};

const uint32_t SHA256::k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x1e376c08, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

std::string ComputeFileSHA256(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";
    SHA256 sha;
    char buf[8192];
    while (file.read(buf, sizeof(buf)) || file.gcount() > 0) {
        sha.update((const uint8_t*)buf, file.gcount());
    }
    return sha.final();
}

void PreloadAllTextures();

void UnloadGPUTexture(const std::string& relPath) {
    auto it = g_gpuTextures.find(relPath);
    if (it != g_gpuTextures.end()) {
        UnloadTexture(it->second);
        g_gpuTextures.erase(it);
    }
}

void LoadGPUCache() {
    if (g_projectDir.empty()) return;
    g_gpuCacheFilePath = (fs::path(g_projectDir) / ".texture_gpu_cache.json").generic_string();
    g_gpuCacheLoaded = true;
    std::ifstream file(g_gpuCacheFilePath);
    if (!file) return;
    
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        size_t pos = line.find('|');
        if (pos == std::string::npos) continue;
        std::string relPath = line.substr(0, pos);
        std::string cachedPath = line.substr(pos + 1);
        g_gpuTexturePaths[relPath] = cachedPath;
    }
}

void SaveGPUCache() {
    if (g_projectDir.empty() || g_gpuCacheFilePath.empty()) return;
    std::ofstream file(g_gpuCacheFilePath);
    if (!file) return;
    
    for (const auto& [relPath, cachedPath] : g_gpuTexturePaths) {
        file << relPath << '|' << cachedPath << '\n';
    }
}

Texture2D GetGPUTexture(const std::string& relPath) {
    auto it = g_gpuTextures.find(relPath);
    if (it != g_gpuTextures.end()) {
        return it->second;
    }
    if (g_projectDir.empty()) return Texture2D{0};
    
    std::string absPath = (fs::path(g_projectDir) / relPath).generic_string();
    
    Texture2D tex = LoadTexture(absPath.c_str());
    if (tex.id != 0) {
        g_gpuTextures[relPath] = tex;
    }
    return tex;
}

void LoadTextureCache() {
    if (g_cacheLoaded || g_projectDir.empty()) return;
    g_cacheFilePath = (fs::path(g_projectDir) / ".texture_cache.json").generic_string();
    g_cacheLoaded = true;
    std::ifstream file(g_cacheFilePath);
    if (!file) return;
    
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        size_t p1 = line.find('|');
        if (p1 == std::string::npos) continue;
        size_t p2 = line.find('|', p1 + 1);
        if (p2 == std::string::npos) continue;
        size_t p3 = line.find('|', p2 + 1);
        if (p3 == std::string::npos) continue;
        
        std::string path = line.substr(0, p1);
        if (path.empty()) continue;
        std::string hash = line.substr(p1 + 1, p2 - p1 - 1);
        std::string mtimeStr = line.substr(p2 + 1, p3 - p2 - 1);
        std::string sizeStr = line.substr(p3 + 1);
        
        TextureCacheEntry entry;
        entry.hash = hash;
        try {
            entry.mtime = fs::file_time_type(std::chrono::duration_cast<fs::file_time_type::duration>(
                std::chrono::seconds(std::stoll(mtimeStr))));
            entry.size = std::stoull(sizeStr);
        } catch (...) {
            // Malformed/corrupt cache line (e.g. non-numeric mtime/size after a
            // hand edit or a partial write) must not abort the whole load.
            continue;
        }
        g_textureCache[path] = entry;
    }
}

void SaveTextureCache() {
    if (g_projectDir.empty() || g_cacheFilePath.empty()) return;
    std::ofstream file(g_cacheFilePath);
    if (!file) return;
    
    for (const auto& [path, entry] : g_textureCache) {
        auto mtimeEpoch = std::chrono::duration_cast<std::chrono::seconds>(
            entry.mtime.time_since_epoch()).count();
        file << path << '|' << entry.hash << '|' << mtimeEpoch << '|' << entry.size << '\n';
    }
}

std::string ComputeFileSHA256Cached(const std::string& absPath, const std::string& relPath) {
    LoadTextureCache();
    
    std::error_code ec;
    fs::file_status status;
    try {
        status = fs::status(absPath, ec);
    } catch (...) {
        return "";
    }
    if (ec || !fs::exists(status)) return "";
    
    fs::file_time_type mtime;
    try {
        mtime = fs::last_write_time(absPath, ec);
    } catch (...) {
        return ComputeFileSHA256(absPath);
    }
    if (ec) return ComputeFileSHA256(absPath);
    
    uintmax_t size = 0;
    try {
        size = fs::file_size(absPath, ec);
    } catch (...) {
        return ComputeFileSHA256(absPath);
    }
    if (ec) return ComputeFileSHA256(absPath);
    
    auto it = g_textureCache.find(relPath);
    if (it != g_textureCache.end() && 
        it->second.mtime == mtime && 
        it->second.size == size) {
        return it->second.hash;
    }
    
    std::string hash = ComputeFileSHA256(absPath);
    if (!hash.empty()) {
        TextureCacheEntry entry;
        entry.hash = hash;
        entry.mtime = mtime;
        entry.size = size;
        g_textureCache[relPath] = entry;
    }
    return hash;
}

std::string GetRelativePath(const std::string& absolutePath, const std::string& baseDir) {
    try {
        if (absolutePath.empty() || baseDir.empty()) return absolutePath;
        std::error_code ec;
        fs::path abs = fs::absolute(absolutePath, ec);
        if (ec) return absolutePath;
        fs::path base = fs::absolute(baseDir, ec);
        if (ec) return absolutePath;
        return fs::relative(abs, base).generic_string();
    } catch (...) {
        return absolutePath;
    }
}

std::string GetModelNameFromPath(const std::string& modelPath) {
    try {
        fs::path p(modelPath);
        return p.stem().string();
    } catch (...) {
        return "";
    }
}

std::string NormalizeKey(const fs::path& p) {
    std::string s = p.lexically_normal().generic_string();
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool CopyFileTo(const std::string& src, const std::string& dst) {
    try {
        fs::create_directories(fs::path(dst).parent_path());
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
        return true;
    } catch (...) {
        return false;
    }
}

bool DeleteFileIfExists(const std::string& path) {
    try {
        if (fs::exists(path)) fs::remove(path);
        return true;
    } catch (...) {
        return false;
    }
}

bool MoveFileTo(const std::string& src, const std::string& dst) {
    try {
        fs::create_directories(fs::path(dst).parent_path());
        fs::rename(src, dst);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace textureManager

namespace textureManager {

void Init(const std::string& projectDir) {
    g_projectDir = projectDir;
    g_refCount.clear();
    g_hashToPath.clear();

    for (auto& [path, tex] : g_gpuTextures) {
        UnloadTexture(tex);
    }
    g_gpuTextures.clear();
    
    LoadGPUCache();
}

void Shutdown() {
    SaveGPUCache();
    
    for (auto& [path, tex] : g_gpuTextures) {
        UnloadTexture(tex);
    }
    g_gpuTextures.clear();
    g_refCount.clear();
    g_hashToPath.clear();
    g_projectDir.clear();
}

void PreloadAllTextures() {
    if (g_projectDir.empty()) return;
    
    std::vector<std::string> texturePaths;
    std::error_code ec;
    fs::path assets3D = fs::path(g_projectDir) / "assets" / "3D";
    fs::path assetsPreset = fs::path(g_projectDir) / "assets" / "PresetTextures";
    fs::path assetsShared = fs::path(g_projectDir) / "assets" / "shared";
    
    auto collectTextures = [&](const fs::path& dir) {
        if (!fs::exists(dir, ec)) return;
        for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); ++it) {
            if (ec) continue;
            if (!it->is_regular_file(ec)) continue;
            std::string ext = it->path().extension().string();
            if (s_compressedExts.count(ext) || 
                ext == ".png" || ext == ".jpg" || ext == ".jpeg" || 
                ext == ".bmp" || ext == ".tga" || ext == ".webp") {
                fs::path rel = fs::relative(it->path(), g_projectDir, ec);
                if (!ec) {
                    texturePaths.push_back(rel.generic_string());
                }
            }
        }
    };
    
    collectTextures(assets3D);
    collectTextures(assetsPreset);
    collectTextures(assetsShared);
    
    for (const auto& relPath : texturePaths) {
        GetGPUTexture(relPath);
    }
}

std::string GetModelTextureDir(const std::string& modelPath) {
    try {
        if (modelPath.empty()) return "";
        fs::path parent = fs::path(modelPath).parent_path();
        if (parent.empty()) {
            parent = fs::path(g_projectDir) / "assets" / "3D" / GetModelNameFromPath(modelPath);
        } else if (parent.is_relative()) {
            parent = fs::path(g_projectDir) / parent;
        }
        return parent.lexically_normal().generic_string();
    } catch (...) {
        return "";
    }
}

std::string GetSharedTextureDir() {
    if (g_projectDir.empty()) return "";
    fs::path dir = fs::path(g_projectDir) / "assets" / "shared";
    return dir.generic_string();
}

std::string ComputeSHA256(const std::string& filePath) {
    return ComputeFileSHA256(filePath);
}

static void IncrementRef(const std::string& relPath) {
    if (g_refCount[relPath] == 0) {
        GetGPUTexture(relPath);
    }
    g_refCount[relPath]++;
}

static void DecrementRef(const std::string& relPath) {
    auto it = g_refCount.find(relPath);
    if (it == g_refCount.end()) return;
    it->second--;
    if (it->second <= 0) {
        UnloadGPUTexture(relPath);
        // Note: we do NOT delete the file from disk here. The refcount can
        // point at a hash-canonicalized path that is an original user asset
        // (see VerifyAndRebuild), so deleting on refcount-zero could destroy
        // project files. GPU texture and bookkeeping only.
        for (auto hit = g_hashToPath.begin(); hit != g_hashToPath.end(); ++hit) {
            if (hit->second == relPath) {
                g_hashToPath.erase(hit);
                break;
            }
        }
        g_refCount.erase(it);
    }
}

static std::string FindObjectUsingTexture(const std::string& relPath, const std::vector<ScatteredObject*>& objects) {
    for (auto* obj : objects) {
        if (obj && obj->GetTexturePath() == relPath) {
            return obj->GetModelPath();
        }
    }
    return "";
}

static void MigrateSharedToModel(const std::string& relPath, const std::vector<ScatteredObject*>& objects) {
    std::string modelPath = FindObjectUsingTexture(relPath, objects);
    if (modelPath.empty()) return;

    std::string srcAbs = (fs::path(g_projectDir) / relPath).generic_string();
    std::string dstDir = GetModelTextureDir(modelPath);
    fs::path srcPath(relPath);
    std::string dstAbs = (fs::path(dstDir) / srcPath.filename()).generic_string();

    if (MoveFileTo(srcAbs, dstAbs)) {
        std::string newRelPath = GetRelativePath(dstAbs, g_projectDir);
        for (auto& kv : g_hashToPath) {
            if (kv.second == relPath) {
                kv.second = newRelPath;
                break;
            }
        }
        int count = g_refCount[relPath];
        g_refCount.erase(relPath);
        g_refCount[newRelPath] = count;
        for (auto* obj : objects) {
            if (obj && obj->GetTexturePath() == relPath) {
            }
        }
    }
}

std::string RegisterTexture(const std::string& sourcePath, const std::string& modelPath) {
    if (sourcePath.empty() || modelPath.empty()) return "";

    std::string hash = ComputeFileSHA256Cached(sourcePath, sourcePath);
    if (hash.empty()) return "";

    auto hit = g_hashToPath.find(hash);
    if (hit != g_hashToPath.end()) {
        std::string canonical = hit->second;
        IncrementRef(canonical);
        return canonical;
    }

    std::string targetDir = GetModelTextureDir(modelPath);
    if (targetDir.empty()) return "";

    try {
        fs::path srcPath(sourcePath);
        std::string dstAbs = (fs::path(targetDir) / srcPath.filename()).generic_string();

        if (!CopyFileTo(sourcePath, dstAbs)) return "";

        std::string relPath = GetRelativePath(dstAbs, g_projectDir);
        g_hashToPath[hash] = relPath;
        IncrementRef(relPath);
        return relPath;
    } catch (...) {
        return "";
    }
}

void UnregisterTexture(const std::string& texturePath) {
    if (texturePath.empty()) return;
    auto it = g_refCount.find(texturePath);
    if (it == g_refCount.end()) return;

    int before = it->second;
    DecrementRef(texturePath);
}

static bool FolderIsUsed(const std::string& folder, const std::vector<ScatteredObject*>& objects) {
    if (folder.empty()) return false;
    std::string key = NormalizeKey(fs::path(folder));
    for (auto* obj : objects) {
        if (!obj) continue;
        std::string mp = obj->GetModelPath();
        if (!mp.empty() && NormalizeKey(fs::path(mp).parent_path()) == key) return true;
        std::string tp = obj->GetTexturePath();
        if (!tp.empty()) {
            fs::path texAbs = fs::path(g_projectDir) / tp;
            if (NormalizeKey(texAbs.parent_path()) == key) return true;
        }
    }
    return false;
}

void RemoveModelDirectory(const std::string& modelPath, const std::vector<ScatteredObject*>& objects) {
    if (modelPath.empty()) return;
    std::string dir = GetModelTextureDir(modelPath);
    if (dir.empty()) return;
    if (FolderIsUsed(dir, objects)) return;
    std::error_code ec;
    fs::remove_all(dir, ec);
}

void VerifyAndRebuild(const std::vector<ScatteredObject*>& objects) {
    try {
        LoadTextureCache();
    } catch (...) {
    }
    g_refCount.clear();
    g_hashToPath.clear();

    for (auto* obj : objects) {
        if (!obj) continue;
        std::string texPath = obj->GetTexturePath();
        if (texPath.empty()) continue;

        std::string absPath = (fs::path(g_projectDir) / texPath).generic_string();
        std::error_code ec;
        bool exists = false;
        try {
            exists = fs::exists(absPath);
        } catch (...) {
            exists = false;
        }
        if (!exists) continue;

        std::string hash = ComputeFileSHA256Cached(absPath, texPath);
        if (hash.empty()) continue;

        auto hit = g_hashToPath.find(hash);
        if (hit != g_hashToPath.end()) {
            std::string canonical = hit->second;
            g_refCount[canonical]++;
        } else {
            g_hashToPath[hash] = texPath;
            g_refCount[texPath] = 1;
        }
    }

    bool changed = true;
    int maxIterations = 100;
    int iteration = 0;
    while (changed && iteration < maxIterations) {
        changed = false;
        iteration++;
        try {
            for (auto it = g_refCount.begin(); it != g_refCount.end(); ) {
                const std::string& relPath = it->first;
                int count = it->second;
                if (count == 1) {
                    if (relPath.find("assets/shared/") == 0) {
                        std::string modelPath = FindObjectUsingTexture(relPath, objects);
                        if (!modelPath.empty()) {
                            std::string srcAbs = (fs::path(g_projectDir) / relPath).generic_string();
                            std::string dstDir = GetModelTextureDir(modelPath);
                            fs::path srcPath(relPath);
                            std::string dstAbs = (fs::path(dstDir) / srcPath.filename()).generic_string();

                            if (MoveFileTo(srcAbs, dstAbs)) {
                                std::string newRelPath = GetRelativePath(dstAbs, g_projectDir);
                                for (auto& kv : g_hashToPath) {
                                    if (kv.second == relPath) {
                                        kv.second = newRelPath;
                                        break;
                                    }
                                }
                                g_refCount.erase(it++);
                                g_refCount[newRelPath] = 1;
                                changed = true;
                                break;
                            }
                        }
                    }
                }
                ++it;
            }
        } catch (...) {
            break;
        }
    }
    try {
        SaveTextureCache();
    } catch (...) {
    }

    fs::path assetsRoot = fs::path(g_projectDir) / "assets" / "3D";
    std::error_code ec;
    if (!fs::exists(assetsRoot, ec)) {
    } else {
        std::vector<fs::path> entries;
        std::error_code iter_ec;
        for (auto it = fs::directory_iterator(assetsRoot, ec); it != fs::directory_iterator(); ++it) {
            try {
                std::error_code ec;
                if (fs::is_directory(it->path(), ec) && !ec) {
                    entries.push_back(it->path());
                }
            } catch (...) {
            }
        }
        
        for (const auto& entry : entries) {
            std::error_code ec;
            try {
                if (fs::is_directory(entry, ec) && !ec) {
                    if (!FolderIsUsed(entry.generic_string(), objects)) {
                        std::error_code rec;
                        fs::remove_all(entry, rec);
                    }
                    }
                } catch (...) {
            }
        }
    }
}

std::vector<std::string> GetPresetTextures(const std::string& projectDir) {
    std::vector<std::string> result;
    if (projectDir.empty()) return result;

    static std::vector<std::string> s_presetCache;
    static std::string s_cachedProjectDir;
    static double s_cacheTimestamp = 0.0;
    double now = GetTime();
    if (!s_presetCache.empty() && s_cachedProjectDir == projectDir && (now - s_cacheTimestamp) < 5.0) {
        return s_presetCache;
    }

    static const std::unordered_set<std::string> validExts = {
        ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".webp", ".qoi",
        ".PNG", ".JPG", ".JPEG", ".BMP", ".TGA", ".WEBP", ".QOI"
    };

    auto scanDir = [&](const fs::path& presetDir, const fs::path& baseDir) {
        std::error_code ec;
        if (!fs::exists(presetDir, ec) || !fs::is_directory(presetDir, ec)) {
            return;
        }
        for (const auto& entry : fs::directory_iterator(presetDir, ec)) {
            if (ec) continue;
            if (!entry.is_regular_file(ec)) continue;
            std::string ext = entry.path().extension().string();
            if (validExts.count(ext)) {
                std::string rel = fs::relative(entry.path(), baseDir, ec).generic_string();
                std::replace(rel.begin(), rel.end(), '\\', '/');
                result.push_back(rel);
            }
        }
    };

    scanDir(fs::path(projectDir) / "assets" / "PresetTextures", projectDir);

    fs::path cwd = fs::current_path();
    if (cwd != fs::path(projectDir)) {
        scanDir(cwd / "assets" / "PresetTextures", projectDir);
    }

    fs::path cwdParent = cwd.parent_path();
    if (cwdParent != cwd) {
        scanDir(cwdParent / "assets" / "PresetTextures", projectDir);
    }

    s_presetCache = result;
    s_cachedProjectDir = projectDir;
    s_cacheTimestamp = GetTime();
    return result;
}

std::string EnsurePresetTextureInProject(const std::string& presetPath, const std::string& projectDir) {
    if (projectDir.empty() || presetPath.empty()) return presetPath;

    if (presetPath.find("PresetTextures/") != std::string::npos &&
        presetPath.find("..") == std::string::npos) {
        return presetPath;
    }

    fs::path srcAbs;
    std::error_code ec;

    fs::path presetFsPath(presetPath);
    if (presetFsPath.is_relative()) {
        srcAbs = fs::current_path() / presetPath;
    } else {
        srcAbs = presetFsPath;
    }
    srcAbs = fs::canonical(srcAbs, ec);
    if (ec) return presetPath;

    fs::path dstDir = fs::path(projectDir) / "assets" / "PresetTextures";
    fs::create_directories(dstDir, ec);
    if (ec) return presetPath;

    fs::path dstAbs = dstDir / srcAbs.filename();
    if (!CopyFileTo(srcAbs.generic_string(), dstAbs.generic_string())) {
        return presetPath;
    }

    fs::path rel = fs::relative(dstAbs, projectDir, ec);
    if (ec) return presetPath;
    std::string relStr = rel.generic_string();
    std::replace(relStr.begin(), relStr.end(), '\\', '/');
    return relStr;
}

bool IsPresetTexture(const std::string& relPath) {
    const std::string prefix = "assets/PresetTextures/";
    if (relPath.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (relPath[i] != prefix[i]) return false;
    }
    return true;
}

std::string GetLODPath(const std::string& presetPath, int level) {
    if (level <= 0) return presetPath;

    if (!IsPresetTexture(presetPath)) return "";

    fs::path p(presetPath);
    std::string stem = p.stem().string();
    std::string parentDir = p.parent_path().filename().string();

    fs::path lodBase = fs::path("assets") / "LODTextures" / stem;

    std::string suffix = (level == 1) ? "_med" : "_low";
    std::string lodFile = stem + suffix + p.extension().string();

    fs::path lodPath = lodBase / lodFile;

    if (g_projectDir.empty()) return "";
    fs::path absPath = fs::path(g_projectDir) / lodPath;
    std::error_code ec;
    if (!fs::exists(absPath, ec)) return "";

    std::string result = lodPath.generic_string();
    return result;
}

int GetLODLevel(float distance) {
    if (distance < 15.0f) return 0;
    if (distance < 40.0f) return 1;
    return 2;
}

} // namespace textureManager