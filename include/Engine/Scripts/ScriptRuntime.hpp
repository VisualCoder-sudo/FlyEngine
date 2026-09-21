#pragma once
#include "../Backend/Entity.hpp"
#include "raylib.h"
#include <memory>
#include <string>
#include <vector>

class ScatteredObject;
class ModelGroup;
class Engine;
class CoreCLRHost;

namespace phys {
class Simulation;
}

// ScriptRuntime — the C++ side world context + lifecycle for C# scripting.
//
// Replaces the legacy flyscript::Runtime. It does NOT interpret Flyscript
// source anymore: scripts are C# classes (IScript implementations) compiled
// into the project's Scripts/FlyScript.dll and executed by the hosted
// CoreCLR. This class owns the world bindings (object list, model list,
// camera, physics simulation, engine) that the FlyNative_* C# bridge reads
// and mutates, plus the standalone-script list and play-state tracking.
//
// It is owned by CoreCLRHost, which ticks the C# ScriptHost each frame and
// routes start/stop lifecycle calls into the managed registries. The pointer
// passed to FlyNative_BindRuntime is a ScriptRuntime*; FlyScriptApi's
// ActiveRuntime() casts the bound handle back to this type. Script lifecycle
// start/stop is forwarded to the bound CoreCLRHost (SetHost).
class ScriptRuntime : public Entity {
public:
    ScriptRuntime();
    ~ScriptRuntime() override;

    void Update(float dt) override;

    // A standalone script from the explorer's "Scripts" group. `name` is a
    // unique label; `typeName` is the fully-qualified IScript class name in
    // FlyScript.dll to instantiate when the script runs (e.g. "MyGame.Bouncer").
    struct StandaloneScript {
        std::string name = "Script";
        std::string typeName;
        bool runOnPlay = true;
    };

    // Bind the live world context. Called once after the project scene loads;
    // references (not copies) are kept, so the caller's objects must outlive
    // this runtime.
    void BindWorld(std::vector<ScatteredObject*>& objects,
                   std::vector<std::unique_ptr<ModelGroup>>& models,
                   Camera3D& camera, phys::Simulation& sim, Engine& engine);

    // Bind the host that owns this runtime, so lifecycle calls can reach C#.
    void SetHost(CoreCLRHost* host) { hostPtr = host; }

    // The owning host (may be null when the CLR never loaded).
    CoreCLRHost* GetHost() const { return hostPtr; }

    // ---- world services (read by FlyNative_* / C#) ----
    const std::vector<ScatteredObject*>& GetObjects() const { return *objectsPtr; }
    const std::vector<std::unique_ptr<ModelGroup>>& GetModels() const { return *modelsPtr; }
    Camera3D& GetCamera() { return *cameraPtr; }
    ScatteredObject* FindByName(const std::string& name) const;
    ScatteredObject* CreateObject(const std::string& shapeName);
    bool IsSimPlaying() const { return isPlaying; }
    void SetSimPlaying(bool playing);

    // The per-coroutine "self" object, bound by the C# host right before a
    // script's Run() advances so GameObject.Self resolves to the object the
    // script is attached to. Null for standalone scripts and between frames.
    void SetScriptSelf(ScatteredObject* obj) { scriptSelf = obj; }
    ScatteredObject* GetScriptSelf() const { return scriptSelf; }

    // Parses a console/script assignment "<prop> = <rhs>" onto one object,
    // routing physics-coupled properties to the simulation when playing.
    // Returns false and fills outError on failure.
    bool SetObjectProperty(ScatteredObject* target, const std::string& prop,
                           const std::string& rhs, std::string& outError);

    // ---- physics get/set routed to the simulation ----
    void SetBodyVelocity(ScatteredObject* object, Vector3 velocity);
    void SetBodyAngularVelocity(ScatteredObject* object, Vector3 velocity);
    void SetBodyPosition(ScatteredObject* object, Vector3 position);
    void SetBodyOrientation(ScatteredObject* object, Vector3 eulerDeg);
    void SetPhysicsGravity(float g);
    float GetPhysicsGravity() const;
    void SetPhysicsFriction(float f);
    float GetPhysicsFriction() const;
    void SetPhysicsRestitution(float r);
    float GetPhysicsRestitution() const;

    // ---- standalone script metadata (explorer "Scripts" group + persistence) ----
    std::vector<StandaloneScript>& StandaloneScripts() { return standaloneScripts; }
    const std::vector<StandaloneScript>& StandaloneScripts() const { return standaloneScripts; }
    void RemoveStandalone(int index);

    // ---- per-object script lifecycle (forwarded to the C# host) ----
    // `obj->script` holds the IScript type name for that object.
    void StartObjectScript(ScatteredObject* obj);
    void StopObjectScript(ScatteredObject* obj);
    void StartStandaloneScript(int index);
    void StopStandaloneScript(int index);
    void StopAllScripts();

    // Host hooks: called when play starts/stops so runOnPlay scripts start and
    // play-time object creation is rolled back.
    void OnPlayStarted();
    void OnPlayStopped();
    void LogCreatedObject(ScatteredObject* obj);
    void RollbackCreatedObjects();

private:
    phys::Simulation* sim = nullptr;
    Engine* engine = nullptr;
    CoreCLRHost* hostPtr = nullptr;
    std::vector<ScatteredObject*>* objectsPtr = nullptr;
    std::vector<std::unique_ptr<ModelGroup>>* modelsPtr = nullptr;
    Camera3D* cameraPtr = nullptr;

    std::vector<StandaloneScript> standaloneScripts;
    std::vector<ScatteredObject*> playCreatedObjects;
    bool isPlaying = false;
    ScatteredObject* scriptSelf = nullptr;
};

// Global play-state hook: phys::Simulation calls it (with the new play state)
// whenever the user toggles play/stop, so the active CoreCLRHost can start/stop
// runOnPlay scripts in the managed host. CoreCLRHost registers itself here.
using PlayStateHook = void(*)(bool playing);
void SetPlayStateHook(PlayStateHook hook);
PlayStateHook GetPlayStateHook();

// Module-level access to the active runtime, set by CoreCLRHost at construction
// and used by editor code (ui, scene persistence, command console) to reach the
// standalone-script list and world services without coupling to CoreCLRHost.
void SetActiveRuntime(ScriptRuntime* rt);
ScriptRuntime* GetActiveRuntime();