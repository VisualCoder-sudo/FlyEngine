// ui_terrain_panel.cpp: the terrain tool panel (Dear ImGui), split out of ui.cpp.
#include "ui_internal.hpp"
#include <map>
#include <set>
#include "../../../include/Engine/ExportManager.hpp"
#include "../../../include/Engine/Backend/VisualScript.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Terrain/Water/WaterBody.hpp"
#include "../../../include/Terrain/BasicTerrain.hpp"
#include "../../../include/Terrain/Foliage.hpp"
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
// Terrain tool panel (Dear ImGui)
// Top: the Sculpt, Generate and Foliage tabs. Bottom: the selected terrain's properties.
// ---------------------------------------------------------------------------
static int g_terrainTab = 0; // 0 = Sculpt tab, 1 = Generate tab, 2 = Foliage tab
static int g_foliagePickType = -1; // Foliage tab: the model type a picked file is added to as a variant; -1 = it makes a new type

Rectangle GetTerrainPanelBounds() { return GetPanelBounds(PanelId::Terrain); }

namespace {

constexpr const char* kToolNames[TTOOLICON_COUNT] = { "Raise", "Lower", "Smooth", "Flatten", "Paint", "Erode", "Noise", "Ramp", "None" };

ImTextureID ToolIcon(int tool) {
    if (tool < 0 || tool >= TTOOLICON_COUNT || g_terrainToolIcons[tool].id == 0) return static_cast<ImTextureID>(0);
    return static_cast<ImTextureID>(GetTextureImGuiId(g_terrainToolIcons[tool]));
}

// typedMax: a number typed in with Ctrl+click may go past the slider's range, up to this limit (default: the range itself).
bool Slider(const char* label, float& value, float mn, float mx, const char* fmt, float typedMax = -1.0e30f) {
    PropertyRow(label);
    ImGui::PushID(label);
    float v = value;
    const bool changed = ImGui::SliderFloat("##v", &v, mn, mx, fmt);
    if (changed) value = std::isfinite(v) ? std::clamp(v, mn, std::max(mx, typedMax)) : mn;
    ImGui::PopID();
    return changed;
}

bool SliderInt(const char* label, int& value, int mn, int mx) {
    PropertyRow(label);
    ImGui::PushID(label);
    const bool changed = ImGui::SliderInt("##v", &value, mn, mx, "%d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopID();
    return changed;
}

// A seed: a typed number with a Random button beside it.
void SeedRow(const char* label, int& seed, int maxSeed) {
    PropertyRow(label);
    ImGui::PushID(label);
    const float randW = ImGui::CalcTextSize("Random").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - randW - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::InputInt("##seed", &seed, 0)) seed = std::clamp(seed, 0, maxSeed);
    ImGui::SameLine();
    if (ImGui::Button("Random")) seed = GetRandomValue(0, maxSeed);
    ImGui::PopID();
}

// A full-width button in the accent colour, for the action a section exists for.
bool AccentButton(const char* label, float width = -FLT_MIN) {
    ImGui::PushStyleColor(ImGuiCol_Button, theme::Vec(theme::ACCENT, 55));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::Vec(theme::ACCENT, 110));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::Vec(theme::ACCENT, 160));
    const bool pressed = ImGui::Button(label, ImVec2(width, 0.0f));
    ImGui::PopStyleColor(3);
    return pressed;
}

float HalfWidth() { return (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f; }

void InfoRow(const char* key, const char* value) {
    PropertyRow(key);
    ImGui::TextUnformatted(value);
}

// The erosion sliders and the two Erode buttons. Returns 1 for Erode region, 2 for Erode all, else 0.
int ErosionSection(float& thermal, float& hydraulic, float& rain, float& evaporation, float& deposit, int& iterations) {
    ImGui::SeparatorText("Erosion");
    SliderInt("Iterations", iterations, 1, 60);
    Slider("Thermal", thermal, 0.0f, 2.0f, "%.2f");
    Slider("Hydraulic", hydraulic, 0.0f, 2.0f, "%.2f");
    Slider("Rain", rain, 0.0f, 0.2f, "%.3f");
    Slider("Evaporation", evaporation, 0.0f, 0.1f, "%.3f");
    Slider("Deposit", deposit, 0.0f, 1.0f, "%.2f");
    const float bw = HalfWidth();
    int pressed = AccentButton("Erode region", bw) ? 1 : 0;
    ImGui::SameLine();
    if (AccentButton("Erode all", bw)) pressed = 2;
    return pressed;
}

// Use a unified brush for the UI: the chunked and the basic terrain keep their own.
struct UnifiedBrush {
    BasicTerrain::Tool tool = BasicTerrain::Tool::None;
    BasicTerrain::Shape shape = BasicTerrain::Shape::Circle;
    float radius = 25.0f;
    float strength = 40.0f;
    float hardness = 0.5f;
    float targetHeight = 0.0f;
    int paintLayer = 0;
    bool paintErase = false;
    int noiseSeed = 1337;
    float noiseScale = 40.0f;
    // Erosion settings
    float erosionThermal = 0.5f;
    float erosionHydraulic = 0.8f;
    float erosionTalusDeg = 3.0f;
    float erosionRain = 0.05f;
    float erosionEvaporation = 0.03f;
    float erosionDeposit = 0.6f;
    int erosionIterations = 6;
};

UnifiedBrush ReadBrush(bool chunked, const terrain::TerrainBrush& c, const BasicTerrain::Brush& b) {
    UnifiedBrush br;
    if (chunked) {
        br.tool = static_cast<BasicTerrain::Tool>(c.tool);
        br.shape = static_cast<BasicTerrain::Shape>(c.shape);
        br.radius = c.radius;
        br.strength = c.strength;
        br.hardness = c.hardness;
        br.targetHeight = c.targetHeight;
        br.paintLayer = c.paintLayer;
        br.paintErase = c.paintErase;
        br.noiseSeed = c.noiseSeed;
        br.noiseScale = c.noiseScale;
        br.erosionThermal = c.erosionThermal;
        br.erosionHydraulic = c.erosionHydraulic;
        br.erosionTalusDeg = c.erosionTalusDeg;
        br.erosionRain = c.erosionRain;
        br.erosionEvaporation = c.erosionEvaporation;
        br.erosionDeposit = c.erosionDeposit;
        br.erosionIterations = c.erosionIterations;
    } else {
        br.tool = b.tool;
        br.shape = b.shape;
        br.radius = b.radius;
        br.strength = b.strength;
        br.hardness = b.hardness;
        br.targetHeight = b.targetHeight;
        br.paintLayer = b.paintLayer;
        br.paintErase = b.paintErase;
        br.noiseSeed = b.noiseSeed;
        br.noiseScale = b.noiseScale;
        // BasicTerrain doesn't have erosion settings in brush, use defaults
    }
    return br;
}

void WriteBrush(const UnifiedBrush& br, bool chunked, terrain::TerrainBrush& c, BasicTerrain::Brush& b) {
    if (chunked) {
        c.tool = static_cast<terrain::TerrainTool>(br.tool);
        c.shape = static_cast<terrain::TerrainBrush::Shape>(br.shape);
        c.radius = br.radius;
        c.strength = br.strength;
        c.hardness = br.hardness;
        c.targetHeight = br.targetHeight;
        c.paintLayer = br.paintLayer;
        c.paintErase = br.paintErase;
        c.noiseSeed = br.noiseSeed;
        c.noiseScale = br.noiseScale;
        c.erosionThermal = br.erosionThermal;
        c.erosionHydraulic = br.erosionHydraulic;
        c.erosionTalusDeg = br.erosionTalusDeg;
        c.erosionRain = br.erosionRain;
        c.erosionEvaporation = br.erosionEvaporation;
        c.erosionDeposit = br.erosionDeposit;
        c.erosionIterations = br.erosionIterations;
    } else {
        b.tool = br.tool;
        b.shape = br.shape;
        b.radius = br.radius;
        b.strength = br.strength;
        b.hardness = br.hardness;
        b.targetHeight = br.targetHeight;
        b.paintLayer = br.paintLayer;
        b.paintErase = br.paintErase;
        b.noiseSeed = br.noiseSeed;
        b.noiseScale = br.noiseScale;
    }
}

void DrawGenerateTab(BasicTerrain* tBasic, terrain::Terrain* tChunked, UnifiedBrush& br, terrain::TerrainBrush& brChunked) {
    if (!tBasic && !tChunked) {
        ImGui::TextDisabled("Select or spawn a terrain first.");
        return;
    }
    if (tBasic) {
        if (!tBasic->genBoxInitialized) {
            tBasic->genBoxPos = { tBasic->position.x, tBasic->position.y + tBasic->GetMaxHeight() * 0.15f, tBasic->position.z };
            float fw = tBasic->GetWidth() * tBasic->GetScale();
            float fd = tBasic->GetDepth() * tBasic->GetScale();
            tBasic->genBoxSize = { fw * 0.5f, std::max(20.0f, (tBasic->GetMaxHeight() - tBasic->GetMinHeight()) * 0.4f), fd * 0.5f };
            tBasic->genBoxInitialized = true;
        }

        SeedRow("Seed", tBasic->genSeed, 999999);

        ImGui::SeparatorText("Include");
        ImGui::Checkbox("Hills", &tBasic->genHills);
        ImGui::Checkbox("Mountains", &tBasic->genMountains);
        ImGui::Checkbox("Extra textures", &tBasic->genTextures);

        ImGui::SeparatorText("Region box position");
        const float posRangeX = tBasic->GetWidth() * tBasic->GetScale();
        const float posRangeZ = tBasic->GetDepth() * tBasic->GetScale();
        ImGui::PushID("boxpos");
        Slider("X", tBasic->genBoxPos.x, tBasic->position.x - posRangeX, tBasic->position.x + posRangeX, "%.0f m");
        Slider("Y", tBasic->genBoxPos.y, tBasic->GetMinHeight(), tBasic->GetMaxHeight(), "%.0f m");
        Slider("Z", tBasic->genBoxPos.z, tBasic->position.z - posRangeZ, tBasic->position.z + posRangeZ, "%.0f m");
        ImGui::PopID();

        ImGui::SeparatorText("Region box size");
        const float maxVSize = std::max(20.0f, tBasic->GetMaxHeight() - tBasic->GetMinHeight());
        ImGui::PushID("boxsize");
        Slider("Width", tBasic->genBoxSize.x, 4.0f, posRangeX * 2.0f, "%.0f m");
        Slider("Height", tBasic->genBoxSize.y, 4.0f, maxVSize, "%.0f m");
        Slider("Depth", tBasic->genBoxSize.z, 4.0f, posRangeZ * 2.0f, "%.0f m");
        ImGui::PopID();

        ImGui::Spacing();
        if (AccentButton("Generate")) tBasic->GenerateTerrainInBox();

        static BasicTerrain::ErosionSettings es;
        const int erode = ErosionSection(es.thermal, es.hydraulic, es.rain, es.evaporation, es.deposit, es.iterations);
        if (erode == 1) {
            // The region box as a grid-cell rectangle.
            const float scaleF = tBasic->GetScale();
            const float c0x = (tBasic->genBoxPos.x - tBasic->genBoxSize.x * 0.5f - tBasic->position.x) / scaleF + tBasic->GetWidth() * 0.5f;
            const float c1x = (tBasic->genBoxPos.x + tBasic->genBoxSize.x * 0.5f - tBasic->position.x) / scaleF + tBasic->GetWidth() * 0.5f;
            const float c0z = (tBasic->genBoxPos.z - tBasic->genBoxSize.z * 0.5f - tBasic->position.z) / scaleF + tBasic->GetDepth() * 0.5f;
            const float c1z = (tBasic->genBoxPos.z + tBasic->genBoxSize.z * 0.5f - tBasic->position.z) / scaleF + tBasic->GetDepth() * 0.5f;
            tBasic->ErodeRegion((int)std::floor(std::min(c0x, c1x)), (int)std::floor(std::min(c0z, c1z)),
                                (int)std::ceil(std::max(c0x, c1x)), (int)std::ceil(std::max(c0z, c1z)), es);
        } else if (erode == 2) {
            tBasic->ErodeRegion(0, 0, tBasic->GetWidth() - 1, tBasic->GetDepth() - 1, es);
        }
        return;
    }

    // Chunked terrain: no procedural generation, but erosion works.
    ImGui::TextDisabled("Chunked terrain (no procedural generation)");
    ImGui::SeparatorText("Region");
    const float halfW = tChunked->size.x * 0.5f;
    const float halfD = tChunked->size.z * 0.5f;
    auto& state = terrain::GetTerrainEditorState();
    Slider("Min X", state.regionMinX, tChunked->center.x - halfW, tChunked->center.x + halfW, "%.0f m");
    Slider("Max X", state.regionMaxX, tChunked->center.x - halfW, tChunked->center.x + halfW, "%.0f m");
    Slider("Min Z", state.regionMinZ, tChunked->center.z - halfD, tChunked->center.z + halfD, "%.0f m");
    Slider("Max Z", state.regionMaxZ, tChunked->center.z - halfD, tChunked->center.z + halfD, "%.0f m");

    const int erode = ErosionSection(br.erosionThermal, br.erosionHydraulic, br.erosionRain, br.erosionEvaporation,
                                     br.erosionDeposit, br.erosionIterations);
    // ErodeRegion reads the chunked brush, so this frame's slider values go in first.
    if (erode != 0) WriteBrush(br, true, brChunked, BasicTerrain::GetBrush());
    if (erode == 1) {
        tChunked->ErodeRegion(state.regionMinX, state.regionMinZ, state.regionMaxX, state.regionMaxZ, brChunked);
    } else if (erode == 2) {
        tChunked->ErodeRegion(tChunked->center.x - halfW, tChunked->center.z - halfD,
                              tChunked->center.x + halfW, tChunked->center.z + halfD, brChunked);
    }
}

void DrawFoliageTab(BasicTerrain* tBasic, UnifiedBrush& br) {
    if (!tBasic) {
        ImGui::TextDisabled("Select or spawn a Basic Terrain first.");
        return;
    }
    foliage::Settings& fs = foliage::For(tBasic);
    const foliage::Settings fsBefore = fs;
    // New plants start unpainted: nothing grows until you paint it with the brush.
    auto addRule = [&](int typeId) {
        foliage::Rule nr = foliage::DefaultRuleForType(typeId);
        nr.painted = true;
        fs.rules.push_back(nr);
        const int added = (int)fs.rules.size() - 1;
        foliage::FillMask(tBasic, added, false);
        foliage::SetPaintTarget(tBasic, added);
        BasicTerrain::GetBrush().tool = BasicTerrain::Tool::None;
        ui::SetSelectedTerrain(tBasic);
    };
    // A model came back from the file dialog: it joins the type it was picked for, or starts a new type.
    {
        std::string picked;
        if (platform::PollDialogResult({ platform::DialogPurpose::PickFoliageModel }, picked) && !picked.empty()) {
            const ::project::Info& proj = ::project::GetCurrentProject();
            std::string stored = picked;
            if (!proj.path.empty()) {   // copied into the project, so the project carries its plants
                const ImportResult imp = ImportModel(picked, proj.path, proj.name);
                if (imp.ok) stored = ResolveStoredAssetPath(imp.storedPath, proj.path);
                else LogAlways("[foliage] import failed: %s", imp.error.c_str());
            }
            const int target = g_foliagePickType;
            g_foliagePickType = -1;
            if (target >= foliage::kBuiltinTypes && target < foliage::TypeCount()) {
                foliage::TypeAt(target).variants.push_back({ stored, 1.0f });
                foliage::TypeChanged(target);
            } else {
                foliage::Type nt;
                nt.name = std::filesystem::path(picked).stem().string();
                if (nt.name.empty()) nt.name = "Plant";
                const std::string stem = nt.name;
                for (int n = 2; foliage::FindType(nt.name) >= 0; n++) nt.name = stem + " " + std::to_string(n);
                nt.variants.push_back({ stored, 1.0f });
                addRule(foliage::AddType(nt));
            }
        }
    }
    ImGui::TextWrapped("Add a plant, then drag on the terrain to paint it.");
    ImGui::Checkbox("Foliage on", &fs.enabled);
    ImGui::PushID("global");
    Slider("Wind", fs.wind, 0.0f, 2.0f, "%.2f");
    Slider("Density", fs.density, 0.1f, 6.0f, "x%.2f");
    Slider("Draw distance", fs.distance, 0.25f, 3.0f, "x%.2f");
    ImGui::PopID();

    ImGui::SeparatorText("Add");
    {
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float bw = (ImGui::GetContentRegionAvail().x - gap * 2.0f) / 3.0f;
        const int typeCount = std::min(foliage::TypeCount(), 70);
        for (int k = 0; k < typeCount; k++) {
            ImGui::PushID(k);
            const std::string& name = foliage::TypeAt(k).name;
            if (ImGui::Button(name.c_str(), ImVec2(bw, 0.0f))) addRule(k);
            if (ImGui::IsItemHovered() && ImGui::CalcTextSize(name.c_str()).x > bw - ImGui::GetStyle().FramePadding.x * 2.0f)
                ImGui::SetTooltip("%s", name.c_str());
            ImGui::PopID();
            if (k % 3 != 2 && k != typeCount - 1) ImGui::SameLine();
        }
        if (ImGui::Button("Import model plant...", ImVec2(-FLT_MIN, 0.0f))) {
            g_foliagePickType = -1;
            BeginChooseModelFor(platform::DialogPurpose::PickFoliageModel, "Choose a plant model");
        }
    }

    int removeIndex = -1;
    for (size_t ri = 0; ri < fs.rules.size(); ri++) {
        foliage::Rule& r = fs.rules[ri];
        ImGui::PushID(static_cast<int>(ri));
        ImGui::Separator();
        ImGui::Checkbox(foliage::TypeAt(r.type).name.c_str(), &r.enabled);
        const float removeW = ImGui::CalcTextSize("Remove").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - removeW));
        if (ImGui::Button("Remove")) removeIndex = (int)ri;
        if (!r.enabled) { ImGui::PopID(); continue; }

        if (foliage::TypeAt(r.type).builtin < 0) {   // a model type: its models and look
            foliage::Type& ty = foliage::TypeAt(r.type);
            const foliage::Type before = ty;
            int removeVariant = -1;
            for (size_t vi = 0; vi < ty.variants.size(); vi++) {
                foliage::Variant& v = ty.variants[vi];
                ImGui::PushID(static_cast<int>(vi));
                const std::string file = std::filesystem::path(v.model).filename().string();
                if (foliage::TypeVariantLoaded(r.type, (int)vi)) ImGui::TextUnformatted(file.c_str());
                else ImGui::TextDisabled("%s", file.c_str());
                if (ty.variants.size() > 1) {
                    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight()));
                    if (ImGui::Button("X", ImVec2(ImGui::GetFrameHeight(), 0.0f))) removeVariant = (int)vi;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this model");
                }
                Slider("Chance", v.weight, 0.0f, 10.0f, "%.1f");
                ImGui::PopID();
            }
            if (removeVariant >= 0) ty.variants.erase(ty.variants.begin() + removeVariant);
            if (ImGui::Button("Add another model...", ImVec2(-FLT_MIN, 0.0f))) {
                g_foliagePickType = r.type;
                BeginChooseModelFor(platform::DialogPurpose::PickFoliageModel, "Choose a plant model");
            }
            Slider("Height", ty.height, 0.1f, 40.0f, "%.1f m");
            Slider("Sink", ty.sink, -0.5f, 2.0f, "%.2f m");
            ImGui::Checkbox("Casts shadows", &ty.castsShadow);
            ImGui::Checkbox("Sways in wind", &ty.wind);
            ImGui::Checkbox("Two-sided", &ty.twoSided);
            // New models and the look flags load the meshes again; the other sliders only place the plants again.
            bool models = before.variants.size() != ty.variants.size() || before.wind != ty.wind || before.twoSided != ty.twoSided;
            bool placed = before.height != ty.height || before.sink != ty.sink || before.castsShadow != ty.castsShadow;
            for (size_t vi = 0; vi < ty.variants.size() && vi < before.variants.size(); vi++) {
                models = models || before.variants[vi].model != ty.variants[vi].model;
                placed = placed || before.variants[vi].weight != ty.variants[vi].weight;
            }
            if (models || placed) foliage::TypeChanged(r.type, models);
        }

        {
            const float bw = HalfWidth();
            if (ImGui::Button(r.painted ? "Grow on: brush###growon" : "Grow on: layer###growon", ImVec2(bw, 0.0f))) {
                r.painted = !r.painted;
                if (!r.painted && foliage::PaintTarget(tBasic)) foliage::SetPaintTarget(nullptr, -1);
            }
            if (r.painted) {
                int target = -1;
                const bool on = foliage::PaintTarget(tBasic, &target) && target == (int)ri;
                ImGui::SameLine();
                const bool pressed = on ? AccentButton("Painting###paint", bw) : ImGui::Button("Paint###paint", ImVec2(bw, 0.0f));
                if (pressed) {
                    foliage::SetPaintTarget(on ? nullptr : tBasic, on ? -1 : (int)ri);
                    if (!on) { BasicTerrain::GetBrush().tool = BasicTerrain::Tool::None; ui::SetSelectedTerrain(tBasic); }
                }
            }
        }
        if (r.painted) {
            // The shared brush sliders (the panel writes br back at the end).
            const float bw = HalfWidth();
            if (ImGui::Button("Fill all", ImVec2(bw, 0.0f))) foliage::FillMask(tBasic, (int)ri, true);
            ImGui::SameLine();
            if (ImGui::Button("Clear all", ImVec2(bw, 0.0f))) foliage::FillMask(tBasic, (int)ri, false);
            Slider("Brush size", br.radius, 2.0f, 100.0f, "%.0f m", 500.0f);
            Slider("Brush strength", br.strength, 5.0f, 100.0f, "%.0f");
            Slider("Brush hardness", br.hardness, 0.0f, 1.0f, "%.2f");
            ImGui::TextDisabled("Drag on the terrain. Ctrl erases, Shift paints faster.");
        } else {
            SliderInt("Layer", r.layer, -1, 3);
            Slider("Painted", r.minWeight, 0.0f, 1.0f, "%.2f");
        }
        const foliage::Type& type = foliage::TypeAt(r.type);
        const float maxDensity = type.builtin == (int)foliage::Kind::Grass ? 3000.0f
            : (type.builtin == (int)foliage::Kind::Flowers || type.height < 0.8f ? 300.0f : 60.0f);
        Slider("Density", r.density, 0.0f, maxDensity, "%.1f");
        Slider("Min size", r.minScale, 0.2f, 3.0f, "%.2f");
        Slider("Max size", r.maxScale, 0.2f, 4.0f, "%.2f");
        if (r.maxScale < r.minScale) r.maxScale = r.minScale;
        Slider("Max slope", r.maxSlope, 0.0f, 90.0f, "%.0f deg");
        Slider("Distance", r.viewDistance, 10.0f, 2000.0f, "%.0f m", 10000.0f);
        Slider("Fade start", r.fadeStart, 0.0f, 0.95f, "%.2f");
        Slider("Far keep", r.farKeep, 0.0f, 1.0f, "%.2f");
        Slider("LOD 1 at", r.lod1, 0.0f, 1.0f, "%.2f");
        Slider("LOD 2 at", r.lod2, 0.0f, 1.0f, "%.2f");
        if (r.lod2 < r.lod1) r.lod2 = r.lod1;
        SliderInt("Seed", r.seed, 0, 999);
        ImGui::PopID();
    }
    if (removeIndex >= 0) { foliage::SetPaintTarget(nullptr, -1); fs.rules.erase(fs.rules.begin() + removeIndex); }
    if (!fs.rules.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("%d plants placed, %d drawn", (int)foliage::InstanceCount(), (int)foliage::DrawnCount());
    }

    // Rules changed: the plants are placed again. (Dragging a slider re-places them every frame, which is cheap:
    // only the cells near the camera are rebuilt.)
    bool changed = fs.rules.size() != fsBefore.rules.size() || fs.enabled != fsBefore.enabled || fs.density != fsBefore.density;
    for (size_t i = 0; !changed && i < fs.rules.size(); i++) changed = !foliage::SameRule(fs.rules[i], fsBefore.rules[i]);
    if (changed) foliage::Invalidate(tBasic);
}

void DrawSculptTab(BasicTerrain* tBasic, terrain::Terrain* tChunked, UnifiedBrush& br) {
    // Tool: icon, then a dropdown of every tool.
    const int activeTool = std::clamp(static_cast<int>(br.tool), 0, TTOOLICON_COUNT - 1);
    PropertyRow("Tool");
    if (ImTextureID icon = ToolIcon(activeTool)) {
        const float s = ImGui::GetFrameHeight();
        ImGui::Image(icon, ImVec2(s, s));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
    }
    if (ImGui::BeginCombo("##tool", kToolNames[activeTool])) {
        const float s = ImGui::GetTextLineHeight();
        for (int i = 0; i < TTOOLICON_COUNT; ++i) {
            ImGui::PushID(i);
            if (ImTextureID icon = ToolIcon(i)) { ImGui::Image(icon, ImVec2(s, s)); ImGui::SameLine(); }
            if (ImGui::Selectable(kToolNames[i], i == activeTool) && i != activeTool) {
                const auto newTool = static_cast<BasicTerrain::Tool>(i);
                if (newTool == BasicTerrain::Tool::Paint) {
                    const int lc = tChunked ? (int)tChunked->layers.size() : BasicTerrain::GetLayerCount();
                    if (lc > 1) br.paintLayer = 1;
                    else if (lc > 0) br.paintLayer = 0;
                }
                br.tool = newTool;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    PropertyRow("Shape");
    if (ImGui::RadioButton("Circle", br.shape == BasicTerrain::Shape::Circle)) br.shape = BasicTerrain::Shape::Circle;
    ImGui::SameLine();
    if (ImGui::RadioButton("Square", br.shape == BasicTerrain::Shape::Square)) br.shape = BasicTerrain::Shape::Square;

    Slider("Size", br.radius, 4.0f, 200.0f, "%.0f m", 500.0f);
    Slider("Strength", br.strength, 2.0f, 200.0f, "%.0f", 1000.0f);
    Slider("Hardness", br.hardness, 0.0f, 1.0f, "%.2f");
    // Target height only matters when flattening.
    if (br.tool == BasicTerrain::Tool::Flatten && (tBasic || tChunked)) {
        const float minH = tBasic ? tBasic->GetMinHeight() : tChunked->minHeight;
        const float maxH = tBasic ? tBasic->GetMaxHeight() : tChunked->maxHeight;
        Slider("Target height", br.targetHeight, minH, maxH, "%.0f m");
    }
    if (br.tool == BasicTerrain::Tool::Noise) {
        SeedRow("Seed", br.noiseSeed, 999999);
        Slider("Feature", br.noiseScale, 4.0f, 200.0f, "%.0f m");
    }

    const char* hint = "Click: stamp. Drag: paint. [ ] size. Ctrl inverts.";
    if (br.tool == BasicTerrain::Tool::Paint) hint = "Click: stamp. Drag: paint. [ ] size. Ctrl erases.";
    else if (br.tool == BasicTerrain::Tool::Ramp) hint = "Drag: grade from the start to the cursor. [ ] width.";
    else if (br.tool == BasicTerrain::Tool::Erode) hint = "Click or drag: erode soil. [ ] size.";
    else if (br.tool == BasicTerrain::Tool::Noise) hint = "Click or drag: add noise. [ ] size.";
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Vec(theme::TEXT_DIM));
    ImGui::TextWrapped("%s", hint);
    ImGui::PopStyleColor();

    if (br.tool != BasicTerrain::Tool::Paint) return;
    ImGui::SeparatorText("Layers");
    // Chunked terrain layers don't have static texture access, so they show without thumbnails.
    const int layerCount = tBasic ? BasicTerrain::GetLayerCount() : tChunked ? (int)tChunked->layers.size() : 0;
    Texture2D* layerTexs = tBasic ? BasicTerrain::GetLayerTextures() : nullptr;
    const std::string* layerNames = tBasic ? BasicTerrain::GetLayerNames() : nullptr;
    if (layerCount == 0) {
        ImGui::TextDisabled("No texture layers.");
        return;
    }
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float cell = std::min(48.0f, (ImGui::GetContentRegionAvail().x - gap * 3.0f) / 4.0f);
    for (int i = 0; i < 4; i++) {
        const bool available = i < layerCount;
        const bool active = br.paintLayer == i;
        ImGui::PushID(i);
        ImGui::BeginGroup();
        ImGui::BeginDisabled(!available);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, theme::Vec(theme::ACCENT, 90));
        bool pressed;
        if (available && layerTexs && layerTexs[i].id > 0) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));
            pressed = ImGui::ImageButton("##layer", static_cast<ImTextureID>(GetTextureImGuiId(layerTexs[i])), ImVec2(cell - 4.0f, cell - 4.0f));
            ImGui::PopStyleVar();
        } else {
            pressed = ImGui::Button("##layer", ImVec2(cell, cell));
        }
        if (active) ImGui::PopStyleColor();
        if (pressed) br.paintLayer = i;
        const char* label = layerNames && available ? layerNames[i].c_str() : "Layer";
        if (active) ImGui::TextUnformatted(label);
        else ImGui::TextDisabled("%s", label);
        ImGui::EndDisabled();
        ImGui::EndGroup();
        ImGui::PopID();
        if (i < 3) ImGui::SameLine();
    }
    ImGui::Checkbox("Erase mode", &br.paintErase);
}

void DrawTerrainProperties(BasicTerrain* tBasic, terrain::Terrain* tChunked) {
    ImGui::SeparatorText("Terrain");
    if (!tBasic && !tChunked) {
        ImGui::TextDisabled("No terrain selected.");
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Vec(theme::TEXT_DIM));
        ImGui::TextWrapped("Pick tools above, then click any terrain to edit.");
        ImGui::PopStyleColor();
        return;
    }
    char buf[96];
    if (tBasic) {
        snprintf(buf, sizeof(buf), "%d x %d", tBasic->GetWidth(), tBasic->GetDepth());
        InfoRow("Grid", buf);
        snprintf(buf, sizeof(buf), "%.0f x %.0f m", tBasic->GetWidth() * tBasic->GetScale(), tBasic->GetDepth() * tBasic->GetScale());
        InfoRow("World size", buf);
        snprintf(buf, sizeof(buf), "%.0f, 0, %.0f", tBasic->position.x, tBasic->position.z);
        InfoRow("Position", buf);
        snprintf(buf, sizeof(buf), "%.0f m .. %.0f m", tBasic->GetMinHeight(), tBasic->GetMaxHeight());
        InfoRow("Height range", buf);
        ImGui::Checkbox("Show wireframe", &tBasic->showWireframe);

        // Reset flat wipes every height and all paint, so it asks first.
        static BasicTerrain* resetConfirmFor = nullptr;
        if (resetConfirmFor != tBasic) resetConfirmFor = nullptr;
        if (!resetConfirmFor) {
            if (ImGui::Button("Reset flat")) resetConfirmFor = tBasic;
            ImGui::SameLine();
            if (tBasic->editActive) ImGui::TextColored(theme::Vec(theme::SUCCESS), "Editing: drag to sculpt");
            else ImGui::TextDisabled("Click the terrain to edit it");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::Vec(theme::DANGER_HOVER));
            ImGui::TextWrapped("Erase all sculpting and paint? This can't be undone.");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Button, theme::Vec(theme::DANGER, 150));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::Vec(theme::DANGER));
            if (ImGui::Button("Reset")) { tBasic->GenerateFlat(0.0f); resetConfirmFor = nullptr; }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) resetConfirmFor = nullptr;
        }

        // Size: the sliders (or typed numbers) edit pending values; the terrain resizes once they are let go,
        // because a resize reallocates the grid.
        static BasicTerrain* sizeFor = nullptr;
        static float sizeW = 0.0f, sizeL = 0.0f, sizeH = 0.0f;
        const float curW = tBasic->GetWidth() * tBasic->GetScale();
        const float curL = tBasic->GetDepth() * tBasic->GetScale();
        const float curH = tBasic->GetMaxHeight();
        if (sizeFor != tBasic) { sizeFor = tBasic; sizeW = curW; sizeL = curL; sizeH = curH; }
        ImGui::SeparatorText("Size");
        ImGui::PushID("size");
        Slider("Width", sizeW, 64.0f, 8192.0f, "%.0f m", 100000.0f);
        Slider("Length", sizeL, 64.0f, 8192.0f, "%.0f m", 100000.0f);
        Slider("Height", sizeH, 10.0f, 2000.0f, "%.0f m", 10000.0f);
        ImGui::PopID();
        const float cell = tBasic->GetScale();
        const bool changed = std::fabs(sizeW - curW) >= cell * 0.5f || std::fabs(sizeL - curL) >= cell * 0.5f
                          || std::fabs(sizeH - curH) >= 0.5f;
        if (changed && !ImGui::IsAnyItemActive()) {
            tBasic->Resize(sizeW, sizeL, sizeH);
            sizeW = tBasic->GetWidth() * tBasic->GetScale();
            sizeL = tBasic->GetDepth() * tBasic->GetScale();
            sizeH = tBasic->GetMaxHeight();
        }
        return;
    }
    snprintf(buf, sizeof(buf), "%d x %d chunks", tChunked->gridWidth, tChunked->gridDepth);
    InfoRow("Grid", buf);
    snprintf(buf, sizeof(buf), "%.0f x %.0f m", tChunked->size.x, tChunked->size.z);
    InfoRow("World size", buf);
    snprintf(buf, sizeof(buf), "%.0f, 0, %.0f", tChunked->center.x, tChunked->center.z);
    InfoRow("Position", buf);
    snprintf(buf, sizeof(buf), "%.0f m .. %.0f m", tChunked->minHeight, tChunked->maxHeight);
    InfoRow("Height range", buf);
    ImGui::Checkbox("Show wireframe", &tChunked->showWireframe);
    ImGui::Checkbox("Show chunk bounds", &tChunked->showChunkBounds);
    ImGui::Checkbox("Show LOD colours", &tChunked->showLODColors);
    ImGui::TextColored(theme::Vec(theme::SUCCESS), "Editing: drag to sculpt");
}

} // namespace

void DrawImGuiTerrainPanel() {
    const Rectangle rec = GetTerrainPanelBounds();
    if (rec.width <= 0.0f) return;

    // Prefer an explicitly selected chunked terrain, else the active BasicTerrain.
    BasicTerrain* tBasic = BasicTerrain::GetActive();
    auto& editorState = terrain::GetTerrainEditorState();
    terrain::Terrain* tChunked = editorState.selectedTerrainLegacy;
    const bool useChunked = tChunked != nullptr;
    BasicTerrain::Brush& brBasic = BasicTerrain::GetBrush();
    terrain::TerrainBrush& brChunked = editorState.brush;
    UnifiedBrush br = ReadBrush(useChunked, brChunked, brBasic);

    ImGui::SetNextWindowPos(ImVec2(rec.x, rec.y));
    ImGui::SetNextWindowSize(ImVec2(rec.width, rec.height));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##TerrainPanel", nullptr, flags);
    // The title strip doubles as the layout's drag handle, so it holds no widgets.
    ImGui::TextColored(theme::AccentVec(), "TERRAIN");
    ImGui::Separator();
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), kPanelTitleH));

    ImGui::BeginChild("##TerrainContent", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::BeginTabBar("##TerrainTabs")) {
        const char* tabs[3] = { "Sculpt", "Generate", "Foliage" };
        for (int i = 0; i < 3; ++i) {
            if (ImGui::BeginTabItem(tabs[i])) { g_terrainTab = i; ImGui::EndTabItem(); }
        }
        ImGui::EndTabBar();
    }

    // The region-box preview in the viewport only makes sense while the Generate tab is open for this terrain.
    if (tBasic) tBasic->showGenBox = (g_terrainTab == 1);
    if (g_terrainTab != 2 || !tBasic) foliage::SetPaintTarget(nullptr, -1);   // the foliage brush only lives on its tab
    // Each tab owns its own brush: off the Sculpt tab the sculpt tool is put away (and comes back with the tab), so a
    // click only ever does what the open tab says.
    {
        static BasicTerrain::Tool savedTool = BasicTerrain::Tool::None;
        static bool toolPutAway = false;
        if (g_terrainTab != 0) {
            if (!toolPutAway) { savedTool = brBasic.tool; toolPutAway = true; }
            brBasic.tool = BasicTerrain::Tool::None;
            br.tool = BasicTerrain::Tool::None;   // written back below
        } else if (toolPutAway) {
            brBasic.tool = savedTool;
            br.tool = savedTool;
            toolPutAway = false;
        }
    }

    if (g_terrainTab == 1) DrawGenerateTab(tBasic, tChunked, br, brChunked);
    else if (g_terrainTab == 2) DrawFoliageTab(tBasic, br);
    else DrawSculptTab(tBasic, tChunked, br);

    ImGui::Spacing();
    DrawTerrainProperties(tBasic, tChunked);
    ImGui::EndChild();
    ImGui::End();

    WriteBrush(br, useChunked, brChunked, brBasic);
}

} // namespace ui
