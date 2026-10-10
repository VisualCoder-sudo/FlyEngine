// SampleScripts.cpp - sample scripts for testing the Flyengine C++ SDK.
// Copy into a project's Scripts/ folder to try them.

#include "fly.hpp"

#include <cmath>

struct RotateScript : fly::Script {
    fly::Task Run() override {
        fly::Object self = fly::Object::Self();
        if (!self) {
            fly::Print("[RotateScript] No self object - running as standalone");
            co_return;
        }

        fly::Print("[RotateScript] Started on ", self.Handle());

        while (true) {
            fly::Vec3 rot = self.Rotation();
            rot.y += 45.0f * fly::DeltaTime();
            self.SetRotation(rot);
            co_await fly::NextFrame();
        }
    }
};
FLY_SCRIPT(RotateScript)

struct SpawnerScript : fly::Script {
    fly::Task Run() override {
        fly::Print("[SpawnerScript] Started");

        int count = 0;
        while (true) {
            co_await fly::Wait(2.0f);

            fly::Object obj = fly::Object::Create("Cube");
            if (obj) {
                obj.SetPosition({ 0, 3.0f + count * 2.0f, 0 });
                obj.SetSize({ 1, 1, 1 });
                obj.SetColor({ static_cast<unsigned char>(count * 50 % 255),
                               static_cast<unsigned char>(100 + count * 30 % 155),
                               static_cast<unsigned char>(200 + count * 20 % 55) });
                obj.SetAnchored(false);
                fly::Print("[SpawnerScript] Spawned cube #", count);
                count++;
            }
        }
    }
};
FLY_SCRIPT(SpawnerScript)

struct LightPulseScript : fly::Script {
    fly::Task Run() override {
        fly::Print("[LightPulseScript] Started - pulsing ambient light");

        float t = 0.0f;
        while (true) {
            fly::Game::Lighting::SetAmbient(0.6f + 0.4f * std::sin(t));
            t += 3.0f * fly::DeltaTime();
            co_await fly::NextFrame();
        }
    }
};
FLY_SCRIPT(LightPulseScript)

struct PhysicsTestScript : fly::Script {
    fly::Task Run() override {
        fly::Object self = fly::Object::Self();
        if (!self) co_return;

        fly::Print("[PhysicsTestScript] Applying upward impulse");
        self.SetVelocity({ 0, 15, 0 });
        self.SetAnchored(false);

        while (self.Position().y >= 0.5f)
            co_await fly::NextFrame();

        fly::Print("[PhysicsTestScript] Object hit ground");
    }
};
FLY_SCRIPT(PhysicsTestScript)
