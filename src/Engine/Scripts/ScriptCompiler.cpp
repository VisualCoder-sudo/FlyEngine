// ScriptCompiler.cpp - builds the project's C++ scripts into a shared library
// with whichever compiler the system has (clang++, g++, MSVC). See
// ScriptCompiler.hpp for the layout of Scripts/.build/.

#include "../../../include/Engine/Scripts/ScriptCompiler.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kBuildDirName = ".build";
constexpr const char* kLibPrefix = "FlyScripts-";
#if defined(_WIN32)
constexpr const char* kLibExt = ".dll";
#else
constexpr const char* kLibExt = ".so";
#endif
constexpr int kBuildTimeoutMs = 5 * 60 * 1000;

enum class CompilerKind {
    None,
    Gnu,         // clang++ / g++ / MinGW g++ (GNU-style flags)
    Msvc,        // cl.exe or clang-cl on PATH (developer prompt)
    MsvcVcvars,  // cl.exe reached by running vcvars64.bat first
};

struct Compiler {
    CompilerKind kind = CompilerKind::None;
    std::string exe;    // compiler, or vcvars64.bat for MsvcVcvars
    bool isGcc = false; // GNU g++ (not clang): needs -fno-gnu-unique / static runtime
};

bool IsMsvcStyleName(const std::string& exe) {
    std::string stem = fs::u8path(exe).stem().string();
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return stem == "cl" || stem == "clang-cl";
}

bool IsGccName(const std::string& exe) {
    const std::string name = fs::u8path(exe).filename().string();
    return name.find("g++") != std::string::npos && name.find("clang") == std::string::npos;
}

#if defined(_WIN32)
// Finds vcvars64.bat of the newest Visual Studio with the C++ workload.
std::string FindVcvars() {
    const std::string vswhere =
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    std::error_code ec;
    if (!fs::is_regular_file(vswhere, ec)) return {};
    platform::ProcessResult r = platform::RunProcessCapture(
        vswhere, {"-latest", "-products", "*", "-requires",
                  "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                  "-property", "installationPath"}, 10000);
    std::string path = r.output;
    while (!path.empty() && (path.back() == '\r' || path.back() == '\n' || path.back() == ' '))
        path.pop_back();
    if (!r.launched || path.empty()) return {};
    const fs::path bat = fs::u8path(path) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
    return fs::is_regular_file(bat, ec) ? bat.string() : std::string();
}
#endif

Compiler DetectCompiler() {
    Compiler c;
    // Explicit override, e.g. FLYENGINE_CXX=clang++-18.
    if (const char* env = std::getenv("FLYENGINE_CXX"); env && *env) {
        c.exe = env;
        c.kind = IsMsvcStyleName(c.exe) ? CompilerKind::Msvc : CompilerKind::Gnu;
        c.isGcc = IsGccName(c.exe);
        return c;
    }
#if defined(_WIN32)
    if (std::string cl = platform::FindOnPath({"cl"}); !cl.empty()) {
        c.kind = CompilerKind::Msvc;
        c.exe = cl;
        return c;
    }
    if (std::string gnu = platform::FindOnPath({"clang++", "g++"}); !gnu.empty()) {
        c.kind = CompilerKind::Gnu;
        c.exe = gnu;
        c.isGcc = IsGccName(gnu);
        return c;
    }
    if (std::string vcvars = FindVcvars(); !vcvars.empty()) {
        c.kind = CompilerKind::MsvcVcvars;
        c.exe = vcvars;
        return c;
    }
#else
    if (std::string gnu = platform::FindOnPath({"clang++", "g++", "c++"}); !gnu.empty()) {
        c.kind = CompilerKind::Gnu;
        c.exe = gnu;
        c.isGcc = IsGccName(gnu);
        return c;
    }
#endif
    return c;
}

const Compiler& GetCompiler() {
    static Compiler compiler;
    static std::once_flag once;
    std::call_once(once, [] { compiler = DetectCompiler(); });
    return compiler;
}

bool IsSourceExt(const fs::path& p, bool includeHeaders) {
    const std::string ext = p.extension().string();
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") return true;
    return includeHeaders && (ext == ".hpp" || ext == ".h" || ext == ".hh");
}

// Every script source under Scripts/, skipping hidden folders (.build, .vscode).
std::vector<fs::path> CollectFiles(const fs::path& scriptsDir, bool includeHeaders) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (!fs::is_directory(scriptsDir, ec)) return files;
    fs::recursive_directory_iterator it(scriptsDir, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path& p = it->path();
        if (it->is_directory(ec)) {
            if (p.filename().string().rfind('.', 0) == 0) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && IsSourceExt(p, includeHeaders)) files.push_back(p);
    }
    std::sort(files.begin(), files.end());
    return files;
}

fs::path FindSdkRoot() {
    const std::string fly = platform::ResolveAsset("ScriptingSDK/include/fly.hpp");
    if (fly.empty()) return {};
    return fs::u8path(fly).parent_path().parent_path();
}

// Lets clangd (VS Code, CLion, etc.) resolve "fly.hpp" in the user's scripts.
void WriteCompileFlags(const fs::path& scriptsDir, const fs::path& sdkInclude) {
    const fs::path file = scriptsDir / "compile_flags.txt";
    const std::string want = "-std=c++20\n-I" + sdkInclude.string() + "\n";
    {
        std::ifstream in(file, std::ios::binary);
        std::string have((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (have == want) return;
    }
    std::ofstream out(file, std::ios::binary);
    out << want;
}

std::string QuoteForCmd(const std::string& s) {
    return "\"" + s + "\"";
}

// A failed build must always carry an Error diagnostic, so the UI has
// something to show even when the output wasn't in a recognised format.
void FinishFailure(scriptCompiler::Result& r) {
    for (const auto& d : r.diagnostics)
        if (d.severity == scriptCompiler::Diagnostic::Severity::Error) return;
    scriptCompiler::Diagnostic d;
    d.severity = scriptCompiler::Diagnostic::Severity::Error;
    std::istringstream in(r.log);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) { d.message = line; break; }
    }
    if (d.message.empty()) d.message = "Script build failed.";
    r.diagnostics.insert(r.diagnostics.begin(), std::move(d));
}

} // namespace

namespace scriptCompiler {

std::string SdkIncludeDir() {
    const fs::path root = FindSdkRoot();
    return root.empty() ? std::string() : (root / "include").string();
}

std::string CompilerDescription() {
    const Compiler& c = GetCompiler();
    switch (c.kind) {
        case CompilerKind::Gnu:
        case CompilerKind::Msvc:       return c.exe;
        case CompilerKind::MsvcVcvars: return "MSVC via " + c.exe;
        default:                       return {};
    }
}

bool HasSources(const std::string& projectFolder) {
    return !CollectFiles(fs::u8path(projectFolder) / "Scripts", false).empty();
}

uint64_t SourcesStamp(const std::string& projectFolder) {
    uint64_t h = 1469598103934665603ULL; // FNV-1a
    auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xff; h *= 1099511628211ULL; }
    };
    for (const fs::path& p : CollectFiles(fs::u8path(projectFolder) / "Scripts", true)) {
        std::error_code ec;
        for (char ch : p.string()) mix(static_cast<unsigned char>(ch));
        mix(static_cast<uint64_t>(fs::file_size(p, ec)));
        mix(static_cast<uint64_t>(fs::last_write_time(p, ec).time_since_epoch().count()));
    }
    return h;
}

std::string LatestBuiltLibrary(const std::string& projectFolder) {
    const fs::path dir = fs::u8path(projectFolder) / "Scripts" / kBuildDirName;
    std::error_code ec;
    fs::path best;
    fs::file_time_type bestTime{};
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind(kLibPrefix, 0) != 0 || e.path().extension() != kLibExt) continue;
        const auto t = e.last_write_time(ec);
        if (best.empty() || t > bestTime) { best = e.path(); bestTime = t; }
    }
    return best.string();
}

void RemoveStaleLibraries(const std::string& projectFolder, const std::string& keep) {
    const fs::path dir = fs::u8path(projectFolder) / "Scripts" / kBuildDirName;
    std::error_code ec;
    std::vector<fs::path> victims;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind(kLibPrefix, 0) != 0) continue;
        if (!keep.empty() && e.path().stem() == fs::u8path(keep).stem()) continue;
        victims.push_back(e.path()); // .so/.dll plus MSVC's .lib/.exp/.pdb siblings
    }
    for (const auto& v : victims) {
        std::error_code rmEc;
        fs::remove(v, rmEc);
    }
}

Result Build(const std::string& projectFolder) {
    Result result;
    const fs::path scriptsDir = fs::u8path(projectFolder) / "Scripts";
    const fs::path buildDir = scriptsDir / kBuildDirName;

    const std::vector<fs::path> sources = CollectFiles(scriptsDir, false);
    if (sources.empty()) {
        result.log = "No .cpp files in " + scriptsDir.string();
        FinishFailure(result);
        return result;
    }

    const fs::path sdkRoot = FindSdkRoot();
    if (sdkRoot.empty()) {
        result.log = "Script SDK not found (ScriptingSDK/include/fly.hpp).";
        FinishFailure(result);
        return result;
    }
    const fs::path sdkInclude = sdkRoot / "include";
    const fs::path moduleSource = sdkRoot / "src" / "fly_module.cpp";

    const Compiler& cc = GetCompiler();
    if (cc.kind == CompilerKind::None) {
#if defined(_WIN32)
        result.log = "No C++ compiler found. Install Visual Studio (Desktop development with C++), "
                     "LLVM clang, or MinGW-w64 g++, or set FLYENGINE_CXX.";
#else
        result.log = "No C++ compiler found. Install clang or g++ (e.g. your distro's "
                     "clang / gcc package), or set FLYENGINE_CXX.";
#endif
        FinishFailure(result);
        return result;
    }

    std::error_code ec;
    fs::create_directories(buildDir, ec);
    WriteCompileFlags(scriptsDir, sdkInclude);

    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const fs::path output = buildDir / (std::string(kLibPrefix) + std::to_string(stamp) + kLibExt);

    platform::ProcessResult pr;
    if (cc.kind == CompilerKind::Gnu) {
        std::vector<std::string> args = {
            "-std=c++20", "-shared", "-O2", "-g",
            "-fvisibility=hidden", "-fvisibility-inlines-hidden",
            "-I" + sdkInclude.string(), "-I" + scriptsDir.string(),
        };
#if !defined(_WIN32)
        args.push_back("-fPIC");
        // g++ marks some inline statics STB_GNU_UNIQUE, which pins the library
        // in memory and breaks hot reload.
        if (cc.isGcc) args.push_back("-fno-gnu-unique");
#else
        if (cc.isGcc) { args.push_back("-static-libgcc"); args.push_back("-static-libstdc++"); }
#endif
        args.push_back(moduleSource.string());
        for (const auto& s : sources) args.push_back(s.string());
        args.push_back("-o");
        args.push_back(output.string());
        pr = platform::RunProcessCapture(cc.exe, args, kBuildTimeoutMs);
    } else {
        std::vector<std::string> args = {
            "/nologo", "/std:c++20", "/EHsc", "/O2", "/MD", "/LD", "/utf-8",
            "/I" + sdkInclude.string(), "/I" + scriptsDir.string(),
            // Two trailing backslashes: a quoted argument ending in one would
            // escape its closing quote. cl accepts the doubled separator.
            "/Fo" + buildDir.string() + "\\\\",
            moduleSource.string(),
        };
        for (const auto& s : sources) args.push_back(s.string());
        args.push_back("/Fe" + output.string());

        if (cc.kind == CompilerKind::Msvc) {
            pr = platform::RunProcessCapture(cc.exe, args, kBuildTimeoutMs);
        } else {
            // cl.exe needs the environment vcvars64.bat sets up, so run both
            // from one batch file.
            const fs::path bat = buildDir / "build.bat";
            {
                std::ofstream out(bat, std::ios::binary);
                out << "@echo off\r\n";
                out << "call " << QuoteForCmd(cc.exe) << " >nul\r\n";
                out << "if errorlevel 1 exit /b 1\r\n";
                out << "cl";
                for (const auto& a : args) out << " " << QuoteForCmd(a);
                out << "\r\n";
                out << "exit /b %errorlevel%\r\n";
            }
            pr = platform::RunProcessCapture("cmd.exe", {"/c", bat.string()}, kBuildTimeoutMs);
        }
    }

    result.log = pr.launched ? pr.output : pr.error;
    result.ok = pr.launched && pr.exitCode == 0 && fs::exists(output, ec);
    if (result.ok) result.libraryPath = output.string();
    result.diagnostics = ParseDiagnostics(result.log);
    if (!result.ok) FinishFailure(result);
    return result;
}

} // namespace scriptCompiler
