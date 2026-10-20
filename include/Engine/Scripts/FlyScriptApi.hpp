#pragma once

// Scripting C API.
//
// The engine-side functions game scripts use to read and mutate the world.
// Script libraries do not link against these directly: NativeScriptHost hands
// them over as the FlyApi function table (ScriptingSDK/include/FlyScriptABI.h),
// which the C++ SDK (fly.hpp) wraps. They are also exported from the engine
// executable for native plugins.
//
// Conventions
// -----------
// * Object handles are id64 (uint64_t). They are opaque to callers. The C++
//   side validates that a handle currently refers to a live scene object via
//   the Runtime registry, so stale handles fail instead of crashing.
// * Every vector is passed as 3 out-floats (&x, &y, &z); every color as 4
//   bytes / 5-float form, matching how Flyscript's rgb()/tuple values worked.
// * Functions return bool on success and false (with an optional out-error
//   buffer) on failure.
// * A handle of 0 means "none".
//
// Property names follow the Flyscript object-property vocabulary:
//   Position Size Rotation Origin Color Velocity AngularVelocity
//   Anchored CanCollide CollisionAccuracy Mass Transparency
//   (plus the model-part fields mirorred below.)

// Supplies FLY_API (export decoration) and the FLY_TRY/FLY_CATCH SEH guard.
// Must come before the extern "C" block: Platform.hpp pulls in <string> and
// <vector>, which are C++ headers.
#include "../Platform/Platform.hpp"

#ifdef __cplusplus
extern "C" {
#endif

// ---- Object registry / handles (id64) ----

// Finds a scene object by display name. Returns an id64 handle (0 = not found).
// `modelMember`: if non-null, searches model parts too (model name "Model.Part").
FLY_API unsigned long long FlyNative_FindObject(const char* name);

// The object the currently-running script is attached to (self). 0 if none.
FLY_API unsigned long long FlyNative_GetSelf(void);

// ---- Object properties ----

// vec3 getters/setters
FLY_API short FlyNative_GetPosition(unsigned long long h, float* x, float* y, float* z);
FLY_API short FlyNative_SetPosition(unsigned long long h, float x, float y, float z);
FLY_API short FlyNative_GetSize(unsigned long long h, float* x, float* y, float* z);
FLY_API short FlyNative_SetSize(unsigned long long h, float x, float y, float z);
FLY_API short FlyNative_GetRotation(unsigned long long h, float* x, float* y, float* z);
FLY_API short FlyNative_SetRotation(unsigned long long h, float x, float y, float z);
FLY_API short FlyNative_GetOrigin(unsigned long long h, float* x, float* y, float* z);
FLY_API short FlyNative_SetOrigin(unsigned long long h, float x, float y, float z);
FLY_API short FlyNative_GetVelocity(unsigned long long h, float* x, float* y, float* z);
FLY_API short FlyNative_SetVelocity(unsigned long long h, float x, float y, float z);
FLY_API short FlyNative_GetAngularVelocity(unsigned long long h, float* x, float* y, float* z);
FLY_API short FlyNative_SetAngularVelocity(unsigned long long h, float x, float y, float z);

// color getter/setter (r,g,b 0..255)
FLY_API short FlyNative_GetColor(unsigned long long h, unsigned char* r, unsigned char* g, unsigned char* b);
FLY_API short FlyNative_SetColor(unsigned long long h, unsigned char r, unsigned char g, unsigned char b);

// scalar/boolean properties
FLY_API short FlyNative_GetAnchored(unsigned long long h);
FLY_API void  FlyNative_SetAnchored(unsigned long long h, short value);
FLY_API short FlyNative_GetCanCollide(unsigned long long h);
FLY_API void  FlyNative_SetCanCollide(unsigned long long h, short value);
FLY_API float FlyNative_GetMass(unsigned long long h);
FLY_API void  FlyNative_SetMass(unsigned long long h, float mass);
FLY_API float FlyNative_GetTransparency(unsigned long long h);
FLY_API void  FlyNative_SetTransparency(unsigned long long h, float t);

// CollisionAccuracy is an enum = Box(0) Hull(1) Default(2) Precise(3).
// Returns -1 on invalid handle.
FLY_API int FlyNative_GetCollisionAccuracy(unsigned long long h);
FLY_API void FlyNative_SetCollisionAccuracy(unsigned long long h, int accuracy);

// ---- World services (the old game.* properties) ----

FLY_API short FlyNative_IsPlaying(void);

FLY_API short FlyNative_GetShadowsEnabled(void);
FLY_API void  FlyNative_SetShadowsEnabled(short on);
FLY_API int   FlyNative_GetShadowQuality(void);
FLY_API void  FlyNative_SetShadowQuality(int q);
FLY_API float FlyNative_GetAmbient(float* intensity); // returns 1.0 (single value); prop is scalar
FLY_API void  FlyNative_SetAmbient(float intensity);
FLY_API short FlyNative_GetGridVisible(void);
FLY_API void  FlyNative_SetGridVisible(short on);
FLY_API short FlyNative_GetWireframe(void);
FLY_API void  FlyNative_SetWireframe(short on);
FLY_API float FlyNative_GetFov(float* fov);
FLY_API void  FlyNative_SetFov(float fov);

// Environment (game.Environment.*): the scene's time, weather, sky and picture. `what` is one of
// the FLY_ENV_* numbers of ScriptingSDK/include/FlyScriptABI.h. Setting a value inserts the Lighting
// item it belongs to.
FLY_API float FlyNative_GetEnvironment(int what);
FLY_API void  FlyNative_SetEnvironment(int what, float value);

// Visual Script Code Snippet nodes: values wired into / out of the running snippet class.
FLY_API const char* FlyNative_SnippetGetInput(const char* snippet, int pin);
FLY_API void FlyNative_SnippetSetOutput(const char* snippet, int pin, const char* value);

// Everything the Visual Script nodes do (see FlyApi in FlyScriptABI.h).
FLY_API void FlyNative_DestroyObject(unsigned long long h);
FLY_API unsigned long long FlyNative_CloneObject(unsigned long long h);
FLY_API const char* FlyNative_GetName(unsigned long long h);
FLY_API void FlyNative_SetName(unsigned long long h, const char* name);
FLY_API short FlyNative_AddForce(unsigned long long h, float x, float y, float z);
FLY_API short FlyNative_AddImpulse(unsigned long long h, float x, float y, float z);
FLY_API const char* FlyNative_Raycast(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistance);
FLY_API short FlyNative_KeyDown(const char* keyName);
FLY_API short FlyNative_KeyPressed(const char* keyName);
FLY_API void FlyNative_PlaySound(const char* file, float volume);
FLY_API void FlyNative_SetMasterVolume(float volume);
FLY_API void FlyNative_ShowText(const char* text, const char* position, float seconds);
FLY_API float FlyNative_TimeSincePlay(void);
FLY_API short FlyNative_GamepadAvailable(int gamepad);
FLY_API short FlyNative_GamepadButtonDown(int gamepad, const char* buttonName);
FLY_API short FlyNative_GamepadButtonPressed(int gamepad, const char* buttonName);
FLY_API float FlyNative_GamepadAxis(int gamepad, const char* axisName);

// Gameplay queries and effects (see FlyRayHit / FlyTouch and friends in FlyScriptABI.h).
FLY_API short FlyNative_RaycastEx(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistance, struct FlyRayHit* out);
FLY_API int   FlyNative_OverlapSphere(float cx, float cy, float cz, float radius, unsigned long long* out, int maxCount);
FLY_API int   FlyNative_PollTouches(struct FlyTouch* out, int maxCount);
FLY_API unsigned FlyNative_CreateJoint(int kind, unsigned long long a, unsigned long long b, float ax, float ay, float az, float bx, float by, float bz);
FLY_API void  FlyNative_DestroyJoint(unsigned joint);
FLY_API void  FlyNative_GetCamera(float* out7);
FLY_API void  FlyNative_SetCamera(const float* in7);
FLY_API void  FlyNative_EmitParticles(const char* preset, float x, float y, float z, float dx, float dy, float dz, int count, float scale);
FLY_API unsigned FlyNative_StartEmitter(const char* preset, float x, float y, float z, float dx, float dy, float dz, float scale);
FLY_API void  FlyNative_MoveEmitter(unsigned emitter, float x, float y, float z, float dx, float dy, float dz);
FLY_API void  FlyNative_StopEmitter(unsigned emitter);
FLY_API unsigned long long FlyNative_SpawnPrefab(const char* file, float x, float y, float z);
FLY_API short FlyNative_UICreate(const char* id, int kind);
FLY_API void  FlyNative_UISetLayout(const char* id, int anchor, float x, float y, float w, float h);
FLY_API void  FlyNative_UISetColor(const char* id, unsigned char r, unsigned char g, unsigned char b, unsigned char a, unsigned char r2, unsigned char g2, unsigned char b2, unsigned char a2);
FLY_API void  FlyNative_UISetText(const char* id, const char* text, float fontSize);
FLY_API void  FlyNative_UISetValue(const char* id, float value);
FLY_API void  FlyNative_UISetImage(const char* id, const char* file);
FLY_API void  FlyNative_UISetVisible(const char* id, short visible);
FLY_API void  FlyNative_UIRemove(const char* id);
FLY_API void  FlyNative_UISetFont(const char* id, const char* font);
FLY_API short FlyNative_UIExists(const char* id);
FLY_API float FlyNative_UIGetValue(const char* id);
FLY_API int   FlyNative_UIGetText(const char* id, char* out, int capacity);
FLY_API short FlyNative_UIGetVisible(const char* id);
FLY_API short FlyNative_UIGetLayout(const char* id, float* out6);
FLY_API short FlyNative_UIGetColor(const char* id, unsigned char* out8);
FLY_API void  FlyNative_GetMouseDelta(float* out2);
FLY_API void  FlyNative_DebugText(const char* key, const char* text);
FLY_API void  FlyNative_SpawnDecal(float x, float y, float z, float nx, float ny, float nz, float size, unsigned char r, unsigned char g, unsigned char b, unsigned char a, float seconds);

// Physics (game.Physics.* - routed to the simulation).
FLY_API float FlyNative_GetGravity(void);
FLY_API void  FlyNative_SetGravity(float g);
FLY_API float FlyNative_GetFriction(void);
FLY_API void  FlyNative_SetFriction(float f);
FLY_API float FlyNative_GetRestitution(void);
FLY_API void  FlyNative_SetRestitution(float r);

// ---- Object creation / destruction ----

// Creates a primitive object (Cube, Sphere, Cylinder, Wedge). Returns a handle
// or 0 on failure. The object is spawned into the simulation when playing.
FLY_API unsigned long long FlyNative_CreateObject(const char* shapeName);

// ---- Scripting helpers ----

// Prints a line to the engine log.
FLY_API void FlyNative_Print(const char* text);

// ---- Script lifecycle (forwarded to the NativeScriptHost) ----
// Returns script ID (>0) or 0 on failure.
FLY_API unsigned long long FlyNative_StartObjectScript(unsigned long long objectHandle, const char* typeName);
FLY_API void FlyNative_StopObjectScript(unsigned long long objectHandle);
FLY_API short FlyNative_IsObjectScriptRunning(unsigned long long objectHandle);

FLY_API unsigned long long FlyNative_StartStandaloneScript(int index, const char* typeName);
FLY_API void FlyNative_StopStandaloneScript(int index);
FLY_API short FlyNative_IsStandaloneScriptRunning(int index);

// Clear all scripts (called when play mode stops).
FLY_API void FlyNative_ClearAllScripts(void);

// ---- Runtime binding ----

// Makes `hostPtr` (a ScriptRuntime*) the active world context.
FLY_API void FlyNative_BindRuntime(void* hostPtr);

// ---- Scripting helpers ----

// Prints a line to the engine log.
FLY_API void FlyNative_Print(const char* text);

#ifdef __cplusplus
} // extern "C"
#endif