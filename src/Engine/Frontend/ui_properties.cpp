// ui_properties.cpp: the Properties panel (Dear ImGui).
#include "ui_internal.hpp"
#include <map>
#include <set>
#include "../../../include/Engine/ExportManager.hpp"
#include "../../../include/Engine/Backend/VisualScript.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Terrain/Water/WaterBody.hpp"
#include "../../../include/Terrain/BasicTerrain.hpp"
#include "../../../include/Engine/Scripts/CommandConsole.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "../../../include/Graphics/Graphics.hpp"
#include "../../../include/Graphics/PostFX.hpp"
#include "../../../include/Engine/LoadingScreen.hpp"
#include "../include/Engine/Scripts/ScriptLauncher.hpp"
#include "../../../include/Engine/Scripts/NativeScriptHost.hpp"
#include "../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../../../include/Engine/Backend/TextureManager.hpp"
#include "../include/Engine/Frontend/ProjectManager.hpp"
#include "../../../include/Engine/Backend/ModelImport.hpp"
#include "../include/Terrain/Terrain.hpp"
#include "../../../include/Terrain/TerrainEditor.hpp"
#include "../../../include/Terrain/TerrainRegistry.hpp"
#include "../../../include/CityGen/CityEditor.hpp"
#include "raymath.h"
#include "imgui.h"
#include "rlimgui.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <algorithm>
#include "ui_internal.hpp"

namespace ui {

// ---------------------------------------------------------------------------
// Properties panel (Dear ImGui)
// ---------------------------------------------------------------------------
float PropertyLabelWidth() { return ImGui::GetFontSize() * 7.5f; }

void PropertyRow(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Vec(theme::TEXT_MUTED));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(PropertyLabelWidth());
    ImGui::SetNextItemWidth(-FLT_MIN);
}

namespace {

// Collapsible property-group state (all open by default).
bool g_generalOpen = true;
bool g_waterInteractOpen = true;
bool g_materialOpen = true;
bool g_positionOpen = true;
bool g_sizeOpen = true;
bool g_rotationOpen = true;
bool g_originOpen = true;
bool g_linearVelocityOpen = true;
bool g_angularVelocityOpen = true;
bool g_waterNoiseOpen = true;
bool g_waterFoamOpen = true;
bool g_waterDetailOpen = true;
bool g_waterReflectOpen = true;
bool g_waterWakeOpen = true;
bool g_waterUnderOpen = true;

// One undo step per edit: the first change while a widget (or its colour picker) is held snapshots the scene as
// it was. Call it right after the widget, every frame, and before the new value is written to the scene.
void UndoOnFirstChange(bool changed) {
    static ImGuiID s_undoneItem = 0;
    const ImGuiID id = ImGui::GetItemID();
    if (ImGui::IsItemActivated() || !ImGui::IsAnyItemActive()) s_undoneItem = 0;
    if (changed && s_undoneItem != id) {
        ui::PushUndo();
        s_undoneItem = id;
    }
}

// A drag field (Ctrl+click or double-click to type). Typed text that isn't a finite number counts as 0, because
// a non-finite value written into a scene can't be read back.
bool Number(const char* label, float& value, float speed = 0.05f, float lo = -1.0e6f, float hi = 1.0e6f,
            const char* fmt = "%.2f") {
    PropertyRow(label);
    ImGui::PushID(label);
    float v = value;
    bool changed = ImGui::DragFloat("##v", &v, speed, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp) && v != value;
    UndoOnFirstChange(changed);
    if (changed) value = std::isfinite(v) ? v : 0.0f;
    ImGui::PopID();
    return changed;
}

bool Slider01(const char* label, float& value) {
    PropertyRow(label);
    ImGui::PushID(label);
    float v = value;
    bool changed = ImGui::SliderFloat("##v", &v, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp) && v != value;
    UndoOnFirstChange(changed);
    if (changed) value = v;
    ImGui::PopID();
    return changed;
}

bool Check(const char* label, bool& value) {
    PropertyRow(label);
    ImGui::PushID(label);
    bool v = value;
    const bool changed = ImGui::Checkbox("##v", &v);
    if (changed) { ui::PushUndo(); value = v; }
    ImGui::PopID();
    return changed;
}

// A colour swatch; clicking it opens ImGui's picker. The alpha channel is left as it was.
bool ColorField(const char* label, Color& c) {
    PropertyRow(label);
    ImGui::PushID(label);
    float f[3] = { c.r / 255.0f, c.g / 255.0f, c.b / 255.0f };
    const bool changed = ImGui::ColorEdit3("##v", f, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    UndoOnFirstChange(changed);
    if (changed) {
        c.r = static_cast<unsigned char>(std::lround(f[0] * 255.0f));
        c.g = static_cast<unsigned char>(std::lround(f[1] * 255.0f));
        c.b = static_cast<unsigned char>(std::lround(f[2] * 255.0f));
    }
    ImGui::PopID();
    return changed;
}

// A collapsible group whose open state lives in one of the g_*Open flags. Returns whether it is open.
bool Group(const char* label, bool& open) {
    ImGui::SetNextItemOpen(open, ImGuiCond_Always);
    open = ImGui::CollapsingHeader(label);
    return open;
}

// Applies `apply` to every selected object but the primary one, which the widget edited directly.
template <class F> void ForOthers(F&& apply) {
    for (auto* obj : g_selection) if (obj && obj != g_selectedObject) apply(*obj);
}

// An X/Y/Z group bound to the primary's vector; a changed axis is copied to the rest of the selection.
template <class Get>
bool Vector3Group(const char* title, bool& open, Vector3& v, const char* const labels[3], Get get,
                  float speed, float lo = -1.0e6f) {
    if (!Group(title, open)) return false;
    ImGui::PushID(title);
    bool any = false;
    float* axis[3] = { &v.x, &v.y, &v.z };
    for (int i = 0; i < 3; ++i) {
        if (!Number(labels[i], *axis[i], speed, lo)) continue;
        any = true;
        const float nv = *axis[i];
        ForOthers([&](ScatteredObject& o) { float* p[3] = { &get(o)->x, &get(o)->y, &get(o)->z }; *p[i] = nv; });
    }
    ImGui::PopID();
    return any;
}

// Preset textures are global: they are scanned from the working directory (the flyengine root), not the project.
std::vector<std::string> s_presets;
std::string s_presetsDir;
double s_presetsScanned = -1.0;

const std::vector<std::string>& PresetTextures(bool listOpen) {
    const std::string dir = std::filesystem::current_path().generic_string();
    const double now = GetTime();
    // Rescanned every couple of seconds while the list is open, so new files show up without a refresh button.
    if (s_presetsScanned < 0.0 || dir != s_presetsDir || (listOpen && now - s_presetsScanned > 2.0)) {
        s_presets = textureManager::GetPresetTextures(dir);
        s_presetsDir = dir;
        s_presetsScanned = now;
    }
    return s_presets;
}

void DrawWaterProperties(WaterBody* w) {
    if (Group("Position", g_positionOpen)) {
        ImGui::PushID("pos");
        Vector3 wp = *w->GetPosPtr();
        if (Number("Pos X", wp.x, 0.1f)) *w->GetPosPtr() = wp;
        float wh = w->GetWaterHeight();
        if (Number("Water height", wh, 0.1f)) w->SetWaterHeight(wh);
        if (Number("Pos Z", wp.z, 0.1f)) *w->GetPosPtr() = wp;
        ImGui::PopID();
    }
    if (Group("Size", g_sizeOpen)) {
        ImGui::PushID("size");
        Vector3 ws = *w->GetSizePtr();
        if (Number("Width", ws.x, 0.1f, 1.0f)) { *w->GetSizePtr() = ws; w->MarkMeshDirty(); }
        if (Number("Depth", ws.z, 0.1f, 1.0f)) { *w->GetSizePtr() = ws; w->MarkMeshDirty(); }
        ImGui::PopID();
    }
    if (Group("Appearance", g_generalOpen)) {
        ImGui::PushID("look");
        Color base = w->GetBaseColor();
        if (ColorField("Colour", base)) w->SetBaseColor(base);
        float trans = w->GetTransparency();
        if (Number("Transparency", trans, 0.01f)) w->SetTransparency(trans);
        // On: the player floats and swims. Off: falls through.
        bool coll = w->GetPlayerCollision();
        if (Check("Player collision", coll)) {
            w->SetPlayerCollision(coll);
            Log("'%s' player collision = %s", w->GetName().c_str(), coll ? "true" : "false");
        }
        ImGui::PopID();
    }
    if (Group("Noise", g_waterNoiseOpen)) {
        ImGui::PushID("noise");
        WaterBody::NoiseParams n = w->GetNoiseParams();
        bool changed = Number("Amplitude", n.amplitude, 0.01f);
        changed |= Number("Frequency", n.frequency, 0.01f);
        changed |= Number("Speed", n.speed, 0.01f);
        PropertyRow("Octaves");
        int oct = n.octaves;
        const bool octChanged = ImGui::DragInt("##octaves", &oct, 0.1f, 1, 12, "%d", ImGuiSliderFlags_AlwaysClamp) && oct != n.octaves;
        UndoOnFirstChange(octChanged);
        if (octChanged) { n.octaves = oct; changed = true; }
        if (changed) w->SetNoiseParams(n);
        ImGui::PopID();
    }
    if (Group("Foam", g_waterFoamOpen)) {
        ImGui::PushID("foam");
        WaterBody::FoamParams f = w->GetFoamParams();
        bool changed = Number("Intensity", f.intensity, 0.01f);
        changed |= Number("Scale", f.scale, 0.01f);
        changed |= Number("Threshold", f.threshold, 0.01f);
        if (changed) w->SetFoamParams(f);
        ImGui::PopID();
    }
    // Capillary droplets and ripples.
    if (Group("Surface detail", g_waterDetailOpen)) {
        ImGui::PushID("detail");
        WaterBody::DetailParams d = w->GetDetailParams();
        bool changed = Number("Intensity", d.intensity, 0.01f);
        changed |= Number("Scale", d.scale, 0.01f);
        if (changed) w->SetDetailParams(d);
        ImGui::PopID();
    }
    // A planar mirror of the world around this water.
    if (Group("Reflections", g_waterReflectOpen)) {
        ImGui::PushID("reflect");
        WaterBody::ReflectionParams r = w->GetReflectionParams();
        bool changed = Number("Strength", r.strength, 0.01f);
        changed |= Number("Distortion", r.distortion, 0.01f);
        if (changed) w->SetReflectionParams(r);
        ImGui::PopID();
    }
    // What the camera sees inside this water.
    if (Group("Underwater", g_waterUnderOpen)) {
        ImGui::PushID("under");
        WaterBody::UnderwaterParams u = w->GetUnderwaterParams();
        bool changed = Number("Range (m)", u.range, 0.1f, 0.0f);
        changed |= Number("Murk", u.murk, 0.01f, 0.0f);
        changed |= Number("Darken", u.darken, 0.01f, 0.0f, 1.0f);
        if (changed) w->SetUnderwaterParams(u);
        ImGui::PopID();
    }
    // Dynamic water: boat wakes, splashes, foam trails, droplets.
    if (Group("Wake & spray", g_waterWakeOpen)) {
        ImGui::PushID("wake");
        WaterBody::RippleParams rp = w->GetRippleParams();
        bool changed = Number("Wake", rp.wakeStrength, 0.01f, 0.0f);
        changed |= Number("Splash", rp.splashStrength, 0.01f, 0.0f);
        changed |= Number("Foam life", rp.foamLifetime, 0.05f, 0.2f, 60.0f);
        // Spray amount: 0 turns droplets off.
        float spray = rp.spray ? rp.sprayAmount : 0.0f;
        if (Number("Spray", spray, 0.01f, 0.0f, 5.0f)) {
            rp.sprayAmount = spray;
            rp.spray = spray > 0.0f;
            changed = true;
        }
        if (changed) w->SetRippleParams(rp);
        ImGui::PopID();
    }
}

void DrawTextureRows(ScatteredObject* obj) {
    const auto& project = ::project::GetCurrentProject();
    bool hasMesh = false, anyTexture = false;
    for (auto* o : g_selection) {
        if (!o) continue;
        hasMesh = hasMesh || o->HasModel();
        anyTexture = anyTexture || !o->GetTexturePath().empty();
    }

    PropertyRow("Texture");
    const std::string tex = obj ? FileName(obj->GetTexturePath()) : std::string();
    if (tex.empty()) ImGui::TextDisabled("None");
    else ImGui::TextUnformatted(tex.c_str());

    // Browse and clear, for imported meshes.
    if (hasMesh && !g_playActive) {
        ImGui::Dummy(ImVec2(PropertyLabelWidth() - ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::SameLine(PropertyLabelWidth());
        const float clearW = ImGui::GetFrameHeight();
        if (ImGui::Button("Browse...", ImVec2(ImGui::GetContentRegionAvail().x - clearW - ImGui::GetStyle().ItemSpacing.x, 0.0f))) {
            // Non-blocking: PumpAssetBrowserDialogs() delivers the result on a later frame.
            BeginChooseTexturePath(project.path);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!anyTexture || project.path.empty());
        bool clear;
        if (g_deleteIcon.id != 0) {
            const float pad = ImGui::GetStyle().FramePadding.y;
            const float icon = clearW - pad * 2.0f;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pad, pad));
            clear = ImGui::ImageButton("##cleartex", static_cast<ImTextureID>(GetTextureImGuiId(g_deleteIcon)), ImVec2(icon, icon));
            ImGui::PopStyleVar();
        } else {
            clear = ImGui::Button("X##cleartex", ImVec2(clearW, 0.0f));
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Remove the texture");
        if (clear) {
            ui::PushUndo();
            for (auto* o : g_selection) if (o && o->HasModel()) o->ClearTexture(project.path);
        }
    }

    // Preset textures, for primitives and meshes alike.
    if (g_selection.empty()) return;
    static bool s_listOpen = false;
    const std::vector<std::string>& presets = PresetTextures(s_listOpen);
    if (presets.empty()) {
        PropertyRow("Preset");
        ImGui::TextDisabled("Could not load preset textures");
        return;
    }
    int selected = -1;   // -1: a custom texture, 0: none, n: presets[n - 1]
    const std::string curTex = obj ? obj->GetTexturePath() : std::string();
    if (curTex.empty()) selected = 0;
    else {
        const std::string stem = std::filesystem::path(curTex).stem().string();
        for (size_t i = 0; i < presets.size(); ++i)
            if (std::filesystem::path(presets[i]).stem().string() == stem) { selected = static_cast<int>(i) + 1; break; }
    }
    const std::string current = selected == 0 ? "None"
        : selected > 0 ? std::filesystem::path(presets[selected - 1]).stem().string() : "Custom texture";
    PropertyRow("Preset");
    s_listOpen = ImGui::BeginCombo("##preset", current.c_str());
    if (!s_listOpen) return;
    int chosen = -1;
    if (ImGui::Selectable("None", selected == 0)) chosen = 0;
    for (size_t i = 0; i < presets.size(); ++i) {
        const std::string name = std::filesystem::path(presets[i]).stem().string();
        if (ImGui::Selectable(name.c_str(), selected == static_cast<int>(i) + 1)) chosen = static_cast<int>(i) + 1;
    }
    ImGui::EndCombo();
    if (chosen < 0) return;
    ui::PushUndo();
    if (chosen == 0) {
        for (auto* o : g_selection) if (o) o->ClearTexture(project.path);
    } else {
        // Copied into the project's PresetTextures folder, so the scene saves a portable path.
        const std::string rel = textureManager::EnsurePresetTextureInProject(presets[chosen - 1], project.path);
        for (auto* o : g_selection) if (o) o->SetTexturePath(rel, project.path);
    }
}

void DrawObjectGeneral(ScatteredObject* obj) {
    if (g_color) {
        ImGui::BeginDisabled(g_playActive);
        if (ColorField("Colour", *g_color) && g_selection.size() > 1)
            ForOthers([](ScatteredObject& o) { *o.GetColorPtr() = *g_color; });
        ImGui::EndDisabled();
    }
    if (!obj) return;

    bool anchored = obj->anchored;
    if (Check("Anchored", anchored)) {
        obj->anchored = anchored;
        ForOthers([&](ScatteredObject& o) { o.anchored = anchored; });
        Log("'%s' anchored = %s", obj->GetName().c_str(), anchored ? "true" : "false");
    }
    bool canCollide = obj->canCollide;
    if (Check("Can collide", canCollide)) {
        obj->canCollide = canCollide;
        ForOthers([&](ScatteredObject& o) { o.canCollide = canCollide; });
        Log("'%s' canCollide = %s", obj->GetName().c_str(), canCollide ? "true" : "false");
    }

    PropertyRow("Collision");
    const pcoll::CollisionAccuracy acc = obj->GetCollisionAccuracy();
    ImGui::BeginDisabled(!obj->IsCollisionAccuracySupported());
    if (ImGui::BeginCombo("##collision", pcoll::CollisionAccuracyName(acc))) {
        for (int i = 0; i < 4; ++i) {
            const auto a = static_cast<pcoll::CollisionAccuracy>(i);
            if (!ImGui::Selectable(pcoll::CollisionAccuracyName(a), a == acc) || a == acc) continue;
            ui::PushUndo();
            obj->SetCollisionAccuracy(a);
            ForOthers([&](ScatteredObject& o) { o.SetCollisionAccuracy(a); });
            Log("'%s' collision accuracy = %s", obj->GetName().c_str(), pcoll::CollisionAccuracyName(a));
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    // 0 = visible, 1 = invisible.
    float transparency = obj->GetTransparency();
    if (Slider01("Transparency", transparency)) {
        obj->SetTransparency(transparency);
        ForOthers([&](ScatteredObject& o) { o.SetTransparency(transparency); });
    }

    // Mass: automatic from the size until typed in; ticking Auto goes back to that.
    {
        PropertyRow("Mass");
        const float autoW = ImGui::CalcTextSize("Auto").x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - autoW - ImGui::GetStyle().ItemSpacing.x);
        float mass = obj->GetMass();
        const bool massChanged = ImGui::DragFloat("##mass", &mass, 0.1f, 0.0f, 1.0e6f, "%.2f", ImGuiSliderFlags_AlwaysClamp) &&
                                 mass != obj->GetMass();
        UndoOnFirstChange(massChanged);
        if (massChanged && std::isfinite(mass)) {
            obj->SetMass(mass);
            ForOthers([&](ScatteredObject& o) { o.SetMass(mass); });
        }
        ImGui::SameLine();
        bool massAuto = obj->IsMassAuto();
        if (ImGui::Checkbox("Auto##mass", &massAuto)) {
            ui::PushUndo();
            auto setAuto = [&](ScatteredObject& o) { if (massAuto) o.ResetMassAuto(); else o.SetMass(o.GetMass()); };
            setAuto(*obj);
            ForOthers(setAuto);
            Log("'%s' mass = %s (%.3f)", obj->GetName().c_str(), obj->IsMassAuto() ? "auto" : "manual", obj->GetMass());
        }
    }

    if (obj->HasModel()) {
        PropertyRow("Mesh");
        ImGui::TextUnformatted(FileName(obj->GetModelPath()).c_str());
    }

    // Skeletal clips of an imported model.
    if (const int clips = obj->GetAnimationCount(); clips > 0) {
        PropertyRow("Animation");
        const int cur = std::max(0, obj->GetCurrentAnimation());
        const char* name = *obj->GetAnimationName(cur) ? obj->GetAnimationName(cur) : "(unnamed)";
        const std::string label = std::to_string(cur + 1) + "/" + std::to_string(clips) + "  " + name + "###clip";
        const float playW = ImGui::GetFrameHeight();
        ImGui::Button(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - playW - ImGui::GetStyle().ItemSpacing.x, 0.0f));
        int next = -1;
        if (clips > 1 && ImGui::IsItemClicked(ImGuiMouseButton_Left)) next = (cur + 1) % clips;
        if (clips > 1 && ImGui::IsItemClicked(ImGuiMouseButton_Right)) next = (cur + clips - 1) % clips;
        if (clips > 1 && ImGui::IsItemHovered()) ImGui::SetTooltip("Click: next clip. Right-click: previous clip.");
        if (next >= 0) {
            obj->PlayAnimation(next, obj->animLoop, obj->animSpeed);
            Log("'%s' animation = %s", obj->GetName().c_str(), obj->GetAnimationName(next));
        }
        ImGui::SameLine();
        const bool playing = obj->animPlay;
        if (ImGui::Button(playing ? "||###play" : ">###play", ImVec2(playW, 0.0f))) {
            if (playing) obj->StopAnimation();
            else obj->PlayAnimation(cur, obj->animLoop, obj->animSpeed);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(playing ? "Pause" : "Play");
    }
}

// Physically based: metallic, roughness, glow.
void DrawObjectMaterial(ScatteredObject* obj) {
    gfx::SurfaceMaterial& m = obj->surface;
    // An imported model keeps its file's materials until this is unticked.
    if (obj->HasModel()) {
        bool own = obj->useModelMaterial;
        if (Check("Model's own", own)) {
            obj->useModelMaterial = own;
            ForOthers([&](ScatteredObject& o) { o.useModelMaterial = own; });
            Log("'%s' uses %s material", obj->GetName().c_str(), own ? "the model's" : "its own");
        }
    }
    // Editing either value on a model switches it to its own material (otherwise the edit would not show).
    if (Slider01("Metallic", m.metallic)) {
        obj->useModelMaterial = false;
        ForOthers([&](ScatteredObject& o) { o.surface.metallic = m.metallic; o.useModelMaterial = false; });
    }
    if (Slider01("Roughness", m.roughness)) {
        obj->useModelMaterial = false;
        ForOthers([&](ScatteredObject& o) { o.surface.roughness = m.roughness; o.useModelMaterial = false; });
    }
    // Glow: strength, then the glow colour's swatch on the same row.
    PropertyRow("Glow");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
    float glow = m.emissiveStrength;
    const bool glowChanged = ImGui::DragFloat("##glow", &glow, 0.05f, 0.0f, 1000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp) &&
                             glow != m.emissiveStrength;
    UndoOnFirstChange(glowChanged);
    if (glowChanged && std::isfinite(glow)) {
        m.emissiveStrength = glow;
        ForOthers([&](ScatteredObject& o) { o.surface.emissiveStrength = glow; });
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(g_playActive);
    float f[3] = { m.emissive.r / 255.0f, m.emissive.g / 255.0f, m.emissive.b / 255.0f };
    const bool colChanged = ImGui::ColorEdit3("##glowcol", f, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    UndoOnFirstChange(colChanged);
    if (colChanged) {
        m.emissive.r = static_cast<unsigned char>(std::lround(f[0] * 255.0f));
        m.emissive.g = static_cast<unsigned char>(std::lround(f[1] * 255.0f));
        m.emissive.b = static_cast<unsigned char>(std::lround(f[2] * 255.0f));
        ForOthers([&](ScatteredObject& o) { o.surface.emissive = m.emissive; });
    }
    ImGui::EndDisabled();
}

void DrawObjectWater(ScatteredObject* obj) {
    struct Opt { const char* label; bool ScatteredObject::WaterInteraction::* member; };
    constexpr Opt opts[3] = {
        { "Floats", &ScatteredObject::WaterInteraction::floats },
        { "Foam",   &ScatteredObject::WaterInteraction::foam },
        { "Splash", &ScatteredObject::WaterInteraction::splash },
    };
    for (const Opt& o : opts) {
        bool on = obj->waterInteraction.*(o.member);
        if (!Check(o.label, on)) continue;
        obj->waterInteraction.*(o.member) = on;
        ForOthers([&](ScatteredObject& s) { s.waterInteraction.*(o.member) = on; });
        Log("'%s' %s %s", obj->GetName().c_str(), o.label, on ? "true" : "false");
    }
}

// Linear or angular velocity: the live body's while playing, else the object's start value.
void DrawVelocity(ScatteredObject* obj, bool angular, const char* const labels[3]) {
    const bool live = g_playActive && g_simulation;
    auto get = [&](ScatteredObject* o) {
        if (live) return angular ? g_simulation->GetBodyAngularVelocity(o) : g_simulation->GetBodyVelocity(o);
        return angular ? o->GetAngularVelocity() : o->GetVelocity();
    };
    auto set = [&](ScatteredObject* o, Vector3 v) {
        if (live) { if (angular) g_simulation->SetBodyAngularVelocity(o, v); else g_simulation->SetBodyVelocity(o, v); }
        else if (angular) o->SetAngularVelocity(v);
        else o->SetVelocity(v);
    };
    Vector3 vel = get(obj);
    float* axis[3] = { &vel.x, &vel.y, &vel.z };
    for (int i = 0; i < 3; ++i) {
        if (!Number(labels[i], *axis[i], 0.1f)) continue;
        set(obj, vel);
        const float nv = *axis[i];
        ForOthers([&](ScatteredObject& o) {
            Vector3 ov = get(&o);
            float* oa[3] = { &ov.x, &ov.y, &ov.z };
            *oa[i] = nv;
            set(&o, ov);
        });
    }
}

} // namespace

void DrawImGuiPropertiesPanel() {
    // The Lighting and node panels take this spot while one of theirs is selected.
    if (g_lightingSel != 0 || g_selectedNodeObj) return;
    const Rectangle rec = GetPropertiesPanelBounds();
    if (rec.width <= 0.0f) return;
    ImGui::SetNextWindowPos(ImVec2(rec.x, rec.y));
    ImGui::SetNextWindowSize(ImVec2(rec.width, rec.height));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##Properties", nullptr, flags);

    // The title strip doubles as the layout's drag handle, so it holds no widgets.
    WaterBody* water = g_selectedWater;
    ImGui::TextColored(theme::AccentVec(), water ? "WATER BODY" : "PROPERTIES");
    if (!water && g_selection.size() > 1) {
        ImGui::SameLine();
        ImGui::TextDisabled("%d selected", static_cast<int>(g_selection.size()));
    }
    ImGui::Separator();
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), kPanelTitleH));

    ImGui::BeginChild("##PropertiesContent", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoSavedSettings);
    if (water) {
        DrawWaterProperties(water);
    } else if (!g_pos && !g_size && !g_color) {
        ImGui::Spacing();
        ImGui::TextWrapped("Select an object in the viewport or the Explorer to edit it here.");
    } else {
        ScatteredObject* obj = g_selectedObject;
        if (Group("General", g_generalOpen)) {
            ImGui::PushID("general");
            DrawObjectGeneral(obj);
            DrawTextureRows(obj);
            ImGui::PopID();
        }
        if (obj && Group("Material", g_materialOpen)) {
            ImGui::PushID("material");
            DrawObjectMaterial(obj);
            ImGui::PopID();
        }
        if (obj && Group("Water interactions", g_waterInteractOpen)) {
            ImGui::PushID("water");
            DrawObjectWater(obj);
            ImGui::PopID();
        }
        static const char* const kPos[3] = { "Pos X", "Pos Y", "Pos Z" };
        static const char* const kSize[3] = { "Width", "Height", "Length" };
        static const char* const kRot[3] = { "Rot X", "Rot Y", "Rot Z" };
        static const char* const kOrigin[3] = { "Origin X", "Origin Y", "Origin Z" };
        if (g_pos) Vector3Group("Position", g_positionOpen, *g_pos, kPos, [](ScatteredObject& o) { return o.GetPosPtr(); }, 0.1f);
        // A new size means a new automatic mass. Sizes stay above zero.
        if (g_size && Vector3Group("Size", g_sizeOpen, *g_size, kSize, [](ScatteredObject& o) { return o.GetSizePtr(); }, 0.05f, 0.01f)) {
            if (obj) obj->ResetMassAuto();
            ForOthers([](ScatteredObject& o) { o.ResetMassAuto(); });
        }
        if (g_rotation) Vector3Group("Rotation", g_rotationOpen, *g_rotation, kRot, [](ScatteredObject& o) { return o.GetRotationPtr(); }, 0.5f);
        if (g_origin) Vector3Group("Origin", g_originOpen, *g_origin, kOrigin, [](ScatteredObject& o) { return o.GetOriginPtr(); }, 0.05f);
        if (obj && Group("Linear velocity", g_linearVelocityOpen)) {
            static const char* const kVel[3] = { "Vel X", "Vel Y", "Vel Z" };
            ImGui::PushID("linvel");
            DrawVelocity(obj, false, kVel);
            ImGui::PopID();
        }
        if (obj && Group("Angular velocity", g_angularVelocityOpen)) {
            static const char* const kAng[3] = { "Ang X", "Ang Y", "Ang Z" };
            ImGui::PushID("angvel");
            DrawVelocity(obj, true, kAng);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace ui
