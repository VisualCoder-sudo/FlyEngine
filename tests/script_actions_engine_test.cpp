// The scripting API (FlyNative_*, what fly.hpp calls) against the real engine: create, clone, rename, destroy, forces,
// raycast, keys, text. Needs a display (the engine opens a window); not registered with ctest:
// build/tests/script_actions_engine_test
#include "test_unique.hpp"
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
#include <string>
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

    // Unions: merge parts during play, with physics, colours and separate; stale and unsupported inputs are refused.
    {
        const unsigned long long p1 = FlyNative_CreateObject("Cube");
        const unsigned long long p2 = FlyNative_CreateObject("Cube");
        const unsigned long long cut = FlyNative_CreateObject("Sphere");
        CHECK(p1 && p2 && cut);
        FlyNative_SetName(p1, "PartA");
        FlyNative_SetName(p2, "PartB");
        FlyNative_SetPosition(p1, 30, 3, 0);
        FlyNative_SetPosition(p2, 31, 3, 0);
        FlyNative_SetPosition(cut, 30.5f, 3, 0);
        FlyNative_SetSize(cut, 0.8f, 8.0f, 0.8f);
        CHECK(FlyNative_SetNegate(cut, 1) == 1 && FlyNative_GetNegate(cut) == 1);
        CHECK(FlyNative_GetNegate(p1) == 0);
        CHECK(FlyNative_UnionPartCount(p1) == 0 && FlyNative_SetUseGlobalColor(p1, 1) == 0);   // not a union

        const size_t beforeUnion = objs.size();
        const unsigned long long bad[2] = { p1, 0 };
        CHECK(FlyNative_UnionObjects(bad, 2, 0) == 0 && objs.size() == beforeUnion);   // a bad handle: nothing happens
        const unsigned long long one[1] = { p1 };
        CHECK(FlyNative_UnionObjects(one, 1, 0) == 0);                                  // a union needs two parts
        const unsigned long long twice[2] = { p1, p1 };
        CHECK(FlyNative_UnionObjects(twice, 2, 0) == 0 && objs.size() == beforeUnion);
        CHECK(FlyNative_UnionObjects(nullptr, 3, 0) == 0);

        const unsigned long long parts[3] = { p1, p2, cut };
        const unsigned long long u = FlyNative_UnionObjects(parts, 3, 0);
        CHECK(u != 0);
        CHECK(objs.size() == beforeUnion - 3 + 1);                                     // three parts gone, one union in
        CHECK(FlyNative_FindObject("PartA") == 0 && FlyNative_GetName(p1)[0] == '\0');   // the sources' handles are stale
        CHECK(FlyNative_UnionPartCount(u) == 3);
        CHECK(FlyNative_UnionObjects(parts, 3, 0) == 0);                                // stale handles are refused
        CHECK(FlyNative_SetUseGlobalColor(u, 1) == 1 && FlyNative_GetUseGlobalColor(u) == 1);
        CHECK(FlyNative_SetUseGlobalColor(u, 0) == 1 && FlyNative_GetUseGlobalColor(u) == 0);
        const unsigned long long unionAndPart[2] = { u, FlyNative_CreateObject("Cube") };
        CHECK(FlyNative_UnionObjects(unionAndPart, 2, 0) == 0);                          // a union cannot be a part
        FlyNative_DestroyObject(unionAndPart[1]);

        // It is a solid body now: a ray down through the carved hole misses it, one beside the hole hits it.
        for (int i = 0; i < 3; i++) sim.Update(1.0f / 60.0f);
        FlyNative_SetPosition(u, 30.5f, 3, 0);
        FlyNative_SetAnchored(u, 1);
        FlyNative_SetCollisionAccuracy(u, 3);   // Precise: a hull collider would fill the hole
        for (int i = 0; i < 3; i++) sim.Update(1.0f / 60.0f);
        const std::string onSolid = FlyNative_Raycast(30.5f - 0.9f, 20, 0, 0, -1, 0, 100.0f);   // copied: the next call reuses the buffer
        const std::string inHole = FlyNative_Raycast(30.5f, 20, 0, 0, -1, 0, 100.0f);
        std::printf("union ray: beside the hole '%s', through it '%s'\n", onSolid.c_str(), inHole.c_str());
        CHECK(onSolid == "Union");
        CHECK(inHole != "Union");

        // Separate: the parts come back (with their names), the union is gone.
        unsigned long long back[8] = {};
        const int n = FlyNative_SeparateUnion(u, back, 8);
        CHECK(n == 3 && back[0] && back[1] && back[2]);
        CHECK(FlyNative_UnionPartCount(u) == 0 && FlyNative_GetName(u)[0] == '\0');   // the union's handle is stale
        CHECK(FlyNative_FindObject("PartA") == back[0] && FlyNative_FindObject("PartB") == back[1]);
        CHECK(FlyNative_GetNegate(back[2]) == 1);                                       // the cutter comes back as a cutter
        CHECK(FlyNative_SeparateUnion(u, back, 8) == 0);                                 // twice: harmless

        // Intersect keeps only what two cubes share.
        const unsigned long long i1 = back[0], i2 = back[1];
        FlyNative_SetPosition(i1, 40, 3, 0);
        FlyNative_SetPosition(i2, 41, 3, 0);
        const unsigned long long both[2] = { i1, i2 };
        const unsigned long long inter = FlyNative_UnionObjects(both, 2, 1);
        CHECK(inter != 0);
        float sx = 0, sy = 0, sz = 0;
        FlyNative_GetSize(inter, &sx, &sy, &sz);
        std::printf("intersect size %.2f %.2f %.2f\n", sx, sy, sz);
        CHECK(sx > 0.45f && sx < 0.55f && sy > 1.4f && sy < 1.6f);   // two 1.5 m cubes one apart share a 0.5 x 1.5 x 1.5 slab
        FlyNative_DestroyObject(inter);
        FlyNative_DestroyObject(back[2]);
    }

    // Visual Script: Union and Separate nodes, and the Negate / UseGlobalColor properties, through the real runtime.
    {
        const unsigned long long va = FlyNative_CreateObject("Cube"), vb = FlyNative_CreateObject("Cube"), vc = FlyNative_CreateObject("Sphere");
        FlyNative_SetName(va, "VsA"); FlyNative_SetName(vb, "VsB"); FlyNative_SetName(vc, "VsCut");
        FlyNative_SetPosition(va, 60, 3, 0); FlyNative_SetPosition(vb, 61, 3, 0); FlyNative_SetPosition(vc, 60.5f, 3, 0);
        FlyNative_SetSize(vc, 0.8f, 8.0f, 0.8f);

        vscript::Clear();
        vscript::NewScript("UnionNodes");
        const int start = vscript::AddNode(vscript::NodeType::Start, 0, 0).id;
        const int cutter = vscript::AddNode(vscript::NodeType::Change, 100, 0).id;
        const int merge = vscript::AddNode(vscript::NodeType::Union, 200, 0).id;
        const int paint = vscript::AddNode(vscript::NodeType::Change, 300, 0).id;
        vscript::Find(cutter)->target = "Workspace/VsCut"; vscript::Find(cutter)->prop = "Negate"; vscript::Find(cutter)->value = "true";
        vscript::Node* u = vscript::Find(merge);
        u->target = "Workspace/VsA"; u->prop = "Union"; u->value = "VsArch";
        u->extra.emplace_back("Workspace/VsB", ""); u->extra.emplace_back("Workspace/VsCut", "");
        vscript::Find(paint)->target = "Workspace/VsArch"; vscript::Find(paint)->prop = "UseGlobalColor"; vscript::Find(paint)->value = "true";
        vscript::Connect(start, cutter); vscript::Connect(cutter, merge); vscript::Connect(merge, paint);
        const size_t beforeVs = objs.size();
        vscript::Run(rt);
        const unsigned long long arch = FlyNative_FindObject("VsArch");
        CHECK(arch != 0 && FlyNative_UnionPartCount(arch) == 3);
        CHECK(FlyNative_GetUseGlobalColor(arch) == 1);                              // the Change node after the Union reached the new object
        CHECK(objs.size() == beforeVs - 3 + 1);
        CHECK(FlyNative_FindObject("VsA") == 0 && FlyNative_FindObject("VsCut") == 0);   // the parts are gone

        vscript::Clear();
        vscript::NewScript("SeparateNode");
        const int s2 = vscript::AddNode(vscript::NodeType::Start, 0, 0).id;
        const int sep = vscript::AddNode(vscript::NodeType::Separate, 100, 0).id;
        vscript::Find(sep)->target = "Workspace/VsArch";
        vscript::Connect(s2, sep);
        vscript::Run(rt);
        CHECK(FlyNative_FindObject("VsArch") == 0);
        const unsigned long long ra = FlyNative_FindObject("VsA"), rb = FlyNative_FindObject("VsB"), rc = FlyNative_FindObject("VsCut");
        CHECK(ra && rb && rc && FlyNative_GetNegate(rc) == 1);                       // back, the cutter still a cutter
        CHECK(objs.size() == beforeVs);
        FlyNative_DestroyObject(ra); FlyNative_DestroyObject(rb); FlyNative_DestroyObject(rc);
        vscript::Clear();
    }

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
        const fs::path proj = fs::temp_directory_path() / TestUnique("fly_play_sound_proj");
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
