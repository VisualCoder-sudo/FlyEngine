// Physics Playground - things to knock over, built by this script when Play starts.
//
// What it shows:
//   * joints: a wrecking ball on a rope (Joint::Rope) and a door on a hinge (Joint::Hinge)
//   * Touches(): every contact that began this frame, with where and how hard
//   * Cast(): a ray that tells you the point and surface it hit (aim with the crosshair)
//   * OverlapSphere(): everything near a point, for an explosion
//   * particles and decals: sparks and scorch marks where things hit
//
// Controls:  F = fire a ball   E = poke what you look at   G = blast where you look   R = rebuild the wall

#include "fly.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace fly;

namespace {

Object Make(const char* shape, Vec3 pos, Vec3 size, Color3 color, const char* name, bool anchored) {
    Object o = Object::Create(shape);
    o.SetName(name);
    o.SetSize(size);
    o.SetPosition(pos);
    o.SetColor(color);
    o.SetAnchored(anchored);
    return o;
}

// Where the player is looking: from the eyes along the view.
void Gaze(Vec3& origin, Vec3& dir) {
    const Game::Camera::View v = Game::Camera::Get();
    origin = v.position;
    dir = Normalize(v.target - v.position);
}

struct Wall {
    std::vector<Object> bricks;
    void Build(Vec3 centre) {
        for (Object& b : bricks) b.Destroy();
        bricks.clear();
        const int cols = 6, rows = 6;
        for (int y = 0; y < rows; y++) {
            for (int x = 0; x < cols; x++) {
                const float shift = (y % 2) ? 0.5f : 0.0f;   // brick bond
                const Vec3 pos = { centre.x + ((float)x - cols * 0.5f + shift) * 1.25f, centre.y + 0.4f + (float)y * 0.82f, centre.z };
                const bool dark = (x + y) % 2 == 0;
                bricks.push_back(Make("Cube", pos, { 1.2f, 0.8f, 1.0f }, dark ? Color3{ 190, 85, 60 } : Color3{ 215, 120, 80 }, "Brick", false));
            }
        }
    }
};

struct Shot { Object obj; float age = 0.0f; };

} // namespace

struct PhysicsPlayground : Script {
    Task Run() override {
        Print("Physics Playground: building...");

        // ---- a brick wall ----
        Wall wall;
        const Vec3 wallAt = { 0.0f, 0.0f, 14.0f };
        wall.Build(wallAt);

        // ---- a wrecking ball hanging from a crane ----
        const Vec3 craneTop = { -12.0f, 12.0f, 6.0f };
        Make("Cube", { craneTop.x, craneTop.y * 0.5f, craneTop.z }, { 0.8f, craneTop.y, 0.8f }, { 90, 90, 100 }, "Crane Post", true);
        Object arm = Make("Cube", { craneTop.x + 3.0f, craneTop.y + 0.4f, craneTop.z }, { 7.0f, 0.8f, 0.8f }, { 90, 90, 100 }, "Crane Arm", true);
        const Vec3 hook = { craneTop.x + 6.0f, craneTop.y, craneTop.z };
        Object ball = Make("Sphere", { hook.x, hook.y - 8.0f, hook.z }, { 2.4f, 2.4f, 2.4f }, { 60, 60, 70 }, "Wrecking Ball", false);
        ball.SetMass(60.0f);
        Joint::Rope(arm, ball, hook, ball.Position() + Vec3{ 0, 1.2f, 0 });
        // Pull the ball back and let go: it swings through the wall's side.
        ball.SetPosition({ hook.x - 3.2f, hook.y - 7.2f, hook.z });   // exactly a rope's length (6.8 m) from the hook
        ball.SetVelocity({ 0, 0, 0 });

        // ---- a door on a hinge ----
        Object doorPost = Make("Cube", { 12.0f, 1.6f, 4.0f }, { 0.4f, 3.2f, 0.4f }, { 70, 70, 80 }, "Door Post", true);
        Object door = Make("Cube", { 12.0f, 1.5f, 6.0f }, { 0.3f, 3.0f, 3.6f }, { 120, 160, 220 }, "Door", false);
        Joint::Hinge(doorPost, door, { 12.0f, 1.5f, 4.0f });

        // ---- some loose crates to play with ----
        for (int i = 0; i < 8; i++)
            Make("Cube", { 6.0f + (float)(i % 4) * 1.6f, 0.6f + (float)(i / 4) * 1.3f, -8.0f }, { 1.2f, 1.2f, 1.2f }, { 200, 170, 100 }, "Crate", false);

        // The hit counter shows how often something struck hard.
        int hits = 0;
        float hardest = 0.0f;
        Events::On("hit", [&hits, &hardest](float speed) {
            hits++;
            hardest = std::max(hardest, speed);
            char buf[96];
            std::snprintf(buf, sizeof buf, "Hits: %d   hardest: %.1f m/s", hits, hardest);
            ShowText(buf, "Top Left");
        });
        Events::Fire("hit", 0.0f);
        hits = 0;
        ShowText("F fire a ball   E poke   G blast   R rebuild the wall", "Bottom Center", 8.0f);

        std::vector<Shot> shots;

        while (true) {
            const float dt = DeltaTime();

            // ---- every contact that began: sparks and a scorch mark where it hit hard ----
            for (const Touch& t : Touches()) {
                if (t.speed < 4.0f) continue;
                Emit("Sparks", t.point, t.normal, 12 + (int)t.speed, 0.8f);
                if (t.normal.y > 0.7f) Decal(t.point, t.normal, 0.9f, { 25, 22, 20, 150 }, 20.0f);
                Events::Fire("hit", t.speed);
            }

            Vec3 eye, dir;
            Gaze(eye, dir);

            // ---- F: fire a ball along the view ----
            if (KeyPressed("F")) {
                Shot s;
                s.obj = Make("Sphere", eye + dir * 1.5f, { 0.7f, 0.7f, 0.7f }, { 240, 200, 60 }, "Ball", false);
                s.obj.SetVelocity(dir * 34.0f);
                shots.push_back(s);
            }

            // ---- E: poke whatever the crosshair is on ----
            if (KeyPressed("E")) {
                const Hit h = Cast(eye, dir, 60.0f);
                if (h) {
                    Decal(h.point, h.normal, 0.6f, { 20, 20, 25, 190 }, 15.0f);
                    Emit("Dust", h.point, h.normal, 10, 0.7f);
                    if (h.object) { Object target = h.object; target.AddImpulse(dir * 40.0f); }
                }
            }

            // ---- G: a blast: everything near the point is thrown away from it ----
            if (KeyPressed("G")) {
                const Hit h = Cast(eye, dir, 80.0f);
                if (h) {
                    const Vec3 centre = h.point + h.normal * 0.5f;
                    Emit("Fire", centre, { 0, 1, 0 }, 40, 2.0f);
                    Emit("Smoke", centre, { 0, 1, 0 }, 30, 2.2f);
                    Emit("Debris", centre, h.normal, 30, 1.4f);
                    Decal(h.point, h.normal, 4.0f, { 15, 12, 10, 200 }, 40.0f);
                    for (Object o : OverlapSphere(centre, 7.0f)) {
                        const Vec3 away = o.Position() - centre;
                        const float d = Length(away);
                        if (d < 0.05f || o.Anchored()) continue;
                        o.AddImpulse(Normalize(away + Vec3{ 0, 1.0f, 0 }) * (140.0f * (1.0f - d / 8.0f)));
                    }
                }
            }

            // ---- R: a fresh wall ----
            if (KeyPressed("R")) wall.Build(wallAt);

            // Balls disappear after a while.
            for (size_t i = 0; i < shots.size();) {
                shots[i].age += dt;
                if (shots[i].age > 18.0f) { shots[i].obj.Destroy(); shots.erase(shots.begin() + (long)i); }
                else ++i;
            }

            co_await NextFrame();
        }
    }
};
FLY_SCRIPT(PhysicsPlayground)
