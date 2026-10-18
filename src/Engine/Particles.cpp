#include "../../include/Engine/Particles.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fx {

namespace {

constexpr int kMaxParticles = 6000;
constexpr int kMaxDecals = 256;
constexpr int kMaxEmitters = 64;

struct Params {
    float lifeMin, lifeMax;        // seconds
    float speedMin, speedMax;      // metres a second
    float spreadDeg;               // half-angle of the cone round the emit direction
    float gravity;                 // metres a second squared (negative falls; positive rises)
    float drag;                    // fraction of velocity lost per second
    float sizeStart, sizeEnd;      // metres
    Color start, end;
    bool additive;
    float spin;                    // degrees a second, up to
    int burst;                     // particles in an Emit() with count 0
    float rate;                    // particles a second from an emitter
};

//                                life        speed       spread grav  drag  size          start colour               end colour                 add  spin burst rate
const Params kPresets[(int)Preset::Count] = {
    /* Smoke  */ { 1.8f, 3.2f,   0.6f, 1.4f,   25.0f, 0.9f,  0.35f, 0.35f, 1.6f, { 90, 90, 90, 150 },    { 140, 140, 140, 0 },  false, 40.0f, 10, 8.0f },
    /* Fire   */ { 0.5f, 1.0f,   0.8f, 2.0f,   18.0f, 2.2f,  0.6f,  0.45f, 0.08f, { 255, 190, 70, 230 },  { 220, 40, 10, 0 },    true,  0.0f,  16, 40.0f },
    /* Sparks */ { 0.4f, 1.1f,   4.0f, 9.0f,   60.0f, -9.0f, 0.2f,  0.07f, 0.02f, { 255, 220, 120, 255 }, { 255, 90, 20, 0 },    true,  0.0f,  18, 30.0f },
    /* Dust   */ { 0.5f, 1.1f,   0.8f, 2.6f,   80.0f, 0.2f,  1.8f,  0.25f, 1.2f, { 150, 130, 105, 120 }, { 150, 130, 105, 0 },  false, 25.0f, 8,  14.0f },
    /* Splash */ { 0.4f, 0.9f,   2.0f, 5.0f,   35.0f, -9.0f, 0.3f,  0.14f, 0.05f, { 200, 225, 255, 200 }, { 200, 225, 255, 0 },  false, 0.0f,  20, 30.0f },
    /* Debris */ { 0.6f, 1.4f,   2.0f, 6.0f,   55.0f, -9.8f, 0.4f,  0.12f, 0.10f, { 120, 105, 90, 255 },  { 120, 105, 90, 0 },   false, 300.0f, 8, 20.0f },
    /* Steam  */ { 1.0f, 2.0f,   1.0f, 2.0f,   15.0f, 1.4f,  0.3f,  0.25f, 1.0f, { 230, 235, 240, 100 }, { 240, 245, 250, 0 },  false, 20.0f, 10, 14.0f },
};

const char* const kPresetNames[(int)Preset::Count] = { "Smoke", "Fire", "Sparks", "Dust", "Splash", "Debris", "Steam" };

struct Particle {
    Vector3 pos, vel;
    float age, life;
    float size0, size1;
    float rot, spin;
    Color c0, c1;
    bool additive;
    float drag, gravity;
};

struct DecalRec {
    Vector3 pos, normal;
    float size, age, life, rot;
    Color color;
    bool used = false;
};

struct EmitterRec {
    int id = 0;
    Preset preset = Preset::Smoke;
    Vector3 pos{}, dir{ 0, 1, 0 };
    float scale = 1.0f;
    float carry = 0.0f;   // fractional particles owed
};

std::vector<Particle> g_particles;
DecalRec g_decals[kMaxDecals];
int g_nextDecal = 0;
int g_decalCount = 0;
std::vector<EmitterRec> g_emitters;
int g_nextEmitterId = 1;
bool g_autoImpacts = true;
uint32_t g_rng = 0x9e3779b9u;
Texture2D g_soft = {};     // round soft-edged puff
bool g_softTried = false;

float Rand01() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (g_rng & 0xffffff) / 16777216.0f;
}
float RandRange(float a, float b) { return a + (b - a) * Rand01(); }

Vector3 AnyPerpendicular(Vector3 n) {
    return Vector3Normalize(std::fabs(n.y) < 0.9f ? Vector3CrossProduct(n, { 0, 1, 0 }) : Vector3CrossProduct(n, { 1, 0, 0 }));
}

// A random direction inside the cone of half-angle `deg` round `axis`.
Vector3 ConeDirection(Vector3 axis, float deg) {
    axis = Vector3Normalize(axis);
    const Vector3 a = AnyPerpendicular(axis);
    const Vector3 b = Vector3CrossProduct(axis, a);
    const float theta = std::sqrt(Rand01()) * deg * DEG2RAD;
    const float phi = Rand01() * 2.0f * PI;
    const float s = std::sin(theta);
    return Vector3Add(Vector3Scale(axis, std::cos(theta)),
                      Vector3Add(Vector3Scale(a, s * std::cos(phi)), Vector3Scale(b, s * std::sin(phi))));
}

Color Lerp(Color a, Color b, float t) {
    return { (unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t),
             (unsigned char)(a.b + (b.b - a.b) * t), (unsigned char)(a.a + (b.a - a.a) * t) };
}

void EnsureTexture() {
    if (g_softTried) return;
    g_softTried = true;
    Image img = GenImageGradientRadial(64, 64, 0.15f, WHITE, BLANK);
    g_soft = LoadTextureFromImage(img);
    UnloadImage(img);
    if (g_soft.id != 0) {
        SetTextureFilter(g_soft, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(g_soft, TEXTURE_WRAP_CLAMP);
    }
}

void Spawn(const Params& p, Vector3 origin, Vector3 dir, float scale) {
    if ((int)g_particles.size() >= kMaxParticles) return;
    Particle q;
    const Vector3 d = ConeDirection(dir, p.spreadDeg);
    q.pos = Vector3Add(origin, Vector3Scale(d, 0.02f));
    q.vel = Vector3Scale(d, RandRange(p.speedMin, p.speedMax) * scale);
    q.age = 0.0f;
    q.life = RandRange(p.lifeMin, p.lifeMax);
    const float jitter = RandRange(0.75f, 1.25f);
    q.size0 = p.sizeStart * scale * jitter;
    q.size1 = p.sizeEnd * scale * jitter;
    q.rot = Rand01() * 360.0f;
    q.spin = RandRange(-p.spin, p.spin);
    q.c0 = p.start; q.c1 = p.end;
    q.additive = p.additive;
    q.drag = p.drag;
    q.gravity = p.gravity;
    g_particles.push_back(q);
}

} // namespace

const char* PresetName(Preset p) {
    const int i = (int)p;
    return (i >= 0 && i < (int)Preset::Count) ? kPresetNames[i] : "";
}

bool PresetFromName(const char* name, Preset& out) {
    if (!name) return false;
    for (int i = 0; i < (int)Preset::Count; i++) {
        const char* a = name; const char* b = kPresetNames[i];
        while (*a && *b && std::tolower((unsigned char)*a) == std::tolower((unsigned char)*b)) { ++a; ++b; }
        if (!*a && !*b) { out = (Preset)i; return true; }
    }
    return false;
}

void Emit(Preset preset, Vector3 position, Vector3 direction, int count, float scale) {
    const int i = (int)preset;
    if (i < 0 || i >= (int)Preset::Count) return;
    const Params& p = kPresets[i];
    if (Vector3Length(direction) < 1e-4f) direction = { 0, 1, 0 };
    if (count <= 0) count = p.burst;
    count = std::min(count, 400);
    if (!(scale > 0.0f)) scale = 1.0f;
    for (int n = 0; n < count; n++) Spawn(p, position, direction, scale);
}

int StartEmitter(Preset preset, Vector3 position, Vector3 direction, float scale) {
    if ((int)g_emitters.size() >= kMaxEmitters || (int)preset < 0 || (int)preset >= (int)Preset::Count) return 0;
    EmitterRec e;
    e.id = g_nextEmitterId++;
    e.preset = preset;
    e.pos = position;
    e.dir = Vector3Length(direction) < 1e-4f ? Vector3{ 0, 1, 0 } : direction;
    e.scale = scale > 0.0f ? scale : 1.0f;
    g_emitters.push_back(e);
    return e.id;
}

void MoveEmitter(int id, Vector3 position, Vector3 direction) {
    for (EmitterRec& e : g_emitters) {
        if (e.id != id) continue;
        e.pos = position;
        if (Vector3Length(direction) > 1e-4f) e.dir = direction;
        return;
    }
}

void StopEmitter(int id) {
    g_emitters.erase(std::remove_if(g_emitters.begin(), g_emitters.end(), [id](const EmitterRec& e) { return e.id == id; }), g_emitters.end());
}

void Decal(Vector3 position, Vector3 normal, float size, Color color, float seconds) {
    if (!(size > 0.0f)) return;
    if (Vector3Length(normal) < 1e-4f) normal = { 0, 1, 0 };
    DecalRec& d = g_decals[g_nextDecal];
    g_nextDecal = (g_nextDecal + 1) % kMaxDecals;
    d.pos = position;
    d.normal = Vector3Normalize(normal);
    d.size = size;
    d.age = 0.0f;
    d.life = seconds > 0.1f ? seconds : 0.1f;
    d.rot = Rand01() * 360.0f;
    d.color = color;
    d.used = true;
    g_decalCount = 0;
    for (const DecalRec& r : g_decals) if (r.used) g_decalCount++;
}

void Update(float dt) {
    if (!(dt > 0.0f)) return;
    dt = std::min(dt, 0.1f);

    for (EmitterRec& e : g_emitters) {
        const Params& p = kPresets[(int)e.preset];
        e.carry += p.rate * e.scale * dt;
        const int n = (int)e.carry;
        e.carry -= (float)n;
        for (int i = 0; i < n; i++) Spawn(p, e.pos, e.dir, e.scale);
    }

    for (size_t i = 0; i < g_particles.size();) {
        Particle& q = g_particles[i];
        q.age += dt;
        if (q.age >= q.life) { q = g_particles.back(); g_particles.pop_back(); continue; }
        q.vel.y += q.gravity * dt;
        q.vel = Vector3Scale(q.vel, std::max(0.0f, 1.0f - q.drag * dt));
        q.pos = Vector3Add(q.pos, Vector3Scale(q.vel, dt));
        q.rot += q.spin * dt;
        ++i;
    }

    bool any = false;
    for (DecalRec& d : g_decals) {
        if (!d.used) continue;
        d.age += dt;
        if (d.age >= d.life) d.used = false;
        else any = true;
    }
    if (!any) g_decalCount = 0;
    else {
        g_decalCount = 0;
        for (const DecalRec& r : g_decals) if (r.used) g_decalCount++;
    }
}

namespace {

// Corner order matches DrawBillboardPro: bottom-left, bottom-right, top-right, top-left (counterclockwise from the camera).
void Quad(Vector3 c, Vector3 right, Vector3 up, float size, float rotDeg, Color col) {
    const float r = rotDeg * DEG2RAD, cs = std::cos(r), sn = std::sin(r);
    const Vector3 rx = Vector3Add(Vector3Scale(right, cs), Vector3Scale(up, sn));
    const Vector3 ry = Vector3Add(Vector3Scale(up, cs), Vector3Scale(right, -sn));
    const float h = size * 0.5f;
    const Vector3 bl = Vector3Add(c, Vector3Add(Vector3Scale(rx, -h), Vector3Scale(ry, -h)));
    const Vector3 br = Vector3Add(c, Vector3Add(Vector3Scale(rx, h), Vector3Scale(ry, -h)));
    const Vector3 tr = Vector3Add(c, Vector3Add(Vector3Scale(rx, h), Vector3Scale(ry, h)));
    const Vector3 tl = Vector3Add(c, Vector3Add(Vector3Scale(rx, -h), Vector3Scale(ry, h)));
    rlColor4ub(col.r, col.g, col.b, col.a);
    rlTexCoord2f(0.0f, 1.0f); rlVertex3f(bl.x, bl.y, bl.z);
    rlTexCoord2f(1.0f, 1.0f); rlVertex3f(br.x, br.y, br.z);
    rlTexCoord2f(1.0f, 0.0f); rlVertex3f(tr.x, tr.y, tr.z);
    rlTexCoord2f(0.0f, 0.0f); rlVertex3f(tl.x, tl.y, tl.z);
}

} // namespace

void Draw(const Camera3D& camera) {
    if (g_particles.empty() && g_decalCount == 0) return;
    EnsureTexture();
    if (g_soft.id == 0) return;

    const Vector3 fwd = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    const Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, camera.up));
    const Vector3 up = Vector3CrossProduct(right, fwd);

    rlDisableDepthMask();

    // Decals first: they lie on surfaces, so they go under the particles.
    if (g_decalCount > 0) {
        rlSetTexture(g_soft.id);
        rlBegin(RL_QUADS);
        for (const DecalRec& d : g_decals) {
            if (!d.used) continue;
            const float fadeStart = d.life * 0.75f;
            const float fade = d.age > fadeStart ? 1.0f - (d.age - fadeStart) / (d.life - fadeStart) : 1.0f;
            Color c = d.color;
            c.a = (unsigned char)(c.a * Clamp(fade, 0.0f, 1.0f));
            const Vector3 a = AnyPerpendicular(d.normal);
            const Vector3 b = Vector3CrossProduct(d.normal, a);
            Quad(Vector3Add(d.pos, Vector3Scale(d.normal, 0.025f)), a, b, d.size, d.rot, c);
        }
        rlEnd();
        rlSetTexture(0);
    }

    static std::vector<int> order;
    for (int pass = 0; pass < 2; pass++) {
        const bool additive = pass == 1;
        order.clear();
        for (int i = 0; i < (int)g_particles.size(); i++) if (g_particles[i].additive == additive) order.push_back(i);
        if (order.empty()) continue;
        if (!additive) {   // alpha blending needs far-to-near
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                return Vector3DistanceSqr(g_particles[a].pos, camera.position) > Vector3DistanceSqr(g_particles[b].pos, camera.position);
            });
        }
        if (additive) BeginBlendMode(BLEND_ADDITIVE);
        rlSetTexture(g_soft.id);
        rlBegin(RL_QUADS);
        for (int i : order) {
            const Particle& q = g_particles[i];
            const float t = q.age / q.life;
            Color c = Lerp(q.c0, q.c1, t);
            Quad(q.pos, right, up, q.size0 + (q.size1 - q.size0) * t, q.rot, c);
        }
        rlEnd();
        rlSetTexture(0);
        if (additive) EndBlendMode();
    }

    rlEnableDepthMask();
}

void Clear() {
    g_particles.clear();
    g_emitters.clear();
    for (DecalRec& d : g_decals) d.used = false;
    g_decalCount = 0;
    g_nextDecal = 0;
}

void OnImpact(Vector3 point, Vector3 normal, float speed) {
    if (!g_autoImpacts || speed < 3.0f) return;
    if (Vector3Length(normal) < 1e-4f) normal = { 0, 1, 0 };
    const float k = std::min(speed / 10.0f, 2.0f);
    Emit(Preset::Dust, point, normal, (int)(4 + 6 * k), 0.6f + 0.5f * k);
    if (speed > 8.0f) Emit(Preset::Debris, point, normal, (int)(3 + 3 * k), 0.5f);
    if (speed > 5.0f && normal.y > 0.5f) Decal(point, normal, 0.5f + 0.5f * k, { 20, 18, 15, 70 }, 25.0f);
}

void SetAutoImpacts(bool on) { g_autoImpacts = on; }
bool AutoImpacts() { return g_autoImpacts; }
int ParticleCount() { return (int)g_particles.size(); }
int DecalCount() { return g_decalCount; }
int EmitterCount() { return (int)g_emitters.size(); }

} // namespace fx
