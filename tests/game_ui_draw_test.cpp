// Draws every kind of in-game UI widget (panel, text, bar, button, slider, toggle) over the 3D view and saves a picture to
// game_ui_draw.png in the working directory. Needs a display; not registered with ctest: build/tests/game_ui_draw_test
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/GameUI.hpp"

#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
    Engine engine(1280, 720, "game_ui_draw_test", 60);
    engine.SetPlayerBuild(true);   // no editor panels
    using gameui::Anchor;
    using gameui::Kind;
    gameui::Create("back", Kind::Panel);
    gameui::SetLayout("back", Anchor::Center, 0, 0, 420, 330);
    gameui::Create("title", Kind::Text);
    gameui::SetLayout("title", Anchor::Center, 0, -145, 0, 0);
    gameui::SetText("title", "Settings", 36);
    gameui::Create("play", Kind::Button);
    gameui::SetLayout("play", Anchor::Center, 0, -80, 380, 52);
    gameui::SetText("play", "Play", 24);
    gameui::Create("vol", Kind::Slider);
    gameui::SetLayout("vol", Anchor::Center, 0, 0, 380, 24);
    gameui::SetValue("vol", 0.65f);
    gameui::Create("fs", Kind::Toggle);
    gameui::SetLayout("fs", Anchor::Center, 0, 50, 380, 28);
    gameui::SetText("fs", "Fullscreen", 22);
    gameui::SetValue("fs", 1.0f);
    gameui::Create("mute", Kind::Toggle);
    gameui::SetLayout("mute", Anchor::Center, 0, 95, 380, 28);
    gameui::SetText("mute", "Mute music", 22);
    gameui::Create("hp", Kind::Bar);
    gameui::SetLayout("hp", Anchor::BottomLeft, 20, -20, 240, 22);
    gameui::SetValue("hp", 0.7f);

    for (int f = 0; f < 5; f++) engine.StepFrame(1.0f / 60.0f);
    TakeScreenshot("game_ui_draw.png");
    CHECK(gameui::Count() == 7);
    gameui::Clear();
    std::printf(g_fail ? "game_ui_draw_test: %d FAILED\n" : "game_ui_draw_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
