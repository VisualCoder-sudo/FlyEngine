// Water: FBM with 0 octaves is finite, parameter clamping on the setters, and every water field survives a scene
// save/load. Opens a window (the WaterBody loads its shader), so ctest needs a display (label needs_display).
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/Backend/ScenePersistence.hpp"
#include "Terrain/Water/WaterBody.hpp"
#include "Terrain/Water/WaterNoise.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <sstream>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
    Engine engine(320, 200, "water_test", 60);

    // Noise: no octaves must not give NaN (0/0).
    CHECK(std::isfinite(WaterNoise::FBM3D(1.f, 2.f, 3.f, 0, 0.5f, 2.f, 1)));
    CHECK(std::isfinite(WaterNoise::FBM3D(1.f, 2.f, 3.f, -4, 0.5f, 2.f, 1)));
    CHECK(std::isfinite(WaterNoise::FBM2D(1.f, 2.f, 0, 0.5f, 2.f, 1)));

    auto owned = std::make_unique<WaterBody>(Vector3{ 10, 0, -20 }, Vector3{ 200, 1, 300 }, 2.5f, Color{ 10, 20, 30, 40 });
    WaterBody* w = owned.get();
    engine.AddEntity(std::move(owned));

    // Setters clamp.
    WaterBody::NoiseParams bad;
    bad.octaves = 0; bad.amplitude = std::numeric_limits<float>::quiet_NaN(); bad.lacunarity = INFINITY;
    w->SetNoiseParams(bad);
    CHECK(w->GetNoiseParams().octaves == 1 && std::isfinite(w->GetNoiseParams().amplitude) && std::isfinite(w->GetNoiseParams().lacunarity));
    bad.octaves = 1000000;
    w->SetNoiseParams(bad);
    CHECK(w->GetNoiseParams().octaves <= 12);
    CHECK(std::isfinite(w->GetHeightAt(10.f, -20.f)));
    WaterBody::UnderwaterParams u; u.range = -3; u.darken = 9; u.murk = -1;
    w->SetUnderwaterParams(u);
    CHECK(w->GetUnderwaterParams().range == 0.0f && w->GetUnderwaterParams().darken == 1.0f && w->GetUnderwaterParams().murk == 0.0f);
    WaterBody::RippleParams rp; rp.wakeStrength = -1; rp.sprayAmount = 99; rp.foamLifetime = 0;
    w->SetRippleParams(rp);
    CHECK(w->GetRippleParams().wakeStrength == 0.0f && w->GetRippleParams().sprayAmount == 5.0f && w->GetRippleParams().foamLifetime >= 0.2f);
    CHECK(w->GetRippleHeightAt(std::numeric_limits<float>::quiet_NaN(), 0.f) == 0.0f);

    // Round trip.
    WaterBody::NoiseParams n; n.amplitude = 0.8f; n.frequency = 0.07f; n.speed = 0.6f; n.direction = { 0.5f, -1.f };
    n.octaves = 6; n.persistence = 0.4f; n.lacunarity = 2.3f; n.seed = 99;
    w->SetNoiseParams(n);
    WaterBody::FoamParams f; f.intensity = 0.9f; f.scale = 5.f; f.threshold = 0.6f; f.color = Color{ 200, 210, 220, 255 };
    w->SetFoamParams(f);
    WaterBody::DetailParams d; d.intensity = 0.11f; d.scale = 9.f; d.speed = 1.5f;
    w->SetDetailParams(d);
    WaterBody::ReflectionParams r; r.strength = 0.33f; r.distortion = 1.4f; r.distanceFade = 0.7f;
    w->SetReflectionParams(r);
    WaterBody::UnderwaterParams uw; uw.range = 7.f; uw.murk = 0.2f; uw.darken = 0.4f;
    w->SetUnderwaterParams(uw);
    WaterBody::RippleParams rr; rr.wakeStrength = 1.5f; rr.splashStrength = 0.2f; rr.foamLifetime = 8.f; rr.spray = false; rr.sprayAmount = 2.f; rr.maxDisplacement = 0.9f;
    w->SetRippleParams(rr);
    w->SetPlayerCollision(true);
    w->SetTransparency(0.45f);
    w->SetName("Lake 1");

    std::vector<ScatteredObject*> objs; std::vector<std::unique_ptr<ModelGroup>> models;
    std::stringstream ss;
    CHECK(SaveSceneToStream(ss, objs, models, "", nullptr));
    // Drop the original so the registry only holds the loaded copy.
    engine.GetEntities().clear();
    CHECK(WaterBody::GetInstances().empty());
    std::vector<ScatteredObject*> objs2; std::vector<std::unique_ptr<ModelGroup>> models2;
    CHECK(LoadSceneFromStream(ss, engine, objs2, models2, "", nullptr, nullptr));
    CHECK(WaterBody::GetInstances().size() == 1);
    if (!WaterBody::GetInstances().empty()) {
        WaterBody* l = WaterBody::GetInstances()[0];
        CHECK(l->GetName() == "Lake 1");
        CHECK(Near(l->GetPosPtr()->x, 10) && Near(l->GetPosPtr()->z, -20) && Near(l->GetSizePtr()->x, 200) && Near(l->GetSizePtr()->z, 300));
        CHECK(Near(l->GetWaterHeight(), 2.5f) && Near(l->GetTransparency(), 0.45f) && l->GetPlayerCollision());
        const Color c = l->GetBaseColor();
        CHECK(c.r == 10 && c.g == 20 && c.b == 30 && c.a == 40);
        const auto& ln = l->GetNoiseParams();
        CHECK(Near(ln.amplitude, 0.8f) && Near(ln.frequency, 0.07f) && Near(ln.speed, 0.6f) && Near(ln.direction.x, 0.5f) && Near(ln.direction.y, -1.f));
        CHECK(ln.octaves == 6 && Near(ln.persistence, 0.4f) && Near(ln.lacunarity, 2.3f) && ln.seed == 99);
        const auto& lf = l->GetFoamParams();
        CHECK(Near(lf.intensity, 0.9f) && Near(lf.scale, 5.f) && Near(lf.threshold, 0.6f) && lf.color.r == 200 && lf.color.g == 210 && lf.color.b == 220);
        const auto& ld = l->GetDetailParams();
        CHECK(Near(ld.intensity, 0.11f) && Near(ld.scale, 9.f) && Near(ld.speed, 1.5f));
        const auto& lr = l->GetReflectionParams();
        CHECK(Near(lr.strength, 0.33f) && Near(lr.distortion, 1.4f) && Near(lr.distanceFade, 0.7f));
        const auto& lu = l->GetUnderwaterParams();
        CHECK(Near(lu.range, 7.f) && Near(lu.murk, 0.2f) && Near(lu.darken, 0.4f));
        const auto& lp = l->GetRippleParams();
        CHECK(Near(lp.wakeStrength, 1.5f) && Near(lp.splashStrength, 0.2f) && Near(lp.foamLifetime, 8.f) && !lp.spray && Near(lp.sprayAmount, 2.f) && Near(lp.maxDisplacement, 0.9f));
    }

    std::printf(g_fail ? "water_test: %d FAILED\n" : "water_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
