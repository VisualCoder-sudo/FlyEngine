#include "../../../include/Engine/Backend/AudioAssets.hpp"
#include "../../../include/Engine/Backend/TextureManager.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace audioassets {
namespace {

constexpr const char* kManagedFile = ".managed";
constexpr const char* kPendingFile = ".pending";
constexpr uintmax_t kMaxScanBytes = 16u * 1024 * 1024;

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

fs::path Folder(const std::string& projectDir) { return fs::path(projectDir) / kFolder; }

// Projects imported before the folder was renamed keep their sounds in assets/Audio; those stay managed. Where the two names
// are one folder (a case-insensitive file system) it is listed once.
struct Place { fs::path dir; std::string rel; };   // rel: project-relative folder, no trailing slash
std::vector<Place> Places(const std::string& projectDir, bool onlyExisting) {
    std::vector<Place> out;
    std::error_code ec;
    for (const char* rel : { kFolder, kLegacyFolder }) {
        const fs::path dir = fs::path(projectDir) / rel;
        const bool exists = fs::is_directory(dir, ec);
        if (onlyExisting && !exists) continue;
        bool same = false;
        for (const Place& p : out) if (exists && fs::is_directory(p.dir, ec) && fs::equivalent(p.dir, dir, ec)) same = true;
        if (!same) out.push_back({ dir, rel });
    }
    return out;
}

std::set<std::string> ReadList(const fs::path& file) {
    std::set<std::string> out;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) out.insert(line);
    }
    return out;
}

void WriteList(const fs::path& file, const std::set<std::string>& names) {
    std::error_code ec;
    if (names.empty()) { fs::remove(file, ec); return; }
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    for (const std::string& n : names) out << n << '\n';
}

// "Boom Effect (2).WAV" -> "Boom_Effect_2" so the name survives being written into the scene file as it is.
std::string SafeStem(const std::string& stem) {
    std::string s;
    for (unsigned char c : stem) s += (std::isalnum(c) || c == '-' || c == '_') ? static_cast<char>(c) : '_';
    while (!s.empty() && s.front() == '_') s.erase(s.begin());
    while (!s.empty() && s.back() == '_') s.pop_back();
    while (s.find("__") != std::string::npos) s.replace(s.find("__"), 2, "_");
    return s.empty() ? "sound" : s;
}

// Files in assets/Audio with exactly this content, if any.
std::string FindIdentical(const fs::path& folder, const std::string& sourcePath, uintmax_t size, const std::string& hash) {
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (fs::file_size(it->path(), ec) != size) continue;
        if (textureManager::ComputeSHA256(it->path().string()) == hash) return name;
    }
    (void)sourcePath;
    return {};
}

bool IsTextual(const fs::path& p) {
    static const std::set<std::string> kExt = { ".flyproj", ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hxx", ".json", ".txt", ".lua", ".cs" };
    return kExt.count(Lower(p.extension().string())) > 0;
}

// Everything the project says in text, minus imported assets and build output. A managed sound is alive while this mentions
// its path.
std::string ProjectText(const std::string& projectDir) {
    std::string all;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(projectDir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        const std::string name = p.filename().string();
        if (it->is_directory(ec)) {
            if ((!name.empty() && name[0] == '.') || name == "assets" || name == "build" || name == "exports" ||
                name.rfind("build-", 0) == 0 || name.rfind("cmake-build", 0) == 0)
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec) || !IsTextual(p)) continue;
        if (name.size() > 13 && name.compare(name.size() - 13, 13, "_temp.flyproj") == 0) continue;   // the Play copy
        if (fs::file_size(p, ec) > kMaxScanBytes) continue;
        std::ifstream in(p, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        all += ss.str();
        all += '\n';
    }
    return all;
}

} // namespace

bool IsSoundFile(const std::string& path) {
    const std::string ext = Lower(fs::path(path).extension().string());
    return ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac";
}

ImportResult Import(const std::string& projectDir, const std::string& source) {
    ImportResult res;
    std::error_code ec;
    if (projectDir.empty()) { res.error = "no project is open"; return res; }
    if (!fs::is_regular_file(source, ec)) { res.error = "file not found"; return res; }
    if (!IsSoundFile(source)) { res.error = "not a sound file (wav, ogg, mp3, flac)"; return res; }

    // Already in the project: refer to it where it is.
    const fs::path srcAbs = fs::weakly_canonical(source, ec);
    const fs::path projAbs = fs::weakly_canonical(projectDir, ec);
    const fs::path inProject = srcAbs.lexically_relative(projAbs);
    if (!inProject.empty() && *inProject.begin() != "..") {
        res.ok = true;
        res.reused = true;
        res.rel = inProject.generic_string();
        return res;
    }

    const fs::path folder = Folder(projectDir);

    const uintmax_t size = fs::file_size(source, ec);
    const std::string hash = textureManager::ComputeSHA256(source);
    if (ec || hash.empty()) { res.error = "can't read the file"; return res; }

    for (const Place& place : Places(projectDir, true)) {
        if (const std::string same = FindIdentical(place.dir, source, size, hash); !same.empty()) {
            res.ok = true;
            res.reused = true;
            res.rel = place.rel + "/" + same;
            return res;
        }
    }

    fs::create_directories(folder, ec);
    if (ec) { res.error = "can't create " + std::string(kFolder) + ": " + ec.message(); return res; }

    const std::string ext = Lower(fs::path(source).extension().string());
    const std::string stem = SafeStem(fs::path(source).stem().string());
    std::string name = stem + ext;
    if (fs::exists(folder / name, ec)) name = stem + "_" + hash.substr(0, 8) + ext;   // same name, different sound
    fs::copy_file(source, folder / name, fs::copy_options::overwrite_existing, ec);
    if (ec) { res.error = "copy failed: " + ec.message(); return res; }

    std::set<std::string> managed = ReadList(folder / kManagedFile), pending = ReadList(folder / kPendingFile);
    managed.insert(name);
    pending.insert(name);
    WriteList(folder / kManagedFile, managed);
    WriteList(folder / kPendingFile, pending);

    res.ok = true;
    res.rel = std::string(kFolder) + "/" + name;
    return res;
}

std::vector<std::string> Commit(const std::string& projectDir) {
    std::vector<std::string> deleted;
    if (projectDir.empty()) return deleted;
    std::error_code ec;

    std::string text;   // read once, and only if some folder has anything managed
    bool haveText = false;
    for (const Place& place : Places(projectDir, true)) {
        std::set<std::string> managed = ReadList(place.dir / kManagedFile);
        if (managed.empty()) { fs::remove(place.dir / kPendingFile, ec); continue; }
        if (!haveText) { text = ProjectText(projectDir); haveText = true; }
        for (auto it = managed.begin(); it != managed.end();) {
            const std::string rel = place.rel + "/" + *it;
            const bool exists = fs::exists(place.dir / *it, ec);
            if (exists && text.find(rel) != std::string::npos) { ++it; continue; }
            if (exists) {
                fs::remove(place.dir / *it, ec);
                if (!ec) deleted.push_back(rel);
            }
            it = managed.erase(it);
        }
        WriteList(place.dir / kManagedFile, managed);
        fs::remove(place.dir / kPendingFile, ec);   // everything that survived is saved now
    }
    return deleted;
}

std::vector<std::string> DiscardPending(const std::string& projectDir) {
    std::vector<std::string> deleted;
    if (projectDir.empty()) return deleted;
    std::error_code ec;
    for (const Place& place : Places(projectDir, true)) {
        const std::set<std::string> pending = ReadList(place.dir / kPendingFile);
        if (pending.empty()) continue;
        std::set<std::string> managed = ReadList(place.dir / kManagedFile);
        for (const std::string& name : pending) {
            if (!managed.count(name)) continue;   // only ever delete what we imported
            if (fs::remove(place.dir / name, ec)) deleted.push_back(place.rel + "/" + name);
            managed.erase(name);
        }
        WriteList(place.dir / kManagedFile, managed);
        fs::remove(place.dir / kPendingFile, ec);
    }
    return deleted;
}

} // namespace audioassets
