// FlyScriptApi.cpp - C API bridge between the engine and C++ game scripts.
// Provides the FlyNative_* functions for object manipulation, world services, and script control.

#include "../include/Engine/Scripts/FlyScriptApi.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "../../../include/Engine/Scripts/NativeScriptHost.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Graphics/Graphics.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Engine/Backend/VisualScript.hpp"
#include "../../../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../../../include/Graphics/Particles.hpp"
#include "../../../include/Engine/GameUI.hpp"
#include "../../../include/Engine/Navigation.hpp"
#include "../../../ScriptingSDK/include/FlyScriptABI.h"
#include "raymath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#if defined(_MSC_VER)
    #include <excpt.h>   // EXCEPTION_EXECUTE_HANDLER, for FLY_CATCH
#endif

namespace {

// The world context owned by the active NativeScriptHost (or bound via
// FlyNative_BindRuntime). When null or not yet bound to a scene, every
// world-touching FlyNative_* call no-ops (scripting disabled).
ScriptRuntime* ActiveRuntime() {
    return GetActiveRuntime();
}

// Maps an id64 handle to a live ScatteredObject*. Returns nullptr if the
// handle is 0 or no longer refers to an object in the scene.
ScatteredObject* Resolve(unsigned long long handle) {
    if (handle == 0) return nullptr;
    ScriptRuntime* rt = ActiveRuntime();
    if (!rt || !rt->HasWorld()) return nullptr;

    // Validate pointer before casting
    auto* obj = reinterpret_cast<ScatteredObject*>(static_cast<uintptr_t>(handle));
    if (!obj) return nullptr;

    // Validate object pointer is in valid memory range (basic check)
    if (reinterpret_cast<uintptr_t>(obj) < 0x10000) return nullptr;

    try {
        const auto& objects = rt->GetObjects();
        for (auto* o : objects) {
            if (o == obj) return obj;
        }
    } catch (...) {
        // Catch any exceptions from GetObjects() or iteration
        return nullptr;
    }
    return nullptr;
}

unsigned long long MakeHandle(ScatteredObject* obj) {
    if (!obj) return 0;
    return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(obj));
}

// A script can hand us NaN or infinity; once it is in a transform or a physics body it spreads (and the physics engine asserts).
static bool Finite3(float x, float y, float z) { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }

// Safe string copy with bounds checking
static void SafeStrCopy(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) return;
    if (!src) { dest[0] = '\0'; return; }
    strncpy(dest, src, destSize - 1);
    dest[destSize - 1] = '\0';
}

// Safe string length
static size_t SafeStrLen(const char* str) {
    if (!str) return 0;
    return strnlen(str, 1024); // Max 1KB strings
}

} // namespace

// Internal implementation functions (no SEH, can use C++ objects)
namespace impl {

void FlyNative_BindRuntime_Impl(void* hostPtr) {
    SetActiveRuntime(reinterpret_cast<ScriptRuntime*>(hostPtr));
}

unsigned long long FlyNative_FindObject_Impl(const char* name) {
    if (!name) return 0;
    ScriptRuntime* rt = ActiveRuntime();
    if (!rt) return 0;
    std::string n = name;
    size_t dot = n.rfind('.');
    if (dot != std::string::npos) {
        std::string modelName = n.substr(0, dot);
        std::string partName = n.substr(dot + 1);
        for (const auto& m : rt->GetModels()) {
            if (!m || m->name != modelName) continue;
            for (auto* part : m->members)
                if (part && part->GetName() == partName) return MakeHandle(part);
        }
        return 0;
    }
    return MakeHandle(rt->FindByName(n));
}

unsigned long long FlyNative_GetSelf_Impl(void) {
    if (ScriptRuntime* rt = ActiveRuntime()) {
        return MakeHandle(rt->GetScriptSelf());
    }
    return 0;
}

void FlyNative_BindSelf_Impl(unsigned long long handle) {
    if (ScriptRuntime* rt = ActiveRuntime()) {
        rt->SetScriptSelf(Resolve(handle));
    }
}

// ---- vec3 getters ----

#define DEFINE_VEC3_GET_IMPL(PropName, Mapper)                                      \
    short FlyNative_Get##PropName##_Impl(unsigned long long h, float* x, float* y, float* z) { \
        auto* obj = Resolve(h);                                                \
        if (!obj || !x || !y || !z) return 0;                                  \
        Vector3 v = Mapper;                                                    \
        *x = v.x; *y = v.y; *z = v.z;                                          \
        return 1;                                                              \
    }

DEFINE_VEC3_GET_IMPL(Position, *obj->GetPosPtr())
DEFINE_VEC3_GET_IMPL(Size, *obj->GetSizePtr())
DEFINE_VEC3_GET_IMPL(Rotation, *obj->GetRotationPtr())
DEFINE_VEC3_GET_IMPL(Origin, *obj->GetOriginPtr())
DEFINE_VEC3_GET_IMPL(Velocity, obj->GetVelocity())
DEFINE_VEC3_GET_IMPL(AngularVelocity, obj->GetAngularVelocity())

// ---- vec3 setters ----

short FlyNative_SetPosition_Impl(unsigned long long h, float x, float y, float z) {
    if (!Finite3(x, y, z)) return 0;
    auto* obj = Resolve(h);
    if (!obj) return 0;
    Vector3 v{ x, y, z };
    if (ScriptRuntime* rt = ActiveRuntime()) if (rt->IsSimPlaying()) rt->SetBodyPosition(obj, v);
    *obj->GetPosPtr() = v;
    return 1;
}
short FlyNative_SetSize_Impl(unsigned long long h, float x, float y, float z) {
    if (!Finite3(x, y, z)) return 0;
    auto* obj = Resolve(h);
    if (!obj) return 0;
    *obj->GetSizePtr() = Vector3{ x, y, z };
    obj->ResetMassAuto();
    if (ScriptRuntime* rt = ActiveRuntime()) rt->RebuildBody(obj);   // the collider was built for the old size
    return 1;
}
short FlyNative_SetRotation_Impl(unsigned long long h, float x, float y, float z) {
    if (!Finite3(x, y, z)) return 0;
    auto* obj = Resolve(h);
    if (!obj) return 0;
    Vector3 v{ x, y, z };
    if (ScriptRuntime* rt = ActiveRuntime()) if (rt->IsSimPlaying()) rt->SetBodyOrientation(obj, v);
    *obj->GetRotationPtr() = v;
    return 1;
}
short FlyNative_SetOrigin_Impl(unsigned long long h, float x, float y, float z) {
    if (!Finite3(x, y, z)) return 0;
    auto* obj = Resolve(h);
    if (!obj) return 0;
    *obj->GetOriginPtr() = Vector3{ x, y, z };
    return 1;
}
short FlyNative_SetVelocity_Impl(unsigned long long h, float x, float y, float z) {
    if (!Finite3(x, y, z)) return 0;
    auto* obj = Resolve(h);
    if (!obj) return 0;
    Vector3 v{ x, y, z };
    if (ScriptRuntime* rt = ActiveRuntime()) if (rt->IsSimPlaying()) rt->SetBodyVelocity(obj, v);
    obj->SetVelocity(v);
    return 1;
}
short FlyNative_SetAngularVelocity_Impl(unsigned long long h, float x, float y, float z) {
    if (!Finite3(x, y, z)) return 0;
    auto* obj = Resolve(h);
    if (!obj) return 0;
    Vector3 v{ x, y, z };
    if (ScriptRuntime* rt = ActiveRuntime()) if (rt->IsSimPlaying()) rt->SetBodyAngularVelocity(obj, v);
    obj->SetAngularVelocity(v);
    return 1;
}

// ---- color ----

short FlyNative_GetColor_Impl(unsigned long long h, unsigned char* r, unsigned char* g, unsigned char* b) {
    auto* obj = Resolve(h);
    if (!obj || !r || !g || !b) return 0;
    Color c = *obj->GetColorPtr();
    *r = c.r; *g = c.g; *b = c.b;
    return 1;
}

short FlyNative_SetColor_Impl(unsigned long long h, unsigned char r, unsigned char g, unsigned char b) {
    auto* obj = Resolve(h);
    if (!obj) return 0;
    Color* c = obj->GetColorPtr();
    c->r = r; c->g = g; c->b = b;
    return 1;
}

// ---- booleans / scalars ----

short FlyNative_GetAnchored_Impl(unsigned long long h) {
    auto* obj = Resolve(h);
    return obj ? obj->anchored : 0;
}
void FlyNative_SetAnchored_Impl(unsigned long long h, short v) {
    auto* obj = Resolve(h);
    if (!obj) return;
    const bool anchored = v != 0;
    if (obj->anchored == anchored) return;
    obj->anchored = anchored;
    if (ScriptRuntime* rt = ActiveRuntime()) rt->RebuildBody(obj);
}

short FlyNative_GetCanCollide_Impl(unsigned long long h) {
    auto* obj = Resolve(h);
    return obj ? obj->canCollide : 0;
}
void FlyNative_SetCanCollide_Impl(unsigned long long h, short v) {
    auto* obj = Resolve(h);
    if (!obj) return;
    const bool collide = v != 0;
    if (obj->canCollide == collide) return;
    obj->canCollide = collide;
    if (ScriptRuntime* rt = ActiveRuntime()) rt->RebuildBody(obj);
}

float FlyNative_GetMass_Impl(unsigned long long h) {
    auto* obj = Resolve(h);
    return obj ? obj->GetMass() : 0.0f;
}
void FlyNative_SetMass_Impl(unsigned long long h, float m) {
    if (auto* obj = Resolve(h)) if (m > 0.0f) obj->SetMass(m);
}

float FlyNative_GetTransparency_Impl(unsigned long long h) {
    auto* obj = Resolve(h);
    return obj ? obj->GetTransparency() : 0.0f;
}
void FlyNative_SetTransparency_Impl(unsigned long long h, float t) {
    if (auto* obj = Resolve(h)) if (t >= 0.0f && t <= 1.0f) obj->SetTransparency(t);
}

int FlyNative_GetCollisionAccuracy_Impl(unsigned long long h) {
    auto* obj = Resolve(h);
    if (!obj) return -1;
    switch (obj->GetCollisionAccuracy()) {
        case pcoll::CollisionAccuracy::Box: return 0;
        case pcoll::CollisionAccuracy::Hull: return 1;
        case pcoll::CollisionAccuracy::Default: return 2;
        case pcoll::CollisionAccuracy::Precise: return 3;
    }
    return 2;
}
void FlyNative_SetCollisionAccuracy_Impl(unsigned long long h, int a) {
    auto* obj = Resolve(h);
    if (!obj) return;
    pcoll::CollisionAccuracy acc = pcoll::CollisionAccuracy::Default;
    switch (a) {
        case 0: acc = pcoll::CollisionAccuracy::Box; break;
        case 1: acc = pcoll::CollisionAccuracy::Hull; break;
        case 2: acc = pcoll::CollisionAccuracy::Default; break;
        case 3: acc = pcoll::CollisionAccuracy::Precise; break;
        default: return;
    }
    obj->SetCollisionAccuracy(acc);
    if (ScriptRuntime* rt = ActiveRuntime()) rt->RebuildBody(obj);
}

// ---- world services ----

short FlyNative_IsPlaying_Impl(void) {
    ScriptRuntime* rt = ActiveRuntime();
    return rt ? (rt->IsSimPlaying() ? 1 : 0) : 0;
}

short FlyNative_GetShadowsEnabled_Impl(void) { return gfx::IsShadowsEnabled() ? 1 : 0; }
void  FlyNative_SetShadowsEnabled_Impl(short on) { gfx::SetShadowsEnabled(on != 0); }
int   FlyNative_GetShadowQuality_Impl(void) { return gfx::GetShadowQuality(); }
void  FlyNative_SetShadowQuality_Impl(int q) { if (q < 5) q = 5; if (q > 100) q = 100; gfx::SetShadowQuality(q); }
float FlyNative_GetAmbient_Impl(float* intensity) {
    const float a = gfx::GetAmbientIntensity();
    if (intensity) *intensity = a;   // the out-parameter is optional
    return a;
}
void  FlyNative_SetAmbient_Impl(float a) { if (!std::isfinite(a)) return; if (a < 0.0f) a = 0.0f; if (a > 2.0f) a = 2.0f; gfx::SetAmbientIntensity(a); }
float FlyNative_GetEnvironment_Impl(int what) {
    const gfx::LightingSettings& L = gfx::Lighting();
    switch (what) {
        case FLY_ENV_TIME_OF_DAY:    return L.timeOfDay;
        case FLY_ENV_DAY_LENGTH:     return L.dayLengthMinutes;
        case FLY_ENV_OVERCAST:       return L.hasWeather ? L.overcast : 0.0f;
        case FLY_ENV_RAIN:           return L.hasWeather ? L.rain : 0.0f;
        case FLY_ENV_WET_GROUND:     return L.hasWeather ? L.wetGround : 0.0f;
        case FLY_ENV_FOG:            return L.hasFog ? L.fogDensity : 0.0f;
        case FLY_ENV_CLOUD_COVER:    return L.hasClouds ? L.cloudCoverage : 0.0f;
        case FLY_ENV_WIND_SPEED:     return L.windSpeed;
        case FLY_ENV_WIND_DIRECTION: return L.windDirection;
        case FLY_ENV_EXPOSURE:       return L.exposure;
        case FLY_ENV_SUN_INTENSITY:  return L.sunIntensity;
        case FLY_ENV_BLOOM:          return L.bloom;
        default:                     return 0.0f;
    }
}
void FlyNative_SetEnvironment_Impl(int what, float v) {
    if (!std::isfinite(v)) return;      // NaN or infinity
    gfx::LightingSettings& L = gfx::Lighting();
    const auto clampf = [](float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); };
    switch (what) {
        case FLY_ENV_TIME_OF_DAY:    gfx::SetTimeOfDay(v); break;
        case FLY_ENV_DAY_LENGTH:     L.dayLengthMinutes = clampf(v, 0.0f, 1440.0f); break;
        case FLY_ENV_OVERCAST:       L.hasWeather = true; L.overcast = clampf(v, 0.0f, 1.0f); break;
        case FLY_ENV_RAIN:           L.hasWeather = true; L.rain = clampf(v, 0.0f, 1.0f); break;
        case FLY_ENV_WET_GROUND:     L.hasWeather = true; L.wetGround = clampf(v, 0.0f, 1.0f); break;
        case FLY_ENV_FOG:            L.hasFog = true; L.fogDensity = clampf(v, 0.0f, 0.2f); break;
        case FLY_ENV_CLOUD_COVER:    L.hasClouds = true; L.cloudCoverage = clampf(v, 0.0f, 1.0f); break;
        case FLY_ENV_WIND_SPEED:     L.windSpeed = clampf(v, 0.0f, 80.0f); break;
        case FLY_ENV_WIND_DIRECTION: L.windDirection = v; break;
        case FLY_ENV_EXPOSURE:       L.hasPicture = true; L.exposure = clampf(v, -4.0f, 4.0f); break;
        case FLY_ENV_SUN_INTENSITY:  L.hasSun = true; L.sunIntensity = clampf(v, 0.0f, 3.0f); break;
        case FLY_ENV_BLOOM:          L.hasPicture = true; L.bloom = clampf(v, 0.0f, 1.0f); break;
        default: break;
    }
}
short FlyNative_GetGridVisible_Impl(void) { return gfx::IsGridVisible() ? 1 : 0; }
void  FlyNative_SetGridVisible_Impl(short on) { gfx::SetGridVisible(on != 0); }
short FlyNative_GetWireframe_Impl(void) { return gfx::IsWireframe() ? 1 : 0; }
void  FlyNative_SetWireframe_Impl(short on) { gfx::SetWireframe(on != 0); }
float FlyNative_GetFov_Impl(float* fov) {
    ScriptRuntime* rt = ActiveRuntime();
    const float f = rt ? rt->GetCamera().fovy : 45.0f;
    if (fov) *fov = f;   // optional out-parameter; the return value is the FOV either way
    return f;
}
void FlyNative_SetFov_Impl(float f) {
    if (!std::isfinite(f)) return;
    if (f < 10.0f) f = 10.0f;
    if (f > 170.0f) f = 170.0f;
    if (ScriptRuntime* rt = ActiveRuntime()) rt->GetCamera().fovy = f;
}

float FlyNative_GetGravity_Impl(void) {
    ScriptRuntime* rt = ActiveRuntime();
    return rt ? rt->GetPhysicsGravity() : -19.62f;
}
void FlyNative_SetGravity_Impl(float g) {
    if (ScriptRuntime* rt = ActiveRuntime()) if (g >= -30.0f && g <= 0.0f) rt->SetPhysicsGravity(g);
}
float FlyNative_GetFriction_Impl(void) {
    ScriptRuntime* rt = ActiveRuntime();
    return rt ? rt->GetPhysicsFriction() : 0.4f;
}
void FlyNative_SetFriction_Impl(float f) {
    if (ScriptRuntime* rt = ActiveRuntime()) if (f >= 0.0f && f <= 1.0f) rt->SetPhysicsFriction(f);
}
float FlyNative_GetRestitution_Impl(void) {
    ScriptRuntime* rt = ActiveRuntime();
    return rt ? rt->GetPhysicsRestitution() : 0.7f;
}
void FlyNative_SetRestitution_Impl(float r) {
    if (ScriptRuntime* rt = ActiveRuntime()) if (r >= 0.0f && r <= 1.0f) rt->SetPhysicsRestitution(r);
}

// ---- create ----

unsigned long long FlyNative_CreateObject_Impl(const char* shapeName) {
    if (!shapeName) return 0;
    ScriptRuntime* rt = ActiveRuntime();
    if (!rt) return 0;
    return MakeHandle(rt->CreateObject(shapeName));
}

// ---- script control (forwarded to the NativeScriptHost) ----

NativeScript::NativeScriptHost* ActiveHost() {
    ScriptRuntime* rt = ActiveRuntime();
    return rt ? rt->GetHost() : nullptr;
}

// Returns the object handle (nonzero) when the script started.
unsigned long long FlyNative_StartObjectScript_Impl(unsigned long long objectHandle, const char* typeName) {
    auto* host = ActiveHost();
    auto* obj = Resolve(objectHandle);
    if (!host || !obj) return 0;
    if (typeName && *typeName) obj->script = typeName;
    return host->StartObjectScriptOn(obj) ? objectHandle : 0;
}
void FlyNative_StopObjectScript_Impl(unsigned long long objectHandle) {
    if (auto* host = ActiveHost()) host->StopObjectScriptOn(Resolve(objectHandle));
}
short FlyNative_IsObjectScriptRunning_Impl(unsigned long long objectHandle) {
    auto* host = ActiveHost();
    auto* obj = Resolve(objectHandle);
    return (host && obj && host->IsObjectScriptRunning(obj)) ? 1 : 0;
}
// Returns index + 1 (nonzero) when the script started.
unsigned long long FlyNative_StartStandaloneScript_Impl(int index, const char* typeName) {
    ScriptRuntime* rt = ActiveRuntime();
    auto* host = ActiveHost();
    if (!rt || !host) return 0;
    auto& scripts = rt->StandaloneScripts();
    if (index < 0 || index >= static_cast<int>(scripts.size())) return 0;
    if (typeName && *typeName) scripts[index].typeName = typeName;
    return host->StartStandaloneScriptAt(index) ? static_cast<unsigned long long>(index) + 1 : 0;
}
void FlyNative_StopStandaloneScript_Impl(int index) {
    if (auto* host = ActiveHost()) host->StopStandaloneScriptAt(index);
}
short FlyNative_IsStandaloneScriptRunning_Impl(int index) {
    auto* host = ActiveHost();
    return (host && host->IsStandaloneScriptRunning(index)) ? 1 : 0;
}
void FlyNative_ClearAllScripts_Impl(void) {
    if (auto* host = ActiveHost()) host->ClearAllScripts();
}

// ---- print ----

void FlyNative_Print_Impl(const char* text) {
    if (!text) return;
    ui::LogAlways("%s", text);
}

const char* FlyNative_SnippetGetInput_Impl(const char* snippet, int pin) {
    static thread_local std::string value;
    value.clear();
    if (ScriptRuntime* rt = ActiveRuntime(); rt && snippet) value = rt->SnippetInput(snippet, pin);
    return value.c_str();
}

void FlyNative_SnippetSetOutput_Impl(const char* snippet, int pin, const char* value) {
    if (ScriptRuntime* rt = ActiveRuntime(); rt && snippet) rt->SetSnippetOutput(snippet, pin, value ? value : "");
}

// ---- what the Visual Script nodes do ----

namespace {
std::string VecText(float x, float y, float z) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "(%.9g, %.9g, %.9g)", x, y, z);
    return buf;
}
}

void FlyNative_DestroyObject_Impl(unsigned long long h) {
    ScriptRuntime* rt = ActiveRuntime();
    if (auto* obj = Resolve(h); obj && rt) rt->DestroyObject(obj);
}
unsigned long long FlyNative_CloneObject_Impl(unsigned long long h) {
    ScriptRuntime* rt = ActiveRuntime();
    auto* obj = Resolve(h);
    return (rt && obj) ? MakeHandle(rt->CloneObject(obj)) : 0;
}
unsigned long long FlyNative_UnionObjects_Impl(const unsigned long long* parts, int count, int mode) {
    ScriptRuntime* rt = ActiveRuntime();
    if (!rt || !parts || count < 2 || count > 4096) return 0;
    std::vector<ScatteredObject*> objs;
    for (int i = 0; i < count; ++i) {
        ScatteredObject* o = Resolve(parts[i]);
        if (!o) return 0;   // a stale handle: build nothing rather than a union of the rest
        objs.push_back(o);
    }
    return MakeHandle(rt->UnionObjects(objs, mode == 1));
}
int FlyNative_SeparateUnion_Impl(unsigned long long h, unsigned long long* out, int maxOut) {
    ScriptRuntime* rt = ActiveRuntime();
    ScatteredObject* u = Resolve(h);
    std::vector<ScatteredObject*> made;
    if (!rt || !u || !rt->SeparateUnion(u, made)) return 0;
    for (int i = 0; out && i < maxOut && i < (int)made.size(); ++i) out[i] = MakeHandle(made[(size_t)i]);
    return (int)made.size();
}
int FlyNative_UnionPartCount_Impl(unsigned long long h) {
    auto* o = Resolve(h);
    return o && o->IsUnion() ? (int)o->UnionParts().size() : 0;
}
short FlyNative_SetNegate_Impl(unsigned long long h, short on) {
    auto* o = Resolve(h);
    if (!o || o->HasModel() || o->IsUnion()) return 0;
    o->negate = on != 0;
    o->canCollide = !o->negate;
    if (ScriptRuntime* rt = ActiveRuntime()) rt->RebuildBody(o);
    return 1;
}
short FlyNative_GetNegate_Impl(unsigned long long h) {
    auto* o = Resolve(h);
    return o && o->negate ? 1 : 0;
}
short FlyNative_SetUseGlobalColor_Impl(unsigned long long h, short on) {
    auto* o = Resolve(h);
    if (!o || !o->IsUnion()) return 0;
    o->SetUseGlobalColor(on != 0);
    return 1;
}
short FlyNative_GetUseGlobalColor_Impl(unsigned long long h) {
    auto* o = Resolve(h);
    return o && o->IsUnion() && o->useGlobalColor ? 1 : 0;
}
const char* FlyNative_GetName_Impl(unsigned long long h) {
    static thread_local std::string name;
    auto* obj = Resolve(h);
    name = obj ? obj->GetName() : std::string();
    return name.c_str();
}
void FlyNative_SetName_Impl(unsigned long long h, const char* name) {
    if (auto* obj = Resolve(h); obj && name) obj->SetName(name);
}
short FlyNative_AddForce_Impl(unsigned long long h, float x, float y, float z, bool impulse) {
    ScriptRuntime* rt = ActiveRuntime();
    auto* obj = Resolve(h);
    std::string err;
    return (rt && obj && rt->AddForce(obj, VecText(x, y, z), impulse, err)) ? 1 : 0;
}
const char* FlyNative_Raycast_Impl(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistance) {
    static thread_local std::string hit;
    hit.clear();
    if (ScriptRuntime* rt = ActiveRuntime()) {
        std::string err;
        hit = rt->RayCastName(VecText(ox, oy, oz), VecText(dx, dy, dz), maxDistance, err);
    }
    return hit.c_str();
}
void FlyNative_PlaySound_Impl(const char* file, float volume) {
    if (ScriptRuntime* rt = ActiveRuntime(); rt && file) vscript::PlaySoundFile(*rt, file, volume);
}


// ---- gameplay queries and effects ----

short FlyNative_RaycastEx_Impl(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistance, FlyRayHit* out) {
    ScriptRuntime* rt = ActiveRuntime();
    if (!rt || !out || !rt->GetSimulation()) return 0;
    const Vector3 dir = { dx, dy, dz };
    if (Vector3Length(dir) < 1e-6f) return 0;
    const phys::RaycastHit hit = rt->GetSimulation()->RayCast({ ox, oy, oz }, Vector3Normalize(dir), maxDistance > 0.0f ? maxDistance : 100.0f);
    if (!hit.hit) return 0;
    out->px = hit.point.x; out->py = hit.point.y; out->pz = hit.point.z;
    out->nx = hit.normal.x; out->ny = hit.normal.y; out->nz = hit.normal.z;
    out->distance = Vector3Distance({ ox, oy, oz }, hit.point);
    out->object = MakeHandle(hit.object);
    return 1;
}

int FlyNative_OverlapSphere_Impl(float cx, float cy, float cz, float radius, unsigned long long* out, int maxCount) {
    ScriptRuntime* rt = ActiveRuntime();
    if (!rt || !rt->HasWorld() || !out || maxCount <= 0 || !(radius > 0.0f)) return 0;
    const Vector3 c = { cx, cy, cz };
    int n = 0;
    for (ScatteredObject* o : rt->GetObjects()) {
        if (!o) continue;
        // The object's bounding ball: half the diagonal of its box.
        const float r = Vector3Length(*o->GetSizePtr()) * 0.5f;
        if (Vector3Distance(c, *o->GetPosPtr()) <= radius + r) {
            out[n++] = MakeHandle(o);
            if (n >= maxCount) break;
        }
    }
    return n;
}

int FlyNative_PollTouches_Impl(FlyTouch* out, int maxCount) {
    ScriptRuntime* rt = ActiveRuntime();
    phys::Simulation* sim = rt ? rt->GetSimulation() : nullptr;
    if (!sim || !out || maxCount <= 0) return 0;
    const auto& begins = sim->GetFrameBeginEvents();
    const auto& hits = sim->GetFrameHitEvents();
    int n = 0;
    for (const phys::ContactEvent& e : begins) {
        if (n >= maxCount) break;
        if (!e.objectA && !e.objectB) continue;
        FlyTouch& t = out[n++];
        t.a = MakeHandle(e.objectA ? e.objectA : e.objectB);
        t.b = e.objectA ? MakeHandle(e.objectB) : 0;
        t.px = e.point.x; t.py = e.point.y; t.pz = e.point.z;
        t.nx = 0.0f; t.ny = 1.0f; t.nz = 0.0f;
        t.speed = 0.0f;
        // The impact report for the same pair carries the real point, normal and speed.
        for (const phys::ContactEvent& h : hits) {
            if ((h.objectA == e.objectA && h.objectB == e.objectB) || (h.objectA == e.objectB && h.objectB == e.objectA)) {
                t.px = h.point.x; t.py = h.point.y; t.pz = h.point.z;
                t.nx = h.normal.x; t.ny = h.normal.y; t.nz = h.normal.z;
                t.speed = std::max(t.speed, h.approachSpeed);
            }
        }
    }
    return n;
}

std::map<unsigned, b3JointId>& Joints() {
    static std::map<unsigned, b3JointId> joints;
    return joints;
}

unsigned FlyNative_CreateJoint_Impl(int kind, unsigned long long a, unsigned long long b, float ax, float ay, float az, float bx, float by, float bz) {
    ScriptRuntime* rt = ActiveRuntime();
    phys::Simulation* sim = rt ? rt->GetSimulation() : nullptr;
    ScatteredObject* oa = Resolve(a);
    ScatteredObject* ob = Resolve(b);
    if (!sim || !sim->IsPlaying() || !oa || !ob || oa == ob) return 0;
    const Vector3 pa = { ax, ay, az }, pb = { bx, by, bz };
    b3JointId j{};
    switch (kind) {
        case FLY_JOINT_WELD:  j = sim->CreateWeldJoint(oa, ob, pa); break;
        case FLY_JOINT_HINGE: j = sim->CreateRevoluteJoint(oa, ob, pa); break;
        case FLY_JOINT_BALL:  j = sim->CreateSphericalJoint(oa, ob, pa); break;
        case FLY_JOINT_ROPE:  j = sim->CreateDistanceJoint(oa, ob, pa, pb); break;
        default: return 0;
    }
    if (!b3Joint_IsValid(j)) return 0;
    static unsigned next = 1;
    auto& joints = Joints();
    for (auto it = joints.begin(); it != joints.end();) it = b3Joint_IsValid(it->second) ? std::next(it) : joints.erase(it);   // the map outlives Play
    joints[next] = j;
    return next++;
}

void FlyNative_DestroyJoint_Impl(unsigned joint) {
    ScriptRuntime* rt = ActiveRuntime();
    phys::Simulation* sim = rt ? rt->GetSimulation() : nullptr;
    auto& joints = Joints();
    auto it = joints.find(joint);
    if (it == joints.end()) return;
    if (sim && b3Joint_IsValid(it->second)) sim->DestroyJoint(it->second);
    joints.erase(it);
}

void FlyNative_GetCamera_Impl(float* out7) {
    ScriptRuntime* rt = ActiveRuntime();
    if (!rt || !rt->HasWorld() || !out7) return;
    const Camera3D& c = rt->GetCamera();
    out7[0] = c.position.x; out7[1] = c.position.y; out7[2] = c.position.z;
    out7[3] = c.target.x;   out7[4] = c.target.y;   out7[5] = c.target.z;
    out7[6] = c.fovy;
}

void FlyNative_SetCamera_Impl(const float* in7) {
    ScriptRuntime* rt = ActiveRuntime();
    if (!rt || !rt->HasWorld() || !in7) return;
    if (!Finite3(in7[0], in7[1], in7[2]) || !Finite3(in7[3], in7[4], in7[5])) return;
    Camera3D& c = rt->GetCamera();
    c.position = { in7[0], in7[1], in7[2] };
    c.target = { in7[3], in7[4], in7[5] };
    if (in7[6] > 1.0f && in7[6] < 170.0f) c.fovy = in7[6];
}

void FlyNative_EmitParticles_Impl(const char* preset, float x, float y, float z, float dx, float dy, float dz, int count, float scale) {
    fx::Preset p;
    if (fx::PresetFromName(preset, p)) fx::Emit(p, { x, y, z }, { dx, dy, dz }, count, scale);
}
unsigned FlyNative_StartEmitter_Impl(const char* preset, float x, float y, float z, float dx, float dy, float dz, float scale) {
    fx::Preset p;
    return fx::PresetFromName(preset, p) ? (unsigned)fx::StartEmitter(p, { x, y, z }, { dx, dy, dz }, scale) : 0u;
}

} // namespace impl

extern "C" {

// SEH-protected wrapper functions (no C++ objects with destructors)

// Vec3 getters
#define WRAP_VEC3_GET(PropName) \
    FLY_API short FlyNative_Get##PropName(unsigned long long h, float* x, float* y, float* z) { \
        FLY_TRY {  return impl::FlyNative_Get##PropName##_Impl(h, x, y, z); } \
        FLY_CATCH(return 0;) \
    }

WRAP_VEC3_GET(Position)
WRAP_VEC3_GET(Size)
WRAP_VEC3_GET(Rotation)
WRAP_VEC3_GET(Origin)
WRAP_VEC3_GET(Velocity)
WRAP_VEC3_GET(AngularVelocity)

// Vec3 setters
#define WRAP_VEC3_SET(PropName) \
    FLY_API short FlyNative_Set##PropName(unsigned long long h, float x, float y, float z) { \
        FLY_TRY {  return impl::FlyNative_Set##PropName##_Impl(h, x, y, z); } \
        FLY_CATCH(return 0;) \
    }

WRAP_VEC3_SET(Position)
WRAP_VEC3_SET(Size)
WRAP_VEC3_SET(Rotation)
WRAP_VEC3_SET(Origin)
WRAP_VEC3_SET(Velocity)
WRAP_VEC3_SET(AngularVelocity)

// Color
FLY_API short FlyNative_GetColor(unsigned long long h, unsigned char* r, unsigned char* g, unsigned char* b) {
    FLY_TRY { return impl::FlyNative_GetColor_Impl(h, r, g, b); } FLY_CATCH(return 0;)
}
FLY_API short FlyNative_SetColor(unsigned long long h, unsigned char r, unsigned char g, unsigned char b) {
    FLY_TRY { return impl::FlyNative_SetColor_Impl(h, r, g, b); } FLY_CATCH(return 0;)
}

// Booleans/scalars
#define WRAP_SCALAR_GET(PropName) \
    FLY_API short FlyNative_Get##PropName(unsigned long long h) { \
        FLY_TRY {  return impl::FlyNative_Get##PropName##_Impl(h); } \
        FLY_CATCH(return 0;) \
    }
#define WRAP_SCALAR_SET(PropName) \
    FLY_API void FlyNative_Set##PropName(unsigned long long h, short v) { \
        FLY_TRY { impl::FlyNative_Set##PropName##_Impl(h, v); } FLY_CATCH() \
    }

WRAP_SCALAR_GET(Anchored)
WRAP_SCALAR_SET(Anchored)
WRAP_SCALAR_GET(CanCollide)
WRAP_SCALAR_SET(CanCollide)

FLY_API float FlyNative_GetMass(unsigned long long h) {
    FLY_TRY { return impl::FlyNative_GetMass_Impl(h); } FLY_CATCH(return 0.0f;)
}
FLY_API void FlyNative_SetMass(unsigned long long h, float m) {
    FLY_TRY { impl::FlyNative_SetMass_Impl(h, m); } FLY_CATCH()
}

FLY_API float FlyNative_GetTransparency(unsigned long long h) {
    FLY_TRY { return impl::FlyNative_GetTransparency_Impl(h); } FLY_CATCH(return 0.0f;)
}
FLY_API void FlyNative_SetTransparency(unsigned long long h, float t) {
    FLY_TRY { impl::FlyNative_SetTransparency_Impl(h, t); } FLY_CATCH()
}

FLY_API int FlyNative_GetCollisionAccuracy(unsigned long long h) {
    FLY_TRY { return impl::FlyNative_GetCollisionAccuracy_Impl(h); } FLY_CATCH(return -1;)
}
FLY_API void FlyNative_SetCollisionAccuracy(unsigned long long h, int a) {
    FLY_TRY { impl::FlyNative_SetCollisionAccuracy_Impl(h, a); } FLY_CATCH()
}

// World services
FLY_API short FlyNative_IsPlaying(void) { FLY_TRY { return impl::FlyNative_IsPlaying_Impl(); } FLY_CATCH(return 0;) }

FLY_API short FlyNative_GetShadowsEnabled(void) { FLY_TRY { return impl::FlyNative_GetShadowsEnabled_Impl(); } FLY_CATCH(return 0;) }
FLY_API void  FlyNative_SetShadowsEnabled(short on) { FLY_TRY { impl::FlyNative_SetShadowsEnabled_Impl(on); } FLY_CATCH() }
FLY_API int   FlyNative_GetShadowQuality(void) { FLY_TRY { return impl::FlyNative_GetShadowQuality_Impl(); } FLY_CATCH(return 0;) }
FLY_API void  FlyNative_SetShadowQuality(int q) { FLY_TRY { impl::FlyNative_SetShadowQuality_Impl(q); } FLY_CATCH() }
FLY_API float FlyNative_GetAmbient(float* intensity) { FLY_TRY { return impl::FlyNative_GetAmbient_Impl(intensity); } FLY_CATCH(return 0.0f;) }
FLY_API void  FlyNative_SetAmbient(float a) { FLY_TRY { impl::FlyNative_SetAmbient_Impl(a); } FLY_CATCH() }
FLY_API float FlyNative_GetEnvironment(int what) { FLY_TRY { return impl::FlyNative_GetEnvironment_Impl(what); } FLY_CATCH(return 0.0f;) }
FLY_API void  FlyNative_SetEnvironment(int what, float value) { FLY_TRY { impl::FlyNative_SetEnvironment_Impl(what, value); } FLY_CATCH() }
FLY_API short FlyNative_GetGridVisible(void) { FLY_TRY { return impl::FlyNative_GetGridVisible_Impl(); } FLY_CATCH(return 0;) }
FLY_API void  FlyNative_SetGridVisible(short on) { FLY_TRY { impl::FlyNative_SetGridVisible_Impl(on); } FLY_CATCH() }
FLY_API short FlyNative_GetWireframe(void) { FLY_TRY { return impl::FlyNative_GetWireframe_Impl(); } FLY_CATCH(return 0;) }
FLY_API void  FlyNative_SetWireframe(short on) { FLY_TRY { impl::FlyNative_SetWireframe_Impl(on); } FLY_CATCH() }
FLY_API float FlyNative_GetFov(float* fov) { FLY_TRY { return impl::FlyNative_GetFov_Impl(fov); } FLY_CATCH(return 0.0f;) }
FLY_API void FlyNative_SetFov(float f) { FLY_TRY { impl::FlyNative_SetFov_Impl(f); } FLY_CATCH() }

FLY_API float FlyNative_GetGravity(void) { FLY_TRY { return impl::FlyNative_GetGravity_Impl(); } FLY_CATCH(return -19.62f;) }
FLY_API void FlyNative_SetGravity(float g) { FLY_TRY { impl::FlyNative_SetGravity_Impl(g); } FLY_CATCH() }
FLY_API float FlyNative_GetFriction(void) { FLY_TRY { return impl::FlyNative_GetFriction_Impl(); } FLY_CATCH(return 0.4f;) }
FLY_API void FlyNative_SetFriction(float f) { FLY_TRY { impl::FlyNative_SetFriction_Impl(f); } FLY_CATCH() }
FLY_API float FlyNative_GetRestitution(void) { FLY_TRY { return impl::FlyNative_GetRestitution_Impl(); } FLY_CATCH(return 0.7f;) }
FLY_API void FlyNative_SetRestitution(float r) { FLY_TRY { impl::FlyNative_SetRestitution_Impl(r); } FLY_CATCH() }

// ---- create ----

FLY_API unsigned long long FlyNative_CreateObject(const char* shapeName) {
    FLY_TRY { return impl::FlyNative_CreateObject_Impl(shapeName); } FLY_CATCH(return 0;)
}

// ---- script control ----

FLY_API unsigned long long FlyNative_StartObjectScript(unsigned long long objectHandle, const char* typeName) {
    FLY_TRY { return impl::FlyNative_StartObjectScript_Impl(objectHandle, typeName); } FLY_CATCH(return 0;)
}

FLY_API void FlyNative_StopObjectScript(unsigned long long objectHandle) {
    FLY_TRY { impl::FlyNative_StopObjectScript_Impl(objectHandle); } FLY_CATCH()
}
FLY_API short FlyNative_IsObjectScriptRunning(unsigned long long objectHandle) { FLY_TRY { return impl::FlyNative_IsObjectScriptRunning_Impl(objectHandle); } FLY_CATCH(return 0;) }

FLY_API unsigned long long FlyNative_StartStandaloneScript(int index, const char* typeName) {
    FLY_TRY { return impl::FlyNative_StartStandaloneScript_Impl(index, typeName); } FLY_CATCH(return 0;)
}
FLY_API void FlyNative_StopStandaloneScript(int index) {
    FLY_TRY { impl::FlyNative_StopStandaloneScript_Impl(index); } FLY_CATCH()
}
FLY_API short FlyNative_IsStandaloneScriptRunning(int index) { FLY_TRY { return impl::FlyNative_IsStandaloneScriptRunning_Impl(index); } FLY_CATCH(return 0;) }

FLY_API void FlyNative_ClearAllScripts(void) {
    FLY_TRY { impl::FlyNative_ClearAllScripts_Impl(); } FLY_CATCH()
}

// ---- runtime binding ----

FLY_API void FlyNative_BindRuntime(void* hostPtr) {
    FLY_TRY { impl::FlyNative_BindRuntime_Impl(hostPtr); } FLY_CATCH()
}

// ---- print ----

FLY_API void FlyNative_Print(const char* text) {
    FLY_TRY { impl::FlyNative_Print_Impl(text); } FLY_CATCH()
}

FLY_API const char* FlyNative_SnippetGetInput(const char* snippet, int pin) {
    FLY_TRY { return impl::FlyNative_SnippetGetInput_Impl(snippet, pin); } FLY_CATCH(return "";)
}
FLY_API void FlyNative_SnippetSetOutput(const char* snippet, int pin, const char* value) {
    FLY_TRY { impl::FlyNative_SnippetSetOutput_Impl(snippet, pin, value); } FLY_CATCH()
}

FLY_API void FlyNative_DestroyObject(unsigned long long h) { FLY_TRY { impl::FlyNative_DestroyObject_Impl(h); } FLY_CATCH() }
FLY_API unsigned long long FlyNative_UnionObjects(const unsigned long long* parts, int count, int mode) { FLY_TRY { return impl::FlyNative_UnionObjects_Impl(parts, count, mode); } FLY_CATCH(return 0;) }
FLY_API int FlyNative_SeparateUnion(unsigned long long h, unsigned long long* out, int maxOut) { FLY_TRY { return impl::FlyNative_SeparateUnion_Impl(h, out, maxOut); } FLY_CATCH(return 0;) }
FLY_API int FlyNative_UnionPartCount(unsigned long long h) { FLY_TRY { return impl::FlyNative_UnionPartCount_Impl(h); } FLY_CATCH(return 0;) }
FLY_API short FlyNative_SetNegate(unsigned long long h, short on) { FLY_TRY { return impl::FlyNative_SetNegate_Impl(h, on); } FLY_CATCH(return 0;) }
FLY_API short FlyNative_GetNegate(unsigned long long h) { FLY_TRY { return impl::FlyNative_GetNegate_Impl(h); } FLY_CATCH(return 0;) }
FLY_API short FlyNative_SetUseGlobalColor(unsigned long long h, short on) { FLY_TRY { return impl::FlyNative_SetUseGlobalColor_Impl(h, on); } FLY_CATCH(return 0;) }
FLY_API short FlyNative_GetUseGlobalColor(unsigned long long h) { FLY_TRY { return impl::FlyNative_GetUseGlobalColor_Impl(h); } FLY_CATCH(return 0;) }
FLY_API unsigned long long FlyNative_CloneObject(unsigned long long h) { FLY_TRY { return impl::FlyNative_CloneObject_Impl(h); } FLY_CATCH(return 0;) }
FLY_API const char* FlyNative_GetName(unsigned long long h) { FLY_TRY { return impl::FlyNative_GetName_Impl(h); } FLY_CATCH(return "";) }
FLY_API void FlyNative_SetName(unsigned long long h, const char* name) { FLY_TRY { impl::FlyNative_SetName_Impl(h, name); } FLY_CATCH() }
FLY_API short FlyNative_AddForce(unsigned long long h, float x, float y, float z) { FLY_TRY { return impl::FlyNative_AddForce_Impl(h, x, y, z, false); } FLY_CATCH(return 0;) }
FLY_API short FlyNative_AddImpulse(unsigned long long h, float x, float y, float z) { FLY_TRY { return impl::FlyNative_AddForce_Impl(h, x, y, z, true); } FLY_CATCH(return 0;) }
FLY_API const char* FlyNative_Raycast(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistance) {
    FLY_TRY { return impl::FlyNative_Raycast_Impl(ox, oy, oz, dx, dy, dz, maxDistance); } FLY_CATCH(return "";)
}
FLY_API short FlyNative_KeyDown(const char* keyName) { FLY_TRY { return keyName && vscript::IsKeyHeld(keyName) ? 1 : 0; } FLY_CATCH(return 0;) }
FLY_API short FlyNative_KeyPressed(const char* keyName) { FLY_TRY { return keyName && vscript::WasKeyPressed(keyName) ? 1 : 0; } FLY_CATCH(return 0;) }
FLY_API void FlyNative_PlaySound(const char* file, float volume) { FLY_TRY { impl::FlyNative_PlaySound_Impl(file, volume); } FLY_CATCH() }
FLY_API void FlyNative_SetMasterVolume(float volume) { FLY_TRY { vscript::SetMasterVolumeLevel(volume); } FLY_CATCH() }
FLY_API void FlyNative_ShowText(const char* text, const char* position, float seconds) {
    FLY_TRY { vscript::ShowText(text ? text : "", position ? position : "", seconds); } FLY_CATCH()
}
FLY_API float FlyNative_TimeSincePlay(void) { FLY_TRY { return vscript::TimeSincePlay(); } FLY_CATCH(return 0.0f;) }

FLY_API short FlyNative_GamepadAvailable(int gamepad) { FLY_TRY { return vscript::IsGamepadAvailable(gamepad) ? 1 : 0; } FLY_CATCH(return 0;) }
FLY_API short FlyNative_GamepadButtonDown(int gamepad, const char* buttonName) {
    FLY_TRY { return buttonName && vscript::IsGamepadButtonHeld(gamepad, buttonName) ? 1 : 0; } FLY_CATCH(return 0;)
}
FLY_API short FlyNative_GamepadButtonPressed(int gamepad, const char* buttonName) {
    FLY_TRY { return buttonName && vscript::WasGamepadButtonPressed(gamepad, buttonName) ? 1 : 0; } FLY_CATCH(return 0;)
}
FLY_API float FlyNative_GamepadAxis(int gamepad, const char* axisName) {
    FLY_TRY { return axisName ? vscript::GamepadAxisValue(gamepad, axisName) : 0.0f; } FLY_CATCH(return 0.0f;)
}


FLY_API short FlyNative_RaycastEx(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistance, FlyRayHit* out) {
    FLY_TRY { return impl::FlyNative_RaycastEx_Impl(ox, oy, oz, dx, dy, dz, maxDistance, out); } FLY_CATCH(return 0;)
}
FLY_API int FlyNative_OverlapSphere(float cx, float cy, float cz, float radius, unsigned long long* out, int maxCount) {
    FLY_TRY { return impl::FlyNative_OverlapSphere_Impl(cx, cy, cz, radius, out, maxCount); } FLY_CATCH(return 0;)
}
FLY_API int FlyNative_PollTouches(FlyTouch* out, int maxCount) { FLY_TRY { return impl::FlyNative_PollTouches_Impl(out, maxCount); } FLY_CATCH(return 0;) }
FLY_API unsigned FlyNative_CreateJoint(int kind, unsigned long long a, unsigned long long b, float ax, float ay, float az, float bx, float by, float bz) {
    FLY_TRY { return impl::FlyNative_CreateJoint_Impl(kind, a, b, ax, ay, az, bx, by, bz); } FLY_CATCH(return 0;)
}
FLY_API void FlyNative_DestroyJoint(unsigned joint) { FLY_TRY { impl::FlyNative_DestroyJoint_Impl(joint); } FLY_CATCH() }
FLY_API void FlyNative_GetCamera(float* out7) { FLY_TRY { impl::FlyNative_GetCamera_Impl(out7); } FLY_CATCH() }
FLY_API void FlyNative_SetCamera(const float* in7) { FLY_TRY { impl::FlyNative_SetCamera_Impl(in7); } FLY_CATCH() }
FLY_API void FlyNative_EmitParticles(const char* preset, float x, float y, float z, float dx, float dy, float dz, int count, float scale) {
    FLY_TRY { impl::FlyNative_EmitParticles_Impl(preset, x, y, z, dx, dy, dz, count, scale); } FLY_CATCH()
}
FLY_API unsigned FlyNative_StartEmitter(const char* preset, float x, float y, float z, float dx, float dy, float dz, float scale) {
    FLY_TRY { return impl::FlyNative_StartEmitter_Impl(preset, x, y, z, dx, dy, dz, scale); } FLY_CATCH(return 0;)
}
FLY_API void FlyNative_MoveEmitter(unsigned emitter, float x, float y, float z, float dx, float dy, float dz) {
    FLY_TRY { fx::MoveEmitter((int)emitter, { x, y, z }, { dx, dy, dz }); } FLY_CATCH()
}
FLY_API void FlyNative_StopEmitter(unsigned emitter) { FLY_TRY { fx::StopEmitter((int)emitter); } FLY_CATCH() }
FLY_API void FlyNative_SpawnDecal(float x, float y, float z, float nx, float ny, float nz, float size, unsigned char r, unsigned char g, unsigned char b, unsigned char a, float seconds) {
    FLY_TRY { fx::Decal({ x, y, z }, { nx, ny, nz }, size, { r, g, b, a }, seconds); } FLY_CATCH()
}

FLY_API unsigned long long FlyNative_SpawnPrefab(const char* file, float x, float y, float z) {
    FLY_TRY {
        ScriptRuntime* rt = ActiveRuntime();
        return (rt && file) ? MakeHandle(rt->SpawnPrefab(file, { x, y, z })) : 0;
    } FLY_CATCH(return 0;)
}

FLY_API short FlyNative_UICreate(const char* id, int kind) {
    FLY_TRY {
        if (!id || kind < 0 || kind > static_cast<int>(gameui::Kind::Toggle)) return 0;
        return gameui::Create(id, static_cast<gameui::Kind>(kind)) ? 1 : 0;
    } FLY_CATCH(return 0;)
}
FLY_API void FlyNative_UISetLayout(const char* id, int anchor, float x, float y, float w, float h) {
    FLY_TRY {
        if (id && anchor >= 0 && anchor < static_cast<int>(gameui::Anchor::Count)) gameui::SetLayout(id, static_cast<gameui::Anchor>(anchor), x, y, w, h);
    } FLY_CATCH()
}
FLY_API void FlyNative_UISetColor(const char* id, unsigned char r, unsigned char g, unsigned char b, unsigned char a, unsigned char r2, unsigned char g2, unsigned char b2, unsigned char a2) {
    FLY_TRY { if (id) gameui::SetColor(id, { r, g, b, a }, { r2, g2, b2, a2 }); } FLY_CATCH()
}
FLY_API void FlyNative_UISetText(const char* id, const char* text, float fontSize) {
    FLY_TRY { if (id) gameui::SetText(id, text ? text : "", fontSize); } FLY_CATCH()
}
FLY_API void FlyNative_UISetValue(const char* id, float value) { FLY_TRY { if (id) gameui::SetValue(id, value); } FLY_CATCH() }
FLY_API void FlyNative_UISetImage(const char* id, const char* file) { FLY_TRY { if (id) gameui::SetImage(id, file ? file : ""); } FLY_CATCH() }
FLY_API void FlyNative_UISetVisible(const char* id, short visible) { FLY_TRY { if (id) gameui::SetVisible(id, visible != 0); } FLY_CATCH() }
FLY_API void FlyNative_UIRemove(const char* id) { FLY_TRY { if (id) gameui::Remove(id); } FLY_CATCH() }
FLY_API void FlyNative_UISetFont(const char* id, const char* font) { FLY_TRY { if (id) gameui::SetFont(id, font ? font : ""); } FLY_CATCH() }

FLY_API short FlyNative_UIExists(const char* id) {
    FLY_TRY { gameui::Info i; return id && gameui::Get(id, i) ? 1 : 0; } FLY_CATCH(return 0;)
}
FLY_API float FlyNative_UIGetValue(const char* id) {
    FLY_TRY { gameui::Info i; return id && gameui::Get(id, i) ? i.value : 0.0f; } FLY_CATCH(return 0.0f;)
}
FLY_API int FlyNative_UIGetText(const char* id, char* out, int capacity) {
    FLY_TRY {
        gameui::Info i;
        if (out && capacity > 0) out[0] = '\0';
        if (!id || !gameui::Get(id, i)) return -1;
        if (out && capacity > 0) { std::strncpy(out, i.text.c_str(), static_cast<size_t>(capacity) - 1); out[capacity - 1] = '\0'; }
        return static_cast<int>(i.text.size());
    } FLY_CATCH(return -1;)
}
FLY_API short FlyNative_UIGetVisible(const char* id) {
    FLY_TRY { gameui::Info i; return id && gameui::Get(id, i) && i.visible ? 1 : 0; } FLY_CATCH(return 0;)
}
FLY_API short FlyNative_UIGetLayout(const char* id, float* out6) {
    FLY_TRY {
        gameui::Info i;
        if (!id || !out6 || !gameui::Get(id, i)) return 0;
        out6[0] = static_cast<float>(i.anchor); out6[1] = i.x; out6[2] = i.y; out6[3] = i.w; out6[4] = i.h; out6[5] = i.fontSize;
        return 1;
    } FLY_CATCH(return 0;)
}
FLY_API short FlyNative_UIGetColor(const char* id, unsigned char* out8) {
    FLY_TRY {
        gameui::Info i;
        if (!id || !out8 || !gameui::Get(id, i)) return 0;
        out8[0] = i.color.r; out8[1] = i.color.g; out8[2] = i.color.b; out8[3] = i.color.a;
        out8[4] = i.color2.r; out8[5] = i.color2.g; out8[6] = i.color2.b; out8[7] = i.color2.a;
        return 1;
    } FLY_CATCH(return 0;)
}

FLY_API void FlyNative_GetMouseDelta(float* out2) {
    FLY_TRY {
        if (!out2) return;
        const Vector2 d = GetMouseDelta();
        out2[0] = d.x; out2[1] = d.y;
    } FLY_CATCH()
}

// Debug lines: one text widget per key, stacked down the top-left corner in the order the keys first appeared.
FLY_API void FlyNative_DebugText(const char* key, const char* text) {
    FLY_TRY {
        if (!key || !*key) return;
        static std::vector<std::string> rows;
        const std::string id = std::string("dbg_") + key;
        auto it = std::find(rows.begin(), rows.end(), key);
        if (!text || !*text) {
            if (it != rows.end()) { gameui::Remove(id); rows.erase(it); }
            return;
        }
        if (it == rows.end()) { rows.push_back(key); it = rows.end() - 1; }
        const float y = 8.0f + 18.0f * static_cast<float>(it - rows.begin());
        gameui::Create(id, gameui::Kind::Text);
        gameui::SetLayout(id, gameui::Anchor::TopLeft, 8.0f, y, 600.0f, 18.0f);
        gameui::SetPrimaryColor(id, Color{ 255, 235, 90, 255 });
        gameui::SetText(id, text, 14.0f);
    } FLY_CATCH()
}

// ---- navigation ----

namespace {

// The first part of the level a ray hits. Objects that can move (the player, crates, projectiles) are looked through:
// they are not part of the floor plan.
bool LevelRay(phys::Simulation* sim, Vector3 origin, Vector3 dir, float maxDistance, Vector3& point, Vector3& normal) {
    float left = maxDistance;
    for (int i = 0; i < 12 && left > 0.0f; ++i) {
        const phys::RaycastHit hit = sim->RayCast(origin, dir, left);
        if (!hit.hit) return false;
        if (!hit.object || sim->IsStaticBody(hit.object)) { point = hit.point; normal = hit.normal; return true; }
        const float advance = hit.fraction * left + 0.01f;
        origin = Vector3Add(origin, Vector3Scale(dir, advance));
        left -= advance;
    }
    return false;
}

} // namespace

FLY_API short FlyNative_NavBuild(const float* in) {
    FLY_TRY {
        ScriptRuntime* rt = ActiveRuntime();
        if (!in || !rt || !rt->GetSimulation()) return 0;
        nav::Params p;
        p.min = { in[0], in[1], in[2] };
        p.max = { in[3], in[4], in[5] };
        p.cellSize = in[6]; p.agentRadius = in[7]; p.agentHeight = in[8]; p.maxStep = in[9]; p.maxSlope = in[10];
        phys::Simulation* sim = rt->GetSimulation();
        nav::Active& world = nav::World();
        world.lastPath.clear();
        const bool ok = world.mesh.Build(p, [sim](Vector3 o, Vector3 d, float m, Vector3& pt, Vector3& n) { return LevelRay(sim, o, d, m, pt, n); });
        if (!ok) std::fprintf(stderr, "[nav] could not bake a navigation mesh (check the box, cell size and that the level has floors)\n");
        else std::fprintf(stderr, "[nav] baked %zu cells (%d x %d columns)\n", world.mesh.CellCount(), world.mesh.ColumnsX(), world.mesh.ColumnsZ());
        return ok ? 1 : 0;
    } FLY_CATCH(return 0;)
}
FLY_API void FlyNative_NavClear(void) { FLY_TRY { nav::ResetWorld(); } FLY_CATCH() }
FLY_API short FlyNative_NavReady(void) { FLY_TRY { return nav::World().mesh.Built() ? 1 : 0; } FLY_CATCH(return 0;) }
FLY_API int FlyNative_NavFindPath(const float* from3, const float* to3, float* out, int maxPoints, short allowPartial) {
    FLY_TRY {
        if (!from3 || !to3) return 0;
        nav::Active& world = nav::World();
        std::vector<Vector3> path;
        const nav::PathResult r = world.mesh.FindPath({ from3[0], from3[1], from3[2] }, { to3[0], to3[1], to3[2] }, path, allowPartial != 0);
        world.lastPath = path;
        if (r == nav::PathResult::None) return 0;
        const int n = out && maxPoints > 0 ? std::min(static_cast<int>(path.size()), maxPoints) : static_cast<int>(path.size());
        for (int i = 0; out && i < n; ++i) { out[i * 3] = path[i].x; out[i * 3 + 1] = path[i].y; out[i * 3 + 2] = path[i].z; }
        return r == nav::PathResult::Partial ? -n : n;
    } FLY_CATCH(return 0;)
}
FLY_API short FlyNative_NavNearest(const float* in3, float* out3) {
    FLY_TRY {
        Vector3 p{};
        if (!in3 || !out3 || !nav::World().mesh.Nearest({ in3[0], in3[1], in3[2] }, p)) return 0;
        out3[0] = p.x; out3[1] = p.y; out3[2] = p.z;
        return 1;
    } FLY_CATCH(return 0;)
}
FLY_API short FlyNative_NavIsWalkable(const float* in3) {
    FLY_TRY { return in3 && nav::World().mesh.IsWalkable({ in3[0], in3[1], in3[2] }) ? 1 : 0; } FLY_CATCH(return 0;)
}
FLY_API short FlyNative_NavRandomPoint(const float* center3, float radius, float* out3) {
    FLY_TRY {
        Vector3 p{};
        if (!center3 || !out3 || !nav::World().mesh.RandomPoint({ center3[0], center3[1], center3[2] }, radius, p)) return 0;
        out3[0] = p.x; out3[1] = p.y; out3[2] = p.z;
        return 1;
    } FLY_CATCH(return 0;)
}
FLY_API void FlyNative_NavDebug(short on) { FLY_TRY { nav::World().debug = on != 0; } FLY_CATCH() }
FLY_API short FlyNative_NavLineClear(const float* from3, const float* to3) {
    FLY_TRY {
        return from3 && to3 && nav::World().mesh.LineClear({ from3[0], from3[1], from3[2] }, { to3[0], to3[1], to3[2] }) ? 1 : 0;
    } FLY_CATCH(return 0;)
}
FLY_API short FlyNative_NavMoveAlong(const float* from3, const float* delta3, float* out3) {
    FLY_TRY {
        if (!from3 || !delta3 || !out3) return 0;
        Vector3 p{};
        const bool ok = nav::World().mesh.MoveAlong({ from3[0], from3[1], from3[2] }, { delta3[0], delta3[1], delta3[2] }, p);
        out3[0] = p.x; out3[1] = p.y; out3[2] = p.z;
        return ok ? 1 : 0;
    } FLY_CATCH(return 0;)
}

// ---- mouse and clickable UI ----

FLY_API int FlyNative_UIGetState(const char* id) { FLY_TRY { return id ? gameui::StateOf(id) : 0; } FLY_CATCH(return 0;) }
FLY_API void FlyNative_GetMousePosition(float* out2) {
    FLY_TRY {
        if (!out2) return;
        const Vector2 p = gameui::PointerInUiPixels();
        out2[0] = p.x; out2[1] = p.y;
    } FLY_CATCH()
}
FLY_API void FlyNative_SetMouseFree(short free) { FLY_TRY { gameui::SetMouseFree(free != 0); } FLY_CATCH() }
FLY_API short FlyNative_MouseOverUI(void) { FLY_TRY { return gameui::PointerOverWidget() ? 1 : 0; } FLY_CATCH(return 0;) }

// FindObject and GetSelf
FLY_API unsigned long long FlyNative_FindObject(const char* name) {
    FLY_TRY { return impl::FlyNative_FindObject_Impl(name); } FLY_CATCH(return 0;)
}
FLY_API unsigned long long FlyNative_GetSelf(void) {
    FLY_TRY { return impl::FlyNative_GetSelf_Impl(); } FLY_CATCH(return 0;)
}
FLY_API void FlyNative_BindSelf(unsigned long long handle) {
    FLY_TRY { impl::FlyNative_BindSelf_Impl(handle); } FLY_CATCH()
}

} // extern "C"