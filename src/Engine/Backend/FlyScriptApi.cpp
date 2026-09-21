// FlyScriptApi.cpp — C API bridge between C++ engine and C# scripting runtime.
// Provides 55 FlyNative_* functions for object manipulation, world services, and script control.

#include "../include/Engine/Scripts/FlyScriptApi.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Graphics.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <excpt.h>

namespace {

// The C++ world context the managed host binds each frame via
// FlyNative_BindRuntime. Set at bind time and cleared at shutdown; when null,
// every world-touching FlyNative_* call no-ops (scripting disabled).
ScriptRuntime* ActiveRuntime() {
    return GetActiveRuntime();
}

// Maps an id64 handle to a live ScatteredObject*. Returns nullptr if the
// handle is 0 or no longer refers to an object in the scene.
ScatteredObject* Resolve(unsigned long long handle) {
    if (handle == 0) return nullptr;
    ScriptRuntime* rt = ActiveRuntime();
    if (!rt) return nullptr;

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

// ---- script control (placeholders - C# side handles these) ----

unsigned long long FlyNative_StartObjectScript_Impl(unsigned long long objectHandle, const char* typeName) {
    (void)objectHandle; (void)typeName; return 0;
}
void FlyNative_StopObjectScript_Impl(unsigned long long objectHandle) { (void)objectHandle; }
short FlyNative_IsObjectScriptRunning_Impl(unsigned long long objectHandle) { (void)objectHandle; return 0; }
unsigned long long FlyNative_StartStandaloneScript_Impl(int index, const char* typeName) { (void)index; (void)typeName; return 0; }
void FlyNative_StopStandaloneScript_Impl(int index) { (void)index; }
short FlyNative_IsStandaloneScriptRunning_Impl(int index) { (void)index; return 0; }
void FlyNative_ClearAllScripts_Impl(void) { }

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
    __declspec(dllexport) short FlyNative_Get##PropName(unsigned long long h, float* x, float* y, float* z) { \
        __try { return impl::FlyNative_Get##PropName##_Impl(h, x, y, z); } \
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } \
    }

WRAP_VEC3_GET(Position)
WRAP_VEC3_GET(Size)
WRAP_VEC3_GET(Rotation)
WRAP_VEC3_GET(Origin)
WRAP_VEC3_GET(Velocity)
WRAP_VEC3_GET(AngularVelocity)

// Vec3 setters
#define WRAP_VEC3_SET(PropName) \
    __declspec(dllexport) short FlyNative_Set##PropName(unsigned long long h, float x, float y, float z) { \
        __try { return impl::FlyNative_Set##PropName##_Impl(h, x, y, z); } \
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } \
    }

WRAP_VEC3_SET(Position)
WRAP_VEC3_SET(Size)
WRAP_VEC3_SET(Rotation)
WRAP_VEC3_SET(Origin)
WRAP_VEC3_SET(Velocity)
WRAP_VEC3_SET(AngularVelocity)

// Color
__declspec(dllexport) short FlyNative_GetColor(unsigned long long h, unsigned char* r, unsigned char* g, unsigned char* b) {
    __try { return impl::FlyNative_GetColor_Impl(h, r, g, b); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
__declspec(dllexport) short FlyNative_SetColor(unsigned long long h, unsigned char r, unsigned char g, unsigned char b) {
    __try { return impl::FlyNative_SetColor_Impl(h, r, g, b); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Booleans/scalars
#define WRAP_SCALAR_GET(PropName) \
    __declspec(dllexport) short FlyNative_Get##PropName(unsigned long long h) { \
        __try { return impl::FlyNative_Get##PropName##_Impl(h); } \
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } \
    }
#define WRAP_SCALAR_SET(PropName) \
    __declspec(dllexport) void FlyNative_Set##PropName(unsigned long long h, short v) { \
        __try { impl::FlyNative_Set##PropName##_Impl(h, v); } __except (EXCEPTION_EXECUTE_HANDLER) { } \
    }

WRAP_SCALAR_GET(Anchored)
WRAP_SCALAR_SET(Anchored)
WRAP_SCALAR_GET(CanCollide)
WRAP_SCALAR_SET(CanCollide)

__declspec(dllexport) float FlyNative_GetMass(unsigned long long h) {
    __try { return impl::FlyNative_GetMass_Impl(h); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
}
__declspec(dllexport) void FlyNative_SetMass(unsigned long long h, float m) {
    __try { impl::FlyNative_SetMass_Impl(h, m); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

__declspec(dllexport) float FlyNative_GetTransparency(unsigned long long h) {
    __try { return impl::FlyNative_GetTransparency_Impl(h); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
}
__declspec(dllexport) void FlyNative_SetTransparency(unsigned long long h, float t) {
    __try { impl::FlyNative_SetTransparency_Impl(h, t); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

__declspec(dllexport) int FlyNative_GetCollisionAccuracy(unsigned long long h) {
    __try { return impl::FlyNative_GetCollisionAccuracy_Impl(h); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
__declspec(dllexport) void FlyNative_SetCollisionAccuracy(unsigned long long h, int a) {
    __try { impl::FlyNative_SetCollisionAccuracy_Impl(h, a); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// World services
__declspec(dllexport) short FlyNative_IsPlaying(void) { __try { return impl::FlyNative_IsPlaying_Impl(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

__declspec(dllexport) short FlyNative_GetShadowsEnabled(void) { __try { return impl::FlyNative_GetShadowsEnabled_Impl(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
__declspec(dllexport) void  FlyNative_SetShadowsEnabled(short on) { __try { impl::FlyNative_SetShadowsEnabled_Impl(on); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
__declspec(dllexport) int   FlyNative_GetShadowQuality(void) { __try { return impl::FlyNative_GetShadowQuality_Impl(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
__declspec(dllexport) void  FlyNative_SetShadowQuality(int q) { __try { impl::FlyNative_SetShadowQuality_Impl(q); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
__declspec(dllexport) float FlyNative_GetAmbient(float* intensity) { __try { return impl::FlyNative_GetAmbient_Impl(intensity); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; } }
__declspec(dllexport) void  FlyNative_SetAmbient(float a) { __try { impl::FlyNative_SetAmbient_Impl(a); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
__declspec(dllexport) short FlyNative_GetGridVisible(void) { __try { return impl::FlyNative_GetGridVisible_Impl(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
__declspec(dllexport) void  FlyNative_SetGridVisible(short on) { __try { impl::FlyNative_SetGridVisible_Impl(on); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
__declspec(dllexport) short FlyNative_GetWireframe(void) { __try { return impl::FlyNative_GetWireframe_Impl(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
__declspec(dllexport) void  FlyNative_SetWireframe(short on) { __try { impl::FlyNative_SetWireframe_Impl(on); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
__declspec(dllexport) float FlyNative_GetFov(float* fov) { __try { return impl::FlyNative_GetFov_Impl(fov); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; } }
__declspec(dllexport) void FlyNative_SetFov(float f) { __try { impl::FlyNative_SetFov_Impl(f); } __except (EXCEPTION_EXECUTE_HANDLER) { } }

__declspec(dllexport) float FlyNative_GetGravity(void) { __try { return impl::FlyNative_GetGravity_Impl(); } __except (EXCEPTION_EXECUTE_HANDLER) { return -19.62f; } }
__declspec(dllexport) void FlyNative_SetGravity(float g) { __try { impl::FlyNative_SetGravity_Impl(g); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
__declspec(dllexport) float FlyNative_GetFriction(void) { __try { return impl::FlyNative_GetFriction_Impl(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.4f; } }
__declspec(dllexport) void FlyNative_SetFriction(float f) { __try { impl::FlyNative_SetFriction_Impl(f); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
__declspec(dllexport) float FlyNative_GetRestitution(void) { __try { return impl::FlyNative_GetRestitution_Impl(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.7f; } }
__declspec(dllexport) void FlyNative_SetRestitution(float r) { __try { impl::FlyNative_SetRestitution_Impl(r); } __except (EXCEPTION_EXECUTE_HANDLER) { } }

// ---- create ----

__declspec(dllexport) unsigned long long FlyNative_CreateObject(const char* shapeName) {
    __try { return impl::FlyNative_CreateObject_Impl(shapeName); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ---- script control (forwarded to C# ScriptHost) ----

__declspec(dllexport) unsigned long long FlyNative_StartObjectScript(unsigned long long objectHandle, const char* typeName) {
    __try { return impl::FlyNative_StartObjectScript_Impl(objectHandle, typeName); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

__declspec(dllexport) void FlyNative_StopObjectScript(unsigned long long objectHandle) {
    __try { impl::FlyNative_StopObjectScript_Impl(objectHandle); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
__declspec(dllexport) short FlyNative_IsObjectScriptRunning(unsigned long long objectHandle) { __try { return impl::FlyNative_IsObjectScriptRunning_Impl(objectHandle); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

__declspec(dllexport) unsigned long long FlyNative_StartStandaloneScript(int index, const char* typeName) {
    __try { return impl::FlyNative_StartStandaloneScript_Impl(index, typeName); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
__declspec(dllexport) void FlyNative_StopStandaloneScript(int index) {
    __try { impl::FlyNative_StopStandaloneScript_Impl(index); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
__declspec(dllexport) short FlyNative_IsStandaloneScriptRunning(int index) { __try { return impl::FlyNative_IsStandaloneScriptRunning_Impl(index); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

__declspec(dllexport) void FlyNative_ClearAllScripts(void) {
    __try { impl::FlyNative_ClearAllScripts_Impl(); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// ---- runtime binding ----

__declspec(dllexport) void FlyNative_BindRuntime(void* hostPtr) {
    __try { impl::FlyNative_BindRuntime_Impl(hostPtr); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// ---- print ----

__declspec(dllexport) void FlyNative_Print(const char* text) {
    __try { impl::FlyNative_Print_Impl(text); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// FindObject and GetSelf
__declspec(dllexport) unsigned long long FlyNative_FindObject(const char* name) {
    __try { return impl::FlyNative_FindObject_Impl(name); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
__declspec(dllexport) unsigned long long FlyNative_GetSelf(void) {
    __try { return impl::FlyNative_GetSelf_Impl(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
__declspec(dllexport) void FlyNative_BindSelf(unsigned long long handle) {
    __try { impl::FlyNative_BindSelf_Impl(handle); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

} // extern "C"