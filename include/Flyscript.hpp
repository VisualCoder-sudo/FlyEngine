#pragma once
#include "Entity.hpp"
#include "ScatteredObject.hpp"
#include "raylib.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Engine;

namespace phys {
class Simulation;
}

// Flyscript: a small imperative scripting language on top of the command-bar
// property syntax.
//
//   -- comment
//   var n = 2 + 2               -- script-local variable (arithmetic: + - * /)
//   var p = (2 - 2) * 3         -- parenthesized expressions
//   var Color = rgb(211, 149, 19, 255, 234)   -- color variable (see rgb below)
//   global rate = game.Lighting.Ambient.value   -- variable shared by all scripts
//   var quality = game.Lighting.ShadowQuality.value   -- read a property value
//   var ball = game.Workspace.Ball              -- object reference
//   ball.Position.value = (0, 3, 0)             -- use the reference like a path
//   n = n + 1                   -- reassign an existing variable
//   print(n)
//   self.Color.value = rgb(211, 149, 19, 255, 234)   -- set a color (r, g, b 0..255,
//                                                    -- optional sat & brightness 0..255)
//   self.Position.value = (0, 3, 0)
//   game.Workspace.Ball.Velocity.value = (0, 10, 0)
//   game.Lighting.GlobalShadows.value = false
//   for every instance { self.Size.value = (1, 1, 1) }
//   for 5 { tick.wait(1) game.Rendering.Grid.value = true }
//   while true { tick.wait(0) game.Lighting.Ambient.value = 0.5 }
//   while not game.Lighting.GlobalShadows.value { tick.wait(0) }
//   if game.Lighting.GlobalShadows.value == true { game.Rendering.Grid.value = false }
//   if not flag == false { print("hi") } else { print("bye") }
//   if lever1 == true and lever2 == true { print("both") }
//   if num == 6 or num == 3 { print("six or three") }
//
// Values are dynamically typed: number, boolean, string, (x, y, z) tuple,
// color (rgb(r, g, b) or rgb(r, g, b, sat, bright), all 0..255), or an object
// reference. `var` is local to the running script; `global` is shared by every
// script. Assigning to an undeclared name is an error.
//
// Conditions (if/while) must be true or false. `not`, `and` and `or` only
// accept booleans, and `==`/`!=` compare two values of the same type.
//
// Scripts are suspended/resumed by the Runtime scheduler, so tick.wait()
// never blocks the app. `for every instance` runs its body once per scene
// object (with `self` bound to each one).
namespace flyscript {

struct Script {
    std::string name = "Script";
    std::string source;
    bool runOnPlay = true;
};

// Runtime value of a Flyscript expression.
struct Value {
    enum class Type { Number, Bool, String, Tuple, Color, Object, Enum };
    Type type = Type::Number;
    float number = 0.0f;
    bool boolean = false;
    std::string str;
    Vector3 tuple{};
    Color color{};
    ScatteredObject* object = nullptr;
    std::string enumName; // "CollisionAccuracy", etc.
    int ordinal = 0;      // index into the enum's members
};

// Shared command executor used by both the command console and Flyscripts.
// `path`/`property`/`rhs` follow the console form ("game.Lighting.Ambient",
// "value", "1.0") plus object forms "self.Position"/"game.Workspace.Name.X"
// with property "Position"/"Size"/"Rotation"/"Color"/"Velocity".
// Returns false and fills outError on failure.
bool ExecuteAssignment(const std::string& path, const std::string& property,
                       const std::string& rhs, ScatteredObject* self,
                       class Runtime& rt, std::string& outError);

// Owns the scene objects' coroutines and the standalone script list.
// Added to the Engine AFTER phys::Simulation so it sees play-state changes.
class Runtime : public Entity {
public:
    Runtime(std::vector<ScatteredObject*>& objects,
            std::vector<std::unique_ptr<ModelGroup>>& models,
            Camera3D& camera, phys::Simulation& sim, Engine& engine);
    ~Runtime() override;
    void Update(float dt) override;

    // Standalone scripts (the explorer "Scripts" group). Per-object scripts
    // live on ScatteredObject::script.
    std::vector<Script> scripts;

    // Variables declared with `global name = ...`; shared by every script.
    std::unordered_map<std::string, Value> globalVars;

    // ---- per-object script control (target = a scene object) ----
    bool StartScript(ScatteredObject* self, const std::string& source,
                     int& outErrLine, std::string& outErrMsg);
    void StopScript(ScatteredObject* self);
    bool IsRunning(ScatteredObject* self) const;
    int GetErrorLine(ScatteredObject* self) const;
    const std::string& GetErrorMessage(ScatteredObject* self) const;

    // ---- standalone script control (target = index into scripts) ----
    bool StartScript(int index, int& outErrLine, std::string& outErrMsg);
    void StopScript(int index);
    bool IsRunning(int index) const;
    int GetErrorLine(int index) const;
    const std::string& GetErrorMessage(int index) const;

    // Stops any coroutine for the script, removes it from `scripts`, and
    // reindexes coroutines of scripts that followed it.
    void RemoveScript(int index);

    void StopAll();

    ScatteredObject* FindByName(const std::string& name) const;
    const std::vector<std::unique_ptr<ModelGroup>>& GetModels() const { return models; }
    void SetBodyVelocity(ScatteredObject* object, Vector3 velocity);
    void SetBodyAngularVelocity(ScatteredObject* object, Vector3 velocity);
    void SetBodyPosition(ScatteredObject* object, Vector3 position);
    void SetBodyOrientation(ScatteredObject* object, Vector3 eulerDeg);
    ScatteredObject* CreateObject(const std::string& shapeName);
    void LogPropertyChange(ScatteredObject* obj, const std::string& prop,
                           const std::string& newValue);
    bool IsSimPlaying() const { return isPlaying; }
    Camera3D& GetCamera() { return camera; }

    // Configurable physics settings (game.Physics.*) routed to the simulation.
    void SetPhysicsGravity(float g);
    float GetPhysicsGravity() const;
    void SetPhysicsFriction(float f);
    float GetPhysicsFriction() const;
    void SetPhysicsRestitution(float r);
    float GetPhysicsRestitution() const;

    const std::vector<ScatteredObject*>& GetObjects() const { return objects; }

private:
    struct Coroutine;

    void OnPlayStarted();
    void OnPlayStopped();
    void StepCoroutines();
    void RunCoroutine(Coroutine& c, double now, int budget);

    // Sandbox: while any script coroutine is running, its property/lighting
    // changes write straight to the world (so the effect is visible), but the
    // original state is captured up front and restored when the last script
    // stops. This keeps scripts from permanently mutating the editor scene.
    struct SnapshotObj {
        ScatteredObject* obj;
        Vector3 pos, size, rot;
        Vector3 velocity, angularVelocity;
        Color color;
        bool anchored;
        float mass;
        bool canCollide;
        pcoll::CollisionAccuracy accuracy;
    };
    struct Snapshot {
        bool shadows = true;
        bool grid = true;
        bool wireframe = false;
        float ambient = 1.0f;
        int shadowQuality = 50;
        float fov = 45.0f;
        float physicsGravity = -19.62f;
        float physicsFriction = 0.4f;
        float physicsRestitution = 0.7f;
        std::vector<SnapshotObj> objects;
    };
    bool sandboxActive = false;
    Snapshot snapshot;
    void BeginSandbox();
    void EndSandbox();
    void EndSandboxIfIdle();

    // Play-mode change tracking: objects created and properties modified during
    // play are logged here and reversed when play stops.
    struct PlayChange {
        ScatteredObject* obj;
        std::string prop;
        std::string oldValue;
    };
    std::vector<ScatteredObject*> playCreatedObjects;
    std::vector<PlayChange> playChanges;
    void RollbackPlayChanges();

    std::vector<ScatteredObject*>& objects;
    std::vector<std::unique_ptr<ModelGroup>>& models;
    Camera3D& camera;
    phys::Simulation& sim;
    Engine& engine;
    bool wasPlaying = false;
    bool isPlaying = false;

    std::vector<std::unique_ptr<Coroutine>> coroutines;
};

Runtime& GetRuntime();
Runtime* GetRuntimePtr();
bool IsRuntimeReady();
void SetRuntime(Runtime* runtime);

// Autocomplete suggestions for the command/script vocabulary. `input` is the
// text typed so far up to the caret (may contain '='). Shared by the command
// console and the script editor so they offer identical completions.
// `scriptSource` is the full source of the script being edited: its declared
// `var`/`global` variables (and object bindings) are added as completions.
// Pass an empty string (the default, used by the command console) to get only
// the built-in vocabulary.
std::vector<std::string> GetCompletions(const std::string& input,
                                        const std::string& scriptSource = std::string());

// Byte offset (within `textUpToCaret`) at which the identifier currently being
// completed starts. Used by the accept-key handlers to replace exactly the
// typed word with the chosen suggestion. Kept consistent with GetCompletions.
size_t CompletionWordStart(const std::string& textUpToCaret);

struct ScriptWarning {
    std::string scriptName;
    int line;
    std::string description;
};

bool HasIgnoreFlag(const std::string& source);
std::vector<ScriptWarning> CheckScriptSafety(const std::string& scriptName,
                                             const std::string& source);

} // namespace flyscript
