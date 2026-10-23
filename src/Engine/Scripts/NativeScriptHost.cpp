#include "../../../include/Engine/Backend/SoundRays.hpp"
#include "../../../include/Engine/Backend/VisualScript.hpp"
/*
 * NativeScriptHost.cpp - Native plugin loading, C++ game scripts, hot-reload.
 * Plugins use CPluginAPI (vtable-based); scripts use FlyScriptABI.h.
 */

#include "../../../include/Engine/Scripts/NativeScriptHost.hpp"
#include "../../../include/Engine/Scripts/StaticModules.hpp"
#include "../../../include/Engine/Plugin/CPluginAPI.h"
#include "../../../include/Engine.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Engine/Frontend/PluginHost.hpp"
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
#include <set>
#include <thread>
#include "Engine/U8Path.hpp"

#if defined(_WIN32)
    #include "../../../include/Engine/Platform/WinBegin.h"
    #include <windows.h>
    #include "../../../include/Engine/Platform/WinEnd.h"
    // windows.h maps LoadLibrary -> LoadLibraryA, which would rename the
    // NativeScriptHost::LoadLibrary member defined below.
    #undef LoadLibrary
#endif

namespace fs = std::filesystem;

namespace NativeScript {

// --- Modules compiled into the executable (exported games), see StaticModules.hpp -----------------------------
namespace {
// Set from static initializers of the generated glue, which may run before this file's own: plain data only.
StaticScriptEntry g_staticScriptEntry = nullptr;
const StaticPlugin* g_staticPluginTable = nullptr;
size_t g_staticPluginCount = 0;
} // namespace

void SetStaticScriptEntry(StaticScriptEntry entry) { g_staticScriptEntry = entry; }
void SetStaticPlugins(const StaticPlugin* plugins, size_t count) { g_staticPluginTable = plugins; g_staticPluginCount = count; }
bool HasStaticModules() { return g_staticScriptEntry != nullptr || g_staticPluginCount > 0; }


// ============================================================================
// PLATFORM-SPECIFIC LIBRARY LOADING
// ============================================================================

#if defined(_WIN32)
    // windows.h is included at the top of the file (it must not end up inside
    // namespace NativeScript).

    void* NativeScriptHost::LoadLibrary(const std::string& path) {
        void* handle = ::LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!handle) {
            // 126 = the library or one of its dependent DLLs was not found, 193 = wrong architecture.
            ui::LogAlways("[NativeScriptHost] LoadLibrary failed (error %lu)", static_cast<unsigned long>(GetLastError()));
        }
        return handle;
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
    a.SnippetGetInput = FlyNative_SnippetGetInput;
    a.SnippetSetOutput = FlyNative_SnippetSetOutput;
    a.DestroyObject = FlyNative_DestroyObject;
    a.CloneObject = FlyNative_CloneObject;
    a.GetName = FlyNative_GetName;
    a.SetName = FlyNative_SetName;
    a.AddForce = FlyNative_AddForce;
    a.AddImpulse = FlyNative_AddImpulse;
    a.Raycast = FlyNative_Raycast;
    a.StartObjectScript = FlyNative_StartObjectScript;
    a.StopObjectScript = FlyNative_StopObjectScript;
    a.KeyDown = FlyNative_KeyDown;
    a.KeyPressed = FlyNative_KeyPressed;
    a.PlaySound = FlyNative_PlaySound;
    a.SetMasterVolume = FlyNative_SetMasterVolume;
    a.ShowText = FlyNative_ShowText;
    a.TimeSincePlay = FlyNative_TimeSincePlay;
    a.GamepadAvailable = FlyNative_GamepadAvailable;
    a.GamepadButtonDown = FlyNative_GamepadButtonDown;
    a.GamepadButtonPressed = FlyNative_GamepadButtonPressed;
    a.GamepadAxis = FlyNative_GamepadAxis;
    a.RaycastEx = FlyNative_RaycastEx;
    a.OverlapSphere = FlyNative_OverlapSphere;
    a.PollTouches = FlyNative_PollTouches;
    a.CreateJoint = FlyNative_CreateJoint;
    a.DestroyJoint = FlyNative_DestroyJoint;
    a.GetCamera = FlyNative_GetCamera;
    a.SetCamera = FlyNative_SetCamera;
    a.EmitParticles = FlyNative_EmitParticles;
    a.StartEmitter = FlyNative_StartEmitter;
    a.MoveEmitter = FlyNative_MoveEmitter;
    a.StopEmitter = FlyNative_StopEmitter;
    a.SpawnDecal = FlyNative_SpawnDecal;
    a.SpawnPrefab = FlyNative_SpawnPrefab;
    a.UICreate = FlyNative_UICreate;
    a.UISetLayout = FlyNative_UISetLayout;
    a.UISetColor = FlyNative_UISetColor;
    a.UISetText = FlyNative_UISetText;
    a.UISetValue = FlyNative_UISetValue;
    a.UISetImage = FlyNative_UISetImage;
    a.UISetVisible = FlyNative_UISetVisible;
    a.UIRemove = FlyNative_UIRemove;
    a.UISetFont = FlyNative_UISetFont;
    a.UIExists = FlyNative_UIExists;
    a.UIGetValue = FlyNative_UIGetValue;
    a.UIGetText = FlyNative_UIGetText;
    a.UIGetVisible = FlyNative_UIGetVisible;
    a.UIGetLayout = FlyNative_UIGetLayout;
    a.UIGetColor = FlyNative_UIGetColor;
    a.GetMouseDelta = FlyNative_GetMouseDelta;
    a.DebugText = FlyNative_DebugText;
    a.NavBuild = FlyNative_NavBuild;
    a.NavClear = FlyNative_NavClear;
    a.NavReady = FlyNative_NavReady;
    a.NavFindPath = FlyNative_NavFindPath;
    a.NavNearest = FlyNative_NavNearest;
    a.NavIsWalkable = FlyNative_NavIsWalkable;
    a.NavRandomPoint = FlyNative_NavRandomPoint;
    a.NavDebug = FlyNative_NavDebug;
    a.UIGetState = FlyNative_UIGetState;
    a.GetMousePosition = FlyNative_GetMousePosition;
    a.SetMouseFree = FlyNative_SetMouseFree;
    a.MouseOverUI = FlyNative_MouseOverUI;
    a.NavLineClear = FlyNative_NavLineClear;
    a.NavMoveAlong = FlyNative_NavMoveAlong;
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
    pluginhost::SetEventSink([this](pluginhost::SceneEvent e) { DispatchSceneEvent(e); });
}

// The scene was swapped under the plugins (load, undo/redo, stop-play): their object and group handles are stale.
void NativeScriptHost::DispatchSceneEvent(pluginhost::SceneEvent e) {
    CP_Internal_InvalidateHandles();
    CP_Event event{};
    event.type = (e == pluginhost::SceneEvent::Loaded) ? CP_EVENT_SCENE_LOADED : CP_EVENT_UNDO_REDO;
    for (auto& plugin : plugins) {
        if (plugin->loaded && plugin->entry && plugin->entry->on_event) plugin->entry->on_event(&event);
    }
}

// Editor viewport pass for plugin tools. Not in Play, and not in the player (no scene host there).
void NativeScriptHost::DrawOverlay3D() {
    if (!pluginhost::IsEditor() || (m_runtime && m_runtime->IsSimPlaying())) return;
    for (auto& plugin : plugins) {
        // on_draw_overlay3d was added to CP_PluginEntry in 1.2: older plugins have a shorter struct.
        if (plugin->loaded && plugin->entry && CP_API_VersionAtLeast(plugin->entry->api_version, CP_API_Version{1, 2, 0}) &&
            plugin->entry->on_draw_overlay3d)
            plugin->entry->on_draw_overlay3d();
    }
}

NativeScriptHost::~NativeScriptHost() {
    Shutdown();
    pluginhost::SetEventSink(nullptr);
    if (g_activeHost == this) {
        g_activeHost = nullptr;
        SetPlayStateHook(nullptr);
    }
    if (GetActiveRuntime() == m_runtime.get()) SetActiveRuntime(nullptr);
}

// The scripting runtime has to know where the project is: Play Sound resolves "assets/audio/x.wav" against it, and plugins ask
// for it through the API. A .flyproj file stands for its folder. (An exported game passes the archive's virtual root.)
void NativeScriptHost::BindProjectFolder(const std::string& projectPath) {
    if (!m_runtime || projectPath.empty()) return;
    fs::path p = U8Path(projectPath);
    if (p.extension() == ".flyproj") p = p.parent_path();
    m_runtime->SetProjectPath(p.generic_string());
}

bool NativeScriptHost::Initialize(const std::string& projectPath) {
    BindProjectFolder(projectPath);
    m_projectPath = projectPath;
    m_lastError.clear();
    
    ui::LogAlways("[NativeScriptHost] Initializing for project: %s", projectPath.c_str());

    // An exported game has its plugins built in: nothing to scan, and no folders to create next to the executable.
    if (g_staticPluginCount > 0 || g_staticScriptEntry) m_projectPath.clear();   // nothing on disk to watch
    if (g_staticPluginCount > 0) {
        for (size_t i = 0; i < g_staticPluginCount; ++i) LoadStaticPlugin(g_staticPluginTable[i].name, g_staticPluginTable[i].getEntry);
        ui::LogAlways("[NativeScriptHost] Initialized with %zu built-in plugins", plugins.size());
        return true;
    }
    if (g_staticScriptEntry) return true;   // scripts only: still nothing on disk to load

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
    
    LoadDisabledList();
    ScanPluginDirs();
    
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
    if (m_runtime->IsSimPlaying()) {
        vscript::Tick(*m_runtime, dt);
        soundrays::Update(*m_runtime, dt);   // object sounds: start, follow, muffle, echo
    }

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
        if (plugin->entry) pluginhost::RemoveModesOf(plugin->entry);   // before its code can go away
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

// plugins/disabled.txt: one library name per line.
void NativeScriptHost::LoadDisabledList() {
    m_disabledPlugins.clear();
    std::ifstream in(fs::path(U8Path(m_projectPath)) / "plugins" / "disabled.txt");
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) m_disabledPlugins.insert(line);
    }
}

void NativeScriptHost::SaveDisabledList() const {
    if (m_projectPath.empty()) return;
    const fs::path file = fs::path(U8Path(m_projectPath)) / "plugins" / "disabled.txt";
    std::error_code ec;
    if (m_disabledPlugins.empty()) { fs::remove(file, ec); return; }
    std::ofstream out(file, std::ios::trunc);
    for (const auto& n : m_disabledPlugins) out << n << '\n';
}

// Fills the manager's display fields from the plugin folder (<dir>/id.json). A plugin without one still loads.
void NativeScriptHost::ReadPluginIdentity(PluginModule& plugin) const {
    const fs::path build = fs::path(U8Path(plugin.path)).parent_path();   // <dir>/build
    plugin.dir = build.parent_path().generic_string();
    plugin.type = build.parent_path().parent_path().filename().string();
    plugin.displayName = plugin.name;

    char* err = nullptr;
    PluginIdentity* id = PluginIdentity_ParseFromFile((build.parent_path() / "id.json").string().c_str(), &err);
    if (err) free(err);
    if (!id) return;
    auto take = [](const char* v) { return std::string(v ? v : ""); };
    if (id->name && *id->name) plugin.displayName = id->name;
    plugin.version = take(id->version);
    plugin.publisher = take(id->publisher);
    plugin.description = take(id->description);
    plugin.category = take(id->category);
    plugin.declaredApi = take(id->cplugin_api_version);
    plugin.hasIdentity = true;
    PluginIdentity_Free(id);
}

// Drops the library and everything the plugin registered; the record stays.
void NativeScriptHost::UnloadModule(PluginModule& plugin) {
    if (plugin.entry) pluginhost::RemoveModesOf(plugin.entry);   // before its code can go away
    if (plugin.loaded && plugin.entry && plugin.entry->on_unload) plugin.entry->on_unload();
    if (plugin.handle) {
        UnloadLibrary(plugin.handle);
        plugin.handle = nullptr;
    }
    plugin.entry = nullptr;   // pointed into the library
    plugin.loaded = false;
}

bool NativeScriptHost::LoadPlugin(const std::string& pluginPath) {
    fs::path path(pluginPath);
    std::string name = path.stem().string();
    
    for (auto& p : plugins) {
        if (p->name == name) {
            return p->enabled && ReloadPlugin(name);
        }
    }
    
    auto plugin = std::make_unique<PluginModule>();
    plugin->name = name;
    plugin->path = pluginPath;
    plugin->lastWriteTime = this->GetFileWriteTime(pluginPath);
    ReadPluginIdentity(*plugin);

    // Every failure keeps the record, so the Plugin Manager can show why it did not load.
    auto fail = [&](const std::string& why) {
        ui::LogAlways("[NativeScriptHost] ERROR: %s: %s", pluginPath.c_str(), why.c_str());
        plugin->loadFailed = true;
        plugin->errorMessage = why;
        UnloadModule(*plugin);
        plugins.push_back(std::move(plugin));
        return false;
    };

    if (m_disabledPlugins.count(name)) {
        plugin->enabled = false;
        ui::LogAlways("[NativeScriptHost] Plugin %s is disabled", name.c_str());
        plugins.push_back(std::move(plugin));
        return true;
    }

    ui::LogAlways("[NativeScriptHost] Loading plugin: %s", pluginPath.c_str());
    
    plugin->handle = LoadLibrary(pluginPath);
    if (!plugin->handle) return fail("Failed to load library");
    
    auto get_entry = reinterpret_cast<CP_PluginEntry* (*)(void)>(GetSymbol(plugin->handle, "CP_GetPluginEntry"));
    if (!get_entry) return fail("Plugin missing CP_GetPluginEntry symbol (requires new CPluginAPI)");
    
    plugin->entry = get_entry();
    if (!plugin->entry) return fail("CP_GetPluginEntry returned NULL");
    
    CP_API_Version engine_ver = CP_API_CURRENT_VERSION;
    CP_API_Version plugin_ver = plugin->entry->api_version;
    if (!CP_API_VersionAtLeast(engine_ver, plugin_ver)) return fail("Plugin API version newer than engine supports");
    
    CP_Internal_SetEngine(m_engine);
    CP_Internal_SetScriptRuntime(m_runtime.get());
    
    if (plugin->entry->on_load) {
        pluginhost::SetLoadingOwner(plugin->entry);
        int result = plugin->entry->on_load(CP_GetEngineAPI());
        pluginhost::SetLoadingOwner(nullptr);
        if (result != 0) return fail("on_load returned error code: " + std::to_string(result));   // UnloadModule drops what it registered
    }
    
    plugin->loaded = true;
    plugins.push_back(std::move(plugin));
    ui::LogAlways("[NativeScriptHost] Loaded plugin: %s", name.c_str());
    return true;
}

bool NativeScriptHost::SetPluginEnabled(const std::string& pluginName, bool enabled) {
    for (auto& p : plugins) {
        if (p->name != pluginName || p->enabled == enabled) continue;
        p->enabled = enabled;
        if (enabled) {
            m_disabledPlugins.erase(pluginName);
            SaveDisabledList();
            return ReloadPlugin(pluginName);
        }
        UnloadModule(*p);
        p->loadFailed = false;
        p->errorMessage.clear();
        m_disabledPlugins.insert(pluginName);
        SaveDisabledList();
        ui::LogAlways("[NativeScriptHost] Disabled plugin: %s", pluginName.c_str());
        return true;
    }
    return false;
}

// Walks plugins/{nat,lib}/*/build/*.{so,dll}. Plugins already known are left alone (LoadPlugin would reload them).
int NativeScriptHost::ScanPluginDirs() {
    const size_t before = plugins.size();
    const fs::path pluginsDir = fs::path(U8Path(m_projectPath)) / "plugins";
    std::error_code ec;
    for (const auto& typeDir : {"nat", "lib"}) {
        fs::path typePath = pluginsDir / typeDir;
        if (!fs::exists(typePath, ec)) continue;
        for (const auto& entry : fs::directory_iterator(typePath, ec)) {
            if (ec || !entry.is_directory()) continue;
            fs::path buildDir = entry.path() / "build";
            if (!fs::exists(buildDir, ec)) continue;
            for (const auto& file : fs::directory_iterator(buildDir, ec)) {
                if (!file.is_regular_file()) continue;
                const std::string ext = file.path().extension().string();
                #if defined(_WIN32)
                    if (ext != ".dll") continue;
                #else
                    if (ext != ".so") continue;
                #endif
                const std::string stem = file.path().stem().string();
                const bool known = std::any_of(plugins.begin(), plugins.end(), [&](const auto& p) { return p->name == stem; });
                if (!known) LoadPlugin(file.path().string());
            }
        }
    }
    return int(plugins.size() - before);
}

static bool IsPluginLibrary(const fs::path& f) {
    const std::string ext = f.extension().string();
#if defined(_WIN32)
    return ext == ".dll";
#else
    return ext == ".so";
#endif
}

// A folder name that is safe to create under plugins/: no separators, no "..", no leading dot.
static bool SafePluginName(const std::string& n) {
    if (n.empty() || n == "." || n == ".." || n[0] == '.') return false;
    return n.find_first_of("/\\:*?\"<>|") == std::string::npos;
}

NativeScriptHost::ImportResult NativeScriptHost::ImportPlugin(const std::string& source) {
    ImportResult r;
    if (m_projectPath.empty()) { r.message = "No project folder: plugins cannot be imported here."; return r; }
    std::error_code ec;
    const fs::path src = fs::path(U8Path(source));
    const fs::path pluginsRoot = fs::path(U8Path(m_projectPath)) / "plugins";

    std::string name, type = "nat";
    fs::path folderSrc, fileSrc;
    if (fs::is_regular_file(src, ec)) {
        if (!IsPluginLibrary(src)) { r.message = "Not a plugin library: " + src.filename().string(); return r; }
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
    if (!SafePluginName(name)) { r.message = "Unusable plugin name: \"" + name + "\""; return r; }

    const fs::path dest = pluginsRoot / type / name;
    if (fs::exists(dest, ec)) { r.message = "\"" + name + "\" is already installed. Remove its folder first to replace it."; return r; }
    if (!folderSrc.empty()) {
        // Importing from inside the plugins folder would copy it onto itself.
        const fs::path a = fs::weakly_canonical(folderSrc, ec), b = fs::weakly_canonical(pluginsRoot, ec);
        if (a.generic_string().rfind(b.generic_string() + "/", 0) == 0) { r.message = "That plugin is already inside the project's plugins folder. Use Rescan."; return r; }
    }

    // Copy next to the destination and rename, so a failed copy never leaves a half-installed plugin.
    const fs::path staging = pluginsRoot / type / (name + ".importing");
    fs::remove_all(staging, ec);
    fs::create_directories(staging / "build", ec);
    if (ec) { r.message = "Could not create " + staging.string() + ": " + ec.message(); return r; }
    if (!fileSrc.empty()) {
        fs::copy_file(fileSrc, staging / "build" / fileSrc.filename(), ec);
    } else {
        fs::copy(folderSrc, staging, fs::copy_options::recursive | fs::copy_options::skip_symlinks, ec);
    }
    if (ec) { fs::remove_all(staging, ec); r.message = "Copy failed: " + ec.message(); return r; }
    fs::rename(staging, dest, ec);
    if (ec) { fs::remove_all(staging, ec); r.message = "Could not install into " + dest.string() + ": " + ec.message(); return r; }

    ScanPluginDirs();
    r.ok = true;
    bool hasLib = false;
    for (const auto& f : fs::directory_iterator(dest / "build", ec))
        if (!ec && f.is_regular_file() && IsPluginLibrary(f.path())) hasLib = true;
    r.message = hasLib ? "Imported \"" + name + "\"."
                       : "Copied \"" + name + "\", but it has no built library in build/. Build it, then press Rescan.";
    ui::LogAlways("[NativeScriptHost] %s", r.message.c_str());
    return r;
}

NativeScriptHost::ImportResult NativeScriptHost::RemovePlugin(const std::string& pluginName) {
    ImportResult r;
    for (auto it = plugins.begin(); it != plugins.end(); ++it) {
        if ((*it)->name != pluginName) continue;
        std::error_code ec;
        const fs::path dir = fs::weakly_canonical(fs::path(U8Path((*it)->dir)), ec);
        const fs::path root = fs::weakly_canonical(fs::path(U8Path(m_projectPath)) / "plugins", ec);
        // Only ever delete a folder that sits below plugins/<type>/.
        if ((*it)->dir.empty() || dir.generic_string().rfind(root.generic_string() + "/", 0) != 0 || dir.parent_path().parent_path() != root) {
            r.message = "Refusing to delete \"" + pluginName + "\": its folder is not under the project's plugins folder.";
            return r;
        }
        UnloadModule(**it);
        fs::remove_all(dir, ec);
        if (ec) r.message = "Unloaded \"" + pluginName + "\" but could not delete " + dir.string() + ": " + ec.message();
        else { r.ok = true; r.message = "Removed \"" + pluginName + "\"."; }
        plugins.erase(it);
        if (m_disabledPlugins.erase(pluginName)) SaveDisabledList();
        ui::LogAlways("[NativeScriptHost] %s", r.message.c_str());
        return r;
    }
    r.message = "No such plugin: " + pluginName;
    return r;
}

int NativeScriptHost::RescanPlugins() {
    if (m_projectPath.empty()) return 0;   // exported game: nothing on disk
    return ScanPluginDirs();
}

bool NativeScriptHost::LoadStaticPlugin(const char* name, CP_PluginEntry* (*getEntry)(void)) {
    auto plugin = std::make_unique<PluginModule>();
    plugin->name = name ? name : "plugin";
    plugin->entry = getEntry ? getEntry() : nullptr;
    if (!plugin->entry) {
        ui::LogAlways("[NativeScriptHost] ERROR: built-in plugin %s returned no entry", plugin->name.c_str());
        return false;
    }
    if (!CP_API_VersionAtLeast(CP_API_CURRENT_VERSION, plugin->entry->api_version)) {
        ui::LogAlways("[NativeScriptHost] ERROR: built-in plugin %s needs a newer plugin API", plugin->name.c_str());
        return false;
    }
    CP_Internal_SetEngine(m_engine);
    CP_Internal_SetScriptRuntime(m_runtime.get());
    if (plugin->entry->on_load) {
        pluginhost::SetLoadingOwner(plugin->entry);
        const int result = plugin->entry->on_load(CP_GetEngineAPI());
        pluginhost::SetLoadingOwner(nullptr);
        if (result != 0) {
            ui::LogAlways("[NativeScriptHost] ERROR: built-in plugin %s on_load failed with code %d", plugin->name.c_str(), result);
            pluginhost::RemoveModesOf(plugin->entry);   // drop whatever it registered before failing
            return false;
        }
    }
    plugin->loaded = true;
    ui::LogAlways("[NativeScriptHost] Loaded built-in plugin: %s", plugin->name.c_str());
    plugins.push_back(std::move(plugin));
    return true;
}

void NativeScriptHost::UnloadPlugin(const std::string& pluginName) {
    for (auto it = plugins.begin(); it != plugins.end(); ++it) {
        if ((*it)->name == pluginName) {
            auto& plugin = *it;
            if (plugin->entry) pluginhost::RemoveModesOf(plugin->entry);   // before its code can go away
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
    
    if (plugin->entry) pluginhost::RemoveModesOf(plugin->entry);   // before its code can go away
    if (plugin->loaded && plugin->entry && plugin->entry->on_unload) {
        plugin->entry->on_unload();
    }
    
    if (plugin->handle) {
        UnloadLibrary(plugin->handle);
        plugin->handle = nullptr;
    }
    plugin->entry = nullptr;   // points into the unloaded library; stays null if the reload below fails
    plugin->loaded = false;
    
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
        pluginhost::SetLoadingOwner(plugin->entry);
        int result = plugin->entry->on_load(CP_GetEngineAPI());
        pluginhost::SetLoadingOwner(nullptr);
        if (result != 0) {
            plugin->loadFailed = true;
            plugin->errorMessage = "on_load failed after reload: " + std::to_string(result);
            pluginhost::RemoveModesOf(plugin->entry);
            plugin->entry = nullptr;
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
        if (!plugin->loaded && !plugin->loadFailed) continue;   // a failed reload is retried once the file changes again
        
        uint64_t currentTime = this->GetFileWriteTime(plugin->path);
        if (currentTime != plugin->lastWriteTime && currentTime != 0) {
            plugin->lastWriteTime = currentTime;   // a failed reload waits for the next change instead of retrying every frame
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
    BindProjectFolder(projectPath);
    if (g_staticScriptEntry) {
        m_projectPath.clear();   // nothing to build, watch or hot-reload: the scripts are part of this executable
        return LoadStaticScripts();
    }
    fs::path p = U8Path(projectPath);
    if (p.extension() == ".flyproj") p = p.parent_path();
    m_projectPath = p.string();

    std::error_code ec;
    fs::create_directories(p / "Scripts", ec);

    vscript::WriteSnippetSources(m_projectPath);   // flush an edit the editor's idle timer hasn't written yet (no-op when unchanged)

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
        StartScript(rr.className, rr.object, rr.standaloneIndex, false);
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

bool NativeScriptHost::LoadStaticScripts() {
    const FlyScriptModule* module = g_staticScriptEntry ? g_staticScriptEntry(&m_api) : nullptr;
    if (!module || module->abi_version != FLY_SCRIPT_ABI_VERSION) {
        ui::LogAlways("[Scripts] ERROR: the scripts built into this executable do not match the engine's script ABI");
        return false;
    }
    UnloadScriptLibrary();
    m_module = module;
    {   // a rebuilt library no longer has the deleted classes: forget the ones that are really gone
        std::vector<std::string> still;
        for (const auto& h : m_hiddenClasses)
            for (int k = 0; k < m_module->class_count; ++k)
                if (h == m_module->classes[k].name) { still.push_back(h); break; }
        m_hiddenClasses = still;
    }
    std::string names;
    for (int i = 0; i < m_module->class_count; ++i) {
        if (i) names += ", ";
        names += m_module->classes[i].name;
    }
    ui::LogAlways("[Scripts] %d built-in script class%s: %s", m_module->class_count,
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
    for (int i = 0; i < m_module->class_count; ++i) {
        const std::string name = m_module->classes[i].name;
        if (name.rfind(vscript::kSnippetClassPrefix, 0) == 0) continue;   // generated for Code Snippet nodes, not a script to list or attach
        names.push_back(name);
    }
    return names;
}

void NativeScriptHost::BindWorld(std::vector<ScatteredObject*>& objects,
                                 std::vector<std::unique_ptr<ModelGroup>>& models,
                                 Camera3D& camera, phys::Simulation& sim, Engine& engine) {
    m_runtime->BindWorld(objects, models, camera, sim, engine);
}

void NativeScriptHost::ForgetScriptClass(const std::string& className) {
    if (std::find(m_hiddenClasses.begin(), m_hiddenClasses.end(), className) == m_hiddenClasses.end())
        m_hiddenClasses.push_back(className);
    RequestScriptRebuild();
}

void NativeScriptHost::SyncStandaloneScripts() {
    auto& scripts = m_runtime->StandaloneScripts();
    for (const std::string& name : GetScriptClassNames()) {
        if (std::find(m_hiddenClasses.begin(), m_hiddenClasses.end(), name) != m_hiddenClasses.end()) continue;
        bool listed = false;
        for (const auto& s : scripts)
            if (s.typeName == name) { listed = true; break; }
        // Classes attached to an object already run there on Play; listing them
        // as standalone too would run them twice.
        if (!listed && m_runtime->HasWorld()) {
            for (auto* obj : m_runtime->GetObjects())
                if (obj && obj->HasScript(name)) { listed = true; break; }
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

bool NativeScriptHost::StartScript(const std::string& className, ScatteredObject* obj, int standaloneIndex, bool replace) {
    if (className.empty()) return false;
    if (vscript::Has(className) && (!m_module || !FindClass(className))) return false;   // a visual-only script
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

    // One script per standalone slot: replace whatever ran there. An object can run several scripts, so the caller
    // stops its old ones once (replace) and then starts each. (Snippets have neither: several can run.)
    if (obj) {
        if (replace) StopObjectScriptOn(obj);
    } else if (standaloneIndex >= 0) StopStandaloneScriptAt(standaloneIndex);

    void* instance = cls->create(cls);
    if (!instance) return false;
    m_running.push_back({ cls, instance, obj, obj ? -1 : standaloneIndex, className });
    return true;
}

void NativeScriptHost::DestroyRunning(RunningScript& rs) {
    // A script stopping itself (StopScript on its own object) must not be destroyed under its own feet.
    if (m_ticking && rs.instance && rs.instance == m_executing) { rs.stopPending = true; return; }
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
        m_executing = m_running[i].instance;
        const int alive = CallScriptTick(m_running[i].cls, m_running[i].instance, dt);
        m_executing = nullptr;
        if (alive == -1) {
            ui::LogAlways("[Scripts] %s crashed and was stopped.", m_running[i].className.c_str());
            m_running[i].instance = nullptr; // state is unknown; leak rather than call into it again
        } else if (!alive || m_running[i].stopPending) {
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
    StopObjectScriptOn(obj);
    bool any = false;
    for (const auto& name : obj->AllScripts()) any |= StartScript(name, obj, -1, false);
    return any;
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

bool NativeScriptHost::StartSnippetScript(const std::string& className) {
    if (!m_module) return false;
    const FlyScriptClass* cls = FindClass(className);
    if (!cls) return false;
    void* instance = cls->create(cls);
    if (!instance) return false;
    m_running.push_back({ cls, instance, nullptr, -1, className });
    return true;
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