// "Get started with Flyengine!": a step-by-step tour card that highlights the real editor panels.
#include "ui_internal.hpp"
#include "imgui_internal.h"
#include "../../../include/Engine/Frontend/ProjectManager.hpp"
#include <cmath>

namespace ui {

namespace {

struct Step {
    const char* title;
    const char* body;
    const char* target;   // ImGui window to outline, or nullptr
    void (*enter)();      // runs once when the step is shown, or nullptr
};

void ClosePrefs() { ShowPreferences(false, 0); }
void ShowAssets() { SelectBottomTab(0); ClosePrefs(); }
void ShowPlugins() { ShowPreferences(true, 3); }
void ShowVisualScript() { ClosePrefs(); SelectBottomTab(2); }

const Step kSteps[] = {
    { "Welcome to Flyengine!",
      "This short tour shows the editor using a real project.\n\n"
      "The Explorer on the side is your scene tree: every object lives there. "
      "Fly the camera with the mouse, select things by clicking them, and save from File > Save.\n\n"
      "Use Next to continue, or Skip to leave at any time.",
      "##Explorer", ClosePrefs },
    { "Meshes",
      "Meshes are the 3D models in your world. The Asset Browser at the bottom lists the models "
      "in your project (.obj, .gltf, .glb, .fbx, .iqm, .vox, .m3d). Drag one into the viewport to place it.\n\n"
      "The Import button in the top bar brings in new meshes.",
      "##AssetBrowser", ShowAssets },
    { "Properties",
      "Select an object and its Properties show up: position, rotation, scale, color, textures, "
      "collision and scripts. Change a value and the viewport updates instantly.\n\n"
      "Try selecting a few objects in the Explorer now.",
      "##Explorer", ClosePrefs },
    { "Worldbuilding",
      "Shape your world with the Terrain tools (sculpt, paint, foliage, water) and the City button "
      "in the top bar, which generates streets and buildings.\n\n"
      "The Lighting section controls time of day, sun, sky, fog, weather and clouds.",
      "##LightingProps", ClosePrefs },
    { "Plugins (CPluginAPI)",
      "Plugins extend the editor with new menu items and tools. They are written in plain C "
      "against include/CPluginAPI.h and loaded from your plugins folder.\n\n"
      "Preferences > Plugins lists what is installed and lets you enable or disable each one.",
      "##Preferences", ShowPlugins },
    { "C++ code for your game",
      "Gameplay is C++. Write a script in your project's Scripts folder with the fly SDK "
      "(ScriptingSDK/include/fly.hpp) and mark it with FLY_SCRIPT. The editor compiles it and "
      "reloads it for you.\n\n"
      "See docs/ScriptAPI.md and the sample projects (CoinDash, PhysicsPlayground, CampfireValley).",
      nullptr, ClosePrefs },
    { "Visual Script",
      "Prefer nodes over code? The Visual Script tab in the bottom panel lets you wire up logic "
      "by connecting nodes. It runs alongside your C++ scripts.",
      "##AssetBrowser", ShowVisualScript },
    { "Exporting and distribution",
      "When your game is ready, choose File > Export... Pick a target and Flyengine packs the "
      "player, your scenes, models and textures into one executable you can share.\n\n"
      "Use Play in Player in the top bar to test the packaged game first. That's the tour - have fun!",
      "##TopBar", ClosePrefs },
};

bool g_active = false;
int g_step = 0;
int g_enteredStep = -1;

} // namespace

void OpenTutorial() { g_active = true; g_step = 0; g_enteredStep = -1; }
bool IsTutorialActive() { return g_active; }

void DrawTutorial() {
    if (!g_active) return;
    constexpr int kCount = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));
    const Step& s = kSteps[g_step];
    if (g_enteredStep != g_step) {
        g_enteredStep = g_step;
        if (s.enter) s.enter();
    }

    if (s.target) {
        if (const ImGuiWindow* w = ImGui::FindWindowByName(s.target); w && w->Active) {
            const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f);
            ImVec4 c = theme::AccentVec();
            c.w = 0.55f + 0.45f * pulse;
            ImGui::GetForegroundDrawList()->AddRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y),
                                                    ImGui::ColorConvertFloat4ToU32(c), 0.0f, 0, 3.0f);
        }
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->GetCenter().x, vp->Pos.y + vp->Size.y - 24.0f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_Border, theme::AccentVec(0.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    ImGui::Begin("##Tutorial", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(theme::AccentVec(), "GET STARTED  %d / %d", g_step + 1, kCount);
    ImGui::Separator();
    ImGui::TextUnformatted(s.title);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(s.body);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    bool finish = false;
    if (g_step > 0 && ImGui::Button("Back", ImVec2(80.0f, 26.0f))) --g_step;
    if (g_step > 0) ImGui::SameLine();
    const bool last = g_step == kCount - 1;
    if (ImGui::Button(last ? "Finish" : "Next", ImVec2(80.0f, 26.0f))) {
        if (last) finish = true; else ++g_step;
    }
    if (!last) {
        ImGui::SameLine();
        if (ImGui::Button("Skip", ImVec2(80.0f, 26.0f))) finish = true;
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    if (finish) {
        g_active = false;
        ClosePrefs();
        SelectBottomTab(0);
        project::SetTutorialDone(true);
    }
}

} // namespace ui
