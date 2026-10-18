// The scripting API (FlyNative_*, what fly.hpp calls) against the real engine: create, clone, rename, destroy, forces,
// raycast, keys, text. Needs a display (the engine opens a window); not registered with ctest:
// build/tests/script_actions_engine_test
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/Backend/PhysicsSimulation.hpp"
#include "Engine/Backend/ScatteredObject.hpp"
#include "Engine/Scripts/ScriptRuntime.hpp"
#include "Engine/Scripts/FlyScriptApi.hpp"
#include "Engine/Backend/VisualScript.hpp"
#include "Engine/Audio.hpp"
#include "Engine/Frontend/ProjectManager.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
    Engine engine(320, 240, "script_actions_engine_test", 60);
    std::vector<ScatteredObject*> objs;
    std::vector<std::unique_ptr<ModelGroup>> models;
    auto simOwner = std::make_unique<phys::Simulation>(objs);
    phys::Simulation& sim = *simOwner;
    engine.AddEntity(std::move(simOwner));

    auto rtOwner = std::make_unique<ScriptRuntime>();
    ScriptRuntime& rt = *rtOwner;
    engine.AddEntity(std::move(rtOwner));
    rt.BindWorld(objs, models, engine.GetCamera(), sim, engine);
    FlyNative_BindRuntime(&rt);

    // A static floor to hit.
    auto floorOwner = std::make_unique<ScatteredObject>(Vector3{ 0, -0.5f, 0 }, Vector3{ 40, 1, 40 }, GRAY, ShapeType::Cube);
    floorOwner->SetName("Floor");
    floorOwner->anchored = true;
    objs.push_back(floorOwner.get());
    engine.AddEntity(std::move(floorOwner));

    sim.StartPlay();
    rt.SetSimPlaying(true);
    for (int i = 0; i < 5; i++) sim.Update(1.0f / 60.0f);

    // Create, name, find.
    const unsigned long long a = FlyNative_CreateObject("Cube");
    CHECK(a != 0);
    FlyNative_SetName(a, "Crate");
    CHECK(std::strcmp(FlyNative_GetName(a), "Crate") == 0);
    CHECK(FlyNative_FindObject("Crate") == a);
    FlyNative_SetPosition(a, 0, 5, 0);

    // Clone: a second object with the same look; the original is untouched.
    const size_t before = objs.size();
    const unsigned long long b = FlyNative_CloneObject(a);
    CHECK(b != 0 && b != a);
    CHECK(objs.size() == before + 1);
    FlyNative_SetName(b, "Crate2");
    CHECK(FlyNative_FindObject("Crate2") == b && FlyNative_FindObject("Crate") == a);
    CHECK(FlyNative_CloneObject(0) == 0);

    // Raycast straight down from above hits the first thing under it (the crate, then the floor once it is gone).
    for (int i = 0; i < 3; i++) sim.Update(1.0f / 60.0f);
    FlyNative_SetPosition(b, 10, 50, 10);
    const char* hit = FlyNative_Raycast(0, 20, 0, 0, -1, 0, 100.0f);
    std::printf("ray hit: '%s'\n", hit);
    CHECK(std::strcmp(hit, "Crate") == 0 || std::strcmp(hit, "Crate2") == 0);   // made during play, so named only if its body is registered
    CHECK(std::strcmp(FlyNative_Raycast(0, 20, 500, 0, 1, 0, 50.0f), "") == 0);   // up into nothing

    // Impulse: the crate gains upward speed.
    float vx, vy, vz;
    FlyNative_SetPosition(a, 0, 8, 0);
    FlyNative_SetVelocity(a, 0, 0, 0);
    CHECK(FlyNative_AddImpulse(a, 0, FlyNative_GetMass(a) * 10.0f, 0) == 1);
    FlyNative_GetVelocity(a, &vx, &vy, &vz);
    sim.Update(1.0f / 60.0f);
    FlyNative_GetVelocity(a, &vx, &vy, &vz);
    std::printf("velocity after impulse: %.2f\n", vy);
    CHECK(vy > 0.5f);
    CHECK(FlyNative_AddForce(a, 0, 100, 0) == 1);
    CHECK(FlyNative_AddForce(0, 0, 1, 0) == 0);   // bad handle: refused, no crash

    // Destroy: gone from the scene; the stale handle is refused.
    const size_t beforeDestroy = objs.size();
    FlyNative_DestroyObject(b);
    CHECK(objs.size() == beforeDestroy - 1);
    CHECK(FlyNative_FindObject("Crate2") == 0);
    CHECK(FlyNative_GetName(b)[0] == '\0');
    FlyNative_DestroyObject(b);   // twice: harmless
    FlyNative_DestroyObject(0);

    // Keys, text, sound, volume and time must at least be safe with nothing pressed / no audio backend.
    CHECK(FlyNative_KeyDown("W") == 0 && FlyNative_KeyPressed("Space") == 0 && FlyNative_KeyDown("NoSuchKey") == 0);
    CHECK(FlyNative_KeyDown(nullptr) == 0);
    FlyNative_ShowText("Hello", "Center", 2.0f);
    FlyNative_ShowText("", "Center", 0.0f);
    FlyNative_ShowText(nullptr, nullptr, 0.0f);
    FlyNative_PlaySound("nothing.wav", 1.0f);
    FlyNative_PlaySound(nullptr, 1.0f);
    FlyNative_SetMasterVolume(0.5f);
    CHECK(FlyNative_TimeSincePlay() >= 0.0f);

    // A Play Sound node, run by the real engine, ends in sound coming out of the device: project-relative path, the
    // player the Engine installed, the mixer. Skipped (not failed) on a machine with no sound device.
    {
        namespace fs = std::filesystem;
        const fs::path proj = fs::temp_directory_path() / "fly_play_sound_proj";
        fs::create_directories(proj / "assets" / "audio");
        {   // 0.5 s of 8 kHz mono 16-bit silence-free tone
            const uint32_t frames = 4000, dataBytes = frames * 2, riff = 36 + dataBytes, fmt = 16, rate = 8000, byteRate = 16000;
            const uint16_t tag = 1, ch = 1, align = 2, bits = 16;
            std::ofstream w(proj / "assets" / "audio" / "tone.wav", std::ios::binary);
            w.write("RIFF", 4); w.write((const char*)&riff, 4); w.write("WAVEfmt ", 8); w.write((const char*)&fmt, 4);
            w.write((const char*)&tag, 2); w.write((const char*)&ch, 2); w.write((const char*)&rate, 4); w.write((const char*)&byteRate, 4);
            w.write((const char*)&align, 2); w.write((const char*)&bits, 2); w.write("data", 4); w.write((const char*)&dataBytes, 4);
            for (uint32_t i = 0; i < frames; ++i) { const int16_t v = (int16_t)(std::sin(i * 0.2) * 8000); w.write((const char*)&v, 2); }
        }
        rt.SetProjectPath(proj.string());
        vscript::Clear();
        vscript::NewScript("PlaySoundTest");
        const int start = vscript::AddNode(vscript::NodeType::Start, 0, 0).id;
        vscript::Node& play = vscript::AddNode(vscript::NodeType::PlaySound, 100, 0);
        play.target = "assets/audio/tone.wav";
        play.value = "1";
        vscript::Connect(start, play.id);
        if (const char* keep = std::getenv("FLY_KEEP_SOUND_PROJECT")) {   // leaves a project (scene + sound) to run FlyPlayer on
            project::SaveProjectFile(proj.string(), objs, models);
            std::printf("saved a Play Sound project to %s\n", proj.string().c_str());
            (void)keep;
            return 0;
        }
        vscript::Run(rt);
        if (audio::Available()) {
            CHECK(audio::ActiveVoices() == 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            audio::Update();
            CHECK(audio::ActiveVoices() == 0);   // it played through to its end
            std::printf("Play Sound node: played through the sound device\n");
        } else {
            std::printf("Play Sound node: no sound device here, skipped\n");
        }
        vscript::Clear();
        std::error_code ec; fs::remove_all(proj, ec);
    }

    std::printf(g_fail ? "script_actions_engine_test: %d FAILED\n" : "script_actions_engine_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
