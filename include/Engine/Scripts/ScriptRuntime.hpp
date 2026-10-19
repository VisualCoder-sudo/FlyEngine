#pragma once
#include "../Backend/Entity.hpp"
#include "raylib.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

class ScatteredObject;
class ModelGroup;
class Engine;
namespace NativeScript { class NativeScriptHost; }

namespace phys {
class Simulation;
}

// ScriptRuntime - the world context + lifecycle bookkeeping for game scripts.
//
// Scripts are C++ classes (fly::Script, registered with FLY_SCRIPT) compiled
// from the project's Scripts/*.cpp into a shared library that
// NativeScriptHost loads and ticks. This class owns the world bindings
// (object list, model list, camera, physics simulation, engine) that the
// FlyNative_* API reads and mutates, plus the standalone-script list and
// play-state tracking.
//
// It is owned by NativeScriptHost. FlyScriptApi's ActiveRuntime() resolves
// to it; script lifecycle start/stop is forwarded to the host (SetHost).
class ScriptRuntime : public Entity {
public:
    ScriptRuntime();
    ~ScriptRuntime() override;

    void Update(float dt) override;

    // A standalone script from the explorer's "Scripts" group. `name` is a
    // unique label; `typeName` is the FLY_SCRIPT class to instantiate when the
    // script runs (e.g. "Bouncer").
    struct StandaloneScript {
        std::string name = "Script";
        std::string typeName;
        bool runOnPlay = true;
    };

    // Plugin entry (forward declared from CPluginAPI.h)
    struct FlyPluginEntry;

    // Magic number for validating active runtime
    static constexpr uint32_t RUNTIME_MAGIC = 0x53435254; // 'SCRT'
    uint32_t runtimeMagic = 0; // Set to RUNTIME_MAGIC in constructor when fully initialized

    // Validation helper
    bool IsValid() const;

    // Bind the live world context. Called once after the project scene loads;
    // references (not copies) are kept, so the caller's objects must outlive
    // this runtime.
    void BindWorld(std::vector<ScatteredObject*>& objects,
                   std::vector<std::unique_ptr<ModelGroup>>& models,
                   Camera3D& camera, phys::Simulation& sim, Engine& engine);

    // Bind the host that owns this runtime, so lifecycle calls reach it.
    void SetHost(NativeScript::NativeScriptHost* host) { hostPtr = host; }

    // The owning host.
    NativeScript::NativeScriptHost* GetHost() const { return hostPtr; }

    // Project info (set by editor/player on initialization)
    void SetProjectPath(const std::string& path) { m_projectPath = path; }
    void SetProjectName(const std::string& name) { m_projectName = name; }
    const std::string& GetProjectPath() const { return m_projectPath; }
    const std::string& GetProjectName() const { return m_projectName; }

    // ---- world services (read by FlyNative_*) ----
    bool HasWorld() const { return objectsPtr != nullptr; }
    const std::vector<ScatteredObject*>& GetObjects() const { return *objectsPtr; }
    const std::vector<std::unique_ptr<ModelGroup>>& GetModels() const { return *modelsPtr; }
    Camera3D& GetCamera() { return *cameraPtr; }
    ScatteredObject* FindByName(const std::string& name) const;
    // The `occurrence`-th (1-based) object with that name, in scene order. Lets same-named objects be told apart.
    ScatteredObject* FindByNameNth(const std::string& name, int occurrence) const;
    // With `deferSpawn` the object is created but gets no physics body until FinishCreate(): a Visual Script's Setup
    // chain can change it first, so it appears already configured.
    ScatteredObject* CreateObject(const std::string& shapeName, bool deferSpawn = false);
    bool IsSimPlaying() const { return isPlaying; }
    phys::Simulation* GetSimulation() const { return sim; }
    void SetSimPlaying(bool playing);

    // The per-coroutine "self" object, bound by the host right before a
    // script's Run() advances so fly::Object::Self() resolves to the object
    // the script is attached to. Null for standalone scripts and between frames.
    void SetScriptSelf(ScatteredObject* obj) { scriptSelf = obj; }
    ScatteredObject* GetScriptSelf() const { return scriptSelf; }

    // Parses a console/script assignment "<prop> = <rhs>" onto one object,
    // routing physics-coupled properties to the simulation when playing.
    // Returns false and fills outError on failure.
    bool SetObjectProperty(ScatteredObject* target, const std::string& prop,
                           const std::string& rhs, std::string& outError);
    // Reads a property in the same console syntax SetObjectProperty accepts: "(x, y, z)", "(r, g, b)", "true", "1.5".
    bool GetObjectProperty(ScatteredObject* target, const std::string& prop, std::string& out);

    // Visual-script object actions. Vectors use console syntax "(x, y, z)"; failures fill outError.
    void DestroyObject(ScatteredObject* obj);                 // scene list, physics body and engine entity
    ScatteredObject* CloneObject(ScatteredObject* source, bool deferSpawn = false);   // copy added to the scene; null if impossible
    void FinishCreate(ScatteredObject* obj);                  // gives an object made with deferSpawn its physics body
    // Adds a prefab (.flyprefab, project-relative or absolute) to the scene with its pivot at `at`. While playing the
    // objects get physics bodies and their scripts start, and they are taken out again when play stops. Returns the
    // prefab's first object (null if the file is missing or not a prefab); `outObjects` gets all of them.
    ScatteredObject* SpawnPrefab(const std::string& file, Vector3 at, std::vector<ScatteredObject*>* outObjects = nullptr);
    bool AddForce(ScatteredObject* obj, const std::string& vec, bool impulse, std::string& outError);
    // Name of the first object a ray hits ("" for none). Needs the simulation.
    std::string RayCastName(const std::string& origin, const std::string& dir, float maxDistance, std::string& outError);

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

    // ---- per-object script lifecycle (forwarded to the host) ----
    // `obj->script` holds the FLY_SCRIPT class name for that object.
    void StartObjectScript(ScatteredObject* obj);
    // Starts a Visual Script Code Snippet's generated class. False if it isn't compiled into the loaded library.
    bool StartSnippet(const std::string& className);
    // Values wired into / out of a Code Snippet node, keyed by its generated class name. Inputs are what the node's
    // pins held when the flow reached it; outputs are the latest value the snippet set (empty until it does).
    void SetSnippetInputs(const std::string& className, std::vector<std::string> values);
    std::string SnippetInput(const std::string& className, int pin) const;
    void SetSnippetOutput(const std::string& className, int pin, const std::string& value);
    std::string SnippetOutput(const std::string& className, int pin) const;
    void ClearSnippetValues();
    void StopObjectScript(ScatteredObject* obj);
    void StartStandaloneScript(int index);
    void StopStandaloneScript(int index);
    void StopAllScripts();

    // Host hooks: called when play starts/stops so runOnPlay scripts start and
    // play-time object creation is rolled back.
    void OnPlayStarted();
    void OnPlayStopped();
    void LogCreatedObject(ScatteredObject* obj);
    // Deletes the objects scripts created during play (scene list, physics and
    // the engine's entity), for hosts with no scene snapshot to restore.
    void RollbackCreatedObjects();
    // Drops the play-created list without touching the objects: called when the
    // editor restores its pre-play scene snapshot, which frees them itself.
    void ForgetCreatedObjects() { playCreatedObjects.clear(); playCreatedGroups.clear(); }

    // Plugin registration (for native C/C++ plugins)
    void RegisterPluginEntry(const FlyPluginEntry* entry) { (void)entry; /* TODO: store for multi-plugin support */ }

private:
    phys::Simulation* sim = nullptr;
    Engine* engine = nullptr;
    NativeScript::NativeScriptHost* hostPtr = nullptr;
    std::vector<ScatteredObject*>* objectsPtr = nullptr;
    std::vector<std::unique_ptr<ModelGroup>>* modelsPtr = nullptr;
    Camera3D* cameraPtr = nullptr;

    struct SnippetValues { std::vector<std::string> in, out; };
    std::map<std::string, SnippetValues> snippetValues;

    std::vector<StandaloneScript> standaloneScripts;
    std::vector<ScatteredObject*> playCreatedObjects;
    std::vector<ModelGroup*> playCreatedGroups;   // the groups prefabs spawned during play made

    // World settings (game.Lighting/Rendering/Camera/Physics) as they were when
    // play started, put back when it stops.
    struct WorldSettings {
        bool valid = false;
        short shadows = 0, grid = 0, wireframe = 0;
        int shadowQuality = 0;
        float ambient = 0, fov = 0, gravity = 0, friction = 0, restitution = 0;
    } savedWorld;
    void CaptureWorldSettings();
    void RestoreWorldSettings();
    bool isPlaying = false;
    ScatteredObject* scriptSelf = nullptr;

    // Project info
    std::string m_projectPath;
    std::string m_projectName;
};

// Global play-state hook: phys::Simulation calls it (with the new play state)
// whenever the user toggles play/stop, so the active NativeScriptHost can
// start/stop runOnPlay scripts. NativeScriptHost registers itself here.
// A playtest never keeps what it changed. The editor takes a snapshot of the scene (PrePlay: before any script, visual
// or C++, has run) and puts it back (PostPlay: after every script has stopped, On Stop chains included).
using PlayEdgeHook = std::function<void()>;
void SetPrePlayHook(PlayEdgeHook hook);
void SetPostPlayHook(PlayEdgeHook hook);

using PlayStateHook = void(*)(bool playing);
void SetPlayStateHook(PlayStateHook hook);
PlayStateHook GetPlayStateHook();

// Module-level access to the active runtime, set by NativeScriptHost at construction
// and used by editor code (ui, scene persistence, command console) to reach the
// standalone-script list and world services without coupling to the host.
void SetActiveRuntime(ScriptRuntime* rt);
ScriptRuntime* GetActiveRuntime();