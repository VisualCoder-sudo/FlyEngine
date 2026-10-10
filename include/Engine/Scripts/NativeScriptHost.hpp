/*
 * NativeScriptHost.hpp - Loads and manages native C/C++ code (.so/.dll):
 *
 *  - Game scripts: the project's Scripts/*.cpp, compiled by ScriptCompiler
 *    into one library and driven through the C ABI in
 *    ScriptingSDK/include/FlyScriptABI.h. Each script is a C++20 coroutine
 *    attached to a scene object or run standalone; the host resumes them
 *    once per frame and rebuilds/reloads the library when a source changes.
 *  - Plugins: prebuilt libraries under plugins/{nat,lib}/ using CPluginAPI.
 *
 * Both hot-reload by watching file timestamps.
 */

#pragma once

#include "../Backend/Entity.hpp"

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <future>

#include "../../CPluginAPI.h"
#include "FlyScriptABI.h"
#include "ScriptCompiler.hpp"

class Engine;
class ScatteredObject;
class ModelGroup;
class ScriptRuntime;
struct Camera3D;
namespace phys { class Simulation; }

namespace NativeScript {

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

// Host manages all loaded plugins and the project's game scripts.
//
// Inherits from Entity so it can be handed to Engine::AddEntity and have
// Update(dt) driven by the engine's main loop.
class NativeScriptHost : public Entity {
public:
    NativeScriptHost();
    ~NativeScriptHost() override;

    NativeScriptHost(const NativeScriptHost&) = delete;
    NativeScriptHost& operator=(const NativeScriptHost&) = delete;

    // Called each frame: hot-reload checks, play/stop edges, plugin updates
    // and one step of every running script.
    void Update(float dt) override;

    // ---- plugins ----

    // Initialize with project path (where plugins/ folder lives) and load plugins.
    bool Initialize(const std::string& projectPath);

    // Shutdown all plugins and scripts
    void Shutdown();

    bool LoadPlugin(const std::string& pluginPath);
    void UnloadPlugin(const std::string& pluginName);
    bool ReloadPlugin(const std::string& pluginName);
    void CheckHotReload();

    const std::vector<std::unique_ptr<PluginModule>>& GetPlugins() const { return plugins; }

    // Set engine pointer (called after Engine is created)
    void SetEngine(Engine* engine) { m_engine = engine; }

    // Ensure plugins/{nat,fpc,lib} exist.
    bool CompileScripts(const std::string& projectPath);

    const std::string& GetLastError() const { return m_lastError; }

    // ---- game scripts ----

    // Builds the project's Scripts/*.cpp (blocking) and loads the result.
    // Falls back to the last successful build when compiling fails. Call
    // before the scene loads; it does not need the world.
    bool InitializeScripts(const std::string& projectPath);

    // True when a script library is loaded.
    bool ScriptsReady() const { return m_scriptLib != nullptr; }

    // Rebuild Scripts/*.cpp in the background and reload on success.
    void RequestScriptRebuild();

    // Watch Scripts/ and rebuild on change (default on).
    void SetScriptHotReload(bool enabled) { m_hotReload = enabled; }

    // The world context FlyNative_* and the editor UI read and mutate.
    ScriptRuntime* GetRuntime() { return m_runtime.get(); }
    void BindWorld(std::vector<ScatteredObject*>& objects,
                   std::vector<std::unique_ptr<ModelGroup>>& models,
                   Camera3D& camera, phys::Simulation& sim, Engine& engine);

    // Names of every FLY_SCRIPT class in the loaded library.
    std::vector<std::string> GetScriptClassNames() const;

    // Adds every script class not yet in the standalone list, so a script
    // dropped into Scripts/ shows up in the explorer and runs on Play.
    void SyncStandaloneScripts();

    // Lifecycle, called through ScriptRuntime by the editor and FlyNative_*.
    bool StartObjectScriptOn(ScatteredObject* obj);
    void StopObjectScriptOn(ScatteredObject* obj);
    bool IsObjectScriptRunning(ScatteredObject* obj) const;
    bool StartStandaloneScriptAt(int index);
    void StopStandaloneScriptAt(int index);
    bool IsStandaloneScriptRunning(int index) const;
    // Standalone script `index` was erased from the list: stop it and shift
    // the indices of the ones after it.
    void OnStandaloneRemoved(int index);
    void ClearAllScripts();

    // Play/stop toggle from the simulation (applied on the next Update).
    void SetPlayActive(bool active);

private:
    struct RunningScript {
        const FlyScriptClass* cls = nullptr;
        void* instance = nullptr;
        ScatteredObject* object = nullptr; // null for standalone scripts
        int standaloneIndex = -1;
        std::string className;
    };

    Engine* m_engine = nullptr;
    std::string m_projectPath;
    std::vector<std::unique_ptr<PluginModule>> plugins;
    std::string m_lastError;

    // Scripts
    std::unique_ptr<ScriptRuntime> m_runtime;
    FlyApi m_api{};
    void* m_scriptLib = nullptr;
    std::string m_scriptLibPath;
    const FlyScriptModule* m_module = nullptr;
    std::vector<RunningScript> m_running;
    bool m_ticking = false;  // inside the per-frame tick loop
    bool m_pendingPlay = false;
    bool m_pendingStop = false;

    // Hot reload
    bool m_hotReload = true;
    uint64_t m_sourceStamp = 0;
    float m_watchTimer = 0.0f;
    std::future<scriptCompiler::Result> m_build;
    bool m_rebuildQueued = false;

    void TickScripts(float dt);
    void WatchScriptSources(float dt);
    void StartBuild();
    void FinishBuild();
    bool LoadScriptLibrary(const std::string& path);
    void UnloadScriptLibrary();
    const FlyScriptClass* FindClass(const std::string& name) const;
    bool StartScript(const std::string& className, ScatteredObject* obj, int standaloneIndex);
    void DestroyRunning(RunningScript& rs);
    bool ObjectAlive(ScatteredObject* obj) const;

    // Platform-specific library loading
    void* LoadLibrary(const std::string& path);
    void UnloadLibrary(void* handle);
    void* GetSymbol(void* handle, const char* name);
    uint64_t GetFileWriteTime(const std::string& path);
};

} // namespace NativeScript
