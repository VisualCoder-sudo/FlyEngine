// ScriptRuntime.cpp — C++ world context + lifecycle backing the C# script host.

#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "../../../include/Engine/Scripts/CoreCLRHost.hpp"
#include "../../../include/Engine.hpp"
#include "../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../../../include/Engine/PhysicsCollision.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace {
PlayStateHook g_playStateHook = nullptr;
ScriptRuntime* g_activeRuntime = nullptr;

bool ParseBool(const std::string& s, bool& out) {
    if (s == "true" || s == "1") { out = true; return true; }
    if (s == "false" || s == "0") { out = false; return true; }
    return false;
}

bool ParseVec3(const std::string& s, Vector3& out) {
    std::string t = s;
    t.erase(std::remove(t.begin(), t.end(), '('), t.end());
    t.erase(std::remove(t.begin(), t.end(), ')'), t.end());
    std::replace(t.begin(), t.end(), ',', ' ');
    std::istringstream iss(t);
    float v;
    std::vector<float> nums;
    while (iss >> v) nums.push_back(v);
    if (nums.size() != 3) return false;
    out = { nums[0], nums[1], nums[2] };
    return true;
}

bool ParseColor(const std::string& s, Color& out) {
    std::string t = s;
    t.erase(std::remove(t.begin(), t.end(), '('), t.end());
    t.erase(std::remove(t.begin(), t.end(), ')'), t.end());
    std::replace(t.begin(), t.end(), ',', ' ');
    std::istringstream iss(t);
    float v;
    std::vector<float> nums;
    while (iss >> v) nums.push_back(v);
    if (nums.size() != 3) return false;
    out = { static_cast<unsigned char>(nums[0]), static_cast<unsigned char>(nums[1]),
            static_cast<unsigned char>(nums[2]), 255 };
    return true;
}
} // namespace

void SetPlayStateHook(PlayStateHook hook) { g_playStateHook = hook; }
PlayStateHook GetPlayStateHook() { return g_playStateHook; }

void SetActiveRuntime(ScriptRuntime* rt) { g_activeRuntime = rt; }
ScriptRuntime* GetActiveRuntime() { return g_activeRuntime; }

ScriptRuntime::ScriptRuntime() = default;
ScriptRuntime::~ScriptRuntime() = default;

void ScriptRuntime::Update(float dt)
{
    // The owning CoreCLRHost already forwards dt to the managed host through
    // its delegates; this runtime only needs to keep its play-state in sync,
    // which CoreCLRHost drives via OnPlayStarted()/OnPlayStopped(). Nothing to
    // step here — scripts are ticked entirely on the managed side.
    (void)dt;
}

void ScriptRuntime::BindWorld(std::vector<ScatteredObject*>& objects,
                              std::vector<std::unique_ptr<ModelGroup>>& models,
                              Camera3D& camera, phys::Simulation& sim, Engine& engine)
{
    objectsPtr = &objects;
    modelsPtr = &models;
    cameraPtr = &camera;
    this->sim = &sim;
    this->engine = &engine;
}

ScatteredObject* ScriptRuntime::FindByName(const std::string& name) const
{
    if (!objectsPtr) return nullptr;
    for (auto* obj : *objectsPtr)
        if (obj && obj->GetName() == name) return obj;
    return nullptr;
}

ScatteredObject* ScriptRuntime::CreateObject(const std::string& shapeName)
{
    if (!objectsPtr || !engine) return nullptr;

    ShapeType type = ShapeType::Cube;
    if (shapeName == "Sphere")        type = ShapeType::Sphere;
    else if (shapeName == "Cylinder") type = ShapeType::Cylinder;
    else if (shapeName == "Wedge")    type = ShapeType::Wedge;
    else if (shapeName != "Cube")     return nullptr;

    auto newObj = std::make_unique<ScatteredObject>(
        Vector3{0, 0, 0}, Vector3{1.5f, 1.5f, 1.5f}, SKYBLUE, type);
    newObj->SetName(shapeName);
    newObj->anchored = false;
    ScatteredObject* raw = newObj.get();
    objectsPtr->push_back(raw);
    engine->AddEntity(std::move(newObj));
    if (isPlaying) {
        playCreatedObjects.push_back(raw);
        if (sim) sim->SpawnBodyForObject(raw);
    }
    return raw;
}

void ScriptRuntime::SetSimPlaying(bool playing)
{
    if (playing == isPlaying) return;
    isPlaying = playing;
    if (playing) OnPlayStarted();
    else OnPlayStopped();
}

void ScriptRuntime::SetBodyVelocity(ScatteredObject* object, Vector3 velocity)
{ if (sim) sim->SetBodyVelocity(object, velocity); }
void ScriptRuntime::SetBodyAngularVelocity(ScatteredObject* object, Vector3 velocity)
{ if (sim) sim->SetBodyAngularVelocity(object, velocity); }
void ScriptRuntime::SetBodyPosition(ScatteredObject* object, Vector3 position)
{ if (sim) sim->SetBodyPosition(object, position); }
void ScriptRuntime::SetBodyOrientation(ScatteredObject* object, Vector3 eulerDeg)
{ if (sim) sim->SetBodyOrientation(object, eulerDeg); }

bool ScriptRuntime::SetObjectProperty(ScatteredObject* target, const std::string& prop,
                                      const std::string& rhs, std::string& outError)
{
    if (!target) { outError = "No target object"; return false; }

    if (prop == "Position") {
        Vector3 v;
        if (!ParseVec3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        if (isPlaying) SetBodyPosition(target, v);
        *target->GetPosPtr() = v;
        return true;
    }
    if (prop == "Size") {
        Vector3 v;
        if (!ParseVec3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        *target->GetSizePtr() = v;
        target->ResetMassAuto();
        return true;
    }
    if (prop == "Mass") {
        float m = static_cast<float>(std::atof(rhs.c_str()));
        if (!(m > 0.0f)) { outError = "'" + rhs + "' is not a valid positive mass"; return false; }
        target->SetMass(m);
        return true;
    }
    if (prop == "Rotation") {
        Vector3 v;
        if (!ParseVec3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        if (isPlaying) SetBodyOrientation(target, v);
        *target->GetRotationPtr() = v;
        return true;
    }
    if (prop == "Origin") {
        Vector3 v;
        if (!ParseVec3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        *target->GetOriginPtr() = v;
        return true;
    }
    if (prop == "Color") {
        Color c;
        if (!ParseColor(rhs, c)) { outError = "'" + rhs + "' is not a valid color (r, g, b)"; return false; }
        *target->GetColorPtr() = c;
        return true;
    }
    if (prop == "Velocity") {
        Vector3 v;
        if (!ParseVec3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        if (isPlaying) SetBodyVelocity(target, v);
        target->SetVelocity(v);
        return true;
    }
    if (prop == "AngularVelocity") {
        Vector3 v;
        if (!ParseVec3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        if (isPlaying) SetBodyAngularVelocity(target, v);
        target->SetAngularVelocity(v);
        return true;
    }
    if (prop == "Anchored") {
        bool value;
        if (!ParseBool(rhs, value)) { outError = "'" + rhs + "' is not a valid boolean (true/false)"; return false; }
        target->anchored = value;
        return true;
    }
    if (prop == "CanCollide") {
        bool value;
        if (!ParseBool(rhs, value)) { outError = "'" + rhs + "' is not a valid boolean (true/false)"; return false; }
        target->canCollide = value;
        return true;
    }
    if (prop == "CollisionAccuracy") {
        pcoll::CollisionAccuracy acc;
        if (!pcoll::ParseCollisionAccuracy(rhs, acc)) {
            outError = "'" + rhs + "' is not a valid collision accuracy (Box, Hull, Default, Precise)";
            return false;
        }
        target->SetCollisionAccuracy(acc);
        return true;
    }
    if (prop == "Transparency") {
        float t = static_cast<float>(std::atof(rhs.c_str()));
        if (t < 0.0f || t > 1.0f) { outError = "'" + rhs + "' is out of range (0 to 1)"; return false; }
        target->SetTransparency(t);
        return true;
    }

    outError = "Unknown property '" + prop + "'";
    return false;
}

void ScriptRuntime::SetPhysicsGravity(float g) { if (sim) sim->SetGravity(g); }
float ScriptRuntime::GetPhysicsGravity() const { return sim ? sim->GetGravity() : -19.62f; }
void ScriptRuntime::SetPhysicsFriction(float f) { if (sim) sim->SetFriction(f); }
float ScriptRuntime::GetPhysicsFriction() const { return sim ? sim->GetFriction() : 0.4f; }
void ScriptRuntime::SetPhysicsRestitution(float r) { if (sim) sim->SetRestitution(r); }
float ScriptRuntime::GetPhysicsRestitution() const { return sim ? sim->GetRestitution() : 0.7f; }

void ScriptRuntime::RemoveStandalone(int index)
{
    if (index < 0 || index >= static_cast<int>(standaloneScripts.size())) return;
    StopStandaloneScript(index);
    standaloneScripts.erase(standaloneScripts.begin() + index);
}

void ScriptRuntime::StartObjectScript(ScatteredObject* obj)
{
    if (hostPtr && obj && !obj->script.empty()) hostPtr->StartObjectScriptOn(obj);
}

void ScriptRuntime::StopObjectScript(ScatteredObject* obj)
{
    if (hostPtr && obj) hostPtr->StopObjectScriptOn(obj);
}

void ScriptRuntime::StartStandaloneScript(int index)
{
    if (hostPtr && index >= 0 && index < static_cast<int>(standaloneScripts.size()))
        hostPtr->StartStandaloneScriptAt(index);
}

void ScriptRuntime::StopStandaloneScript(int index)
{
    if (hostPtr) hostPtr->StopStandaloneScriptAt(index);
}

void ScriptRuntime::StopAllScripts()
{
    if (hostPtr) hostPtr->ClearAllScripts();
}

void ScriptRuntime::OnPlayStarted()
{
    if (!hostPtr) return;
    playCreatedObjects.clear();

    // Start every runOnPlay object script and standalone script.
    if (objectsPtr) {
        for (auto* obj : *objectsPtr)
            if (obj && obj->runOnPlay && !obj->script.empty())
                hostPtr->StartObjectScriptOn(obj);
    }
    for (int i = 0; i < static_cast<int>(standaloneScripts.size()); ++i)
        if (standaloneScripts[i].runOnPlay)
            hostPtr->StartStandaloneScriptAt(i);
}

void ScriptRuntime::RollbackCreatedObjects()
{
    // Remove objects created during play and give the engine back ownership
    // only for the ones we tracked.
    if (objectsPtr) {
        for (auto it = playCreatedObjects.rbegin(); it != playCreatedObjects.rend(); ++it) {
            ScatteredObject* obj = *it;
            if (!obj) continue;
            auto fit = std::find(objectsPtr->begin(), objectsPtr->end(), obj);
            if (fit != objectsPtr->end()) objectsPtr->erase(fit);
        }
    }
    playCreatedObjects.clear();
}

void ScriptRuntime::OnPlayStopped()
{
    if (hostPtr) hostPtr->ClearAllScripts();
    RollbackCreatedObjects();
}

void ScriptRuntime::LogCreatedObject(ScatteredObject* obj)
{
    if (isPlaying && obj) playCreatedObjects.push_back(obj);
}