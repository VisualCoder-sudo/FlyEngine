// Sound rays (include/Engine/Backend/SoundRays.hpp): how much a wall muffles a sound, and what a room does to an echo, measured
// over a fake world of slabs and boxes. No window, no physics.
#include "Engine/Backend/SoundRays.hpp"

#include <cmath>
#include <cstdio>
#include <string>

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

    // Close to the source nothing muffles it, even with a wall right there; the effect fades in with distance.
    {
        const RayFn wall = Wall(0.9f, 1.2f);
        Occlusion close = MeasureOcclusion(wall, fan, Vec3{ 1.4f, 0, 0 }, 0.5f);        // inside the near zone (1.5 m for this size)
        CHECK(close.openness == 1.0f && close.nearness == 1.0f);
        const RayFn farWall = Wall(2.0f, 2.5f);
        Occlusion mid = MeasureOcclusion(farWall, fan, Vec3{ 3.0f, 0, 0 }, 0.5f);       // 3 m: halfway through the fade
        Occlusion far = MeasureOcclusion(farWall, fan, Vec3{ 9.0f, 0, 0 }, 0.5f);       // 9 m: fully on
        CHECK(mid.nearness > 0.3f && mid.nearness < 0.7f && mid.openness > far.openness + 0.2f);
        CHECK(far.nearness == 0.0f && far.openness < 0.3f);
        float prev = 1.0f;                                                              // moving off, the muffling only ever grows
        for (float x = 1.6f; x <= 9.0f; x += 0.4f) {
            const float o = MeasureOcclusion(farWall, fan, Vec3{ x, 0, 0 }, 0.5f).openness;
            CHECK(o <= prev + 1e-3f);
            prev = o;
        }
    }

    // A big object counts as near for longer: its nearness zone scales with its size.
    CHECK(MeasureOcclusion(Wall(3.0f, 3.5f), fan, Vec3{ 5.0f, 0, 0 }, 3.0f).nearness > MeasureOcclusion(Wall(3.0f, 3.5f), fan, Vec3{ 5.0f, 0, 0 }, 0.5f).nearness);

    // What sits right at the source or at the listener is not a wall: a thin slab 0.3 m from the fan (its mount), or one just in front
    // of the listener (their own body), is carried past; a real wall in the middle still counts, with or without them.
    {
        const RayFn mount = Wall(0.45f, 0.5f), body = Wall(9.5f, 9.6f);
        CHECK(MeasureOcclusion(mount, fan, ear, 0.3f).blockedShare == 0.0f);
        CHECK(MeasureOcclusion(body, fan, ear, 0.3f).blockedShare == 0.0f);
        const RayFn both = [&](const Vec3& o, const Vec3& d, float m) -> RayHit {
            RayHit best;
            for (const RayFn* f : { &mount, &body }) { const RayHit h = (*f)(o, d, m); if (h.hit && (!best.hit || h.distance < best.distance)) best = h; }
            return best;
        };
        CHECK(MeasureOcclusion(both, fan, ear, 0.3f).blockedShare == 0.0f);
        const RayFn real = Wall(5.0f, 5.4f);
        const RayFn all = [&](const Vec3& o, const Vec3& d, float m) -> RayHit {
            RayHit best;
            for (const RayFn* f : { &mount, &body, &real }) { const RayHit h = (*f)(o, d, m); if (h.hit && (!best.hit || h.distance < best.distance)) best = h; }
            return best;
        };
        const Occlusion withClutter = MeasureOcclusion(all, fan, ear, 0.3f), clean = MeasureOcclusion(real, fan, ear, 0.3f);
        CHECK(withClutter.blockedShare == 1.0f && std::fabs(withClutter.thickness - clean.thickness) < 0.05f);
        CHECK(std::fabs(withClutter.direct - clean.direct) < 0.01f);
    }

    // A floor under the source never blocks it: the sample points are not placed below it.
    {
        const RayFn floorBelow = [](const Vec3& o, const Vec3& d, float maxDistance) -> RayHit {
            if (std::fabs(d.y) < 1e-6f) return o.y < 0.0f ? RayHit{ true, 0.0f } : RayHit{};   // a ray that starts under the floor is inside it
            const float t = (0.0f - o.y) / d.y;
            return (t >= 0.0f && t <= maxDistance) ? RayHit{ true, t } : RayHit{};
        };
        const Occlusion onFloor = MeasureOcclusion(floorBelow, Vec3{ 0, 0.5f, 0 }, Vec3{ 10, 1.6f, 0 }, 0.5f);
        CHECK(onFloor.blockedShare == 0.0f && onFloor.openness == 1.0f);
    }

    // A doorway: the straight line is blocked, but a way round exists, so it is clearer than a closed wall; a wall with no way round
    // (a closed room) gets no help. The result is cached between refreshes.
    {
        auto wallWithGap = [](float gapLow, float gapHigh) {   // a wall at x 5..5.3, open between gapLow and gapHigh in y
            return RayFn([=](const Vec3& o, const Vec3& d, float maxDistance) -> RayHit {
                if (std::fabs(d.x) < 1e-6f) return {};
                const float face = d.x > 0 ? 5.0f : 5.3f;
                const float t = (face - o.x) / d.x;
                if (t < 0.0f || t > maxDistance) return {};
                const float y = o.y + d.y * t;
                if (y > gapLow && y < gapHigh) return {};
                return { true, t };
            });
        };
        const RayFn withGap = wallWithGap(1.9f, 1e9f), closed = wallWithGap(1e9f, 1e9f);
        float cache = 0.0f;
        const Occlusion door = MeasureOcclusion(withGap, fan, ear, 0.3f, &cache, true);
        const Occlusion shut = MeasureOcclusion(closed, fan, ear, 0.3f, nullptr, true);
        CHECK(door.direct < 0.5f && door.detour > 0.3f && door.openness > door.direct + 0.2f);
        CHECK(shut.detour == 0.0f && shut.openness == shut.direct);
        CHECK(cache == door.detour);
        const Occlusion cached = MeasureOcclusion(withGap, fan, ear, 0.3f, &cache, false);   // no refresh: the cached detour is used
        CHECK(std::fabs(cached.detour - door.detour) < 1e-6f);
        float noDetour = 0.0f;
        MeasureOcclusion(closed, fan, ear, 0.3f, &noDetour, true);
        CHECK(noDetour == 0.0f);
    }

    // The blocker's name is reported for the debug log.
    {
        const RayFn named = [](const Vec3& o, const Vec3& d, float m) -> RayHit { RayHit h = Wall(5.0f, 5.4f)(o, d, m); if (h.hit) h.name = "Brick wall"; return h; };
        const Occlusion o = MeasureOcclusion(named, fan, ear, 0.3f);
        CHECK(o.blocker && std::string(o.blocker) == "Brick wall");
    }

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
