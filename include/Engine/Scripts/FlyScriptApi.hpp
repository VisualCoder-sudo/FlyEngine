#pragma once

// External .NET scripting C API.
//
// This is the stable ABI between the C++ engine and the C# scripting runtime.
// C# game scripts P/Invoke these functions (exported from the engine exe) to
// read and mutate the world, exactly as Flyscript's "game.Workspace.X.Prop"
// / "self.Prop" syntax used to. See the C# mirror in the scripting SDK.
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

#ifdef __cplusplus
extern "C" {
#endif

// ---- Object registry / handles (id64) ----

// Finds a scene object by display name. Returns an id64 handle (0 = not found).
// `modelMember`: if non-null, searches model parts too (model name "Model.Part").
__declspec(dllexport) unsigned long long FlyNative_FindObject(const char* name);

// The object the currently-running script is attached to (self). 0 if none.
__declspec(dllexport) unsigned long long FlyNative_GetSelf(void);

// ---- Object properties ----

// vec3 getters/setters
__declspec(dllexport) short FlyNative_GetPosition(unsigned long long h, float* x, float* y, float* z);
__declspec(dllexport) short FlyNative_SetPosition(unsigned long long h, float x, float y, float z);
__declspec(dllexport) short FlyNative_GetSize(unsigned long long h, float* x, float* y, float* z);
__declspec(dllexport) short FlyNative_SetSize(unsigned long long h, float x, float y, float z);
__declspec(dllexport) short FlyNative_GetRotation(unsigned long long h, float* x, float* y, float* z);
__declspec(dllexport) short FlyNative_SetRotation(unsigned long long h, float x, float y, float z);
__declspec(dllexport) short FlyNative_GetOrigin(unsigned long long h, float* x, float* y, float* z);
__declspec(dllexport) short FlyNative_SetOrigin(unsigned long long h, float x, float y, float z);
__declspec(dllexport) short FlyNative_GetVelocity(unsigned long long h, float* x, float* y, float* z);
__declspec(dllexport) short FlyNative_SetVelocity(unsigned long long h, float x, float y, float z);
__declspec(dllexport) short FlyNative_GetAngularVelocity(unsigned long long h, float* x, float* y, float* z);
__declspec(dllexport) short FlyNative_SetAngularVelocity(unsigned long long h, float x, float y, float z);

// color getter/setter (r,g,b 0..255)
__declspec(dllexport) short FlyNative_GetColor(unsigned long long h, unsigned char* r, unsigned char* g, unsigned char* b);
__declspec(dllexport) short FlyNative_SetColor(unsigned long long h, unsigned char r, unsigned char g, unsigned char b);

// scalar/boolean properties
__declspec(dllexport) short FlyNative_GetAnchored(unsigned long long h);
__declspec(dllexport) void  FlyNative_SetAnchored(unsigned long long h, short value);
__declspec(dllexport) short FlyNative_GetCanCollide(unsigned long long h);
__declspec(dllexport) void  FlyNative_SetCanCollide(unsigned long long h, short value);
__declspec(dllexport) float FlyNative_GetMass(unsigned long long h);
__declspec(dllexport) void  FlyNative_SetMass(unsigned long long h, float mass);
__declspec(dllexport) float FlyNative_GetTransparency(unsigned long long h);
__declspec(dllexport) void  FlyNative_SetTransparency(unsigned long long h, float t);

// CollisionAccuracy is an enum = Box(0) Hull(1) Default(2) Precise(3).
// Returns -1 on invalid handle.
__declspec(dllexport) int FlyNative_GetCollisionAccuracy(unsigned long long h);
__declspec(dllexport) void FlyNative_SetCollisionAccuracy(unsigned long long h, int accuracy);

// ---- World services (the old game.* properties) ----

__declspec(dllexport) short FlyNative_IsPlaying(void);

__declspec(dllexport) short FlyNative_GetShadowsEnabled(void);
__declspec(dllexport) void  FlyNative_SetShadowsEnabled(short on);
__declspec(dllexport) int   FlyNative_GetShadowQuality(void);
__declspec(dllexport) void  FlyNative_SetShadowQuality(int q);
__declspec(dllexport) float FlyNative_GetAmbient(float* intensity); // returns 1.0 (single value); prop is scalar
__declspec(dllexport) void  FlyNative_SetAmbient(float intensity);
__declspec(dllexport) short FlyNative_GetGridVisible(void);
__declspec(dllexport) void  FlyNative_SetGridVisible(short on);
__declspec(dllexport) short FlyNative_GetWireframe(void);
__declspec(dllexport) void  FlyNative_SetWireframe(short on);
__declspec(dllexport) float FlyNative_GetFov(float* fov);
__declspec(dllexport) void  FlyNative_SetFov(float fov);

// Physics (game.Physics.* — routed to the simulation).
__declspec(dllexport) float FlyNative_GetGravity(void);
__declspec(dllexport) void  FlyNative_SetGravity(float g);
__declspec(dllexport) float FlyNative_GetFriction(void);
__declspec(dllexport) void  FlyNative_SetFriction(float f);
__declspec(dllexport) float FlyNative_GetRestitution(void);
__declspec(dllexport) void  FlyNative_SetRestitution(float r);

// ---- Object creation / destruction ----

// Creates a primitive object (Cube, Sphere, Cylinder, Wedge). Returns a handle
// or 0 on failure. The object is spawned into the simulation when playing.
__declspec(dllexport) unsigned long long FlyNative_CreateObject(const char* shapeName);

// ---- Scripting helpers ----

// Prints a line from C# to the engine log.
__declspec(dllexport) void FlyNative_Print(const char* text);

// ---- Script lifecycle (called from editor UI, forwarded to C# ScriptHost) ----
// Returns script ID (>0) or 0 on failure.
__declspec(dllexport) unsigned long long FlyNative_StartObjectScript(unsigned long long objectHandle, const char* typeName);
__declspec(dllexport) void FlyNative_StopObjectScript(unsigned long long objectHandle);
__declspec(dllexport) short FlyNative_IsObjectScriptRunning(unsigned long long objectHandle);

__declspec(dllexport) unsigned long long FlyNative_StartStandaloneScript(int index, const char* typeName);
__declspec(dllexport) void FlyNative_StopStandaloneScript(int index);
__declspec(dllexport) short FlyNative_IsStandaloneScriptRunning(int index);

// Clear all scripts (called when play mode stops).
__declspec(dllexport) void FlyNative_ClearAllScripts(void);

// ---- Runtime binding ----

// Binds the CoreCLR host pointer so C# can call back into C++ via P/Invoke.
// Called by the engine after CoreCLR initialization.
__declspec(dllexport) void FlyNative_BindRuntime(void* hostPtr);

// ---- Scripting helpers ----

// Prints a line from C# to the engine log.
__declspec(dllexport) void FlyNative_Print(const char* text);

#ifdef __cplusplus
} // extern "C"
#endif