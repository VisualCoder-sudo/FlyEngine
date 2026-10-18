// Campfire Valley - a terrain with grass, flowers, bushes, trees and rocks, and a campfire to sit by.
//
// The valley itself (terrain and foliage) is part of the scene; this script adds life to it:
//   * a campfire from emitters: flames, rising smoke, and sparks that pop now and then
//   * Cast() straight down to find the ground, so the camp stands on the terrain wherever it is
//   * the day passing (Game::Environment), wind that comes and goes, rain you can switch on
//   * dust kicked up by your footsteps
//
// Controls:  WASD walk, Space jump    R toggle rain    T next time of day    1 / 2 / 3 wind calm / breezy / storm

#include "fly.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace fly;

namespace {

Object Make(const char* shape, Vec3 pos, Vec3 size, Vec3 rot, Color3 color, const char* name) {
    Object o = Object::Create(shape);
    o.SetName(name);
    o.SetSize(size);
    o.SetPosition(pos);
    o.SetRotation(rot);
    o.SetColor(color);
    o.SetAnchored(true);
    return o;
}

// The ground height under (x, z).
float GroundAt(float x, float z, float fallback) {
    const Hit h = Cast({ x, 200.0f, z }, { 0, -1, 0 }, 400.0f);
    return h ? h.point.y : fallback;
}

const char* kWindNames[3] = { "calm", "breezy", "storm" };

} // namespace

struct CampfireValley : Script {
    Task Run() override {
        // The physics world needs a moment to build the terrain's collision.
        co_await WaitFrames(3);

        // ---- the camp: a ring of stones round the fire, two logs to sit on ----
        const float cx = 7.0f, cz = 9.0f;
        const float gy = GroundAt(cx, cz, 0.0f);
        const Vec3 fire = { cx, gy, cz };
        for (int i = 0; i < 9; i++) {
            const float a = (float)i / 9.0f * 6.2831853f;
            const float x = cx + std::cos(a) * 1.25f, z = cz + std::sin(a) * 1.25f;
            const float shade = 95.0f + 12.0f * (float)(i % 3);
            Make("Sphere", { x, GroundAt(x, z, gy) + 0.12f, z }, { 0.55f, 0.38f, 0.5f }, { 0, a * 57.3f, 0 },
                 { (unsigned char)shade, (unsigned char)(shade - 4.0f), (unsigned char)(shade - 10.0f) }, "Fire Stone");
        }
        Make("Cylinder", { cx - 2.6f, gy + 0.3f, cz + 0.4f }, { 0.55f, 2.4f, 0.55f }, { 0, 0, 90 }, { 110, 78, 50 }, "Log");
        Make("Cylinder", { cx + 0.5f, gy + 0.3f, cz - 2.7f }, { 0.55f, 2.4f, 0.55f }, { 90, 20, 0 }, { 104, 74, 48 }, "Log");
        // A scorch mark where the fire burns.
        Decal({ cx, gy + 0.02f, cz }, { 0, 1, 0 }, 3.2f, { 12, 10, 8, 200 }, 100000.0f);

        // ---- the fire ----
        Emitter flames = Emitter::Start("Fire", fire + Vec3{ 0, 0.15f, 0 }, { 0, 1, 0 }, 1.1f);
        Emitter smoke = Emitter::Start("Smoke", fire + Vec3{ 0, 1.3f, 0 }, { 0, 1, 0 }, 1.5f);
        (void)flames; (void)smoke;

        Game::Environment::SetTimeOfDay(18.0f);   // golden hour
        Game::Environment::SetDayLength(3.0f);    // a day lasts three real minutes
        ShowText("WASD walk   R rain   T time of day   1 2 3 wind", "Bottom Center", 7.0f);

        float clock = 0.0f, nextSpark = 1.0f, stepTimer = 0.0f, hudTimer = 0.0f;
        int wind = 1;
        float windTarget = 4.0f;
        bool rain = false;
        float rainAmount = 0.0f;
        Vec3 last = Game::Camera::Get().position;

        while (true) {
            const float dt = DeltaTime();
            clock += dt;

            // ---- sparks pop out of the fire every now and then ----
            nextSpark -= dt;
            if (nextSpark <= 0.0f) {
                Emit("Sparks", fire + Vec3{ 0, 0.6f, 0 }, { 0.1f, 1, 0 }, 14, 0.9f);
                nextSpark = 0.6f + (float)((int)(clock * 977.0f) % 100) / 100.0f * 1.8f;
            }

            // ---- wind: a base speed with slow gusts on top ----
            if (KeyPressed("1")) { wind = 0; windTarget = 1.5f; }
            if (KeyPressed("2")) { wind = 1; windTarget = 5.0f; }
            if (KeyPressed("3")) { wind = 2; windTarget = 15.0f; }
            const float gust = 1.0f + 0.35f * std::sin(clock * 0.7f) + 0.2f * std::sin(clock * 2.3f);
            Game::Environment::SetWindSpeed(Lerp(Game::Environment::WindSpeed(), windTarget * gust, 1.0f - std::exp(-dt * 1.5f)));

            // ---- R: rain comes and goes smoothly; T: jump ahead in the day ----
            if (KeyPressed("R")) rain = !rain;
            rainAmount = Lerp(rainAmount, rain ? 0.8f : 0.0f, 1.0f - std::exp(-dt * 0.8f));
            Game::Environment::SetRain(rainAmount);
            Game::Environment::SetOvercast(0.15f + 0.7f * rainAmount);
            Game::Environment::SetWetGround(rainAmount > 0.05f ? rainAmount : Game::Environment::WetGround() * (1.0f - dt * 0.05f));
            if (KeyPressed("T")) {
                float h = Game::Environment::TimeOfDay() + 4.0f;
                if (h >= 24.0f) h -= 24.0f;
                Game::Environment::SetTimeOfDay(h);
            }

            // ---- footsteps kick up dust while you walk ----
            const Vec3 eyes = Game::Camera::Get().position;
            const float moved = Length(Vec3{ eyes.x - last.x, 0.0f, eyes.z - last.z });
            last = eyes;
            stepTimer -= dt;
            if (moved > 3.0f * dt && stepTimer <= 0.0f) {
                const Hit ground = Cast(eyes, { 0, -1, 0 }, 5.0f);
                if (ground && ground.distance < 2.4f) Emit("Dust", ground.point, { 0, 1, 0 }, 4, 0.45f);
                stepTimer = 0.32f;
            }

            // ---- a small readout ----
            hudTimer -= dt;
            if (hudTimer <= 0.0f) {
                const float t = Game::Environment::TimeOfDay();
                char buf[128];
                std::snprintf(buf, sizeof buf, "%02d:%02d   wind %s   %s", (int)t, (int)((t - std::floor(t)) * 60.0f), kWindNames[wind], rain ? "raining" : "dry");
                ShowText(buf, "Top Left");
                hudTimer = 0.5f;
            }
            co_await NextFrame();
        }
    }
};
FLY_SCRIPT(CampfireValley)
