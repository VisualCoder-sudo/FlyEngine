/*
 * NativeScriptHost.cpp - Implementation of native plugin loading and hot-reload
 * Uses the new CPluginAPI (vtable-based) for plugin communication.
 */

#include "../../../include/Engine/Scripts/NativeScriptHost.hpp"
#include "../../../include/CPluginAPI.h"
#include "../../../include/Engine.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"

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

NativeScriptHost::NativeScriptHost() = default;

NativeScriptHost::~NativeScriptHost() {
    Shutdown();
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
    if (!m_engine) return;
    
    CheckHotReload();
    
    for (auto& plugin : plugins) {
        if (plugin->loaded && plugin->entry && plugin->entry->on_update) {
            plugin->entry->on_update(dt);
        }
    }
}

void NativeScriptHost::Shutdown() {
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
    CP_Internal_SetScriptRuntime(nullptr);
    
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
    CP_Internal_SetScriptRuntime(nullptr);
    
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

std::string NativeScriptHost::GetCompilerCommand(const std::string& sourcePath, const std::string& outputPath) {
    #if defined(_WIN32)
        return "cl /LD /O2 /Fe:" + outputPath + " " + sourcePath;
    #else
        return "gcc -shared -fPIC -O2 -o " + outputPath + " " + sourcePath;
    #endif
}

std::string NativeScriptHost::FindCompiler() {
    #if defined(_WIN32)
        if (system("where cl.exe >nul 2>nul") == 0) return "cl.exe";
        if (system("where gcc.exe >nul 2>nul") == 0) return "gcc.exe";
        return "";
    #else
        if (system("which gcc >/dev/null 2>&1") == 0) return "gcc";
        if (system("which clang >/dev/null 2>&1") == 0) return "clang";
        return "";
    #endif
}

} // namespace NativeScript