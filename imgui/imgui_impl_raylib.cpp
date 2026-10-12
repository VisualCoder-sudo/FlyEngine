#include "imgui_impl_raylib.h"
#include "imgui_impl_opengl3.h"
#include "raylib.h"
#include "rlgl.h"

// ---------------------------------------------------------------------------
// Internal state & helpers
// ---------------------------------------------------------------------------

namespace {

ImGuiKey ImGuiKeyFromRaylib(int key) {
    switch (key) {
        case KEY_SPACE:         return ImGuiKey_Space;
        case KEY_APOSTROPHE:    return ImGuiKey_Apostrophe;
        case KEY_COMMA:         return ImGuiKey_Comma;
        case KEY_MINUS:         return ImGuiKey_Minus;
        case KEY_PERIOD:        return ImGuiKey_Period;
        case KEY_SLASH:         return ImGuiKey_Slash;
        case KEY_ZERO:          return ImGuiKey_0;
        case KEY_ONE:           return ImGuiKey_1;
        case KEY_TWO:           return ImGuiKey_2;
        case KEY_THREE:         return ImGuiKey_3;
        case KEY_FOUR:          return ImGuiKey_4;
        case KEY_FIVE:          return ImGuiKey_5;
        case KEY_SIX:           return ImGuiKey_6;
        case KEY_SEVEN:         return ImGuiKey_7;
        case KEY_EIGHT:         return ImGuiKey_8;
        case KEY_NINE:          return ImGuiKey_9;
        case KEY_SEMICOLON:     return ImGuiKey_Semicolon;
        case KEY_EQUAL:         return ImGuiKey_Equal;
        case KEY_A:             return ImGuiKey_A;
        case KEY_B:             return ImGuiKey_B;
        case KEY_C:             return ImGuiKey_C;
        case KEY_D:             return ImGuiKey_D;
        case KEY_E:             return ImGuiKey_E;
        case KEY_F:             return ImGuiKey_F;
        case KEY_G:             return ImGuiKey_G;
        case KEY_H:             return ImGuiKey_H;
        case KEY_I:             return ImGuiKey_I;
        case KEY_J:             return ImGuiKey_J;
        case KEY_K:             return ImGuiKey_K;
        case KEY_L:             return ImGuiKey_L;
        case KEY_M:             return ImGuiKey_M;
        case KEY_N:             return ImGuiKey_N;
        case KEY_O:             return ImGuiKey_O;
        case KEY_P:             return ImGuiKey_P;
        case KEY_Q:             return ImGuiKey_Q;
        case KEY_R:             return ImGuiKey_R;
        case KEY_S:             return ImGuiKey_S;
        case KEY_T:             return ImGuiKey_T;
        case KEY_U:             return ImGuiKey_U;
        case KEY_V:             return ImGuiKey_V;
        case KEY_W:             return ImGuiKey_W;
        case KEY_X:             return ImGuiKey_X;
        case KEY_Y:             return ImGuiKey_Y;
        case KEY_Z:             return ImGuiKey_Z;
        case KEY_LEFT_BRACKET:  return ImGuiKey_LeftBracket;
        case KEY_BACKSLASH:     return ImGuiKey_Backslash;
        case KEY_RIGHT_BRACKET: return ImGuiKey_RightBracket;
        case KEY_GRAVE:         return ImGuiKey_GraveAccent;
        case KEY_ESCAPE:        return ImGuiKey_Escape;
        case KEY_ENTER:         return ImGuiKey_Enter;
        case KEY_TAB:           return ImGuiKey_Tab;
        case KEY_BACKSPACE:     return ImGuiKey_Backspace;
        case KEY_INSERT:        return ImGuiKey_Insert;
        case KEY_DELETE:        return ImGuiKey_Delete;
        case KEY_RIGHT:         return ImGuiKey_RightArrow;
        case KEY_LEFT:          return ImGuiKey_LeftArrow;
        case KEY_DOWN:          return ImGuiKey_DownArrow;
        case KEY_UP:            return ImGuiKey_UpArrow;
        case KEY_PAGE_UP:       return ImGuiKey_PageUp;
        case KEY_PAGE_DOWN:     return ImGuiKey_PageDown;
        case KEY_HOME:          return ImGuiKey_Home;
        case KEY_END:           return ImGuiKey_End;
        case KEY_CAPS_LOCK:     return ImGuiKey_CapsLock;
        case KEY_SCROLL_LOCK:   return ImGuiKey_ScrollLock;
        case KEY_NUM_LOCK:      return ImGuiKey_NumLock;
        case KEY_PRINT_SCREEN:  return ImGuiKey_PrintScreen;
        case KEY_PAUSE:         return ImGuiKey_Pause;
        case KEY_F1:            return ImGuiKey_F1;
        case KEY_F2:            return ImGuiKey_F2;
        case KEY_F3:            return ImGuiKey_F3;
        case KEY_F4:            return ImGuiKey_F4;
        case KEY_F5:            return ImGuiKey_F5;
        case KEY_F6:            return ImGuiKey_F6;
        case KEY_F7:            return ImGuiKey_F7;
        case KEY_F8:            return ImGuiKey_F8;
        case KEY_F9:            return ImGuiKey_F9;
        case KEY_F10:           return ImGuiKey_F10;
        case KEY_F11:           return ImGuiKey_F11;
        case KEY_F12:           return ImGuiKey_F12;
        case KEY_KP_0:          return ImGuiKey_Keypad0;
        case KEY_KP_1:          return ImGuiKey_Keypad1;
        case KEY_KP_2:          return ImGuiKey_Keypad2;
        case KEY_KP_3:          return ImGuiKey_Keypad3;
        case KEY_KP_4:          return ImGuiKey_Keypad4;
        case KEY_KP_5:          return ImGuiKey_Keypad5;
        case KEY_KP_6:          return ImGuiKey_Keypad6;
        case KEY_KP_7:          return ImGuiKey_Keypad7;
        case KEY_KP_8:          return ImGuiKey_Keypad8;
        case KEY_KP_9:          return ImGuiKey_Keypad9;
        case KEY_KP_DECIMAL:    return ImGuiKey_KeypadDecimal;
        case KEY_KP_DIVIDE:     return ImGuiKey_KeypadDivide;
        case KEY_KP_MULTIPLY:   return ImGuiKey_KeypadMultiply;
        case KEY_KP_SUBTRACT:   return ImGuiKey_KeypadSubtract;
        case KEY_KP_ADD:        return ImGuiKey_KeypadAdd;
        case KEY_KP_ENTER:      return ImGuiKey_KeypadEnter;
        case KEY_KP_EQUAL:      return ImGuiKey_KeypadEqual;
        case KEY_LEFT_SHIFT:    return ImGuiKey_LeftShift;
        case KEY_LEFT_CONTROL:  return ImGuiKey_LeftCtrl;
        case KEY_LEFT_ALT:      return ImGuiKey_LeftAlt;
        case KEY_LEFT_SUPER:    return ImGuiKey_LeftSuper;
        case KEY_RIGHT_SHIFT:   return ImGuiKey_RightShift;
        case KEY_RIGHT_CONTROL: return ImGuiKey_RightCtrl;
        case KEY_RIGHT_ALT:     return ImGuiKey_RightAlt;
        case KEY_RIGHT_SUPER:   return ImGuiKey_RightSuper;
        default:                return ImGuiKey_None;
    }
}

const char* ImGui_ImplRaylib_GetClipboardText(void* /*user_data*/) {
    return GetClipboardText();
}

void ImGui_ImplRaylib_SetClipboardText(void* /*user_data*/, const char* text) {
    SetClipboardText(text);
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool ImGui_ImplRaylib_Init() {
    if (ImGui::GetCurrentContext() == nullptr)
        return false;

    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "imgui_impl_raylib";

    // NOTE: We deliberately do NOT set ImGuiConfigFlags_NoMouseCursorChange here.
    // The app calls ImGui_ImplRaylib_UpdateMouseCursor() only while the cursor is
    // over an ImGui window, so raylib's own hand/ibeam cursor over the engine
    // panels keeps working. UpdateMouseCursor() still respects the flag if an
    // application decides to set it later.

    io.SetClipboardTextFn = ImGui_ImplRaylib_SetClipboardText;
    io.GetClipboardTextFn = ImGui_ImplRaylib_GetClipboardText;
    io.ClipboardUserData  = nullptr;

    return true;
}

void ImGui_ImplRaylib_Shutdown() {
    ImGuiIO& io = ImGui::GetIO();
    io.SetClipboardTextFn = nullptr;
    io.GetClipboardTextFn = nullptr;
    io.BackendPlatformName = nullptr;
}

void ImGui_ImplRaylib_NewFrame() {
    ImGuiIO& io = ImGui::GetIO();

    io.DeltaTime     = (float)GetFrameTime();
    io.DisplaySize   = ImVec2((float)GetScreenWidth(), (float)GetScreenHeight());

    Vector2 dpiScale = GetWindowScaleDPI();
    io.DisplayFramebufferScale = ImVec2(dpiScale.x, dpiScale.y);

    // Window focus
    io.AddFocusEvent(IsWindowFocused());

    // Mouse position / buttons / wheel
    io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
    io.AddMousePosEvent((float)GetMouseX(), (float)GetMouseY());
    io.AddMouseButtonEvent(0, IsMouseButtonDown(MOUSE_BUTTON_LEFT));
    io.AddMouseButtonEvent(1, IsMouseButtonDown(MOUSE_BUTTON_RIGHT));
    io.AddMouseButtonEvent(2, IsMouseButtonDown(MOUSE_BUTTON_MIDDLE));

    Vector2 wheel = GetMouseWheelMoveV();
    if (wheel.x != 0.0f || wheel.y != 0.0f)
        io.AddMouseWheelEvent(wheel.x, wheel.y);

    // Keyboard state (translated to ImGuiKey)
    // Send merged modifier aliases too (like the GLFW backend does): even if the
    // user holds only the right Ctrl, ImGuiMod_Ctrl must read as pressed so
    // Ctrl+Shortcut() style combos keep working regardless of key side.
    io.AddKeyEvent(ImGuiMod_Ctrl,  IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL));
    io.AddKeyEvent(ImGuiMod_Shift, IsKeyDown(KEY_LEFT_SHIFT)   || IsKeyDown(KEY_RIGHT_SHIFT));
    io.AddKeyEvent(ImGuiMod_Alt,   IsKeyDown(KEY_LEFT_ALT)     || IsKeyDown(KEY_RIGHT_ALT));
    io.AddKeyEvent(ImGuiMod_Super, IsKeyDown(KEY_LEFT_SUPER)   || IsKeyDown(KEY_RIGHT_SUPER));

    for (int key = 32; key <= 348; ++key) {
        ImGuiKey imguiKey = ImGuiKeyFromRaylib(key);
        if (imguiKey != ImGuiKey_None)
            io.AddKeyEvent(imguiKey, IsKeyDown(key));
    }

    // Text input (queued characters since last frame). Delivered as codepoints.
    int c = GetCharPressed();
    while (c > 0) {
        io.AddInputCharacter((unsigned)c);
        c = GetCharPressed();
    }
}

void ImGui_ImplRaylib_RenderDrawData(ImDrawData* draw_data) {
    // Flush everything raylib has queued in its render batch before ImGui draws,
    // otherwise the two would corrupt each other's bound texture/buffer state.
    rlDrawRenderBatchActive();
    ImGui_ImplOpenGL3_RenderDrawData(draw_data);
}

void ImGui_ImplRaylib_UpdateMouseCursor() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)
        return;

    ImGuiMouseCursor cursor = io.MouseDrawCursor ? ImGuiMouseCursor_None : ImGui::GetMouseCursor();
    if (cursor == ImGuiMouseCursor_None)
        return;

    int raylibCursor = MOUSE_CURSOR_DEFAULT;
    switch (cursor) {
        case ImGuiMouseCursor_Arrow:      raylibCursor = MOUSE_CURSOR_ARROW;      break;
        case ImGuiMouseCursor_TextInput:  raylibCursor = MOUSE_CURSOR_IBEAM;      break;
        case ImGuiMouseCursor_ResizeAll:  raylibCursor = MOUSE_CURSOR_RESIZE_ALL; break;
        case ImGuiMouseCursor_ResizeNS:   raylibCursor = MOUSE_CURSOR_RESIZE_NS;  break;
        case ImGuiMouseCursor_ResizeEW:   raylibCursor = MOUSE_CURSOR_RESIZE_EW;  break;
        case ImGuiMouseCursor_ResizeNESW: raylibCursor = MOUSE_CURSOR_RESIZE_NESW; break;
        case ImGuiMouseCursor_ResizeNWSE: raylibCursor = MOUSE_CURSOR_RESIZE_NWSE; break;
        case ImGuiMouseCursor_Hand:       raylibCursor = MOUSE_CURSOR_POINTING_HAND; break;
        case ImGuiMouseCursor_NotAllowed: raylibCursor = MOUSE_CURSOR_NOT_ALLOWED; break;
        default:                          raylibCursor = MOUSE_CURSOR_DEFAULT;   break;
    }
    SetMouseCursor(raylibCursor);
}