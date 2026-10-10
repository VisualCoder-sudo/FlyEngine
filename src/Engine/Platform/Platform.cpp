// Platform.cpp -- well-known locations, font discovery, and desktop helpers.
//
// Everything here replaces a direct Win32 call with a POSIX/XDG equivalent.

#include "Engine/Platform/Platform.hpp"
#include "Engine/Platform/PlatformInternal.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <shlobj.h>
    #include <shellapi.h>
#else
    #include <pwd.h>
    #include <unistd.h>
#endif

namespace platform {
namespace {

namespace fs = std::filesystem;

std::string EnvOrEmpty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

bool FileExists(const std::string& p) {
    if (p.empty()) return false;
    std::error_code ec;
    return fs::exists(fs::u8path(p), ec);
}

// The XDG user-dirs file maps XDG_DOCUMENTS_DIR to $HOME/Documents and is
// what `xdg-user-dir DOCUMENTS` would report. Read it directly so we do not
// have to shell out at startup.
std::string DocumentsFromUserDirs(const std::string& home) {
    const std::string configHome = EnvOrEmpty("XDG_CONFIG_HOME").empty()
                                       ? home + "/.config"
                                       : EnvOrEmpty("XDG_CONFIG_HOME");
    const std::string file = configHome + "/user-dirs.dirs";
    std::ifstream in(file);
    if (!in) return std::string();
    std::string line;
    while (std::getline(in, line)) {
        // Format: XDG_DOCUMENTS_DIR="$HOME/Documents"
        if (line.rfind("XDG_DOCUMENTS_DIR", 0) != 0) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string val = line.substr(eq + 1);
        const size_t b = val.find('"');
        const size_t e = val.rfind('"');
        if (b == std::string::npos || e <= b) continue;
        val = val.substr(b + 1, e - b - 1);
        // Expand $HOME.
        if (val.rfind("$HOME", 0) == 0) val = home + val.substr(5);
        return val;
    }
    return std::string();
}

} // namespace

PathPrompt& GetPathPrompt() {
    static PathPrompt prompt;
    return prompt;
}

std::string ExecutablePath() {
#if defined(_WIN32)
    char buf[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return n ? std::string(buf, n) : std::string();
#else
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.string();
    return std::string();
#endif
}

std::string FindOnPath(const std::vector<std::string>& names) {
    const std::string envPath = EnvOrEmpty("PATH");
    if (envPath.empty()) return {};
#if defined(_WIN32)
    const char sep = ';';
    const std::vector<std::string> suffixes = {"", ".cmd", ".exe", ".bat"};
#else
    const char sep = ':';
    const std::vector<std::string> suffixes = {""};
#endif
    size_t start = 0;
    while (start <= envPath.size()) {
        const size_t end = envPath.find(sep, start);
        const std::string dir =
            envPath.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            for (const std::string& name : names) {
                for (const std::string& suffix : suffixes) {
                    std::error_code ec;
                    const fs::path full = fs::path(dir) / (name + suffix);
                    if (fs::is_regular_file(full, ec)) return full.string();
                }
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

std::string UserHomeDir() {
#if defined(_WIN32)
    const char* profile = std::getenv("USERPROFILE");
    if (profile && *profile) return profile;
    const char* drive = std::getenv("HOMEDRIVE");
    const char* path = std::getenv("HOMEPATH");
    if (drive && path) return std::string(drive) + path;
    return std::string();
#else
    std::string h = EnvOrEmpty("HOME");
    if (!h.empty()) return h;
    // Fall back to the passwd database when HOME is unset (e.g. setuid).
    if (const passwd* pw = ::getpwuid(::getuid())) return pw->pw_dir ? pw->pw_dir : "";
    return std::string();
#endif
}

std::string ConfigDir() {
#if defined(_WIN32)
    // %APPDATA% (Roaming)
    std::string appdata = EnvOrEmpty("APPDATA");
    if (appdata.empty()) {
        char buf[MAX_PATH] = {0};
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf))) {
            appdata = buf;
        }
    }
    if (appdata.empty()) return UserHomeDir();
    return (fs::u8path(appdata) / "Flyengine").string();
#else
    std::string xdg = EnvOrEmpty("XDG_CONFIG_HOME");
    if (xdg.empty()) {
        const std::string home = UserHomeDir();
        if (home.empty()) return std::string();
        xdg = home + "/.config";
    }
    return (fs::u8path(xdg) / "flyengine").string();
#endif
}

std::string DocumentsDir() {
#if defined(_WIN32)
    char buf[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        return std::string(buf);
    }
    return UserHomeDir();
#else
    const std::string home = UserHomeDir();
    const std::string fromFile = DocumentsFromUserDirs(home);
    if (!fromFile.empty() && FileExists(fromFile)) return fromFile;
    if (!home.empty() && FileExists(home + "/Documents")) return home + "/Documents";
    return home;
#endif
}

std::vector<std::string> FontCandidates() {
    // Ordered by preference. Arial on Windows; DejaVu Sans is the near-universal
    // Linux equivalent and is also what most distros ship by default. Liberation
    // Sans is the metric-compatible Arial clone, so it goes next for projects
    // that care about text matching the Windows build.
    return {
#if defined(_WIN32)
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
#endif
    };
}

const std::string& ResolveFontPath() {
    // A font next to the working directory (or one level above it, which is
    // where an out-of-source build directory puts it) wins over the system
    // candidates. The "../arial.ttf" entry is what Benchmark.cpp used to
    // probe on its own; keeping it here means every caller gets it.
    static const std::string resolved = [] {
        for (const std::string& local : { std::string("arial.ttf"), std::string("../arial.ttf") }) {
            if (FileExists(local)) return local;
        }
        for (const std::string& candidate : FontCandidates()) {
            if (FileExists(candidate)) return candidate;
        }
        return std::string();
    }();
    return resolved;
}

bool HasUsableFont() {
    return !ResolveFontPath().empty();
}

// --- Engine data root ------------------------------------------------------
//
// The engine ships two things it has to read at runtime: assets/ (editor
// icons, the logo, preset textures) and shaders/. In the repository they sit
// side by side at the top level; an install puts both under
// <prefix>/share/flyengine. CMake also flattens the shaders next to the
// executable so an in-tree build can run from the build directory.
//
// Rather than have every call site carry its own list of "../.." guesses, the
// search happens once here and is cached.

namespace {

// A directory counts as the data root if it holds the assets tree. The shaders
// are handled separately because a dev build keeps them flattened.
bool LooksLikeDataRoot(const fs::path& dir) {
    if (dir.empty()) return false;
    std::error_code ec;
    return fs::is_directory(dir / "assets", ec);
}

void AddRoot(std::vector<std::string>& roots, const fs::path& p) {
    std::error_code ec;
    const fs::path clean = p.lexically_normal();
    if (clean.empty()) return;
    const std::string s = clean.string();
    for (const std::string& existing : roots) {
        if (existing == s) return;
    }
    roots.push_back(s);
}

// Filled on first use. Cheap to compute and never changes during a run, but
// the engine touches a few hundred paths while the editor UI is building
// itself, so it is worth computing only once.
const std::vector<std::string>& CachedRoots() {
    static const std::vector<std::string> roots = [] {
        std::vector<std::string> out;

        // 1. An explicit override, checked first so it always wins. This is
        //    how a packager relocates the data, and how the test harness
        //    points the engine at a fixture tree.
        const std::string env = EnvOrEmpty("FLYENGINE_DATA_DIR");
        if (!env.empty()) AddRoot(out, fs::u8path(env));

        // 2. The working directory and its parents. This is the "cd build &&
        //    ./Flyengine" and "cd build/Debug && ./Flyengine" developer case.
        //    Four levels covers out-of-source build dirs, which are the ones
        //    that actually occur; deeper nesting is a misconfiguration.
        std::error_code ec;
        fs::path cwd = fs::current_path(ec);
        for (int i = 0; i < 5 && !cwd.empty(); ++i) {
            AddRoot(out, cwd);
            const fs::path parent = cwd.parent_path();
            if (parent == cwd) break;
            cwd = parent;
        }

        // 3. Relative to the executable, which is the only thing that is
        //    stable when launched from a menu:
        //      <exe>/                       a relocatable/portable directory
        //      <exe>/../share/flyengine     <prefix>/bin -> <prefix>/share
        //      <exe>/../share               the same, without the component name
        //      <exe>/../lib/flyengine       for packagers who prefer lib/
        //      <exe>/../../share/flyengine  the CMake build-tree bin/ layout
        const std::string exe = ExecutablePath();
        if (!exe.empty()) {
            const fs::path exeDir = fs::u8path(exe).parent_path();
            AddRoot(out, exeDir);
            AddRoot(out, exeDir / ".." / "share" / "flyengine");
            AddRoot(out, exeDir / ".." / "share");
            AddRoot(out, exeDir / ".." / "lib" / "flyengine");
            AddRoot(out, exeDir / ".." / ".." / "share" / "flyengine");
        }

        // 4. The install prefix this binary was configured for. Last, because
        //    a self-contained bundle or an explicit override should have
        //    already matched above.
#if defined(FLYENGINE_DATA_DIR)
        AddRoot(out, fs::u8path(FLYENGINE_DATA_DIR));
#endif
        return out;
    }();
    return roots;
}

} // namespace

std::vector<std::string> DataSearchRoots() {
    return CachedRoots();
}

std::string FoundDataRoot() {
    for (const std::string& r : CachedRoots()) {
        if (LooksLikeDataRoot(fs::u8path(r))) return r;
    }
    return std::string();
}

std::string ResolveAsset(const std::string& repoRelative) {
    if (repoRelative.empty()) return std::string();

    // An absolute path is the caller telling us exactly where it is; respect
    // that instead of appending it to a search root.
    const fs::path rel = fs::u8path(repoRelative);
    if (rel.is_absolute()) {
        return FileExists(repoRelative) ? repoRelative : std::string();
    }

    for (const std::string& r : CachedRoots()) {
        const fs::path candidate = fs::u8path(r) / rel;
        if (FileExists(candidate.string())) return candidate.string();
    }
    return std::string();
}

std::string ResolveShader(const std::string& fileName) {
    if (fileName.empty()) return std::string();

    if (fs::u8path(fileName).is_absolute()) {
        return FileExists(fileName) ? fileName : std::string();
    }

    // The caller may pass either a bare name ("terrain.vert") or a
    // repo-relative one ("shaders/terrain.vert"); honour the form given first,
    // then try the other, then the flattened build-directory copy.
    const bool looksQualified = fileName.find('/') != std::string::npos;
    for (const std::string& r : CachedRoots()) {
        const fs::path root = fs::u8path(r);
        if (looksQualified) {
            if (FileExists((root / fileName).string())) return (root / fileName).string();
        } else {
            if (FileExists((root / "shaders" / fileName).string()))
                return (root / "shaders" / fileName).string();
            if (FileExists((root / fileName).string())) return (root / fileName).string();
        }
    }
    return std::string();
}

void RevealInFileManager(const std::string& path) {
    if (path.empty()) return;
#if defined(_WIN32)
    std::string win = path;
    for (char& c : win) if (c == '/') c = '\\';
    const std::string cmd = "explorer.exe /select,\"" + win + "\"";
    ShellExecuteA(nullptr, "open", "explorer.exe", ("/select,\"" + win + "\"").c_str(), nullptr, SW_SHOWNORMAL);
    (void)cmd;
#else
    // No Linux file manager has a portable "reveal/select" verb, so open the
    // containing directory. `gio open` can select a file where available.
    std::error_code ec;
    fs::path target = fs::u8path(path);
    fs::path dir = fs::is_directory(target, ec) ? target : target.parent_path();
    if (dir.empty()) return;
    RunProcessCapture("xdg-open", {dir.string()}, 10000);
#endif
}

void OpenWithDefaultApp(const std::string& path) {
    if (path.empty()) return;
#if defined(_WIN32)
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    RunProcessCapture("xdg-open", {path}, 10000);
#endif
}

bool RemoveTree(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::remove_all(fs::u8path(path), ec) != static_cast<std::uintmax_t>(-1) && !ec;
}

// --- internal helpers -----------------------------------------------------

namespace internal {

bool HelperAvailable(const std::string& name) {
    if (name.empty()) return false;
    if (name.find('/') != std::string::npos) return FileExists(name);

    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return false;
    const std::string path(pathEnv);
#if defined(_WIN32)
    const char sep = ';';
    const char* exeExt[] = {".exe", ".com", ".bat", nullptr};
#else
    const char sep = ':';
    // The empty string is the extension-less name, i.e. the helper itself. It
    // has to be here: the loop below is NULL-terminated, so a list of just
    // {nullptr} made the very first test `exeExt[0]` false and the loop body
    // never ran. HelperAvailable() then returned false for every helper on
    // every Unix, which is why the Import button (and every other file dialog)
    // fell back to the "type the path" prompt even with zenity installed.
    const char* exeExt[] = {"", nullptr};
#endif
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find(sep, start);
        const std::string dir = path.substr(start, end == std::string::npos ? std::string::npos
                                                                            : end - start);
        if (!dir.empty()) {
            for (int i = 0; exeExt[i]; ++i) {
                const std::string full = dir + "/" + name + exeExt[i];
                if (FileExists(full)) {
#if !defined(_WIN32)
                    if (::access(full.c_str(), X_OK) != 0) continue;
#endif
                    return true;
                }
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

std::string TrimNewline(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

} // namespace internal

} // namespace platform
