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

#include <coroutine>
#include <cstddef>
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
