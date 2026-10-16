#pragma once
// Sound rays: sounds that come from an object, and are muffled and echoed by the world around it.
//
// Play Sound with an object hands its request to Queue(); Update(), called every frame while playing, starts the sound at the
// object, keeps it there as the object moves, and measures the world with rays:
//   Muffle  rays from the object to the listener; the share of them a wall blocks, and how thick it is, close a low-pass filter
//           and lower the volume. Smoothed, so walking behind a wall fades rather than clicks.
//   Echo    rays out of the object in every direction; how close and how many of them hit a wall (how enclosed the room is) set an
//           echo's delay (fixed when the sound starts), how long it rings and how loud it is.
// The measuring is plain functions over a ray callback, so it can be tested without a physics world.
#include "VisualScript.hpp"

#include <functional>
#include <string>

class ScriptRuntime;

namespace soundrays {

struct Vec3 { float x = 0, y = 0, z = 0; };
struct RayHit { bool hit = false; float distance = 0; const char* name = nullptr; };   // name: what was hit, for the debug log
// Casts a ray from `origin` along the unit vector `dir`, at most `maxDistance` long; the closest thing it meets.
using RayFn = std::function<RayHit(const Vec3& origin, const Vec3& dir, float maxDistance)>;

struct Occlusion {
    float openness = 1.0f;       // 1 = clear path, 0 = fully shut in behind thick walls (after nearness and detours)
    float cutoffHz = 22000.0f;   // low-pass cutoff to apply
    float gain = 1.0f;           // volume factor to apply
    float blockedShare = 0.0f;   // fraction of the direct rays that hit something
    float thickness = 0.0f;      // average thickness (m) of what blocked them
    float direct = 1.0f;         // openness of the straight line alone
    float detour = 0.0f;         // openness of the best way round the blocker (0 = none found); see MeasureOcclusion
    float nearness = 0.0f;       // 1 = so close to the source that nothing muffles it, fading to 0 farther out
    const char* blocker = nullptr;   // what the first blocked ray hit, for the debug log
};
// How the muffling is judged:
//  * The straight line source -> listener is probed with 5 rays (centre, left, right, above, higher above; never below the source,
//    so a floor under it does not count).
//  * A hit within 0.5 m of the source's surface, or within ~0.8 m of the listener (your own body, the floor under a fan, its mount)
//    is not a wall: the ray carries on past it.
//  * Within `nearA` of the source (about 2 object sizes, at least 1.5 m) nothing muffles, fading in over the next 3 m.
//  * When the line is blocked, a few detours round the blocker are tried (sideways, over it); an open one (a door, a corner)
//    leaves the sound clearer than the wall alone would. Detours cost more rays, so they are only redone when `refreshDetour`;
//    otherwise `*detourCache` (the last result, 0 = none) is used.
// `sourceRadius`: rays start just outside the source's own bounds, so it never blocks itself.
Occlusion MeasureOcclusion(const RayFn& ray, const Vec3& source, const Vec3& listener, float sourceRadius,
                           float* detourCache = nullptr, bool refreshDetour = true);

constexpr int kEchoRays = 12;
constexpr float kEchoMaxDistance = 40.0f;
Vec3 EchoDirection(int index);                    // 0..kEchoRays-1, evenly spread over the sphere

struct Room {
    float enclosed = 0.0f;       // fraction of the rays that hit a wall
    float meanDistance = 0.0f;   // average distance to those walls
    float delaySeconds = 0.03f;  // the echo's delay: sound out to a wall and back
    float feedback = 0.15f;      // how long it rings, 0..0.9
    float wet = 0.0f;            // how loud it is, 0..1
};
// `distances[i] < 0` means ray i found nothing within kEchoMaxDistance.
Room SummarizeRoom(const float* distances, int count);
void MeasureRoom(const RayFn& ray, const Vec3& source, float sourceRadius, float* distances /*kEchoRays*/);

// --- In the running game -------------------------------------------------------------------------------------------------
void Queue(const vscript::SoundRequest& request);                  // an object sound to start on the next Update
void Update(ScriptRuntime& rt, float dt);
void StopSounds(const std::string& path, const std::string& objectPath);   // Stop Sound
void Clear();                                                      // Play ended: forget every emitter
int  ActiveEmitters();

} // namespace soundrays
