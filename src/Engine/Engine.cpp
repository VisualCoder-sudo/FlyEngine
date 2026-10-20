#include "../../include/Engine.hpp"
#include "../../include/Graphics/Graphics.hpp"
#include "../../include/Graphics/PostFX.hpp"
#include "../../include/Graphics/Occlusion.hpp"
#include "../../include/Engine/Frontend/ui.hpp"
#include "../../include/Engine/Scripts/CommandConsole.hpp"
#include "../../include/Engine/Backend/VisualScript.hpp"
#include "../../include/Engine/GameUI.hpp"
#include "../../include/Engine/Audio.hpp"
#include "../../include/Graphics/Particles.hpp"
#include "../../include/Terrain/Foliage.hpp"
#include "../../include/Engine/Backend/SoundRays.hpp"
#include "../../include/Engine/Backend/ScatteredObject.hpp"
#include "../include/Terrain/Terrain.hpp"
#include "../../include/Terrain/BasicTerrain.hpp"
#include "../../include/Terrain/Water/WaterBody.hpp"
#include <algorithm>
#include <cmath>

Engine::Engine(int width, int height, const std::string& title, int targetFPS) {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(width, height, title.c_str());
    SetWindowMinSize(1024, 640);
    SetExitKey(KEY_NULL);
    SetTargetFPS(targetFPS);

    gfx::Init();

    camera.position = { 10.0f, 10.0f, 10.0f };
    camera.target = { 0.0f, 0.0f, 0.0f };
    camera.up = { 0.0f, 1.0f, 0.0f };
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    console::AttachCamera(camera);

    audio::SetLogHandler([](const std::string& msg) { ui::LogAlways("%s", msg.c_str()); });

    // Visual Script's Play Sound node and fly::PlaySound() end here. The sound device opens on the first sound.
    vscript::SetSoundPlayer([](const vscript::SoundRequest& r) {
        if (!r.objectPath.empty()) { soundrays::Queue(r); return; }   // from an object: started by soundrays::Update, which has the world
        audio::PlayParams p;
        p.volume = r.volume;
        p.loop = r.loop;
        audio::Play(r.path, p);
    });
    vscript::SetSoundStopper([](const std::string& path, const std::string& objectPath) { soundrays::StopSounds(path, objectPath); });
}

Engine::~Engine() {
    // Destroy in reverse creation order: entities added later (character
    // controller, interaction manager) hold references to ones added earlier
    // (the physics simulation). vector::clear() destroys front to back, which
    // crashed the Player on exit.
    while (!pendingEntities.empty()) pendingEntities.pop_back();
    while (!entities.empty()) entities.pop_back();
    audio::Shutdown();
    soundrays::Clear();
    vscript::SetSoundPlayer(nullptr);
    vscript::SetSoundStopper(nullptr);
    foliage::Shutdown();
    gfx::Shutdown();
    CloseWindow();
}

Entity* Engine::AddEntity(std::unique_ptr<Entity> entity) {
    Entity* rawEntity = entity.get();
    if (isUpdating) {
        pendingEntities.push_back(std::move(entity));
        return rawEntity;
    }
    entities.push_back(std::move(entity));
    return rawEntity;
}

void Engine::SetTimeScale(float s) {
    // Clamped rather than rejected: time scale is reachable from the console,
    // where a typo like "timescale 0" would otherwise silently stop the world.
    // Zero is still allowed (that is a legitimate freeze), the floor just stops
    // a negative value from running time backwards into physics.
    timeScale = std::clamp(s, 0.0f, 10.0f);
}

void Engine::Update(float deltaTime) {
    console::Update();
    {
        // The day/night clock only runs while playing (editor Play or the player); the editor keeps the hour
        // it was set to, and gets it back when Play stops.
        static bool wasPlaying = false;
        static float timeBeforePlay = 12.0f;
        const bool playing = isPlayerBuild || ui::IsPlayActive();
        if (playing && !wasPlaying) timeBeforePlay = gfx::Lighting().timeOfDay;
        if (!playing && wasPlaying) gfx::Lighting().timeOfDay = timeBeforePlay;
        if (!playing && wasPlaying) { audio::StopAll(); soundrays::Clear(); fx::Clear(); }   // Play ended: cut the sounds off
        wasPlaying = playing;
        if (playing) gfx::TickLighting(deltaTime);
    }
    {
        // Sound follows the master volume (SetMasterVolume / fly::SetVolume) and hears from the camera.
        audio::SetMasterVolume(GetMasterVolume());
        const Vector3 f = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
        audio::SetListener(camera.position.x, camera.position.y, camera.position.z, f.x, f.y, f.z, camera.up.x, camera.up.y, camera.up.z);
        audio::Update();
    }
    if (!isPlayerBuild) ui::UpdateInput();

    // Decide whether entities tick this frame. A paused engine skips the entity
    // loop outright rather than calling Update(0): update code routinely
    // branches on dt and sometimes divides by it, so a zero dt is not a safe
    // way to say "no time passed".
    bool tickEntities = true;
    if (paused) {
        if (pendingSteps > 0) {
            --pendingSteps;       // a queued single-frame step consumes one
        } else {
            tickEntities = false;
        }
    }

    if (tickEntities) fx::Update(deltaTime * timeScale);

    if (tickEntities) {
        isUpdating = true;
        for (auto& entity : entities) {
            if (isPlayerBuild && entity->IsEditorOnly()) continue;
            entity->Update(deltaTime * timeScale);
        }
        isUpdating = false;
    }

    // Housekeeping runs even on a frozen frame, so entities that marked
    // themselves dead are still reaped and queued additions still land. A
    // paused engine that refused new entities would be a nasty surprise.

    // Remove anything that marked itself dead.
    entities.erase(
        std::remove_if(entities.begin(), entities.end(),
            [](const std::unique_ptr<Entity>& e) { return !e->alive; }),
        entities.end());

    for (auto& entity : pendingEntities) entities.push_back(std::move(entity));
    pendingEntities.clear();
}

namespace {
// Collects the enabled light nodes of the visible objects, nearest to the camera first, up to the scene's cap,
// and hands them to the lit shaders.
void GatherNodeLights(const std::vector<std::unique_ptr<Entity>>& entities, const Camera3D& camera) {
    struct Cand { float dist; gfx::NodeLight light; };
    std::vector<Cand> found;
    for (const auto& entity : entities) {
        auto* obj = dynamic_cast<ScatteredObject*>(entity.get());
        if (!obj || obj->nodes.empty() || obj->IsHiddenEffective()) continue;
        for (const ObjectNode& n : obj->nodes) {
            if (!n.enabled || !n.IsLight()) continue;
            gfx::NodeLight l;
            l.range = n.range;
            l.castShadows = n.castShadows;
            l.shadowRes = n.shadowRes;
            l.host = obj;
            l.color = { n.color[0] * n.brightness, n.color[1] * n.brightness, n.color[2] * n.brightness };
            l.falloff = n.falloff == LightFalloff::Linear ? 0 : 1;
            Vector3 hu, hv;
            obj->GetNodeEmitter(n, l.pos, l.dir, hu, hv);
            l.kind = n.type == ObjectNodeType::PointLight ? 0 : n.type == ObjectNodeType::SpotLight ? 1 : 2;
            if (l.kind != 0) {
                const float outer = DEG2RAD * std::clamp(n.angle, 1.0f, 89.0f);
                l.cosOuter = std::cos(outer);
                l.cosInner = std::cos(outer * 0.7f);
                l.u = hu;
                l.v = hv;
            }
            found.push_back({ Vector3Distance(l.pos, camera.position), l });
        }
    }
    const int cap = std::clamp(gfx::Lighting().maxLights, 1, gfx::kMaxNodeLights);
    if ((int)found.size() > cap) {
        std::partial_sort(found.begin(), found.begin() + cap, found.end(), [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
        found.resize((size_t)cap);
    }
    gfx::NodeLight lights[gfx::kMaxNodeLights];
    for (size_t i = 0; i < found.size(); ++i) lights[i] = found[i].light;
    gfx::SetNodeLights(lights, (int)found.size());
}
} // namespace

void Engine::Draw() {
    // Update LOD camera position so objects can compute distance-based textures
    SetLODCameraPos(camera.position);

    BeginDrawing();
    gfx::SetEngineClearColor(clearColor);
    const Color skyColor = gfx::CurrentSky();
    ClearBackground(skyColor);

    double p0 = GetTime();
    GatherNodeLights(entities, camera);   // before the shadow passes: they need to know which lights cast
    // 0. Shadow map pass: render occluders from the light's point of view.
    // (Once per shadow cascade; the further cascades only ask for it every few frames.)
    for (int cascade = 0; cascade < gfx::ShadowCascadeCount(); cascade++) {
        if (!gfx::BeginShadowCascade(cascade)) continue;
        gfx::BeginShapeBatch();
        for (auto& entity : entities) {
            if (isPlayerBuild && entity->IsEditorOnly()) continue;
            entity->Draw();
        }
        gfx::EndShapeBatch();
        foliage::Draw(camera, true);
        gfx::EndShadowCascade();
    }
    double p1 = GetTime();

    // Shadows of the light nodes: the scene drawn once into each tile of their atlas.
    for (int tile = 0; tile < gfx::NodeShadowTileCount(); tile++) {
        if (!gfx::BeginNodeShadowTile(tile)) continue;
        gfx::BeginShapeBatch();
        for (auto& entity : entities) {
            if (isPlayerBuild && entity->IsEditorOnly()) continue;
            entity->Draw();
        }
        gfx::EndShapeBatch();
        foliage::Draw(camera, true);   // trees and rocks shadow the light too
        gfx::EndNodeShadowTile();
    }
    gfx::UpdateLighting(camera);

    // 0.5 Planar reflection pass - render the opaque world from a camera mirrored
    // below the first play-active water body's surface. Water shaders sample this
    // image during the transparent pass, so mountains and objects show up in the water.
    if (gfx::IsReflectionsEnabled() && ui::IsPlayActive()) {
        const auto& waterBodies = WaterBody::GetInstances();
        WaterBody* reflBody = waterBodies.empty() ? nullptr : waterBodies.front();
        if (reflBody) {
            Camera3D reflCam = gfx::BeginReflectionPass(reflBody->GetWaterHeight(), camera, skyColor);
            terrain::Terrain::SetDrawCamera(&reflCam);
            BasicTerrain::SetActiveCamera(&reflCam);
            BeginMode3D(reflCam);
                gfx::ClipReflectionPassToWater();
                // Draw opaque entities first (they write depth). Anything fully
                // underwater has no reflection (the mirrored camera would wrongly
                // lift it above the surface), so skip it.
                gfx::BeginShapeBatch();
                for (auto& entity : entities) {
                    if (isPlayerBuild && entity->IsEditorOnly()) continue;
                    if (entity->IsTransparent()) continue;
                    auto* obj = dynamic_cast<ScatteredObject*>(entity.get());
                    if (obj) {
                        Vector3 op = *obj->GetPosPtr();
                        Vector3 os = *obj->GetSizePtr();
                        if (op.y + os.y * 0.5f < reflBody->GetWaterHeight()) continue;
                    }
                    entity->Draw();
                }
                gfx::EndShapeBatch();
            // Must be called while still inside BeginMode3D(reflCam): it reads the
            // mirrored camera's matrices, then ends both the 3D and texture modes.
            gfx::EndReflectionPass();
            terrain::Terrain::SetDrawCamera(&camera);
            BasicTerrain::SetActiveCamera(&camera);
        }
    }
    double p2 = GetTime();

    // 1. 3D Pass
    terrain::Terrain::SetDrawCamera(&camera);
    BasicTerrain::SetActiveCamera(&camera);
    WaterBody::SetActiveCamera(&camera);
    WaterBody::SetActiveEngine(this);
    
    // Reset debug counters at start of frame
    gfx::ResetFrameStats();
    
    // The scene is lit into a float target; gfx::EndScene() turns it into the picture
    // (fog, clouds, bloom, exposure, tone curve) and writes that to the screen target.
    gfx::BeginScene(camera);
    double pSky = GetTime();
    double pOpaque = pSky;
    BeginMode3D(camera);
        // Big opaque boxes hide what is behind them: rasterize them once so the objects drawn below can be
        // skipped when they are completely covered.
        if (gfx::GetFrustumCullingEnabled()) {
            static std::vector<occlusion::OccluderBox> occluders;
            occluders.clear();
            for (auto& entity : entities) {
                if (isPlayerBuild && entity->IsEditorOnly()) continue;
                auto* obj = dynamic_cast<ScatteredObject*>(entity.get());
                occlusion::OccluderBox box;
                if (obj && obj->GetOccluderCorners(box.corners, box.volume)) occluders.push_back(box);
            }
            occlusion::BeginFrame(occluders);
        }
        // Draw opaque entities first (they write depth)
        gfx::BeginShapeBatch();
        for (auto& entity : entities) {
            if (isPlayerBuild && entity->IsEditorOnly()) continue;
            if (!entity->IsTransparent()) {
                entity->Draw(); // Entity handles its own frustum culling and draw call counting
            }
        }
        gfx::EndShapeBatch();
        gfx::DrawNodeSurfaces();   // Decal and Texture nodes, on top of the surfaces they cover
        foliage::Draw(camera, false);
        occlusion::EndFrame();
        pOpaque = GetTime();
        // Draw transparent entities last (water renders on top, reads depth)
        for (auto& entity : entities) {
            if (isPlayerBuild && entity->IsEditorOnly()) continue;
            if (entity->IsTransparent()) {
                entity->Draw(); // Entity handles its own frustum culling and draw call counting
            }
        }
        gfx::DrawWeather(camera);   // falling rain (nothing unless the Weather item sets rain)
        fx::Draw(camera);           // particles and decals
        // Editor overlays (transform gizmos) render after every entity so they
        // stay readable even when buried inside another object.
        for (auto& entity : entities) {
            if (isPlayerBuild && entity->IsEditorOnly()) continue;
            entity->DrawOverlay3D();
        }
    EndMode3D();
    double p3 = GetTime();

    // Custom 3D overlay callback (debug stats, etc.). Still inside the scene target, so it is
    // depth-tested against the scene.
    if (onDrawOverlay3D) {
        onDrawOverlay3D();
    }
    // Camera inside a water body: tint the picture with its colour, murkier and darker the
    // deeper it is, but only within the body's underwater range below the surface.
    {
        float bestMurk = 0.0f;
        for (WaterBody* water : WaterBody::GetInstances()) {
            if (!water) continue;
            const WaterBody::UnderwaterParams& uw = water->GetUnderwaterParams();
            if (uw.range <= 0.0f || uw.murk <= 0.0f) continue;
            const BoundingBox cb = { { camera.position.x - 0.01f, camera.position.y - 0.01f, camera.position.z - 0.01f },
                                     { camera.position.x + 0.01f, camera.position.y + 0.01f, camera.position.z + 0.01f } };
            if (!water->IntersectsXZ(cb)) continue;
            const float depth = water->GetHeightAt(camera.position.x, camera.position.z) - camera.position.y;
            if (depth <= 0.0f || depth >= uw.range) continue;
            const float frac = depth / uw.range;
            const float edge = std::min(1.0f, (1.0f - frac) * 8.0f);   // eases out over the last eighth of the range
            const float fade = std::min(1.0f, depth * 4.0f);           // and in over the first quarter metre
            const float murk = uw.murk * (0.5f + frac) * edge * fade;
            if (murk <= bestMurk) continue;
            bestMurk = murk;
            const Vector3 sun = gfx::SunRadiance();
            const float day = std::min(1.0f, std::max(sun.x, std::max(sun.y, sun.z)));
            const float light = (0.12f + 0.45f * day) * (1.0f - std::clamp(uw.darken, 0.0f, 1.0f) * frac);
            const Vector3 col = gfx::SrgbToLinear(water->GetBaseColor());
            gfx::SetUnderwaterView({ col.x * light, col.y * light, col.z * light }, murk);
        }
    }
    gfx::EndScene();
    double pPost = GetTime();

    // 2. 2D Pass
    if (!isPlayerBuild) ui::Draw();

    // Slow background loads (a big city building) show "<name> attempting to load [n%]".
    {
        Rectangle area = { 0.0f, 0.0f, (float)GetScreenWidth(), (float)GetScreenHeight() };
        if (!isPlayerBuild) { const Rectangle mid = ui::GetConsoleBarArea(); area = { mid.x, 126.0f, mid.width, (float)GetScreenHeight() - 126.0f }; }
        gfx::DrawLoadingStatus(area);
        vscript::DrawHud(area.x, area.y, area.width, area.height);   // Show Text messages
        gameui::Draw(area.x, area.y, area.width, area.height);       // fly::UI widgets
    }

    // ImGui pass (draws after every raylib overlay so it stays on top)
    if (!isPlayerBuild) ui::DrawImGuiFrame(camera);

    // Custom 2D overlay callback (debug stats, etc.)
    if (onDrawOverlay2D) {
        onDrawOverlay2D();
    }

    EndDrawing();
    double p4 = GetTime();

    // The GetTime() calls above run unconditionally, so publish the results
    // unconditionally too -- the F2 overlay shows them, and gating them behind
    // timingEnabled meant the editor showed nothing while the stress harness
    // saw the real numbers.
    gfx::FrameTimings t;
    t.shadowMs      = (p1 - p0) * 1000.0;
    t.reflectionMs  = (p2 - p1) * 1000.0;
    t.skyMs         = (pSky - p2) * 1000.0;
    t.opaqueMs      = (pOpaque - pSky) * 1000.0;
    t.transparentMs = (p3 - pOpaque) * 1000.0;
    t.postMs        = (pPost - p3) * 1000.0;
    t.twoDMs        = (p4 - pPost) * 1000.0;
    gfx::SetFrameTimings(t);

    // Kept as public members for the stress harness, which still gates on
    // timingEnabled so its own numbers are only read when it asked for them.
    if (timingEnabled) {
        lastShadowMs = t.shadowMs;
        lastReflectionMs = t.reflectionMs;
        lastOpaqueMs = t.opaqueMs;
        lastTransparentMs = t.transparentMs;
        last2DMs = t.twoDMs;
    }
}
void Engine::Run() {
    while (!WindowShouldClose()) {
        // Clamp the frame delta so a hitch (window drag, modal hang, debugger
        // break, tab switch) can't push physics/simulation into a multi-second
        // jump that destabilizes the fixed-step integrator.
        float dt = fminf(GetFrameTime(), MAX_FRAME_DT);
        Update(dt);
        Draw();
    }
}

void Engine::UpdateFrame(float deltaTime) {
    Update(deltaTime);
}

void Engine::RenderFrame() {
    Draw();
}

void Engine::StepFrame(float deltaTime) {
    Update(deltaTime);
    Draw();
}

void Engine::RemoveEntity(Entity* entity) {
    if (!entity) return;
    entity->alive = false;

    if (!isUpdating) {
        entities.erase(
            std::remove_if(entities.begin(), entities.end(),
                [entity](const std::unique_ptr<Entity>& e) { return e.get() == entity; }),
            entities.end());
    }
}
