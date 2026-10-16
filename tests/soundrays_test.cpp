// Sound rays (include/Engine/Backend/SoundRays.hpp): how much a wall muffles a sound, and what a room does to an echo, measured
// over a fake world of slabs and boxes. No window, no physics.
#include "Engine/Backend/SoundRays.hpp"

#include <cmath>
#include <cstdio>

using namespace soundrays;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// A wall: the slab xMin <= x <= xMax, from yMin upward, any z. First thing a ray meets.
static RayFn Wall(float xMin, float xMax, float yMin = -1e9f) {
    return [=](const Vec3& o, const Vec3& d, float maxDistance) -> RayHit {
        if (std::fabs(d.x) < 1e-6f) return {};
        const float face = d.x > 0 ? xMin : xMax;
        const float t = (face - o.x) / d.x;
        if (t < 0.0f || t > maxDistance) return {};
        if (o.y + d.y * t < yMin) return {};
        return { true, t };
    };
}

// A closed box of half-size `half` around the origin, seen from inside.
static RayFn Box(float half) {
    return [=](const Vec3& o, const Vec3& d, float maxDistance) -> RayHit {
        float best = 1e9f;
        const float oo[3] = { o.x, o.y, o.z }, dd[3] = { d.x, d.y, d.z };
        for (int a = 0; a < 3; ++a) {
            if (std::fabs(dd[a]) < 1e-6f) continue;
            const float t = ((dd[a] > 0 ? half : -half) - oo[a]) / dd[a];
            if (t >= 0.0f && t < best) best = t;
        }
        if (best > maxDistance) return {};
        return { true, best };
    };
}

static RayFn Nothing() { return [](const Vec3&, const Vec3&, float) -> RayHit { return {}; }; }

int main() {
    const Vec3 fan{ 0, 0, 0 }, ear{ 10, 0, 0 };

    // Clear path: nothing is muffled.
    Occlusion open = MeasureOcclusion(Nothing(), fan, ear, 0.5f);
    CHECK(open.openness == 1.0f && open.blockedShare == 0.0f && open.gain == 1.0f && open.cutoffHz > 21000.0f);

    // A thin wall muffles some; a thick one a lot more; the more it is blocked, the lower the cutoff.
    Occlusion thin = MeasureOcclusion(Wall(5.0f, 5.1f), fan, ear, 0.5f);
    Occlusion thick = MeasureOcclusion(Wall(5.0f, 6.0f), fan, ear, 0.5f);
    CHECK(thin.blockedShare == 1.0f && std::fabs(thin.thickness - 0.1f) < 0.03f);
    CHECK(std::fabs(thick.thickness - 1.0f) < 0.05f);
    CHECK(thin.openness < 0.6f && thin.openness > 0.2f);
    CHECK(thick.openness < 0.05f && thick.cutoffHz < 450.0f && thick.cutoffHz >= 399.0f);
    CHECK(thin.cutoffHz < open.cutoffHz && thick.cutoffHz < thin.cutoffHz);
    CHECK(thick.gain < thin.gain && thin.gain < open.gain && thick.gain >= 0.29f);

    // A wall that only covers the top of the path blocks only some of the rays.
    Occlusion partial = MeasureOcclusion(Wall(5.0f, 5.5f, 0.05f), fan, ear, 0.5f);
    CHECK(partial.blockedShare > 0.0f && partial.blockedShare < 1.0f);
    CHECK(partial.openness > thick.openness && partial.openness < open.openness);

    // The listener right next to the source: nothing can be in between.
    Occlusion near = MeasureOcclusion(Wall(0.7f, 0.8f), fan, Vec3{ 1.0f, 0, 0 }, 0.5f);
    CHECK(near.openness == 1.0f);

    // A wall behind the listener, or behind the source, is not between them.
    CHECK(MeasureOcclusion(Wall(12.0f, 13.0f), fan, ear, 0.5f).blockedShare == 0.0f);
    CHECK(MeasureOcclusion(Wall(-3.0f, -2.0f), fan, ear, 0.5f).blockedShare == 0.0f);

    // Echo directions: unit vectors, all different.
    for (int i = 0; i < kEchoRays; ++i) {
        const Vec3 a = EchoDirection(i);
        CHECK(std::fabs(std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z) - 1.0f) < 1e-4f);
        for (int j = i + 1; j < kEchoRays; ++j) {
            const Vec3 b = EchoDirection(j);
            CHECK(std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z) > 0.05f);
        }
    }

    // A small closed room: every ray hits, a short echo, loud and ringing. Open ground: no echo at all.
    float dist[kEchoRays];
    MeasureRoom(Box(3.0f), fan, 0.5f, dist);
    const Room closed = SummarizeRoom(dist, kEchoRays);
    CHECK(closed.enclosed == 1.0f && closed.meanDistance > 2.5f && closed.meanDistance < 5.5f);
    CHECK(closed.wet > 0.2f && closed.feedback > 0.5f && closed.delaySeconds >= 0.03f);
    MeasureRoom(Nothing(), fan, 0.5f, dist);
    const Room field = SummarizeRoom(dist, kEchoRays);
    CHECK(field.enclosed == 0.0f && field.wet == 0.0f);
    // A big hall: the walls are farther, so the echo is later; walls past the ray range count as open.
    MeasureRoom(Box(25.0f), fan, 0.5f, dist);
    const Room hall = SummarizeRoom(dist, kEchoRays);
    CHECK(hall.enclosed > 0.0f && hall.enclosed <= 1.0f && hall.delaySeconds > closed.delaySeconds * 3.0f && hall.delaySeconds <= 0.6f);
    CHECK(hall.feedback >= 0.15f && hall.feedback <= 0.9f && hall.wet >= 0.0f && hall.wet <= 1.0f);

    // Nothing is queued or playing to begin with, and Clear is safe.
    CHECK(ActiveEmitters() == 0);
    Clear();

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
