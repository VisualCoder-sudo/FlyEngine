/*
 * FlyScriptABI.h - the C ABI between the engine and a compiled script module.
 *
 * The .cpp files in a project's Scripts/ folder are compiled by the editor into one shared
 * library (.so / .dll). Only plain C types cross this boundary, so a module
 * built with any compiler (clang, g++, MinGW, MSVC) works with an engine built
 * by any other. C++ exceptions, allocations and coroutine frames never leave
 * the module.
 *
 *   engine -> module : FlyScript_GetModule(const FlyApi*) once after loading
 *   module -> engine : calls through the FlyApi function table
 *
 * Script authors do not use this header directly; include "fly.hpp".
 */

#ifndef FLY_SCRIPT_ABI_H
#define FLY_SCRIPT_ABI_H

#ifdef __cplusplus
#include <cstdint>
#else
#include <stdint.h>
#endif

#define FLY_SCRIPT_ABI_VERSION 1u

#if defined(_WIN32)
    #define FLY_SCRIPT_EXPORT __declspec(dllexport)
#else
    #define FLY_SCRIPT_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long long FlyHandle; /* 0 = none */

/* Engine services, filled in by the engine. Mirrors the FlyNative_* set in
 * include/Engine/Scripts/FlyScriptApi.hpp. Append only; never reorder. */
typedef struct FlyApi {
    uint32_t abi_version;
    uint32_t size; /* sizeof(FlyApi) as built by the engine */

    /* objects */
    FlyHandle (*FindObject)(const char* name);
    FlyHandle (*GetSelf)(void);
    FlyHandle (*CreateObject)(const char* shapeName);

    short (*GetPosition)(FlyHandle h, float* x, float* y, float* z);
    short (*SetPosition)(FlyHandle h, float x, float y, float z);
    short (*GetSize)(FlyHandle h, float* x, float* y, float* z);
    short (*SetSize)(FlyHandle h, float x, float y, float z);
    short (*GetRotation)(FlyHandle h, float* x, float* y, float* z);
    short (*SetRotation)(FlyHandle h, float x, float y, float z);
    short (*GetOrigin)(FlyHandle h, float* x, float* y, float* z);
    short (*SetOrigin)(FlyHandle h, float x, float y, float z);
    short (*GetVelocity)(FlyHandle h, float* x, float* y, float* z);
    short (*SetVelocity)(FlyHandle h, float x, float y, float z);
    short (*GetAngularVelocity)(FlyHandle h, float* x, float* y, float* z);
    short (*SetAngularVelocity)(FlyHandle h, float x, float y, float z);

    short (*GetColor)(FlyHandle h, unsigned char* r, unsigned char* g, unsigned char* b);
    short (*SetColor)(FlyHandle h, unsigned char r, unsigned char g, unsigned char b);

    short (*GetAnchored)(FlyHandle h);
    void  (*SetAnchored)(FlyHandle h, short value);
    short (*GetCanCollide)(FlyHandle h);
    void  (*SetCanCollide)(FlyHandle h, short value);
    float (*GetMass)(FlyHandle h);
    void  (*SetMass)(FlyHandle h, float mass);
    float (*GetTransparency)(FlyHandle h);
    void  (*SetTransparency)(FlyHandle h, float t);
    int   (*GetCollisionAccuracy)(FlyHandle h);
    void  (*SetCollisionAccuracy)(FlyHandle h, int accuracy);

    /* world */
    short (*IsPlaying)(void);
    short (*GetShadowsEnabled)(void);
    void  (*SetShadowsEnabled)(short on);
    int   (*GetShadowQuality)(void);
    void  (*SetShadowQuality)(int q);
    float (*GetAmbient)(float* intensity);
    void  (*SetAmbient)(float intensity);
    short (*GetGridVisible)(void);
    void  (*SetGridVisible)(short on);
    short (*GetWireframe)(void);
    void  (*SetWireframe)(short on);
    float (*GetFov)(float* fov);
    void  (*SetFov)(float fov);

    /* physics */
    float (*GetGravity)(void);
    void  (*SetGravity)(float g);
    float (*GetFriction)(void);
    void  (*SetFriction)(float f);
    float (*GetRestitution)(void);
    void  (*SetRestitution)(float r);

    /* misc */
    void  (*Print)(const char* text);
} FlyApi;

/* One script class compiled into the module. Instances are opaque to the
 * engine. tick() advances the script's coroutine by one frame and returns 0
 * once the script has finished (or failed). */
typedef struct FlyScriptClass {
    const char* name;
    void* context; /* module-private */
    void* (*create)(const struct FlyScriptClass* cls);
    int   (*tick)(void* instance, float dt);
    void  (*destroy)(void* instance);
} FlyScriptClass;

typedef struct FlyScriptModule {
    uint32_t abi_version;
    int class_count;
    const FlyScriptClass* classes;
} FlyScriptModule;

/* Exported by every script module (defined once by fly.hpp). */
typedef const FlyScriptModule* (*FlyScript_GetModuleFn)(const FlyApi* api);
#define FLY_SCRIPT_ENTRY_NAME "FlyScript_GetModule"

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FLY_SCRIPT_ABI_H */
