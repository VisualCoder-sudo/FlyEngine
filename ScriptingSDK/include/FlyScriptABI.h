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

/* Where a ray hit: the point, the surface's outward normal, the distance from the origin, and the object (0 when it
 * hit terrain, the city or something else that is not a scene object). */
typedef struct FlyRayHit {
    float px, py, pz;
    float nx, ny, nz;
    float distance;
    FlyHandle object;
} FlyRayHit;

/* Two things that started touching this frame. `speed` is how hard they met (0 when the engine saw no impact for
 * the pair); `px,py,pz` is roughly where. `b` is 0 when `a` touched terrain or the city. */
typedef struct FlyTouch {
    FlyHandle a, b;
    float px, py, pz;
    float nx, ny, nz;
    float speed;
} FlyTouch;

/* Joint kinds for CreateJoint. */
enum {
    FLY_JOINT_WELD = 0,   /* glued together */
    FLY_JOINT_HINGE = 1,  /* swings about an axis through the anchor (a door) */
    FLY_JOINT_BALL = 2,   /* free rotation about the anchor (a rope swing, a ragdoll shoulder) */
    FLY_JOINT_ROPE = 3    /* keeps the two anchors a fixed distance apart */
};

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

    /* environment: the scene's time, weather, sky and picture (the Explorer's
     * Lighting section). `what` is one of the FLY_ENV_* numbers below. Added
     * after the first release of this table: an engine older than that has a
     * smaller `size`, and fly.hpp checks it before calling these. */
    float (*GetEnvironment)(int what);
    void  (*SetEnvironment)(int what, float value);

    /* Visual Script "Code Snippet" nodes: the values wired into / out of the node whose generated class is `snippet`.
     * SnippetGetInput returns the pin's text ("" if nothing is wired); the pointer stays valid until the next call
     * from the same thread. Added after the first releases: check `size` first (fly.hpp does). */
    const char* (*SnippetGetInput)(const char* snippet, int pin);
    void        (*SnippetSetOutput)(const char* snippet, int pin, const char* value);

    /* Everything a Visual Script node can do, for C++ scripts too. Added after the first releases: fly.hpp checks
     * `size` before calling these (an older engine ignores them). Returned strings stay valid until the next call from
     * the same thread. */
    void      (*DestroyObject)(FlyHandle h);
    FlyHandle (*CloneObject)(FlyHandle h);
    const char* (*GetName)(FlyHandle h);
    void      (*SetName)(FlyHandle h, const char* name);
    short     (*AddForce)(FlyHandle h, float x, float y, float z);      /* a push; call every frame for a steady one */
    short     (*AddImpulse)(FlyHandle h, float x, float y, float z);    /* a sudden kick */
    const char* (*Raycast)(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistance); /* name of the first object hit, "" for none */
    FlyHandle (*StartObjectScript)(FlyHandle h, const char* typeName);  /* typeName may be NULL: use the script attached to the object */
    void      (*StopObjectScript)(FlyHandle h);
    short     (*KeyDown)(const char* keyName);       /* true while held */
    short     (*KeyPressed)(const char* keyName);    /* true on the frame it went down */
    void      (*PlaySound)(const char* file, float volume);   /* file relative to the project */
    void      (*SetMasterVolume)(float volume);
    void      (*ShowText)(const char* text, const char* position, float seconds);   /* seconds 0 = until replaced; "" clears */
    float     (*TimeSincePlay)(void);

    /* Gamepads. `gamepad` is 0-based (0..3); button/axis names match the Visual Script Input nodes
     * (GamepadButtonDown/GamepadAxis). Added after the first releases: fly.hpp checks `size` first. */
    short     (*GamepadAvailable)(int gamepad);
    short     (*GamepadButtonDown)(int gamepad, const char* buttonName);      /* true while held */
    short     (*GamepadButtonPressed)(int gamepad, const char* buttonName);   /* true on the frame it went down */
    float     (*GamepadAxis)(int gamepad, const char* axisName);              /* -1..1; triggers rest at -1 */

    /* Gameplay queries and effects. Added after the first releases: fly.hpp checks `size` first. */
    short     (*RaycastEx)(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistance, FlyRayHit* out); /* 1 on a hit */
    int       (*OverlapSphere)(float cx, float cy, float cz, float radius, FlyHandle* out, int maxCount);   /* objects touching the ball; returns how many were stored */
    int       (*PollTouches)(FlyTouch* out, int maxCount);          /* contacts that began since the last Update; each is delivered once per frame */
    unsigned  (*CreateJoint)(int kind, FlyHandle a, FlyHandle b, float ax, float ay, float az, float bx, float by, float bz); /* anchors in world space; the second is used by ROPE only. 0 = failed */
    void      (*DestroyJoint)(unsigned joint);
    void      (*GetCamera)(float* out7);                            /* x y z, target x y z, fov */
    void      (*SetCamera)(const float* in7);                       /* the same layout; call every frame to keep control of the view */
    void      (*EmitParticles)(const char* preset, float x, float y, float z, float dx, float dy, float dz, int count, float scale);
    unsigned  (*StartEmitter)(const char* preset, float x, float y, float z, float dx, float dy, float dz, float scale); /* 0 = failed */
    void      (*MoveEmitter)(unsigned emitter, float x, float y, float z, float dx, float dy, float dz);
    void      (*StopEmitter)(unsigned emitter);
    void      (*SpawnDecal)(float x, float y, float z, float nx, float ny, float nz, float size, unsigned char r, unsigned char g, unsigned char b, unsigned char a, float seconds);

    /* Prefabs. Added after the gameplay set: fly.hpp checks `size` first. */
    FlyHandle (*SpawnPrefab)(const char* file, float x, float y, float z); /* project-relative .flyprefab; its pivot lands at x y z. Returns its first object, 0 = failed */

    /* In-game UI: widgets made by id and drawn over the 3D view. Added after prefabs: fly.hpp checks `size` first.
     * kind: 0 panel, 1 text, 2 bar, 3 image. anchor: 0..8 = TopLeft TopCenter TopRight Left Center Right BottomLeft
     * BottomCenter BottomRight, 9 = Fill. Sizes and offsets are in pixels of a 1280x720 screen, scaled to the window. */
    short     (*UICreate)(const char* id, int kind);                /* 1 if the widget exists afterwards */
    void      (*UISetLayout)(const char* id, int anchor, float x, float y, float w, float h);
    void      (*UISetColor)(const char* id, unsigned char r, unsigned char g, unsigned char b, unsigned char a,
                            unsigned char r2, unsigned char g2, unsigned char b2, unsigned char a2);   /* bars: fill, then background */
    void      (*UISetText)(const char* id, const char* text, float fontSize);   /* fontSize <= 0 keeps the size */
    void      (*UISetValue)(const char* id, float value);           /* bars: 0..1 */
    void      (*UISetImage)(const char* id, const char* file);      /* project-relative */
    void      (*UISetVisible)(const char* id, short visible);
    void      (*UIRemove)(const char* id);
    void      (*UISetFont)(const char* id, const char* font);       /* "" / "Sans", "Serif", "Mono", "Pixel", or a project .ttf / .otf */
    /* Reading widgets back. All of them give "nothing" (0, "", -1) for an id that does not exist. */
    short     (*UIExists)(const char* id);
    float     (*UIGetValue)(const char* id);                        /* a bar's 0..1 */
    int       (*UIGetText)(const char* id, char* out, int capacity);/* length of the text (it is cut to fit `capacity`, always 0-terminated); -1 = no such widget */
    short     (*UIGetVisible)(const char* id);
    short     (*UIGetLayout)(const char* id, float* out6);          /* anchor x y w h fontSize; 1 if the widget exists */
    short     (*UIGetColor)(const char* id, unsigned char* out8);   /* r g b a, then the bar background r g b a; 1 if it exists */

    /* Reading input and debugging. Added after the UI: fly.hpp checks `size` first. */
    void      (*GetMouseDelta)(float* out2);                        /* pixels moved since the last frame: x y */
    void      (*DebugText)(const char* key, const char* text);      /* a line of text in the top-left corner, one row per key; "" removes the row */
} FlyApi;

/* What GetEnvironment / SetEnvironment read and write. Append only. */
enum {
    FLY_ENV_TIME_OF_DAY = 0,    /* hours, 0..24 */
    FLY_ENV_DAY_LENGTH = 1,     /* real minutes per day; 0 = time stands still */
    FLY_ENV_OVERCAST = 2,       /* 0..1, the Weather item's cloud cover */
    FLY_ENV_RAIN = 3,           /* 0..1 */
    FLY_ENV_WET_GROUND = 4,     /* 0..1 */
    FLY_ENV_FOG = 5,            /* density per metre, 0..0.2 */
    FLY_ENV_CLOUD_COVER = 6,    /* 0..1, the Clouds item's (needs the atmosphere sky) */
    FLY_ENV_WIND_SPEED = 7,     /* metres a second */
    FLY_ENV_WIND_DIRECTION = 8, /* degrees, the compass direction the wind blows towards */
    FLY_ENV_EXPOSURE = 9,       /* stops, -4..4 */
    FLY_ENV_SUN_INTENSITY = 10, /* 0..3 */
    FLY_ENV_BLOOM = 11,         /* 0..1 */
    FLY_ENV_COUNT
};

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
