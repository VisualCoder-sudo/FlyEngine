#include "../../../include/Engine/Backend/SoundRays.hpp"
#include "../../../include/Engine/Audio.hpp"
#include "../../../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace soundrays {
namespace {

Vec3 operator+(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 operator-(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3 operator*(const Vec3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
Vec3 Normalized(const Vec3& a) { const float l = Length(a); return l > 1e-6f ? a * (1.0f / l) : Vec3{ 0, 1, 0 }; }

constexpr float kListenerClear = 0.6f;     // rays stop this far from the listener: the player's own body never blocks
constexpr float kSourceClear = 0.05f;      // and start this far outside the source's bounds
constexpr float kOpenHz = 22000.0f, kShutHz = 400.0f;
constexpr float kBlockedGain = 0.3f;
constexpr float kSpeedOfSound = 343.0f;

} // namespace

namespace {

constexpr float kSourceZone = 0.5f;     // hits this close to the source's surface are the source's own surroundings
constexpr float kListenerZone = 0.2f;   // ...and this much nearer the listener than kListenerClear: the listener's surroundings
constexpr float kNearMin = 1.5f;        // nothing muffles within this far, or twice the object's size
constexpr float kNearFade = 3.0f;       // ...and the muffling fades in over this much more
constexpr float kDetourBest = 0.8f;     // a perfect detour (no extra length) leaves this much of the sound open

float Smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct Cast { bool hit = false; float distance = 0; const char* name = nullptr; };

// Casts from `a` along `dir` for `span`, carrying on past hits that lie in the ends' zones (line coordinate `s` along source->listener
// outside [sMin, sMax]): they are the source's or listener's surroundings, not a wall between them.
Cast CastPath(const RayFn& ray, const Vec3& a, const Vec3& dir, float span, const Vec3& source, const Vec3& d, float sMin, float sMax) {
    float travelled = 0.0f;
    for (int i = 0; i < 4 && travelled < span; ++i) {
        const RayHit h = ray(a + dir * travelled, dir, span - travelled);
        if (!h.hit) return {};
        const float dist = travelled + h.distance;
        const float s = Dot((a + dir * dist) - source, d);
        if (s < sMin || s > sMax) { travelled = dist + 0.03f; continue; }
        return { true, dist, h.name };
    }
    return {};
}

float DirectOpenness(float share, float thickness) {
    // A thin pane still muffles a little; half a metre of wall (or more) is the full effect for the rays it blocks.
    return 1.0f - share * (0.5f + 0.5f * std::min(thickness / 0.5f, 1.0f));
}

} // namespace

Occlusion MeasureOcclusion(const RayFn& ray, const Vec3& source, const Vec3& listener, float sourceRadius,
                           float* detourCache, bool refreshDetour) {
    Occlusion out;
    const Vec3 line = listener - source;
    const float length = Length(line);
    const float start = std::max(0.0f, sourceRadius) + kSourceClear;   // along the line, where the rays leave the source
    const float stop = length - kListenerClear;                        // ...and where they end, short of the listener
    const float nearA = std::max(kNearMin, sourceRadius * 2.0f);
    out.nearness = 1.0f - Smooth(nearA, nearA + kNearFade, length);
    if (stop - start < 0.2f || out.nearness >= 0.999f) {               // too close for anything to be in between
        out.direct = out.openness = 1.0f;
        if (detourCache) *detourCache = 0.0f;
        return out;
    }
    const Vec3 d = line * (1.0f / length);
    const Vec3 up = std::fabs(d.y) > 0.95f ? Vec3{ 1, 0, 0 } : Vec3{ 0, 1, 0 };
    const Vec3 u = Normalized(Cross(d, up));
    Vec3 v = Cross(d, u);
    if (v.y < 0.0f) v = v * -1.0f;                                     // "v" points up: sample points never go below the source
    const float r = std::clamp(sourceRadius * 0.6f, 0.15f, 0.8f);
    const Vec3 offsets[5] = { {}, u * r, u * -r, v * r, v * (r * 1.8f) };
    const Vec3 end = source + d * stop;
    const float sMin = start + kSourceZone, sMax = stop - kListenerZone;

    int blocked = 0;
    float thickness = 0.0f;
    Vec3 pivot = end;
    for (const Vec3& off : offsets) {
        const Vec3 a = source + off + d * start;           // leaves the source
        const Vec3 toEnd = end - a;
        const float span = Length(toEnd);
        if (span < 0.05f) continue;
        const Vec3 dir = toEnd * (1.0f / span);
        const Cast fwd = CastPath(ray, a, dir, span, source, d, sMin, sMax);
        if (!fwd.hit) continue;
        if (!blocked) { pivot = a + dir * fwd.distance; out.blocker = fwd.name; }
        ++blocked;
        // From the listener's side: where is the wall's far face? What lies between the two hits is the wall.
        const Cast back = CastPath(ray, end, dir * -1.0f, span, source, d, sMin, sMax);
        thickness += back.hit ? std::max(0.0f, span - fwd.distance - back.distance) : 0.0f;
    }
    out.blockedShare = (float)blocked / 5.0f;
    out.thickness = blocked ? thickness / (float)blocked : 0.0f;
    out.direct = DirectOpenness(out.blockedShare, out.thickness);

    // Round the blocker: waypoints beside and above where the line was stopped; a clear path through one is a detour.
    float detour = 0.0f;
    if (blocked && out.direct < 0.95f) {
        if (refreshDetour || !detourCache) {
            const Vec3 from = source + d * start;
            const Vec3 offs[7] = { u * 2.0f, u * -2.0f, u * 5.0f, u * -5.0f, v * 3.0f, u * 3.0f + v * 3.0f, u * -3.0f + v * 3.0f };
            for (const Vec3& o : offs) {
                const Vec3 w = pivot + o;
                const Vec3 leg1 = w - from, leg2 = end - w;
                const float l1 = Length(leg1), l2 = Length(leg2);
                if (l1 < 0.1f || l2 < 0.1f) continue;
                const float extra = l1 + l2 - Length(end - from);
                const float openness = kDetourBest / (1.0f + std::max(0.0f, extra) / 6.0f);
                if (openness <= detour) continue;               // cannot beat the best so far: skip its rays
                if (CastPath(ray, from, leg1 * (1.0f / l1), l1, source, d, sMin, sMax).hit) continue;
                if (CastPath(ray, w, leg2 * (1.0f / l2), l2, source, d, sMin, sMax).hit) continue;
                detour = openness;
            }
            if (detourCache) *detourCache = detour;
        } else {
            detour = *detourCache;
        }
    } else if (detourCache) {
        *detourCache = 0.0f;
    }
    out.detour = detour;

    // Nearness blends everything toward open; a detour can only help.
    out.openness = std::max(out.direct, detour);
    out.openness = out.openness + (1.0f - out.openness) * out.nearness;
    out.cutoffHz = kOpenHz * std::pow(kShutHz / kOpenHz, 1.0f - out.openness);
    out.gain = kBlockedGain + (1.0f - kBlockedGain) * out.openness;
    return out;
}

Vec3 EchoDirection(int index) {
    const int i = std::clamp(index, 0, kEchoRays - 1);
    const float y = 1.0f - 2.0f * ((float)i + 0.5f) / (float)kEchoRays;
    const float radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
    const float phi = (float)i * 2.39996323f;   // the golden angle
    return { std::cos(phi) * radius, y, std::sin(phi) * radius };
}

Room SummarizeRoom(const float* distances, int count) {
    Room room;
    int hits = 0;
    float sum = 0.0f;
    for (int i = 0; i < count; ++i) if (distances[i] >= 0.0f) { ++hits; sum += distances[i]; }
    if (count <= 0 || hits == 0) return room;                       // open ground: no echo
    room.enclosed = (float)hits / (float)count;
    room.meanDistance = sum / (float)hits;
    room.delaySeconds = std::clamp(2.0f * room.meanDistance / kSpeedOfSound, 0.03f, 0.6f);   // out to a wall and back
    room.feedback = 0.15f + 0.45f * room.enclosed;
    room.wet = room.enclosed * (0.25f + 0.35f * std::clamp(room.meanDistance / 8.0f, 0.0f, 1.0f));
    return room;
}

void MeasureRoom(const RayFn& ray, const Vec3& source, float sourceRadius, float* distances) {
    const float start = std::max(0.0f, sourceRadius) + kSourceClear;
    for (int i = 0; i < kEchoRays; ++i) {
        const Vec3 d = EchoDirection(i);
        const RayHit h = ray(source + d * start, d, kEchoMaxDistance);
        distances[i] = h.hit ? h.distance + start : -1.0f;
    }
}

// --- In the running game -------------------------------------------------------------------------------------------------
namespace {

struct Emitter {
    audio::Handle handle = 0;
    std::string path, objectPath;
    bool muffle = false, echo = false;
    float range = 30.0f;
    float openness = 1.0f;                 // smoothed
    float detourCache = 0.0f;              // last detour result (re-measured every kDetourEvery seconds)
    float detourTimer = 0.0f;
    float room[kEchoRays];                 // latest echo ray distances
    int nextRay = 0;
    float debugTimer = 0.0f;
};

std::vector<vscript::SoundRequest> g_pending;
std::vector<Emitter> g_emitters;

Vec3 ToVec(const Vector3& v) { return { v.x, v.y, v.z }; }

// Where an object's sound comes from, and how big it is (rays start outside this).
bool SourceOf(ScatteredObject* obj, Vec3& pos, float& radius) {
    if (!obj) return false;
    const BoundingBox box = obj->GetBoundingBox();
    pos = { (box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f, (box.min.z + box.max.z) * 0.5f };
    const Vec3 half = { (box.max.x - box.min.x) * 0.5f, (box.max.y - box.min.y) * 0.5f, (box.max.z - box.min.z) * 0.5f };
    radius = std::clamp(Length(half), 0.15f, 4.0f);
    return true;
}

RayFn MakeRayFn(ScriptRuntime& rt, ScatteredObject* source) {
    phys::Simulation* sim = rt.GetSimulation();
    return [sim, source](const Vec3& o, const Vec3& d, float maxDistance) -> RayHit {
        if (!sim || maxDistance <= 0.0f) return {};
        const phys::RaycastHit h = sim->RayCast(Vector3{ o.x, o.y, o.z }, Vector3{ d.x, d.y, d.z }, maxDistance, source);
        return { h.hit, h.fraction * maxDistance, h.object ? h.object->GetName().c_str() : "terrain/ground" };
    };
}

bool DebugOn() { static const bool on = std::getenv("FLY_SOUND_DEBUG") != nullptr; return on; }

void Start(ScriptRuntime& rt, const vscript::SoundRequest& req) {
    ScatteredObject* obj = vscript::ResolveObject(rt, req.objectPath);
    Vec3 pos; float radius = 0.0f;
    if (!SourceOf(obj, pos, radius)) return;
    const RayFn ray = MakeRayFn(rt, obj);
    Vec3 listener;
    audio::GetListener(listener.x, listener.y, listener.z);

    Emitter e;
    e.path = req.path; e.objectPath = req.objectPath; e.muffle = req.muffle; e.echo = req.echo; e.range = req.range;
    for (float& d : e.room) d = -1.0f;

    audio::PlayParams p;
    p.volume = req.volume;
    p.loop = req.loop;
    p.minDistance = std::min(1.0f, req.range * 0.5f);
    p.maxDistance = req.range;
    // Measure before the first sample plays, so the sound never starts unmuffled behind a wall.
    if (req.muffle) {
        const Occlusion o = MeasureOcclusion(ray, pos, listener, radius, &e.detourCache, true);
        e.openness = o.openness;
        p.muffle = true;
        p.muffleCutoff = o.cutoffHz;
        p.muffleGain = o.gain;
    }
    if (req.echo) {
        MeasureRoom(ray, pos, radius, e.room);
        const Room room = SummarizeRoom(e.room, kEchoRays);
        p.echoDelay = room.delaySeconds;
        p.echoWet = room.wet;
        p.echoFeedback = room.feedback;
    }
    e.handle = audio::PlayAt(req.path, pos.x, pos.y, pos.z, p);
    if (e.handle == 0) return;
    g_emitters.push_back(std::move(e));
}

} // namespace

void Queue(const vscript::SoundRequest& request) { g_pending.push_back(request); }

void Update(ScriptRuntime& rt, float dt) {
    if (!g_pending.empty()) {
        std::vector<vscript::SoundRequest> todo;
        todo.swap(g_pending);
        for (const auto& req : todo) Start(rt, req);
    }
    if (g_emitters.empty()) return;

    Vec3 listener;
    audio::GetListener(listener.x, listener.y, listener.z);
    const float smoothing = 1.0f - std::exp(-std::max(dt, 0.0f) / 0.08f);

    for (size_t i = 0; i < g_emitters.size();) {
        Emitter& e = g_emitters[i];
        ScatteredObject* obj = vscript::ResolveObject(rt, e.objectPath);
        Vec3 pos; float radius = 0.0f;
        if (!audio::IsPlaying(e.handle) || !SourceOf(obj, pos, radius)) {   // finished, or its object is gone
            if (audio::IsPlaying(e.handle)) audio::Stop(e.handle);
            g_emitters.erase(g_emitters.begin() + (ptrdiff_t)i);
            continue;
        }
        audio::SetPosition(e.handle, pos.x, pos.y, pos.z);

        const float dist = Length(listener - pos);
        if (dist <= e.range) {   // out of earshot there is nothing to shape
            const RayFn ray = MakeRayFn(rt, obj);
            if (e.muffle) {
                const bool refresh = (e.detourTimer -= dt) <= 0.0f;
                if (refresh) e.detourTimer = 0.25f;
                const Occlusion o = MeasureOcclusion(ray, pos, listener, radius, &e.detourCache, refresh);
                e.openness += (o.openness - e.openness) * smoothing;
                const float cutoff = kOpenHz * std::pow(kShutHz / kOpenHz, 1.0f - e.openness);
                audio::SetMuffle(e.handle, cutoff, kBlockedGain + (1.0f - kBlockedGain) * e.openness);
                if (DebugOn() && (e.debugTimer -= dt) <= 0.0f) {
                    e.debugTimer = 1.0f;
                    ui::LogAlways("[soundrays] %s: %.1f m away, near %.0f%%, blocked %.0f%% by %s, wall %.2f m, direct %.2f, detour %.2f -> openness %.2f, cutoff %.0f Hz",
                                  e.objectPath.c_str(), dist, o.nearness * 100.0f, o.blockedShare * 100.0f, o.blocker ? o.blocker : "nothing",
                                  o.thickness, o.direct, o.detour, e.openness, cutoff);
                }
            }
            if (e.echo) {
                const float start = std::max(0.0f, radius) + kSourceClear;
                for (int k = 0; k < 4; ++k) {   // a few directions per frame: the room is re-read about every three frames
                    const int idx = e.nextRay;
                    e.nextRay = (e.nextRay + 1) % kEchoRays;
                    const Vec3 d = EchoDirection(idx);
                    const RayHit h = ray(pos + d * start, d, kEchoMaxDistance);
                    e.room[idx] = h.hit ? h.distance + start : -1.0f;
                }
                const Room room = SummarizeRoom(e.room, kEchoRays);
                audio::SetEcho(e.handle, room.wet, room.feedback);
                if (DebugOn() && !e.muffle && (e.debugTimer -= dt) <= 0.0f) {
                    e.debugTimer = 1.0f;
                    ui::LogAlways("[soundrays] %s: room %.0f%% enclosed, walls %.1f m away, echo wet %.2f", e.objectPath.c_str(),
                                  room.enclosed * 100.0f, room.meanDistance, room.wet);
                }
            }
        }
        ++i;
    }
}

void StopSounds(const std::string& path, const std::string& objectPath) {
    if (objectPath.empty()) audio::StopByPath(path);
    for (size_t i = 0; i < g_emitters.size();) {
        Emitter& e = g_emitters[i];
        if (e.path == path && (objectPath.empty() || e.objectPath == objectPath)) {
            audio::Stop(e.handle);
            g_emitters.erase(g_emitters.begin() + (ptrdiff_t)i);
        } else {
            ++i;
        }
    }
    g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(), [&](const vscript::SoundRequest& r) {
        return r.path == path && (objectPath.empty() || r.objectPath == objectPath);
    }), g_pending.end());
}

void Clear() { g_pending.clear(); g_emitters.clear(); }
int ActiveEmitters() { return (int)g_emitters.size(); }

} // namespace soundrays
