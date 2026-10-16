// Sound rays against the real engine: a Play Sound node on an object, rays through the real physics world, and the
// mixed output measured with a device-less audio engine. A wall between the fan and the listener muffles it; remove the
// wall and the sound opens up; destroy the fan and its sound stops. Needs a display (the engine opens a window); not
// registered with ctest: build/tests/sound_rays_engine_test
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
    const fs::path proj = fs::temp_directory_path() / "fly_sound_rays_proj";
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

    rt.DestroyObject(fan);                            // its object is gone: the sound stops
    frame();
    CHECK(soundrays::ActiveEmitters() == 0 && audio::ActiveVoices() == 0);

    soundrays::Clear();
    audio::Shutdown();
    std::error_code ec; fs::remove_all(proj, ec);
    std::printf(g_fail ? "sound_rays_engine_test: %d FAILED\n" : "sound_rays_engine_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
