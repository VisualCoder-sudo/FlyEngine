// Navigation mesh: bake from a fake world of boxes, then find paths. Pure arithmetic, no window. Registered with ctest.
#include "Engine/Navigation.hpp"
#include "Engine/Scripts/FlyScriptApi.hpp"
#include "fly.hpp"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

// fly.hpp's Nav::Agent runs here against the engine's own FlyNative_Nav* functions, which search nav::World().
namespace fly::detail {
const FlyApi*& Api() { static const FlyApi* a = nullptr; return a; }
float& Dt() { static float d = 0; return d; }
std::vector<ClassEntry>& Registry() { static std::vector<ClassEntry> r; return r; }
}

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

namespace {

struct Box { Vector3 lo, hi; };
// A ramp rising along +x from y = 0 at x0 to y = rise at x1 (solid below the surface), spanning z0..z1.
struct Ramp { float x0, x1, z0, z1, rise; };

struct World {
    std::vector<Box> boxes;
    std::vector<Ramp> ramps;

    bool Cast(Vector3 o, Vector3 d, float maxDist, Vector3& point, Vector3& normal) const {
        float best = maxDist;
        bool hit = false;
        for (const Box& b : boxes) {
            float tmin = 0.0f, tmax = best;
            Vector3 n{};
            bool ok = true;
            const float oo[3] = { o.x, o.y, o.z }, dd[3] = { d.x, d.y, d.z };
            const float lo[3] = { b.lo.x, b.lo.y, b.lo.z }, hi[3] = { b.hi.x, b.hi.y, b.hi.z };
            for (int a = 0; a < 3 && ok; ++a) {
                if (std::fabs(dd[a]) < 1e-9f) { if (oo[a] < lo[a] || oo[a] > hi[a]) ok = false; continue; }
                float t1 = (lo[a] - oo[a]) / dd[a], t2 = (hi[a] - oo[a]) / dd[a];
                float sign = -1.0f;
                if (t1 > t2) { std::swap(t1, t2); sign = 1.0f; }
                if (t1 > tmin) { tmin = t1; n = {}; (&n.x)[a] = sign; }
                tmax = std::fmin(tmax, t2);
                if (tmin > tmax) ok = false;
            }
            // Starting inside a box is not a hit; a hit needs a real entry face.
            if (ok && tmin > 1e-6f && tmin < best && (n.x != 0 || n.y != 0 || n.z != 0)) {
                best = tmin; hit = true; normal = n;
                point = { o.x + d.x * tmin, o.y + d.y * tmin, o.z + d.z * tmin };
            }
        }
        for (const Ramp& r : ramps) {   // only straight-down rays matter for a ramp
            if (d.y > -0.999f || o.x < r.x0 || o.x > r.x1 || o.z < r.z0 || o.z > r.z1) continue;
            const float slope = r.rise / (r.x1 - r.x0);
            const float y = (o.x - r.x0) * slope;
            const float t = o.y - y;
            if (t > 1e-6f && t < best) {
                best = t; hit = true;
                const float len = std::sqrt(1.0f + slope * slope);
                normal = { -slope / len, 1.0f / len, 0.0f };
                point = { o.x, y, o.z };
            }
        }
        return hit;
    }
};

nav::RayFn Fn(const World& w) {
    return [&w](Vector3 o, Vector3 d, float m, Vector3& p, Vector3& n) { return w.Cast(o, d, m, p, n); };
}

// A wall along z from the room's south edge up to z = 0.2, then west along x to -4: an L whose outer corner is at
// (0.2, 0.2). The fake world's rays do not hit from inside a box, so the bake finds floor inside the walls and the
// radius takes those columns away: the walkable ground near the corner is x >= 0.5 or z >= 0.5.
World LRoom() {
    World w;
    w.boxes.push_back({ { -10, -1, -10 }, { 10, 0, 10 } });
    w.boxes.push_back({ { -0.2f, 0, -10 }, { 0.2f, 3, 0.2f } });
    w.boxes.push_back({ { -4, 0, -0.2f }, { 0.2f, 3, 0.2f } });
    return w;
}

fly::Vec3 V(Vector3 v) { return { v.x, v.y, v.z }; }
Vector3 R(fly::Vec3 v) { return { v.x, v.y, v.z }; }

fly::Vec3 g_bodyPos;   // the one object the fake table knows
int g_findCalls = 0;   // path searches asked for through the table

FlyApi NavApi(bool withMove) {
    FlyApi a{};
    a.abi_version = FLY_SCRIPT_ABI_VERSION;
    a.size = withMove ? sizeof(FlyApi) : offsetof(FlyApi, NavDebug) + sizeof(void*);   // an older engine stops before it
    a.NavReady = FlyNative_NavReady;
    a.NavFindPath = [](const float* from, const float* to, float* out, int max, short partial) -> int {
        ++g_findCalls;
        return FlyNative_NavFindPath(from, to, out, max, partial);
    };
    a.NavNearest = FlyNative_NavNearest;
    a.NavIsWalkable = FlyNative_NavIsWalkable;
    a.NavLineClear = FlyNative_NavLineClear;
    a.NavMoveAlong = FlyNative_NavMoveAlong;
    a.GetPosition = [](FlyHandle, float* x, float* y, float* z) -> short { *x = g_bodyPos.x; *y = g_bodyPos.y; *z = g_bodyPos.z; return 1; };
    a.SetPosition = [](FlyHandle, float x, float y, float z) -> short { g_bodyPos = { x, y, z }; return 1; };
    return a;
}

World Room(float half = 10.0f) {
    World w;
    w.boxes.push_back({ { -half, -1, -half }, { half, 0, half } });   // floor, top at y = 0
    return w;
}

nav::Params Area(float half = 10.0f) {
    nav::Params p;
    p.min = { -half, -2, -half };
    p.max = { half, 8, half };
    return p;
}

float PathLength(const std::vector<Vector3>& path) {
    float len = 0;
    for (size_t i = 0; i + 1 < path.size(); ++i) len += std::sqrt((path[i + 1].x - path[i].x) * (path[i + 1].x - path[i].x) + (path[i + 1].z - path[i].z) * (path[i + 1].z - path[i].z));
    return len;
}

// True if the straight segments between the points never enter the box (flattened to the floor plan).
bool AvoidsBox(const std::vector<Vector3>& path, const Box& b) {
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        for (int k = 0; k <= 200; ++k) {
            const float t = k / 200.0f;
            const float x = path[i].x + (path[i + 1].x - path[i].x) * t, z = path[i].z + (path[i + 1].z - path[i].z) * t;
            if (x > b.lo.x && x < b.hi.x && z > b.lo.z && z < b.hi.z) return false;
        }
    }
    return true;
}

} // namespace

int main() {
    std::vector<Vector3> path;

    // Bad input is refused and leaves the mesh empty.
    {
        nav::Mesh m;
        World w = Room();
        nav::Params bad = Area();
        bad.cellSize = 0.0f;
        CHECK(!m.Build(bad, Fn(w)) && !m.Built());
        bad = Area(); bad.max = bad.min;
        CHECK(!m.Build(bad, Fn(w)));
        bad = Area(); bad.maxSlope = 0.0f;
        CHECK(!m.Build(bad, Fn(w)));
        bad = Area(5000.0f);
        CHECK(!m.Build(bad, Fn(w)));
        World empty;
        CHECK(!m.Build(Area(), Fn(empty)) && m.FindPath({ 0, 0, 0 }, { 1, 0, 1 }, path) == nav::PathResult::None);
    }

    // An empty room: one straight leg.
    {
        World w = Room();
        nav::Mesh m;
        CHECK(m.Build(Area(), Fn(w)) && m.Built() && m.CellCount() > 100);
        CHECK(m.FindPath({ -8, 0, -8 }, { 8, 0, 8 }, path) == nav::PathResult::Complete);
        CHECK(path.size() == 2);
        CHECK(std::fabs(path.front().x + 8) < 1e-3f && std::fabs(path.back().z - 8) < 1e-3f);
        CHECK(std::fabs(PathLength(path) - 16.0f * 1.41421356f) < 0.1f);
        const size_t legs = path.size();
        CHECK(m.FindPath({ 1, 0, 1 }, { 1.1f, 0, 1.1f }, path) == nav::PathResult::Complete && !path.empty());
        CHECK(m.IsWalkable({ 0, 0, 0 }) && !m.IsWalkable({ 0, 3, 0 }) && !m.IsWalkable({ 50, 0, 0 }));
        Vector3 on{};
        CHECK(m.Nearest({ 2, 1.0f, 2 }, on) && std::fabs(on.y) < 1e-3f);   // standing in the air above the floor
        CHECK(!m.Nearest({ 2, 20.0f, 2 }, on));
        // The ring next to the room's edge is removed for the agent's radius.
        CHECK(!m.IsWalkable({ 9.9f, 0, 0 }) && m.IsWalkable({ 8.5f, 0, 0 }));
        // Same answer twice (the search scratch is reused).
        std::vector<Vector3> again;
        CHECK(m.FindPath({ -8, 0, -8 }, { 8, 0, 8 }, again) == nav::PathResult::Complete && again.size() == legs);
        Vector3 r{};
        for (int i = 0; i < 20; ++i) { CHECK(m.RandomPoint({ 0, 0, 0 }, 5.0f, r) && r.x * r.x + r.z * r.z <= 25.1f && m.IsWalkable(r)); }
        CHECK(!m.RandomPoint({ 0, 0, 0 }, 0.0f, r));
    }

    // A wall with a gap: the path goes through the gap, not through the wall.
    {
        World w = Room();
        const Box wall = { { -0.2f, 0, -10 }, { 0.2f, 3, 3 } };
        w.boxes.push_back(wall);
        nav::Mesh m;
        CHECK(m.Build(Area(), Fn(w)));
        CHECK(m.FindPath({ -5, 0, -5 }, { 5, 0, -5 }, path) == nav::PathResult::Complete);
        CHECK(path.size() >= 3 && AvoidsBox(path, wall));
        CHECK(PathLength(path) > 10.0f + 6.0f);
        // Every point on the path stays on the mesh.
        for (size_t i = 0; i + 1 < path.size(); ++i)
            for (int k = 0; k <= 50; ++k) {
                const float t = k / 50.0f;
                CHECK(m.IsWalkable({ path[i].x + (path[i + 1].x - path[i].x) * t, 0, path[i].z + (path[i + 1].z - path[i].z) * t }));
            }
    }

    // A wall thinner than a cell with no gap still blocks; a partial path stops short of it.
    {
        World w = Room();
        w.boxes.push_back({ { -0.03f, 0, -10 }, { 0.03f, 3, 10 } });
        nav::Mesh m;
        CHECK(m.Build(Area(), Fn(w)));
        CHECK(m.FindPath({ -5, 0, 0 }, { 5, 0, 0 }, path) == nav::PathResult::None && path.empty());
        CHECK(m.FindPath({ -5, 0, 0 }, { 5, 0, 0 }, path, true) == nav::PathResult::Partial);
        CHECK(path.size() >= 2 && path.back().x < 0.0f && path.back().x > -2.0f);
    }

    // Low obstacles (a 0.3 m step) are walked over; a 1 m block is walked around.
    {
        World w = Room();
        w.boxes.push_back({ { -1, 0, -10 }, { 1, 0.3f, 10 } });
        nav::Mesh m;
        CHECK(m.Build(Area(), Fn(w)));
        CHECK(m.FindPath({ -5, 0, 0 }, { 5, 0, 0 }, path) == nav::PathResult::Complete && path.size() == 2);
        CHECK(std::fabs(PathLength(path) - 10.0f) < 0.1f);

        World w2 = Room();
        const Box block = { { -1, 0, -2 }, { 1, 1.0f, 2 } };
        w2.boxes.push_back(block);
        CHECK(m.Build(Area(), Fn(w2)));
        CHECK(m.FindPath({ -5, 0, 0 }, { 5, 0, 0 }, path) == nav::PathResult::Complete);
        CHECK(path.size() >= 3 && AvoidsBox(path, block) && PathLength(path) > 10.5f);
    }

    // A platform 3 m up is reached by a ramp, and only by it.
    {
        World w = Room();
        const Box platform = { { 2, 2.9f, -4 }, { 8, 3.0f, 4 } };
        w.boxes.push_back(platform);
        nav::Mesh noRamp;
        CHECK(noRamp.Build(Area(), Fn(w)));
        CHECK(noRamp.FindPath({ -5, 0, 0 }, { 5, 3, 0 }, path) == nav::PathResult::None);
        w.ramps.push_back({ -8, 2, -1, 1, 3.0f });   // rises 3 m over 10 m, ending at the platform's edge
        nav::Mesh m;
        CHECK(m.Build(Area(), Fn(w)));
        CHECK(m.FindPath({ -9, 0, 0 }, { 5, 3, 0 }, path) == nav::PathResult::Complete);
        CHECK(path.size() >= 2 && std::fabs(path.back().y - 3.0f) < 0.05f && std::fabs(path.front().y) < 0.05f);
    }

    // A low ceiling takes away the floor under it; a high one does not.
    {
        World w = Room();
        w.boxes.push_back({ { 2, 1.2f, -10 }, { 6, 1.5f, 10 } });   // 1.2 m up: too low for a 1.8 m agent
        w.boxes.push_back({ { -6, 2.5f, -10 }, { -2, 2.8f, 10 } }); // 2.5 m up: fine
        nav::Mesh m;
        CHECK(m.Build(Area(), Fn(w)));
        CHECK(!m.IsWalkable({ 4, 0, 0 }) && m.IsWalkable({ -4, 0, 0 }));
        CHECK(m.IsWalkable({ 4, 1.5f, 0 }));   // the slab's top is a surface of its own, but nothing leads up to it
        CHECK(m.FindPath({ -4, 0, 0 }, { 4, 1.5f, 0 }, path) == nav::PathResult::None);
    }

    // A bigger agent keeps further from the walls.
    {
        World w = Room();
        nav::Params small = Area(), big = Area();
        small.agentRadius = 0.2f; big.agentRadius = 1.2f;
        nav::Mesh a, b;
        CHECK(a.Build(small, Fn(w)) && b.Build(big, Fn(w)));
        CHECK(a.CellCount() > b.CellCount());
        CHECK(a.IsWalkable({ 9.2f, 0, 0 }) && !b.IsWalkable({ 9.2f, 0, 0 }) && b.IsWalkable({ 7.5f, 0, 0 }));
    }

    // Clear empties the mesh.
    {
        World w = Room();
        nav::Mesh m;
        CHECK(m.Build(Area(), Fn(w)));
        m.Clear();
        CHECK(!m.Built() && m.CellCount() == 0 && m.FindPath({ 0, 0, 0 }, { 1, 0, 1 }, path) == nav::PathResult::None);
    }

    // Line checks from anywhere inside a cell: next to the L's outer corner the line between two cell centres passes the
    // corner, but the same line started from the south-west of the first cell cuts across it.
    {
        World w = LRoom();
        nav::Mesh m;
        CHECK(m.Build(Area(), Fn(w)));
        CHECK(!m.IsWalkable({ 0.25f, 0, 0.25f }) && m.IsWalkable({ 0.75f, 0, 0.25f }) && m.IsWalkable({ 0.25f, 0, 0.75f }));
        CHECK(m.LineClear({ 0.75f, 0, 0.25f }, { 0.25f, 0, 1.25f }));
        CHECK(!m.LineClear({ 0.55f, 0, 0.05f }, { 0.25f, 0, 1.25f }));
        CHECK(!m.LineClear({ 0.75f, 0, 0.25f }, { 0.25f, 0, 0.75f }));   // through the corner point itself: not allowed
        CHECK(m.LineClear({ 0.55f, 0, 0.05f }, { 0.55f, 0, 6.0f }));
        CHECK(m.LineClear({ 1.4f, 0, -3.0f }, { 1.4f, 0, 5.0f }) && !m.LineClear({ 1.4f, 0, -3.0f }, { -2.0f, 0, -3.0f }));
        CHECK(!m.LineClear({ 1.4f, 0, 1.4f }, { 1.4f, 3.0f, 3.0f }));   // a point far above the floor is not on this line
        CHECK(!m.LineClear({ 0, 0, -3.0f }, { 3.0f, 0, -3.0f }));       // starting inside the wall
        // Ending exactly on a column edge counts the end's own column, in either direction.
        CHECK(m.LineClear({ 2.0f, 0, -3.0f }, { 0.5f, 0, -3.0f }) && m.LineClear({ 0.6f, 0, -3.0f }, { 2.0f, 0, -3.0f }));
        CHECK(m.LineClear({ 2.0f, 0, -2.9f }, { 2.0f, 0, -1.0f }) && m.LineClear({ 2.0f, 0, -1.0f }, { 2.0f, 0, -3.0f }));
        CHECK(!m.LineClear({ 2.0f, 0, -3.0f }, { 0.4999f, 0, -3.0f }));

        // A path's first leg is checked from where the caller stands, so it never cuts the corner either.
        CHECK(m.FindPath({ 0.55f, 0, 0.05f }, { -2.0f, 0, 2.0f }, path) == nav::PathResult::Complete && path.size() >= 2);
        CHECK(m.LineClear(path[0], path[1]));
        for (size_t i = 0; i + 1 < path.size(); ++i) CHECK(m.LineClear(path[i], path[i + 1]));
    }

    // MoveAlong stays on walkable cells: it stops at a wall, slides along it, and never crosses it.
    {
        World w = LRoom();
        nav::Mesh m;
        CHECK(m.Build(Area(), Fn(w)));
        Vector3 out{};
        CHECK(m.MoveAlong({ 1.25f, 0, -2.9f }, { -2.0f, 0, 0 }, out));
        CHECK(out.x >= 0.5f && out.x < 0.51f && std::fabs(out.z + 2.9f) < 1e-4f && m.IsWalkable(out));
        CHECK(m.MoveAlong({ 1.25f, 0, -2.9f }, { -2.0f, 0, 1.0f }, out));   // into the wall at a slant: slides north
        CHECK(out.x >= 0.5f && out.x < 0.51f && std::fabs(out.z + 1.9f) < 1e-3f && m.IsWalkable(out));
        CHECK(m.MoveAlong({ 2.0f, 0, -3.0f }, { 1.0f, 0, 0.5f }, out) && std::fabs(out.x - 3.0f) < 1e-3f && std::fabs(out.z + 2.5f) < 1e-3f);
        CHECK(m.MoveAlong({ 1.25f, 0.7f, -3.0f }, { 0, 0, 0 }, out) && std::fabs(out.y) < 1e-4f);   // y is the ground
        CHECK(m.MoveAlong({ 0.3f, 0, -3.0f }, { 0, 0, 0 }, out) && m.IsWalkable(out) && out.x > 0.0f);   // pulled back on, own side
        CHECK(!m.MoveAlong({ 0, 30.0f, 0 }, { 1, 0, 0 }, out) && std::fabs(out.x - 1.0f) < 1e-6f);
        // A long random walk with big and small steps never leaves the mesh and never gets through the wall.
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> step(-1.5f, 1.5f);
        Vector3 p = { 3.0f, 0, -5.0f };
        int off = 0, crossed = 0;
        for (int i = 0; i < 4000; ++i) {
            const float scale = (i % 7 == 0) ? 3.0f : 0.3f;
            const Vector3 before = p;
            CHECK(m.MoveAlong(p, { step(rng) * scale, 0, step(rng) * scale }, p));
            off += !m.IsWalkable(p);
            // East of the long wall and south of z = 0.2 it can only get west by going round the top of the L.
            if (before.z < -1.0f && p.z < -1.0f && (before.x > 0.0f) != (p.x > 0.0f)) ++crossed;
        }
        CHECK(off == 0 && crossed == 0);

        CHECK(m.MoveAlong({ 3.0f, 0, -5.0f }, { 1.0e30f, 0, 0 }, out) && out.x > 9.0f && m.IsWalkable(out));   // huge, still finite

        // From the middle of a 3 m block (well over two cells from the nearest walkable cell) nothing is pulled.
        World blocky = Room();
        blocky.boxes.push_back({ { -1.5f, 0, -1.5f }, { 1.5f, 2, 1.5f } });
        nav::Mesh mb;
        CHECK(mb.Build(Area(), Fn(blocky)));
        CHECK(!mb.MoveAlong({ 0.1f, 0, 0.1f }, { 0.1f, 0, 0 }, out));

        // On a ramp the height follows the slope smoothly.
        World r = Room();
        r.ramps.push_back({ -8, 2, -1, 1, 3.0f });
        nav::Mesh mr;
        CHECK(mr.Build(Area(), Fn(r)));
        CHECK(mr.MoveAlong({ -5.0f, 0.9f, 0 }, { 2.1f, 0, 0 }, out) && std::fabs(out.y - (out.x + 8.0f) * 0.3f) < 0.1f);

        // Frame by frame, small steps up and down the ramp and a 0.3 m step get all the way, without stalling at a cell
        // edge or jumping ahead.
        World stair = Room();
        stair.boxes.push_back({ { 0, 0, -10 }, { 10, 0.3f, 10 } });
        nav::Mesh ms;
        CHECK(ms.Build(Area(), Fn(stair)));
        for (const float step : { 0.02f, 0.06f, 0.1f, 0.2f }) {
            int jumps = 0;
            auto walk = [&](const nav::Mesh& mesh, Vector3 p, float dx, int frames) {
                mesh.MoveAlong(p, { 0, 0, 0 }, p);
                for (int i = 0; i < frames; ++i) {
                    const Vector3 before = p;
                    mesh.MoveAlong(p, { dx, 0, 0 }, p);
                    jumps += std::fabs(p.x - before.x) > std::fabs(dx) + 1e-4f;
                }
                return p;
            };
            Vector3 up = walk(mr, { -6.0f, 0.6f, 0 }, step, 300);
            CHECK(std::fabs(up.x - std::fmin(-6.0f + 300 * step, 1.5f)) < 0.3f);
            if (step >= 0.06f) CHECK(up.y > 2.5f);
            Vector3 down = walk(mr, { 1.0f, 2.7f, 0 }, -step, 300);
            CHECK(std::fabs(down.x - std::fmax(1.0f - 300 * step, -9.5f)) < 0.3f);
            Vector3 stepUp = walk(ms, { -3.0f, 0, 0 }, step, 300);
            CHECK(std::fabs(stepUp.x - std::fmin(-3.0f + 300 * step, 9.5f)) < 0.3f);
            if (step >= 0.06f) CHECK(std::fabs(stepUp.y - 0.3f) < 1e-3f);
            Vector3 stepDown = walk(ms, { 8.0f, 0.3f, 0 }, -step, 300);
            CHECK(std::fabs(stepDown.x - std::fmax(8.0f - 300 * step, -9.5f)) < 0.3f);
            CHECK(jumps == 0);
        }
    }

    // The script Agent walks round the L from an off-centre start, through the engine's API table, without leaving
    // the mesh or getting stuck, also when something keeps bumping it sideways.
    {
        World w = LRoom();
        nav::Active& world = nav::World();
        CHECK(world.mesh.Build(Area(), Fn(w)));
        const FlyApi api = NavApi(true);
        fly::detail::Api() = &api;
        CHECK(fly::Nav::CanMove());

        for (int bump = 0; bump < 2; ++bump) {
            fly::Nav::Agent agent(4.0f, 0.4f, 0.45f);
            fly::Vec3 feet{ 3.1f, 0, -2.9f };
            const fly::Vec3 goal{ -2.0f, 0, -3.0f };   // in the pocket west of the long wall, south of the short one
            agent.SetTarget(goal);
            std::mt19937 rng(11);
            std::uniform_real_distribution<float> jolt(-0.06f, 0.06f);
            int off = 0, frames = 0, stuck = 0;
            for (; frames < 60 * 30 && !agent.Arrived(); ++frames) {
                const fly::Vec3 before = feet;
                const fly::Vec3 v = agent.Move(feet, 1.0f / 60.0f);
                // What Move returns is how far it really went.
                if (std::fabs(v.x / 60.0f - (feet.x - before.x)) > 1e-4f || std::fabs(v.z / 60.0f - (feet.z - before.z)) > 1e-4f) ++off;
                if (bump) fly::Nav::MoveAlong(feet, { jolt(rng), 0, jolt(rng) }, feet);
                off += !world.mesh.IsWalkable(R(feet));
                stuck += agent.Stuck();
            }
            CHECK(agent.Arrived() && off == 0 && stuck == 0);
            CHECK(std::hypot(feet.x - goal.x, feet.z - goal.z) < 0.5f);
            CHECK(frames < 60 * 12);   // round the L is about 20 m: 5 s at 4 m/s, with room for the corners
        }

        // Something unreachable from where it already stands as near as it gets (a wall with no gap in between): it
        // has arrived, and it does not search every frame; nor with no path at all.
        {
            World thin = Room();
            thin.boxes.push_back({ { -0.03f, 0, -10 }, { 0.03f, 3, 10 } });
            CHECK(world.mesh.Build(Area(), Fn(thin)));
            std::vector<Vector3> partial;
            CHECK(world.mesh.FindPath({ -5, 0, 0 }, { 5, 0, 0 }, partial, true) == nav::PathResult::Partial);
            CHECK(world.mesh.FindPath(partial.back(), { 5, 0, 0 }, path, true) == nav::PathResult::Partial && path.size() == 1);
            fly::Nav::Agent agent(4.0f, 0.5f, 0.45f);
            fly::Vec3 feet = V(partial.back());
            agent.SetTarget({ 5.0f, 0, 0 });
            g_findCalls = 0;
            for (int i = 0; i < 60; ++i) agent.Move(feet, 1.0f / 60.0f);
            CHECK(agent.Arrived() && g_findCalls <= 3);
            agent.SetTarget({ 50.0f, 0, 50.0f });     // off the map: no path at all
            g_findCalls = 0;
            for (int i = 0; i < 60; ++i) agent.Move(feet, 1.0f / 60.0f);
            CHECK(!agent.HasPath() && g_findCalls <= 3);
            CHECK(world.mesh.Build(Area(), Fn(w)));
        }

        // Held in place chasing a target that hops 1.2 m every 0.25 s: it still finds out it is stuck.
        {
            fly::Nav::Agent agent(4.0f, 0.4f, 0.45f);
            const fly::Vec3 at{ 3.0f, 0, -3.0f };
            bool stuck = false;
            for (int i = 0; i < 60 * 4; ++i) {
                agent.SetTarget({ 3.0f + 1.2f * static_cast<float>((i / 15) % 2), 0, 5.0f });
                agent.Step(at, 1.0f / 60.0f);
                stuck |= agent.Stuck();
            }
            CHECK(stuck);
        }

        // Nowhere near the map (inside a 3 m block): Move leaves it where it is instead of walking through.
        {
            World blocky = Room();
            blocky.boxes.push_back({ { -1.5f, 0, -1.5f }, { 1.5f, 2, 1.5f } });
            CHECK(world.mesh.Build(Area(), Fn(blocky)));
            fly::Nav::Agent agent(4.0f);
            fly::Vec3 feet{ 0.1f, 0, 0.1f };
            agent.SetTarget({ 5.0f, 0, 5.0f });
            fly::Vec3 v{ 1, 0, 1 };
            for (int i = 0; i < 30; ++i) v = agent.Move(feet, 1.0f / 60.0f);
            CHECK(v.x == 0.0f && v.z == 0.0f && feet.x == 0.1f && feet.z == 0.1f);
        }

        // Up the ramp, frame by frame, to the top.
        {
            World r = Room();
            r.ramps.push_back({ -8, 2, -1, 1, 3.0f });
            CHECK(world.mesh.Build(Area(), Fn(r)));
            fly::Nav::Agent agent(3.0f, 0.4f, 0.45f);
            fly::Vec3 feet{ -9.0f, 0, 0.1f };
            agent.SetTarget({ 1.0f, 2.7f, 0 });
            bool stuck = false;
            for (int i = 0; i < 60 * 10 && !agent.Arrived(); ++i) { agent.Move(feet, 1.0f / 60.0f); stuck |= agent.Stuck(); }
            CHECK(agent.Arrived() && !stuck && feet.y > 2.4f && feet.x > 0.5f);
            CHECK(world.mesh.Build(Area(), Fn(w)));
        }

        // Move(Object): the body's position sits `feetToCentre` above its feet and keeps doing so.
        {
            fly::Nav::Agent agent(4.0f, 0.4f, 0.45f);
            g_bodyPos = { 3.0f, 0.75f, -3.0f };
            agent.SetTarget({ 3.0f, 0, 3.0f });
            for (int i = 0; i < 60 * 5 && !agent.Arrived(); ++i) agent.Move(fly::Object(1), 1.0f / 60.0f, 0.75f);
            CHECK(agent.Arrived() && std::fabs(g_bodyPos.y - 0.75f) < 1e-3f && std::fabs(g_bodyPos.z - 3.0f) < 0.5f);
        }

        // Held in place: after a stuck time it asks for a new path, after another it reports Stuck; moving clears it.
        {
            fly::Nav::Agent agent(4.0f, 10.0f, 0.45f);
            agent.SetStuckTime(0.5f);
            const fly::Vec3 at{ 3.0f, 0, -3.0f };
            agent.SetTarget({ 3.0f, 0, 5.0f });
            bool early = false;
            for (int i = 0; i < 40; ++i) { agent.Step(at, 1.0f / 60.0f); early |= agent.Stuck(); }
            CHECK(!early);
            for (int i = 0; i < 40; ++i) agent.Step(at, 1.0f / 60.0f);
            CHECK(agent.Stuck());
            agent.Step({ 3.0f, 0, -1.0f }, 1.0f / 60.0f);
            CHECK(!agent.Stuck());
        }

        // An engine without the new calls: MoveAlong just adds the step, the agent walks as before.
        {
            const FlyApi older = NavApi(false);
            fly::detail::Api() = &older;
            fly::Vec3 out;
            CHECK(!fly::Nav::CanMove() && !fly::Nav::LineClear({ 1, 0, 1 }, { 2, 0, 2 }));
            CHECK(!fly::Nav::MoveAlong({ 1, 0, 1 }, { -5, 0, 0 }, out) && out.x == -4.0f);
            fly::Nav::Agent agent(4.0f);
            agent.SetTarget({ 3.0f, 0, 3.0f });
            fly::Vec3 feet{ 3.0f, 0, -3.0f };
            CHECK(agent.Move(feet, 0.1f).z > 0.0f && feet.z > -3.0f);
        }
        fly::detail::Api() = nullptr;
        nav::ResetWorld();
    }

    std::printf(g_fail ? "navigation_test: %d FAILED\n" : "navigation_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
