// ui_properties.cpp: the Properties panel, text and number inputs, colour picker, split out of ui.cpp.
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
#include "../../../include/Engine/Graphics.hpp"
#include "../../../include/Engine/PostFX.hpp"
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
// 3. Number Input Helper
// ---------------------------------------------------------------------------
static int GetCursorIndexFromMouse(const char* text, float clickX, float textStartX, float fontSize) {
    auto len = static_cast<int>(strlen(text));
    float bestDist = 1e9f;
    int bestIndex = len;

    for (int i = 0; i <= len; ++i) {
        char sub[32] = { 0 };
        memcpy(sub, text, static_cast<size_t>(i));
        sub[i] = '\0';

        float charX = textStartX + MeasureTextArial(sub, fontSize);
        float dist = fabsf(clickX - charX);

        if (dist < bestDist) {
            bestDist = dist;
            bestIndex = i;
        }
    }
    return bestIndex;
}

static int TextLenCap(int idx, int len) {
    return idx < 0 ? 0 : (idx > len ? len : idx);
}

static void TextEditDeleteSel(char* buf, int& cursor, int& sel) {
    if (sel < 0 || sel == cursor) return;
    int lo = sel < cursor ? sel : cursor;
    int hi = sel > cursor ? sel : cursor;
    auto len = static_cast<int>(strlen(buf));
    int moveLen = len - hi + 1;
    memmove(buf + lo, buf + hi, static_cast<size_t>(moveLen));
    cursor = lo;
    sel = -1;
}

static void TextEditCopy(char* buf, int& cursor, int& sel, bool cut) {
    if (sel < 0 || sel == cursor) return;
    int lo = sel < cursor ? sel : cursor;
    int hi = sel > cursor ? sel : cursor;
    int n = hi - lo;
    if (n <= 0) return;
    char tmp[160];
    if (n >= static_cast<int>(sizeof(tmp))) n = static_cast<int>(sizeof(tmp)) - 1;
    memcpy(tmp, buf + lo, static_cast<size_t>(n));
    tmp[n] = '\0';
    SetClipboardText(tmp);
    if (cut) TextEditDeleteSel(buf, cursor, sel);
}

static int TextEditWordLeft(const char* buf, int pos) {
    if (pos <= 0) return 0;
    int p = pos;
    while (p > 0 && buf[p - 1] == ' ') --p;
    while (p > 0 && buf[p - 1] != ' ') --p;
    return p;
}

static int TextEditWordRight(const char* buf, int pos) {
    auto len = static_cast<int>(strlen(buf));
    if (pos >= len) return len;
    int p = pos;
    while (p < len && buf[p] != ' ') ++p;
    while (p < len && buf[p] == ' ') ++p;
    return p;
}

bool TextEditKeys(char* buf, int bufSize, int& cursor, int& sel, bool& commit) {
    commit = false;
    bool changed = false;
    auto len = static_cast<int>(strlen(buf));
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
        commit = true;
        sel = -1;
        return changed;
    }

    if (ctrl && IsKeyPressed(KEY_A)) {
        cursor = len;
        sel = 0;
        return changed;
    }
    if (ctrl && IsKeyPressed(KEY_C)) {
        TextEditCopy(buf, cursor, sel, false);
        return changed;
    }
    if (ctrl && IsKeyPressed(KEY_X)) {
        TextEditCopy(buf, cursor, sel, true);
        changed = true;
        return changed;
    }
    if (ctrl && IsKeyPressed(KEY_V)) {
        const char* clip = GetClipboardText();
        if (clip && clip[0] != '\0') {
            if (sel >= 0 && sel != cursor) TextEditDeleteSel(buf, cursor, sel);
            len = static_cast<int>(strlen(buf));
            auto clipLen = static_cast<int>(strlen(clip));
            int room = bufSize - 1 - len;
            if (room > 0) {
                if (clipLen > room) clipLen = room;
                int moveLen = len - cursor + 1;
                memmove(buf + cursor + clipLen, buf + cursor, static_cast<size_t>(moveLen));
                memcpy(buf + cursor, clip, static_cast<size_t>(clipLen));
                cursor += clipLen;
                sel = -1;
                changed = true;
            }
        }
        return changed;
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (sel >= 0 && sel != cursor) {
            TextEditDeleteSel(buf, cursor, sel);
        } else if (cursor > 0) {
            int moveLen = len - cursor + 1;
            memmove(buf + cursor - 1, buf + cursor, static_cast<size_t>(moveLen));
            cursor--;
        } else {
            return changed;
        }
        changed = true;
        return changed;
    }
    if (ctrl && IsKeyPressed(KEY_BACKSPACE)) {
        if (sel >= 0 && sel != cursor) {
            TextEditDeleteSel(buf, cursor, sel);
        } else if (cursor > 0) {
            int start = TextEditWordLeft(buf, cursor);
            int moveLen = len - cursor + 1;
            memmove(buf + start, buf + cursor, static_cast<size_t>(moveLen));
            cursor = start;
        } else {
            return changed;
        }
        changed = true;
        return changed;
    }

    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    int oldCursor = cursor;
    bool moved = false;

    if (ctrl && (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT))) {
        cursor = TextEditWordLeft(buf, cursor);
        moved = true;
    } else if (ctrl && (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT))) {
        cursor = TextEditWordRight(buf, cursor);
        moved = true;
    } else if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) {
        if (sel >= 0 && sel != cursor && !shift) cursor = sel < cursor ? sel : cursor;
        else if (cursor > 0) --cursor;
        moved = true;
    } else if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) {
        if (sel >= 0 && sel != cursor && !shift) cursor = sel > cursor ? sel : cursor;
        else if (cursor < len) ++cursor;
        moved = true;
    } else if (IsKeyPressed(KEY_HOME) || IsKeyPressedRepeat(KEY_HOME)) {
        cursor = 0;
        moved = true;
    } else if (IsKeyPressed(KEY_END) || IsKeyPressedRepeat(KEY_END)) {
        cursor = len;
        moved = true;
    }

    if (moved) {
        if (shift) {
            if (sel < 0) sel = oldCursor;
        } else {
            sel = -1;
        }
        if (sel == cursor) sel = -1;
    }

    // ImGui_ImplRaylib_NewFrame() (called once per frame in UpdateInput())
    // already drained raylib's GetCharPressed() queue into ImGui's IO, so a
    // second GetCharPressed() call here would always come back empty. Read
    // the same characters back out of ImGui's buffered queue instead - it's
    // not cleared until the next NewFrame, so it still reflects this frame's
    // typed input. Skip entirely while an ImGui text field (e.g. an Explorer
    // rename box) wants the keyboard, so keystrokes aren't applied twice.
    ImGuiIO& imguiIO = ImGui::GetIO();
    if (!imguiIO.WantTextInput) {
        for (int i = 0; i < imguiIO.InputQueueCharacters.Size; ++i) {
            int key = imguiIO.InputQueueCharacters[i];
            if (key >= 32 && key <= 126) {
                if (sel >= 0 && sel != cursor) {
                    TextEditDeleteSel(buf, cursor, sel);
                    len = static_cast<int>(strlen(buf));
                }
                if (len < bufSize - 1) {
                    int moveLen = len - cursor + 1;
                    memmove(buf + cursor + 1, buf + cursor, static_cast<size_t>(moveLen));
                    buf[cursor] = static_cast<char>(key);
                    cursor++;
                    len++;
                    changed = true;
                }
            }
        }
    }

    return changed;
}

void TextEditMouse(const char* buf, int& cursor, int& sel, Rectangle rec,
                          float textStartX, float fontSize, bool active) {
    if (!active) return;
    Vector2 mouse = GetMousePosition();
    auto len = static_cast<int>(strlen(buf));

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (CheckCollisionPointRec(mouse, rec)) {
            int idx = TextLenCap(GetCursorIndexFromMouse(buf, mouse.x, textStartX, fontSize), len);
            if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
                if (sel < 0) sel = cursor;
                cursor = idx;
            } else {
                cursor = idx;
                sel = idx;
            }
            g_mouseDrag = true;
        }
    } else if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && g_mouseDrag) {
        int idx = TextLenCap(GetCursorIndexFromMouse(buf, mouse.x, textStartX, fontSize), len);
        cursor = idx;
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) g_mouseDrag = false;
    if (sel == cursor) sel = -1;
}

void DrawTextSel(const char* buf, int cursor, int sel, float textStartX,
                        float y, float height, float fontSize, Color color) {
    if (sel < 0 || sel == cursor) return;
    int lo = sel < cursor ? sel : cursor;
    int hi = sel > cursor ? sel : cursor;
    auto len = static_cast<int>(strlen(buf));
    if (lo >= len) return;
    if (hi > len) hi = len;
    char a[192], b[192];
    int al = lo; if (al >= static_cast<int>(sizeof(a))) al = static_cast<int>(sizeof(a)) - 1;
    memcpy(a, buf, static_cast<size_t>(al)); a[al] = '\0';
    int bl = hi; if (bl >= static_cast<int>(sizeof(b))) bl = static_cast<int>(sizeof(b)) - 1;
    memcpy(b, buf, static_cast<size_t>(bl)); b[bl] = '\0';
    float x0 = textStartX + MeasureTextArial(a, fontSize);
    float x1 = textStartX + MeasureTextArial(b, fontSize);
    DrawRectangleRec({ x0, y, x1 - x0, height }, color);
}

// strtof accepts "nan", "inf" and out-of-range numbers ("1e39" is inf). A non-finite value written into a scene
// can't be read back (the loader's >> fails on "nan"), so typed text that isn't a finite number counts as 0.
static float ParseFiniteFloat(const char* text) {
    const float v = std::strtof(text, nullptr);
    return std::isfinite(v) ? v : 0.0f;
}

bool DrawNumberInput(Rectangle rec, FieldID fieldId, float& value, const char* label, float labelX) {
    // One undo step per edited field: the first change after the field was clicked snapshots the scene as it was.
    static FieldID s_undoneField = FIELD_NONE;
    static float s_origValue = 0.0f;
    DrawTextArial(label, labelX, rec.y + 3.0f, 13.0f, theme::TEXT_MUTED);

    bool isActive = (g_activeField == fieldId);
    bool changed = false;
    Vector2 mouse = GetMousePosition();
    bool hovered = CheckCollisionPointRec(mouse, rec);
    float textStartX = rec.x + 6.0f;

    if (isActive) g_activeFieldRect = rec;

    if (isActive && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !hovered) {
        g_activeField = FIELD_NONE;
        g_selStart = -1;
        g_mouseDrag = false;
        isActive = false;
    }

    if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (!isActive) {
            g_activeField = fieldId;
            isActive = true;
            snprintf(g_textBuffer, sizeof(g_textBuffer), "%.2f", value);
            g_cursorPos = static_cast<int>(strlen(g_textBuffer));
            g_selStart = -1;
            s_undoneField = FIELD_NONE;
            s_origValue = value;
        }
    }

    // `value` is the live scene field, so put the original back for the snapshot, then restore the new value.
    auto pushUndoOnce = [&]() {
        if (s_undoneField == fieldId || value == s_origValue) return;
        const float edited = value;
        value = s_origValue;
        ui::PushUndo();
        value = edited;
        s_undoneField = fieldId;
    };

    if (isActive) {
        bool commit = false;
        if (TextEditKeys(g_textBuffer, static_cast<int>(sizeof(g_textBuffer)), g_cursorPos, g_selStart, commit)) {
            if (g_textBuffer[0] != '\0') {
                value = ParseFiniteFloat(g_textBuffer);
                pushUndoOnce();
                changed = true;
            }
        }
        TextEditMouse(g_textBuffer, g_cursorPos, g_selStart, rec, textStartX, 13.0f, true);
        if (commit) {
            value = (g_textBuffer[0] == '\0') ? 0.0f : ParseFiniteFloat(g_textBuffer);
            pushUndoOnce();
            changed = true;
            g_activeField = FIELD_NONE;
            g_selStart = -1;
            g_mouseDrag = false;
            isActive = false;
        }
    }

    float hoverT = HoverProgress(static_cast<uint64_t>(fieldId) << 16U, hovered);
    Color boxBg = isActive ? theme::BG_INPUT : Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, hoverT);
    Color border = isActive ? theme::ACCENT : Mix(theme::BORDER, theme::BORDER_STRONG, hoverT);

    DrawRectangleRounded(rec, 0.15f, 4, boxBg);
    DrawRectangleLinesEx(rec, isActive ? 2.0f : 1.0f, border);

    if (isActive) {
        DrawTextSel(g_textBuffer, g_cursorPos, g_selStart, textStartX, rec.y + 2.0f, rec.height - 4.0f, 13.0f, Fade(theme::ACCENT, 0.35f));
    }

    const char* textToDraw = isActive ? g_textBuffer : TextFormat("%.2f", value);
    DrawTextArial(textToDraw, textStartX, rec.y + 3.0f, 13.0f, isActive ? theme::TEXT : theme::TEXT_MUTED);

    if (hovered) MarkIbeam();

    if (isActive) {
        char sub[32] = { 0 };
        memcpy(sub, g_textBuffer, static_cast<size_t>(g_cursorPos));
        sub[g_cursorPos] = '\0';

        float cursorOffsetX = MeasureTextArial(sub, 13.0f);
        float cursorX = textStartX + cursorOffsetX;

        if (static_cast<int>(GetTime() * 2.5) % 2 == 0) {
            Vector2 p1 = { cursorX, rec.y + 3.0f };
            Vector2 p2 = { cursorX, rec.y + rec.height - 3.0f };
            DrawLineEx(p1, p2, 1.5f, theme::ACCENT);
        }
    }

    return changed;
}

// Draws a number field bound to the primary selection's value and, when the
// user edits it, copies the resulting value to every other selected object so
// multi-select property edits apply to the whole selection.
template <typename Apply>
static void DrawMultiNumberInput(Rectangle rec, FieldID fieldId, float& value, const char* label, float labelX, Apply apply,
                                 float lo = -1.0e6f, float hi = 1.0e6f) {
    float before = value;
    bool changed = DrawNumberInput(rec, fieldId, value, label, labelX);
    // Typed values stay finite and inside [lo, hi] (sizes pass a small positive lo so an object never gets zero size).
    if (changed) value = std::clamp(value, lo, hi);
    if (changed && value != before) {
        for (auto* obj : g_selection) {
            if (obj && obj != g_selectedObject) apply(obj, value);
        }
    }
}

// ---------------------------------------------------------------------------
// 4. Color Picker Pop-up
// ---------------------------------------------------------------------------
void DrawHueBar(float x, float y, float width, float height) {
    constexpr Color rainbow[] = {
        Color{ 255, 0, 0, 255 },     Color{ 255, 255, 0, 255 },
        Color{ 0, 255, 0, 255 },     Color{ 0, 255, 255, 255 },
        Color{ 0, 0, 255, 255 },     Color{ 255, 0, 255, 255 },
        Color{ 255, 0, 0, 255 }
    };

    float segWidth = width / 6.0f;
    for (int i = 0; i < 6; ++i) {
        DrawRectangleGradientH(
            static_cast<int>(x + static_cast<float>(i) * segWidth), static_cast<int>(y),
            static_cast<int>(segWidth + 1.0f), static_cast<int>(height),
            rainbow[i], rainbow[i + 1]
        );
    }
}

static Color g_colorPickerTemp = BLACK; // temp for water body color picker

void DrawColorPickerPopupWindow() {
    if (!g_showColorPickerWindow || (!g_color && !g_selectedWater)) return;

    // Resolve working color pointer: either g_color or water body's base color
    Color* workColor = g_color;
    if (!workColor && g_selectedWater) {
        g_colorPickerTemp = g_selectedWater->GetBaseColor();
        workColor = &g_colorPickerTemp;
    }

    Rectangle popupRec;
    GetColorPickerWindowBounds(popupRec);

    Vector2 mouse = GetMousePosition();

    DrawRectangleRec(popupRec, theme::BG_PANEL);
    DrawRectangleLinesEx(popupRec, 1.0f, theme::BORDER_STRONG);

    DrawRectangleRec({ popupRec.x, popupRec.y, popupRec.width, 26.0f }, theme::BG_TITLE);
    DrawLine(static_cast<int>(popupRec.x), static_cast<int>(popupRec.y + 26.0f), static_cast<int>(popupRec.x + popupRec.width), static_cast<int>(popupRec.y + 26.0f), theme::DIVIDER);
    DrawTextArial("Color Picker", popupRec.x + 10.0f, popupRec.y + 5.0f, 14.0f, theme::TEXT);

    Rectangle closeBtn;
    closeBtn.x = popupRec.x + popupRec.width - 22.0f;
    closeBtn.y = popupRec.y + 3.0f;
    closeBtn.width = 18.0f;
    closeBtn.height = 18.0f;

    bool closeHover = CheckCollisionPointRec(mouse, closeBtn);
    float closeT = HoverProgress(30, closeHover);
    if (closeT > 0.0f) DrawRectangleRounded(closeBtn, 0.2f, 4, Mix(theme::BG_WIDGET, theme::DANGER, closeT));
    if (closeHover) MarkHand();
    if (closeHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g_showColorPickerWindow = false;
        g_colorPickerAnchorY = -1.0f;
        return;
    }
    DrawCenteredTextArial("X", closeBtn, 12.0f, closeHover ? theme::TEXT : theme::TEXT_MUTED);

    float startY = popupRec.y + 36.0f;

    Rectangle previewBox = { popupRec.x + 15.0f, startY, 32.0f, 22.0f };
    DrawRectangleRounded(previewBox, 0.15f, 4, *workColor);
    DrawRectangleLinesEx(previewBox, 1.0f, theme::BORDER_STRONG);
    DrawTextArial(TextFormat("R:%d G:%d B:%d", workColor->r, workColor->g, workColor->b), popupRec.x + 58.0f, startY + 4.0f, 13.0f, theme::TEXT_MUTED);

    Rectangle hueTrack    = { popupRec.x + 65.0f, startY + 32.0f, 195.0f, 14.0f };
    Rectangle satTrack    = { popupRec.x + 65.0f, startY + 58.0f, 195.0f, 14.0f };
    Rectangle brightTrack = { popupRec.x + 65.0f, startY + 84.0f, 195.0f, 14.0f };

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) g_activeSlider = 0;

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (CheckCollisionPointRec(mouse, hueTrack))    g_activeSlider = 1;
        if (CheckCollisionPointRec(mouse, satTrack))    g_activeSlider = 2;
        if (CheckCollisionPointRec(mouse, brightTrack)) g_activeSlider = 3;
    }

    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && g_activeSlider > 0) {
        if (g_activeSlider == 1) {
            float t = (mouse.x - hueTrack.x) / hueTrack.width;
            g_hsv.x = Clamp(t, 0.0f, 1.0f) * 360.0f;
            if (g_hsv.x >= 360.0f) g_hsv.x = 359.9f;
        } else if (g_activeSlider == 2) {
            float t = (mouse.x - satTrack.x) / satTrack.width;
            g_hsv.y = Clamp(t, 0.0f, 1.0f);
        } else if (g_activeSlider == 3) {
            float t = (mouse.x - brightTrack.x) / brightTrack.width;
            g_hsv.z = Clamp(t, 0.0f, 1.0f);
        }

        *workColor = ColorFromHSV(g_hsv.x, g_hsv.y, g_hsv.z);
        if (!g_color && g_selectedWater) {
            uint8_t originalAlpha = g_selectedWater->GetBaseColor().a;
            workColor->a = originalAlpha;
            g_selectedWater->SetBaseColor(*workColor);
        }
    }

    DrawTextArial("Hue", popupRec.x + 15.0f, hueTrack.y + 1.0f, 13.0f, theme::TEXT_MUTED);
    DrawHueBar(hueTrack.x, hueTrack.y, hueTrack.width, hueTrack.height);
    DrawRectangleLinesEx(hueTrack, 1.0f, theme::BORDER);
    float hueHandleX = hueTrack.x + (g_hsv.x / 360.0f) * hueTrack.width;
    DrawRectangle(static_cast<int>(hueHandleX) - 3, static_cast<int>(hueTrack.y) - 2, 6, static_cast<int>(hueTrack.height) + 4, COL_WHITE);
    DrawRectangleLines(static_cast<int>(hueHandleX) - 3, static_cast<int>(hueTrack.y) - 2, 6, static_cast<int>(hueTrack.height) + 4, theme::TEXT_DIM);

    DrawTextArial("Sat", popupRec.x + 15.0f, satTrack.y + 1.0f, 13.0f, theme::TEXT_MUTED);
    Color satMin = ColorFromHSV(g_hsv.x, 0.0f, g_hsv.z);
    Color satMax = ColorFromHSV(g_hsv.x, 1.0f, g_hsv.z);
    DrawRectangleGradientH(static_cast<int>(satTrack.x), static_cast<int>(satTrack.y), static_cast<int>(satTrack.width), static_cast<int>(satTrack.height), satMin, satMax);
    DrawRectangleLinesEx(satTrack, 1.0f, theme::BORDER);
    float satHandleX = satTrack.x + g_hsv.y * satTrack.width;
    DrawRectangle(static_cast<int>(satHandleX) - 3, static_cast<int>(satTrack.y) - 2, 6, static_cast<int>(satTrack.height) + 4, COL_WHITE);
    DrawRectangleLines(static_cast<int>(satHandleX) - 3, static_cast<int>(satTrack.y) - 2, 6, static_cast<int>(satTrack.height) + 4, theme::TEXT_DIM);

    DrawTextArial("Bright", popupRec.x + 15.0f, brightTrack.y + 1.0f, 13.0f, theme::TEXT_MUTED);
    Color brightMin = ColorFromHSV(g_hsv.x, g_hsv.y, 0.0f);
    Color brightMax = ColorFromHSV(g_hsv.x, g_hsv.y, 1.0f);
    DrawRectangleGradientH(static_cast<int>(brightTrack.x), static_cast<int>(brightTrack.y), static_cast<int>(brightTrack.width), static_cast<int>(brightTrack.height), brightMin, brightMax);
    DrawRectangleLinesEx(brightTrack, 1.0f, theme::BORDER);
    float brightHandleX = brightTrack.x + g_hsv.z * brightTrack.width;
    DrawRectangle(static_cast<int>(brightHandleX) - 3, static_cast<int>(brightTrack.y) - 2, 6, static_cast<int>(brightTrack.height) + 4, COL_WHITE);
    DrawRectangleLines(static_cast<int>(brightHandleX) - 3, static_cast<int>(brightTrack.y) - 2, 6, static_cast<int>(brightTrack.height) + 4, theme::TEXT_DIM);

    auto swatchY = static_cast<float>(static_cast<int>(brightTrack.y) + 24);
    DrawTextArial("Presets:", popupRec.x + 15.0f, swatchY + 3.0f, 13.0f, theme::TEXT_MUTED);

    constexpr Color colors[] = { COL_RED, COL_ORANGE, COL_GOLD, COL_LIME, COL_GREEN, COL_BLUE, COL_PURPLE, COL_MAGENTA };
    for (int i = 0; i < 8; ++i) {
        Rectangle colorBtn;
        colorBtn.x = popupRec.x + 65.0f + (static_cast<float>(i) * 25.0f);
        colorBtn.y = swatchY;
        colorBtn.width = 20.0f;
        colorBtn.height = 20.0f;
        DrawRectangleRounded(colorBtn, 0.2f, 4, colors[i]);

        if (workColor->r == colors[i].r && workColor->g == colors[i].g && workColor->b == colors[i].b) {
            DrawRectangleLinesEx(colorBtn, 2.0f, COL_WHITE);
        } else {
            DrawRectangleLinesEx(colorBtn, 1.0f, theme::TEXT_DIM);
        }

        if (CheckCollisionPointRec(mouse, colorBtn)) MarkHand();
        if (CheckCollisionPointRec(mouse, colorBtn) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            *workColor = colors[i];
            g_hsv = ColorToHSV(*workColor);
            if (!g_color && g_selectedWater) g_selectedWater->SetBaseColor(*workColor);
        }
    }

    // Multi-select: while the picker is open the primary's color is mirrored to
    // every other selected object.
    if (g_color && g_selection.size() > 1) {
        for (auto* obj : g_selection) {
            if (obj && obj != g_selectedObject) *obj->GetColorPtr() = *g_color;
        }
    }
}

void DrawCollisionPopup() {
    if (!g_collisionPopupOpen || !g_selectedObject) return;

    Vector2 mouse = GetMousePosition();
    constexpr float itemH = 26.0f;
    constexpr float popupW = 130.0f;
    constexpr int itemCount = 4;

    float x = g_collisionPopupAnchor.x + g_collisionPopupAnchor.width - popupW;
    float y = g_collisionPopupAnchor.y + g_collisionPopupAnchor.height + 2.0f;
    float popupH = static_cast<float>(itemCount) * itemH + 4.0f;
    if (y + popupH > static_cast<float>(GetScreenHeight())) {
        y = g_collisionPopupAnchor.y - popupH - 2.0f;
    }
    Rectangle popupRec = { x, y, popupW, popupH };

    if (IsKeyPressed(KEY_ESCAPE)) {
        g_collisionPopupOpen = false;
        return;
    }
    // The press that opened the popup fires this frame too, so give it a short
    // grace period before treating a click as an outside-click dismissal.
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mouse, popupRec)
        && (GetTime() - g_collisionPopupOpenTime) > 0.15) {
        g_collisionPopupOpen = false;
        return;
    }

    DrawRectangleRec({ popupRec.x + 3.0f, popupRec.y + 3.0f, popupRec.width, popupRec.height }, theme::SHADOW);
    DrawRectangleRounded(popupRec, 0.1f, 4, theme::BG_MENU);
    DrawRectangleLinesEx(popupRec, 1.0f, theme::BORDER_STRONG);

    for (int i = 0; i < itemCount; ++i) {
        auto acc = static_cast<pcoll::CollisionAccuracy>(i);
        Rectangle item = { popupRec.x + 3.0f, popupRec.y + 3.0f + static_cast<float>(i) * itemH,
                           popupRec.width - 6.0f, itemH - 4.0f };
        bool hovered = CheckCollisionPointRec(mouse, item);
        bool current = g_selectedObject->GetCollisionAccuracy() == acc;
        if (hovered || current) DrawDropdownBox(item, true);
        DrawTextArial(pcoll::CollisionAccuracyName(acc), item.x + 10.0f, item.y + 5.0f, 13.0f,
            (hovered || current) ? theme::TEXT : theme::TEXT_MUTED);
        if (hovered) MarkHand();
        if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            g_collisionPopupOpen = false;
            g_selectedObject->SetCollisionAccuracy(acc);
            if (g_selection.size() > 1) {
                for (auto* obj : g_selection) {
                    if (obj && obj != g_selectedObject) obj->SetCollisionAccuracy(acc);
                }
            }
            Log("'%s' collision accuracy = %s", g_selectedObject->GetName().c_str(),
                pcoll::CollisionAccuracyName(acc));
            return;
        }
    }
}

static float DrawGroupHeader(const Rectangle& panelRec, float y, const char* label,
                             bool& open, uint64_t hoverKey) {
    Vector2 mouse = GetMousePosition();
    Rectangle header = { panelRec.x + 10.0f, y, panelRec.width - 20.0f, 24.0f };
    bool hovered = CheckCollisionPointRec(mouse, header);

    DrawLine(static_cast<int>(header.x), static_cast<int>(header.y), static_cast<int>(header.x + header.width), static_cast<int>(header.y), theme::DIVIDER);

    float t = HoverProgress(hoverKey, hovered);
    if (t > 0.0f) {
        DrawRectangleRec(header, Mix(theme::BG_WIDGET, theme::BG_WIDGET_HOVER, t));
    }

    float cx = header.x + 9.0f;
    float cy = header.y + header.height * 0.5f;
    if (open) {
        DrawLineEx({ cx - 4.0f, cy - 2.0f }, { cx, cy + 2.0f }, 1.5f, theme::ACCENT);
        DrawLineEx({ cx, cy + 2.0f }, { cx + 4.0f, cy - 2.0f }, 1.5f, theme::ACCENT);
    } else {
        DrawLineEx({ cx - 2.0f, cy - 4.0f }, { cx + 2.0f, cy }, 1.5f, theme::ACCENT);
        DrawLineEx({ cx + 2.0f, cy }, { cx - 2.0f, cy + 4.0f }, 1.5f, theme::ACCENT);
    }

    DrawTextArial(label, header.x + 20.0f, header.y + 4.0f, 13.0f,
        open ? theme::TEXT : theme::TEXT_MUTED);
    if (hovered) MarkHand();
    if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_clickConsumedThisFrame) open = !open;
    return y + 24.0f;
}

// ---------------------------------------------------------------------------
// 5. Right Panel (Properties Window)
// ---------------------------------------------------------------------------

// Total vertical extent of the properties content, mirroring the layout in
// DrawPropertiesPanel so the scroll offset can be clamped before drawing.
static float PropertiesContentHeight() {
    constexpr float rowHeight = 28.0f;
    constexpr float groupGap = 12.0f;
    auto group = [&](bool open, float rows) {
        return 24.0f + (open ? rows * rowHeight : 0.0f) + groupGap;
    };

    // Water body properties
    if (g_selectedWater) {
        float h = 50.0f;
        h += group(g_positionOpen, 3.0f);   // Pos X, Water H, Pos Z
        h += group(g_sizeOpen, 2.0f);        // Width, Depth
        h += group(g_generalOpen, 3.0f);     // Color swatch, Transparency, Player Collision
        h += group(g_waterNoiseOpen, 4.0f); // Noise: Amplitude, Frequency, Speed, Octaves
        h += group(g_waterFoamOpen, 3.0f);      // Foam: Intensity, Scale, Threshold
        h += group(g_waterDetailOpen, 2.0f);    // Surface detail: Intensity, Scale
        h += group(g_waterReflectOpen, 2.0f);   // Reflections: Strength, Distortion
        h += group(g_waterWakeOpen, 4.0f);      // Wake & spray: Wake, Splash, Foam life, Spray
        h += group(g_waterUnderOpen, 3.0f);     // Underwater: Range, Murk, Darken
        h += 10.0f;
        return h;
    }

    float h = 50.0f; // below the title strip
    h += group(true, 6.0f);               // General (color + anchored + can collide + collision + transparency + mass)
    if (g_selectedObject && g_selectedObject->HasModel()) h += rowHeight; // mesh info line
    if (g_selectedObject && g_selectedObject->GetAnimationCount() > 0) h += rowHeight; // animation row
    h += 2 * rowHeight;                                         // texture label + preset dropdown row
    if (g_selectedObject && g_selectedObject->HasModel() && !g_playActive) h += rowHeight; // browse / clear row
    if (g_selectedObject) h += group(g_waterInteractOpen, 3.0f);  // Water Interactions
    h += group(g_positionOpen, 3.0f);     // Position
    h += group(g_sizeOpen, 3.0f);         // Size
    if (g_rotation) h += group(g_rotationOpen, 3.0f);
    if (g_origin)   h += group(g_originOpen, 3.0f);
    if (g_selectedObject) h += group(g_linearVelocityOpen, 3.0f);
    if (g_selectedObject) h += group(g_angularVelocityOpen, 3.0f);
    h += 10.0f; // bottom padding
    return h;
}

void DrawPropertiesPanel() {
    Rectangle panelRec = GetPropertiesPanelBounds();

    DrawRectangleRec(panelRec, theme::BG_PANEL);
    DrawRectangleLinesEx(panelRec, 2.0f, theme::BORDER);

    DrawRectangleRec({ panelRec.x, panelRec.y, panelRec.width, 36.0f }, theme::BG_TITLE);
    DrawLine(static_cast<int>(panelRec.x), static_cast<int>(panelRec.y + 36.0f), static_cast<int>(panelRec.x + panelRec.width), static_cast<int>(panelRec.y + 36.0f), theme::DIVIDER);

    // Content area (below the title strip) used for scissor clipping + scrolling.
    Rectangle contentRec = { panelRec.x + 2.0f, panelRec.y + 38.0f, panelRec.width - 4.0f, panelRec.height - 40.0f };

if (!g_pos && !g_size && !g_color && !g_selectedWater) {
        DrawTextArial("[Properties]", panelRec.x + (panelRec.width / 2.0f) - 40.0f, panelRec.y + (panelRec.height / 2.0f) - 10.0f, 16.0f, theme::TEXT_DIM);
        return;
    }

    // --- Properties ---
    DrawTextArial("PROPERTIES", panelRec.x + 15.0f, panelRec.y + 10.0f, 14.0f, theme::ACCENT);
if (g_selection.size() > 1) {
        DrawTextArial(TextFormat("(%d selected)", static_cast<int>(g_selection.size())),
            panelRec.x + 15.0f, panelRec.y + 24.0f, 11.0f, theme::TEXT_DIM);
    }

    Vector2 mouse = GetMousePosition();

    float contentHeight = PropertiesContentHeight();
    float maxScroll = contentHeight - contentRec.height;
    if (maxScroll < 0.0f) maxScroll = 0.0f;

    if (CheckCollisionPointRec(mouse, panelRec)) {
        g_propertiesScrollY -= GetMouseWheelMove() * 24.0f;
        if (g_propertiesScrollY < 0.0f) g_propertiesScrollY = 0.0f;
    }
    if (g_propertiesScrollY > maxScroll) g_propertiesScrollY = maxScroll;

    constexpr float rowHeight = 28.0f;
    constexpr float inputWidth = 125.0f;
    constexpr float inputHeight = 22.0f;
    constexpr float groupGap = 12.0f;
    float inputX = panelRec.x + 95.0f;
    float labelX = panelRec.x + 15.0f;
    float generalControlX = inputX + inputWidth - 22.0f;

    float y = panelRec.y + 50.0f - g_propertiesScrollY;
    bool live = g_playActive && g_simulation;

    BeginScissorMode(static_cast<int>(contentRec.x), static_cast<int>(contentRec.y), static_cast<int>(contentRec.width), static_cast<int>(contentRec.height));

    // --- Water body properties ---
    if (g_selectedWater) {
        WaterBody* w = g_selectedWater;

        DrawTextArial("WATER BODY", panelRec.x + 15.0f, panelRec.y + 10.0f, 14.0f, theme::ACCENT);

        // Position
        y = DrawGroupHeader(panelRec, y, "Position", g_positionOpen, 100);
        if (g_positionOpen) {
            Vector3 wp = *w->GetPosPtr();
            float valX = wp.x;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_POS_X, valX, "Pos X:", labelX)) { wp.x = valX; *w->GetPosPtr() = wp; }
            y += rowHeight;
            float wh = w->GetWaterHeight();
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_WATER_H, wh, "Water H:", labelX)) w->SetWaterHeight(wh);
            y += rowHeight;
            float valZ = wp.z;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_POS_Z, valZ, "Pos Z:", labelX)) { wp.z = valZ; *w->GetPosPtr() = wp; }
            y += rowHeight;
        }
        y += groupGap;

        // Size
        y = DrawGroupHeader(panelRec, y, "Size", g_sizeOpen, 101);
        if (g_sizeOpen) {
            Vector3 ws = *w->GetSizePtr();
            float valW = ws.x;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_SIZE_W, valW, "Width:", labelX)) { ws.x = fmaxf(1.0f, valW); *w->GetSizePtr() = ws; w->MarkMeshDirty(); }
            y += rowHeight;
            float valD = ws.z;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_SIZE_D, valD, "Depth:", labelX)) { ws.z = fmaxf(1.0f, valD); *w->GetSizePtr() = ws; w->MarkMeshDirty(); }
            y += rowHeight;
        }
        y += groupGap;

        // Appearance (color + transparency)
        y = DrawGroupHeader(panelRec, y, "Appearance", g_generalOpen, 105);
        if (g_generalOpen) {
            Color baseCol = w->GetBaseColor();
            DrawTextArial("Color:", labelX, y + 3.0f, 13.0f, theme::TEXT_MUTED);
            Rectangle colorSwatchBtn = { generalControlX, y, 22.0f, 22.0f };
            bool swatchHovered = CheckCollisionPointRec(mouse, colorSwatchBtn);
            float swatchT = HoverProgress(20, swatchHovered);
            DrawRectangleRounded(colorSwatchBtn, 0.2f, 4, baseCol);
            DrawRectangleLinesEx(colorSwatchBtn, swatchHovered ? 2.0f : 1.0f, Mix(theme::BORDER_STRONG, theme::ACCENT_HOVER, swatchT));
            if (swatchHovered) MarkHand();
            if (swatchHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                g_hsv = ColorToHSV(baseCol);
                g_showColorPickerWindow = !g_showColorPickerWindow;
            }
            y += rowHeight;

            float trans = w->GetTransparency();
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_TRANSPARENCY, trans, "Transparency:", labelX)) w->SetTransparency(trans);
            y += rowHeight;

            // Player Collision: on = the player floats and swims; off = falls through.
            DrawTextArial("Player Collision:", labelX, y + 3.0f, 13.0f, theme::TEXT_MUTED);
            {
                Rectangle check = { generalControlX, y, 22.0f, 22.0f };
                bool on = w->GetPlayerCollision();
                bool hovered = CheckCollisionPointRec(mouse, check);
                float t = HoverProgress(29, hovered);
                DrawRectangleRounded(check, 0.2f, 4, Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, t));
                DrawRectangleLinesEx(check, hovered ? 2.0f : 1.0f, hovered ? theme::ACCENT : theme::BORDER);
                if (on) {
                    Vector2 c1 = { check.x + 3.0f, check.y + check.height * 0.5f };
                    Vector2 c2 = { check.x + check.width * 0.5f, check.y + check.height - 5.0f };
                    Vector2 c3 = { check.x + check.width - 3.0f, check.y + 4.0f };
                    DrawLineEx(c1, c2, 2.5f, theme::SUCCESS);
                    DrawLineEx(c2, c3, 2.5f, theme::SUCCESS);
                }
                if (hovered) MarkHand();
                if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    w->SetPlayerCollision(!on);
                    Log("'%s' player collision = %s", w->GetName().c_str(), !on ? "true" : "false");
                }
            }
            y += rowHeight;
        }
        y += groupGap;

        // Noise
        y = DrawGroupHeader(panelRec, y, "Noise", g_waterNoiseOpen, 104);
        if (g_waterNoiseOpen) {
            WaterBody::NoiseParams n = w->GetNoiseParams();
            float valA = n.amplitude;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_AMPLITUDE, valA, "Amplitude:", labelX)) { n.amplitude = valA; w->SetNoiseParams(n); }
            y += rowHeight;
            float valF = n.frequency;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_FREQUENCY, valF, "Frequency:", labelX)) { n.frequency = valF; w->SetNoiseParams(n); }
            y += rowHeight;
            float valS = n.speed;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_SPEED, valS, "Speed:", labelX)) { n.speed = valS; w->SetNoiseParams(n); }
            y += rowHeight;
            float valO = (float)n.octaves;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_OCTAVES, valO, "Octaves:", labelX)) { n.octaves = (int)valO; w->SetNoiseParams(n); }
            y += rowHeight;
        }
        y += groupGap;

        // Foam
        y = DrawGroupHeader(panelRec, y, "Foam", g_waterFoamOpen, 106);
        if (g_waterFoamOpen) {
            WaterBody::FoamParams f = w->GetFoamParams();
            float valI = f.intensity;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_FOAM_INTENSITY, valI, "Intensity:", labelX)) { f.intensity = valI; w->SetFoamParams(f); }
            y += rowHeight;
            float valFS = f.scale;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_FOAM_SCALE, valFS, "Scale:", labelX)) { f.scale = valFS; w->SetFoamParams(f); }
            y += rowHeight;
            float valFT = f.threshold;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_FOAM_THRESHOLD, valFT, "Threshold:", labelX)) { f.threshold = valFT; w->SetFoamParams(f); }
            y += rowHeight;
        }
        y += groupGap;

        // Surface detail (capillary droplets/ripples)
        y = DrawGroupHeader(panelRec, y, "Surface detail", g_waterDetailOpen, 107);
        if (g_waterDetailOpen) {
            WaterBody::DetailParams d = w->GetDetailParams();
            float valDI = d.intensity;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_DETAIL_INTENSITY, valDI, "Intensity:", labelX)) { d.intensity = valDI; w->SetDetailParams(d); }
            y += rowHeight;
            float valDS = d.scale;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_DETAIL_SCALE, valDS, "Scale:", labelX)) { d.scale = valDS; w->SetDetailParams(d); }
            y += rowHeight;
        }
        y += groupGap;

        // Reflections (planar mirror of the world around this water)
        y = DrawGroupHeader(panelRec, y, "Reflections", g_waterReflectOpen, 108);
        if (g_waterReflectOpen) {
            WaterBody::ReflectionParams r = w->GetReflectionParams();
            float valRS = r.strength;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_REFLECT_STRENGTH, valRS, "Strength:", labelX)) { r.strength = valRS; w->SetReflectionParams(r); }
            y += rowHeight;
            float valRD = r.distortion;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_REFLECT_DISTORTION, valRD, "Distortion:", labelX)) { r.distortion = valRD; w->SetReflectionParams(r); }
            y += rowHeight;
        }
        y += groupGap;

        // Underwater (what the camera sees inside this water)
        y = DrawGroupHeader(panelRec, y, "Underwater", g_waterUnderOpen, 111);
        if (g_waterUnderOpen) {
            WaterBody::UnderwaterParams u = w->GetUnderwaterParams();
            float valUR = u.range;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_UNDER_RANGE, valUR, "Range (m):", labelX)) { u.range = std::max(0.0f, valUR); w->SetUnderwaterParams(u); }
            y += rowHeight;
            float valUM = u.murk;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_UNDER_MURK, valUM, "Murk:", labelX)) { u.murk = std::max(0.0f, valUM); w->SetUnderwaterParams(u); }
            y += rowHeight;
            float valUD = u.darken;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_UNDER_DARKEN, valUD, "Darken:", labelX)) { u.darken = std::clamp(valUD, 0.0f, 1.0f); w->SetUnderwaterParams(u); }
            y += rowHeight;
        }
        y += groupGap;

        // Wake & spray (dynamic water: boat wakes, splashes, foam trails, droplets)
        y = DrawGroupHeader(panelRec, y, "Wake & spray", g_waterWakeOpen, 109);
        if (g_waterWakeOpen) {
            WaterBody::RippleParams rp = w->GetRippleParams();
            float valWK = rp.wakeStrength;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_WAKE_STRENGTH, valWK, "Wake:", labelX)) { rp.wakeStrength = std::max(0.0f, valWK); w->SetRippleParams(rp); }
            y += rowHeight;
            float valSP = rp.splashStrength;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_SPLASH_STRENGTH, valSP, "Splash:", labelX)) { rp.splashStrength = std::max(0.0f, valSP); w->SetRippleParams(rp); }
            y += rowHeight;
            float valFL = rp.foamLifetime;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_FOAM_LIFE, valFL, "Foam life:", labelX)) { rp.foamLifetime = std::clamp(valFL, 0.2f, 60.0f); w->SetRippleParams(rp); }
            y += rowHeight;
            // Spray amount: 0 turns droplets off.
            float valSA = rp.spray ? rp.sprayAmount : 0.0f;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_SPRAY_AMOUNT, valSA, "Spray:", labelX)) {
                rp.sprayAmount = std::clamp(valSA, 0.0f, 5.0f);
                rp.spray = rp.sprayAmount > 0.0f;
                w->SetRippleParams(rp);
            }
            y += rowHeight;
        }
        y += groupGap;

        EndScissorMode();
        return;
    }

    // --- General (color + anchored) ---
    y = DrawGroupHeader(panelRec, y, "General", g_generalOpen, 105);
    if (g_generalOpen) {
        DrawTextArial("Color:", labelX, y + 3.0f, 13.0f, theme::TEXT_MUTED);

        Rectangle colorSwatchBtn = { generalControlX, y, 22.0f, 22.0f };

        bool swatchHovered = CheckCollisionPointRec(mouse, colorSwatchBtn);
        float swatchT = HoverProgress(20, swatchHovered);

        DrawRectangleRounded(colorSwatchBtn, 0.2f, 4, *g_color);
        DrawRectangleLinesEx(colorSwatchBtn, swatchHovered ? 2.0f : 1.0f, Mix(theme::BORDER_STRONG, theme::ACCENT_HOVER, swatchT));
        if (swatchHovered) MarkHand();

        if (!g_playActive && swatchHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            g_showColorPickerWindow = !g_showColorPickerWindow;
        }
        y += rowHeight;

        DrawTextArial("Anchored:", labelX, y + 3.0f, 13.0f, theme::TEXT_MUTED);

        Rectangle anchoredCheck = { generalControlX, y, 22.0f, 22.0f };

        bool anchored = g_selectedObject != nullptr && g_selectedObject->anchored;
        bool anchoredHovered = CheckCollisionPointRec(mouse, anchoredCheck);
        bool anchoredDisabled = g_selectedObject == nullptr;
        float anchorT = HoverProgress(21, anchoredHovered && !anchoredDisabled);

        Color checkBg = anchoredDisabled ? theme::BG_WIDGET
            : Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, anchorT);
        DrawRectangleRounded(anchoredCheck, 0.2f, 4, checkBg);
        DrawRectangleLinesEx(anchoredCheck, anchoredHovered ? 2.0f : 1.0f,
            anchoredHovered ? theme::ACCENT : theme::BORDER);

        if (anchored) {
            Color checkColor = anchoredDisabled ? theme::TEXT_DIM : theme::SUCCESS;
            Vector2 c1 = { anchoredCheck.x + 3.0f, anchoredCheck.y + anchoredCheck.height * 0.5f };
            Vector2 c2 = { anchoredCheck.x + anchoredCheck.width * 0.5f, anchoredCheck.y + anchoredCheck.height - 5.0f };
            Vector2 c3 = { anchoredCheck.x + anchoredCheck.width - 3.0f, anchoredCheck.y + 4.0f };
            DrawLineEx(c1, c2, 2.5f, checkColor);
            DrawLineEx(c2, c3, 2.5f, checkColor);
        }
        if (anchoredHovered && !anchoredDisabled) MarkHand();

        if (!anchoredDisabled && anchoredHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            bool newValue = !g_selectedObject->anchored;
            g_selectedObject->anchored = newValue;
            if (g_selection.size() > 1) {
                for (auto* obj : g_selection) {
                    if (obj && obj != g_selectedObject) obj->anchored = newValue;
                }
            }
            Log("'%s' anchored = %s", g_selectedObject->GetName().c_str(),
                g_selectedObject->anchored ? "true" : "false");
        }

        // --- Can Collide ---
        y += rowHeight;
        DrawTextArial("Can Collide:", labelX, y + 3.0f, 13.0f, theme::TEXT_MUTED);

        Rectangle canCollideCheck = { generalControlX, y, 22.0f, 22.0f };

        bool canCollide = g_selectedObject != nullptr && g_selectedObject->canCollide;
        bool canCollideHovered = CheckCollisionPointRec(mouse, canCollideCheck);
        bool canCollideDisabled = g_selectedObject == nullptr;
        float canCollideT = HoverProgress(24, canCollideHovered && !canCollideDisabled);

        Color ccBg = canCollideDisabled ? theme::BG_WIDGET
            : Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, canCollideT);
        DrawRectangleRounded(canCollideCheck, 0.2f, 4, ccBg);
        DrawRectangleLinesEx(canCollideCheck, canCollideHovered ? 2.0f : 1.0f,
            canCollideHovered ? theme::ACCENT : theme::BORDER);

        if (canCollide) {
            Color checkColor = canCollideDisabled ? theme::TEXT_DIM : theme::SUCCESS;
            Vector2 c1 = { canCollideCheck.x + 3.0f, canCollideCheck.y + canCollideCheck.height * 0.5f };
            Vector2 c2 = { canCollideCheck.x + canCollideCheck.width * 0.5f, canCollideCheck.y + canCollideCheck.height - 5.0f };
            Vector2 c3 = { canCollideCheck.x + canCollideCheck.width - 3.0f, canCollideCheck.y + 4.0f };
            DrawLineEx(c1, c2, 2.5f, checkColor);
            DrawLineEx(c2, c3, 2.5f, checkColor);
        }
        if (canCollideHovered && !canCollideDisabled) MarkHand();

        if (!canCollideDisabled && canCollideHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            bool newValue = !g_selectedObject->canCollide;
            g_selectedObject->canCollide = newValue;
            if (g_selection.size() > 1) {
                for (auto* obj : g_selection) {
                    if (obj && obj != g_selectedObject) obj->canCollide = newValue;
                }
            }
            Log("'%s' canCollide = %s", g_selectedObject->GetName().c_str(),
                g_selectedObject->canCollide ? "true" : "false");
        }

        // --- Collision accuracy (combo) ---
        y += rowHeight;
        DrawTextArial("Collision:", labelX, y + 3.0f, 13.0f, theme::TEXT_MUTED);

        bool accSupported = g_selectedObject && g_selectedObject->IsCollisionAccuracySupported();
        pcoll::CollisionAccuracy acc = g_selectedObject
            ? g_selectedObject->GetCollisionAccuracy() : pcoll::CollisionAccuracy::Default;

        Rectangle accCombo = { inputX, y, inputWidth, inputHeight };
        bool accHovered = CheckCollisionPointRec(mouse, accCombo);
        float accT = HoverProgress(25, accHovered && accSupported);
        (void)accT;
        if (accSupported) DrawDropdownBox(accCombo, (accHovered && accSupported) || g_collisionPopupOpen);
        else {
            DrawRectangleRounded(accCombo, 0.2f, 4, theme::BG_WIDGET);
            DrawRectangleLinesEx(accCombo, 1.0f, theme::BORDER);
        }
        DrawTextArial(pcoll::CollisionAccuracyName(acc), inputX + 8.0f, y + 4.0f, 13.0f,
            accSupported ? theme::TEXT : theme::TEXT_MUTED);
        if (accSupported) {
            float ax = inputX + inputWidth - 14.0f;
            float ay = y + inputHeight * 0.5f;
            DrawLineEx({ ax - 3.0f, ay - 1.5f }, { ax, ay + 1.5f }, 1.5f, theme::TEXT_DIM);
            DrawLineEx({ ax, ay + 1.5f }, { ax + 3.0f, ay - 1.5f }, 1.5f, theme::TEXT_DIM);
            if (accHovered) MarkHand();
            if (accHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                g_collisionPopupOpen = !g_collisionPopupOpen;
                if (g_collisionPopupOpen) g_collisionPopupOpenTime = GetTime();
                g_collisionPopupAnchor = accCombo;
            }
        }

        // --- Transparency (0 = visible, 1 = invisible) ---
        if (g_selectedObject) {
            y += rowHeight;

            float transparencyValue = g_selectedObject->GetTransparency();
            float transparencyBefore = transparencyValue;
            bool transparencyEdited = DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_TRANSPARENCY, transparencyValue, "Transparency:", labelX);

            if (transparencyEdited && transparencyValue != transparencyBefore) {
                transparencyValue = transparencyValue < 0.0f ? 0.0f : (transparencyValue > 1.0f ? 1.0f : transparencyValue);
                g_selectedObject->SetTransparency(transparencyValue);
                if (g_selection.size() > 1) {
                    for (auto* obj : g_selection) {
                        if (obj && obj != g_selectedObject) obj->SetTransparency(transparencyValue);
                    }
                }
                Log("'%s' transparency = %.2f", g_selectedObject->GetName().c_str(), transparencyValue);
            }
        }

        // --- Mass (auto from size; manual override; reset to auto when scaled) ---
        if (g_selectedObject) {
            y += rowHeight;

            float massValue = g_selectedObject->GetMass();
            float massBefore = massValue;
            bool massAuto = g_selectedObject->IsMassAuto();
            bool massEdited = DrawNumberInput({ inputX, y, inputWidth - 30.0f, inputHeight }, FIELD_MASS, massValue, "Mass:", labelX);

            Rectangle massCheck = { generalControlX, y, 22.0f, 22.0f };
            bool massHovered = CheckCollisionPointRec(mouse, massCheck);
            float massT = HoverProgress(23, massHovered);
            Color massBg = massAuto
                ? Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, massT)
                : Mix(theme::BG_WIDGET, theme::BG_WIDGET, massT);
            DrawRectangleRounded(massCheck, 0.2f, 4, massBg);
            DrawRectangleLinesEx(massCheck, massHovered ? 2.0f : 1.0f,
                massHovered ? theme::ACCENT : theme::BORDER);
            if (massAuto) {
                Color checkColor = theme::SUCCESS;
                Vector2 c1 = { massCheck.x + 3.0f, massCheck.y + massCheck.height * 0.5f };
                Vector2 c2 = { massCheck.x + massCheck.width * 0.5f, massCheck.y + massCheck.height - 5.0f };
                Vector2 c3 = { massCheck.x + massCheck.width - 3.0f, massCheck.y + 4.0f };
                DrawLineEx(c1, c2, 2.5f, checkColor);
                DrawLineEx(c2, c3, 2.5f, checkColor);
            }
            if (massHovered) MarkHand();

            if (massHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (massAuto) g_selectedObject->SetMass(g_selectedObject->GetMass());
                else g_selectedObject->ResetMassAuto();
                if (g_selection.size() > 1) {
                    bool makeManual = !massAuto;
                    for (auto* obj : g_selection) {
                        if (!obj || obj == g_selectedObject) continue;
                        if (makeManual) obj->SetMass(obj->GetMass());
                        else obj->ResetMassAuto();
                    }
                }
                Log("'%s' mass = %s (%.3f)", g_selectedObject->GetName().c_str(),
                    g_selectedObject->IsMassAuto() ? "auto" : "manual", g_selectedObject->GetMass());
            }

            if (massValue != g_selectedObject->GetMass()) {
                g_selectedObject->SetMass(massValue);
            }
            if (massEdited && massValue != massBefore && g_selection.size() > 1) {
                for (auto* obj : g_selection) {
                    if (obj && obj != g_selectedObject) obj->SetMass(massValue);
                }
            }
        }

        // --- Mesh (read-only info for imported models) ---
        if (g_selectedObject && g_selectedObject->HasModel()) {
            y += rowHeight;
            std::string meshName = g_selectedObject->GetModelPath();
            size_t slash = meshName.find_last_of("/\\");
            if (slash != std::string::npos) meshName = meshName.substr(slash + 1);
            DrawTextArial("Mesh:", labelX, y + 6.0f, 14.0f, theme::TEXT_MUTED);
            DrawTextArial(FitTextToWidth(meshName, 13.0f, inputWidth).c_str(), inputX, y + 6.0f, 13.0f, theme::TEXT);
        }

        // --- Animation (imported models that carry skeletal clips) ---
        if (g_selectedObject && g_selectedObject->GetAnimationCount() > 0) {
            y += rowHeight;
            DrawTextArial("Anim:", labelX, y + 6.0f, 14.0f, theme::TEXT_MUTED);
            const int clipCount = g_selectedObject->GetAnimationCount();
            const float playW = 24.0f, gap = 4.0f;
            // Clip button: click = next clip, right click = previous.
            const Rectangle clipBtn = { inputX, y + 2.0f, inputWidth - playW - gap, 22.0f };
            const Rectangle playBtn = { inputX + inputWidth - playW, y + 2.0f, playW, 22.0f };

            const bool clipHovered = CheckCollisionPointRec(mouse, clipBtn);
            const float clipT = HoverProgress(210, clipHovered);
            DrawRectangleRounded(clipBtn, 0.2f, 4, Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, clipT));
            DrawRectangleLinesEx(clipBtn, clipHovered ? 2.0f : 1.0f, clipHovered ? theme::ACCENT : theme::BORDER);
            if (clipHovered) MarkHand();
            int cur = g_selectedObject->GetCurrentAnimation();
            if (cur < 0) cur = 0;
            const std::string clipLabel = std::to_string(cur + 1) + "/" + std::to_string(clipCount) + "  " +
                (*g_selectedObject->GetAnimationName(cur) ? g_selectedObject->GetAnimationName(cur) : "(unnamed)");
            DrawTextArial(FitTextToWidth(clipLabel, 12.0f, clipBtn.width - 12.0f).c_str(), clipBtn.x + 6.0f, clipBtn.y + 4.0f, 12.0f, theme::TEXT);
            if (clipHovered && clipCount > 1) {
                int next = -1;
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) next = (cur + 1) % clipCount;
                else if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) next = (cur + clipCount - 1) % clipCount;
                if (next >= 0) {
                    g_selectedObject->PlayAnimation(next, g_selectedObject->animLoop, g_selectedObject->animSpeed);
                    Log("'%s' animation = %s", g_selectedObject->GetName().c_str(), g_selectedObject->GetAnimationName(next));
                }
            }

            // Play / pause toggle.
            const bool playHovered = CheckCollisionPointRec(mouse, playBtn);
            const bool playing = g_selectedObject->animPlay;
            DrawRectangleRounded(playBtn, 0.2f, 4, Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, HoverProgress(211, playHovered)));
            DrawRectangleLinesEx(playBtn, playHovered ? 2.0f : 1.0f, playHovered ? theme::ACCENT : theme::BORDER);
            if (playHovered) MarkHand();
            const float cx = playBtn.x + playBtn.width * 0.5f, cy = playBtn.y + playBtn.height * 0.5f;
            if (playing) {
                DrawRectangle((int)(cx - 5.0f), (int)(cy - 5.0f), 3, 10, theme::TEXT);
                DrawRectangle((int)(cx + 2.0f), (int)(cy - 5.0f), 3, 10, theme::TEXT);
            } else {
                DrawTriangle({ cx - 4.0f, cy - 6.0f }, { cx - 4.0f, cy + 6.0f }, { cx + 6.0f, cy }, theme::TEXT);
            }
            if (playHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (playing) g_selectedObject->StopAnimation();
                else g_selectedObject->PlayAnimation(cur, g_selectedObject->animLoop, g_selectedObject->animSpeed);
            }
        }

        // --- Texture (for both primitives and imported meshes) ---
        y += rowHeight;
        DrawTextArial("Texture:", labelX, y + 3.0f, 13.0f, theme::TEXT_MUTED);

        std::string texName = "None";
        Color texColor = theme::TEXT_DIM;
        if (g_selectedObject && !g_selectedObject->GetTexturePath().empty()) {
            std::string tp = g_selectedObject->GetTexturePath();
            size_t tslash = tp.find_last_of("/\\");
            if (tslash != std::string::npos) tp = tp.substr(tslash + 1);
            texName = tp;
            texColor = theme::TEXT;
        }

        // Texture file name, using the whole value column (the buttons have their own row).
        DrawTextArial(FitTextToWidth(texName, 13.0f, inputWidth).c_str(), inputX, y + 3.0f, 13.0f, texColor);

        // Preset Texture Dropdown (shown for primitives AND meshes if presets exist)
        const auto& currentProject = ::project::GetCurrentProject();
        bool hasPrimitive = false, hasMesh = false;
        for (auto* obj : g_selection) {
            if (obj && obj->HasModel()) hasMesh = true;
            else if (obj) hasPrimitive = true;
        }

        std::vector<std::string> presets;

        // Preset textures are global - always use the working directory (flyengine root)
        // for both scanning and resolving paths. This avoids path resolution issues
        // when a project is open in a different location.
        std::string presetBaseDir = std::filesystem::current_path().generic_string();

        if (!presetBaseDir.empty()) {
            // Auto-refresh cache when dropdown opens (if >2s since last refresh)
            double now = GetTime();
            if (!g_presetDropdownItemsCached || g_presetDropdownProjectDir != presetBaseDir ||
                (g_presetDropdownOpen && now - g_presetDropdownLastRefresh > 2.0)) {
                g_presetDropdownItems = textureManager::GetPresetTextures(presetBaseDir);
                g_presetDropdownProjectDir = presetBaseDir;
                g_presetDropdownItemsCached = true;
                g_presetDropdownLastRefresh = now;
            }
            presets = g_presetDropdownItems;
        }

        // Store the base dir for use in click handler
        std::string projectDirForPresets = presetBaseDir;

        bool showPresetDropdown = (hasPrimitive || hasMesh) && !presets.empty();
        bool showPresetError = (hasPrimitive || hasMesh) && presets.empty() && !projectDirForPresets.empty();

        // Browse / clear buttons get their own row under the file name.
        const bool showTexButtons = hasMesh && !g_playActive;
        float texBtnRowY = y;
        if (showTexButtons) y += rowHeight;

        if (showPresetDropdown) {
            // Advance y for dropdown row
            y += rowHeight;
            // Build dropdown items: "None" + preset filenames (stem only)
            std::vector<std::string> items = {"None"};
            for (const auto& p : presets) {
                std::string name = std::filesystem::path(p).stem().string();
                items.push_back(name);
            }

            // The list rescans itself every couple of seconds while open, so no
            // manual refresh button is needed.
            Rectangle dropdownRect = { inputX, y, inputWidth, inputHeight };
            bool dropdownHovered = CheckCollisionPointRec(mouse, dropdownRect);

            // Draw dropdown background
            DrawDropdownBox(dropdownRect, dropdownHovered || g_presetDropdownOpen);
            if (dropdownHovered) MarkHand();

            // Auto-detect current texture among presets
            if (g_selectedObject) {
                std::string curTex = g_selectedObject->GetTexturePath();
                if (curTex.empty()) {
                    g_presetDropdownSelected = 0; // "None"
                } else {
                    g_presetDropdownSelected = -1;
                    std::string curStem = std::filesystem::path(curTex).stem().string();
                    for (int pi = 0; pi < (int)presets.size(); ++pi) {
                        std::string presetStem = std::filesystem::path(presets[pi]).stem().string();
                        if (curStem == presetStem) {
                            g_presetDropdownSelected = pi + 1;
                            break;
                        }
                    }
                }
            }

            // Draw current selection text
            const bool presetActive = g_presetDropdownSelected >= 0 && g_presetDropdownSelected < (int)items.size();
            // A custom texture isn't one of the presets; say so instead of a stale prompt.
            std::string currentText = presetActive ? items[g_presetDropdownSelected]
                : (g_selectedObject && !g_selectedObject->GetTexturePath().empty() ? "Custom texture" : "Presets...");
            float textX = dropdownRect.x + 8.0f;
            float textY = dropdownRect.y + (dropdownRect.height - 13.0f) * 0.5f;
            DrawTextArial(FitTextToWidth(currentText, 13.0f, dropdownRect.width - 34.0f).c_str(), textX, textY, 13.0f,
                          presetActive ? theme::TEXT : theme::TEXT_DIM);

            // Draw dropdown arrow
            float arrowX = dropdownRect.x + dropdownRect.width - 20.0f;
            float arrowY = dropdownRect.y + dropdownRect.height * 0.5f;
            DrawTriangle(
                { arrowX - 5.0f, arrowY - 3.0f },
                { arrowX + 5.0f, arrowY - 3.0f },
                { arrowX, arrowY + 3.0f },
                theme::TEXT
            );

            // Handle click to open/close
            if (dropdownHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                g_presetDropdownOpen = !g_presetDropdownOpen;
            }

            // Draw open dropdown list (store state for later rendering after scissor ends)
            if (g_presetDropdownOpen) {
                float itemHeight = 24.0f;
                int maxVisible = 8;
                int visibleCount = (int)std::min<size_t>(items.size(), maxVisible);
                Rectangle listRect = { dropdownRect.x, dropdownRect.y + dropdownRect.height,
                                     dropdownRect.width, itemHeight * visibleCount };

                // Keep the list fully on-screen: if it doesn't fit below the
                // dropdown header, flip it to open upward instead of letting
                // it run off (and get sliced at) the bottom of the window.
                float screenH = static_cast<float>(GetScreenHeight());
                if (listRect.y + listRect.height > screenH) {
                    float flippedY = dropdownRect.y - listRect.height;
                    listRect.y = (flippedY >= 0.0f) ? flippedY : (screenH - listRect.height);
                    if (listRect.y < 0.0f) listRect.y = 0.0f;
                }
                // Also clamp horizontally in case the header sits near a screen edge.
                float screenW = static_cast<float>(GetScreenWidth());
                if (listRect.x + listRect.width > screenW) listRect.x = screenW - listRect.width;
                if (listRect.x < 0.0f) listRect.x = 0.0f;

                // Store state for deferred rendering
                g_presetDropdownListPending = true;
                g_presetDropdownListRect = listRect;
                g_presetDropdownHeaderRect = dropdownRect;
                g_presetDropdownListItems = items;
                g_presetDropdownListPresets = presets;
                g_presetDropdownListProjectDir = projectDirForPresets;
            } else {
                g_presetDropdownListPending = false;
            }
        }
        else if (showPresetError) {
            // Advance y for error row and show error message
            y += rowHeight;
            DrawTextArial("Could not load preset textures", inputX, y + 3.0f, 13.0f, theme::TEXT_DIM);
        }

        // Advance past texture section: always 1 more row to get past the
        // dropdown/error row (or the label row if neither is shown). Must run
        // for primitives too, not just meshes, or the Position header overlaps
        // the dropdown.
        y += rowHeight;

        // Browse + clear buttons (only for imported meshes)
        if (showTexButtons) {
            const float clearW = 24.0f, gap = 4.0f;
            Rectangle browseBtn = { inputX, texBtnRowY, inputWidth - clearW - gap, 22.0f };
            Rectangle clearBtn = { inputX + inputWidth - clearW, texBtnRowY, clearW, 22.0f };

            bool browseHovered = CheckCollisionPointRec(mouse, browseBtn);
            float browseT = HoverProgress(200, browseHovered);
            DrawRectangleRounded(browseBtn, 0.2f, 4, Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, browseT));
            DrawRectangleLinesEx(browseBtn, browseHovered ? 2.0f : 1.0f, browseHovered ? theme::ACCENT : theme::BORDER);
            if (browseHovered) MarkHand();
            const char* browseLabel = "Browse...";
            const float browseLabelW = MeasureTextArial(browseLabel, 12.0f);
            DrawTextArial(browseLabel, browseBtn.x + (browseBtn.width - browseLabelW) * 0.5f, browseBtn.y + 4.0f, 12.0f, theme::TEXT);

            bool anySelectedHasTexture = false;
            for (auto* obj : g_selection) {
                if (obj && !obj->GetTexturePath().empty()) { anySelectedHasTexture = true; break; }
            }
            bool clearHovered = CheckCollisionPointRec(mouse, clearBtn) && anySelectedHasTexture;
            float clearT = HoverProgress(201, clearHovered);
            Color clearBg = clearHovered ? Mix(theme::BG_INPUT, theme::DANGER, clearT) : theme::BG_WIDGET;
            DrawRectangleRounded(clearBtn, 0.2f, 4, clearBg);
            DrawRectangleLinesEx(clearBtn, clearHovered ? 2.0f : 1.0f,
                clearHovered ? theme::DANGER : theme::BORDER);
            if (clearHovered) MarkHand();
            const Color clearTint = anySelectedHasTexture ? theme::TEXT : theme::TEXT_DIM;
            if (g_deleteIcon.id != 0) {
                DrawTexturePro(g_deleteIcon,
                    { 0, 0, (float)g_deleteIcon.width, (float)g_deleteIcon.height },
                    { clearBtn.x + 4.0f, clearBtn.y + 3.0f, clearBtn.width - 8.0f, clearBtn.height - 6.0f },
                    { 0, 0 }, 0.0f, clearTint);
            } else {
                DrawTextArial("X", clearBtn.x + 8.0f, clearBtn.y + 4.0f, 12.0f, clearTint);
            }

            if (browseHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                // Non-blocking: the result is delivered by PumpAssetBrowserDialogs()
                // on a later frame, not here.
                BeginChooseTexturePath(currentProject.path);
            }
            if (clearHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !currentProject.path.empty()) {
                for (auto* obj : g_selection) {
                    if (obj && obj->HasModel()) {
                        obj->ClearTexture(currentProject.path);
                    }
                }
            }
        }
    }
    y += groupGap;

    // --- Water Interactions ---
    if (g_selectedObject) {
        y = DrawGroupHeader(panelRec, y, "Water Interactions", g_waterInteractOpen, 110);
        if (g_waterInteractOpen) {
            struct WaterOpt { const char* label; bool ScatteredObject::WaterInteraction::* member; int hoverId; };
            const WaterOpt opts[3] = {
                { "Floats:", &ScatteredObject::WaterInteraction::floats, 26 },
                { "Foam:",   &ScatteredObject::WaterInteraction::foam,   27 },
                { "Splash:", &ScatteredObject::WaterInteraction::splash, 28 },
            };
            for (const WaterOpt& o : opts) {
                DrawTextArial(o.label, labelX, y + 3.0f, 13.0f, theme::TEXT_MUTED);
                Rectangle check = { generalControlX, y, 22.0f, 22.0f };
                bool on = g_selectedObject->waterInteraction.*(o.member);
                bool hovered = CheckCollisionPointRec(mouse, check);
                float t = HoverProgress(o.hoverId, hovered);
                DrawRectangleRounded(check, 0.2f, 4, Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, t));
                DrawRectangleLinesEx(check, hovered ? 2.0f : 1.0f, hovered ? theme::ACCENT : theme::BORDER);
                if (on) {
                    Vector2 c1 = { check.x + 3.0f, check.y + check.height * 0.5f };
                    Vector2 c2 = { check.x + check.width * 0.5f, check.y + check.height - 5.0f };
                    Vector2 c3 = { check.x + check.width - 3.0f, check.y + 4.0f };
                    DrawLineEx(c1, c2, 2.5f, theme::SUCCESS);
                    DrawLineEx(c2, c3, 2.5f, theme::SUCCESS);
                }
                if (hovered) MarkHand();
                if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    bool nv = !on;
                    g_selectedObject->waterInteraction.*(o.member) = nv;
                    for (auto* obj : g_selection) if (obj && obj != g_selectedObject) obj->waterInteraction.*(o.member) = nv;
                    Log("'%s' %s %s", g_selectedObject->GetName().c_str(), o.label, nv ? "true" : "false");
                }
                y += rowHeight;
            }
        }
        y += groupGap;
    }

    // --- Position ---
    y = DrawGroupHeader(panelRec, y, "Position", g_positionOpen, 100);
    if (g_positionOpen) {
        DrawMultiNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_POS_X, g_pos->x, "Pos X:", labelX,
            [](ScatteredObject* o, float v) { o->GetPosPtr()->x = v; });
        DrawMultiNumberInput({ inputX, y + rowHeight, inputWidth, inputHeight }, FIELD_POS_Y, g_pos->y, "Pos Y:", labelX,
            [](ScatteredObject* o, float v) { o->GetPosPtr()->y = v; });
        DrawMultiNumberInput({ inputX, y + rowHeight * 2.0f, inputWidth, inputHeight }, FIELD_POS_Z, g_pos->z, "Pos Z:", labelX,
            [](ScatteredObject* o, float v) { o->GetPosPtr()->z = v; });
        y += rowHeight * 3.0f;
    }
    y += groupGap;

    // --- Size ---
    y = DrawGroupHeader(panelRec, y, "Size", g_sizeOpen, 101);
    if (g_sizeOpen) {
        Vector3 sizeBefore = *g_size;
        DrawMultiNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_SIZE_W, g_size->x, "Width:", labelX,
            [](ScatteredObject* o, float v) { o->GetSizePtr()->x = v; o->ResetMassAuto(); }, 0.01f);
        DrawMultiNumberInput({ inputX, y + rowHeight, inputWidth, inputHeight }, FIELD_SIZE_H, g_size->y, "Height:", labelX,
            [](ScatteredObject* o, float v) { o->GetSizePtr()->y = v; o->ResetMassAuto(); }, 0.01f);
        DrawMultiNumberInput({ inputX, y + rowHeight * 2.0f, inputWidth, inputHeight }, FIELD_SIZE_L, g_size->z, "Length:", labelX,
            [](ScatteredObject* o, float v) { o->GetSizePtr()->z = v; o->ResetMassAuto(); }, 0.01f);
        if (g_selectedObject &&
            (g_size->x != sizeBefore.x || g_size->y != sizeBefore.y || g_size->z != sizeBefore.z)) {
            g_selectedObject->ResetMassAuto();
        }
        y += rowHeight * 3.0f;
    }
    y += groupGap;

    // --- Rotation ---
    if (g_rotation) {
        y = DrawGroupHeader(panelRec, y, "Rotation", g_rotationOpen, 102);
        if (g_rotationOpen) {
            DrawMultiNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_ROT_X, g_rotation->x, "Rot X:", labelX,
                [](ScatteredObject* o, float v) { o->GetRotationPtr()->x = v; });
            DrawMultiNumberInput({ inputX, y + rowHeight, inputWidth, inputHeight }, FIELD_ROT_Y, g_rotation->y, "Rot Y:", labelX,
                [](ScatteredObject* o, float v) { o->GetRotationPtr()->y = v; });
            DrawMultiNumberInput({ inputX, y + rowHeight * 2.0f, inputWidth, inputHeight }, FIELD_ROT_Z, g_rotation->z, "Rot Z:", labelX,
                [](ScatteredObject* o, float v) { o->GetRotationPtr()->z = v; });
            y += rowHeight * 3.0f;
        }
        y += groupGap;
    }

    // --- Origin (pivot) ---
    if (g_origin) {
        y = DrawGroupHeader(panelRec, y, "Origin", g_originOpen, 106);
        if (g_originOpen) {
            DrawMultiNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_ORIGIN_X, g_origin->x, "Or X:", labelX,
                [](ScatteredObject* o, float v) { o->GetOriginPtr()->x = v; });
            DrawMultiNumberInput({ inputX, y + rowHeight, inputWidth, inputHeight }, FIELD_ORIGIN_Y, g_origin->y, "Or Y:", labelX,
                [](ScatteredObject* o, float v) { o->GetOriginPtr()->y = v; });
            DrawMultiNumberInput({ inputX, y + rowHeight * 2.0f, inputWidth, inputHeight }, FIELD_ORIGIN_Z, g_origin->z, "Or Z:", labelX,
                [](ScatteredObject* o, float v) { o->GetOriginPtr()->z = v; });
            y += rowHeight * 3.0f;
        }
        y += groupGap;
    }

    // --- Linear Velocity ---
    if (g_selectedObject) {
        y = DrawGroupHeader(panelRec, y, "Linear Velocity", g_linearVelocityOpen, 103);
        if (g_linearVelocityOpen) {
            Vector3 vel = live ? g_simulation->GetBodyVelocity(g_selectedObject) : g_selectedObject->GetVelocity();
            Vector3 velBefore = vel;
            bool velX = DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_VEL_X, vel.x, "Vel X:", labelX);
            bool velY = DrawNumberInput({ inputX, y + rowHeight, inputWidth, inputHeight }, FIELD_VEL_Y, vel.y, "Vel Y:", labelX);
            bool velZ = DrawNumberInput({ inputX, y + rowHeight * 2.0f, inputWidth, inputHeight }, FIELD_VEL_Z, vel.z, "Vel Z:", labelX);
            if (live) g_simulation->SetBodyVelocity(g_selectedObject, vel);
            else g_selectedObject->SetVelocity(vel);
            if ((velX || velY || velZ) && (vel.x != velBefore.x || vel.y != velBefore.y || vel.z != velBefore.z) && g_selection.size() > 1) {
                for (auto* obj : g_selection) {
                    if (!obj || obj == g_selectedObject) continue;
                    if (live) g_simulation->SetBodyVelocity(obj, vel);
                    else obj->SetVelocity(vel);
                }
            }
            y += rowHeight * 3.0f;
        }
        y += groupGap;
    }

    // --- Angular Velocity ---
    if (g_selectedObject) {
        y = DrawGroupHeader(panelRec, y, "Angular Velocity", g_angularVelocityOpen, 104);
        if (g_angularVelocityOpen) {
            Vector3 angVel = live ? g_simulation->GetBodyAngularVelocity(g_selectedObject) : g_selectedObject->GetAngularVelocity();
            Vector3 angVelBefore = angVel;
            bool angX = DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_ANG_X, angVel.x, "Ang X:", labelX);
            bool angY = DrawNumberInput({ inputX, y + rowHeight, inputWidth, inputHeight }, FIELD_ANG_Y, angVel.y, "Ang Y:", labelX);
            bool angZ = DrawNumberInput({ inputX, y + rowHeight * 2.0f, inputWidth, inputHeight }, FIELD_ANG_Z, angVel.z, "Ang Z:", labelX);
            if (live) g_simulation->SetBodyAngularVelocity(g_selectedObject, angVel);
            else g_selectedObject->SetAngularVelocity(angVel);
            if ((angX || angY || angZ) && (angVel.x != angVelBefore.x || angVel.y != angVelBefore.y || angVel.z != angVelBefore.z) && g_selection.size() > 1) {
                for (auto* obj : g_selection) {
                    if (!obj || obj == g_selectedObject) continue;
                    if (live) g_simulation->SetBodyAngularVelocity(obj, angVel);
                    else obj->SetAngularVelocity(angVel);
                }
            }
            y += rowHeight * 3.0f;
        }
        y += groupGap;
    }

    EndScissorMode();
}

// Cuts `text` to fit `maxWidth`, ending in "..." when it had to be shortened.
std::string FitTextToWidth(const std::string& text, float fontSize, float maxWidth) {
    if (MeasureTextArial(text.c_str(), fontSize) <= maxWidth) return text;
    std::string out = text;
    while (!out.empty() && MeasureTextArial((out + "...").c_str(), fontSize) > maxWidth) out.pop_back();
    return out + "...";
}

static std::string BaseName(const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

void DrawScriptErrorPanel() {
    Rectangle panel = GetScriptPanelBounds();
    if (panel.width <= 0.0f) return;

    constexpr float textSize = 12.0f;
    const Vector2 mouse = GetMousePosition();
    const bool failed = g_scriptBuildState == ScriptBuildState::Failed;

    int errors = 0, warnings = 0;
    for (const auto& d : g_scriptDiagnostics) {
        if (d.severity == ScriptDiagnostic::Severity::Error) ++errors;
        else if (d.severity == ScriptDiagnostic::Severity::Warning) ++warnings;
    }

    DrawRectangleRec({ panel.x + 3.0f, panel.y + 3.0f, panel.width, panel.height }, theme::SHADOW);
    DrawRectangleRec(panel, Color{ 20, 21, 26, 240 });
    DrawRectangleLinesEx(panel, 1.0f, failed ? theme::DANGER : theme::BORDER_STRONG);

    // --- header -----------------------------------------------------------
    std::string title;
    if (!failed) {
        title = "COMPILING SCRIPTS...";
    } else {
        title = "SCRIPT BUILD FAILED - " + std::to_string(errors) + (errors == 1 ? " error" : " errors");
        if (warnings > 0) title += ", " + std::to_string(warnings) + (warnings == 1 ? " warning" : " warnings");
    }
    DrawTextArial(title.c_str(), panel.x + 10.0f, panel.y + 6.0f, 13.0f, failed ? theme::DANGER : theme::ACCENT);

    if (failed) {
        // [Rebuild] and [X] on the right
        Rectangle closeBtn = { panel.x + panel.width - 24.0f, panel.y + 4.0f, 18.0f, 18.0f };
        Rectangle rebuildBtn = { closeBtn.x - 70.0f, panel.y + 4.0f, 64.0f, 18.0f };

        const bool rebuildHover = CheckCollisionPointRec(mouse, rebuildBtn);
        const float rebuildT = HoverProgress(9700, rebuildHover);
        DrawRectangleRounded(rebuildBtn, 0.2f, 4, Mix(theme::BG_WIDGET, theme::ACCENT_PRESSED, rebuildT));
        DrawRectangleLinesEx(rebuildBtn, 1.0f, Mix(theme::BORDER, theme::ACCENT, rebuildT));
        DrawCenteredTextArial("Rebuild", rebuildBtn, 12.0f, rebuildHover ? theme::TEXT : theme::TEXT_MUTED);
        if (rebuildHover) MarkHand();
        if (rebuildHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (ScriptRuntime* rt = GetActiveRuntime())
                if (auto* host = rt->GetHost()) host->RequestScriptRebuild();
        }

        const bool closeHover = CheckCollisionPointRec(mouse, closeBtn);
        const float closeT = HoverProgress(9701, closeHover);
        DrawRectangleRounded(closeBtn, 0.2f, 4, Mix(theme::BG_WIDGET, theme::DANGER, closeT));
        DrawRectangleLinesEx(closeBtn, 1.0f, Mix(theme::BORDER, theme::DANGER_HOVER, closeT));
        DrawCenteredTextArial("X", closeBtn, 12.0f, closeHover ? theme::TEXT : theme::TEXT_MUTED);
        if (closeHover) MarkHand();
        if (closeHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_scriptPanelDismissed = true;
    }
    DrawLine(static_cast<int>(panel.x + 6.0f), static_cast<int>(panel.y + kScriptPanelHeaderH),
             static_cast<int>(panel.x + panel.width - 6.0f), static_cast<int>(panel.y + kScriptPanelHeaderH), theme::DIVIDER);
    if (!failed) return;

    // --- rows -------------------------------------------------------------
    const int total = static_cast<int>(g_scriptDiagnostics.size());
    const int visible = std::min(kScriptPanelMaxRows, total);
    const int maxScroll = std::max(0, total - visible);
    const bool overPanel = CheckCollisionPointRec(mouse, panel);
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f && overPanel) g_scriptPanelScroll -= static_cast<int>(wheel * 2.0f);
    g_scriptPanelScroll = std::clamp(g_scriptPanelScroll, 0, maxScroll);

    const float rowsTop = panel.y + kScriptPanelHeaderH + 2.0f;
    const float textRight = panel.x + panel.width - (maxScroll > 0 ? 16.0f : 10.0f);

    for (int i = 0; i < visible; ++i) {
        const ScriptDiagnostic& d = g_scriptDiagnostics[static_cast<size_t>(g_scriptPanelScroll + i)];
        const Rectangle row = { panel.x + 4.0f, rowsTop + static_cast<float>(i) * kScriptPanelRowH,
                                panel.width - 8.0f, kScriptPanelRowH };
        const bool clickable = !d.file.empty();
        const bool hover = clickable && CheckCollisionPointRec(mouse, row);
        if (hover) {
            DrawRectangleRec(row, theme::BG_ROW_HOVER);
            MarkHand();
        }

        Color sevColor = theme::TEXT_MUTED;
        const char* sev = "note";
        if (d.severity == ScriptDiagnostic::Severity::Error) { sevColor = theme::DANGER; sev = "error"; }
        else if (d.severity == ScriptDiagnostic::Severity::Warning) { sevColor = Color{ 226, 178, 70, 255 }; sev = "warning"; }
        const bool isNote = d.severity == ScriptDiagnostic::Severity::Note;
        const float ty = row.y + (row.height - textSize) * 0.5f;

        float x = row.x + 6.0f + (isNote ? 14.0f : 0.0f);
        DrawTextArial(sev, x, ty, textSize, sevColor);
        x += 52.0f;

        if (clickable) {
            std::string where = BaseName(d.file);
            if (d.line > 0) where += ":" + std::to_string(d.line);
            DrawTextArial(where.c_str(), x, ty, textSize, theme::ACCENT);
            x += MeasureTextArial(where.c_str(), textSize) + 10.0f;
        }
        const std::string msg = FitTextToWidth(d.message, textSize, textRight - x);
        DrawTextArial(msg.c_str(), x, ty, textSize, isNote ? theme::TEXT_DIM : theme::TEXT);

        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            scriptLauncher::RequestOpenAt(d.file, d.line, d.column);
    }

    if (maxScroll > 0) {
        const float sbX = panel.x + panel.width - 10.0f;
        const float sbH = static_cast<float>(visible) * kScriptPanelRowH;
        DrawRectangle(static_cast<int>(sbX), static_cast<int>(rowsTop), 4, static_cast<int>(sbH), Color{ 255, 255, 255, 18 });
        float thumbH = std::max(12.0f, sbH * static_cast<float>(visible) / static_cast<float>(total));
        const float thumbY = rowsTop + (sbH - thumbH) * (static_cast<float>(g_scriptPanelScroll) / static_cast<float>(maxScroll));
        DrawRectangle(static_cast<int>(sbX), static_cast<int>(thumbY), 4, static_cast<int>(thumbH), theme::BORDER);
    }
}

} // namespace ui
