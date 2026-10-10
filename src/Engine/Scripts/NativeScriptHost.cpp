/*
 * NativeScriptHost.cpp - Native plugin loading, C++ game scripts, hot-reload.
 * Plugins use CPluginAPI (vtable-based); scripts use FlyScriptABI.h.
 */

#include "../../../include/Engine/Scripts/NativeScriptHost.hpp"
#include "../../../include/CPluginAPI.h"
#include "../../../include/Engine.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "../../../include/Engine/Scripts/ScriptCompiler.hpp"
#include "../../../include/Engine/Scripts/FlyScriptApi.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;

namespace NativeScript {

// ============================================================================
// PLATFORM-SPECIFIC LIBRARY LOADING
// ============================================================================

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

    void* NativeScriptHost::LoadLibrary(const std::string& path) {
        return ::LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }

    void NativeScriptHost::UnloadLibrary(void* handle) {
        if (handle) ::FreeLibrary(static_cast<HMODULE>(handle));
    }

    void* NativeScriptHost::GetSymbol(void* handle, const char* name) {
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
    }

    uint64_t NativeScriptHost::GetFileWriteTime(const std::string& path) {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
            ULARGE_INTEGER ul;
            ul.LowPart = data.ftLastWriteTime.dwLowDateTime;
            ul.HighPart = data.ftLastWriteTime.dwHighDateTime;
            return ul.QuadPart;
        }
        return 0;
    }

#else
    #include <dlfcn.h>
    #include <sys/stat.h>

    void* NativeScriptHost::LoadLibrary(const std::string& path) {
        void* handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (!handle) {
            ui::LogAlways("[NativeScriptHost] dlopen failed: %s", dlerror());
        }
        return handle;
    }

    void NativeScriptHost::UnloadLibrary(void* handle) {
        if (handle) dlclose(handle);
    }

    void* NativeScriptHost::GetSymbol(void* handle, const char* name) {
        dlerror();
        void* sym = dlsym(handle, name);
        const char* err = dlerror();
        if (err) {
            ui::LogAlways("[NativeScriptHost] dlsym('%s') failed: %s", name, err);
        }
        return sym;
    }

    uint64_t NativeScriptHost::GetFileWriteTime(const std::string& path) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            return static_cast<uint64_t>(st.st_mtim.tv_sec) * 1000000000ULL + static_cast<uint64_t>(st.st_mtim.tv_nsec);
        }
        return 0;
    }
#endif

// ============================================================================
// NATIVE SCRIPT HOST IMPLEMENTATION
// ============================================================================

namespace {
NativeScriptHost* g_activeHost = nullptr;

// The engine services handed to the script library: the FlyNative_* exports.
FlyApi MakeScriptApi() {
    FlyApi a{};
    a.abi_version = FLY_SCRIPT_ABI_VERSION;
    a.size = sizeof(FlyApi);
    a.FindObject = FlyNative_FindObject;
    a.GetSelf = FlyNative_GetSelf;
    a.CreateObject = FlyNative_CreateObject;
    a.GetPosition = FlyNative_GetPosition;
    a.SetPosition = FlyNative_SetPosition;
    a.GetSize = FlyNative_GetSize;
    a.SetSize = FlyNative_SetSize;
    a.GetRotation = FlyNative_GetRotation;
    a.SetRotation = FlyNative_SetRotation;
    a.GetOrigin = FlyNative_GetOrigin;
    a.SetOrigin = FlyNative_SetOrigin;
    a.GetVelocity = FlyNative_GetVelocity;
    a.SetVelocity = FlyNative_SetVelocity;
    a.GetAngularVelocity = FlyNative_GetAngularVelocity;
    a.SetAngularVelocity = FlyNative_SetAngularVelocity;
    a.GetColor = FlyNative_GetColor;
    a.SetColor = FlyNative_SetColor;
    a.GetAnchored = FlyNative_GetAnchored;
    a.SetAnchored = FlyNative_SetAnchored;
    a.GetCanCollide = FlyNative_GetCanCollide;
    a.SetCanCollide = FlyNative_SetCanCollide;
    a.GetMass = FlyNative_GetMass;
    a.SetMass = FlyNative_SetMass;
    a.GetTransparency = FlyNative_GetTransparency;
    a.SetTransparency = FlyNative_SetTransparency;
    a.GetCollisionAccuracy = FlyNative_GetCollisionAccuracy;
    a.SetCollisionAccuracy = FlyNative_SetCollisionAccuracy;
    a.IsPlaying = FlyNative_IsPlaying;
    a.GetShadowsEnabled = FlyNative_GetShadowsEnabled;
    a.SetShadowsEnabled = FlyNative_SetShadowsEnabled;
    a.GetShadowQuality = FlyNative_GetShadowQuality;
    a.SetShadowQuality = FlyNative_SetShadowQuality;
    a.GetAmbient = FlyNative_GetAmbient;
    a.SetAmbient = FlyNative_SetAmbient;
    a.GetGridVisible = FlyNative_GetGridVisible;
    a.SetGridVisible = FlyNative_SetGridVisible;
    a.GetWireframe = FlyNative_GetWireframe;
    a.SetWireframe = FlyNative_SetWireframe;
    a.GetFov = FlyNative_GetFov;
    a.SetFov = FlyNative_SetFov;
    a.GetGravity = FlyNative_GetGravity;
    a.SetGravity = FlyNative_SetGravity;
    a.GetFriction = FlyNative_GetFriction;
    a.SetFriction = FlyNative_SetFriction;
    a.GetRestitution = FlyNative_GetRestitution;
    a.SetRestitution = FlyNative_SetRestitution;
    a.Print = FlyNative_Print;
    a.GetEnvironment = FlyNative_GetEnvironment;
    a.SetEnvironment = FlyNative_SetEnvironment;
    return a;
}

// Echo multi-line compiler output into the editor log, one line at a time.
// Error lines are shown in red.
void LogLines(const char* prefix, const std::string& text) {
    for (std::string::size_type s = 0, i; s < text.size(); s = i + 1) {
        i = text.find('\n', s);
        if (i == std::string::npos) i = text.size();
        std::string line = text.substr(s, i - s);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        const bool isError = line.find(": error") != std::string::npos ||
                             line.find(": fatal error") != std::string::npos ||
                             line.find(" error C") != std::string::npos;
        ui::LogWithSeverity(isError ? ui::LogSeverity::Error : ui::LogSeverity::Neutral,
                            "%s %s", prefix, line.c_str());
    }
}

// Hands a build result to the editor's script panel.
void PublishBuildResult(const scriptCompiler::Result& r) {
    std::vector<ui::ScriptDiagnostic> diags;
    diags.reserve(r.diagnostics.size());
    for (const auto& d : r.diagnostics) {
        ui::ScriptDiagnostic u;
        switch (d.severity) {
            case scriptCompiler::Diagnostic::Severity::Note:    u.severity = ui::ScriptDiagnostic::Severity::Note; break;
            case scriptCompiler::Diagnostic::Severity::Warning: u.severity = ui::ScriptDiagnostic::Severity::Warning; break;
            default:                                            u.severity = ui::ScriptDiagnostic::Severity::Error; break;
        }
        u.file = d.file;
        u.line = d.line;
        u.column = d.column;
        u.message = d.message;
        diags.push_back(std::move(u));
    }
    ui::SetScriptBuildState(r.ok ? ui::ScriptBuildState::Succeeded : ui::ScriptBuildState::Failed,
                            std::move(diags));
}

constexpr float kWatchInterval = 0.5f; // seconds between source scans
} // namespace

NativeScriptHost::NativeScriptHost()
    : m_runtime(std::make_unique<ScriptRuntime>())
    , m_api(MakeScriptApi())
{
    m_runtime->SetHost(this);
    SetActiveRuntime(m_runtime.get());
    g_activeHost = this;
    SetPlayStateHook([](bool playing) {
        if (g_activeHost) g_activeHost->SetPlayActive(playing);
    });
}

NativeScriptHost::~NativeScriptHost() {
    Shutdown();
    if (g_activeHost == this) {
        g_activeHost = nullptr;
        SetPlayStateHook(nullptr);
    }
    if (GetActiveRuntime() == m_runtime.get()) SetActiveRuntime(nullptr);
}

bool NativeScriptHost::Initialize(const std::string& projectPath) {
    m_projectPath = projectPath;
    m_lastError.clear();
    
    ui::LogAlways("[NativeScriptHost] Initializing for project: %s", projectPath.c_str());
    
    fs::path pluginsDir = fs::path(projectPath) / "plugins";
    std::error_code ec;
    if (!fs::exists(pluginsDir, ec)) {
        fs::create_directories(pluginsDir, ec);
        if (ec) {
            m_lastError = "Failed to create plugins directory: " + ec.message();
            ui::LogAlways("[NativeScriptHost] ERROR: %s", m_lastError.c_str());
            return false;
        }
    }
    
    for (const auto& typeDir : {"nat", "lib"}) {
        fs::path typePath = pluginsDir / typeDir;
        if (!fs::exists(typePath, ec)) continue;
        
        for (const auto& entry : fs::directory_iterator(typePath, ec)) {
            if (!ec && entry.is_directory()) {
                fs::path buildDir = entry.path() / "build";
                if (fs::exists(buildDir, ec)) {
                    for (const auto& file : fs::directory_iterator(buildDir, ec)) {
                        if (file.is_regular_file()) {
                            std::string ext = file.path().extension().string();
                            #if defined(_WIN32)
                                if (ext == ".dll") LoadPlugin(file.path().string());
                            #else
                                if (ext == ".so") LoadPlugin(file.path().string());
                            #endif
                        }
                    }
                }
            }
        }
    }
    
    ui::LogAlways("[NativeScriptHost] Initialized with %zu plugins", plugins.size());
    return true;
}

void NativeScriptHost::Update(float dt) {
    WatchScriptSources(dt);

    // Play/stop edges from the simulation. SetSimPlaying starts runOnPlay
    // scripts (or stops everything and rolls back play-created objects).
    if (m_pendingPlay) {
        m_pendingPlay = false;
        m_runtime->SetSimPlaying(true);
    } else if (m_pendingStop) {
        m_pendingStop = false;
        m_runtime->SetSimPlaying(false);
    }

    TickScripts(dt);

    if (!m_engine) return;

    CheckHotReload();

    for (auto& plugin : plugins) {
        if (plugin->loaded && plugin->entry && plugin->entry->on_update) {
            plugin->entry->on_update(dt);
        }
    }
}

void NativeScriptHost::Shutdown() {
    if (m_build.valid()) m_build.wait();
    UnloadScriptLibrary();

    ui::LogAlways("[NativeScriptHost] Shutting down %zu plugins", plugins.size());

    for (auto it = plugins.rbegin(); it != plugins.rend(); ++it) {
        auto& plugin = *it;
        if (plugin->loaded && plugin->entry && plugin->entry->on_unload) {
            plugin->entry->on_unload();
        }
        if (plugin->handle) {
            UnloadLibrary(plugin->handle);
            plugin->handle = nullptr;
        }
        plugin->loaded = false;
    }
    plugins.clear();
    m_engine = nullptr;
}

bool NativeScriptHost::LoadPlugin(const std::string& pluginPath) {
    fs::path path(pluginPath);
    std::string name = path.stem().string();
    
    for (auto& p : plugins) {
        if (p->name == name) {
            return ReloadPlugin(name);
        }
    }
    
    ui::LogAlways("[NativeScriptHost] Loading plugin: %s", pluginPath.c_str());
    
    auto plugin = std::make_unique<PluginModule>();
    plugin->name = name;
    plugin->path = pluginPath;
    plugin->lastWriteTime = this->GetFileWriteTime(pluginPath);
    
    plugin->handle = LoadLibrary(pluginPath);
    if (!plugin->handle) {
        plugin->loadFailed = true;
        plugin->errorMessage = "Failed to load library";
        ui::LogAlways("[NativeScriptHost] ERROR: Failed to load %s", pluginPath.c_str());
        return false;
    }
    
    auto get_entry = reinterpret_cast<CP_PluginEntry* (*)(void)>(GetSymbol(plugin->handle, "CP_GetPluginEntry"));
    if (!get_entry) {
        plugin->loadFailed = true;
        plugin->errorMessage = "Plugin missing CP_GetPluginEntry symbol (requires new CPluginAPI)";
        ui::LogAlways("[NativeScriptHost] ERROR: %s missing CP_GetPluginEntry", pluginPath.c_str());
        UnloadLibrary(plugin->handle);
        plugin->handle = nullptr;
        return false;
    }
    
    plugin->entry = get_entry();
    if (!plugin->entry) {
        plugin->loadFailed = true;
        plugin->errorMessage = "CP_GetPluginEntry returned NULL";
        ui::LogAlways("[NativeScriptHost] ERROR: %s CP_GetPluginEntry returned NULL", pluginPath.c_str());
        UnloadLibrary(plugin->handle);
        plugin->handle = nullptr;
        return false;
    }
    
    CP_API_Version engine_ver = CP_API_CURRENT_VERSION;
    CP_API_Version plugin_ver = plugin->entry->api_version;
    if (!CP_API_VersionAtLeast(engine_ver, plugin_ver)) {
        plugin->loadFailed = true;
        plugin->errorMessage = "Plugin API version newer than engine supports";
        ui::LogAlways("[NativeScriptHost] ERROR: %s API version mismatch", pluginPath.c_str());
        UnloadLibrary(plugin->handle);
        plugin->handle = nullptr;
        return false;
    }
    
    CP_Internal_SetEngine(m_engine);
    CP_Internal_SetScriptRuntime(m_runtime.get());
    
    if (plugin->entry->on_load) {
        int result = plugin->entry->on_load(CP_GetEngineAPI());
        if (result != 0) {
            plugin->loadFailed = true;
            plugin->errorMessage = "on_load returned error code: " + std::to_string(result);
            ui::LogAlways("[NativeScriptHost] ERROR: %s on_load failed with code %d", pluginPath.c_str(), result);
            UnloadLibrary(plugin->handle);
            plugin->handle = nullptr;
            return false;
        }
    }
    
    plugin->loaded = true;
    plugins.push_back(std::move(plugin));
    ui::LogAlways("[NativeScriptHost] Loaded plugin: %s", name.c_str());
    return true;
}

void NativeScriptHost::UnloadPlugin(const std::string& pluginName) {
    for (auto it = plugins.begin(); it != plugins.end(); ++it) {
        if ((*it)->name == pluginName) {
            auto& plugin = *it;
            if (plugin->loaded && plugin->entry && plugin->entry->on_unload) {
                plugin->entry->on_unload();
            }
            if (plugin->handle) {
                UnloadLibrary(plugin->handle);
            }
            plugins.erase(it);
            ui::LogAlways("[NativeScriptHost] Unloaded plugin: %s", pluginName.c_str());
            return;
        }
    }
}

bool NativeScriptHost::ReloadPlugin(const std::string& pluginName) {
    PluginModule* plugin = nullptr;
    for (auto& p : plugins) {
        if (p->name == pluginName) {
            plugin = p.get();
            break;
        }
    }
    if (!plugin) return false;
    
    ui::LogAlways("[NativeScriptHost] Reloading plugin: %s", pluginName.c_str());
    
    if (plugin->loaded && plugin->entry && plugin->entry->on_unload) {
        plugin->entry->on_unload();
    }
    
    if (plugin->handle) {
        UnloadLibrary(plugin->handle);
        plugin->handle = nullptr;
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    plugin->handle = LoadLibrary(plugin->path);
    if (!plugin->handle) {
        plugin->loadFailed = true;
        plugin->errorMessage = "Failed to reload library";
        plugin->loaded = false;
        return false;
    }
    
    auto get_entry = reinterpret_cast<CP_PluginEntry* (*)(void)>(GetSymbol(plugin->handle, "CP_GetPluginEntry"));
    if (!get_entry) {
        plugin->loadFailed = true;
        plugin->errorMessage = "Plugin missing CP_GetPluginEntry after reload";
        UnloadLibrary(plugin->handle);
        plugin->handle = nullptr;
        plugin->loaded = false;
        return false;
    }
    
    plugin->entry = get_entry();
    if (!plugin->entry) {
        plugin->loadFailed = true;
        plugin->errorMessage = "CP_GetPluginEntry returned NULL after reload";
        UnloadLibrary(plugin->handle);
        plugin->handle = nullptr;
        plugin->loaded = false;
        return false;
    }
    
    CP_Internal_SetEngine(m_engine);
    CP_Internal_SetScriptRuntime(m_runtime.get());
    
    if (plugin->entry->on_load) {
        int result = plugin->entry->on_load(CP_GetEngineAPI());
        if (result != 0) {
            plugin->loadFailed = true;
            plugin->errorMessage = "on_load failed after reload: " + std::to_string(result);
            UnloadLibrary(plugin->handle);
            plugin->handle = nullptr;
            plugin->loaded = false;
            return false;
        }
    }
    
    plugin->loaded = true;
    plugin->loadFailed = false;
    plugin->errorMessage.clear();
    plugin->lastWriteTime = this->GetFileWriteTime(plugin->path);
    ui::LogAlways("[NativeScriptHost] Reloaded plugin: %s", pluginName.c_str());
    return true;
}

void NativeScriptHost::CheckHotReload() {
    for (auto& plugin : plugins) {
        if (!plugin->loaded) continue;
        
        uint64_t currentTime = this->GetFileWriteTime(plugin->path);
        if (currentTime != plugin->lastWriteTime && currentTime != 0) {
            ui::LogAlways("[NativeScriptHost] Detected change in %s, reloading...", plugin->name.c_str());
            ReloadPlugin(plugin->name);
        }
    }
}

bool NativeScriptHost::CompileScripts(const std::string& projectPath) {
    fs::path pluginsDir = fs::path(projectPath) / "plugins";
    std::error_code ec;
    
    if (!fs::exists(pluginsDir, ec)) {
        fs::create_directories(pluginsDir / "nat", ec);
        fs::create_directories(pluginsDir / "fpc", ec);
        fs::create_directories(pluginsDir / "lib", ec);
        if (ec) {
            m_lastError = "Failed to create plugin directories: " + ec.message();
            ui::LogAlways("[NativeScriptHost] ERROR: %s", m_lastError.c_str());
            return false;
        }
    }
    
    fs::create_directories(pluginsDir / "nat", ec);
    fs::create_directories(pluginsDir / "fpc", ec);
    fs::create_directories(pluginsDir / "lib", ec);
    
    return true;
}

// ============================================================================
// GAME SCRIPTS
// ============================================================================

namespace {
// SEH guard so a crashing script on MSVC builds is stopped instead of taking
// the editor down. Elsewhere the guard is a plain scope (see Platform.hpp).
int CallScriptTick(const FlyScriptClass* cls, void* instance, float dt) {
    FLY_TRY { return cls->tick(instance, dt); } FLY_CATCH(return -1;)
}
} // namespace

bool NativeScriptHost::InitializeScripts(const std::string& projectPath) {
    fs::path p = fs::u8path(projectPath);
    if (p.extension() == ".flyproj") p = p.parent_path();
    m_projectPath = p.string();

    std::error_code ec;
    fs::create_directories(p / "Scripts", ec);

    m_sourceStamp = scriptCompiler::SourcesStamp(m_projectPath);
    if (!scriptCompiler::HasSources(m_projectPath)) {
        ui::LogAlways("[Scripts] No scripts in %s", (p / "Scripts").string().c_str());
        return false;
    }

    ui::LogAlways("[Scripts] Compiling with %s ...",
                  scriptCompiler::CompilerDescription().empty() ? "(no compiler)"
                                                                : scriptCompiler::CompilerDescription().c_str());
    ui::SetScriptBuildState(ui::ScriptBuildState::Building);
    scriptCompiler::Result r = scriptCompiler::Build(m_projectPath);
    LogLines("[compiler]", r.log);
    PublishBuildResult(r);

    std::string lib = r.ok ? r.libraryPath : scriptCompiler::LatestBuiltLibrary(m_projectPath);
    if (!r.ok) {
        ui::LogWithSeverity(ui::LogSeverity::Error, "[Scripts] Build FAILED.%s",
                            lib.empty() ? "" : " Using the last successful build.");
    }
    if (lib.empty() || !LoadScriptLibrary(lib)) return false;
    scriptCompiler::RemoveStaleLibraries(m_projectPath, lib);
    return true;
}

void NativeScriptHost::RequestScriptRebuild() {
    if (m_projectPath.empty()) return;
    m_sourceStamp = scriptCompiler::SourcesStamp(m_projectPath);
    StartBuild();
}

void NativeScriptHost::WatchScriptSources(float dt) {
    if (m_projectPath.empty()) return;

    if (m_build.valid() &&
        m_build.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        FinishBuild();
    }

    if (!m_hotReload) return;
    m_watchTimer += dt;
    if (m_watchTimer < kWatchInterval) return;
    m_watchTimer = 0.0f;

    const uint64_t stamp = scriptCompiler::SourcesStamp(m_projectPath);
    if (stamp != m_sourceStamp) {
        m_sourceStamp = stamp;
        if (scriptCompiler::HasSources(m_projectPath)) StartBuild();
    } else if (m_rebuildQueued && !m_build.valid()) {
        StartBuild();
    }
}

void NativeScriptHost::StartBuild() {
    if (m_build.valid()) {
        m_rebuildQueued = true; // a source changed mid-build; go again afterwards
        return;
    }
    m_rebuildQueued = false;
    ui::LogAlways("[Scripts] Change detected, recompiling...");
    ui::SetScriptBuildState(ui::ScriptBuildState::Building);
    m_build = std::async(std::launch::async, scriptCompiler::Build, m_projectPath);
}

void NativeScriptHost::FinishBuild() {
    scriptCompiler::Result r = m_build.get();
    LogLines("[compiler]", r.log);
    if (!r.ok) {
        PublishBuildResult(r);
        ui::LogWithSeverity(ui::LogSeverity::Error,
                            "[Scripts] Build FAILED; keeping the previously loaded scripts.");
        return;
    }
    if (m_rebuildQueued) {
        // Already stale; don't bother swapping it in.
        scriptCompiler::RemoveStaleLibraries(m_projectPath, m_scriptLibPath);
        return;
    }

    // Remember what was running, swap libraries, then restart the same set.
    struct Restart { std::string className; ScatteredObject* object; int standaloneIndex; };
    std::vector<Restart> restart;
    for (const auto& rs : m_running)
        if (rs.instance) restart.push_back({ rs.className, rs.object, rs.standaloneIndex });

    if (!LoadScriptLibrary(r.libraryPath)) {
        scriptCompiler::Result failed;
        failed.diagnostics.push_back({ scriptCompiler::Diagnostic::Severity::Error, "", 0, 0,
                                       "The new script library could not be loaded (see the Output log)." });
        PublishBuildResult(failed);
        ui::LogWithSeverity(ui::LogSeverity::Error,
                            "[Scripts] Reload failed; keeping the previously loaded scripts.");
        return;
    }
    PublishBuildResult(r);

    for (const auto& rr : restart) {
        if (rr.object && !ObjectAlive(rr.object)) continue;
        StartScript(rr.className, rr.object, rr.standaloneIndex);
    }
    scriptCompiler::RemoveStaleLibraries(m_projectPath, m_scriptLibPath);
    SyncStandaloneScripts();
    ui::LogAlways("[Scripts] Reloaded (%d script%s restarted).", (int)restart.size(),
                  restart.size() == 1 ? "" : "s");
}

bool NativeScriptHost::LoadScriptLibrary(const std::string& path) {
    void* handle = LoadLibrary(path);
    if (!handle) {
        ui::LogAlways("[Scripts] ERROR: could not load %s", path.c_str());
        return false;
    }
    auto getModule = reinterpret_cast<FlyScript_GetModuleFn>(GetSymbol(handle, FLY_SCRIPT_ENTRY_NAME));
    const FlyScriptModule* module = getModule ? getModule(&m_api) : nullptr;
    if (!module || module->abi_version != FLY_SCRIPT_ABI_VERSION) {
        ui::LogAlways("[Scripts] ERROR: %s is not a compatible script library (rebuild it with this engine's SDK)",
                      path.c_str());
        UnloadLibrary(handle);
        return false;
    }

    // The new library loaded fine: retire the old one and its instances.
    UnloadScriptLibrary();
    m_scriptLib = handle;
    m_scriptLibPath = path;
    m_module = module;

    std::string names;
    for (int i = 0; i < m_module->class_count; ++i) {
        if (i) names += ", ";
        names += m_module->classes[i].name;
    }
    ui::LogAlways("[Scripts] Loaded %d script class%s: %s", m_module->class_count,
                  m_module->class_count == 1 ? "" : "es", names.empty() ? "(none)" : names.c_str());
    return true;
}

void NativeScriptHost::UnloadScriptLibrary() {
    for (auto& rs : m_running) DestroyRunning(rs);
    m_running.clear();
    if (m_scriptLib) UnloadLibrary(m_scriptLib);
    m_scriptLib = nullptr;
    m_module = nullptr;
    m_scriptLibPath.clear();
}

const FlyScriptClass* NativeScriptHost::FindClass(const std::string& name) const {
    if (!m_module) return nullptr;
    for (int i = 0; i < m_module->class_count; ++i)
        if (name == m_module->classes[i].name) return &m_module->classes[i];
    return nullptr;
}

std::vector<std::string> NativeScriptHost::GetScriptClassNames() const {
    std::vector<std::string> names;
    if (!m_module) return names;
    for (int i = 0; i < m_module->class_count; ++i) names.push_back(m_module->classes[i].name);
    return names;
}

void NativeScriptHost::BindWorld(std::vector<ScatteredObject*>& objects,
                                 std::vector<std::unique_ptr<ModelGroup>>& models,
                                 Camera3D& camera, phys::Simulation& sim, Engine& engine) {
    m_runtime->BindWorld(objects, models, camera, sim, engine);
}

void NativeScriptHost::SyncStandaloneScripts() {
    auto& scripts = m_runtime->StandaloneScripts();
    for (const std::string& name : GetScriptClassNames()) {
        bool listed = false;
        for (const auto& s : scripts)
            if (s.typeName == name) { listed = true; break; }
        // Classes attached to an object already run there on Play; listing them
        // as standalone too would run them twice.
        if (!listed && m_runtime->HasWorld()) {
            for (auto* obj : m_runtime->GetObjects())
                if (obj && obj->script == name) { listed = true; break; }
        }
        if (listed) continue;
        ScriptRuntime::StandaloneScript s;
        s.name = name;
        s.typeName = name;
        s.runOnPlay = true;
        scripts.push_back(std::move(s));
    }
}

bool NativeScriptHost::ObjectAlive(ScatteredObject* obj) const {
    if (!obj || !m_runtime->HasWorld()) return false;
    const auto& objects = m_runtime->GetObjects();
    return std::find(objects.begin(), objects.end(), obj) != objects.end();
}

bool NativeScriptHost::StartScript(const std::string& className, ScatteredObject* obj, int standaloneIndex) {
    if (className.empty()) return false;
    if (!m_module) {
        ui::LogAlways("[Scripts] Cannot start '%s': no script library is loaded (check the compiler output).",
                      className.c_str());
        return false;
    }
    const FlyScriptClass* cls = FindClass(className);
    if (!cls) {
        ui::LogAlways("[Scripts] Cannot start '%s': no such class. Is FLY_SCRIPT(%s) in a Scripts/*.cpp file?",
                      className.c_str(), className.c_str());
        return false;
    }

    // One script per object / standalone slot: replace whatever ran there.
    if (obj) StopObjectScriptOn(obj);
    else StopStandaloneScriptAt(standaloneIndex);

    void* instance = cls->create(cls);
    if (!instance) return false;
    m_running.push_back({ cls, instance, obj, obj ? -1 : standaloneIndex, className });
    return true;
}

void NativeScriptHost::DestroyRunning(RunningScript& rs) {
    if (rs.instance && rs.cls) rs.cls->destroy(rs.instance);
    rs.instance = nullptr;
}

void NativeScriptHost::TickScripts(float dt) {
    if (m_running.empty()) return;
    m_ticking = true;
    for (size_t i = 0; i < m_running.size(); ++i) {
        if (!m_running[i].instance) continue;
        ScatteredObject* obj = m_running[i].object;
        if (obj && !ObjectAlive(obj)) {
            DestroyRunning(m_running[i]); // its object was deleted
            continue;
        }
        m_runtime->SetScriptSelf(obj);
        const int alive = CallScriptTick(m_running[i].cls, m_running[i].instance, dt);
        if (alive == -1) {
            ui::LogAlways("[Scripts] %s crashed and was stopped.", m_running[i].className.c_str());
            m_running[i].instance = nullptr; // state is unknown; leak rather than call into it again
        } else if (!alive) {
            DestroyRunning(m_running[i]);
        }
    }
    m_runtime->SetScriptSelf(nullptr);
    m_ticking = false;
    m_running.erase(std::remove_if(m_running.begin(), m_running.end(),
                                   [](const RunningScript& rs) { return !rs.instance; }),
                    m_running.end());
}

bool NativeScriptHost::StartObjectScriptOn(ScatteredObject* obj) {
    if (!obj || obj->script.empty()) return false;
    return StartScript(obj->script, obj, -1);
}

void NativeScriptHost::StopObjectScriptOn(ScatteredObject* obj) {
    for (auto& rs : m_running)
        if (rs.object == obj && rs.instance) DestroyRunning(rs);
    if (!m_ticking)
        m_running.erase(std::remove_if(m_running.begin(), m_running.end(),
                                       [](const RunningScript& rs) { return !rs.instance; }),
                        m_running.end());
}

bool NativeScriptHost::IsObjectScriptRunning(ScatteredObject* obj) const {
    for (const auto& rs : m_running)
        if (rs.object == obj && rs.instance) return true;
    return false;
}

bool NativeScriptHost::StartStandaloneScriptAt(int index) {
    auto& scripts = m_runtime->StandaloneScripts();
    if (index < 0 || index >= static_cast<int>(scripts.size())) return false;
    return StartScript(scripts[index].typeName, nullptr, index);
}

void NativeScriptHost::StopStandaloneScriptAt(int index) {
    for (auto& rs : m_running)
        if (!rs.object && rs.standaloneIndex == index && rs.instance) DestroyRunning(rs);
    if (!m_ticking)
        m_running.erase(std::remove_if(m_running.begin(), m_running.end(),
                                       [](const RunningScript& rs) { return !rs.instance; }),
                        m_running.end());
}

bool NativeScriptHost::IsStandaloneScriptRunning(int index) const {
    for (const auto& rs : m_running)
        if (!rs.object && rs.standaloneIndex == index && rs.instance) return true;
    return false;
}

void NativeScriptHost::OnStandaloneRemoved(int index) {
    StopStandaloneScriptAt(index);
    for (auto& rs : m_running)
        if (!rs.object && rs.standaloneIndex > index) --rs.standaloneIndex;
}

void NativeScriptHost::ClearAllScripts() {
    for (auto& rs : m_running) DestroyRunning(rs);
    if (!m_ticking) m_running.clear();
}

void NativeScriptHost::SetPlayActive(bool active) {
    m_pendingPlay = active;
    m_pendingStop = !active;
}

} // namespace NativeScript