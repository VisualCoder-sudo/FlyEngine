#include "../../../include/Engine/ExportManager.hpp"
#include "../../../include/Engine/PackArchive.hpp"
#include "../../../include/Engine/PostFX.hpp"
#include "../../../include/Engine/PeIcon.hpp"
#include "raylib.h"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine/Backend/TextureManager.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace exporter {
namespace {

bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string HostDistro() {
    std::ifstream in("/etc/os-release");
    std::string line, id, like;
    while (std::getline(in, line)) {
        auto value = [&](const char* key) -> std::string {
            const std::string k = std::string(key) + "=";
            if (line.compare(0, k.size(), k) != 0) return std::string();
            std::string v = line.substr(k.size());
            v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
            return v;
        };
        if (std::string v = value("ID"); !v.empty()) id = v;
        if (std::string v = value("ID_LIKE"); !v.empty()) like = v;
    }
    const std::string all = " " + id + " " + like + " ";
    if (all.find(" arch ") != std::string::npos) return "arch";
    if (all.find(" debian ") != std::string::npos || all.find(" ubuntu ") != std::string::npos) return "debian";
    return std::string();
}

std::string SanitizeName(const std::string& name) {
    std::string out;
    for (unsigned char c : name) out += (std::isalnum(c) || c == '-' || c == '_' || c == '.') ? (char)c : '_';
    return out.empty() ? std::string("Game") : out;
}

// Files that never belong in a shipped game: editor state, caches, scratch, and the C++ sources of scripts and
// plugins (those are compiled into the executable instead, never shipped).
bool Excluded(const fs::path& rel) {
    for (const fs::path& part : rel) {
        const std::string p = part.string();
        if (!p.empty() && p[0] == '.') return true;                       // .build, .texture caches, .pending_imports ...
    }
    const std::string first = rel.empty() ? std::string() : rel.begin()->string();
    if (first == "plugins") return true;
    const std::string name = rel.filename().string();
    if (name == "export.cfg" || name == "compile_flags.txt") return true;
    if (EndsWith(name, "_temp.flyproj")) return true;
    if (first == "Scripts") {
        const std::string ext = Lower(rel.extension().string());
        if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".h" || ext == ".hpp" || ext == ".hxx") return true;
    }
    return false;
}

// The game's icon: a PNG, read whole. Dimensions come from the IHDR chunk so nothing needs a graphics context.
struct IconFile {
    std::vector<uint8_t> bytes;
    unsigned width = 0, height = 0;
};

bool LoadIconFile(const fs::path& project, const std::string& iconPath, IconFile& icon, std::string& error) {
    fs::path file = fs::u8path(iconPath);
    if (file.is_relative()) file = project / file;
    std::ifstream in(file, std::ios::binary);
    if (!in) { error = "Icon not found: " + file.string(); return false; }
    icon.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    const std::vector<uint8_t>& b = icon.bytes;
    if (b.size() < 33 || !std::equal(sig, sig + 8, b.begin()) || std::string(b.begin() + 12, b.begin() + 16) != "IHDR") {
        error = "The icon must be a PNG file: " + file.string();
        return false;
    }
    auto be32 = [&](size_t o) { return ((unsigned)b[o] << 24) | ((unsigned)b[o + 1] << 16) | ((unsigned)b[o + 2] << 8) | b[o + 3]; };
    icon.width = be32(16);
    icon.height = be32(20);
    if (icon.width == 0 || icon.height == 0 || icon.width > 4096 || icon.height > 4096) {
        error = "The icon has an unusable size (" + std::to_string(icon.width) + "x" + std::to_string(icon.height) + ").";
        return false;
    }
    return true;
}

std::string FindProjectFile(const fs::path& dir) {
    std::error_code ec;
    std::vector<std::string> found;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".flyproj") continue;
        if (EndsWith(it->path().stem().string(), "_temp")) continue;
        found.push_back(it->path().filename().string());
    }
    if (found.empty()) return std::string();
    const std::string preferred = dir.filename().string() + ".flyproj";
    if (std::find(found.begin(), found.end(), preferred) != found.end()) return preferred;
    std::sort(found.begin(), found.end());
    return found.front();
}


// --- Prebuilt players for other operating systems ---------------------------------------------------------------
// A game that has no C++ scripts or plugins needs nothing compiled: the data archive is appended to a ready-made
// player for the target OS. Those players are built on that OS (or by CI) and dropped in a "players" folder.

std::vector<fs::path> PlayerSearchDirs() {
    std::vector<fs::path> dirs;
    if (const char* env = std::getenv("FLYENGINE_PLAYERS_DIR")) dirs.push_back(fs::u8path(env));
    const std::string self = platform::ExecutablePath();
    if (!self.empty()) {
        const fs::path dir = fs::u8path(self).parent_path();
        dirs.push_back(dir / "players");
        dirs.push_back(dir / ".." / "share" / "flyengine" / "players");
    }
    for (const std::string& r : platform::DataSearchRoots()) dirs.push_back(fs::u8path(r) / "players");
    return dirs;
}

bool ReadBytes(const fs::path& file, std::streamoff offset, void* dst, size_t n) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    in.seekg(offset);
    return (bool)in.read((char*)dst, (std::streamsize)n);
}

// A 64-bit Windows executable: "MZ", a PE header, machine type x86-64, not a DLL.
bool IsWindowsX64Exe(const fs::path& file) {
    unsigned char mz[2] = {0};
    if (!ReadBytes(file, 0, mz, 2) || mz[0] != 'M' || mz[1] != 'Z') return false;
    uint32_t peOffset = 0;
    if (!ReadBytes(file, 0x3C, &peOffset, 4) || peOffset > (1u << 20)) return false;
    unsigned char hdr[24] = {0};
    if (!ReadBytes(file, peOffset, hdr, sizeof hdr)) return false;
    if (hdr[0] != 'P' || hdr[1] != 'E' || hdr[2] != 0 || hdr[3] != 0) return false;
    const unsigned machine = hdr[4] | (hdr[5] << 8);
    const unsigned characteristics = hdr[22] | (hdr[23] << 8);
    return machine == 0x8664 && (characteristics & 0x2000) == 0;   // IMAGE_FILE_DLL
}

bool IsLinuxExe(const fs::path& file) {
    unsigned char magic[4] = {0};
    return ReadBytes(file, 0, magic, 4) && magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

bool IsWindowsTarget(const std::string& id) { return id == "windows-x64"; }

constexpr bool HostIsWindows() {
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

std::string FindPrebuiltPlayer(const std::string& targetId) {
    if (!IsWindowsTarget(targetId)) return std::string();
    for (const fs::path& dir : PlayerSearchDirs()) {
        for (const fs::path& candidate : { dir / "windows-x64" / "FlyPlayer.exe", dir / "FlyPlayer-windows-x64.exe" }) {
            std::error_code ec;
            if (fs::is_regular_file(candidate, ec) && IsWindowsX64Exe(candidate)) return candidate.string();
        }
    }
    return std::string();
}

// Does `player` run on the target? Stops an Arch player being packed as a Windows game (or the reverse).
bool PlayerMatchesTarget(const std::string& player, const std::string& targetId) {
    const fs::path p = fs::u8path(player);
    return IsWindowsTarget(targetId) ? IsWindowsX64Exe(p) : IsLinuxExe(p);
}

// --- Scripts and plugins built into the player -----------------------------------------------------------------
// The kit (see packaging/make_kit.py) lets us relink FlyPlayer with extra object files: its static libraries, the
// link command, and the compile flags the player was built with. User code is compiled with those flags, so the
// objects match the engine's ABI, and then linked in; nothing of it ships as source or as a loadable library.

struct Kit {
    std::string dir, compiler;
    std::vector<std::string> compileFlags;   // flags the player was built with
    std::vector<std::string> linkArgs;       // template: @KIT@ @OUT@ @USER_OBJECTS@
};

std::vector<std::string> ReadLines(const fs::path& file) {
    std::vector<std::string> lines;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

std::string FindKitDir() {
    std::vector<fs::path> candidates;
    if (const char* env = std::getenv("FLYENGINE_KIT_DIR")) candidates.push_back(fs::u8path(env));
    const std::string self = platform::ExecutablePath();
    if (!self.empty()) {
        const fs::path dir = fs::u8path(self).parent_path();
        candidates.push_back(dir / "kit");
        candidates.push_back(dir / ".." / "share" / "flyengine" / "kit");
    }
    for (const std::string& r : platform::DataSearchRoots()) candidates.push_back(fs::u8path(r) / "kit");
    for (const fs::path& c : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(c / "link.txt", ec) && fs::is_regular_file(c / "kit.txt", ec)) return c.lexically_normal().string();
    }
    return std::string();
}

bool LoadKit(const std::string& dir, Kit& kit, std::string& error) {
    kit.dir = dir;
    for (const std::string& l : ReadLines(fs::u8path(dir) / "kit.txt")) {
        if (l.compare(0, 9, "compiler=") == 0) kit.compiler = l.substr(9);
    }
    kit.compileFlags = ReadLines(fs::u8path(dir) / "compile.txt");
    kit.linkArgs = ReadLines(fs::u8path(dir) / "link.txt");
    if (kit.compiler.empty() || kit.linkArgs.empty()) { error = "The runtime kit in " + dir + " is incomplete."; return false; }
    std::error_code ec;
    if (!fs::exists(fs::u8path(kit.compiler), ec)) { error = "The compiler the kit was built with is missing: " + kit.compiler; return false; }
    return true;
}

std::string Identifier(const std::string& name) {
    std::string out;
    for (unsigned char c : name) out += std::isalnum(c) ? (char)c : '_';
    return out.empty() ? std::string("plugin") : out;
}

bool IsCppSource(const fs::path& p) {
    const std::string e = Lower(p.extension().string());
    return e == ".cpp" || e == ".cc" || e == ".cxx";
}

void CollectSources(const fs::path& dir, std::vector<std::string>& out) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        const std::string name = it->path().filename().string();
        if (!name.empty() && name[0] == '.') { if (it->is_directory(e2)) it.disable_recursion_pending(); continue; }
        if (it->is_regular_file(e2) && IsCppSource(it->path())) out.push_back(it->path().string());
    }
    std::sort(out.begin(), out.end());
}

struct UserCode {
    std::vector<std::string> scripts;
    struct Plugin { std::string name, id; fs::path dir; std::vector<std::string> sources; };
    std::vector<Plugin> plugins;
    std::vector<std::string> binaryOnly;   // plugin folders that have a built library but no source
    bool Any() const { return !scripts.empty() || !plugins.empty(); }
};

UserCode FindUserCode(const fs::path& project) {
    UserCode code;
    CollectSources(project / "Scripts", code.scripts);
    std::error_code ec;
    const fs::path pluginsDir = project / "plugins";
    // Layouts: plugins/<name>/src, plus the editor's older plugins/nat/<name> and plugins/lib/<name>.
    std::vector<fs::path> pluginDirs;
    for (auto it = fs::directory_iterator(pluginsDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string n = it->path().filename().string();
        if (n == "nat" || n == "lib") {
            for (auto sub = fs::directory_iterator(it->path(), ec); !ec && sub != fs::directory_iterator(); sub.increment(ec)) {
                if (sub->is_directory(ec)) pluginDirs.push_back(sub->path());
            }
        } else if (!n.empty() && n[0] != '.') {
            pluginDirs.push_back(it->path());
        }
    }
    std::sort(pluginDirs.begin(), pluginDirs.end());
    for (const fs::path& d : pluginDirs) {
        UserCode::Plugin p;
        p.name = d.filename().string();
        p.id = Identifier(p.name);
        p.dir = d;
        CollectSources(d / "src", p.sources);
        if (!p.sources.empty()) { code.plugins.push_back(std::move(p)); continue; }
        // A folder with a built library and no source cannot be linked in.
        for (auto f = fs::directory_iterator(d / "build", ec); !ec && f != fs::directory_iterator(); f.increment(ec)) {
            const std::string e = Lower(f->path().extension().string());
            if (e == ".so" || e == ".dll" || e == ".dylib") { code.binaryOnly.push_back(p.name); break; }
        }
    }
    return code;
}

std::string TrimmedFirstLines(const std::string& text, size_t maxLines) {
    std::istringstream in(text);
    std::string line, out;
    for (size_t i = 0; i < maxLines && std::getline(in, line); ++i) out += "    " + line + "\n";
    return out;
}

// Compiles `jobs` (each a full argument list) in parallel; returns false and the compiler output on the first failure.
bool CompileAll(const std::string& compiler, const std::vector<std::vector<std::string>>& jobs,
                const LogFn& log, const std::function<bool()>& cancelled, std::string& error) {
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex errMutex;
    const unsigned workers = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), (unsigned)jobs.size()));
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back([&]() {
            for (;;) {
                const size_t i = next.fetch_add(1);
                if (i >= jobs.size() || failed.load() || (cancelled && cancelled())) return;
                const platform::ProcessResult r = platform::RunProcessCapture(compiler, jobs[i], 15 * 60 * 1000);
                if (!r.launched || r.exitCode != 0) {
                    std::lock_guard<std::mutex> lock(errMutex);
                    if (!failed.exchange(true)) {
                        error = r.launched ? "A script or plugin failed to compile:\n" + TrimmedFirstLines(r.output, 40)
                                           : "Could not start the compiler: " + r.error;
                    }
                }
            }
        });
    }
    for (auto& t : pool) t.join();
    (void)log;
    return !failed.load() && !(cancelled && cancelled());
}

// Builds a player with the project's scripts and plugins linked in. Returns its path, or "" with `error` set.
std::string BuildPlayerWithUserCode(const fs::path& project, const UserCode& code, const std::string& workDir,
                                    const LogFn& log, const std::function<bool()>& cancelled, std::string& error) {
    auto say = [&](const std::string& s) { if (log) log(s); };
    const std::string kitDir = FindKitDir();
    if (kitDir.empty()) {
        error = "This project has C++ scripts/plugins, which are compiled into the game, but no runtime kit was found. "
                "Create one with packaging/make_kit.py <build dir> <kit dir> and put it next to the editor (a folder named 'kit') "
                "or point FLYENGINE_KIT_DIR at it.";
        return std::string();
    }
    Kit kit;
    if (!LoadKit(kitDir, kit, error)) return std::string();
    say("Runtime kit: " + kitDir);

    const fs::path work = fs::u8path(workDir);
    std::error_code ec;
    fs::create_directories(work / "obj", ec);

    // Common flags: what the player was built with, release-style, no debug info, no build-machine paths in the result.
    std::vector<std::string> common = kit.compileFlags;
    for (const char* f : { "-O2", "-g0", "-fPIC", "-c" }) common.push_back(f);
    common.push_back("-ffile-prefix-map=" + project.string() + "=.");
    common.push_back("-I" + kit.dir + "/sdk/include");
    common.push_back("-I" + kit.dir + "/include");
    common.push_back("-I" + kit.dir + "/include/rl");

    std::vector<std::vector<std::string>> jobs;
    std::vector<std::string> objects;
    auto addJob = [&](const std::string& source, const std::vector<std::string>& extra) {
        const std::string obj = (work / "obj" / (std::to_string(objects.size()) + "_" + fs::u8path(source).stem().string() + ".o")).string();
        std::vector<std::string> args = common;
        args.insert(args.end(), extra.begin(), extra.end());
        args.push_back(source);
        args.push_back("-o");
        args.push_back(obj);
        jobs.push_back(std::move(args));
        objects.push_back(obj);
    };

    if (!code.scripts.empty()) {
        // The script module (entry point renamed so it can sit inside the executable) and the user's scripts.
        addJob(kit.dir + "/sdk/src/fly_module.cpp", { "-DFlyScript_GetModule=FlyScript_GetModule_Static" });
        for (const std::string& s : code.scripts) addJob(s, { "-I" + (project / "Scripts").string() });
    }
    for (const UserCode::Plugin& p : code.plugins) {
        for (const std::string& s : p.sources) {
            addJob(s, { "-DCP_GetPluginEntry=CP_GetPluginEntry_" + p.id, "-I" + (p.dir / "src").string(),
                        "-I" + (p.dir / "include").string() });
        }
    }

    // The glue that hands them to the NativeScriptHost before main().
    {
        std::ostringstream g;
        g << "// Generated by Export. Registers the scripts and plugins that were compiled into this executable.\n"
          << "#include \"Engine/Scripts/StaticModules.hpp\"\n";
        if (!code.scripts.empty()) g << "extern \"C\" const FlyScriptModule* FlyScript_GetModule_Static(const FlyApi*);\n";
        for (const auto& p : code.plugins) g << "extern \"C\" CP_PluginEntry* CP_GetPluginEntry_" << p.id << "(void);\n";
        g << "namespace {\n";
        if (!code.plugins.empty()) {
            g << "const NativeScript::StaticPlugin kPlugins[] = {\n";
            for (const auto& p : code.plugins) g << "    { \"" << p.name << "\", &CP_GetPluginEntry_" << p.id << " },\n";
            g << "};\n";
        }
        g << "struct Install {\n    Install() {\n";
        if (!code.scripts.empty()) g << "        NativeScript::SetStaticScriptEntry(&FlyScript_GetModule_Static);\n";
        if (!code.plugins.empty()) g << "        NativeScript::SetStaticPlugins(kPlugins, sizeof kPlugins / sizeof kPlugins[0]);\n";
        g << "    }\n} install;\n} // namespace\n";
        const fs::path glue = work / "export_glue.cpp";
        std::ofstream(glue) << g.str();
        addJob(glue.string(), {});
    }

    say("Compiling " + std::to_string(jobs.size() - 1) + " script/plugin source file(s) ...");
    if (!CompileAll(kit.compiler, jobs, log, cancelled, error)) {
        if (error.empty()) error = "Cancelled";
        return std::string();
    }

    // Link: the kit's own link command, with the user objects added and the result written to the work folder.
    const std::string out = (work / "player").string();
    std::vector<std::string> args;
    for (size_t i = 0; i < kit.linkArgs.size(); ++i) {
        const std::string& t = kit.linkArgs[i];
        if (i == 0 && t == kit.compiler) continue;   // the first word of the template is the compiler itself
        if (t == "@USER_OBJECTS@") { args.insert(args.end(), objects.begin(), objects.end()); continue; }
        std::string a = t;
        for (size_t pos; (pos = a.find("@KIT@")) != std::string::npos;) a.replace(pos, 5, kit.dir);
        if (a == "@OUT@") a = out;
        args.push_back(a);
    }
    args.push_back("-s");                       // no symbol table: nothing to read names of the game's code from
    args.push_back("-Wl,--gc-sections");
    say("Linking the game ...");
    const platform::ProcessResult r = platform::RunProcessCapture(kit.compiler, args, 30 * 60 * 1000);
    if (!r.launched || r.exitCode != 0) {
        error = "Linking failed:\n" + TrimmedFirstLines(r.launched ? r.output : r.error, 40);
        return std::string();
    }
    return out;
}

} // namespace

std::vector<Target> Targets() {
    const std::string distro = HostDistro();
    std::vector<Target> t(3);
    t[0] = { "linux-arch", "Linux (Arch)", false, "" };
    t[1] = { "linux-debian", "Linux (Debian)", false, "" };
    t[2] = { "windows-x64", "Windows (x86-64)", false, "" };
#if defined(__linux__)
    if (distro == "arch") t[0].available = true;
    if (distro == "debian") t[1].available = true;
    if (!t[0].available) t[0].reason = "Exporting for Arch from this system needs a container (podman or docker); not supported yet.";
    if (!t[1].available) t[1].reason = "Exporting for Debian from this system needs a container (podman or docker); not supported yet.";
#else
    for (int i = 0; i < 2; ++i) t[i].reason = "Linux exports are only available on a Linux machine for now.";
#endif
    if (HostIsWindows()) {
        t[2].available = !FindPlayerExecutable().empty();
        if (!t[2].available) t[2].reason = "FlyPlayer.exe was not found next to the editor.";
    } else {
        t[2].available = !FindPrebuiltPlayer("windows-x64").empty();
        if (!t[2].available) {
            t[2].reason = "Needs a 64-bit Windows FlyPlayer.exe in a 'players/windows-x64' folder next to the editor "
                          "(or FLYENGINE_PLAYERS_DIR). Only games without C++ scripts or plugins can be exported this way.";
        }
    }
    return t;
}

std::string DefaultTarget() {
    for (const Target& t : Targets()) if (t.available) return t.id;
    return Targets().front().id;
}

Config LoadConfig(const std::string& projectDir, const std::string& projectName) {
    Config cfg;
    cfg.target = DefaultTarget();
    cfg.exeName = SanitizeName(projectName);
    cfg.windowTitle = projectName;
    cfg.fpsCap = gfx::Quality().fpsCap;
    std::ifstream in(fs::u8path(projectDir) / "export.cfg");
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        const std::string key = line.substr(0, sp), v = line.substr(sp + 1);
        if (key == "target") cfg.target = v;
        else if (key == "outputDir") cfg.outputDir = v;
        else if (key == "exeName") cfg.exeName = SanitizeName(v);
        else if (key == "windowTitle") cfg.windowTitle = v;
        else if (key == "icon") cfg.iconPath = v;
        else if (key == "linuxLauncher") cfg.linuxLauncher = std::atoi(v.c_str()) != 0;
        else if (key == "windowWidth") cfg.windowWidth = std::max(320, std::atoi(v.c_str()));
        else if (key == "windowHeight") cfg.windowHeight = std::max(240, std::atoi(v.c_str()));
        else if (key == "fullscreen") cfg.fullscreen = std::atoi(v.c_str()) != 0;
        else if (key == "fpsCap") cfg.fpsCap = std::clamp(std::atoi(v.c_str()), 0, 1000);
        else if (key == "assetsFolder") cfg.assetsFolder = std::atoi(v.c_str()) != 0;
    }
    return cfg;
}

bool SaveConfig(const std::string& projectDir, const Config& cfg) {
    std::ofstream out(fs::u8path(projectDir) / "export.cfg", std::ios::trunc);
    if (!out) return false;
    out << "target " << cfg.target << "\noutputDir " << cfg.outputDir << "\nexeName " << cfg.exeName
        << "\nwindowTitle " << cfg.windowTitle << "\nicon " << cfg.iconPath << "\nlinuxLauncher " << (cfg.linuxLauncher ? 1 : 0) << "\nwindowWidth " << cfg.windowWidth
        << "\nwindowHeight " << cfg.windowHeight << "\nfullscreen " << (cfg.fullscreen ? 1 : 0)
        << "\nfpsCap " << cfg.fpsCap << "\nassetsFolder " << (cfg.assetsFolder ? 1 : 0) << "\n";
    return out.good();
}

// One line per file, "<size><TAB><archive-relative path>"; paths never contain a tab or a newline we would ship.
std::string AssetsManifestText(const std::vector<vfs::ExternalFile>& files) {
    std::ostringstream out;
    out << "folder " << kAssetsFolder << "\n";
    for (const vfs::ExternalFile& f : files) out << f.size << '\t' << f.path << '\n';
    return out.str();
}

std::vector<vfs::ExternalFile> ParseAssetsManifest(const std::string& text) {
    std::vector<vfs::ExternalFile> files;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) continue;   // the "folder ..." line, or junk
        vfs::ExternalFile f;
        f.size = std::strtoull(line.substr(0, tab).c_str(), nullptr, 10);
        f.path = line.substr(tab + 1);
        if (!f.path.empty()) files.push_back(std::move(f));
    }
    return files;
}

std::string FindPlayerExecutable() {
    const std::string self = platform::ExecutablePath();
    if (self.empty()) return std::string();
    const fs::path dir = fs::u8path(self).parent_path();
#if defined(_WIN32)
    // Windows file names are case-insensitive, so "FlyPlayer" would also match a Linux "Flyplayer" ELF left in a shared
    // build folder. Only .exe names are candidates here.
    for (const char* name : { "FlyPlayer.exe", "FlyPlayer_win.exe" }) {
#else
    for (const char* name : { "FlyPlayer", "flyplayer", "FLYPLAYER", "Flyplayer", "FlyPlayer.exe",
                              // Copies collected into exe/ carry a platform tag (see fly_collect_exe in CMakeLists.txt).
                              "FlyPlayer_win.exe", "FlyPlayer_deb", "FlyPlayer_arch", "FlyPlayer_linux", "FlyPlayer_mac" }) {
#endif
        std::error_code ec;
        const fs::path p = dir / name;
        // The case-insensitive symlinks CMake makes all resolve to the same file; any regular one will do.
        if (fs::is_regular_file(p, ec)) return p.string();
    }
    return std::string();
}

Result Run(const std::string& projectDir, const Config& cfg, const LogFn& log, const ProgressFn& progress) {
    Result res;
    auto say = [&](const std::string& s) { if (log) log(s); };
    auto fail = [&](const std::string& why) { res.ok = false; res.error = why; say("Export failed: " + why); return res; };
    float lastReported = 0.0f;
    auto report = [&](float f) { lastReported = f; return !progress || progress(f); };

    const fs::path project = fs::u8path(projectDir);
    std::error_code ec;
    if (!fs::is_directory(project, ec)) return fail("Project folder not found: " + projectDir);
    const std::string flyproj = FindProjectFile(project);
    if (flyproj.empty()) return fail("No .flyproj file found in " + projectDir);

    const std::vector<Target> targets = Targets();
    auto tgt = std::find_if(targets.begin(), targets.end(), [&](const Target& t) { return t.id == cfg.target; });
    if (tgt == targets.end()) return fail("Unknown target: " + cfg.target);
    if (!tgt->available) return fail(tgt->label + " is not available: " + tgt->reason);

    if (cfg.outputDir.empty()) return fail("Choose an output folder.");
    const bool windowsTarget = IsWindowsTarget(cfg.target);
    const bool crossBuild = windowsTarget && !HostIsWindows();   // a player built elsewhere: nothing can be compiled for it here
    std::string player = cfg.playerExe;
    if (player.empty()) player = crossBuild ? FindPrebuiltPlayer(cfg.target) : FindPlayerExecutable();
    if (player.empty() || !fs::is_regular_file(fs::u8path(player), ec)) {
        return fail(crossBuild ? "No 64-bit Windows FlyPlayer.exe found (see the target's hint in the Export dialog)."
                               : "Could not find FlyPlayer next to the editor.");
    }
    if (!PlayerMatchesTarget(player, cfg.target)) {
        return fail("'" + player + "' is not a " + tgt->label + " executable, so it cannot be the base of this export.");
    }

    say("Exporting '" + flyproj + "' for " + tgt->label + " ...");

    IconFile icon;
    if (!cfg.iconPath.empty()) {
        std::string iconError;
        if (!LoadIconFile(project, cfg.iconPath, icon, iconError)) return fail(iconError);
        say("Icon: " + cfg.iconPath + " (" + std::to_string(icon.width) + "x" + std::to_string(icon.height) + ")");
        if (icon.width != icon.height) say("Warning: the icon is not square; it will look stretched.");
        else if (icon.width < 256) say("Warning: the icon is smaller than 256x256; it may look blurry.");
    }

    // Scripts and plugins are compiled into the executable, so a project that has any needs its own link of the player.
    std::string basePlayer = player;
    std::string workDir;
    const UserCode userCode = FindUserCode(project);
    if (!userCode.binaryOnly.empty()) {
        std::string names;
        for (const std::string& n : userCode.binaryOnly) names += (names.empty() ? "" : ", ") + n;
        return fail("Plugin(s) with a built library but no source: " + names +
                    ". Export compiles plugins into the game, so put their source in plugins/<name>/src.");
    }
    struct WorkCleanup {
        std::string dir;
        ~WorkCleanup() { if (!dir.empty()) { std::error_code e; fs::remove_all(fs::u8path(dir), e); } }
    } cleanup;
    if (userCode.Any() && crossBuild) {
        return fail("This project has C++ scripts/plugins, which are compiled into the game, and that cannot be done for " +
                    tgt->label + " from this system (it needs that OS's compiler). Export from a " + tgt->label +
                    " machine, or remove the scripts/plugins.");
    }
    if (userCode.Any()) {
        say("C++ code to build in: " + std::to_string(userCode.scripts.size()) + " script file(s), " +
            std::to_string(userCode.plugins.size()) + " plugin(s)");
        workDir = (fs::temp_directory_path() / ("fly_export_" + std::to_string((unsigned long long)std::time(nullptr)))).string();
        cleanup.dir = workDir;
        std::string buildError;
        basePlayer = BuildPlayerWithUserCode(project, userCode, workDir, log, [&]() { return !report(lastReported); }, buildError);
        if (basePlayer.empty()) return fail(buildError);
    }
    say("Player: " + basePlayer);

    // Windows shows the icon of the .exe file itself, so it goes into a copy of the player's resources first.
    if (windowsTarget && !icon.bytes.empty()) {
        Image img = LoadImageFromMemory(".png", icon.bytes.data(), (int)icon.bytes.size());
        if (img.data == nullptr) return fail("Could not decode the icon PNG.");
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        if (cleanup.dir.empty()) {
            cleanup.dir = (fs::temp_directory_path() / ("fly_export_" + std::to_string((unsigned long long)std::time(nullptr)))).string();
        }
        fs::create_directories(fs::u8path(cleanup.dir), ec);
        const fs::path iconPlayer = fs::u8path(cleanup.dir) / "player_icon.exe";
        fs::copy_file(fs::u8path(basePlayer), iconPlayer, fs::copy_options::overwrite_existing, ec);
        std::string peError;
        const int n = ec ? 0 : peicon::ReplaceExeIcon(iconPlayer.string(), (const uint8_t*)img.data, img.width, img.height, peError);
        UnloadImage(img);
        if (n > 0) {
            basePlayer = iconPlayer.string();
            say("Windows file icon set (" + std::to_string(n) + " sizes).");
        } else {
            say("Warning: the .exe's file icon was not changed (" + (ec ? ec.message() : peError) +
                "). The window icon still applies.");
        }
    }

    // --- Stage the project's files.
    std::vector<pack::Entry> entries;
    std::set<std::string> seen;
    unsigned long long projectBytes = 0;
    bool hasBasicTerrain = false;
    for (auto it = fs::recursive_directory_iterator(project, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        const fs::path rel = fs::relative(it->path(), project, e2);
        if (e2 || rel.empty()) continue;
        if (Excluded(rel)) {
            if (it->is_directory(e2)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(e2)) continue;
        pack::Entry e;
        e.path = rel.generic_string();
        e.diskPath = it->path().string();
        projectBytes += fs::file_size(it->path(), e2);
        if (e.path == "basicterrain.bt") hasBasicTerrain = true;
        seen.insert(e.path);
        entries.push_back(std::move(e));
    }
    say("Project files: " + std::to_string(entries.size()) + " (" + std::to_string(projectBytes / (1024 * 1024)) + " MB)");

    // --- Assets folder mode: the project's assets/ stay outside the executable, in a folder beside it. The executable
    // only records what belongs there, so the game can tell when the folder is missing or incomplete.
    struct ExternalCopy { std::string rel; std::string disk; };
    std::vector<ExternalCopy> externals;
    if (cfg.assetsFolder) {
        std::vector<pack::Entry> kept;
        for (pack::Entry& e : entries) {
            if (e.path.rfind("assets/", 0) == 0 && e.path.size() > 7) externals.push_back({ e.path, e.diskPath });
            else kept.push_back(std::move(e));
        }
        entries = std::move(kept);
        if (!externals.empty()) {
            std::vector<vfs::ExternalFile> listed;
            for (const ExternalCopy& x : externals) {
                std::error_code e3;
                listed.push_back({ x.rel, (uint64_t)fs::file_size(fs::u8path(x.disk), e3) });
            }
            const std::string text = AssetsManifestText(listed);
            pack::Entry m;
            m.path = kAssetsManifest;
            m.fromBytes = true;
            m.bytes.assign(text.begin(), text.end());
            entries.push_back(std::move(m));
            say("Assets folder: " + std::to_string(externals.size()) + " file(s) go beside the game in '" + kAssetsFolder + "'.");
        }
    }

    // --- The engine's own data, only what the runtime can ask for.
    const std::string dataRoot = platform::FoundDataRoot();
    if (hasBasicTerrain) {
        if (dataRoot.empty()) return fail("Could not find the engine's data folder (set FLYENGINE_DATA_DIR).");
        const fs::path root = fs::u8path(dataRoot);
        std::vector<std::pair<std::string, fs::path>> engineFiles;
        const fs::path leafy = root / "assets" / "PresetTextures" / "LeafyGrass.qoi";
        if (fs::is_regular_file(leafy, ec)) engineFiles.push_back({ "__engine/assets/PresetTextures/LeafyGrass.qoi", leafy });
        const fs::path texDir = root / "assets" / "Textures" / "TerrainTextures";
        for (auto it = fs::directory_iterator(texDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const std::string ext = Lower(it->path().extension().string());
            if (it->is_regular_file(ec) && (ext == ".qoi" || ext == ".png")) {
                engineFiles.push_back({ "__engine/assets/Textures/TerrainTextures/" + it->path().filename().string(), it->path() });
            }
        }
        std::sort(engineFiles.begin(), engineFiles.end());
        // Identical files (the default albedo is also a terrain layer) are stored once.
        std::vector<std::pair<std::string, std::string>> hashes;   // (sha, path)
        for (auto& [vpath, disk] : engineFiles) {
            pack::Entry e;
            e.path = vpath;
            e.diskPath = disk.string();
            const std::string sha = textureManager::ComputeSHA256(disk.string()) + std::to_string(fs::file_size(disk, ec));
            auto same = std::find_if(hashes.begin(), hashes.end(), [&](const auto& h) { return h.first == sha; });
            if (same != hashes.end()) e.aliasOf = same->second;
            else hashes.push_back({ sha, vpath });
            entries.push_back(std::move(e));
        }
        say("Engine terrain textures: " + std::to_string(engineFiles.size()));
    }
    if (!report(0.05f)) return fail("Cancelled");

    // --- How the game presents itself, and the graphics settings chosen now.
    {
        std::ostringstream cfgText;
        cfgText << "projectFile " << flyproj << "\nname " << fs::u8path(flyproj).stem().string()
                << "\ntitle " << (cfg.windowTitle.empty() ? fs::u8path(flyproj).stem().string() : cfg.windowTitle)
                << "\nwidth " << cfg.windowWidth << "\nheight " << cfg.windowHeight
                << "\nfullscreen " << (cfg.fullscreen ? 1 : 0) << "\nexe " << SanitizeName(cfg.exeName) << "\n";
        pack::Entry meta;
        meta.path = "__meta/export.cfg";
        meta.fromBytes = true;
        const std::string text = cfgText.str();
        meta.bytes.assign(text.begin(), text.end());
        entries.push_back(meta);

        if (!icon.bytes.empty()) {
            pack::Entry ic;
            ic.path = "__meta/icon.png";
            ic.fromBytes = true;
            ic.bytes = icon.bytes;
            entries.push_back(std::move(ic));
        }

        gfx::RenderQuality exportQuality = gfx::Quality();   // a copy: this may run on a worker thread
        exportQuality.fpsCap = cfg.fpsCap;
        const std::string graphics = gfx::QualitySettingsText(exportQuality);
        pack::Entry g;
        g.path = "__meta/graphics.cfg";
        g.fromBytes = true;
        g.bytes.assign(graphics.begin(), graphics.end());
        entries.push_back(g);
    }

    // --- Pack onto a copy of the player.
    const fs::path out = fs::u8path(cfg.outputDir) / (SanitizeName(cfg.exeName) + (windowsTarget ? ".exe" : ""));
    say("Writing " + out.string() + " ...");
    std::string error;
    const float packEnd = externals.empty() ? 1.0f : 0.9f;
    const bool ok = pack::BuildExecutable(basePlayer, out.string(), entries, error, [&](size_t done, size_t total) {
        return report(0.05f + (packEnd - 0.05f) * (total ? (float)done / (float)total : 1.0f));
    });
    if (!ok) return fail(error);

    if (!externals.empty()) {
        const fs::path assetsRoot = fs::u8path(cfg.outputDir) / kAssetsFolder;
        size_t copied = 0;
        for (const ExternalCopy& x : externals) {
            const fs::path dst = assetsRoot / fs::u8path(x.rel.substr(7));   // without "assets/"
            fs::create_directories(dst.parent_path(), ec);
            fs::copy_file(fs::u8path(x.disk), dst, fs::copy_options::overwrite_existing, ec);
            if (ec) return fail("Could not write " + dst.string() + ": " + ec.message());
            if (!report(0.9f + 0.1f * (float)(++copied) / (float)externals.size())) return fail("Cancelled");
        }
        say("Wrote " + std::to_string(externals.size()) + " file(s) to " + assetsRoot.string() +
            ". Ship this folder next to the game.");
    }

    // Optional: Linux has no icon inside an executable, so launchers need the PNG and a .desktop file beside it.
    if (!icon.bytes.empty() && !windowsTarget && cfg.linuxLauncher) {
        const std::string base = SanitizeName(cfg.exeName);
        std::ofstream(fs::u8path(cfg.outputDir) / (base + ".png"), std::ios::binary)
            .write((const char*)icon.bytes.data(), (std::streamsize)icon.bytes.size());
        std::ofstream(fs::u8path(cfg.outputDir) / (base + ".desktop"))
            << "[Desktop Entry]\nType=Application\nName=" << (cfg.windowTitle.empty() ? base : cfg.windowTitle)
            << "\nExec=./" << base << "\nIcon=./" << base << ".png\nStartupWMClass=" << base << "\nTerminal=false\nCategories=Game;\n";
        say("Wrote " + base + ".png and " + base + ".desktop (launcher entry; edit Exec/Icon to absolute paths when installing).");
    }

    res.ok = true;
    res.outputPath = out.string();
    res.bytes = fs::file_size(out, ec);
    say("Done: " + out.string() + " (" + std::to_string(res.bytes / (1024 * 1024)) + " MB)");
    return res;
}

} // namespace exporter
