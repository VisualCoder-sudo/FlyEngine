#pragma once

#include "../Backend/Entity.hpp"

#include <memory>
#include <string>
#include <vector>

class ScatteredObject;
class ModelGroup;
class Engine;
struct Camera3D;
namespace phys { class Simulation; }
class ScriptRuntime;

// CoreCLRHost - embeds the .NET CoreCLR runtime inside the Flyengine process
// and drives C# script coroutines from the engine's Update loop.
//
// Inherits from Entity so it can be added to the Engine and get Update(dt)
// called automatically each frame.
//
// Usage:
//   - auto host = std::make_unique<CoreCLRHost>();
//   - host->Initialize(projectPath);
//   - engine.AddEntity(std::move(host));
//   - // host->Update(dt) called automatically by Engine::Run()
//
// The host loads the C# scripts assembly (FlyScript.dll) from the project's
// Scripts/ folder, creates delegates for script entry points, and runs them.

class CoreCLRHost : public Entity
{
public:
    // CoreCLR function pointers (loaded from coreclr.dll).
    using coreclr_initialize_fn      = int (*)(const char*, const char*, int, const char* const*, const char* const*, void**, unsigned int*);
    using coreclr_create_delegate_fn = int (*)(void*, unsigned int, const char*, const char*, const char*, void**);
    using coreclr_shutdown_fn        = int (*)(void*);

    // C# delegate signatures.
    using ScriptRunDelegate = void(*)(void* host, float dt, int scriptId); // ticks all script coroutines for one frame
    using VoidScriptDelegate = void(*)(void* host);                         // arg-free lifecycle hook into C#
    using ObjectScriptDelegate = void(*)(void* host, unsigned long long handle, const char* typeName); // per-object lifecycle hook
    using StandaloneScriptDelegate = void(*)(void* host, int index, const char* typeName); // index-based lifecycle hook
    using ScriptTypesDelegate = int (*)(void* host, void* buffer, int capacity);          // enumerate IScript type names into a UTF-8 buffer
    using ConsoleExecuteDelegate = int (*)(void* host, void* code, void* buffer, int capacity); // evaluate C# console input; returns 1 on success, 0 on error

    CoreCLRHost();
    ~CoreCLRHost() override;

    // Non-copyable (owns runtime state).
    CoreCLRHost(const CoreCLRHost&) = delete;
    CoreCLRHost& operator=(const CoreCLRHost&) = delete;
    CoreCLRHost(CoreCLRHost&&) = default;
    CoreCLRHost& operator=(CoreCLRHost&&) = default;

    // Initialize the CoreCLR runtime and load the scripts assembly.
    // Returns true on success.
    bool Initialize(const std::string& projectPath);

    // Entity::Update override - drives all active script coroutines for this frame.
    void Update(float dt) override;

    // Shutdown and release CoreCLR.
    void Shutdown();

    // Check if the runtime is initialized and ready.
    bool IsReady() const;

    // Get the last error message.
    const std::string& GetError() const;

    // Bind a script assembly (FlyScript.dll) for hot-reload.
    // Returns true if the assembly was (re)loaded.
    bool LoadScriptsAssembly(const std::string& assemblyPath);

    // The world-facing runtime context for C# scripting. The host owns it and
    // binds it to the managed host each frame; the editor and FlyNative_* use
    // it to read/mutate the world and list standalone scripts.
    class ScriptRuntime* GetRuntime() { return m_script_runtime.get(); }
    void BindWorld(std::vector<ScatteredObject*>& objects,
                   std::vector<std::unique_ptr<ModelGroup>>& models,
                   Camera3D& camera, phys::Simulation& sim, Engine& engine);

    // C++ -> C# script lifecycle. These thunk to the managed PerObjectScripts /
    // StandaloneScripts registries resolved during Initialize().
    void StartObjectScriptOn(ScatteredObject* obj);
    void StopObjectScriptOn(ScatteredObject* obj);
    void StartStandaloneScriptAt(int index);
    void StopStandaloneScriptAt(int index);
    void ClearAllScripts();

    // Seed the standalone-script list with every IScript type in the compiled
    // assembly (skipping ones already listed), so scripts dropped into the
    // project's Scripts/ folder appear in the explorer and run on Play.
    void SyncStandaloneScripts();

    // Called by the editor on the play/stop toggle to keep the runtime's play
    // state and the physics simulation in sync.
    void SetPlayActive(bool active);

    // Evaluates a line of C# in the hosted CLR using Roslyn scripting. The
    // result (or error) is written to `outText`; `outError` is set when the
    // snippet failed to compile/run. Returns false if the host is not ready.
    bool ExecuteConsoleCode(const std::string& code, std::string& outText, bool& outError);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    std::unique_ptr<ScriptRuntime> m_script_runtime;
};