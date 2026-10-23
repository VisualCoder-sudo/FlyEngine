// The AI panel against the real editor UI: seeded chat (markdown, code blocks), the growing input box, the chats list.
// Writes ai_panel_*.png into the working directory for a look. Opens a window: build/tests/ai_panel_test
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/AI/AI.hpp"
#include "Engine/Frontend/ui.hpp"
#include "Engine/Frontend/ProjectManager.hpp"
#include <fstream>
#include "Engine/Backend/ScatteredObject.hpp"
#include "Engine/Backend/PhysicsSimulation.hpp"
#include "test_unique.hpp"
#include "imgui.h"
#include <chrono>
#include <cstdio>
#include <thread>
#include <string>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

void flyrl_test_set_mouse(float x, float y, bool leftDown);
namespace ui { void AIAttachForTesting(const std::string& path, bool asImage); }

static Engine* g_engine = nullptr;
static void Frames(int n) { for (int i = 0; i < n; i++) g_engine->StepFrame(1.0f / 60.0f); }
static float g_mx = 0, g_my = 0;
// The editor's ImGui is fed by the windowing events, not raylib's input state, so the mouse goes to ImGui directly.
static void Mouse(float x, float y) { g_mx = x; g_my = y; flyrl_test_set_mouse(x, y, false); ImGui::GetIO().AddMousePosEvent(x, y); }
static void Down(bool d) { flyrl_test_set_mouse(g_mx, g_my, d); ImGui::GetIO().AddMouseButtonEvent(0, d); }
static void Click(float x, float y) { Mouse(x, y); Frames(2); Down(true); Frames(2); Down(false); Frames(3); }
static void Type(const char* s) { for (; *s; ++s) { ImGui::GetIO().AddInputCharacter((unsigned)*s); Frames(1); } }

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / TestUnique("fly_ai_panel");
    fs::remove_all(dir);
    ai::SetConfigPathForTesting((dir / "ai.cfg").string());
    ai::SetChatsDirForTesting((dir / "chats").string());
    ai::GetSettings().enabled = true;
    ai::GetSettings().key[0] = "sk-ant-test-key";
    ai::GetSettings().modeScript = true;               // the Apply bar counts the blocks the + menu allows
    ai::GetSettings().modeBuild = true;
    {   // a cached model list (fresh, so nothing is fetched from the network)
        std::vector<ai::ModelInfo> list;
        const char* names[] = { "Claude Opus 5.5", "Claude Sonnet 5.5", "Claude Haiku 5.5", "Claude Fable 5.1", "Claude Opus 4.1", "Claude Sonnet 4", "Claude Haiku 3.5" };
        const char* ids[] = { "claude-opus-5-5", "claude-sonnet-5-5", "claude-haiku-5-5", "claude-fable-5-1", "claude-opus-4-1", "claude-sonnet-4-0", "claude-3-5-haiku" };
        for (int i = 0; i < 7; ++i) list.push_back({ ids[i], names[i], true, 0 });
        ai::SetModels(ai::ProviderId::Claude, list);
        ai::SetModelsStatus(ai::ProviderId::Claude, "7 models");
    }

    const int W = argc > 1 ? std::atoi(argv[1]) : 1400, H = argc > 2 ? std::atoi(argv[2]) : 900;
    Engine engine(W, H, "ai_panel_test", 60);
    g_engine = &engine;
    ui::Init();
    std::vector<ScatteredObject*> objs;
    std::vector<std::unique_ptr<ModelGroup>> models;
    ui::SetSceneObjects(&objs);
    ui::SetSceneModels(&models);
    auto simOwner = std::make_unique<phys::Simulation>(objs);
    ui::SetSimulation(simOwner.get());
    engine.AddEntity(std::move(simOwner));

    std::vector<ai::Message> msgs = {
        { true, false, "How do I make a terrain with hills, and also add some trees and rocks to it? Please be detailed." },
        { false, false,
          "# Terrain basics\n\nYou can shape the terrain with **brushes** in the terrain panel rather than through console commands or scripts:\n\n"
          "- **Shape:** the Noise brush adds small bumps and roughness, and Erode gives a weathered look.\n"
          "- **Surface:** the Paint brush adds different ground layers, such as `grass`, `dirt` and `rock`.\n"
          "  - nested item with a very_long_unbroken_word_that_does_not_fit_in_the_panel_width_at_all_ok\n"
          "1. First step\n2. Second step with **bold** and `code`\n\n"
          "Try this:\n```fly-console\nCube.Position = (0, 5, 0)\nCube.Scale = (2, 2, 2)\n```\nOr a script:\n"
          "```fly-script:Spinner.cpp\n#include <fly.hpp>\nFLY_SCRIPT(Spinner) { fly::Task Run() { co_return; } };\n```\nDo you want to change the shape, the texture, or add foliage?" },
    };
    ai::SaveChatFile({ "c1", "Terrain with hills and trees", 100 }, msgs);
    ai::SaveChatFile({ "c2", "Another older chat", 50 }, msgs);
    ai::OpenChat("c1");
    Frames(200);                                           // the reply is typed out first
    TakeScreenshot("ai_panel_1_chat.png");

    // The panel's input is at the bottom of the left column's AI share: click near it and type a long line.
    // Apply on the script block, then let the "build" fail the way the editor reports it: the chat shows the errors and offers
    // to send them back. (A real Send would call the API, so the button is only checked on screen.)
    project::Info info; info.name = "AiPanelTest"; info.path = (dir / "proj").string();
    project::SetCurrentProject(info);
    Click(229.0f, 752.0f);                                             // the Apply button of the changes bar
    Frames(3);
    TakeScreenshot("ai_panel_5a_applied.png");
    ui::SetScriptBuildState(ui::ScriptBuildState::Building);
    Frames(8);
    TakeScreenshot("ai_panel_5c_building.png");
    ui::ScriptDiagnostic d;
    d.file = (dir / "proj" / "Scripts" / "Spinner.cpp").string(); d.line = 4; d.message = "'FindByMagic' is not a member of 'fly::Object'";
    ui::SetScriptBuildState(ui::ScriptBuildState::Failed, { d });
    Frames(6);
    TakeScreenshot("ai_panel_5_apply_failed.png");
    Mouse(100.0f, 650.0f);
    for (int i = 0; i < 6; ++i) { ImGui::GetIO().AddMouseWheelEvent(0.0f, -3.0f); Frames(1); }
    TakeScreenshot("ai_panel_5b_send_errors.png");
    std::printf("script written: %d\n", (int)fs::exists(dir / "proj" / "Scripts" / "Spinner.cpp"));
    ai::OpenChat("c1");   // back to a clean view for the shots below
    Frames(6);
    // Scroll the chat back to its top to see the formatted reply.
    Mouse(100.0f, 700.0f);
    for (int i = 0; i < 30; ++i) { ImGui::GetIO().AddMouseWheelEvent(0.0f, 5.0f); Frames(1); }
    TakeScreenshot("ai_panel_1b_top.png");
    for (int i = 0; i < 4; ++i) { ImGui::GetIO().AddMouseWheelEvent(0.0f, -1.0f); Frames(1); }
    TakeScreenshot("ai_panel_1c_mid.png");

    // Default layout: the left column (220 wide) from y=120, the Explorer on top and the AI panel in the lower half.
    const float half = ((float)GetScreenHeight() - 120.0f) * 0.5f;
    std::printf("screen %dx%d (asked %dx%d)\n", GetScreenWidth(), GetScreenHeight(), W, H);
    const Rectangle r = { 0.0f, 120.0f + half, 220.0f, half };
    std::printf("AI panel bounds: %.0f %.0f %.0f %.0f\n", r.x, r.y, r.width, r.height);
    Click(r.x + r.width * 0.5f, r.y + r.height - 48.0f);   // the one-line input, just above the model / Send row
    Type("how do I make a terrain with hills and also add some trees and rocks to it please be detailed and step by step");
    Frames(6);
    std::printf("mouse imgui %.0f,%.0f  wantText=%d active=%d\n", ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y, ImGui::GetIO().WantTextInput, ImGui::IsAnyItemActive());
    TakeScreenshot("ai_panel_2_typing.png");

    for (int i = 0; i < 12; ++i) { ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true); ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true); Frames(1); ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false); ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false); Frames(1); Type("line"); }
    Frames(6);
    TakeScreenshot("ai_panel_3_many_lines.png");

    Click(r.x + 135.0f, r.y + 50.0f);   // the Chats tab
    Frames(6);
    TakeScreenshot("ai_panel_4_chats.png");
    Click(r.x + 45.0f, r.y + 50.0f);    // back to the Chat tab
    Frames(6);
    // Preferences > AI for each provider (each keeps its own key and model).
    ai::GetSettings().key[1] = "sk-openai-test-abcdef"; ai::GetSettings().key[2] = "gemini-key-123";
    ui::ShowPreferences(true, 1);
    for (int i = 0; i < ai::kProviderCount; ++i) {
        ai::GetSettings().provider = static_cast<ai::ProviderId>(i);
        Frames(8);
        TakeScreenshot((std::string("ai_panel_6_prefs_") + std::to_string(i) + ".png").c_str());
    }
    ui::ShowPreferences(false, 0);
    ai::GetSettings().provider = ai::ProviderId::Claude;
    Frames(4);
    Click(60.0f, 830.0f);                                  // the model dropdown in the panel
    Frames(4);
    TakeScreenshot("ai_panel_7_models.png");
    Type("opus");
    Frames(3);
    TakeScreenshot("ai_panel_8_models_filtered.png");
    // A reply typed out, with a question card; then the + menu with a mode on.
    for (int i = 0; i < 2; ++i) { ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); Frames(1); ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false); Frames(2); }   // close the model dropdown
    // Empty the message box the typing steps above filled.
    Click(110.0f, 812.0f);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true); ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true); Frames(2);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false); ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false); Frames(1);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, true); Frames(2); ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, false); Frames(3);
    Mouse(600.0f, 400.0f);
    ai::NewChat();
    ui::ShowPreferences(false, 0);
    Frames(4);
    ai::PushAssistantMessageForTesting("# Terrain\n\nI can shape hills with the **Noise** brush. Before I do:\n\n```fly-question\nWhich terrain type are you using?\n- Chunked `Terrain` (large worlds)\n- Brush-based `BasicTerrain`\n- I am not sure\n```\n");
    Frames(14);
    TakeScreenshot("ai_panel_9_typing_out.png");
    Frames(150);
    TakeScreenshot("ai_panel_10_question.png");
    Frames(3);
    Click(20.0f, 812.0f);                                  // the + button
    Frames(4);
    TakeScreenshot("ai_panel_11_plus_menu.png");
    // Stop: a request that hangs (a stand-in provider, no network), then the Stop button.
    {
        struct Hang final : ai::IProvider {
            const char* Name() const override { return "Hang"; }
            ai::ModelListResult ListModels(const std::string&) override { return {}; }
            ai::ChatResult Chat(const ai::ChatRequest& req, const std::string&) override {
                for (int i = 0; i < 500 && !req.cancel->load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return { false, {}, "Stopped.", false };
            }
        };
        ai::SetProviderFactoryForTesting([](ai::ProviderId) { return std::unique_ptr<ai::IProvider>(new Hang()); });
        ai::GetSettings().modeScript = false; ai::GetSettings().modeBuild = false;
        for (int i = 0; i < 2; ++i) { ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); Frames(1); ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false); Frames(2); }   // close the + menu
        ai::NewChat();
        Frames(4);
        ai::Send("Can you create a new script with revamped logic?", "");
        Frames(40);
        TakeScreenshot("ai_panel_12_working_stop.png");
        Click(233.0f, 832.0f);                             // the Stop button
        Frames(8);
        TakeScreenshot("ai_panel_13_stopped.png");
        ai::SetProviderFactoryForTesting(nullptr);
    }
    // Attachments: a file and an image waiting in the message box, then one that cannot be attached.
    {
        fs::create_directories(dir / "att");
        { std::ofstream f(dir / "att" / "Spinner.cpp"); f << "#include \"fly.hpp\"\nstruct Spinner : fly::Script {};\nFLY_SCRIPT(Spinner)\n"; }
        Image img = GenImageGradientLinear(200, 120, 90, ORANGE, DARKBLUE);
        ExportImage(img, (dir / "att" / "screenshot.png").string().c_str());
        UnloadImage(img);
        { std::ofstream f(dir / "att" / "blob.bin", std::ios::binary); f << std::string("a\0b", 3); }
        ui::AIAttachForTesting((dir / "att" / "Spinner.cpp").string(), false);
        ui::AIAttachForTesting((dir / "att" / "screenshot.png").string(), true);
        ui::AIAttachForTesting((dir / "att" / "blob.bin").string(), false);   // refused: not text
        Frames(6);
        TakeScreenshot("ai_panel_14_attachments.png");
    }
    fs::remove_all(dir);
    std::printf("done\n");
    return 0;
}
