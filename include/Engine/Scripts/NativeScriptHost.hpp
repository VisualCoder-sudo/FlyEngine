/*
 * NativeScriptHost.hpp - Loads and manages native C/C++ plugin modules (.so/.dll)
 * Uses the new CPluginAPI (vtable-based) for plugin communication.
 * Supports hot-reload by watching file timestamps.
 */

#pragma once

#include "../Backend/Entity.hpp"

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

#include "../../CPluginAPI.h"

class Engine;

namespace NativeScript {

// Forward declarations
struct PluginModule;

// A loaded plugin module using new CPluginAPI
struct PluginModule {
    std::string name;
    std::string path;
    void* handle = nullptr;  // dlopen / LoadLibrary handle
    
    // Plugin entry points (new CPluginAPI)
    const CP_PluginEntry* entry = nullptr;
    
    // Hot-reload tracking
    uint64_t lastWriteTime = 0;
    bool loaded = false;
    bool loadFailed = false;
    std::string errorMessage;
};

// Host manages all loaded plugins
//
// Inherits from Entity (like CoreCLRHost) so it can be handed to
// Engine::AddEntity and have Update(dt) driven by the engine's main loop.
// Without this, nothing ticks the loaded plugins.
class NativeScriptHost : public Entity {
public:
    NativeScriptHost();
    ~NativeScriptHost() override;
    
    // Non-copyable, movable
    NativeScriptHost(const NativeScriptHost&) = delete;
    NativeScriptHost& operator=(const NativeScriptHost&) = delete;
    NativeScriptHost(NativeScriptHost&&) = default;
    NativeScriptHost& operator=(NativeScriptHost&&) = default;
    
    // Called each frame to update all loaded plugins.
    // Entity::Update override, so the engine drives this from its main loop.
    void Update(float dt) override;
    
    // Initialize with project path (where plugins/ folder lives)
    bool Initialize(const std::string& projectPath);
    
    // Shutdown all plugins
    void Shutdown();
    
    // Manually load a specific plugin file
    bool LoadPlugin(const std::string& pluginPath);
    
    // Unload a specific plugin
    void UnloadPlugin(const std::string& pluginName);
    
    // Reload a specific plugin (for hot-reload)
    bool ReloadPlugin(const std::string& pluginName);
    
    // Check if any plugin needs hot-reload (file changed)
    void CheckHotReload();
    
    // Get loaded plugins
    const std::vector<std::unique_ptr<PluginModule>>& GetPlugins() const { return plugins; }
    
    // Set engine pointer (called after Engine is created)
    void SetEngine(Engine* engine) { m_engine = engine; }
    
    // Compile all scripts in project's plugins/ folder
    bool CompileScripts(const std::string& projectPath);
    
    // Get last compile error
    const std::string& GetLastError() const { return m_lastError; }
    
private:
    Engine* m_engine = nullptr;
    std::string m_projectPath;
    std::vector<std::unique_ptr<PluginModule>> plugins;
    std::string m_lastError;
    
    // Platform-specific library loading
    void* LoadLibrary(const std::string& path);
    void UnloadLibrary(void* handle);
    void* GetSymbol(void* handle, const char* name);
    uint64_t GetFileWriteTime(const std::string& path);
    
    // Compiler invocation
    bool CompileSinglePlugin(const std::string& sourcePath, const std::string& outputPath);
    std::string GetCompilerCommand(const std::string& sourcePath, const std::string& outputPath);
    std::string FindCompiler();
};

} // namespace NativeScript