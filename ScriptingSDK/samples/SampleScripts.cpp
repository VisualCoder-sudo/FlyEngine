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

// Wanders over the level: bakes the walkable ground once, then keeps walking to random spots within 25 m.
// Put it on an object. Needs floors the object can stand on; `fly::Nav::ShowDebug()` draws the map.
struct NavPatrolScript : fly::Script {
    fly::Task Run() override {
        fly::Object self = fly::Object::Self();
        if (!self) co_return;

        if (!fly::Nav::Ready() && !fly::Nav::Build({ -60, -5, -60 }, { 60, 30, 60 })) {
            fly::Print("[NavPatrolScript] could not bake the level (is there a floor in the box?)");
            co_return;
        }
        fly::Nav::Agent agent(3.0f);
        while (true) {
            fly::Vec3 spot;
            if (!fly::Nav::RandomPoint(self.Position(), 25.0f, spot)) { co_await fly::Wait(1.0f); continue; }
            agent.SetTarget(spot);
            float giveUp = 15.0f;   // seconds, in case something blocks the way
            while (!agent.Arrived() && giveUp > 0.0f) {
                const float dt = fly::DeltaTime();
                self.SetPosition(self.Position() + agent.Step(self.Position(), dt) * dt);
                giveUp -= dt;
                co_await fly::NextFrame();
            }
            co_await fly::Wait(1.0f);
        }
    }
};
FLY_SCRIPT(NavPatrolScript)

// A pause menu: Tab opens it and frees the mouse; Resume closes it; the slider sets the volume.
struct PauseMenuScript : fly::Script {
    fly::Task Run() override {
        using namespace fly::UI;
        bool open = false;
        while (true) {
            if (fly::KeyPressed("Tab")) {
                open = !open;
                if (open) {
                    Panel("pause_back", { Anchor::Fill, 0, 0 }, { 0, 0, 0, 150 });
                    Text("pause_title", "Paused", { Anchor::Center, 0, -120 }, { 255, 255, 255, 255 }, 48);
                    Button("pause_resume", "Resume", { Anchor::Center, 0, -40, 220, 48 });
                    Text("pause_vol_label", "Volume", { Anchor::Center, -110, 20 }, { 255, 255, 255, 255 }, 20);
                    Slider("pause_vol", 1.0f, { Anchor::Center, -110, 50, 220, 22 });
                } else {
                    for (const char* id : { "pause_back", "pause_title", "pause_resume", "pause_vol_label", "pause_vol" }) Remove(id);
                }
                SetMouseFree(open);
            }
            if (open) {
                if (Find("pause_resume").Clicked()) {
                    for (const char* id : { "pause_back", "pause_title", "pause_resume", "pause_vol_label", "pause_vol" }) Remove(id);
                    SetMouseFree(false);
                    open = false;
                }
                else if (Find("pause_vol").Changed()) fly::SetVolume(Find("pause_vol").Value());
            }
            co_await fly::NextFrame();
        }
    }
};
FLY_SCRIPT(PauseMenuScript)
