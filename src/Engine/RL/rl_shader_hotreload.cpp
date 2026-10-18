// Shader hot-reload: watches shaders/*.glsl for edits and, on a change,
// shells out to the same pinned sokol-shdc used at build time (rebuilt for
// only the current backend's shading language), compiles a tiny wrapper
// translation unit that #includes the regenerated header, dlopen()s it and
// swaps the resolved function pointers into the program's ProgramInfo
// (rl_programs.hpp) via SetProgramOverride(), then drops the cached sg_shader
// (InvalidateProgramShader()) so the next draw rebuilds it from the new code.
// Mirrors NativeScriptHost's C++ script hot-reload (periodic mtime poll ->
// rebuild -> dlopen) rather than a filesystem-event watcher.
//
// Known limits:
//  - Only the function pointers are swapped; the uniform/texture/attribute
//    *name* lists stay the ones baked at build time (there is no reflection
//    API to enumerate them, only to look one up -- the build-time registry
//    gets them by scraping sokol-shdc's generated C text, see
//    cmake/GenShaderRegistry.cmake). So adding, removing or renaming a
//    uniform/texture/attribute needs a full rebuild; tweaking the code or
//    existing uniform values hot-swaps fine.
//  - Each successful reload leaks its small compiled wrapper library for the
//    rest of the process: the new ProgramInfo's function pointers live
//    inside it, and nothing else in the program still references the old
//    one, so it is never dlclose()'d. Acceptable for a dev session.
//  - Vulkan/D3D11 pay a sokol-shdc subprocess spawn plus a compiler
//    subprocess spawn per reload (same cost as the GL path); there's no
//    faster driver-side recompile available for either.
#include "rl_shader_hotreload.hpp"
#include "rl_internal.hpp"
#include "rl_programs.hpp"
#include "sokol_gfx.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace rli {

namespace {

bool g_enabled = true;
constexpr double kWatchIntervalSec = 0.5;

// Must mirror FLY_SHADERS / FLY_SHADER_INCLUDES in cmake/FlySokol.cmake.
constexpr const char* kShaderModules[] = {
    "rl_default", "rl_blit", "lit", "terrain", "terrain_paint",
    "water", "sky", "clouds", "post",
};
constexpr const char* kShaderIncludes[] = { "fly_common", "fly_atmosphere" };

std::string ShadersSrcDir() {
#if defined(FLY_SHADERS_SRC_DIR)
    return FLY_SHADERS_SRC_DIR;
#else
    return {};
#endif
}

std::string SokolShdcPath() {
#if defined(FLY_SOKOL_SHDC_PATH)
    return FLY_SOKOL_SHDC_PATH;
#else
    return {};
#endif
}

std::string SokolIncludeDir() {
#if defined(FLY_SOKOL_INCLUDE_DIR)
    return FLY_SOKOL_INCLUDE_DIR;
#else
    return {};
#endif
}

fs::file_time_type FileStamp(const fs::path& p) {
    std::error_code ec;
    return fs::last_write_time(p, ec);
}

// Quote one argument for a shell command line (POSIX sh / cmd.exe), good
// enough for the fixed, developer-controlled paths and flags used here.
std::string ShellQuote(const std::string& s) {
#if defined(_WIN32)
    return "\"" + s + "\"";
#else
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

// Runs a shell command line, capturing combined stdout+stderr.
struct RunResult {
    bool ok = false;
    std::string output;
};

RunResult RunCapture(const std::string& cmd) {
    RunResult r;
#if defined(_WIN32)
    FILE* pipe = _popen((cmd + " 2>&1").c_str(), "r");
#else
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
#endif
    if (!pipe) return r;
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) r.output += buf;
#if defined(_WIN32)
    int rc = _pclose(pipe);
#else
    int rc = pclose(pipe);
#endif
    r.ok = rc == 0;
    return r;
}

std::string SlangForCurrentBackend() {
    switch (sg_query_backend()) {
        case SG_BACKEND_D3D11: return "hlsl5";
        case SG_BACKEND_VULKAN: return "spirv_vk";
        default: return "glsl410"; // SG_BACKEND_GLCORE and anything else we can hot-reload for
    }
}

#if defined(_WIN32)
const char* kLibExt = ".dll";
#else
const char* kLibExt = ".so";
#endif

std::vector<std::string> CandidateCompilers() {
#if defined(_WIN32)
    return { "clang++", "g++", "cl" };
#else
    if (const char* env = std::getenv("FLYENGINE_CXX"); env && *env) return { env };
    return { "c++", "clang++", "g++" };
#endif
}

// Keeps every hot-reloaded wrapper library mapped for the process lifetime;
// their function pointers are still referenced from program overrides.
std::vector<void*> g_keepAlive;

void* LoadLib(const std::string& path) {
#if defined(_WIN32)
    return (void*)LoadLibraryA(path.c_str());
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* LibSymbol(void* handle, const std::string& name) {
#if defined(_WIN32)
    return (void*)GetProcAddress((HMODULE)handle, name.c_str());
#else
    return dlsym(handle, name.c_str());
#endif
}

// One shader source file may contain several @program blocks (e.g. post.glsl
// has post_ao, post_ao_blur, ...). sokol-shdc names each program's generated
// functions "<module>_<program>_shader_desc" etc; this finds every distinct
// "<module>_<program>" prefix in the regenerated header.
std::vector<std::string> FindProgramPrefixes(const std::string& headerText, const std::string& module) {
    static const std::regex re(R"(([A-Za-z_][A-Za-z0-9_]*)_shader_desc\s*\(\s*sg_backend)");
    std::set<std::string> prefixes;
    for (auto it = std::sregex_iterator(headerText.begin(), headerText.end(), re);
         it != std::sregex_iterator(); ++it) {
        std::string prefix = (*it)[1].str();
        if (prefix.rfind(module + "_", 0) == 0) prefixes.insert(prefix);
    }
    return { prefixes.begin(), prefixes.end() };
}

// Reloads every @program in shaders/<module>.glsl. Logs and leaves existing
// shaders bound on any failure.
void ReloadModule(const std::string& module) {
    const std::string shdc = SokolShdcPath();
    const std::string srcDir = ShadersSrcDir();
    const std::string sokolInc = SokolIncludeDir();
    if (shdc.empty() || srcDir.empty() || sokolInc.empty()) return;

    std::error_code ec;
    fs::path tmpDir = fs::temp_directory_path(ec) / "FlyShaderHotReload" /
                       (module + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tmpDir, ec);

    const fs::path src = fs::path(srcDir) / (module + ".glsl");
    const fs::path outHeader = tmpDir / (module + ".glsl.h");

    std::ostringstream shdcCmd;
    shdcCmd << ShellQuote(shdc) << " -i " << ShellQuote(src.string()) << " -o " << ShellQuote(outHeader.string())
            << " -l " << SlangForCurrentBackend() << " --reflection --errfmt=gcc --no-log-cmdline";
    RunResult shdcResult = RunCapture(shdcCmd.str());
    if (!shdcResult.ok) {
        TraceLog(LOG_WARNING, "SHADER HOTRELOAD: %s failed to compile:\n%s", module.c_str(), shdcResult.output.c_str());
        return;
    }

    std::ifstream headerFile(outHeader);
    std::string headerText((std::istreambuf_iterator<char>(headerFile)), std::istreambuf_iterator<char>());
    std::vector<std::string> prefixes = FindProgramPrefixes(headerText, module);
    if (prefixes.empty()) {
        TraceLog(LOG_WARNING, "SHADER HOTRELOAD: %s produced no recognizable programs", module.c_str());
        return;
    }

    // { programName, prefix, original ProgramInfo (build-time, for the name arrays) }
    struct Entry { std::string program, prefix; const ProgramInfo* original; };
    std::vector<Entry> entries;
    for (const std::string& prefix : prefixes) {
        std::string program = prefix.substr(module.size() + 1);
        const ProgramInfo* original = FindProgram(program.c_str());
        if (!original) {
            TraceLog(LOG_WARNING, "SHADER HOTRELOAD: %s: unknown program '%s', skipping", module.c_str(), program.c_str());
            continue;
        }
        entries.push_back({ program, prefix, original });
    }
    if (entries.empty()) return;

    fs::path wrapperCpp = tmpDir / "wrapper.cpp";
    {
        std::ofstream w(wrapperCpp);
        w << "#include \"sokol_gfx.h\"\n";
        std::string headerInclude = outHeader.string();
#if defined(_WIN32)
        for (char& c : headerInclude) if (c == '\\') c = '/';
#endif
        w << "#include \"" << headerInclude << "\"\n";
        w << "extern \"C\" {\n";
        for (const Entry& e : entries) {
            w << "const sg_shader_desc* FlyHotReload_" << e.program << "_desc(sg_backend b) { return "
              << e.prefix << "_shader_desc(b); }\n";
            w << "int FlyHotReload_" << e.program << "_uniformblock_slot(const char* n) { return "
              << e.prefix << "_uniformblock_slot(n); }\n";
            w << "size_t FlyHotReload_" << e.program << "_uniformblock_size(const char* n) { return "
              << e.prefix << "_uniformblock_size(n); }\n";
            w << "int FlyHotReload_" << e.program << "_uniform_offset(const char* ub, const char* u) { return "
              << e.prefix << "_uniform_offset(ub, u); }\n";
            w << "sg_glsl_shader_uniform FlyHotReload_" << e.program << "_uniform_desc(const char* ub, const char* u) { return "
              << e.prefix << "_uniform_desc(ub, u); }\n";
            w << "int FlyHotReload_" << e.program << "_texture_slot(const char* n) { return "
              << e.prefix << "_texture_slot(n); }\n";
            w << "int FlyHotReload_" << e.program << "_attr_slot(const char* n) { return "
              << e.prefix << "_attr_slot(n); }\n";
        }
        w << "}\n";
    }

    fs::path wrapperLib = tmpDir / ("wrapper" + std::string(kLibExt));
    bool compiled = false;
    std::string compileLog;
    for (const std::string& cxx : CandidateCompilers()) {
        std::ostringstream cmd;
#if defined(_WIN32)
        if (cxx == "cl") {
            cmd << ShellQuote(cxx) << " /nologo /LD /std:c++17 /I" << ShellQuote(sokolInc)
                << " /I" << ShellQuote(tmpDir.string()) << " " << ShellQuote(wrapperCpp.string())
                << " /Fe" << ShellQuote(wrapperLib.string());
        } else
#endif
        {
            cmd << ShellQuote(cxx) << " -shared -fPIC -std=c++17 -O0 -I" << ShellQuote(sokolInc)
                << " -I" << ShellQuote(tmpDir.string()) << " -o " << ShellQuote(wrapperLib.string())
                << " " << ShellQuote(wrapperCpp.string());
        }
        RunResult r = RunCapture(cmd.str());
        compileLog += r.output;
        if (r.ok && fs::exists(wrapperLib)) { compiled = true; break; }
    }
    if (!compiled) {
        TraceLog(LOG_WARNING, "SHADER HOTRELOAD: %s: wrapper compile failed:\n%s", module.c_str(), compileLog.c_str());
        return;
    }

    void* handle = LoadLib(wrapperLib.string());
    if (!handle) {
        TraceLog(LOG_WARNING, "SHADER HOTRELOAD: %s: failed to load compiled wrapper", module.c_str());
        return;
    }

    int swapped = 0;
    for (const Entry& e : entries) {
        auto sym = [&](const char* suffix) { return LibSymbol(handle, "FlyHotReload_" + e.program + suffix); };
        void* desc = sym("_desc");
        void* ubSlot = sym("_uniformblock_slot");
        void* ubSize = sym("_uniformblock_size");
        void* uOff = sym("_uniform_offset");
        void* uDesc = sym("_uniform_desc");
        void* texSlot = sym("_texture_slot");
        void* attrSlot = sym("_attr_slot");
        if (!desc || !ubSlot || !ubSize || !uOff || !uDesc || !texSlot || !attrSlot) {
            TraceLog(LOG_WARNING, "SHADER HOTRELOAD: %s: missing symbols for program '%s'", module.c_str(), e.program.c_str());
            continue;
        }
        ProgramInfo info = *e.original; // keeps the build-time name arrays
        info.desc = reinterpret_cast<const sg_shader_desc* (*)(sg_backend)>(desc);
        info.uniformblockSlot = reinterpret_cast<int (*)(const char*)>(ubSlot);
        info.uniformblockSize = reinterpret_cast<size_t (*)(const char*)>(ubSize);
        info.uniformOffset = reinterpret_cast<int (*)(const char*, const char*)>(uOff);
        info.uniformDesc = reinterpret_cast<sg_glsl_shader_uniform (*)(const char*, const char*)>(uDesc);
        info.textureSlot = reinterpret_cast<int (*)(const char*)>(texSlot);
        info.attrSlot = reinterpret_cast<int (*)(const char*)>(attrSlot);
        SetProgramOverride(e.original->name, info);
        InvalidateProgramShader(e.original->name);
        ++swapped;
    }
    if (swapped > 0) {
        g_keepAlive.push_back(handle);
        TraceLog(LOG_INFO, "SHADER HOTRELOAD: %s: reloaded %d program(s)", module.c_str(), swapped);
    }
}

struct Watcher {
    std::chrono::steady_clock::time_point lastCheck{};
    std::unordered_map<std::string, fs::file_time_type> moduleStamp;
    fs::file_time_type includesStamp{};
    bool initialized = false;
};

Watcher& GetWatcher() {
    static Watcher w;
    return w;
}

void InitWatcher(Watcher& w) {
    const std::string srcDir = ShadersSrcDir();
    if (srcDir.empty()) { w.initialized = true; return; }
    for (const char* m : kShaderModules) w.moduleStamp[m] = FileStamp(fs::path(srcDir) / (std::string(m) + ".glsl"));
    for (const char* inc : kShaderIncludes) {
        fs::file_time_type t = FileStamp(fs::path(srcDir) / (std::string(inc) + ".glsl"));
        if (t > w.includesStamp) w.includesStamp = t;
    }
    w.initialized = true;
}

} // namespace

void SetShaderHotReloadEnabled(bool enabled) { g_enabled = enabled; }

void TickShaderHotReload() {
    if (!g_enabled) return;
    const std::string srcDir = ShadersSrcDir();
    if (srcDir.empty() || SokolShdcPath().empty()) return; // not an editor/dev build with the tool available

    Watcher& w = GetWatcher();
    if (!w.initialized) InitWatcher(w);

    const auto now = std::chrono::steady_clock::now();
    if (w.lastCheck.time_since_epoch().count() != 0 &&
        now - w.lastCheck < std::chrono::duration<double>(kWatchIntervalSec)) {
        return;
    }
    w.lastCheck = now;

    fs::file_time_type includesStamp{};
    for (const char* inc : kShaderIncludes) {
        fs::file_time_type t = FileStamp(fs::path(srcDir) / (std::string(inc) + ".glsl"));
        if (t > includesStamp) includesStamp = t;
    }
    const bool includesChanged = includesStamp != w.includesStamp;
    if (includesChanged) w.includesStamp = includesStamp;

    for (const char* m : kShaderModules) {
        const fs::path path = fs::path(srcDir) / (std::string(m) + ".glsl");
        const fs::file_time_type stamp = FileStamp(path);
        auto& stored = w.moduleStamp[m];
        if (includesChanged || stamp != stored) {
            stored = stamp;
            ReloadModule(m);
        }
    }
}

} // namespace rli
