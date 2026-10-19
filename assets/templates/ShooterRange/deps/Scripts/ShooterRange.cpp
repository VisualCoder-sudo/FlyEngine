// Shooter Range - pop-up targets at three distances, built by this script when Play starts.
//
// What it shows:
//   * Cast(): a hitscan shot along the view, with the point and surface it hit
//   * Object comparison: telling which target the ray struck
//   * Tween: targets fall when hit and pop back up
//   * particles and decals: sparks and bullet holes where a shot lands
//   * ShowText(): score, time and ammo on screen
//
// Controls:  F = shoot   R = reload (or restart when the round is over)
// Nearer targets are worth less. Hit all you can before the time runs out.

#include "fly.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace fly;

namespace {

constexpr float kRoundSeconds = 60.0f;
constexpr float kReloadSeconds = 1.2f;
constexpr int kMagazine = 8;

struct Target {
    Object obj;
    Vec3 home;
    int points = 0;
    float sway = 0.0f;       // metres either side of home; 0 = holds still
    float phase = 0.0f;
    bool up = true;
    float downFor = 0.0f;    // seconds until it pops up again
    Tween<Vec3> motion;      // the fall and the pop-up
};

Object Make(const char* shape, Vec3 pos, Vec3 size, Color3 color, const char* name) {
    Object o = Object::Create(shape);
    o.SetName(name);
    o.SetSize(size);
    o.SetPosition(pos);
    o.SetColor(color);
    o.SetAnchored(true);
    return o;
}

} // namespace

struct ShooterRange : Script {
    Task Run() override {
        Print("Shooter Range: building...");

        // ---- the firing line, a back wall and three rows of targets ----
        Make("Cube", { 0.0f, 0.5f, -1.0f }, { 8.0f, 1.0f, 0.6f }, { 90, 70, 50 }, "Firing Line");
        Make("Cube", { 0.0f, 3.0f, 52.0f }, { 40.0f, 6.0f, 1.0f }, { 110, 100, 90 }, "Back Wall");

        std::vector<Target> targets;
        const float rowZ[3] = { 12.0f, 26.0f, 42.0f };
        const int rowPoints[3] = { 10, 25, 50 };
        const float rowSize[3] = { 1.4f, 1.8f, 2.4f };   // farther targets are bigger, so they stay fair
        for (int row = 0; row < 3; row++) {
            for (int i = 0; i < 5; i++) {
                Target t;
                t.home = { ((float)i - 2.0f) * (4.0f + (float)row * 2.0f), 1.6f, rowZ[row] };
                t.points = rowPoints[row];
                t.sway = row == 2 ? 2.5f : 0.0f;
                t.phase = (float)i * 1.3f;
                t.obj = Make("Cube", t.home, { rowSize[row], rowSize[row], 0.15f }, { 220, 60, 50 }, "Target");
                targets.push_back(t);
            }
        }

        int score = 0, ammo = kMagazine, shots = 0, hitsLanded = 0;
        float timeLeft = kRoundSeconds;
        float reloadFor = 0.0f;
        bool over = false;

        const auto status = [&] {
            char buf[128];
            std::snprintf(buf, sizeof buf, "Score %d   Time %02d   Ammo %d/%d", score, (int)std::ceil(timeLeft), ammo, kMagazine);
            ShowText(buf, "Top Left");
        };
        status();
        ShowText("F shoot   R reload", "Bottom Center", 8.0f);

        while (true) {
            const float dt = DeltaTime();

            if (!over) {
                timeLeft -= dt;
                if (timeLeft <= 0.0f) {
                    timeLeft = 0.0f;
                    over = true;
                    char buf[160];
                    std::snprintf(buf, sizeof buf, "Time! Score %d   %d of %d shots hit   R to play again", score, hitsLanded, shots);
                    ShowText(buf, "Top Center");
                }

                // ---- reload ----
                if (reloadFor > 0.0f) {
                    reloadFor -= dt;
                    if (reloadFor <= 0.0f) ammo = kMagazine;
                } else if (KeyPressed("R") && ammo < kMagazine) {
                    reloadFor = kReloadSeconds;
                }

                // ---- shoot along the view ----
                if (KeyPressed("F") && ammo > 0 && reloadFor <= 0.0f) {
                    ammo--;
                    shots++;
                    const Game::Camera::View v = Game::Camera::Get();
                    const Hit h = Cast(v.position, Normalize(v.target - v.position), 120.0f);
                    if (h) {
                        Emit("Sparks", h.point, h.normal, 6, 0.5f);
                        Decal(h.point, h.normal, 0.25f, { 15, 15, 15, 220 }, 25.0f);
                        for (Target& t : targets) {
                            if (!t.up || !(t.obj == h.object)) continue;
                            t.up = false;
                            t.downFor = 2.5f;
                            t.motion = Tween<Vec3>(t.obj.Position(), { t.home.x, t.home.y - 3.0f, t.home.z }, 0.35f, Ease::Back);
                            score += t.points;
                            hitsLanded++;
                            break;
                        }
                    }
                    if (ammo == 0) reloadFor = kReloadSeconds;
                }
            } else if (KeyPressed("R")) {
                score = shots = hitsLanded = 0;
                ammo = kMagazine;
                timeLeft = kRoundSeconds;
                reloadFor = 0.0f;
                over = false;
                for (Target& t : targets) { t.up = true; t.motion = Tween<Vec3>(); t.obj.SetPosition(t.home); }
                ShowText("", "Top Center");
            }

            // ---- targets: sway, fall when hit, pop back up ----
            for (Target& t : targets) {
                if (!t.motion.Done()) {
                    t.obj.SetPosition(t.motion.Step(dt));
                } else if (t.up) {
                    Vec3 p = t.home;
                    if (t.sway > 0.0f) p.x += std::sin(TimeSincePlay() * 1.2f + t.phase) * t.sway;
                    t.obj.SetPosition(p);
                } else {
                    t.downFor -= dt;
                    if (t.downFor <= 0.0f && !over) {
                        t.up = true;
                        t.motion = Tween<Vec3>(t.obj.Position(), t.home, 0.4f, Ease::Back);
                    }
                }
            }

            if (!over) status();
            co_await NextFrame();
        }
    }
};
FLY_SCRIPT(ShooterRange)
