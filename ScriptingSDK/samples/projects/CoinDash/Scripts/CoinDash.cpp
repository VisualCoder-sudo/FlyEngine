// Coin Dash - a small platformer course, built entirely by this script when Play starts.
//
// What it shows:
//   * creating and configuring objects from a script (Object::Create, SetSize, SetColor, SetAnchored)
//   * Tween: the platforms rise into place when the level starts, and some keep bobbing
//   * Game::Camera::Get(): the camera is the player's eyes, so it tells us where the player is
//   * particles: Emit("Sparks", ...) when a coin is picked up, a firework when you win
//   * ShowText for the score, and Events for letting parts of a game talk to each other
//
// Walk with WASD, jump with Space. Collect every coin.

#include "fly.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace fly;

namespace {

constexpr float kPi = 3.14159265f;

struct Platform {
    Object obj;
    Vec3 home;              // where it ends up
    Tween<Vec3> rise;       // the intro: rises from below
    float delay = 0.0f;
    float bobHeight = 0.0f; // 0 = holds still once it has risen
    float bobPeriod = 3.0f;
};

struct Coin {
    Object obj;
    Vec3 home;
    bool taken = false;
    float phase = 0.0f;
};

// 0 -> 1 -> 0 -> 1 ... as x counts up.
float PingPong(float x) { return 1.0f - std::fabs(std::fmod(x, 2.0f) - 1.0f); }

Object MakeBlock(const char* shape, Vec3 pos, Vec3 size, Color3 color, const char* name) {
    Object o = Object::Create(shape);
    o.SetName(name);
    o.SetSize(size);
    o.SetPosition(pos);
    o.SetColor(color);
    o.SetAnchored(true);   // stays where it is, nothing pushes it
    return o;
}

} // namespace

struct CoinDash : Script {
    Task Run() override {
        Print("Coin Dash: building the course...");

        // ---- the course: a spiral of platforms climbing away from the start ----
        std::vector<Platform> platforms;
        std::vector<Coin> coins;
        const int count = 14;
        for (int i = 0; i < count; i++) {
            const float a = 0.2f + (float)i * 0.62f;           // angle round the start
            const float r = 9.0f + (float)i * 0.45f;           // spiralling outwards
            const float h = 0.7f + (float)i * 0.85f;           // and upwards
            const Vec3 pos = { std::cos(a) * r, h, std::sin(a) * r };
            const float t = (float)i / (float)(count - 1);
            const Color3 color = { (unsigned char)(60 + 190 * t), (unsigned char)(170 - 90 * t), (unsigned char)(255 - 180 * t) };

            Platform p;
            p.obj = MakeBlock("Cube", { pos.x, pos.y - 8.0f, pos.z }, { 4.2f, 0.6f, 4.2f }, color, ("Platform " + std::to_string(i + 1)).c_str());
            p.home = pos;
            p.rise = Tween<Vec3>(Vec3{ pos.x, pos.y - 8.0f, pos.z }, pos, 1.1f, Ease::Back);
            p.delay = 0.12f * (float)i;
            if (i % 3 == 2) { p.bobHeight = 1.2f; p.bobPeriod = 2.4f + 0.3f * (float)(i % 4); }
            platforms.push_back(p);

            Coin c;
            c.home = { pos.x, pos.y + 1.4f, pos.z };
            c.obj = MakeBlock("Cylinder", c.home, { 0.9f, 0.12f, 0.9f }, { 255, 205, 40 }, ("Coin " + std::to_string(i + 1)).c_str());
            c.obj.SetCanCollide(false);   // you walk through a coin
            c.obj.SetRotation({ 90.0f, 0.0f, 0.0f });
            c.phase = (float)i;
            coins.push_back(c);
        }
        // A crown at the top.
        {
            Coin c;
            c.home = { platforms.back().home.x, platforms.back().home.y + 3.0f, platforms.back().home.z };
            c.obj = MakeBlock("Sphere", c.home, { 1.1f, 1.1f, 1.1f }, { 255, 90, 160 }, "Gem");
            c.obj.SetCanCollide(false);
            coins.push_back(c);
        }

        const int total = (int)coins.size();
        int collected = 0;
        float clock = 0.0f;
        bool won = false;

        // Other scripts (or later versions of this one) could listen for this.
        Events::On("coin", [total](float n) { ShowText("Coins: " + std::to_string((int)n) + " / " + std::to_string(total), "Top Left"); });
        Events::Fire("coin", 0.0f);
        ShowText("Collect every coin!  WASD to move, Space to jump.", "Bottom Center", 6.0f);

        while (true) {
            const float dt = DeltaTime();
            clock += dt;
            const Vec3 eyes = Game::Camera::Get().position;

            // Platforms rise into place, then the bobbing ones keep moving.
            for (Platform& p : platforms) {
                if (clock < p.delay) continue;
                if (!p.rise.Done()) {
                    p.obj.SetPosition(p.rise.Step(dt));
                } else if (p.bobHeight > 0.0f) {
                    const float k = Ease01(Ease::InOut, PingPong((clock - p.delay) / p.bobPeriod));
                    p.obj.SetPosition({ p.home.x, p.home.y + p.bobHeight * (k - 0.5f), p.home.z });
                }
            }

            // Coins spin and bob; touching one picks it up.
            for (Coin& c : coins) {
                if (c.taken) continue;
                const float bob = std::sin(clock * 2.2f + c.phase) * 0.12f;
                c.obj.SetPosition({ c.home.x, c.home.y + bob, c.home.z });
                const Vec3 r = c.obj.Rotation();
                c.obj.SetRotation({ r.x, r.y + 140.0f * dt, r.z });

                if (Distance(eyes, c.obj.Position()) < 2.4f) {
                    c.taken = true;
                    collected++;
                    Emit("Sparks", c.obj.Position(), { 0, 1, 0 }, 36, 1.0f);
                    c.obj.Destroy();
                    Events::Fire("coin", (float)collected);
                }
            }

            if (!won && collected == total) {
                won = true;
                char buf[96];
                std::snprintf(buf, sizeof buf, "You got them all in %.1f seconds!", clock);
                ShowText(buf, "Center", 0.0f);
                for (int i = 0; i < 6; i++) {
                    const float a = (float)i / 6.0f * 2.0f * kPi;
                    const Vec3 at = platforms.back().home + Vec3{ std::cos(a) * 3.0f, 4.0f + (float)(i % 3), std::sin(a) * 3.0f };
                    Emit("Sparks", at, { 0, 1, 0 }, 60, 1.6f);
                }
            }
            co_await NextFrame();
        }
    }
};
FLY_SCRIPT(CoinDash)
