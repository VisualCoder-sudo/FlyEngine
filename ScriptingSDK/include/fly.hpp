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
inline bool HaveUnions() { return Api()->size >= offsetof(FlyApi, GetUseGlobalColor) + sizeof(void*); }
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
    // Unions (solid modelling, like the editor's Union dropdown); on an engine from before them these do nothing.
    // IsUnion / UnionPartCount: a union holds the parts it was made from. Separate() removes it and gives the parts back
    // (this handle becomes invalid). SetNegate marks a primitive as a cutter: not drawn in play, never collides, and
    // carved out of a union it is made part of. SetUseGlobalColor (unions only): off = each face keeps its part's
    // colour, on = the whole union is drawn in the object's own colour.
    bool IsUnion() const { return UnionPartCount() > 0; }
    int UnionPartCount() const { return detail::HaveUnions() ? detail::Api()->UnionPartCount(m_handle) : 0; }
    std::vector<Object> Separate() {
        std::vector<Object> parts;
        if (!detail::HaveUnions()) return parts;
        const int n = detail::Api()->UnionPartCount(m_handle);
        if (n <= 0) return parts;
        std::vector<FlyHandle> raw((size_t)n, 0);
        const int got = detail::Api()->SeparateUnion(m_handle, raw.data(), n);
        for (int i = 0; i < got && i < n; ++i) parts.emplace_back(raw[(size_t)i]);
        m_handle = 0;
        return parts;
    }
    bool IsNegate() const { return detail::HaveUnions() && detail::Api()->GetNegate(m_handle) != 0; }
    bool SetNegate(bool on) { return detail::HaveUnions() && detail::Api()->SetNegate(m_handle, on ? 1 : 0) != 0; }
    bool UseGlobalColor() const { return detail::HaveUnions() && detail::Api()->GetUseGlobalColor(m_handle) != 0; }
    bool SetUseGlobalColor(bool on) { return detail::HaveUnions() && detail::Api()->SetUseGlobalColor(m_handle, on ? 1 : 0) != 0; }

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

namespace detail {
inline bool HaveDebug() { return Api()->size >= offsetof(FlyApi, DebugText) + sizeof(void*); }
}
// Mouse movement since the last frame in pixels (x right, y down). Both are 0 on an engine too old to say.
inline void MouseDelta(float& dx, float& dy) {
    float d[2] = { 0.0f, 0.0f };
    if (detail::HaveDebug()) detail::Api()->GetMouseDelta(d);
    dx = d[0]; dy = d[1];
}
// A line of text in the top-left corner of the game view, one row per key, so a script's state can be watched live.
// An empty text removes the row. Does nothing on an engine too old to have it.
inline void DebugText(const std::string& key, const std::string& text) {
    if (detail::HaveDebug()) detail::Api()->DebugText(key.c_str(), text.c_str());
}

// Keys by name: "Space", "Enter", "W", "A", "S", "D", "Q", "E", "R", "F", "Up", "Down", "Left", "Right", "1".."4", "Shift", "Ctrl", "Tab", "Escape", "P",
// and the mouse buttons "Mouse Left", "Mouse Right", "Mouse Middle".
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

// ---- prefabs ---------------------------------------------------------------

namespace detail {
inline bool HavePrefabs() { return Api()->size >= offsetof(FlyApi, SpawnPrefab) + sizeof(void*); }
}

// Prefabs saved from the editor (Explorer > right-click a group > Save as Prefab) live in assets/Prefabs.
namespace Prefab {
// Adds a copy of the prefab with its pivot (middle of its footprint, lowest point) at `position`. Returns the
// prefab's first object, or an invalid Object if the file is missing. While playing, the copy and its scripts start
// at once, and it is removed again when play stops. `file` is project-relative: "assets/Prefabs/Tree.flyprefab".
inline Object Spawn(const std::string& file, Vec3 position) {
    return detail::HavePrefabs() ? Object(detail::Api()->SpawnPrefab(file.c_str(), position.x, position.y, position.z)) : Object();
}
}

// ---- unions ----------------------------------------------------------------

namespace detail {
inline Object MakeUnion(const std::vector<Object>& parts, int mode) {
    if (!HaveUnions() || parts.size() < 2) return {};
    std::vector<FlyHandle> raw;
    raw.reserve(parts.size());
    for (const Object& o : parts) raw.push_back(o.Handle());
    return Object(Api()->UnionObjects(raw.data(), (int)raw.size(), mode));
}
}

// Merges primitive parts (cube, sphere, cylinder, wedge) into one solid object, like the editor's Union command. The
// parts are removed (their handles become invalid) and the new object is returned; faces keep their part's colour. A
// part with SetNegate(true) is carved out of the result. Returns an invalid Object, and changes nothing, when it cannot
// be built: fewer than two parts, an imported mesh or a union among them, or nothing left after the cuts.
inline Object Union(const std::vector<Object>& parts) { return detail::MakeUnion(parts, 0); }
// Like Union, but keeps only the volume all the parts share.
inline Object Intersect(const std::vector<Object>& parts) { return detail::MakeUnion(parts, 1); }

// ---- in-game UI ------------------------------------------------------------

namespace detail {
inline bool HaveUI() { return Api()->size >= offsetof(FlyApi, UIRemove) + sizeof(void*); }
inline bool HaveUIFont() { return Api()->size >= offsetof(FlyApi, UISetFont) + sizeof(void*); }
inline bool HaveInput() { return Api()->size >= offsetof(FlyApi, MouseOverUI) + sizeof(void*); }
}

// Panels, text, bars and images drawn over the 3D view, made by id. Sizes and offsets are in pixels of a 1280 x 720
// screen and scale with the window. Everything is removed when play starts and stops.
//
//   UI::Text("score", "Score: 0", { UI::Anchor::TopRight, -20, 20 });
//   UI::Bar("hp", 1.0f, { UI::Anchor::BottomLeft, 20, -20, 240, 22 }, { 200, 40, 40 });
//   ...each frame:  UI::SetText("score", "Score: " + std::to_string(score));  UI::SetValue("hp", health / 100.0f);
namespace UI {
// Where a widget sits in the screen. x and y move it right and down from there (so a right-anchored widget wants a
// negative x). Fill covers the whole screen, x and y as the margin.
enum class Anchor { TopLeft, TopCenter, TopRight, Left, Center, Right, BottomLeft, BottomCenter, BottomRight, Fill };
// w and h are ignored by text, which is as big as its letters. An image with w and h 0 keeps its own size.
struct Layout { Anchor anchor = Anchor::TopLeft; float x = 0, y = 0, w = 0, h = 0; };

namespace detail {
inline bool Make(const std::string& id, int kind, Layout l) {
    if (!fly::detail::HaveUI() || !fly::detail::Api()->UICreate(id.c_str(), kind)) return false;
    fly::detail::Api()->UISetLayout(id.c_str(), static_cast<int>(l.anchor), l.x, l.y, l.w, l.h);
    return true;
}
inline void Color(const std::string& id, ColorRGBA a, ColorRGBA b) {
    fly::detail::Api()->UISetColor(id.c_str(), a.r, a.g, a.b, a.a, b.r, b.g, b.b, b.a);
}
inline bool HaveQuery() { return fly::detail::Api()->size >= offsetof(FlyApi, UIGetColor) + sizeof(void*); }
}

// A UI item on screen, found by its id. Reads and changes any part of it directly; the setters return the widget, so
// calls chain. Cheap to make and copy (it only holds the id), and safe to keep: if the item is gone, reads give
// empty values and changes do nothing. Reading and the per-part setters (SetOffset, SetFontSize ...) need an engine
// with UI queries; on an older one they do nothing.
//
//   UI::Widget hp = UI::Find("hp");
//   hp.SetValue(hp.Value() - 0.1f).SetColor({ 220, 60, 60 });
//   UI::Find("score").SetText("Score: " + std::to_string(score)).SetFontSize(32).SetOffset(-30, 24);
class Widget {
public:
    Widget() = default;
    explicit Widget(std::string id) : m_id(std::move(id)) {}
    const std::string& Id() const { return m_id; }

    // ---- reading
    bool Exists() const { return detail::HaveQuery() && fly::detail::Api()->UIExists(m_id.c_str()) != 0; }
    explicit operator bool() const { return Exists(); }
    std::string Text() const {
        if (!detail::HaveQuery()) return {};
        const int n = fly::detail::Api()->UIGetText(m_id.c_str(), nullptr, 0);
        if (n <= 0) return {};
        std::vector<char> buf(static_cast<size_t>(n) + 1, '\0');
        fly::detail::Api()->UIGetText(m_id.c_str(), buf.data(), n + 1);
        return std::string(buf.data());
    }
    float Value() const { return detail::HaveQuery() ? fly::detail::Api()->UIGetValue(m_id.c_str()) : 0.0f; }
    bool Visible() const { return detail::HaveQuery() && fly::detail::Api()->UIGetVisible(m_id.c_str()) != 0; }
    // Mouse state of a button, slider or toggle. Clicked and Changed are true for one frame: poll them once a frame.
    bool Hovered() const { return State() & 1; }
    bool Held() const    { return State() & 2; }
    bool Clicked() const { return State() & 4; }   // a button or toggle was pressed and released on it
    bool Changed() const { return State() & 8; }   // a slider moved or a toggle flipped
    Layout GetLayout() const { Layout l; float v[6]; Read(v, l); return l; }
    float FontSize() const { Layout l; float v[6] = {}; Read(v, l); return v[5]; }
    ColorRGBA Color() const { ColorRGBA c, b; Colors(c, b); return c; }
    ColorRGBA Background() const { ColorRGBA c, b; Colors(c, b); return b; }

    // ---- changing
    Widget& SetText(const std::string& text) { if (fly::detail::HaveUI()) fly::detail::Api()->UISetText(m_id.c_str(), text.c_str(), 0.0f); return *this; }
    Widget& SetValue(float value) { if (fly::detail::HaveUI()) fly::detail::Api()->UISetValue(m_id.c_str(), value); return *this; }
    Widget& SetFont(const std::string& font) { if (fly::detail::HaveUIFont()) fly::detail::Api()->UISetFont(m_id.c_str(), font.c_str()); return *this; }
    Widget& SetImage(const std::string& file) { if (fly::detail::HaveUI()) fly::detail::Api()->UISetImage(m_id.c_str(), file.c_str()); return *this; }
    Widget& Show(bool visible = true) { if (fly::detail::HaveUI()) fly::detail::Api()->UISetVisible(m_id.c_str(), visible ? 1 : 0); return *this; }
    Widget& Hide() { return Show(false); }
    // The main colour (text, panel, bar fill, image tint); a bar's background stays as it is.
    Widget& SetColor(ColorRGBA color) { ColorRGBA c, b; if (Colors(c, b)) detail::Color(m_id, color, b); return *this; }
    Widget& SetBackground(ColorRGBA color) { ColorRGBA c, b; if (Colors(c, b)) detail::Color(m_id, c, color); return *this; }
    Widget& SetLayout(Layout l) { if (fly::detail::HaveUI()) fly::detail::Api()->UISetLayout(m_id.c_str(), static_cast<int>(l.anchor), l.x, l.y, l.w, l.h); return *this; }
    Widget& SetAnchor(Anchor a) { Layout l; float v[6]; if (Read(v, l)) { l.anchor = a; SetLayout(l); } return *this; }
    Widget& SetOffset(float x, float y) { Layout l; float v[6]; if (Read(v, l)) { l.x = x; l.y = y; SetLayout(l); } return *this; }
    Widget& SetSize(float w, float h) { Layout l; float v[6]; if (Read(v, l)) { l.w = w; l.h = h; SetLayout(l); } return *this; }
    Widget& SetFontSize(float size) { if (Exists()) fly::detail::Api()->UISetText(m_id.c_str(), Text().c_str(), size); return *this; }
    void Remove() { if (fly::detail::HaveUI()) fly::detail::Api()->UIRemove(m_id.c_str()); }

private:
    int State() const { return fly::detail::HaveInput() ? fly::detail::Api()->UIGetState(m_id.c_str()) : 0; }
    bool Read(float* v, Layout& l) const {
        if (!detail::HaveQuery() || !fly::detail::Api()->UIGetLayout(m_id.c_str(), v)) return false;
        l = { static_cast<Anchor>(static_cast<int>(v[0])), v[1], v[2], v[3], v[4] };
        return true;
    }
    bool Colors(ColorRGBA& c, ColorRGBA& b) const {
        unsigned char v[8] = {};
        if (!detail::HaveQuery() || !fly::detail::Api()->UIGetColor(m_id.c_str(), v)) return false;
        c = { v[0], v[1], v[2], v[3] };
        b = { v[4], v[5], v[6], v[7] };
        return true;
    }
    std::string m_id;
};

// The item with this id (check `if (w)` or w.Exists() to know whether it is there).
inline Widget Find(const std::string& id) { return Widget(id); }

// Make items. Making one again with an id that exists changes its layout and colours. Each returns the widget.
// A filled rectangle.
inline Widget Panel(const std::string& id, Layout l, ColorRGBA color = { 0, 0, 0, 140 }) {
    if (detail::Make(id, 0, l)) detail::Color(id, color, color);
    return Widget(id);
}
// `font`: "" or "Sans" (the default), "Serif", "Mono", "Pixel" (the blocky built-in one), a font file in the project's
// assets/Fonts folder ("Roboto.ttf"), or a path to one. Put the file in the project so it ships with the game; system
// fonts differ per machine.
inline Widget Text(const std::string& id, const std::string& text, Layout l = {}, ColorRGBA color = { 255, 255, 255, 255 }, float fontSize = 24.0f, const std::string& font = "") {
    if (!detail::Make(id, 1, l)) return Widget(id);
    detail::Color(id, color, color);
    fly::detail::Api()->UISetText(id.c_str(), text.c_str(), fontSize);
    if (!font.empty() && fly::detail::HaveUIFont()) fly::detail::Api()->UISetFont(id.c_str(), font.c_str());
    return Widget(id);
}
// A bar filled to `value` (0..1) in `fill` over `background`.
inline Widget Bar(const std::string& id, float value, Layout l, ColorRGBA fill = { 80, 200, 90, 255 }, ColorRGBA background = { 0, 0, 0, 160 }) {
    if (!detail::Make(id, 2, l)) return Widget(id);
    detail::Color(id, fill, background);
    fly::detail::Api()->UISetValue(id.c_str(), value);
    return Widget(id);
}
// A picture from the project ("assets/ui/heart.png"), tinted by `tint`.
inline Widget Image(const std::string& id, const std::string& file, Layout l, ColorRGBA tint = { 255, 255, 255, 255 }) {
    if (!detail::Make(id, 3, l)) return Widget(id);
    detail::Color(id, tint, tint);
    fly::detail::Api()->UISetImage(id.c_str(), file.c_str());
    return Widget(id);
}

// A clickable button. Check `UI::Find(id).Clicked()` each frame. Make the mouse visible first: UI::SetMouseFree(true).
// `color` is the face (it lightens under the mouse and darkens while pressed), `textColor` the label.
inline Widget Button(const std::string& id, const std::string& text, Layout l = { Anchor::TopLeft, 0, 0, 160, 40 }, ColorRGBA color = { 60, 90, 150, 255 },
                     ColorRGBA textColor = { 255, 255, 255, 255 }, float fontSize = 22.0f, const std::string& font = "") {
    if (!detail::Make(id, 4, l)) return Widget(id);
    detail::Color(id, color, textColor);
    fly::detail::Api()->UISetText(id.c_str(), text.c_str(), fontSize);
    if (!font.empty() && fly::detail::HaveUIFont()) fly::detail::Api()->UISetFont(id.c_str(), font.c_str());
    return Widget(id);
}
// A slider the player drags, 0..1. Read `Value()`; `Changed()` is true on the frames it moved.
inline Widget Slider(const std::string& id, float value, Layout l = { Anchor::TopLeft, 0, 0, 200, 20 }, ColorRGBA fill = { 80, 200, 90, 255 }, ColorRGBA background = { 0, 0, 0, 160 }) {
    if (!detail::Make(id, 5, l)) return Widget(id);
    detail::Color(id, fill, background);
    fly::detail::Api()->UISetValue(id.c_str(), value);
    return Widget(id);
}
// A check box with a label. `Value() > 0.5f` is on; `Changed()` is true on the frame it flipped.
inline Widget Toggle(const std::string& id, const std::string& label, bool on, Layout l = { Anchor::TopLeft, 0, 0, 200, 24 }, ColorRGBA mark = { 80, 200, 90, 255 },
                     float fontSize = 20.0f) {
    if (!detail::Make(id, 6, l)) return Widget(id);
    detail::Color(id, mark, { 0, 0, 0, 160 });
    fly::detail::Api()->UISetText(id.c_str(), label.c_str(), fontSize);
    fly::detail::Api()->UISetValue(id.c_str(), on ? 1.0f : 0.0f);
    return Widget(id);
}

// The cursor. Menus call SetMouseFree(true): the mouse is released and the first-person view stops following it; call it
// again with false to go back to looking around. MousePosition is in the same 1280 x 720 pixels as layouts.
inline void SetMouseFree(bool free) { if (fly::detail::HaveInput()) fly::detail::Api()->SetMouseFree(free ? 1 : 0); }
inline Vec3 MousePosition() {
    float p[2] = {};
    if (fly::detail::HaveInput()) fly::detail::Api()->GetMousePosition(p);
    return { p[0], p[1], 0.0f };
}
// True while the pointer is over a button, slider or toggle, so a click there should not also shoot or pick.
inline bool MouseOverUI() { return fly::detail::HaveInput() && fly::detail::Api()->MouseOverUI() != 0; }

// Shortcuts by id, the same as Find(id).SetX(...). Unknown ids are ignored.
inline void SetText(const std::string& id, const std::string& text)  { Widget(id).SetText(text); }
inline void SetFont(const std::string& id, const std::string& font)  { Widget(id).SetFont(font); }
inline void SetValue(const std::string& id, float value)             { Widget(id).SetValue(value); }
inline void SetColor(const std::string& id, ColorRGBA color)         { Widget(id).SetColor(color); }   // keeps a bar's background
inline void SetColor(const std::string& id, ColorRGBA color, ColorRGBA background) { if (fly::detail::HaveUI()) detail::Color(id, color, background); }
inline void Show(const std::string& id, bool visible = true)        { Widget(id).Show(visible); }
inline void Hide(const std::string& id)                              { Widget(id).Hide(); }
inline void Remove(const std::string& id)                            { Widget(id).Remove(); }
}

// ---- navigation ------------------------------------------------------------

namespace detail {
inline bool HaveNav() { return Api()->size >= offsetof(FlyApi, NavDebug) + sizeof(void*); }
inline bool HaveNavMove() { return Api()->size >= offsetof(FlyApi, NavMoveAlong) + sizeof(void*); }
}

// Finding the way across the level. Bake once the level is in place (usually at the start of the script), then ask for
// paths. Only ground the agent can stand on is used: floors, ramps and steps under `maxStep`, platforms, with room for
// `agentHeight` above and `agentRadius` of clearance from walls and ledges. Moving things (the player, crates) are not
// part of the map.
//
//   fly::Nav::Build({ -60, -5, -60 }, { 60, 30, 60 });
//   fly::Nav::Agent bot(4.0f);                           // 4 m/s
//   bot.SetTarget(player.Position());
//   while (true) {
//       Vec3 feet = self.Position() - fly::Vec3{ 0, 0.9f, 0 };   // a 1.8 m capsule's position is its middle
//       Vec3 v = bot.Step(feet, fly::DeltaTime());               // where to head, as a velocity
//       self.SetVelocity({ v.x, self.Velocity().y, v.z });
//       co_await fly::NextFrame();
//   }
namespace Nav {

struct Settings {
    Vec3 min{ -50, -10, -50 }, max{ 50, 40, 50 };   // the box that is baked; keep it no bigger than the level
    float cellSize = 0.5f;      // metres per grid square: smaller is more exact and slower to bake
    float agentRadius = 0.4f;
    float agentHeight = 1.8f;
    float maxStep = 0.5f;       // the highest step the agent walks up
    float maxSlope = 45.0f;     // degrees
};

// True when this engine can do navigation at all.
inline bool Available() { return detail::HaveNav(); }

// Bakes the map from what is in the world now (only while the game is playing). Returns false when the settings make no
// sense, the box has no floor, or it is too big (over 4 million squares). Baking again replaces the map.
inline bool Build(const Settings& s) {
    if (!detail::HaveNav()) return false;
    const float v[13] = { s.min.x, s.min.y, s.min.z, s.max.x, s.max.y, s.max.z, s.cellSize, s.agentRadius, s.agentHeight, s.maxStep, s.maxSlope, 0, 0 };
    return detail::Api()->NavBuild(v) != 0;
}
inline bool Build(Vec3 min, Vec3 max) { Settings s; s.min = min; s.max = max; return Build(s); }
inline void Clear() { if (detail::HaveNav()) detail::Api()->NavClear(); }
inline bool Ready() { return detail::HaveNav() && detail::Api()->NavReady() != 0; }

struct Path {
    std::vector<Vec3> points;   // the corners from start to end, start first
    bool partial = false;       // the goal could not be reached; the path ends at the nearest point that can
    bool Valid() const { return !points.empty(); }
    float Length() const {
        float len = 0;
        for (size_t i = 1; i < points.size(); ++i) {
            const Vec3 d = points[i] - points[i - 1];
            len += std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        }
        return len;
    }
};

// A path from `from` to `to`. Both are moved onto the nearest walkable ground first. Empty when there is no way (or no
// map). With `allowPartial` an unreachable goal gives the path to the closest point that can be reached.
inline Path FindPath(Vec3 from, Vec3 to, bool allowPartial = false, int maxPoints = 256) {
    Path path;
    if (!detail::HaveNav() || maxPoints <= 0) return path;
    const float a[3] = { from.x, from.y, from.z }, b[3] = { to.x, to.y, to.z };
    std::vector<float> buf(static_cast<size_t>(maxPoints) * 3);
    const int n = detail::Api()->NavFindPath(a, b, buf.data(), maxPoints, allowPartial ? 1 : 0);
    path.partial = n < 0;
    for (int i = 0; i < (n < 0 ? -n : n); ++i) path.points.push_back({ buf[i * 3], buf[i * 3 + 1], buf[i * 3 + 2] });
    return path;
}

// The walkable point nearest `p`; false if none is near.
inline bool Nearest(Vec3 p, Vec3& out) {
    if (!detail::HaveNav()) return false;
    const float a[3] = { p.x, p.y, p.z };
    float o[3] = {};
    if (!detail::Api()->NavNearest(a, o)) return false;
    out = { o[0], o[1], o[2] };
    return true;
}
// True when `p` is standing on baked ground.
inline bool IsWalkable(Vec3 p) {
    if (!detail::HaveNav()) return false;
    const float a[3] = { p.x, p.y, p.z };
    return detail::Api()->NavIsWalkable(a) != 0;
}
// A random walkable point within `radius` of `center`: a place to patrol to.
inline bool RandomPoint(Vec3 center, float radius, Vec3& out) {
    if (!detail::HaveNav()) return false;
    const float a[3] = { center.x, center.y, center.z };
    float o[3] = {};
    if (!detail::Api()->NavRandomPoint(a, radius, o)) return false;
    out = { o[0], o[1], o[2] };
    return true;
}
// Draws the map (green squares, orange at the edges) and the last path asked for in the 3D view.
inline void ShowDebug(bool on = true) { if (detail::HaveNav()) detail::Api()->NavDebug(on ? 1 : 0); }

// True when an agent standing at `from` can walk in a straight line to `to` without leaving the map. `from` may be
// anywhere inside a square, not just its middle. False on an engine without it (check with Nav::CanMove()) or no map.
inline bool CanMove() { return detail::HaveNav() && detail::HaveNavMove(); }
inline bool LineClear(Vec3 from, Vec3 to) {
    if (!CanMove()) return false;
    const float a[3] = { from.x, from.y, from.z }, b[3] = { to.x, to.y, to.z };
    return detail::Api()->NavLineClear(a, b) != 0;
}
// Moves `from` by `delta` (only its flat part counts) over walkable ground and puts the result in `out`. Where a wall
// or ledge is in the way it slides along it instead of going through, so `out` is always on the map, and `out.y` is the
// ground height there. A `from` just off the map is pulled back onto it. Returns false when there is no map, no
// ground near `from` or the engine is too old; `out` is then simply `from + delta`, so it is safe to use either way.
inline bool MoveAlong(Vec3 from, Vec3 delta, Vec3& out) {
    out = from + delta;
    if (!CanMove()) return false;
    const float a[3] = { from.x, from.y, from.z }, d[3] = { delta.x, delta.y, delta.z };
    float o[3] = {};
    if (!detail::Api()->NavMoveAlong(a, d, o)) return false;
    out = { o[0], o[1], o[2] };
    return true;
}

// Walks a path for you. Give it a target and call Move every frame: it finds the path, moves the agent along it over
// walkable ground and slides along walls, so the agent cannot leave the map. Or call Step with where the agent is and
// move it yourself at the velocity it returns (flat: y is 0). It asks for a new path every `repathSeconds` and when the
// target moves, so it follows a moving target and works round things that changed.
//
// Before heading for the next corner it checks that the straight line from where the agent really is (not the middle
// of its square) is clear; if not it goes back to the corner it came from or asks for a new path (at most every 0.25 s).
// If it wants to move but has not got anywhere for `SetStuckTime` seconds it asks for a new path; if that does not help
// either, Stuck() turns true until it moves again. A target that keeps moving does not reset this. It never picks
// another target by itself: that is up to you.
// Keep `arriveDistance` no bigger than the map's cell size (0.5 m by default).
class Agent {
public:
    explicit Agent(float speed = 3.0f, float repathSeconds = 0.5f, float arriveDistance = 0.4f)
        : m_speed(speed), m_repath(repathSeconds), m_arrive(arriveDistance) {}

    // Safe to call every frame with a moving target: a new path is only asked for once it has moved over a metre.
    void SetTarget(Vec3 target) {
        const bool far = !m_hasTarget || Flat(target - m_pathTarget) > 1.0f;
        m_target = target;
        m_hasTarget = true;
        // Only the path is redone: a chased target moves all the time, and that must not hide that the agent is stuck.
        if (far) { m_arrived = false; m_timer = 0.0f; }
    }
    void ClearTarget() { m_hasTarget = false; m_path.points.clear(); m_arrived = false; ResetStuck(); }
    void SetSpeed(float speed) { m_speed = speed; }
    // How long the agent may want to move without getting anywhere before it asks for a new path (1 s by default;
    // 0 turns the check off).
    void SetStuckTime(float seconds) { m_stuckTime = seconds; }
    bool HasTarget() const { return m_hasTarget; }
    // True once the agent reached the end of its path (also when the goal was unreachable and it got as near as it could).
    bool Arrived() const { return m_arrived; }
    // False when no way was found to (or towards) the target on the last try.
    bool HasPath() const { return m_path.Valid(); }
    // True when it has not moved for two stuck times in a row, a new path in between. Pick another target, or wait.
    bool Stuck() const { return m_stuck; }
    const Path& CurrentPath() const { return m_path; }
    // The corner the agent is heading for.
    Vec3 NextPoint() const { return m_next < m_path.points.size() ? m_path.points[m_next] : m_target; }

    // Where to head right now, as a velocity of `speed` metres a second along the ground. Zero when there is nothing to do.
    // `position` is where the agent stands (its feet).
    Vec3 Step(Vec3 position, float dt) {
        if (!m_hasTarget || m_arrived) { ResetStuck(); return {}; }
        m_timer -= dt;
        m_retry -= dt;
        // A failed search waits for the timer too: with no way to the target it would otherwise search every frame.
        if (m_timer <= 0.0f) Repath(position);
        const size_t count = m_path.points.size();
        bool exact = false;   // the corner has to be reached, not just come near
        while (m_next < count && Flat(m_path.points[m_next] - position) <= m_arrive) {
            // The last corner is the end: stop there.
            if (m_next + 1 == count) { m_arrived = true; ResetStuck(); return {}; }
            // Cutting the corner is only fine when the leg after it is straight ahead from here; otherwise finish it.
            if (Flat(m_path.points[m_next] - position) > 0.02f && !Clear(position, m_path.points[m_next + 1])) { exact = true; break; }
            ++m_next;
        }
        if (m_next >= count) return {};
        Vec3 aim = m_path.points[m_next];
        if (!Clear(position, aim)) {
            // Pushed off the line: back to the corner it came from, whose leg is known to be clear, or a new path.
            if (m_next > 0 && Clear(position, m_path.points[m_next - 1])) { aim = m_path.points[m_next - 1]; exact = true; }
            else if (m_retry <= 0.0f) { Repath(position); aim = NextPoint(); }
        }
        Vec3 d = aim - position;
        d.y = 0.0f;
        const float len = std::sqrt(d.x * d.x + d.z * d.z);
        float speed = m_speed;
        if (exact && dt > 0.0f && len < speed * dt) speed = len / dt;   // land on it instead of overshooting
        const Vec3 v = len > 1e-5f ? d * (speed / len) : Vec3{};
        TrackProgress(position, dt);
        return v;
    }

    // Steps and moves `feet` (where the agent stands) along the ground with Nav::MoveAlong, so it slides along walls and
    // never leaves the map; `feet.y` follows the ground. Returns the flat velocity it really moved at (the distance moved
    // over `dt`): slower than `speed` while sliding along a wall, zero when it did not move. If `feet` is nowhere near
    // the map it stays where it is. On an engine without MoveAlong it moves straight, as Step says.
    Vec3 Move(Vec3& feet, float dt) {
        const Vec3 v = Step(feet, dt);
        if ((v.x == 0.0f && v.z == 0.0f) || !(dt > 0.0f)) return {};
        Vec3 out;
        if (!MoveAlong(feet, v * dt, out) && CanMove()) return {};
        const Vec3 moved{ (out.x - feet.x) / dt, 0.0f, (out.z - feet.z) / dt };
        feet = out;
        return moved;
    }
    // The same for an anchored object that is moved by setting its position. `feetToCentre` is how far its position
    // sits above its feet: half the height for a cube or capsule whose middle is its position, 0 for a model whose
    // pivot is at its feet. Not for objects physics moves (use Step and SetVelocity for those).
    Vec3 Move(Object body, float dt, float feetToCentre = 0.0f) {
        Vec3 feet = body.Position() - Vec3{ 0.0f, feetToCentre, 0.0f };
        const Vec3 v = Move(feet, dt);
        if (v.x != 0.0f || v.z != 0.0f) body.SetPosition(feet + Vec3{ 0.0f, feetToCentre, 0.0f });
        return v;
    }

private:
    static float Flat(Vec3 d) { return std::sqrt(d.x * d.x + d.z * d.z); }
    // An engine without the line check walks the legs as they come, like before.
    static bool Clear(Vec3 a, Vec3 b) { return !detail::HaveNavMove() || LineClear(a, b); }
    void Repath(Vec3 position) {
        m_timer = m_repath;
        m_retry = std::min(m_repath, 0.25f);
        m_path = FindPath(position, m_target, true);
        m_pathTarget = m_target;
        m_next = m_path.points.size() > 1 ? 1 : 0;
    }
    void ResetStuck() { m_stuckClock = 0.0f; m_stuck = false; m_stuckRepathed = false; m_anchorSet = false; }
    // Progress is getting a quarter of what it could walk in a stuck time away from where it last made progress, so
    // shuffling on the spot against a wall still counts as stuck.
    void TrackProgress(Vec3 position, float dt) {
        if (m_stuckTime <= 0.0f) return;
        const float need = std::max(0.1f, 0.25f * m_speed * m_stuckTime);
        if (!m_anchorSet || Flat(position - m_anchor) >= need) {
            m_anchor = position;
            m_anchorSet = true;
            m_stuckClock = 0.0f;
            m_stuck = false;
            m_stuckRepathed = false;
            return;
        }
        m_stuckClock += dt;
        if (m_stuckClock < m_stuckTime) return;
        m_stuckClock = 0.0f;
        if (!m_stuckRepathed) { m_stuckRepathed = true; Repath(position); }
        else m_stuck = true;
    }

    float m_speed, m_repath, m_arrive;
    float m_stuckTime = 1.0f;
    Vec3 m_target, m_pathTarget, m_anchor;
    bool m_hasTarget = false, m_arrived = false;
    bool m_stuck = false, m_stuckRepathed = false, m_anchorSet = false;
    float m_timer = 0.0f, m_retry = 0.0f, m_stuckClock = 0.0f;
    Path m_path;
    size_t m_next = 0;
};

} // namespace Nav

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
