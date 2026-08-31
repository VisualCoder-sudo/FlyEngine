#include "ScriptEditor.hpp"
#include "Flyscript.hpp"
#include "ui.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace scriptEditor {

namespace {

constexpr float WIN_W = 820.0f;
constexpr float WIN_H = 560.0f;
constexpr float TITLE_H = 34.0f;
constexpr float TOOLBAR_H = 36.0f;
constexpr float STATUS_H = 24.0f;
constexpr float LINE_H = 20.0f;
constexpr float CODE_SIZE = 16.0f;
constexpr float GUTTER_W = 48.0f;
constexpr float PAD_X = 8.0f;

// Mirrors ui.cpp's theme:: palette so the script editor reads as part of the
// same app instead of a visually separate window.
namespace theme {
static const Color BG_PANEL       = Color{ 30, 32, 38, 255 };
static const Color BG_TITLE       = Color{ 38, 41, 48, 255 };
static const Color BG_WIDGET      = Color{ 48, 51, 60, 255 };
static const Color BG_WIDGET_HOVER = Color{ 63, 67, 78, 255 };
static const Color BG_INPUT       = Color{ 21, 23, 28, 255 };
static const Color BG_INPUT_HOVER = Color{ 27, 29, 36, 255 };
static const Color BORDER         = Color{ 72, 77, 88, 255 };
static const Color BORDER_STRONG  = Color{ 104, 110, 122, 255 };
static const Color DIVIDER        = Color{ 56, 60, 70, 255 };
static const Color ACCENT         = Color{ 0, 190, 200, 255 };
static const Color ACCENT_HOVER   = Color{ 40, 210, 220, 255 };
static const Color TEXT           = Color{ 232, 232, 238, 255 };
static const Color TEXT_MUTED     = Color{ 156, 161, 172, 255 };
static const Color DANGER         = Color{ 216, 84, 84, 255 };
static const Color DANGER_HOVER   = Color{ 236, 112, 112, 255 };
static const Color SUCCESS        = Color{ 74, 176, 104, 255 };
static const Color SHADOW         = Color{ 0, 0, 0, 92 };
} // namespace theme

static Color Mix(Color from, Color to, float t) { return ColorLerp(from, to, t); }

// Eased hover progress for the two title/toolbar buttons, same easing speeds
// used throughout ui.cpp so hover feel is consistent across windows.
float g_closeHoverT = 0.0f;
float g_playHoverT = 0.0f;

Font g_codeFont = { 0 };

bool g_open = false;
bool g_capture = false;       // keyboard focus inside the window
ScatteredObject* g_targetObject = nullptr;
int g_scriptIndex = -1;
std::string g_text;
int g_caretLine = 0;
int g_caretCol = 0;
int g_scrollLine = 0;
float g_scrollX = 0.0f;
bool g_dirty = false;
int g_errorLine = 0;
std::string g_errorMsg;

// Selection state
int g_selAnchorLine = 0;
int g_selAnchorCol = 0;
bool g_hasSelection = false;
bool g_dragging = false;

// Autocomplete dropdown state (same vocabulary as the command console).
std::vector<std::string> g_completions;
int g_completionIndex = 0;
// Mouse position from the previous frame, used so hover over a dropdown item
// only updates the highlight while the mouse is actually moving. This stops
// the hover logic from fighting (and "spasming" against) Up/Down selection.
Vector2 g_lastMousePos = { -9999.0f, -9999.0f };

Rectangle WindowRect() {
    return { (GetScreenWidth() - WIN_W) * 0.5f, (GetScreenHeight() - WIN_H) * 0.5f, WIN_W, WIN_H };
}

Rectangle CodeRect() {
    Rectangle w = WindowRect();
    return { w.x, w.y + TITLE_H + TOOLBAR_H, w.width, w.height - TITLE_H - TOOLBAR_H - STATUS_H };
}

Rectangle CloseRect() {
    Rectangle w = WindowRect();
    return { w.x + w.width - 26.0f, w.y + 6.0f, 20.0f, 20.0f };
}

Rectangle PlayCheckRect() {
    Rectangle w = WindowRect();
    return { w.x + 10.0f, w.y + TITLE_H + 9.0f, 18.0f, 18.0f };
}

bool InRect(Vector2 p, Rectangle r) { return CheckCollisionPointRec(p, r); }

void DrawTextCode(const char* text, float x, float y, float size, Color color) {
    // Round to whole pixels so glyphs snap to the pixel grid and consecutive
    // lines/caret stay aligned instead of jittering on fractional offsets.
    DrawTextEx(g_codeFont, text, { (float)(int)(x + 0.5f), (float)(int)(y + 0.5f) }, size, 1.0f, color);
}

float TextWidth(const std::string& s) {
    return MeasureTextEx(g_codeFont, s.c_str(), CODE_SIZE, 1.0f).x;
}

float TextWidthAt(const char* s, float size) {
    return MeasureTextEx(g_codeFont, s, size, 1.0f).x;
}

// Rendered x-offset (relative to the text-area base) at which the caret for a
// column sits. Walks the same snapped span layout as the draw loop, so the
// caret always lands exactly on the drawn text.
float CaretOffsetX(const std::string& line, int col);

// Centers short text (e.g. the "X" close glyph) inside a rectangle, both
// horizontally and vertically, instead of relying on hand-tuned offsets.
void DrawCenteredTextCode(const char* text, Rectangle rec, float size, Color color) {
    float w = TextWidthAt(text, size);
    float x = rec.x + (rec.width - w) * 0.5f;
    float y = rec.y + (rec.height - size) * 0.5f;
    DrawTextCode(text, x, y, size, color);
}

// ---- text buffer helpers ----

int CountLines(const std::string& s) {
    int n = 1;
    for (char c : s) if (c == '\n') ++n;
    return n;
}

size_t LineStart(const std::string& s, int line) {
    size_t off = 0;
    for (int l = 0; l < line; ++l) {
        size_t p = s.find('\n', off);
        if (p == std::string::npos) return s.size();
        off = p + 1;
    }
    return off;
}

size_t LineEnd(const std::string& s, int line) {
    size_t p = s.find('\n', LineStart(s, line));
    return p == std::string::npos ? s.size() : p;
}

std::string GetLineText(const std::string& s, int line) {
    size_t a = LineStart(s, line);
    size_t b = LineEnd(s, line);
    return s.substr(a, b - a);
}

void OffsetToLineCol(const std::string& s, size_t offset, int& line, int& col) {
    line = 0; col = 0;
    size_t n = std::min(offset, s.size());
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '\n') { ++line; col = 0; }
        else ++col;
    }
}

size_t LineColToOffset(const std::string& s, int line, int col) {
    size_t off = LineStart(s, line);
    int c = 0;
    while (c < col && off < s.size() && s[off] != '\n') { ++off; ++c; }
    return off;
}

void ClampCaret() {
    int total = CountLines(g_text);
    if (g_caretLine < 0) g_caretLine = 0;
    if (g_caretLine >= total) g_caretLine = total - 1;
    int len = (int)GetLineText(g_text, g_caretLine).size();
    if (g_caretCol < 0) g_caretCol = 0;
    if (g_caretCol > len) g_caretCol = len;
}

void ClampScroll() {
    int total = CountLines(g_text);
    int visible = (int)(CodeRect().height / LINE_H);
    if (visible < 1) visible = 1;
    if (g_scrollLine < 0) g_scrollLine = 0;
    if (g_scrollLine >= total) g_scrollLine = std::max(0, total - 1);
    if (g_caretLine < 0) g_caretLine = 0;
    if (g_caretLine >= total) g_caretLine = std::max(0, total - 1);
    if (g_scrollLine + visible > total && total >= visible) g_scrollLine = total - visible;
    if (g_scrollLine < 0) g_scrollLine = 0;

    // Clamp horizontal scroll to the longest line so Shift+wheel cannot drift
    // into empty space (and none is shown when every line fits).
    float maxLineW = 0.0f;
    for (int i = 0; i < total; ++i) {
        std::string ln = GetLineText(g_text, i);
        float w = CaretOffsetX(ln, (int)ln.size());
        if (w > maxLineW) maxLineW = w;
    }
    Rectangle code = CodeRect();
    float contentW = code.width - GUTTER_W - 2.0f * PAD_X;
    if (maxLineW + PAD_X < contentW) g_scrollX = 0.0f;
    else if (g_scrollX > maxLineW + PAD_X - contentW) g_scrollX = maxLineW + PAD_X - contentW;
    if (g_scrollX < 0.0f) g_scrollX = 0.0f;
}

// Scrolls the view to keep the caret visible. Called only when the caret
// actually moves (typing, arrow keys, click), so manual wheel scrolling is
// free to leave the caret off-screen.
void RevealCaret() {
    int total = CountLines(g_text);
    int visible = (int)(CodeRect().height / LINE_H);
    if (visible < 1) visible = 1;
    if (g_caretLine < g_scrollLine) g_scrollLine = g_caretLine;
    if (g_caretLine >= g_scrollLine + visible) g_scrollLine = g_caretLine - visible + 1;
    if (g_scrollLine < 0) g_scrollLine = 0;

    float caretX = CaretOffsetX(GetLineText(g_text, g_caretLine), g_caretCol);
    Rectangle code = CodeRect();
    float contentW = code.width - GUTTER_W - 2.0f * PAD_X;
    if (caretX - g_scrollX > contentW) g_scrollX = caretX - contentW;
    if (caretX - g_scrollX < 0.0f) g_scrollX = caretX;
    if (g_scrollX < 0.0f) g_scrollX = 0.0f;
}

void ResetCaret() {
    g_caretLine = 0;
    g_caretCol = 0;
    g_scrollLine = 0;
    g_scrollX = 0.0f;
}

// ---- target binding / live commit ----

bool HasObjectTarget() { return g_targetObject != nullptr; }

void CommitToTarget() {
    if (g_targetObject) {
        g_targetObject->script = g_text;
    } else if (g_scriptIndex >= 0 && g_scriptIndex < (int)flyscript::GetRuntime().scripts.size()) {
        flyscript::GetRuntime().scripts[g_scriptIndex].source = g_text;
    }
}

std::string TargetName() {
    if (g_targetObject) return g_targetObject->GetName();
    if (g_scriptIndex >= 0 && g_scriptIndex < (int)flyscript::GetRuntime().scripts.size())
        return flyscript::GetRuntime().scripts[g_scriptIndex].name;
    return "Untitled";
}

void SyncErrorFromRuntime() {
    flyscript::Runtime& rt = flyscript::GetRuntime();
    int line = 0;
    const std::string& msg = g_targetObject ? rt.GetErrorMessage(g_targetObject) : rt.GetErrorMessage(g_scriptIndex);
    if (g_targetObject) line = rt.GetErrorLine(g_targetObject);
    else line = rt.GetErrorLine(g_scriptIndex);
    if (!msg.empty()) {
        g_errorLine = line;
        g_errorMsg = msg;
    }
}

void ToggleRunOnPlay() {
    if (g_targetObject) {
        g_targetObject->runOnPlay = !g_targetObject->runOnPlay;
    } else if (g_scriptIndex >= 0 && g_scriptIndex < (int)flyscript::GetRuntime().scripts.size()) {
        flyscript::GetRuntime().scripts[g_scriptIndex].runOnPlay =
            !flyscript::GetRuntime().scripts[g_scriptIndex].runOnPlay;
    }
}

bool RunOnPlayValue() {
    if (g_targetObject) return g_targetObject->runOnPlay;
    if (g_scriptIndex >= 0 && g_scriptIndex < (int)flyscript::GetRuntime().scripts.size())
        return flyscript::GetRuntime().scripts[g_scriptIndex].runOnPlay;
    return false;
}

// ---- editing ----

void InsertText(size_t offset, const std::string& text) {
    g_text.insert(offset, text);
    int lines = 0;
    size_t lastNl = std::string::npos;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') { ++lines; lastNl = i; }
    }
    if (lines == 0) {
        g_caretCol += (int)text.size();
    } else {
        g_caretLine += lines;
        g_caretCol = (int)(text.size() - lastNl - 1);
    }
}

void OnEdit() {
    CommitToTarget();
    g_dirty = true;
    ClampCaret();
}

// ---- selection ----

void ClearSelection() {
    g_hasSelection = false;
    g_selAnchorLine = g_caretLine;
    g_selAnchorCol = g_caretCol;
}

void SetSelectionAnchor() {
    g_selAnchorLine = g_caretLine;
    g_selAnchorCol = g_caretCol;
    g_hasSelection = true;
}

void StartSelection() {
    if (!g_hasSelection) {
        SetSelectionAnchor();
    }
}

void ExtendSelectionTo(int line, int col) {
    g_hasSelection = true;
    g_caretLine = line;
    g_caretCol = col;
}

// Returns normalized selection: start <= end in line/col order.
void GetSelectionRange(int& startLine, int& startCol, int& endLine, int& endCol) {
    if (g_selAnchorLine < g_caretLine ||
        (g_selAnchorLine == g_caretLine && g_selAnchorCol <= g_caretCol)) {
        startLine = g_selAnchorLine; startCol = g_selAnchorCol;
        endLine = g_caretLine;       endCol = g_caretCol;
    } else {
        startLine = g_caretLine;     startCol = g_caretCol;
        endLine = g_selAnchorLine;   endCol = g_selAnchorCol;
    }
}

std::string GetSelectedText() {
    if (!g_hasSelection) return "";
    int sl, sc, el, ec;
    GetSelectionRange(sl, sc, el, ec);
    size_t a = LineColToOffset(g_text, sl, sc);
    size_t b = LineColToOffset(g_text, el, ec);
    return g_text.substr(a, b - a);
}

void DeleteSelection() {
    if (!g_hasSelection) return;
    int sl, sc, el, ec;
    GetSelectionRange(sl, sc, el, ec);
    size_t a = LineColToOffset(g_text, sl, sc);
    size_t b = LineColToOffset(g_text, el, ec);
    g_text.erase(a, b - a);
    g_caretLine = sl;
    g_caretCol = sc;
    ClearSelection();
    OnEdit();
}

bool HasSelection() {
    return g_hasSelection && (g_selAnchorLine != g_caretLine || g_selAnchorCol != g_caretCol);
}

void RefreshCompletionList();
void AcceptCompletion();
void ClearCompletions();

void SetCaretFromMouse(Vector2 mouse) {
    Rectangle code = CodeRect();
    int line = g_scrollLine + (int)((mouse.y - code.y) / LINE_H);
    if (line < 0) line = 0;
    if (line >= CountLines(g_text)) line = CountLines(g_text) - 1;
    g_caretLine = line;
    std::string text = GetLineText(g_text, line);
    float baseX = (float)(int)(code.x + GUTTER_W + PAD_X - g_scrollX + 0.5f);
    int col = 0;
    float best = 1e9f;
    int bestCol = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        float cx = baseX + CaretOffsetX(text, (int)i);
        float dist = fabsf(mouse.x - cx);
        if (dist < best) { best = dist; bestCol = (int)i; }
    }
    g_caretCol = bestCol;
    RefreshCompletionList();
}

// ---- autocomplete ----

// Text typed on the current line from its start up to the caret. This mirrors
// what the command console treats as its input, so both surfaces complete the
// same vocabulary via flyscript::GetCompletions.
std::string CompletionPrefix() {
    std::string line = GetLineText(g_text, g_caretLine);
    return line.substr(0, (size_t)g_caretCol);
}

// Recomputes the completion list from the current prefix. The highlight index
// is only reset when the list itself changes, so Up/Down navigation survives
// the per-frame refresh that happens at the end of KeyDown().
void RefreshCompletionList() {
    std::vector<std::string> next = flyscript::GetCompletions(CompletionPrefix(), g_text);
    if (next != g_completions) {
        g_completions = std::move(next);
        g_completionIndex = 0;
    }
}

void ClearCompletions() {
    g_completions.clear();
    g_completionIndex = 0;
}

void ReplaceLineText(int line, const std::string& newText) {
    size_t a = LineStart(g_text, line);
    size_t b = LineEnd(g_text, line);
    g_text.replace(a, b - a, newText);
}

void AcceptCompletion() {
    if (g_completions.empty()) return;
    const std::string& pick = g_completions[g_completionIndex % (int)g_completions.size()];

    // Replace only the identifier being completed (after the last '.', after
    // '=', or anywhere a bare word was typed), keeping the rest of the line.
    std::string prefix = CompletionPrefix();
    std::string line = GetLineText(g_text, g_caretLine);
    size_t wordStart = flyscript::CompletionWordStart(prefix);

    std::string newPrefix = prefix.substr(0, wordStart) + pick;
    ReplaceLineText(g_caretLine, newPrefix + line.substr(prefix.size()));
    g_caretCol = (int)newPrefix.size();
    OnEdit();
    RefreshCompletionList();
}

Rectangle CompletionPanelRect() {
    Rectangle code = CodeRect();
    float rowH = 20.0f;
    int n = std::min((int)g_completions.size(), 12);
    float height = n * rowH + 8.0f;

    // Calculate width based on longest completion + padding
    float maxWidth = 200.0f; // minimum width
    for (const auto& comp : g_completions) {
        float w = TextWidth(comp);
        if (w > maxWidth) maxWidth = w;
    }
    float width = maxWidth + 32.0f; // 16px padding on each side
    // Cap at code area width
    if (width > code.width - GUTTER_W - 8.0f) width = code.width - GUTTER_W - 8.0f;

    float x = code.x + GUTTER_W + 4.0f;
    float y = code.y + 6.0f;
    if (g_caretLine >= g_scrollLine && g_caretLine < g_scrollLine + (int)(code.height / LINE_H)) {
        y = code.y + (g_caretLine - g_scrollLine) * LINE_H + LINE_H + 2.0f;
    }
    if (y + height > code.y + code.height) y = code.y + code.height - height;
    if (x + width > code.x + code.width) x = code.x + code.width - width;
    return { x, y, width, height };
}

void KeyDown() {
    if (!g_completions.empty()) {
        int n = (int)g_completions.size();
        if (IsKeyPressed(KEY_TAB)) { AcceptCompletion(); RevealCaret(); return; }
        if (IsKeyPressed(KEY_ENTER)) { AcceptCompletion(); RevealCaret(); return; }
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) { g_completionIndex = (g_completionIndex + 1) % n; return; }
        if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) { g_completionIndex = (g_completionIndex + n - 1) % n; return; }
        if (IsKeyPressed(KEY_ESCAPE)) { ClearCompletions(); return; }
    }

    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    // ---- CTRL shortcuts ----
    if (ctrl) {
        if (IsKeyPressed(KEY_A)) {
            g_caretLine = CountLines(g_text) - 1;
            g_caretCol = (int)GetLineText(g_text, g_caretLine).size();
            SetSelectionAnchor();
            g_caretLine = 0;
            g_caretCol = 0;
            RevealCaret();
            return;
        }
        if (IsKeyPressed(KEY_C)) {
            std::string sel = GetSelectedText();
            if (!sel.empty()) SetClipboardText(sel.c_str());
            return;
        }
        if (IsKeyPressed(KEY_X)) {
            std::string sel = GetSelectedText();
            if (!sel.empty()) {
                SetClipboardText(sel.c_str());
                DeleteSelection();
                RevealCaret();
                RefreshCompletionList();
            }
            return;
        }
        if (IsKeyPressed(KEY_V)) {
            const char* clip = GetClipboardText();
            if (clip && clip[0]) {
                if (HasSelection()) DeleteSelection();
                size_t off = LineColToOffset(g_text, g_caretLine, g_caretCol);
                InsertText(off, std::string(clip));
                ClearSelection();
                OnEdit();
                RevealCaret();
                RefreshCompletionList();
            }
            return;
        }
    }

    // ---- Arrow keys (with Shift selection) ----
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) {
        int targetCol = g_caretCol;
        if (g_caretLine < CountLines(g_text) - 1) ++g_caretLine;
        g_caretCol = targetCol;
        if (shift) { StartSelection(); }
        else { ClearSelection(); }
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) {
        int targetCol = g_caretCol;
        if (g_caretLine > 0) --g_caretLine;
        g_caretCol = targetCol;
        if (shift) { StartSelection(); }
        else { ClearSelection(); }
    }
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) {
        if (g_caretCol > 0) --g_caretCol;
        else if (g_caretLine > 0) { --g_caretLine; g_caretCol = (int)GetLineText(g_text, g_caretLine).size(); }
        if (shift) { StartSelection(); }
        else { ClearSelection(); }
    }
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) {
        int len = (int)GetLineText(g_text, g_caretLine).size();
        if (g_caretCol < len) ++g_caretCol;
        else if (g_caretLine < CountLines(g_text) - 1) { ++g_caretLine; g_caretCol = 0; }
        if (shift) { StartSelection(); }
        else { ClearSelection(); }
    }
    if (IsKeyPressed(KEY_HOME)) {
        if (shift) StartSelection();
        g_caretCol = 0;
        if (!shift) ClearSelection();
    }
    if (IsKeyPressed(KEY_END)) {
        if (shift) StartSelection();
        g_caretCol = (int)GetLineText(g_text, g_caretLine).size();
        if (!shift) ClearSelection();
    }
    if (IsKeyPressed(KEY_PAGE_UP)) {
        int visible = (int)(CodeRect().height / LINE_H);
        g_caretLine -= visible;
        ClampCaret();
        if (shift) StartSelection();
        else ClearSelection();
    }
    if (IsKeyPressed(KEY_PAGE_DOWN)) {
        int visible = (int)(CodeRect().height / LINE_H);
        g_caretLine += visible;
        ClampCaret();
        if (shift) StartSelection();
        else ClearSelection();
    }

    // ---- Tab ----
    if (IsKeyPressed(KEY_TAB)) {
        if (HasSelection()) DeleteSelection();
        size_t off = LineColToOffset(g_text, g_caretLine, g_caretCol);
        InsertText(off, "    ");
        ClearSelection();
        OnEdit();
    }

    // ---- Enter ----
    if (IsKeyPressed(KEY_ENTER)) {
        if (HasSelection()) DeleteSelection();
        size_t off = LineColToOffset(g_text, g_caretLine, g_caretCol);
        InsertText(off, "\n");
        ClearSelection();
        OnEdit();
    }

    // ---- Backspace ----
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (HasSelection()) {
            DeleteSelection();
        } else {
            size_t off = LineColToOffset(g_text, g_caretLine, g_caretCol);
            if (g_caretCol > 0) {
                g_text.erase(off - 1, 1);
                --g_caretCol;
            } else if (g_caretLine > 0) {
                --g_caretLine;
                g_caretCol = (int)GetLineText(g_text, g_caretLine).size();
                size_t jo = LineColToOffset(g_text, g_caretLine, g_caretCol);
                g_text.erase(jo, 1);
            }
        }
        OnEdit();
    }

    // ---- Delete ----
    if (IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE)) {
        if (HasSelection()) {
            DeleteSelection();
        } else {
            size_t off = LineColToOffset(g_text, g_caretLine, g_caretCol);
            if (off < g_text.size()) {
                g_text.erase(off, 1);
                OnEdit();
            }
        }
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        if (g_capture) g_capture = false;
        else Close();
    }
    RevealCaret();
    RefreshCompletionList();
}

// ---- syntax highlighting ----

bool IsKeyword(const std::string& w) {
    return w == "for" || w == "while" || w == "every" || w == "instance" || w == "new" || w == "wait" ||
           w == "tick" || w == "print" || w == "rgb" || w == "true" || w == "false" ||
           w == "if" || w == "else" || w == "not" || w == "and" || w == "or" || w == "Vector3";
}

void TokenizeLine(const std::string& line, std::vector<std::pair<std::string, Color>>& spans) {
    static const Color COL_COMMENT  = { 125, 140, 125, 255 };
    static const Color COL_STRING   = { 175, 215, 140, 255 };
    static const Color COL_NUMBER   = { 130, 190, 230, 255 };
    static const Color COL_KEYWORD  = { 240, 170, 90, 255 };
    static const Color COL_WORLD    = { 150, 200, 255, 255 };
    static const Color COL_PROPERTY = { 130, 205, 190, 255 };
    static const Color COL_DEFAULT  = { 215, 220, 225, 255 };

    size_t i = 0;
    bool afterDot = false;
    while (i < line.size()) {
        char c = line[i];
        if (c == '-' && i + 1 < line.size() && line[i + 1] == '-') {
            spans.push_back({ line.substr(i), COL_COMMENT });
            return;
        }
        if (c == '"') {
            size_t j = i + 1;
            while (j < line.size() && line[j] != '"') ++j;
            spans.push_back({ line.substr(i, j - i + 1), COL_STRING });
            afterDot = false;
            i = j + 1;
            continue;
        }
        if (isdigit((unsigned char)c)) {
            size_t j = i;
            while (j < line.size() && (isdigit((unsigned char)line[j]) || line[j] == '.')) ++j;
            spans.push_back({ line.substr(i, j - i), COL_NUMBER });
            afterDot = false;
            i = j;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < line.size() && (isalnum((unsigned char)line[j]) || line[j] == '_')) ++j;
            std::string w = line.substr(i, j - i);
            Color col = COL_DEFAULT;
            if (IsKeyword(w)) col = COL_KEYWORD;
            else if (w == "self" || w == "game") col = COL_WORLD;
            else if (afterDot) col = COL_PROPERTY;
            spans.push_back({ w, col });
            afterDot = false;
            i = j;
            continue;
        }
        spans.push_back({ std::string(1, c), COL_DEFAULT });
        afterDot = (c == '.');
        ++i;
    }
}

// Rendered x-offset (relative to the text-area base) for a caret column.
// Reproduces the draw loop's span layout exactly: each span start snaps to the
// pixel grid with the same (int)(x + width + 0.5f) rounding, and columns
// inside a span measure continuously from that snapped start. The old
// approach measured the whole prefix as one string, which drifted from the
// drawn text by the rounding accumulated at every colored-span boundary.
float CaretOffsetX(const std::string& line, int col) {
    std::vector<std::pair<std::string, Color>> spans;
    TokenizeLine(line, spans);
    float x = 0.0f;
    int pos = 0;
    for (const auto& span : spans) {
        int len = (int)span.first.size();
        if (col < pos + len) {
            return x + TextWidth(span.first.substr(0, col - pos));
        }
        x = (float)(int)(x + TextWidth(span.first) + 0.5f);
        pos += len;
    }
    return x;
}

} // namespace

// ---- public interface ----

void Init() {
    if (FileExists("C:/Windows/Fonts/consola.ttf")) {
        g_codeFont = LoadFontEx("C:/Windows/Fonts/consola.ttf", (int)CODE_SIZE, nullptr, 256);
    } else if (FileExists("consola.ttf")) {
        g_codeFont = LoadFontEx("consola.ttf", (int)CODE_SIZE, nullptr, 256);
    } else {
        g_codeFont = GetFontDefault();
    }
    SetTextureFilter(g_codeFont.texture, TEXTURE_FILTER_BILINEAR);
}

void Unload() {
    if (g_codeFont.texture.id != GetFontDefault().texture.id) {
        UnloadFont(g_codeFont);
    }
    g_codeFont = { 0 };
}

void OpenObject(ScatteredObject* obj) {
    g_open = true;
    g_targetObject = obj;
    g_scriptIndex = -1;
    g_text = obj ? obj->script : std::string();
    g_dirty = false;
    g_errorLine = 0;
    g_errorMsg.clear();
    ResetCaret();
    g_capture = true;
    ClearCompletions();
    ClearSelection();
}

void OpenScript(int index) {
    g_open = true;
    g_targetObject = nullptr;
    g_scriptIndex = index;
    g_text = (index >= 0 && index < (int)flyscript::GetRuntime().scripts.size())
        ? flyscript::GetRuntime().scripts[index].source : std::string();
    g_dirty = false;
    g_errorLine = 0;
    g_errorMsg.clear();
    ResetCaret();
    g_capture = true;
    ClearCompletions();
    ClearSelection();
}

void Close() {
    g_open = false;
    g_capture = false;
    g_dragging = false;
    ClearCompletions();
    ClearSelection();
}

void CloseIfTarget(const ScatteredObject* obj) {
    if (g_open && g_targetObject == obj) Close();
}

void CloseIfIndex(int index) {
    if (g_open && g_scriptIndex == index) Close();
}

bool IsOpen() { return g_open; }
bool IsCapturingKeyboard() { return g_open && g_capture; }
bool IsMouseOverWindow(Vector2 point) { return g_open && InRect(point, WindowRect()); }

void Update() {
    if (!g_open) return;

    SyncErrorFromRuntime();

    Vector2 mouse = GetMousePosition();
    Rectangle win = WindowRect();
    bool mouseMoved = mouse.x != g_lastMousePos.x || mouse.y != g_lastMousePos.y;
    g_lastMousePos = mouse;

    // Autocomplete dropdown: hover selects an item while the mouse is moving,
    // click always accepts the item under the cursor. Once the mouse stops,
    // Up/Down arrows own the highlight (hover stops overriding it).
    if (!g_completions.empty()) {
        Rectangle panel = CompletionPanelRect();
        int n = std::min((int)g_completions.size(), 8);
        for (int i = 0; i < n; ++i) {
            Rectangle itemRec = { panel.x + 2.0f, panel.y + 4.0f + i * 20.0f, panel.width - 4.0f, 20.0f - 2.0f };
            if (CheckCollisionPointRec(mouse, itemRec)) {
                if (mouseMoved) g_completionIndex = i;
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    g_completionIndex = i;
                    AcceptCompletion();
                    return;
                }
                break;
            }
        }
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (InRect(mouse, win)) {
            g_capture = true;
            if (InRect(mouse, CloseRect())) { Close(); return; }
            if (InRect(mouse, PlayCheckRect())) { ToggleRunOnPlay(); return; }
            if (InRect(mouse, CodeRect())) {
                bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                if (shift && g_hasSelection) {
                    SetCaretFromMouse(mouse);
                } else {
                    if (!shift) SetSelectionAnchor();
                    SetCaretFromMouse(mouse);
                    if (!shift) {
                        g_selAnchorLine = g_caretLine;
                        g_selAnchorCol = g_caretCol;
                    }
                    g_hasSelection = true;
                }
                g_dragging = true;
                return;
            }
        } else {
            g_capture = false;
        }
    }

    // Release drag
    if (g_dragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        g_dragging = false;
    }

    // Drag to extend selection
    if (g_dragging && InRect(mouse, win) && InRect(mouse, CodeRect())) {
        SetCaretFromMouse(mouse);
    }

    if (InRect(mouse, CodeRect())) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
                g_scrollX -= wheel * 48.0f;
            } else {
                g_scrollLine -= (int)wheel;
            }
            ClampScroll();
        }
    }

    if (!g_capture) return;

    int key = GetCharPressed();
    while (key > 0) {
        if (key >= 32 && key <= 126) {
            if (HasSelection()) DeleteSelection();
            size_t off = LineColToOffset(g_text, g_caretLine, g_caretCol);
            InsertText(off, std::string(1, (char)key));
            ClearSelection();
            OnEdit();
        }
        key = GetCharPressed();
    }

    KeyDown();
    ClampScroll();
}

void Draw() {
    if (!g_open) return;

    Rectangle win = WindowRect();
    Rectangle code = CodeRect();
    Vector2 mouse = GetMousePosition();
    float dt = GetFrameTime();

    // Drop shadow behind the whole floating window, same offset convention
    // used by the app's other floating surfaces (context menu, output panel).
    DrawRectangleRec({ win.x + 3.0f, win.y + 3.0f, win.width, win.height }, theme::SHADOW);
    DrawRectangleRec(win, theme::BG_PANEL);
    DrawRectangleLinesEx(win, 1, theme::BORDER_STRONG);

    // --- Title bar ---
    DrawRectangleRec({ win.x, win.y, win.width, TITLE_H }, theme::BG_TITLE);
    DrawLine((int)win.x, (int)(win.y + TITLE_H), (int)(win.x + win.width), (int)(win.y + TITLE_H), theme::DIVIDER);

    if (g_dirty) DrawCircle((int)(win.x + 16), (int)(win.y + 17), 3.5f, Color{ 240, 200, 60, 255 });

    // "FLYSCRIPT" reads as a section label (matches PROPERTIES/OUTPUT headers
    // elsewhere), with the target name following in normal text.
    const char* prefix = "FLYSCRIPT";
    float px = win.x + 26.0f;
    DrawTextCode(prefix, px, win.y + 10.0f, 13.0f, theme::ACCENT);
    px += TextWidthAt(prefix, 13.0f) + 8.0f;
    std::string name = TargetName();
    DrawTextCode(name.c_str(), px, win.y + 9.0f, CODE_SIZE, theme::TEXT);

    Rectangle closeBtn = CloseRect();
    bool closeHover = InRect(mouse, closeBtn);
    g_closeHoverT += closeHover ? dt * 14.0f : -dt * 12.0f;
    if (g_closeHoverT < 0.0f) g_closeHoverT = 0.0f;
    if (g_closeHoverT > 1.0f) g_closeHoverT = 1.0f;

    DrawRectangleRounded(closeBtn, 0.25f, 4, Mix(theme::BG_WIDGET, theme::DANGER, g_closeHoverT));
    DrawRectangleLinesEx(closeBtn, closeHover ? 2 : 1, Mix(theme::BORDER_STRONG, theme::DANGER_HOVER, g_closeHoverT));
    DrawCenteredTextCode("X", closeBtn, 12.0f, theme::TEXT);

    // --- Toolbar ---
    Rectangle chk = PlayCheckRect();
    bool chkHover = InRect(mouse, chk);
    g_playHoverT += chkHover ? dt * 14.0f : -dt * 12.0f;
    if (g_playHoverT < 0.0f) g_playHoverT = 0.0f;
    if (g_playHoverT > 1.0f) g_playHoverT = 1.0f;

    bool runOnPlay = RunOnPlayValue();
    DrawRectangleRounded(chk, 0.25f, 4, Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, g_playHoverT));
    DrawRectangleLinesEx(chk, chkHover ? 2 : 1, chkHover ? theme::ACCENT : theme::BORDER);
    if (runOnPlay) {
        DrawLineEx({ chk.x + 3.0f, chk.y + 9.0f }, { chk.x + 7.0f, chk.y + 13.0f }, 2.0f, theme::SUCCESS);
        DrawLineEx({ chk.x + 7.0f, chk.y + 13.0f }, { chk.x + 14.0f, chk.y + 4.0f }, 2.0f, theme::SUCCESS);
    }
    DrawTextCode("Run on play", chk.x + 24.0f, chk.y + 2.0f, 14.0f, theme::TEXT_MUTED);
    DrawLine((int)win.x, (int)(win.y + TITLE_H + TOOLBAR_H), (int)(win.x + win.width), (int)(win.y + TITLE_H + TOOLBAR_H), theme::DIVIDER);

    // --- Code area ---
    DrawRectangleRec(code, theme::BG_PANEL);
    DrawRectangleRec({ code.x, code.y, GUTTER_W, code.height }, theme::BG_TITLE);
    DrawLine((int)(code.x + GUTTER_W), (int)code.y, (int)(code.x + GUTTER_W), (int)(code.y + code.height), theme::DIVIDER);

    int total = CountLines(g_text);
    int visible = (int)(code.height / LINE_H);
    if (visible < 1) visible = 1;

    int blink = (int)(GetTime() * 2.0);
    int line = g_scrollLine;

    // Compute selection range once per frame for drawing
    int selStartLine = -1, selStartCol = 0, selEndLine = -1, selEndCol = 0;
    if (HasSelection()) {
        GetSelectionRange(selStartLine, selStartCol, selEndLine, selEndCol);
    }

    for (int i = 0; i < visible && line < total; ++i, ++line) {
        float y = code.y + i * LINE_H;

        if (line == g_errorLine) {
            DrawRectangleRec({ code.x, y, code.width, LINE_H }, Color{ 90, 35, 35, 255 });
        } else if (line == g_caretLine) {
            DrawRectangleRec({ code.x, y, code.width, LINE_H }, theme::BG_WIDGET);
        }

        // Selection highlight
        if (HasSelection() && line >= selStartLine && line <= selEndLine) {
            std::string lineText = GetLineText(g_text, line);
            float baseX = (float)(int)(code.x + GUTTER_W + PAD_X - g_scrollX + 0.5f);
            int sc = (line == selStartLine) ? selStartCol : 0;
            int ec = (line == selEndLine) ? selEndCol : (int)lineText.size();
            float x1 = baseX + CaretOffsetX(lineText, sc);
            float x2 = baseX + CaretOffsetX(lineText, ec);
            if (x1 < code.x + code.width && x2 > code.x + GUTTER_W) {
                float hlX = fmaxf(x1, code.x + GUTTER_W);
                float hlW = fminf(x2, code.x + code.width) - hlX;
                if (hlW > 0.0f) {
                    DrawRectangleRec({ hlX, y, hlW, LINE_H }, Color{ 60, 100, 160, 80 });
                }
            }
        }

        DrawTextCode(TextFormat("%d", line + 1),
            code.x + GUTTER_W - 8.0f - TextWidth(TextFormat("%d", line + 1)),
            y + 2.0f, 13.0f, theme::TEXT_MUTED);

        std::vector<std::pair<std::string, Color>> spans;
        TokenizeLine(GetLineText(g_text, line), spans);
        // Snap the line base and each span start to the pixel grid so all
        // glyphs share one consistent grid (no jitter between colored spans).
        float x = (float)(int)(code.x + GUTTER_W + PAD_X - g_scrollX + 0.5f);
        for (const auto& span : spans) {
            if (x > code.x + code.width) break;
            if (x + TextWidth(span.first) > code.x + GUTTER_W + PAD_X) {
                DrawTextCode(span.first.c_str(), x, y + 2.0f, CODE_SIZE, span.second);
            }
            x = (float)(int)(x + TextWidth(span.first) + 0.5f);
        }
    }

    // Caret
    if (g_capture && g_caretLine >= g_scrollLine && g_caretLine < g_scrollLine + visible && (blink % 2 == 0)) {
        float cy = code.y + (g_caretLine - g_scrollLine) * LINE_H;
        std::string line = GetLineText(g_text, g_caretLine);
        float baseX = (float)(int)(code.x + GUTTER_W + PAD_X - g_scrollX + 0.5f);
        float cx = (float)(int)(baseX + CaretOffsetX(line, g_caretCol) + 0.5f);
        DrawLine((int)cx, (int)cy + 2, (int)cx, (int)cy + LINE_H - 2, theme::TEXT);
    }

    // Autocomplete dropdown: same shadow + rounded-panel treatment as the
    // app's other popups so it reads as one design language.
    if (!g_completions.empty()) {
        Rectangle panel = CompletionPanelRect();
        int n = std::min((int)g_completions.size(), 12);

        DrawRectangleRounded({ panel.x + 3.0f, panel.y + 3.0f, panel.width, panel.height }, 0.15f, 4, theme::SHADOW);
        DrawRectangleRounded(panel, 0.15f, 4, theme::BG_TITLE);
        DrawRectangleLinesEx(panel, 1, theme::BORDER_STRONG);

        for (int i = 0; i < n; ++i) {
            Rectangle itemRec = { panel.x + 2.0f, panel.y + 4.0f + i * 20.0f, panel.width - 4.0f, 20.0f - 2.0f };
            bool hovered = CheckCollisionPointRec(mouse, itemRec);
            bool selected = (i == g_completionIndex);

            if (selected) {
                DrawRectangleRec(itemRec, theme::ACCENT);
            } else if (hovered) {
                DrawRectangleRec(itemRec, theme::BG_WIDGET_HOVER);
            }

            // Same monospace font and baseline as the code text so the
            // suggestions line up with what will be typed.
            DrawTextCode(g_completions[i].c_str(), itemRec.x + 10.0f, itemRec.y + 2.0f,
                CODE_SIZE, selected ? theme::TEXT : theme::TEXT_MUTED);
        }
    }

    // --- Status bar ---
    DrawLine((int)win.x, (int)(win.y + win.height - STATUS_H), (int)(win.x + win.width), (int)(win.y + win.height - STATUS_H), theme::DIVIDER);
    DrawRectangleRec({ win.x, win.y + win.height - STATUS_H, win.width, STATUS_H }, theme::BG_TITLE);
    std::string status = "Ln " + std::to_string(g_caretLine + 1) + ", Col " + std::to_string(g_caretCol + 1);
    DrawTextCode(status.c_str(), win.x + 10.0f, win.y + win.height - STATUS_H + 4.0f, 13.0f, theme::TEXT_MUTED);
    if (!g_errorMsg.empty()) {
        std::string err = "Line " + std::to_string(g_errorLine) + ": " + g_errorMsg;
        float ew = TextWidth(err);
        DrawTextCode(err.c_str(), win.x + win.width - ew - 10.0f, win.y + win.height - STATUS_H + 4.0f, 13.0f, theme::DANGER_HOVER);
    }

    // Cursor feedback, consistent with the hand/ibeam behavior used across
    // the rest of the UI.
    if (closeHover || chkHover) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
    else if (InRect(mouse, code)) SetMouseCursor(MOUSE_CURSOR_IBEAM);
    else if (InRect(mouse, win)) SetMouseCursor(MOUSE_CURSOR_DEFAULT);
}

} // namespace scriptEditor