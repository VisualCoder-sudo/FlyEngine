#pragma once
#include "raylib.h"

class ScatteredObject;

// Particles and decals: puffs of smoke and dust, fire, sparks, splashes, debris, and marks left on surfaces.
// Everything is simulated on the CPU in a fixed-size pool and drawn as camera-facing quads (no per-effect
// objects to manage). Engine::Update() calls Update(), Engine::Draw() calls Draw() inside the 3D pass, and the
// physics simulation reports hard hits through OnImpact(), so falling and colliding objects kick up dust by
// themselves. Scripts reach the same functions through fly::Emit / fly::Decal (ScriptingSDK/include/fly.hpp).
namespace fx {

enum class Preset { Smoke, Fire, Sparks, Dust, Splash, Debris, Steam, Count };

const char* PresetName(Preset p);                   // "Smoke", "Fire", ...
bool PresetFromName(const char* name, Preset& out); // case-insensitive

// One-shot puff: `count` particles (0 = the preset's own burst size) leaving `position` around `direction`.
// `scale` multiplies particle size and speed.
void Emit(Preset preset, Vector3 position, Vector3 direction = { 0.0f, 1.0f, 0.0f }, int count = 0, float scale = 1.0f);

// A continuous emitter (a campfire, a chimney). Returns an id (> 0), or 0 when the table is full. It keeps
// running until StopEmitter(); Clear() removes them all.
int  StartEmitter(Preset preset, Vector3 position, Vector3 direction = { 0.0f, 1.0f, 0.0f }, float scale = 1.0f);
void MoveEmitter(int id, Vector3 position, Vector3 direction);
void StopEmitter(int id);

// A flat mark on a surface (a scorch, a stain, a bullet hole). `normal` is the surface's outward direction,
// `size` the diameter in metres. It fades out over the last quarter of `seconds`. The oldest decal is
// replaced when the ring is full.
void Decal(Vector3 position, Vector3 normal, float size, Color color, float seconds = 30.0f);

void Update(float dt);
void Draw(const Camera3D& camera);    // call inside the 3D pass, after the opaque scene
void Clear();                         // every particle, emitter and decal (Play stopped)

// A hard contact reported by the physics simulation. Does nothing unless auto impacts are on and `speed` is
// large enough to matter; otherwise kicks up dust and leaves a faint scuff.
void OnImpact(Vector3 point, Vector3 normal, float speed);
void SetAutoImpacts(bool on);
bool AutoImpacts();

int ParticleCount();
int DecalCount();
int EmitterCount();

} // namespace fx
