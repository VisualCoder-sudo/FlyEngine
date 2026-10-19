// The numpad Enter counts as Enter: feeding keypad-Enter window events into the input layer must also set the
// KEY_ENTER state, so every KEY_ENTER check in the editor, player, scripts and plugins accepts both keys.
// No window needed: events are fabricated and handed to the dispatcher directly.
#include "rl_internal.hpp"
#include "sokol_app.h"

#include <cstdio>
#include <cstdlib>

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failed; } } while (0)

static void Key(sapp_event_type type, int key, bool repeat = false) {
    sapp_event ev{};
    ev.type = type;
    ev.key_code = (sapp_keycode)key;
    ev.key_repeat = repeat;
    rli::DispatchEvent(&ev);
}

int main() {
    // Keypad Enter pressed: both codes are down and pressed.
    Key(SAPP_EVENTTYPE_KEY_DOWN, KEY_KP_ENTER);
    CHECK(IsKeyPressed(KEY_KP_ENTER));
    CHECK(IsKeyPressed(KEY_ENTER));
    CHECK(IsKeyDown(KEY_ENTER));

    // Released: both are up and report released.
    Key(SAPP_EVENTTYPE_KEY_UP, KEY_KP_ENTER);
    CHECK(!IsKeyDown(KEY_KP_ENTER));
    CHECK(!IsKeyDown(KEY_ENTER));
    CHECK(IsKeyReleased(KEY_ENTER));

    // Key repeat of the keypad Enter repeats Enter too.
    Key(SAPP_EVENTTYPE_KEY_DOWN, KEY_KP_ENTER, true);
    CHECK(IsKeyPressedRepeat(KEY_ENTER));
    Key(SAPP_EVENTTYPE_KEY_UP, KEY_KP_ENTER);

    // The main Enter still works on its own and does not light the keypad code.
    Key(SAPP_EVENTTYPE_KEY_DOWN, KEY_ENTER);
    CHECK(IsKeyDown(KEY_ENTER));
    CHECK(!IsKeyDown(KEY_KP_ENTER));
    Key(SAPP_EVENTTYPE_KEY_UP, KEY_ENTER);
    CHECK(!IsKeyDown(KEY_ENTER));

    if (g_failed) { std::fprintf(stderr, "numpad_enter_test: %d check(s) failed\n", g_failed); return 1; }
    std::printf("numpad_enter_test: ok\n");
    return 0;
}
