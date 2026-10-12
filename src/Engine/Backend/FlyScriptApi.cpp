// FlyScriptApi.cpp - C API bridge between the engine and C++ game scripts.
// Provides the FlyNative_* functions for object manipulation, world services, and script control.

#include "../include/Engine/Scripts/FlyScriptApi.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "../../../include/Engine/Scripts/NativeScriptHost.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Graphics.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
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
    auto* obj = Resolve(h);
    if (!obj) return 0;
    Vector3 v{ x, y, z };
    if (ScriptRuntime* rt = ActiveRuntime()) if (rt->IsSimPlaying()) rt->SetBodyPosition(obj, v);
    *obj->GetPosPtr() = v;
    return 1;
}
short FlyNative_SetSize_Impl(unsigned long long h, float x, float y, float z) {
    auto* obj = Resolve(h);
    if (!obj) return 0;
    *obj->GetSizePtr() = Vector3{ x, y, z };
    obj->ResetMassAuto();
    return 1;
}
short FlyNative_SetRotation_Impl(unsigned long long h, float x, float y, float z) {
    auto* obj = Resolve(h);
    if (!obj) return 0;
    Vector3 v{ x, y, z };
    if (ScriptRuntime* rt = ActiveRuntime()) if (rt->IsSimPlaying()) rt->SetBodyOrientation(obj, v);
    *obj->GetRotationPtr() = v;
    return 1;
}
short FlyNative_SetOrigin_Impl(unsigned long long h, float x, float y, float z) {
    auto* obj = Resolve(h);
    if (!obj) return 0;
    *obj->GetOriginPtr() = Vector3{ x, y, z };
    return 1;
}
short FlyNative_SetVelocity_Impl(unsigned long long h, float x, float y, float z) {
    auto* obj = Resolve(h);
    if (!obj) return 0;
    Vector3 v{ x, y, z };
    if (ScriptRuntime* rt = ActiveRuntime()) if (rt->IsSimPlaying()) rt->SetBodyVelocity(obj, v);
    obj->SetVelocity(v);
    return 1;
}
short FlyNative_SetAngularVelocity_Impl(unsigned long long h, float x, float y, float z) {
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
    if (auto* obj = Resolve(h)) obj->anchored = v != 0;
}

short FlyNative_GetCanCollide_Impl(unsigned long long h) {
    auto* obj = Resolve(h);
    return obj ? obj->canCollide : 0;
}
void FlyNative_SetCanCollide_Impl(unsigned long long h, short v) {
    if (auto* obj = Resolve(h)) obj->canCollide = v != 0;
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
float FlyNative_GetAmbient_Impl(float* intensity) { *intensity = gfx::GetAmbientIntensity(); return *intensity; }
void  FlyNative_SetAmbient_Impl(float a) { if (a < 0.0f) a = 0.0f; if (a > 2.0f) a = 2.0f; gfx::SetAmbientIntensity(a); }
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
    if (!(v == v)) return;      // not a number
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
    if (rt && fov) *fov = rt->GetCamera().fovy;
    return fov ? *fov : 45.0f;
}
void FlyNative_SetFov_Impl(float f) {
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