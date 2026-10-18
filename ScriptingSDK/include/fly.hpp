// fly.hpp - the C++20 scripting SDK for Flyengine.
//
// Every .cpp file in a project's Scripts/ folder is compiled by the editor
// into one shared library and hot-reloaded when you save. A script is a class
// deriving from fly::Script whose Run() is a coroutine; register it with
// FLY_SCRIPT so the editor can attach it to objects or run it standalone:
//
//     #include "fly.hpp"
//
//     struct Spinner : fly::Script {
//         fly::Task Run() override {
//             fly::Object self = fly::Object::Self();
//             while (true) {
//                 fly::Vec3 r = self.Rotation();
//                 r.y += 45.0f * fly::DeltaTime();
//                 self.SetRotation(r);
//                 co_await fly::NextFrame();
//             }
//         }
//     };
//     FLY_SCRIPT(Spinner)
//
// Inside Run():
//     co_await fly::NextFrame();   resume next frame
//     co_await fly::Wait(2.0f);    resume after 2 seconds
//     co_await fly::WaitFrames(10) resume after 10 frames
//     co_return;                   finish the script
//
// Script state is lost on hot reload: running scripts are destroyed and
// started again from the top of Run().

#pragma once

#include "FlyScriptABI.h"

#include <algorithm>
#include <cmath>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <exception>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fly {

// ---- engine binding (set once by the module entry point) -------------------

namespace detail {
const FlyApi*& Api();
float& Dt();
}

// Visual Script Code Snippet bridge (an engine from before it existed has a shorter table: inputs read "", outputs vanish).
namespace detail {
inline bool HaveSnippets() { return Api()->size >= offsetof(FlyApi, SnippetSetOutput) + sizeof(void*); }
inline std::string SnippetInput(const char* snippet, int pin) {
    return HaveSnippets() ? std::string(Api()->SnippetGetInput(snippet, pin)) : std::string();
}
inline void SnippetOutput(const char* snippet, int pin, const std::string& value) {
    if (HaveSnippets()) Api()->SnippetSetOutput(snippet, pin, value.c_str());
}
}

// The actions Visual Script nodes also have (destroy, clone, force, raycast, keys, sound, text). An engine from before
// they existed has a shorter function table: reads give 0 / "", actions do nothing.
namespace detail {
inline bool HaveActions() { return Api()->size >= offsetof(FlyApi, TimeSincePlay) + sizeof(void*); }
}

// Seconds since the previous frame.
inline float DeltaTime() { return detail::Dt(); }

// ---- value types -----------------------------------------------------------

struct Vec3 {
    float x = 0, y = 0, z = 0;
    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    constexpr Vec3 operator+(Vec3 o) const { return { x + o.x, y + o.y, z + o.z }; }
    constexpr Vec3 operator-(Vec3 o) const { return { x - o.x, y - o.y, z - o.z }; }
    constexpr Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
    Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(Vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
};

struct Color3 {
    unsigned char r = 0, g = 0, b = 0;
};

// Box(0) Hull(1) Default(2) Precise(3), matching the engine's enum.
enum class CollisionAccuracy : int { Box = 0, Hull = 1, Default = 2, Precise = 3 };

// ---- logging ---------------------------------------------------------------

inline void Print(std::string_view text) {
    std::string s(text);
    detail::Api()->Print(s.c_str());
}

// Print("score = ", score, " at ", pos.x) - streams every argument.
template <typename... Args>
    requires (sizeof...(Args) != 1 || !(std::is_convertible_v<const Args&, std::string_view> && ...))
void Print(const Args&... args) {
    std::ostringstream oss;
    (oss << ... << args);
    Print(std::string_view(oss.str()));
}

// ---- scene objects ---------------------------------------------------------

// A handle to a scene object. Stale handles (deleted objects) are rejected by
// the engine: getters return zero values and setters do nothing.
class Object {
public:
    Object() = default;
    explicit Object(FlyHandle h) : m_handle(h) {}

    // The object the running script is attached to (invalid for standalone scripts).
    static Object Self() { return Object(detail::Api()->GetSelf()); }
    // Finds an object by name; "Model.Part" searches a model's parts.
    static Object Find(const char* name) { return Object(detail::Api()->FindObject(name)); }
    static Object Find(const std::string& name) { return Find(name.c_str()); }
    // Creates a primitive: "Cube", "Sphere", "Cylinder" or "Wedge".
    static Object Create(const char* shape) { return Object(detail::Api()->CreateObject(shape)); }

    bool IsValid() const { return m_handle != 0; }
    explicit operator bool() const { return IsValid(); }
    FlyHandle Handle() const { return m_handle; }

    Vec3 Position() const        { return GetVec(detail::Api()->GetPosition); }
    void SetPosition(Vec3 v)     { detail::Api()->SetPosition(m_handle, v.x, v.y, v.z); }
    Vec3 Size() const            { return GetVec(detail::Api()->GetSize); }
    void SetSize(Vec3 v)         { detail::Api()->SetSize(m_handle, v.x, v.y, v.z); }
    Vec3 Rotation() const        { return GetVec(detail::Api()->GetRotation); }
    void SetRotation(Vec3 v)     { detail::Api()->SetRotation(m_handle, v.x, v.y, v.z); }
    Vec3 Origin() const          { return GetVec(detail::Api()->GetOrigin); }
    void SetOrigin(Vec3 v)       { detail::Api()->SetOrigin(m_handle, v.x, v.y, v.z); }
    Vec3 Velocity() const        { return GetVec(detail::Api()->GetVelocity); }
    void SetVelocity(Vec3 v)     { detail::Api()->SetVelocity(m_handle, v.x, v.y, v.z); }
    Vec3 AngularVelocity() const { return GetVec(detail::Api()->GetAngularVelocity); }
    void SetAngularVelocity(Vec3 v) { detail::Api()->SetAngularVelocity(m_handle, v.x, v.y, v.z); }

    Color3 Color() const {
        Color3 c;
        detail::Api()->GetColor(m_handle, &c.r, &c.g, &c.b);
        return c;
    }
    void SetColor(Color3 c) { detail::Api()->SetColor(m_handle, c.r, c.g, c.b); }

    bool Anchored() const          { return detail::Api()->GetAnchored(m_handle) != 0; }
    void SetAnchored(bool v)       { detail::Api()->SetAnchored(m_handle, v ? 1 : 0); }
    bool CanCollide() const        { return detail::Api()->GetCanCollide(m_handle) != 0; }
    void SetCanCollide(bool v)     { detail::Api()->SetCanCollide(m_handle, v ? 1 : 0); }
    float Mass() const             { return detail::Api()->GetMass(m_handle); }
    void SetMass(float m)          { detail::Api()->SetMass(m_handle, m); }
    float Transparency() const     { return detail::Api()->GetTransparency(m_handle); }
    void SetTransparency(float t)  { detail::Api()->SetTransparency(m_handle, t); }
    CollisionAccuracy GetCollisionAccuracy() const {
        return static_cast<CollisionAccuracy>(detail::Api()->GetCollisionAccuracy(m_handle));
    }
    void SetCollisionAccuracy(CollisionAccuracy a) {
        detail::Api()->SetCollisionAccuracy(m_handle, static_cast<int>(a));
    }

    // Visual Script's object actions.
    void Destroy() { if (detail::HaveActions()) detail::Api()->DestroyObject(m_handle); m_handle = 0; }   // the handle is invalid afterwards
    Object Clone() const { return detail::HaveActions() ? Object(detail::Api()->CloneObject(m_handle)) : Object(); }
    std::string Name() const { return detail::HaveActions() ? std::string(detail::Api()->GetName(m_handle)) : std::string(); }
    void SetName(const std::string& name) { if (detail::HaveActions()) detail::Api()->SetName(m_handle, name.c_str()); }
    // A push; call it every frame for a steady one. AddImpulse is a sudden kick, once.
    void AddForce(Vec3 v)   { if (detail::HaveActions()) detail::Api()->AddForce(m_handle, v.x, v.y, v.z); }
    void AddImpulse(Vec3 v) { if (detail::HaveActions()) detail::Api()->AddImpulse(m_handle, v.x, v.y, v.z); }
    // Starts the script attached to the object (or the named script class), like the Run Script node.
    bool StartScript(const char* typeName = nullptr) {
        return detail::HaveActions() && detail::Api()->StartObjectScript(m_handle, typeName) != 0;
    }
    void StopScript() { if (detail::HaveActions()) detail::Api()->StopObjectScript(m_handle); }

    friend bool operator==(Object a, Object b) { return a.m_handle == b.m_handle; }

private:
    using VecGetter = short (*)(FlyHandle, float*, float*, float*);
    Vec3 GetVec(VecGetter fn) const {
        Vec3 v;
        fn(m_handle, &v.x, &v.y, &v.z);
        return v;
    }

    FlyHandle m_handle = 0;
};

// ---- world services --------------------------------------------------------

namespace Game {

inline bool IsPlaying() { return detail::Api()->IsPlaying() != 0; }

namespace Lighting {
inline bool GlobalShadows()            { return detail::Api()->GetShadowsEnabled() != 0; }
inline void SetGlobalShadows(bool on)  { detail::Api()->SetShadowsEnabled(on ? 1 : 0); }
inline int  ShadowQuality()            { return detail::Api()->GetShadowQuality(); }
inline void SetShadowQuality(int q)    { detail::Api()->SetShadowQuality(q); }
inline float Ambient()                 { float a = 0; detail::Api()->GetAmbient(&a); return a; }
inline void SetAmbient(float a)        { detail::Api()->SetAmbient(a); }
}

// The scene's time, weather, sky and picture: what the Explorer's Lighting
// section sets. Setting something inserts its item there if it is not there yet
// (SetRain(0.5f) brings the Weather item). Clouds need the Sky item's atmosphere.
namespace Environment {
namespace detail_env {
// An engine from before these existed has a shorter function table: reads give 0, writes do nothing.
inline bool Have() { return detail::Api()->size >= offsetof(FlyApi, SetEnvironment) + sizeof(void*); }
inline float Get(int what)          { return Have() ? detail::Api()->GetEnvironment(what) : 0.0f; }
inline void Set(int what, float v)  { if (Have()) detail::Api()->SetEnvironment(what, v); }
}
inline float TimeOfDay()               { return detail_env::Get(FLY_ENV_TIME_OF_DAY); }      // hours, 0..24
inline void SetTimeOfDay(float hours)  { detail_env::Set(FLY_ENV_TIME_OF_DAY, hours); }
inline float DayLength()               { return detail_env::Get(FLY_ENV_DAY_LENGTH); }       // real minutes per day, 0 = time stands still
inline void SetDayLength(float min)    { detail_env::Set(FLY_ENV_DAY_LENGTH, min); }
inline float Overcast()                { return detail_env::Get(FLY_ENV_OVERCAST); }         // 0..1
inline void SetOvercast(float v)       { detail_env::Set(FLY_ENV_OVERCAST, v); }
inline float Rain()                    { return detail_env::Get(FLY_ENV_RAIN); }             // 0..1
inline void SetRain(float v)           { detail_env::Set(FLY_ENV_RAIN, v); }
inline float WetGround()               { return detail_env::Get(FLY_ENV_WET_GROUND); }       // 0..1
inline void SetWetGround(float v)      { detail_env::Set(FLY_ENV_WET_GROUND, v); }
inline float Fog()                     { return detail_env::Get(FLY_ENV_FOG); }              // density per metre
inline void SetFog(float density)      { detail_env::Set(FLY_ENV_FOG, density); }
inline float CloudCover()              { return detail_env::Get(FLY_ENV_CLOUD_COVER); }      // 0..1
inline void SetCloudCover(float v)     { detail_env::Set(FLY_ENV_CLOUD_COVER, v); }
inline float WindSpeed()               { return detail_env::Get(FLY_ENV_WIND_SPEED); }       // m/s
inline void SetWindSpeed(float v)      { detail_env::Set(FLY_ENV_WIND_SPEED, v); }
inline float WindDirection()           { return detail_env::Get(FLY_ENV_WIND_DIRECTION); }   // compass degrees
inline void SetWindDirection(float d)  { detail_env::Set(FLY_ENV_WIND_DIRECTION, d); }
inline float Exposure()                { return detail_env::Get(FLY_ENV_EXPOSURE); }         // stops
inline void SetExposure(float stops)   { detail_env::Set(FLY_ENV_EXPOSURE, stops); }
inline float SunIntensity()            { return detail_env::Get(FLY_ENV_SUN_INTENSITY); }    // 0..3
inline void SetSunIntensity(float v)   { detail_env::Set(FLY_ENV_SUN_INTENSITY, v); }
inline float Bloom()                   { return detail_env::Get(FLY_ENV_BLOOM); }            // 0..1
inline void SetBloom(float v)          { detail_env::Set(FLY_ENV_BLOOM, v); }
}

namespace Rendering {
inline bool Grid()                     { return detail::Api()->GetGridVisible() != 0; }
inline void SetGrid(bool on)           { detail::Api()->SetGridVisible(on ? 1 : 0); }
inline bool Wireframe()                { return detail::Api()->GetWireframe() != 0; }
inline void SetWireframe(bool on)      { detail::Api()->SetWireframe(on ? 1 : 0); }
}

namespace Camera {
inline float FOV()                     { float f = 0; detail::Api()->GetFov(&f); return f; }
inline void SetFOV(float f)            { detail::Api()->SetFov(f); }
}

namespace Physics {
inline float Gravity()                 { return detail::Api()->GetGravity(); }
inline void SetGravity(float g)        { detail::Api()->SetGravity(g); }
inline float Friction()                { return detail::Api()->GetFriction(); }
inline void SetFriction(float f)       { detail::Api()->SetFriction(f); }
inline float Restitution()             { return detail::Api()->GetRestitution(); }
inline void SetRestitution(float r)    { detail::Api()->SetRestitution(r); }
}

} // namespace Game

// ---- input, sound, screen text, raycast, time ------------------------------

// Name of the first object a ray hits, "" for none. Directions need not be normalised.
inline std::string Raycast(Vec3 origin, Vec3 direction, float maxDistance = 100.0f) {
    if (!detail::HaveActions()) return {};
    return detail::Api()->Raycast(origin.x, origin.y, origin.z, direction.x, direction.y, direction.z, maxDistance);
}

// Keys by name: "Space", "Enter", "W", "A", "S", "D", "Q", "E", "R", "F", "Up", "Down", "Left", "Right", "1".."4", "Shift", "Ctrl".
inline bool KeyDown(const char* key)    { return detail::HaveActions() && detail::Api()->KeyDown(key) != 0; }      // held
inline bool KeyPressed(const char* key) { return detail::HaveActions() && detail::Api()->KeyPressed(key) != 0; }   // went down this frame

// Gamepads: `gamepad` is 0-based. Buttons: "A", "B", "X", "Y", "Left Bumper", "Right Bumper", "Left Trigger",
// "Right Trigger", "Back", "Start", "Guide", "Left Stick", "Right Stick", "Up", "Down", "Left", "Right".
// Axes: "Left X", "Left Y", "Right X", "Right Y", "Left Trigger", "Right Trigger" (-1..1; triggers rest at -1).
namespace detail {
inline bool HaveGamepad() { return Api()->size >= offsetof(FlyApi, GamepadAxis) + sizeof(void*); }
}
inline bool GamepadAvailable(int gamepad = 0) { return detail::HaveGamepad() && detail::Api()->GamepadAvailable(gamepad) != 0; }
inline bool GamepadDown(const char* button, int gamepad = 0) {
    return detail::HaveGamepad() && detail::Api()->GamepadButtonDown(gamepad, button) != 0;
}
inline bool GamepadPressed(const char* button, int gamepad = 0) {
    return detail::HaveGamepad() && detail::Api()->GamepadButtonPressed(gamepad, button) != 0;
}
inline float GamepadAxis(const char* axis, int gamepad = 0) {
    return detail::HaveGamepad() ? detail::Api()->GamepadAxis(gamepad, axis) : 0.0f;
}

// Sound: a file in the project, volume 0..1. SetVolume is the master volume.
inline void PlaySound(const std::string& file, float volume = 1.0f) { if (detail::HaveActions()) detail::Api()->PlaySound(file.c_str(), volume); }
inline void SetVolume(float volume) { if (detail::HaveActions()) detail::Api()->SetMasterVolume(volume); }

// Text on the game screen. `position` is "Top Left", "Top Center", "Top Right", "Center", "Bottom Left", "Bottom Center"
// or "Bottom Right"; seconds 0 keeps it until replaced; empty text clears that spot.
inline void ShowText(const std::string& text, const std::string& position = "Top Center", float seconds = 0.0f) {
    if (detail::HaveActions()) detail::Api()->ShowText(text.c_str(), position.c_str(), seconds);
}

// Seconds since Play started.
inline float TimeSincePlay() { return detail::HaveActions() ? detail::Api()->TimeSincePlay() : 0.0f; }

// ---- vector helpers --------------------------------------------------------

inline float Dot(Vec3 a, Vec3 b)      { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Length(Vec3 v)           { return std::sqrt(Dot(v, v)); }
inline float Distance(Vec3 a, Vec3 b) { return Length(a - b); }
inline Vec3 Normalize(Vec3 v)         { const float l = Length(v); return l > 1e-6f ? v * (1.0f / l) : Vec3{}; }
inline Vec3 Cross(Vec3 a, Vec3 b)     { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
inline Vec3 Lerp(Vec3 a, Vec3 b, float t)    { return a + (b - a) * t; }

// ---- gameplay: rays, overlaps, touches, joints, camera ---------------------
//
// Everything in this section needs an engine that has them; on an older one the queries find nothing and the
// actions do nothing.

namespace detail {
inline bool HaveGameplay() { return Api()->size >= offsetof(FlyApi, SpawnDecal) + sizeof(void*); }
}

// What a ray hit. `object` is invalid when it hit terrain, the city or anything else that is not a scene object.
struct Hit {
    bool hit = false;
    Vec3 point, normal;
    float distance = 0.0f;
    Object object;
    explicit operator bool() const { return hit; }
};

// Shoots a ray (the direction need not be normalised) and reports the first thing it meets, with where and how far.
inline Hit Cast(Vec3 origin, Vec3 direction, float maxDistance = 100.0f) {
    Hit h;
    if (!detail::HaveGameplay()) return h;
    FlyRayHit r;
    if (!detail::Api()->RaycastEx(origin.x, origin.y, origin.z, direction.x, direction.y, direction.z, maxDistance, &r)) return h;
    h.hit = true;
    h.point = { r.px, r.py, r.pz };
    h.normal = { r.nx, r.ny, r.nz };
    h.distance = r.distance;
    h.object = Object(r.object);
    return h;
}

// Every scene object whose bounds reach into the ball (up to `maxCount`).
inline std::vector<Object> OverlapSphere(Vec3 center, float radius, int maxCount = 64) {
    std::vector<Object> found;
    if (!detail::HaveGameplay() || maxCount <= 0) return found;
    std::vector<FlyHandle> handles(static_cast<size_t>(maxCount));
    const int n = detail::Api()->OverlapSphere(center.x, center.y, center.z, radius, handles.data(), maxCount);
    for (int i = 0; i < n; i++) found.push_back(Object(handles[static_cast<size_t>(i)]));
    return found;
}

// Two things that started touching. `b` is invalid when `a` touched terrain or the city. `speed` is how hard they
// met (0 for a gentle contact the engine reported no impact for).
struct Touch {
    Object a, b;
    Vec3 point, normal;
    float speed = 0.0f;
    // True when `o` is one of the two.
    bool Involves(Object o) const { return o.IsValid() && (a == o || b == o); }
    // The one that is not `o` (invalid if `o` is neither).
    Object Other(Object o) const { return a == o ? b : (b == o ? a : Object()); }
};

// The contacts that began since the last physics update. Every script that asks in a frame sees the same list.
//
//     for (const fly::Touch& t : fly::Touches())
//         if (t.Involves(self) && t.speed > 4.0f) fly::Emit("Sparks", t.point, t.normal);
inline std::vector<Touch> Touches(int maxCount = 128) {
    std::vector<Touch> out;
    if (!detail::HaveGameplay() || maxCount <= 0) return out;
    std::vector<FlyTouch> raw(static_cast<size_t>(maxCount));
    const int n = detail::Api()->PollTouches(raw.data(), maxCount);
    for (int i = 0; i < n; i++) {
        const FlyTouch& r = raw[static_cast<size_t>(i)];
        Touch t;
        t.a = Object(r.a); t.b = Object(r.b);
        t.point = { r.px, r.py, r.pz };
        t.normal = { r.nx, r.ny, r.nz };
        t.speed = r.speed;
        out.push_back(t);
    }
    return out;
}

// A physics joint between two objects. Joints exist only while playing; they vanish when Play stops.
class Joint {
public:
    Joint() = default;
    bool IsValid() const { return m_id != 0; }
    explicit operator bool() const { return IsValid(); }
    void Destroy() { if (m_id && detail::HaveGameplay()) detail::Api()->DestroyJoint(m_id); m_id = 0; }

    // Glued together at `anchor` (a world position).
    static Joint Weld(Object a, Object b, Vec3 anchor) { return Make(FLY_JOINT_WELD, a, b, anchor, anchor); }
    // Swings about an axis through `anchor` (a door on its post).
    static Joint Hinge(Object a, Object b, Vec3 anchor) { return Make(FLY_JOINT_HINGE, a, b, anchor, anchor); }
    // Turns freely about `anchor` (a rope swing, a shoulder).
    static Joint Ball(Object a, Object b, Vec3 anchor) { return Make(FLY_JOINT_BALL, a, b, anchor, anchor); }
    // Keeps `anchorA` (on a) and `anchorB` (on b) the distance apart they are now.
    static Joint Rope(Object a, Object b, Vec3 anchorA, Vec3 anchorB) { return Make(FLY_JOINT_ROPE, a, b, anchorA, anchorB); }

private:
    static Joint Make(int kind, Object a, Object b, Vec3 pa, Vec3 pb) {
        Joint j;
        if (detail::HaveGameplay())
            j.m_id = detail::Api()->CreateJoint(kind, a.Handle(), b.Handle(), pa.x, pa.y, pa.z, pb.x, pb.y, pb.z);
        return j;
    }
    unsigned m_id = 0;
};

namespace Game {
namespace Camera {
// Where the view is and what it looks at. Whatever drives the camera (the player, the editor) may move it again, so
// call SetView every frame to keep hold of it.
struct View { Vec3 position, target; float fov = 60.0f; };
inline View Get() {
    View v;
    if (!detail::HaveGameplay()) return v;
    float f[7] = {};
    detail::Api()->GetCamera(f);
    v.position = { f[0], f[1], f[2] };
    v.target = { f[3], f[4], f[5] };
    v.fov = f[6];
    return v;
}
inline void SetView(Vec3 position, Vec3 target, float fov = 0.0f) {
    if (!detail::HaveGameplay()) return;
    const float f[7] = { position.x, position.y, position.z, target.x, target.y, target.z, fov > 0.0f ? fov : Get().fov };
    detail::Api()->SetCamera(f);
}
}
}

// ---- gameplay: particles and decals ----------------------------------------
//
// Presets: "Smoke", "Fire", "Sparks", "Dust", "Splash", "Debris", "Steam". Heavy impacts already kick up dust by
// themselves; use these for anything else.

// One puff. `count` 0 uses the preset's own size; `scale` multiplies particle size and speed.
inline void Emit(const char* preset, Vec3 position, Vec3 direction = { 0, 1, 0 }, int count = 0, float scale = 1.0f) {
    if (detail::HaveGameplay()) detail::Api()->EmitParticles(preset, position.x, position.y, position.z, direction.x, direction.y, direction.z, count, scale);
}

// A steady source (a campfire, a chimney) that runs until Stop().
class Emitter {
public:
    Emitter() = default;
    static Emitter Start(const char* preset, Vec3 position, Vec3 direction = { 0, 1, 0 }, float scale = 1.0f) {
        Emitter e;
        if (detail::HaveGameplay()) e.m_id = detail::Api()->StartEmitter(preset, position.x, position.y, position.z, direction.x, direction.y, direction.z, scale);
        return e;
    }
    bool IsValid() const { return m_id != 0; }
    void Move(Vec3 position, Vec3 direction = { 0, 1, 0 }) {
        if (m_id) detail::Api()->MoveEmitter(m_id, position.x, position.y, position.z, direction.x, direction.y, direction.z);
    }
    void Stop() { if (m_id && detail::HaveGameplay()) detail::Api()->StopEmitter(m_id); m_id = 0; }
private:
    unsigned m_id = 0;
};

struct ColorRGBA { unsigned char r = 0, g = 0, b = 0, a = 255; };

// A flat mark on a surface: `normal` points out of it, `size` is its diameter in metres. It fades out over its
// last quarter. Use the point and normal of a Cast() or a Touch.
inline void Decal(Vec3 position, Vec3 normal, float size, ColorRGBA color = { 20, 18, 15, 160 }, float seconds = 30.0f) {
    if (detail::HaveGameplay()) detail::Api()->SpawnDecal(position.x, position.y, position.z, normal.x, normal.y, normal.z, size, color.r, color.g, color.b, color.a, seconds);
}

// ---- tweens and events (scripts only, no engine support needed) ------------

enum class Ease { Linear, In, Out, InOut, Bounce, Back };

inline float Ease01(Ease e, float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    switch (e) {
        case Ease::In: return t * t * t;
        case Ease::Out: { const float u = 1.0f - t; return 1.0f - u * u * u; }
        case Ease::InOut: return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
        case Ease::Bounce: {
            const float n = 7.5625f, d = 2.75f;
            if (t < 1.0f / d) return n * t * t;
            if (t < 2.0f / d) { t -= 1.5f / d; return n * t * t + 0.75f; }
            if (t < 2.5f / d) { t -= 2.25f / d; return n * t * t + 0.9375f; }
            t -= 2.625f / d; return n * t * t + 0.984375f;
        }
        case Ease::Back: { const float c1 = 1.70158f, c3 = c1 + 1.0f; const float u = t - 1.0f; return 1.0f + c3 * u * u * u + c1 * u * u; }
        default: return t;
    }
}

// Moves a value (float or Vec3) from `from` to `to` over `seconds`.
//
//     fly::Tween<fly::Vec3> tw(door.Position(), door.Position() + fly::Vec3{0, 3, 0}, 1.5f, fly::Ease::InOut);
//     while (!tw.Done()) { door.SetPosition(tw.Step(fly::DeltaTime())); co_await fly::NextFrame(); }
template <typename T>
class Tween {
public:
    Tween() : m_from(), m_to(), m_seconds(1.0f), m_ease(Ease::Linear), m_t(1.0f) {}   // already finished
    Tween(T from, T to, float seconds, Ease ease = Ease::Linear) : m_from(from), m_to(to), m_seconds(seconds > 1e-4f ? seconds : 1e-4f), m_ease(ease) {}
    // Advances by `dt` and returns the new value.
    T Step(float dt) { m_t += dt; return Value(); }
    T Value() const { return Lerp(m_from, m_to, Ease01(m_ease, m_t / m_seconds)); }
    bool Done() const { return m_t >= m_seconds; }
    void Restart() { m_t = 0.0f; }
private:
    T m_from, m_to;
    float m_seconds;
    Ease m_ease;
    float m_t = 0.0f;
};

// Named events that scripts in the same project can fire and listen to. Handlers run immediately inside Fire().
//
//     auto sub = fly::Events::On("door_open", [](float who) { fly::Print("opened by ", who); });
//     fly::Events::Fire("door_open", 1.0f);
//     fly::Events::Off(sub);
//
// Handlers are lost when scripts hot-reload, like any other script state; subscribe again from Run().
namespace Events {
using Handler = std::function<void(float)>;
namespace detail_ev {
struct Sub { int id; std::string name; Handler fn; };
inline std::vector<Sub>& Subs() { static std::vector<Sub> s; return s; }
inline int& NextId() { static int n = 1; return n; }
}
inline int On(const std::string& name, Handler fn) {
    const int id = detail_ev::NextId()++;
    detail_ev::Subs().push_back({ id, name, std::move(fn) });
    return id;
}
inline void Off(int subscription) {
    auto& s = detail_ev::Subs();
    for (size_t i = 0; i < s.size(); i++) if (s[i].id == subscription) { s.erase(s.begin() + static_cast<std::ptrdiff_t>(i)); return; }
}
inline void Fire(const std::string& name, float value = 0.0f) {
    auto copy = detail_ev::Subs();   // a handler may subscribe or unsubscribe
    for (auto& sub : copy) if (sub.name == name && sub.fn) sub.fn(value);
}
}

// ---- coroutines ------------------------------------------------------------

// The return type of Script::Run(). Starts suspended; the engine resumes it
// once per frame unless it is waiting.
class Task {
public:
    struct promise_type {
        float waitSeconds = 0.0f;
        int waitFrames = 0;
        std::exception_ptr error;

        Task get_return_object() { return Task(std::coroutine_handle<promise_type>::from_promise(*this)); }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { error = std::current_exception(); }
    };

    Task() = default;
    explicit Task(std::coroutine_handle<promise_type> h) : m_h(h) {}
    Task(Task&& o) noexcept : m_h(std::exchange(o.m_h, {})) {}
    Task& operator=(Task&& o) noexcept {
        if (this != &o) { Reset(); m_h = std::exchange(o.m_h, {}); }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { Reset(); }

    // Advances by one frame. Returns false once finished. Rethrows anything the
    // script threw.
    bool Step(float dt) {
        if (!m_h || m_h.done()) return false;
        promise_type& p = m_h.promise();
        if (p.waitSeconds > 0.0f) {
            p.waitSeconds -= dt;
            if (p.waitSeconds > 0.0f) return true;
            p.waitSeconds = 0.0f;
        }
        if (p.waitFrames > 0) {
            --p.waitFrames;
            return true;
        }
        m_h.resume();
        if (p.error) std::rethrow_exception(std::exchange(p.error, nullptr));
        return !m_h.done();
    }

private:
    void Reset() { if (m_h) { m_h.destroy(); m_h = {}; } }
    std::coroutine_handle<promise_type> m_h;
};

namespace detail {
struct WaitAwaiter {
    float seconds = 0.0f;
    int frames = 0;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<Task::promise_type> h) const noexcept {
        h.promise().waitSeconds = seconds;
        h.promise().waitFrames = frames;
    }
    void await_resume() const noexcept {}
};
}

// Resume on the next frame.
inline detail::WaitAwaiter NextFrame() { return {}; }
// Resume once `seconds` of game time have passed.
inline detail::WaitAwaiter Wait(float seconds) { return { seconds, 0 }; }
// Skip `frames` frames, then resume on the one after.
inline detail::WaitAwaiter WaitFrames(int frames) { return { 0.0f, frames }; }

// ---- scripts ---------------------------------------------------------------

struct Script {
    virtual ~Script() = default;
    virtual Task Run() = 0;
};

namespace detail {

struct ClassEntry {
    const char* name;
    Script* (*make)();
};

std::vector<ClassEntry>& Registry();

struct Registrar {
    Registrar(const char* name, Script* (*make)()) { Registry().push_back({ name, make }); }
};

} // namespace detail

} // namespace fly

// Registers a script class under its C++ name. Use once per class, at
// namespace scope in a .cpp file.
#define FLY_SCRIPT(ClassName)                                                  \
    static ::fly::detail::Registrar fly_registrar_##ClassName(                 \
        #ClassName, []() -> ::fly::Script* { return new ClassName(); });
