// PluginStore.cpp - plugin folders on disk: the local (per project) and global (per user) roots.

#include "../../../include/Engine/Frontend/PluginStore.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine/U8Path.hpp"
extern "C" {
#include "../../../include/Engine/Plugin/PluginManifest.h"
}

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace pluginstore {

namespace {

std::string g_globalOverride;

// A folder name that is safe to create under a plugins root: no separators, no "..", no leading dot.
bool SafeName(const std::string& n) {
    if (n.empty() || n == "." || n == ".." || n[0] == '.') return false;
    return n.find_first_of("/\\:*?\"<>|") == std::string::npos;
}

// True when `child` is `root/<a>/<b>` (a plugin folder directly below a type folder).
bool IsPluginFolderOf(const fs::path& root, const fs::path& child) {
    std::error_code ec;
    const fs::path r = fs::weakly_canonical(root, ec), c = fs::weakly_canonical(child, ec);
    return !c.empty() && c.generic_string().rfind(r.generic_string() + "/", 0) == 0 && c.parent_path().parent_path() == r;
}

const Entry* Find(const std::vector<Entry>& all, const std::string& name) {
    for (const Entry& e : all) if (e.name == name) return &e;
    return nullptr;
}

} // namespace

std::string GlobalRoot() {
    if (!g_globalOverride.empty()) return g_globalOverride;
    if (const char* env = std::getenv("FLYENGINE_GLOBAL_PLUGINS"); env && *env) return env;
    const std::string cfg = platform::ConfigDir();
    return cfg.empty() ? std::string() : (U8Path(cfg) / "plugins").generic_string();
}

void SetGlobalRootOverride(const std::string& root) { g_globalOverride = root; }

std::string LocalRoot(const std::string& projectPath) {
    return projectPath.empty() ? std::string() : (U8Path(projectPath) / "plugins").generic_string();
}

bool IsLibrary(const std::string& file) {
    const std::string ext = U8Path(file).extension().string();
#if defined(_WIN32)
    return ext == ".dll";
#else
    return ext == ".so";
#endif
}

std::set<std::string> LoadDisabled(const std::string& root) {
    std::set<std::string> out;
    if (root.empty()) return out;
    std::ifstream in(U8Path(root) / "disabled.txt");
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) out.insert(line);
    }
    return out;
}

void SaveDisabled(const std::string& root, const std::set<std::string>& names) {
    if (root.empty()) return;
    const fs::path file = U8Path(root) / "disabled.txt";
    std::error_code ec;
    if (names.empty()) { fs::remove(file, ec); return; }
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    for (const auto& n : names) out << n << '\n';
}

bool SetEnabled(const std::string& root, const std::string& name, bool enabled) {
    if (root.empty() || name.empty()) return false;
    std::set<std::string> off = LoadDisabled(root);
    if (enabled) off.erase(name); else off.insert(name);
    SaveDisabled(root, off);
    return true;
}

std::vector<Entry> List(const std::string& root, bool global) {
    std::vector<Entry> out;
    if (root.empty()) return out;
    const std::set<std::string> off = LoadDisabled(root);
    std::error_code ec;
    for (const char* typeDir : { "nat", "lib" }) {
        const fs::path typePath = U8Path(root) / typeDir;
        if (!fs::is_directory(typePath, ec)) continue;
        for (const auto& folder : fs::directory_iterator(typePath, ec)) {
            // `ec` belongs to the directory iterator: a folder without build/ must not leave an error in it, or every
            // folder after that one would be skipped.
            std::error_code probe;
            if (!folder.is_directory(probe)) continue;
            const fs::path build = folder.path() / "build";
            if (!fs::is_directory(build, probe)) continue;
            std::error_code buildEc;
            for (const auto& file : fs::directory_iterator(build, buildEc)) {
                if (!file.is_regular_file(probe) || !IsLibrary(file.path().string())) continue;
                Entry e;
                e.name = file.path().stem().string();
                e.displayName = e.name;
                e.type = typeDir;
                e.dir = folder.path().generic_string();
                e.libPath = file.path().string();
                e.global = global;
                e.enabled = off.count(e.name) == 0;
                char* err = nullptr;
                PluginIdentity* id = PluginIdentity_ParseFromFile((folder.path() / "id.json").string().c_str(), &err);
                if (err) free(err);
                if (id) {
                    auto take = [](const char* v) { return std::string(v ? v : ""); };
                    if (id->name && *id->name) e.displayName = id->name;
                    e.version = take(id->version);
                    e.publisher = take(id->publisher);
                    e.description = take(id->description);
                    e.category = take(id->category);
                    e.hasIdentity = true;
                    PluginIdentity_Free(id);
                }
                out.push_back(std::move(e));
                break;   // one library per plugin folder, like the host
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    return out;
}

Result Import(const std::string& rootStr, const std::string& source) {
    Result r;
    if (rootStr.empty()) { r.message = "No plugins folder here: plugins cannot be imported."; return r; }
    std::error_code ec;
    const fs::path src = U8Path(source), root = U8Path(rootStr);

    std::string name, type = "nat";
    fs::path folderSrc, fileSrc;
    if (fs::is_regular_file(src, ec)) {
        if (!IsLibrary(src.string())) { r.message = "Not a plugin library: " + src.filename().string(); return r; }
        fileSrc = src;
        name = src.stem().string();
    } else if (fs::is_directory(src, ec)) {
        folderSrc = src;
        // The user may have picked the build/ folder itself.
        if (!fs::exists(folderSrc / "id.json", ec) && folderSrc.filename() == "build" && fs::exists(folderSrc.parent_path() / "id.json", ec))
            folderSrc = folderSrc.parent_path();
        name = folderSrc.filename().string();
        char* err = nullptr;
        PluginIdentity* id = PluginIdentity_ParseFromFile((folderSrc / "id.json").string().c_str(), &err);
        if (err) free(err);
        if (id) {
            if (id->name && *id->name) name = id->name;
            if (id->type == PLUGIN_TYPE_LIB) type = "lib";
            else if (id->type == PLUGIN_TYPE_FPC) { PluginIdentity_Free(id); r.message = "FPC script plugins are not supported by the editor."; return r; }
            PluginIdentity_Free(id);
        } else if (!fs::exists(folderSrc / "build", ec)) {
            r.message = "That folder has no id.json and no build/ folder, so it does not look like a plugin.";
            return r;
        }
    } else {
        r.message = "Not found: " + source;
        return r;
    }
    if (!SafeName(name)) { r.message = "Unusable plugin name: \"" + name + "\""; return r; }

    const fs::path dest = root / type / name;
    if (fs::exists(dest, ec)) { r.message = "\"" + name + "\" is already installed there. Remove its folder first to replace it."; return r; }
    if (!folderSrc.empty()) {
        // Importing from inside the plugins folder would copy it onto itself.
        const fs::path a = fs::weakly_canonical(folderSrc, ec), b = fs::weakly_canonical(root, ec);
        if (a.generic_string().rfind(b.generic_string() + "/", 0) == 0) { r.message = "That plugin is already inside the plugins folder. Use Rescan."; return r; }
    }

    // Copy next to the destination and rename, so a failed copy never leaves a half-installed plugin.
    const fs::path staging = root / type / (name + ".importing");
    fs::remove_all(staging, ec);
    fs::create_directories(staging / "build", ec);
    if (ec) { r.message = "Could not create " + staging.string() + ": " + ec.message(); return r; }
    if (!fileSrc.empty()) fs::copy_file(fileSrc, staging / "build" / fileSrc.filename(), ec);
    else fs::copy(folderSrc, staging, fs::copy_options::recursive | fs::copy_options::skip_symlinks, ec);
    if (ec) { fs::remove_all(staging, ec); r.message = "Copy failed: " + ec.message(); return r; }
    fs::rename(staging, dest, ec);
    if (ec) { fs::remove_all(staging, ec); r.message = "Could not install into " + dest.string() + ": " + ec.message(); return r; }

    r.ok = true;
    bool hasLib = false;
    for (const auto& f : fs::directory_iterator(dest / "build", ec))
        if (!ec && f.is_regular_file() && IsLibrary(f.path().string())) hasLib = true;
    r.message = hasLib ? "Imported \"" + name + "\"."
                       : "Copied \"" + name + "\", but it has no built library in build/. Build it, then press Rescan.";
    return r;
}

Result Remove(const std::string& root, const std::string& name) {
    Result r;
    const std::vector<Entry> all = List(root, false);
    const Entry* e = Find(all, name);
    if (!e) { r.message = "No such plugin: " + name; return r; }
    if (!IsPluginFolderOf(U8Path(root), U8Path(e->dir))) {
        r.message = "Refusing to delete \"" + name + "\": its folder is not under the plugins folder.";
        return r;
    }
    std::error_code ec;
    fs::remove_all(U8Path(e->dir), ec);
    std::set<std::string> off = LoadDisabled(root);
    if (off.erase(name)) SaveDisabled(root, off);
    if (ec) { r.message = "Could not delete " + e->dir + ": " + ec.message(); return r; }
    r.ok = true;
    r.message = "Removed \"" + name + "\".";
    return r;
}

Result Move(const std::string& fromRoot, const std::string& toRoot, const std::string& name) {
    Result r;
    if (fromRoot.empty() || toRoot.empty()) { r.message = "No plugins folder to move between."; return r; }
    const std::vector<Entry> all = List(fromRoot, false);
    const Entry* e = Find(all, name);
    if (!e) { r.message = "No such plugin: " + name; return r; }
    if (!IsPluginFolderOf(U8Path(fromRoot), U8Path(e->dir))) { r.message = "Refusing to move \"" + name + "\": its folder is not under the plugins folder."; return r; }

    const fs::path from = U8Path(e->dir);
    const fs::path dest = U8Path(toRoot) / e->type / from.filename();
    std::error_code ec;
    if (fs::exists(dest, ec) || Find(List(toRoot, false), name)) { r.message = "\"" + name + "\" already exists on the other side. Remove it there first."; return r; }
    fs::create_directories(dest.parent_path(), ec);
    fs::rename(from, dest, ec);
    if (ec) {   // another filesystem (the config dir and the project often are): copy, then delete
        std::error_code cec;
        fs::copy(from, dest, fs::copy_options::recursive | fs::copy_options::skip_symlinks, cec);
        if (cec) { fs::remove_all(dest, cec); r.message = "Move failed: " + cec.message(); return r; }
        fs::remove_all(from, cec);
    }
    // The enabled state travels with the plugin.
    std::set<std::string> offFrom = LoadDisabled(fromRoot);
    if (offFrom.erase(name)) {
        SaveDisabled(fromRoot, offFrom);
        std::set<std::string> offTo = LoadDisabled(toRoot);
        offTo.insert(name);
        SaveDisabled(toRoot, offTo);
    }
    r.ok = true;
    r.message = "Moved \"" + name + "\".";
    return r;
}

} // namespace pluginstore
