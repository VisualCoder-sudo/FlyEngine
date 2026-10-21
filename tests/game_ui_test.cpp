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

    // Mouse: a button is clicked by pressing and releasing on it; sliders follow the pointer; toggles flip.
    {
        using gameui::Pointer;
        const Rectangle area = { 0, 0, 1280, 720 };
        gameui::Create("ok", gameui::Kind::Button);
        gameui::SetLayout("ok", Anchor::TopLeft, 100, 100, 200, 50);
        gameui::Create("vol", gameui::Kind::Slider);
        gameui::SetLayout("vol", Anchor::TopLeft, 100, 200, 200, 20);
        gameui::Create("opt", gameui::Kind::Toggle);
        gameui::SetLayout("opt", Anchor::TopLeft, 100, 300, 200, 24);
        gameui::Create("backdrop", gameui::Kind::Panel);
        gameui::SetLayout("backdrop", Anchor::Fill, 0, 0, 0, 0);

        Pointer p; p.x = 10; p.y = 10;
        gameui::UpdateInput(area, p);
        CHECK(gameui::StateOf("ok") == 0 && !gameui::PointerOverWidget());
        CHECK(gameui::StateOf("missing") == 0);

        p.x = 150; p.y = 120;                                   // over the button
        gameui::UpdateInput(area, p);
        CHECK(gameui::StateOf("ok") == gameui::kHovered && gameui::PointerOverWidget());
        p.down = p.pressed = true;
        gameui::UpdateInput(area, p);
        CHECK(gameui::StateOf("ok") == (gameui::kHovered | gameui::kHeld));
        p.pressed = false;
        gameui::UpdateInput(area, p);
        CHECK(!(gameui::StateOf("ok") & gameui::kClicked));     // still held
        p.down = false; p.released = true;
        gameui::UpdateInput(area, p);
        CHECK(gameui::StateOf("ok") & gameui::kClicked);        // released over it
        p.released = false;
        gameui::UpdateInput(area, p);
        CHECK(!(gameui::StateOf("ok") & gameui::kClicked));     // only for one update

        // Pressed on the button but released elsewhere: no click.
        p.down = p.pressed = true; p.x = 150; p.y = 120;
        gameui::UpdateInput(area, p);
        p.pressed = false; p.x = 600; p.y = 600;
        gameui::UpdateInput(area, p);
        CHECK(gameui::StateOf("ok") & gameui::kHeld);           // the press stays with the button while dragging
        p.down = false; p.released = true;
        gameui::UpdateInput(area, p);
        CHECK(!(gameui::StateOf("ok") & (gameui::kClicked | gameui::kHeld)));
        p.released = false;

        // Pressed elsewhere, released on the button: no click either.
        p.down = p.pressed = true; p.x = 600; p.y = 600;
        gameui::UpdateInput(area, p);
        p.pressed = false; p.x = 150; p.y = 120;
        gameui::UpdateInput(area, p);
        p.down = false; p.released = true;
        gameui::UpdateInput(area, p);
        CHECK(!(gameui::StateOf("ok") & gameui::kClicked));
        p.released = false;

        // The slider: 0.5 along its 200 px track is x = 200; dragging past the ends clamps.
        p.down = p.pressed = true; p.x = 200; p.y = 210;
        gameui::UpdateInput(area, p);
        gameui::Info info;
        CHECK(gameui::Get("vol", info) && std::fabs(info.value - 0.5f) < 1e-4f && (gameui::StateOf("vol") & gameui::kChanged));
        p.pressed = false; p.x = 1000; p.y = 500;
        gameui::UpdateInput(area, p);
        CHECK(gameui::Get("vol", info) && info.value == 1.0f);
        p.x = -50;
        gameui::UpdateInput(area, p);
        CHECK(gameui::Get("vol", info) && info.value == 0.0f);
        p.down = false; p.released = true;
        gameui::UpdateInput(area, p);
        p.released = false;
        CHECK(!(gameui::StateOf("vol") & gameui::kHeld));

        // The toggle flips on a click and says so for one update.
        p.x = 150; p.y = 310;
        p.down = p.pressed = true;
        gameui::UpdateInput(area, p);
        p.pressed = false; p.down = false; p.released = true;
        gameui::UpdateInput(area, p);
        CHECK(gameui::Get("opt", info) && info.value == 1.0f && (gameui::StateOf("opt") & (gameui::kClicked | gameui::kChanged)));
        p.released = false;
        gameui::UpdateInput(area, p);
        CHECK(!(gameui::StateOf("opt") & gameui::kChanged));
        p.down = p.pressed = true;
        gameui::UpdateInput(area, p);
        p.pressed = false; p.down = false; p.released = true;
        gameui::UpdateInput(area, p);
        CHECK(gameui::Get("opt", info) && info.value == 0.0f);
        p.released = false;

        // A hidden button does not react; an inactive pointer (cursor locked, or over an editor panel) does nothing.
        gameui::SetVisible("ok", false);
        p.x = 150; p.y = 120;
        gameui::UpdateInput(area, p);
        CHECK(gameui::StateOf("ok") == 0);
        gameui::SetVisible("ok", true);
        p.active = false;
        gameui::UpdateInput(area, p);
        CHECK(gameui::StateOf("ok") == 0 && !gameui::PointerOverWidget());
        p.active = true;

        // The topmost widget wins where two overlap, and layouts scale with the area.
        gameui::Create("over", gameui::Kind::Button);
        gameui::SetLayout("over", Anchor::TopLeft, 100, 100, 200, 50);
        gameui::UpdateInput(area, p);
        CHECK(gameui::StateOf("over") & gameui::kHovered && !(gameui::StateOf("ok") & gameui::kHovered));
        const Rectangle big = { 0, 0, 2560, 1440 };
        p.x = 300; p.y = 240;                                   // 150, 120 in UI pixels, doubled
        gameui::UpdateInput(big, p);
        CHECK(gameui::StateOf("over") & gameui::kHovered);
        const Vector2 ui = gameui::PointerInUiPixels();
        CHECK(std::fabs(ui.x - 150.0f) < 1e-3f && std::fabs(ui.y - 120.0f) < 1e-3f);

        // Removing a widget the mouse is holding does not leave a stale grab.
        p.down = p.pressed = true;
        gameui::UpdateInput(big, p);
        gameui::Remove("over");
        p.pressed = false;
        gameui::UpdateInput(big, p);
        p.down = false; p.released = true;
        gameui::UpdateInput(big, p);
        CHECK(gameui::StateOf("over") == 0);

        // Free cursor flag is reset by Clear.
        gameui::SetMouseFree(true);
        CHECK(gameui::MouseFree());
        gameui::Clear();
        CHECK(!gameui::MouseFree() && gameui::Count() == 0);
    }

    std::printf(g_fail ? "game_ui_test: %d FAILED\n" : "game_ui_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
