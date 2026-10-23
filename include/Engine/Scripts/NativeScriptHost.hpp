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

#include <set>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <future>

#include "../Plugin/CPluginAPI.h"
extern "C" {
#include "../Plugin/PluginManifest.h"
}
#include "FlyScriptABI.h"
#include "ScriptCompiler.hpp"

class Engine;
class ScatteredObject;
class ModelGroup;
class ScriptRuntime;
struct Camera3D;
namespace phys { class Simulation; }
namespace pluginhost { enum class SceneEvent; }

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

    // Plugin Manager. A disabled plugin keeps its record (so the panel can list it and turn it back on) but has no
    // library loaded. The metadata comes from the plugin folder's id.json when there is one.
    bool enabled = true;
    std::string dir;           // <project>/plugins/<type>/<name>
    std::string type;          // "nat" or "lib"
    std::string displayName, version, publisher, description, category, declaredApi;
    bool hasIdentity = false;
    bool global = false;       // lives in the user's global plugins folder (every project) rather than <project>/plugins
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

    // Gives plugins their viewport overlay pass (editor, outside Play).
    void DrawOverlay3D() override;

    // ---- plugins ----

    // Initialize with project path (where plugins/ folder lives) and load plugins.
    bool Initialize(const std::string& projectPath);

    // Shutdown all plugins and scripts
    void Shutdown();

    bool LoadPlugin(const std::string& pluginPath, bool global = false);
    void UnloadPlugin(const std::string& pluginName);
    bool ReloadPlugin(const std::string& pluginName);
    void CheckHotReload();

    // Plugin Manager: turn a plugin off/on without deleting it (remembered in <project>/plugins/disabled.txt), and
    // pick up plugin folders added since startup. Both are no-ops for an exported game's built-in plugins.
    bool SetPluginEnabled(const std::string& pluginName, bool enabled);
    // Copies a plugin into <project>/plugins/<type>/<name>/ (or, with `global`, into the user's global plugins folder, so
    // every project loads it) and loads it. `source` is a plugin folder (the one holding id.json and build/) or a bare
    // library file. Never overwrites an installed plugin.
    struct ImportResult { bool ok = false; std::string message; };
    ImportResult ImportPlugin(const std::string& source, bool global = false);
    // Unloads a plugin and deletes its folder (local or global). Refuses anything outside the plugins folders.
    ImportResult RemovePlugin(const std::string& pluginName);
    // Moves a plugin between this project's plugins and the global ones. A plugin of that name must not exist on the
    // other side.
    ImportResult MovePlugin(const std::string& pluginName, bool toGlobal);
    int  RescanPlugins();   // returns how many new plugins were found
    const std::string& GetProjectPath() const { return m_projectPath; }

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
    bool ScriptsReady() const { return m_module != nullptr; }   // a loaded library, or scripts built into the executable

    // Rebuild Scripts/*.cpp in the background and reload on success.
    void RequestScriptRebuild();

    // Watch Scripts/ and rebuild on change (default on).
    void SetScriptHotReload(bool enabled) { m_hotReload = enabled; }

    // The world context FlyNative_* and the editor UI read and mutate.
    ScriptRuntime* GetRuntime() { return m_runtime.get(); }
    void BindProjectFolder(const std::string& projectPath);   // tells the runtime where the project is (Initialize* do it)
    void BindWorld(std::vector<ScatteredObject*>& objects,
                   std::vector<std::unique_ptr<ModelGroup>>& models,
                   Camera3D& camera, phys::Simulation& sim, Engine& engine);

    // Names of every FLY_SCRIPT class in the loaded library.
    std::vector<std::string> GetScriptClassNames() const;

    // Adds every script class not yet in the standalone list, so a script
    // dropped into Scripts/ shows up in the explorer and runs on Play.
    void SyncStandaloneScripts();
    // A script was deleted: keep its (still compiled) class from being listed again, and rebuild without it.
    void ForgetScriptClass(const std::string& className);

    // Lifecycle, called through ScriptRuntime by the editor and FlyNative_*.
    bool StartObjectScriptOn(ScatteredObject* obj);
    void StopObjectScriptOn(ScatteredObject* obj);
    bool IsObjectScriptRunning(ScatteredObject* obj) const;
    // Visual Script Code Snippet nodes: starts the generated class `className` on its own (no object, no explorer
    // slot). Returns false when the library has no such class (not compiled yet).
    bool StartSnippetScript(const std::string& className);
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
        bool stopPending = false; // asked to stop while its own tick was running: destroyed once that tick returns
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
    void* m_executing = nullptr; // the instance whose tick is running right now
    std::vector<std::string> m_hiddenClasses;
    bool m_pendingPlay = false;
    bool m_pendingStop = false;

    // Hot reload
    bool m_hotReload = true;
    uint64_t m_sourceStamp = 0;
    float m_watchTimer = 0.0f;
    std::future<scriptCompiler::Result> m_build;
    bool m_rebuildQueued = false;

    bool m_builtIn = false;   // an exported game: plugins are linked in, nothing on disk to scan
    std::set<std::string> m_disabledPlugins;   // local; the global list lives in <global root>/disabled.txt
    std::set<std::string> m_disabledGlobal;
    void LoadDisabledList();
    void SaveDisabledList(bool global) const;
    std::string RootFor(bool global) const;
    int  ScanPluginDirs();
    void ReadPluginIdentity(PluginModule& plugin) const;
    void UnloadModule(PluginModule& plugin);

    void DispatchSceneEvent(pluginhost::SceneEvent e);
    void TickScripts(float dt);
    void WatchScriptSources(float dt);
    void StartBuild();
    void FinishBuild();
    bool LoadScriptLibrary(const std::string& path);
    bool LoadStaticScripts();                                   // scripts linked into the executable (exported game)
    bool LoadStaticPlugin(const char* name, CP_PluginEntry* (*getEntry)(void));
    void UnloadScriptLibrary();
    const FlyScriptClass* FindClass(const std::string& name) const;
    bool StartScript(const std::string& className, ScatteredObject* obj, int standaloneIndex, bool replace = true);
    void DestroyRunning(RunningScript& rs);
    bool ObjectAlive(ScatteredObject* obj) const;

    // Platform-specific library loading
    void* LoadLibrary(const std::string& path);
    void UnloadLibrary(void* handle);
    void* GetSymbol(void* handle, const char* name);
    uint64_t GetFileWriteTime(const std::string& path);
};

} // namespace NativeScript
