// Dynamic water disturbance layer: boat wakes, splashes and lingering foam.
//
// A camera-centred RIPPLE_N x RIPPLE_N grid holds ripple height, vertical
// velocity and a foam mask. Height obeys the 2D wave equation (so a moving
// source produces a real V-shaped wake), foam decays over a few seconds.
// Height + foam are uploaded as an RGBA16F texture that water.glsl samples in
// both the vertex stage (displacement + normals) and fragment stage (foam).
// The CPU keeps the same grid so GetHeightAt() - and therefore buoyancy -
// sees the wake the player sees. Idle water costs nothing: the grid sleeps
// until something disturbs it.

#include "../../../include/Terrain/Water/WaterBody.hpp"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <algorithm>
#include <cmath>
#include <cstring>

Texture2D WaterBody::s_spraySprite = { 0 };

namespace {

constexpr float kStepDt = 1.0f / 60.0f;

// float -> IEEE half (round to nearest, flush denormals). Enough for heights.
unsigned short ToHalf(float f) {
    unsigned int x;
    std::memcpy(&x, &f, 4);
    const unsigned int sign = (x >> 16) & 0x8000u;
    int exp = (int)((x >> 23) & 0xFFu) - 127 + 15;
    unsigned int mant = x & 0x7FFFFFu;
    if (exp <= 0) return (unsigned short)sign;
    if (exp >= 31) return (unsigned short)(sign | 0x7C00u);
    unsigned short h = (unsigned short)(sign | ((unsigned)exp << 10) | (mant >> 13));
    if (mant & 0x1000u) ++h; // round
    return h;
}

} // namespace

void WaterBody::EnsureRippleGrid() {
    if (!rippleH.empty()) return;
    const size_t n = (size_t)RIPPLE_N * RIPPLE_N;
    rippleH.assign(n, 0.0f);
    rippleV.assign(n, 0.0f);
    rippleFoam.assign(n, 0.0f);
    rippleHalf.assign(n * 4, 0);
    const float camX = s_activeCamera ? s_activeCamera->position.x : position.x;
    const float camZ = s_activeCamera ? s_activeCamera->position.z : position.z;
    const float cs = RIPPLE_WINDOW / RIPPLE_N;
    rippleOriginX = std::floor((camX - RIPPLE_WINDOW * 0.5f) / cs) * cs;
    rippleOriginZ = std::floor((camZ - RIPPLE_WINDOW * 0.5f) / cs) * cs;
}

// Scroll the window (by whole cells) so it stays around the camera.
void WaterBody::RecentreRipples(float camX, float camZ) {
    if (rippleH.empty()) return;
    const float cs = RIPPLE_WINDOW / RIPPLE_N;
    const int dx = (int)std::lround((camX - RIPPLE_WINDOW * 0.5f - rippleOriginX) / cs);
    const int dz = (int)std::lround((camZ - RIPPLE_WINDOW * 0.5f - rippleOriginZ) / cs);
    // Hysteresis: only shift once the camera has moved a fair way, so a
    // slowly drifting camera doesn't copy the grid every frame.
    if (std::abs(dx) < 16 && std::abs(dz) < 16) return;

    const int N = RIPPLE_N;
    auto shift = [&](std::vector<float>& a) {
        std::vector<float> out(a.size(), 0.0f);
        for (int z = 0; z < N; ++z) {
            const int sz = z + dz;
            if (sz < 0 || sz >= N) continue;
            for (int x = 0; x < N; ++x) {
                const int sx = x + dx;
                if (sx < 0 || sx >= N) continue;
                out[(size_t)z * N + x] = a[(size_t)sz * N + sx];
            }
        }
        a.swap(out);
    };
    shift(rippleH);
    shift(rippleV);
    shift(rippleFoam);
    rippleOriginX += dx * cs;
    rippleOriginZ += dz * cs;
    ripplePendingUpload = true;
}

void WaterBody::StampGaussian(float wx, float wz, float radius, float dHeight, float dVel, float dFoam) {
    if (rippleH.empty()) return;
    const int N = RIPPLE_N;
    const float cs = RIPPLE_WINDOW / RIPPLE_N;
    const float gx = (wx - rippleOriginX) / cs - 0.5f;
    const float gz = (wz - rippleOriginZ) / cs - 0.5f;
    const float rc = std::max(radius / cs, 1.2f);
    const int ext = (int)std::ceil(rc * 2.0f);
    const int x0 = std::max(1, (int)std::floor(gx) - ext), x1 = std::min(N - 2, (int)std::ceil(gx) + ext);
    const int z0 = std::max(1, (int)std::floor(gz) - ext), z1 = std::min(N - 2, (int)std::ceil(gz) + ext);
    if (x0 > x1 || z0 > z1) return;
    const float inv = 1.0f / (rc * rc);
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            const float ddx = x - gx, ddz = z - gz;
            const float w = std::exp(-(ddx * ddx + ddz * ddz) * inv);
            if (w < 0.01f) continue;
            const size_t i = (size_t)z * N + x;
            rippleH[i] += dHeight * w;
            rippleV[i] += dVel * w;
            rippleFoam[i] = std::min(1.0f, rippleFoam[i] + dFoam * w);
        }
    }
    rippleActive = true;
    rippleIdleTime = 0.0f;
    ripplePendingUpload = true;
}

void WaterBody::AddBodyWake(const void* id, Vector3 worldPos, Vector3 velocity,
                            float radius, float submergedFraction, float dt) {
    if (!ripple.enabled || submergedFraction <= 0.0f) return;
    const float cs = RIPPLE_WINDOW / RIPPLE_N;
    const double now = GetTime();

    const float speedH = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
    const float speed = std::sqrt(speedH * speedH + velocity.y * velocity.y);

    // Path since the last call, so a fast hull leaves a continuous trail.
    Vector2 cur = { worldPos.x, worldPos.z };
    Vector2 prev = cur;
    auto it = wakeTracks.find(id);
    if (it != wakeTracks.end() && now - it->second.time < 0.25 &&
        Vector2Distance(it->second.last, cur) < 40.0f) {
        prev = it->second.last;
    }
    wakeTracks[id] = { cur, now };

    if (speed < 0.15f) return; // resting object: nothing to disturb
    EnsureRippleGrid();

    const float dist = Vector2Distance(prev, cur);
    const int stamps = std::clamp((int)std::ceil(dist / (cs * 0.75f)), 1, 48);
    const float share = 1.0f / stamps;
    const float stepScale = std::clamp(dt * 60.0f, 0.25f, 4.0f);

    const float r = std::clamp(radius, 0.4f, 6.0f);
    const float hullPush = std::min(0.00018f * ripple.wakeStrength * speed * submergedFraction, 0.0025f) * stepScale;
    const float bowLift  = std::min(0.0003f * ripple.wakeStrength * speed * submergedFraction, 0.004f) * stepScale;
    // Bobbing/drifting objects shouldn't churn foam; it ramps in once actually moving.
    const float foamRamp = std::clamp((speed - 0.5f) / 2.0f, 0.0f, 1.0f);
    const float foamAmt  = std::clamp(speed / 6.0f, 0.0f, 1.0f) * foamRamp * submergedFraction * 0.03f * stepScale;

    // Heading for the bow wave; fall back to the path direction.
    Vector2 dir = { velocity.x, velocity.z };
    if (speedH > 0.05f) dir = Vector2Scale(dir, 1.0f / speedH);
    else if (dist > 1e-4f) dir = Vector2Scale(Vector2Subtract(cur, prev), 1.0f / dist);
    else dir = { 1.0f, 0.0f };

    for (int s = 0; s < stamps; ++s) {
        const float t = (s + 1) * share;
        const Vector2 p = Vector2Lerp(prev, cur, t);
        // Hull pushes water down and aside along its length...
        StampGaussian(p.x, p.y, r, 0.0f, -hullPush * share, foamAmt * share);
        // ...and piles it up in front (the bow wave) with a little foam.
        StampGaussian(p.x + dir.x * r * 1.1f, p.y + dir.y * r * 1.1f, r * 0.8f,
                      0.0f, bowLift * share, foamAmt * share * 0.6f);
        // Churned white water right behind the hull.
        StampGaussian(p.x - dir.x * r * 0.9f, p.y - dir.y * r * 0.9f, r * 0.9f,
                      0.0f, 0.0f, foamAmt * share * 0.8f);
    }

    // Bow spray: fast hulls throw droplets forward and out to the sides.
    if (ripple.spray && speedH > 3.5f) {
        const float expected = (speedH - 3.5f) * 0.35f * submergedFraction * ripple.sprayAmount * stepScale * std::min(r, 2.0f);
        int n = (int)expected;
        if (SprayRnd() < expected - (float)n) ++n;
        const Vector2 side = { -dir.y, dir.x };
        for (int i = 0; i < n; ++i) {
            const float sx = (SprayRnd() * 2.0f - 1.0f);
            const float bx = cur.x + dir.x * r * 1.1f + side.x * sx * r * 0.5f;
            const float bz = cur.y + dir.y * r * 1.1f + side.y * sx * r * 0.5f;
            const float by = GetHeightAt(bx, bz, 0.0f);
            const float fwd = speedH * (0.15f + 0.25f * SprayRnd());
            const float out = sx * (1.0f + 2.0f * SprayRnd());
            EmitSpray({ bx, by + 0.05f, bz },
                      { dir.x * fwd + side.x * out, 1.5f + 2.5f * SprayRnd(), dir.y * fwd + side.y * out },
                      (0.07f + 0.09f * SprayRnd()) * std::clamp(r, 0.5f, 1.5f), 0.7f + 0.6f * SprayRnd());
        }
    }
}

float WaterBody::SprayRnd() {
    sprayRng ^= sprayRng << 13; sprayRng ^= sprayRng >> 17; sprayRng ^= sprayRng << 5;
    return (float)(sprayRng & 0xFFFFFFu) / 16777216.0f;
}

void WaterBody::EmitSpray(Vector3 pos, Vector3 vel, float size, float life) {
    SprayParticle sp = { pos, vel, 0.0f, life, size };
    if (spray.size() < SPRAY_MAX) spray.push_back(sp);
    else spray[sprayNext++ % SPRAY_MAX] = sp; // overwrite the oldest-ish
}

// A crown of droplets thrown up and out from the impact ring.
void WaterBody::EmitSplashSpray(Vector3 pos, float impactSpeed, float radius) {
    if (!ripple.spray) return;
    const int n = std::clamp((int)(impactSpeed * (4.0f + radius * 10.0f) * ripple.sprayAmount), 6, 220);
    const float sizeScale = 0.6f + 0.35f * std::min(radius, 3.0f);
    for (int i = 0; i < n; ++i) {
        const float a = SprayRnd() * 6.2831853f;
        const float ring = radius * (0.6f + 0.6f * SprayRnd());
        const float outV = impactSpeed * (0.12f + 0.30f * SprayRnd()) * (0.6f + 0.25f * std::min(radius, 3.0f));
        const float upV  = impactSpeed * (0.18f + 0.35f * SprayRnd());
        EmitSpray({ pos.x + std::cos(a) * ring, pos.y + 0.05f, pos.z + std::sin(a) * ring },
                  { std::cos(a) * outV, std::min(upV, 9.0f), std::sin(a) * outV },
                  (0.08f + 0.14f * SprayRnd()) * sizeScale, 0.8f + 0.9f * SprayRnd());
    }
}

void WaterBody::StepSpray(float dtReal) {
    if (spray.empty()) return;
    const float dt = std::min(dtReal, 0.05f);
    for (size_t i = 0; i < spray.size();) {
        SprayParticle& sp = spray[i];
        sp.age += dt;
        sp.v.y -= 9.81f * dt;
        const float drag = std::max(0.0f, 1.0f - 0.6f * dt); // light air drag
        sp.v.x *= drag; sp.v.z *= drag;
        sp.p = Vector3Add(sp.p, Vector3Scale(sp.v, dt));
        const float surf = GetHeightAt(sp.p.x, sp.p.z, 0.0f);
        bool dead = sp.age >= sp.life;
        if (!dead && sp.v.y < 0.0f && sp.p.y <= surf) {
            // Landed: a faint foam dot and the particle is done.
            if (!rippleH.empty()) StampGaussian(sp.p.x, sp.p.z, 0.3f, 0.0f, 0.0f, 0.06f);
            dead = true;
        }
        if (dead) {
            spray[i] = spray.back();
            spray.pop_back();
            if (sprayNext > 0) --sprayNext;
        } else {
            ++i;
        }
    }
}

void WaterBody::DrawSpray(const Camera3D& camera) {
    if (spray.empty()) return;
    if (s_spraySprite.id == 0) {
        // 32x32 soft white disc: alpha falls off smoothly from the centre.
        const int N = 32;
        std::vector<unsigned char> px((size_t)N * N * 4);
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                const float dx = (x + 0.5f) / N * 2.0f - 1.0f, dy = (y + 0.5f) / N * 2.0f - 1.0f;
                float t = 1.0f - std::sqrt(dx * dx + dy * dy);
                t = std::clamp(t, 0.0f, 1.0f);
                t = t * t * (3.0f - 2.0f * t);
                unsigned char* o = &px[((size_t)y * N + x) * 4];
                o[0] = o[1] = o[2] = 255;
                o[3] = (unsigned char)(t * 255.0f);
            }
        }
        Image img = {};
        img.data = px.data(); img.width = N; img.height = N; img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        s_spraySprite = LoadTextureFromImage(img);
        if (s_spraySprite.id != 0) {
            SetTextureFilter(s_spraySprite, TEXTURE_FILTER_BILINEAR);
            SetTextureWrap(s_spraySprite, TEXTURE_WRAP_CLAMP);
        }
        if (s_spraySprite.id == 0) return;
    }
    for (const SprayParticle& sp : spray) {
        const float t = sp.age / sp.life;
        const float a = (t < 0.15f ? t / 0.15f : 1.0f - (t - 0.15f) / 0.85f) * 0.85f;
        const float sz = sp.size * (1.0f - 0.35f * t);
        DrawBillboard(camera, s_spraySprite, sp.p, sz * 2.0f,
                      Color{ 245, 250, 255, (unsigned char)(std::clamp(a, 0.0f, 1.0f) * 255.0f) });
    }
}

void WaterBody::AddSplash(Vector3 worldPos, float impactSpeed, float radius) {
    if (!ripple.enabled || impactSpeed < 0.8f) return;
    EnsureRippleGrid();
    const float r = std::clamp(radius, 0.4f, 6.0f);
    const float depth = std::min(impactSpeed * ripple.splashStrength, 1.0f) * std::min(1.0f, 0.5f + r * 0.5f);
    // Crater: the wave equation turns the hole into a rebound + outgoing rings.
    StampGaussian(worldPos.x, worldPos.z, r * 1.2f, -depth, 0.0f, 0.0f);
    // Foam burst, wider than the crater.
    StampGaussian(worldPos.x, worldPos.z, r * 1.8f, 0.0f, 0.0f, std::clamp(impactSpeed / 5.0f, 0.3f, 0.7f));
    EmitSplashSpray(worldPos, impactSpeed, r);
}

float WaterBody::GetRippleHeightAt(float x, float z) const {
    if (!ripple.enabled || rippleH.empty() || !rippleActive) return 0.0f;
    const int N = RIPPLE_N;
    const float cs = RIPPLE_WINDOW / RIPPLE_N;
    const float gx = (x - rippleOriginX) / cs - 0.5f;
    const float gz = (z - rippleOriginZ) / cs - 0.5f;
    if (gx < 0.0f || gz < 0.0f || gx >= N - 1 || gz >= N - 1) return 0.0f;
    const int ix = (int)gx, iz = (int)gz;
    const float fx = gx - ix, fz = gz - iz;
    const size_t i = (size_t)iz * N + ix;
    const float a = rippleH[i], b = rippleH[i + 1];
    const float c = rippleH[i + N], d = rippleH[i + N + 1];
    return (a + (b - a) * fx) * (1.0f - fz) + (c + (d - c) * fx) * fz;
}

void WaterBody::StepRipples(float dtReal) {
    if (rippleH.empty()) return;
    if (!rippleActive) return;

    // Prune stale wake tracks.
    const double now = GetTime();
    for (auto it = wakeTracks.begin(); it != wakeTracks.end();) {
        if (now - it->second.time > 1.0) it = wakeTracks.erase(it);
        else ++it;
    }

    rippleAccum += std::min(dtReal, 0.1f);
    int steps = 0;
    const int N = RIPPLE_N;
    const float cs = RIPPLE_WINDOW / RIPPLE_N;
    // Courant number: c * dt / cs, clamped for stability (2D explicit scheme).
    const float cfl = ripple.waveSpeed * kStepDt / cs;
    const float c2 = std::min(cfl * cfl, 0.45f);
    const float damp = std::clamp(ripple.damping, 0.9f, 0.9999f);
    const float foamDecay = std::exp(-kStepDt / std::max(ripple.foamLifetime, 0.2f));
    const float maxD = std::max(ripple.maxDisplacement, 0.05f);

    float peakH = 0.0f, peakV = 0.0f, peakF = 0.0f;
    static int blurTick = 0;

    while (rippleAccum >= kStepDt && steps < 4) {
        rippleAccum -= kStepDt;
        ++steps;

        for (int z = 1; z < N - 1; ++z) {
            const int zEdge = std::min(z, N - 1 - z);
            for (int x = 1; x < N - 1; ++x) {
                const size_t i = (size_t)z * N + x;
                const float lap = rippleH[i - 1] + rippleH[i + 1] + rippleH[i - N] + rippleH[i + N] - 4.0f * rippleH[i];
                float v = (rippleV[i] + c2 * lap) * damp;
                // Absorb at the window edge so waves don't reflect back in.
                const int edge = std::min(zEdge, std::min(x, N - 1 - x));
                if (edge < 12) v *= 0.80f + 0.2f * (edge / 12.0f);
                rippleV[i] = v;
            }
        }
        peakH = peakV = peakF = 0.0f;
        for (int z = 1; z < N - 1; ++z) {
            for (int x = 1; x < N - 1; ++x) {
                const size_t i = (size_t)z * N + x;
                float h = rippleH[i] + rippleV[i];
                h = std::clamp(h, -maxD, maxD);
                rippleH[i] = h;
                rippleFoam[i] *= foamDecay;
                peakH = std::max(peakH, std::fabs(h));
                peakV = std::max(peakV, std::fabs(rippleV[i]));
                peakF = std::max(peakF, rippleFoam[i]);
            }
        }

        // Cheap foam diffusion every few steps so trails soften and widen.
        if ((++blurTick & 3) == 0) {
            std::vector<float> tmp(rippleFoam);
            for (int z = 1; z < N - 1; ++z) {
                for (int x = 1; x < N - 1; ++x) {
                    const size_t i = (size_t)z * N + x;
                    rippleFoam[i] = tmp[i] * 0.84f + 0.04f * (tmp[i - 1] + tmp[i + 1] + tmp[i - N] + tmp[i + N]);
                }
            }
        }
    }
    if (steps == 0) return;
    ripplePendingUpload = true;

    // Sleep once everything has settled.
    if (peakH < 0.0015f && peakV < 0.0008f && peakF < 0.01f) {
        rippleIdleTime += steps * kStepDt;
        if (rippleIdleTime > 0.5f) {
            std::fill(rippleH.begin(), rippleH.end(), 0.0f);
            std::fill(rippleV.begin(), rippleV.end(), 0.0f);
            std::fill(rippleFoam.begin(), rippleFoam.end(), 0.0f);
            rippleActive = false; // final all-zero upload still happens
        }
    } else {
        rippleIdleTime = 0.0f;
    }
}

void WaterBody::UploadRippleTexture() {
    if (rippleTex.id == 0) {
        // 256x256 RGBA16F, zero-filled: r = height, g = foam.
        const size_t n = (size_t)RIPPLE_N * RIPPLE_N;
        std::vector<unsigned short> zero(n * 4, 0);
        Image img = {};
        img.data = zero.data();
        img.width = RIPPLE_N;
        img.height = RIPPLE_N;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R16G16B16A16;
        rippleTex = LoadTextureFromImage(img);
        if (rippleTex.id != 0) {
            SetTextureFilter(rippleTex, TEXTURE_FILTER_BILINEAR);
            SetTextureWrap(rippleTex, TEXTURE_WRAP_CLAMP);
        }
        return;
    }
    if (!ripplePendingUpload || rippleH.empty()) return;

    const size_t n = (size_t)RIPPLE_N * RIPPLE_N;
    if (rippleHalf.size() != n * 4) rippleHalf.assign(n * 4, 0);
    const unsigned short one = ToHalf(1.0f);
    for (size_t i = 0; i < n; ++i) {
        rippleHalf[i * 4 + 0] = ToHalf(rippleH[i]);
        rippleHalf[i * 4 + 1] = ToHalf(rippleFoam[i]);
        rippleHalf[i * 4 + 2] = 0;
        rippleHalf[i * 4 + 3] = one;
    }
    UpdateTexture(rippleTex, rippleHalf.data());
    ripplePendingUpload = false;
}

void WaterBody::BindRippleTexture() {
    if (rippleTex.id == 0) return;
    // The shader samples the ripple map in both stages under separate names;
    // point both at the same texture unit.
    const int slot = RIPPLE_SLOT;
    if (rippleTexLoc >= 0) SetShaderValue(shader, rippleTexLoc, &slot, SHADER_UNIFORM_INT);
    if (rippleTexVSLoc >= 0) SetShaderValue(shader, rippleTexVSLoc, &slot, SHADER_UNIFORM_INT);
    rlActiveTextureSlot(slot);
    rlEnableTexture(rippleTex.id);

    const bool on = ripple.enabled && !rippleH.empty();
    Vector4 params = { rippleOriginX, rippleOriginZ, 1.0f / RIPPLE_WINDOW, on ? 1.0f : 0.0f };
    if (rippleParamsLoc >= 0) SetShaderValue(shader, rippleParamsLoc, &params, SHADER_UNIFORM_VEC4);
}
