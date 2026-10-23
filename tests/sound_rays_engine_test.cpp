// Sound rays against the real engine: a Play Sound node on an object, rays through the real physics world, and the
// mixed output measured with a device-less audio engine. A wall between the fan and the listener muffles it; remove the
// wall and the sound opens up; destroy the fan and its sound stops. Needs a display (the engine opens a window); not
// registered with ctest: build/tests/sound_rays_engine_test
#include "test_unique.hpp"
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/Audio.hpp"
#include "Engine/Backend/PhysicsSimulation.hpp"
#include "Engine/Backend/ScatteredObject.hpp"
#include "Engine/Backend/SoundRays.hpp"
#include "Engine/Backend/VisualScript.hpp"
#include "Engine/Scripts/ScriptRuntime.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void WriteTone(const std::filesystem::path& path, uint32_t rate, float seconds, float hz) {
    const uint32_t frames = static_cast<uint32_t>(rate * seconds), dataBytes = frames * 2, riff = 36 + dataBytes, fmt = 16, byteRate = rate * 2;
    const uint16_t tag = 1, ch = 1, align = 2, bits = 16;
    std::ofstream w(path, std::ios::binary);
    w.write("RIFF", 4); w.write((const char*)&riff, 4); w.write("WAVEfmt ", 8); w.write((const char*)&fmt, 4);
    w.write((const char*)&tag, 2); w.write((const char*)&ch, 2); w.write((const char*)&rate, 4); w.write((const char*)&byteRate, 4);
    w.write((const char*)&align, 2); w.write((const char*)&bits, 2); w.write("data", 4); w.write((const char*)&dataBytes, 4);
    for (uint32_t i = 0; i < frames; ++i) {
        const int16_t v = (int16_t)(std::sin(6.2831853f * hz * (float)i / (float)rate) * 20000.0f);
        w.write((const char*)&v, 2);
    }
}

int main() {
    namespace fs = std::filesystem;
    const fs::path proj = fs::temp_directory_path() / TestUnique("fly_sound_rays_proj");
    fs::create_directories(proj);
    WriteTone(proj / "hum.wav", 48000, 2.0f, 6000.0f);   // a high hum: what a wall takes away

    Engine engine(320, 240, "sound_rays_engine_test", 60);
    std::vector<ScatteredObject*> objs;
    std::vector<std::unique_ptr<ModelGroup>> models;
    auto simOwner = std::make_unique<phys::Simulation>(objs);
    phys::Simulation& sim = *simOwner;
    engine.AddEntity(std::move(simOwner));
    auto rtOwner = std::make_unique<ScriptRuntime>();
    ScriptRuntime& rt = *rtOwner;
    engine.AddEntity(std::move(rtOwner));
    rt.BindWorld(objs, models, engine.GetCamera(), sim, engine);
    rt.SetProjectPath(proj.string());

    auto fanOwner = std::make_unique<ScatteredObject>(Vector3{ 12, 1, 0 }, Vector3{ 1, 1, 1 }, GRAY, ShapeType::Cube);
    fanOwner->SetName("Fan"); fanOwner->anchored = true;
    ScatteredObject* fan = fanOwner.get();
    objs.push_back(fan); engine.AddEntity(std::move(fanOwner));
    auto wallOwner = std::make_unique<ScatteredObject>(Vector3{ 6, 1, 0 }, Vector3{ 0.4f, 8, 8 }, GRAY, ShapeType::Cube);
    wallOwner->SetName("Wall"); wallOwner->anchored = true;
    ScatteredObject* wall = wallOwner.get();
    objs.push_back(wall); engine.AddEntity(std::move(wallOwner));

    sim.StartPlay();
    rt.SetSimPlaying(true);
    for (int i = 0; i < 5; i++) sim.Update(1.0f / 60.0f);

    CHECK(audio::Init(/*noDevice=*/true));
    audio::SetListener(0, 1, 0, 1, 0, 0, 0, 1, 0);   // at the origin, facing the fan (+X), the wall in between

    vscript::Clear();
    vscript::NewScript("FanHum");
    const int start = vscript::AddNode(vscript::NodeType::Start, 0, 0).id;
    vscript::Node& play = vscript::AddNode(vscript::NodeType::PlaySound, 100, 0);
    play.target = "hum.wav";
    play.value = "1";
    vscript::SoundOptionRef(play, "object") = "Workspace/Fan";
    vscript::SoundOptionRef(play, "loop") = "true";
    vscript::SoundOptionRef(play, "range") = "40";
    vscript::SoundOptionRef(play, "muffle") = "true";
    vscript::SoundOptionRef(play, "echo") = "true";
    vscript::Connect(start, play.id);
    // What Engine installs: object sounds go to soundrays, which starts them with the world to measure.
    vscript::SetSoundPlayer([](const vscript::SoundRequest& r) { if (!r.objectPath.empty()) soundrays::Queue(r); });
    vscript::Run(rt);

    auto rms = [](uint32_t frames) {
        std::vector<float> buf(static_cast<size_t>(frames) * 2, 0.0f);
        const uint32_t got = audio::Pump(frames, buf.data());
        double sum = 0.0;
        for (uint32_t i = 0; i < got * 2; ++i) sum += (double)buf[i] * buf[i];
        return got ? static_cast<float>(std::sqrt(sum / (got * 2))) : 0.0f;
    };
    // One frame of the game: physics, the sound rays, and 1/60 s of mixed sound.
    auto frame = [&]() { sim.Update(1.0f / 60.0f); soundrays::Update(rt, 1.0f / 60.0f); return rms(800); };

    float behindWall = 0.0f;
    for (int i = 0; i < 40; ++i) behindWall = frame();
    CHECK(soundrays::ActiveEmitters() == 1);
    CHECK(audio::ActiveVoices() == 1);
    CHECK(behindWall > 0.0f);

    rt.DestroyObject(wall);                           // the wall goes: the hum opens up
    float inTheOpen = 0.0f;
    for (int i = 0; i < 60; ++i) inTheOpen = frame();
    std::printf("behind the wall: %.4f   in the open: %.4f\n", behindWall, inTheOpen);
    CHECK(inTheOpen > behindWall * 3.0f);

    // Standing next to the fan with your own body around the listener and a floor under the fan: turning round must not change how
    // loud it is, and neither the body nor the floor counts as a wall.
    {
        std::string err;
        ScatteredObject* floorSlab = rt.CreateObject("Cube");
        rt.SetObjectProperty(floorSlab, "Position", "(12, -0.5, 0)", err);
        rt.SetObjectProperty(floorSlab, "Size", "(30, 1, 30)", err);
        rt.SetObjectProperty(floorSlab, "Anchored", "true", err);
        ScatteredObject* body = rt.CreateObject("Cube");   // stands in for the player's capsule
        rt.SetObjectProperty(body, "Position", "(10.9, 1, 0)", err);
        rt.SetObjectProperty(body, "Size", "(0.8, 1.8, 0.8)", err);
        rt.SetObjectProperty(body, "Anchored", "true", err);
        for (int i = 0; i < 5; ++i) sim.Update(1.0f / 60.0f);

        auto loud = [&](float lx, float fx, bool rays) {
            audio::StopAll(); soundrays::Clear();
            audio::SetListener(lx, 1.4f, 0, fx, 0, 0, 0, 1, 0);
            vscript::Node* n = vscript::Find(play.id);
            vscript::SoundOptionRef(*n, "muffle") = rays ? "true" : "false";
            vscript::SoundOptionRef(*n, "echo") = "false";
            vscript::Run(rt);
            float r = 0.0f;
            for (int i = 0; i < 40; ++i) r = frame();
            return r;
        };
        const float facing = loud(10.9f, 1.0f, true), away = loud(10.9f, -1.0f, true);
        const float flat = loud(10.9f, 1.0f, false);
        std::printf("1.1 m from the fan, body around the listener, floor under the fan: facing %.4f, away %.4f, rays off %.4f\n", facing, away, flat);
        CHECK(facing > flat * 0.9f && away > flat * 0.9f);   // no muffling at all up close
        // A few metres off, the body and the floor still do not muffle: only the wall would (there is none now).
        const float farFacing = loud(7.0f, 1.0f, true), farAway = loud(7.0f, -1.0f, true), farFlat = loud(7.0f, 1.0f, false);
        std::printf("5 m from the fan: facing %.4f, away %.4f, rays off %.4f\n", farFacing, farAway, farFlat);
        CHECK(farFacing > farFlat * 0.8f && farAway > farFlat * 0.8f);
        rt.DestroyObject(floorSlab);
        rt.DestroyObject(body);
        audio::StopAll(); soundrays::Clear();
        audio::SetListener(0, 1, 0, 1, 0, 0, 0, 1, 0);
        vscript::SoundOptionRef(*vscript::Find(play.id), "muffle") = "true";
        vscript::SoundOptionRef(*vscript::Find(play.id), "echo") = "true";
        vscript::Run(rt);
        for (int i = 0; i < 10; ++i) frame();
    }

    rt.DestroyObject(fan);                            // its object is gone: the sound stops
    frame();
    CHECK(soundrays::ActiveEmitters() == 0 && audio::ActiveVoices() == 0);

    soundrays::Clear();
    audio::Shutdown();
    std::error_code ec; fs::remove_all(proj, ec);
    std::printf(g_fail ? "sound_rays_engine_test: %d FAILED\n" : "sound_rays_engine_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
