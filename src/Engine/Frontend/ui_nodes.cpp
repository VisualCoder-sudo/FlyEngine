#include "ui_internal.hpp"
#include "../../../include/Engine/Backend/TextureManager.hpp"
#include "../../../include/Engine/Frontend/ProjectManager.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Graphics/Graphics.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace ui {

namespace {

// Copies a picked image into <project>/assets/NodeTextures/ and returns the project-relative path ("" on failure).
std::string ImportNodeTexture(const std::string& picked, const std::string& projectDir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path src(picked);
    if (projectDir.empty() || !fs::exists(src, ec)) return std::string();
    // Already inside the project: use it where it is.
    const fs::path rel = fs::relative(src, projectDir, ec);
    if (!ec && !rel.empty() && rel.native().rfind("..", 0) != 0) return rel.generic_string();
    const fs::path dir = fs::path(projectDir) / "assets" / "NodeTextures";
    fs::create_directories(dir, ec);
    const fs::path dst = dir / src.filename();
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) return std::string();
    return "assets/NodeTextures/" + src.filename().generic_string();
}

const char* const kFaceNames[] = { "Front", "Back", "Left", "Right", "Top", "Bottom" };

} // namespace

// Settings of the selected child node (light / decal / texture), shown over the properties area.
void DrawImGuiNodePanel() {
    // Pick up a finished texture dialog even while the panel is hidden.
    std::string picked;
    const bool gotPick = platform::PollDialogResult({ platform::DialogPurpose::PickNodeTexture }, picked);

    // The node belongs to an object that must still be selected; a deleted object leaves the selection.
    if (g_selectedNodeObj && std::find(g_selection.begin(), g_selection.end(), g_selectedNodeObj) == g_selection.end()) {
        g_selectedNodeObj = nullptr;
        g_selectedNodeIdx = -1;
    }
    if (g_selectedNodeObj && (g_selectedNodeIdx < 0 || g_selectedNodeIdx >= (int)g_selectedNodeObj->nodes.size())) {
        g_selectedNodeObj = nullptr;
        g_selectedNodeIdx = -1;
    }
    if (!g_selectedNodeObj || g_lightingSel != 0) return;

    ObjectNode& n = g_selectedNodeObj->nodes[(size_t)g_selectedNodeIdx];
    const std::string& projectDir = ::project::GetCurrentProject().path;
    if (gotPick && !picked.empty() && n.IsSurfaceNode()) {
        const std::string rel = ImportNodeTexture(picked, projectDir);
        if (!rel.empty()) n.texturePath = rel;
        else LogAlways("Could not use that image (is a project open?)");
    }

    Rectangle rec = GetPropertiesPanelBounds();
    if (rec.width <= 0.0f) return;
    ImGui::SetNextWindowPos(ImVec2(rec.x, rec.y));
    ImGui::SetNextWindowSize(ImVec2(rec.width, rec.height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##NodeProps", nullptr, flags);
    ImGui::TextColored(theme::AccentVec(), "%s", ObjectNodeTypeName(n.type));
    ImGui::TextDisabled("on %s", g_selectedNodeObj->GetName().c_str());
    ImGui::Separator();

    char nameBuf[128];
    std::snprintf(nameBuf, sizeof nameBuf, "%s", n.name.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##nodename", nameBuf, sizeof nameBuf)) n.name = nameBuf;
    ImGui::Checkbox("Enabled", &n.enabled);

    int face = (int)n.face;
    const char* faceLabel = n.type == ObjectNodeType::PointLight ? nullptr : "Face";
    if (faceLabel) {
        ImGui::TextDisabled(n.IsLight() ? "Points out of" : "Projects onto");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##face", &face, kFaceNames, 6)) n.face = (NodeFace)face;
    }
    ImGui::Spacing();

    if (n.IsLight()) {
        ImGui::ColorEdit3("Colour", n.color, ImGuiColorEditFlags_NoInputs);
        ImGui::TextDisabled("Brightness");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##bright", &n.brightness, 0.0f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::TextDisabled("Range (metres)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##range", &n.range, 0.5f, 200.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
        if (n.type == ObjectNodeType::SpotLight || n.type == ObjectNodeType::SurfaceLight) {
            ImGui::TextDisabled(n.type == ObjectNodeType::SpotLight ? "Cone angle (degrees)" : "Spread angle (degrees)");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::SliderFloat("##angle", &n.angle, 5.0f, 89.0f, "%.0f");
        }
        if (n.type == ObjectNodeType::SurfaceLight) {
            ImGui::TextDisabled("Size (fraction of the face)");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::SliderFloat2("##ssize", n.surfaceSize, 0.05f, 1.0f, "%.2f");
        }
        ImGui::TextDisabled("Falloff");
        int fo = n.falloff == LightFalloff::Linear ? 0 : 1;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##falloff", &fo, "Linear\0Inverse square\0")) n.falloff = fo == 0 ? LightFalloff::Linear : LightFalloff::InverseSquare;
        ImGui::Spacing();
        ImGui::Checkbox("Cast shadows", &n.castShadows);
        if (n.castShadows) {
            ImGui::TextDisabled("Shadow resolution");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::SliderInt("##shres", &n.shadowRes, 128, 4096);
            const gfx::LightingSettings& L = gfx::Lighting();
            ImGui::TextWrapped("At most %d lights cast shadows at once (Lighting > Time).", L.maxShadowCasters);
        }
    } else {
        ImGui::TextDisabled("Image");
        std::string shown = n.texturePath.empty() ? "None" : std::filesystem::path(n.texturePath).filename().string();
        ImGui::TextUnformatted(shown.c_str());
        if (ImGui::Button("Choose file...")) {
            platform::BeginOpenFileDialog(platform::DialogPurpose::PickNodeTexture, "Choose image", projectDir,
                                          { { "Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.webp" } });
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear")) n.texturePath.clear();
        // Presets that ship with the editor.
        static std::vector<std::string> s_presets;
        static bool s_presetsLoaded = false;
        if (!s_presetsLoaded) {
            s_presets = textureManager::GetPresetTextures(std::filesystem::current_path().generic_string());
            s_presetsLoaded = true;
        }
        if (!s_presets.empty()) {
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##preset", "Preset texture...")) {
                for (const std::string& p : s_presets) {
                    if (ImGui::Selectable(std::filesystem::path(p).stem().string().c_str())) {
                        const std::string rel = textureManager::EnsurePresetTextureInProject(p, projectDir);
                        if (!rel.empty()) n.texturePath = rel;
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::Spacing();
        ImGui::ColorEdit3("Tint", n.tint, ImGuiColorEditFlags_NoInputs);
        ImGui::TextDisabled("Transparency");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##ntrans", &n.transparency, 0.0f, 1.0f, "%.2f");
        if (n.type == ObjectNodeType::Texture) {
            ImGui::Spacing();
            ImGui::TextDisabled("Studs per tile (X, Y)");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragFloat2("##spt", n.studsPerTile, 0.05f, 0.05f, 1000.0f, "%.2f"))
                for (float& v : n.studsPerTile) v = std::max(v, 0.05f);
            ImGui::TextDisabled("Offset (X, Y, in tiles)");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat2("##off", n.offset, 0.01f, -100.0f, 100.0f, "%.2f");
            ImGui::TextWrapped("The image repeats across the face and keeps its size when the part is resized.");
        } else {
            ImGui::TextWrapped("The image covers the whole face.");
        }
    }
    ImGui::End();
}

} // namespace ui
