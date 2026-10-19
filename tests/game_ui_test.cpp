// Where in-game UI widgets land on screen. Pure arithmetic, no window. Registered with ctest.
#include "Engine/GameUI.hpp"

#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static bool Is(Rectangle r, float x, float y, float w, float h) {
    return std::fabs(r.x - x) < 1e-3f && std::fabs(r.y - y) < 1e-3f && std::fabs(r.width - w) < 1e-3f && std::fabs(r.height - h) < 1e-3f;
}

int main() {
    using gameui::Anchor;
    const Rectangle screen = { 0, 0, 1280, 720 };
    CHECK(gameui::ScaleFor(720.0f) == 1.0f && gameui::ScaleFor(1440.0f) == 2.0f);

    CHECK(Is(gameui::LayoutRect(Anchor::TopLeft, 10, 20, 100, 50, screen, 1.0f), 10, 20, 100, 50));
    CHECK(Is(gameui::LayoutRect(Anchor::TopRight, -10, 20, 100, 50, screen, 1.0f), 1170, 20, 100, 50));
    CHECK(Is(gameui::LayoutRect(Anchor::Center, 0, 0, 100, 50, screen, 1.0f), 590, 335, 100, 50));
    CHECK(Is(gameui::LayoutRect(Anchor::BottomCenter, 0, -10, 200, 20, screen, 1.0f), 540, 690, 200, 20));
    CHECK(Is(gameui::LayoutRect(Anchor::BottomLeft, 20, -20, 240, 22, screen, 1.0f), 20, 678, 240, 22));
    CHECK(Is(gameui::LayoutRect(Anchor::Fill, 8, 8, 0, 0, screen, 1.0f), 8, 8, 1264, 704));

    // Twice the height, twice the size; an editor viewport that does not start at the window's corner.
    const Rectangle view = { 300, 126, 1000, 500 };
    const float s = gameui::ScaleFor(view.height);
    CHECK(Is(gameui::LayoutRect(Anchor::TopLeft, 10, 10, 100, 50, view, s), 300 + 10 * s, 126 + 10 * s, 100 * s, 50 * s));
    CHECK(Is(gameui::LayoutRect(Anchor::BottomRight, 0, 0, 100, 50, view, s), 1300 - 100 * s, 626 - 50 * s, 100 * s, 50 * s));

    // Widgets: made once per id, replaced when the kind changes, gone after Remove / Clear.
    CHECK(gameui::Create("a", gameui::Kind::Text) && gameui::Create("a", gameui::Kind::Text) && gameui::Count() == 1);
    CHECK(gameui::Create("a", gameui::Kind::Bar) && gameui::Count() == 1);
    CHECK(gameui::Create("b", gameui::Kind::Panel) && gameui::Count() == 2);
    CHECK(!gameui::Create("", gameui::Kind::Panel));
    // Colours: SetColor sets both, SetPrimaryColor leaves the background; a kind change resets the widget.
    gameui::Info info;
    CHECK(!gameui::Get("nope", info));
    CHECK(gameui::Create("c", gameui::Kind::Bar));
    gameui::SetColor("c", { 1, 2, 3, 4 }, { 5, 6, 7, 8 });
    CHECK(gameui::Get("c", info) && info.kind == gameui::Kind::Bar);
    CHECK(info.color.r == 1 && info.color.a == 4 && info.color2.r == 5 && info.color2.a == 8);
    gameui::SetPrimaryColor("c", { 9, 9, 9, 9 });
    CHECK(gameui::Get("c", info) && info.color.r == 9 && info.color2.r == 5 && info.color2.a == 8);
    gameui::SetColor("missing", { 1, 1, 1, 1 }, { 1, 1, 1, 1 });   // ignored
    CHECK(gameui::Create("c", gameui::Kind::Text) && gameui::Get("c", info));
    CHECK(info.kind == gameui::Kind::Text && info.color.r == 255 && info.color2.a == 160);   // back to the defaults
    gameui::Remove("c");

    gameui::Remove("a");
    CHECK(gameui::Count() == 1);
    gameui::SetText("missing", "x", 10);   // unknown ids are ignored
    gameui::Clear();
    CHECK(gameui::Count() == 0);

    std::printf(g_fail ? "game_ui_test: %d FAILED\n" : "game_ui_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
