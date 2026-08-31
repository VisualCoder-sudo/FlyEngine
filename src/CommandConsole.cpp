#include "CommandConsole.hpp"
#include "Graphics.hpp"
#include "Flyscript.hpp"
#include "ScriptEditor.hpp"
#include "ui.hpp"
#include <algorithm>
#include <string>
#include <vector>
#include <cctype>
#include <cstdlib>

namespace console {

namespace {

constexpr int MAX_INPUT = 256;
constexpr float BAR_HEIGHT = 30.0f;
constexpr float FONT_SIZE = 16.0f;
constexpr float FEEDBACK_FONT_SIZE = 14.0f;
constexpr float DROPDOWN_ROW_HEIGHT = 22.0f;
constexpr int DROPDOWN_MAX_ROWS = 8;

struct Feedback {
    std::string text;
    bool error = false;
    double time = 0.0;
};

bool barFocused = false;
std::string input;
int cursorPos = 0;
std::vector<std::string> history;
int historyIndex = -1;
Feedback feedback;

Camera3D* attachedCamera = nullptr;

// Autocomplete state
std::vector<std::string> completions;
int completionIndex = 0;

Rectangle GetBarBounds() {
    Rectangle area = ui::GetConsoleBarArea();
    return { area.x, (float)GetScreenHeight() - BAR_HEIGHT, area.width, BAR_HEIGHT };
}

// Bounds of the autocomplete dropdown (empty rect when nothing is shown).
Rectangle GetDropdownBounds() {
    if (completions.empty()) return { 0, 0, 0, 0 };
    int n = std::min((int)completions.size(), DROPDOWN_MAX_ROWS);
    float width = std::min(460.0f, (float)GetScreenWidth() - 16.0f);
    float height = n * DROPDOWN_ROW_HEIGHT + 8.0f;
    float x = GetBarBounds().x + 8.0f;
    float y = GetBarBounds().y - height - 4.0f;
    return { x, y, width, height };
}

bool BarHitTest(Vector2 point) {
    return CheckCollisionPointRec(point, GetBarBounds());
}

bool DropdownHitTest(Vector2 point) {
    Rectangle dd = GetDropdownBounds();
    return dd.width > 0 && CheckCollisionPointRec(point, dd);
}

void RefreshCompletions(); // defined below; used by FocusBar

Rectangle GetRunButtonBounds() {
    Rectangle bar = GetBarBounds();
    return { bar.x + bar.width - 68.0f, bar.y + 4.0f, 60.0f, bar.height - 8.0f };
}

bool RunButtonHitTest(Vector2 point) {
    return CheckCollisionPointRec(point, GetRunButtonBounds());
}

void FocusBar() {
    if (barFocused) return;
    ui::BlurAllInput();
    barFocused = true;
    cursorPos = (int)input.size();
    RefreshCompletions();
}

void BlurBar() {
    barFocused = false;
    historyIndex = -1;
    completions.clear();
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Rebuilds the candidate list from whatever the user has typed so far. The
// vocabulary and matching rules live in flyscript::GetCompletions so the
// command console and the script editor offer identical autocomplete.
void RefreshCompletions() {
    completions = flyscript::GetCompletions(input);
    completionIndex = 0;
}

// Applies the currently highlighted completion to the input.
void AcceptCompletion() {
    if (completions.empty()) return;
    if (completionIndex < 0 || completionIndex >= (int)completions.size()) completionIndex = 0;
    const std::string& pick = completions[completionIndex];

    // Replace only the identifier being completed, keeping the path/value
    // context typed so far.
    size_t wordStart = flyscript::CompletionWordStart(input);
    input = input.substr(0, wordStart) + pick;
    cursorPos = (int)input.size();
    RefreshCompletions();
}

// Executes a command line of the form: <path>.<property> = <value>.
// The actual dispatch lives in the shared Flyscript executor so the console
// and scripts understand the exact same command language.
void ExecuteCommand(const std::string& line) {
    std::string trimmed = Trim(line);
    if (trimmed.empty()) return;

    size_t eq = trimmed.find('=');
    if (eq == std::string::npos) {
        feedback = { "Syntax: <path>.<property> = <value>", true, GetTime() };
        return;
    }

    std::string lhs = Trim(trimmed.substr(0, eq));
    std::string rhs = Trim(trimmed.substr(eq + 1));

    size_t dot = lhs.rfind('.');
    if (dot == std::string::npos) {
        feedback = { "Syntax: <path>.<property> = <value>", true, GetTime() };
        return;
    }

    std::string path = Trim(lhs.substr(0, dot));
    std::string property = Trim(lhs.substr(dot + 1));

    if (path.empty() || property.empty() || rhs.empty()) {
        feedback = { "Syntax: <path>.<property> = <value>", true, GetTime() };
        return;
    }

    if (!flyscript::IsRuntimeReady()) {
        feedback = { "Script runtime not ready", true, GetTime() };
        return;
    }

    std::string error;
    if (flyscript::ExecuteAssignment(path, property, rhs, nullptr, flyscript::GetRuntime(), error)) {
        feedback = { lhs + " = " + rhs, false, GetTime() };
        ui::Log("%s = %s", lhs.c_str(), rhs.c_str());
    } else {
        feedback = { error, true, GetTime() };
        ui::Log("%s", error.c_str());
    }
}

// Runs the current input, stores it in history, and clears the field.
void RunCurrentInput() {
    if (!input.empty()) {
        if (history.empty() || history.back() != input) {
            history.push_back(input);
            if ((int)history.size() > 50) history.erase(history.begin());
        }
        ExecuteCommand(input);
    }
    input.clear();
    cursorPos = 0;
    historyIndex = -1;
    completions.clear();
}

} // namespace

Rectangle GetBounds() {
    Rectangle bar = GetBarBounds();
    Rectangle dd = GetDropdownBounds();
    if (dd.width > 0 && dd.y < bar.y) {
        bar.height += (bar.y - dd.y);
        bar.y = dd.y;
    }
    return bar;
}

bool IsActive() {
    return barFocused;
}

bool IsOverBar(Vector2 point) {
    return BarHitTest(point) || DropdownHitTest(point);
}

void Focus() {
    FocusBar();
}

void Blur() {
    BlurBar();
}

void AttachCamera(Camera3D& cam) {
    attachedCamera = &cam;
}

void Update() {
    // The script editor owns the keyboard while it has focus.
    if (scriptEditor::IsCapturingKeyboard()) return;

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && BarHitTest(GetMousePosition())) {
        FocusBar();
        // Click-to-position the cursor
        Font font = ui::GetFont();
        float promptWidth = MeasureTextEx(font, ">", FONT_SIZE, 1.0f).x + 4.0f;
        float textX = GetBarBounds().x + 8.0f + promptWidth;
        cursorPos = 0;
        for (int i = 0; i < (int)input.size(); ++i) {
            float mid = textX + MeasureTextEx(font, input.substr(0, i + 1).c_str(), FONT_SIZE, 1.0f).x;
            if (GetMousePosition().x <= mid) break;
            cursorPos = i + 1;
        }
    }

    // Autocomplete dropdown: hover selects an item, click accepts it.
    if (barFocused && !completions.empty()) {
        Rectangle dd = GetDropdownBounds();
        Vector2 mouse = GetMousePosition();
        int n = std::min((int)completions.size(), DROPDOWN_MAX_ROWS);
        for (int i = 0; i < n; ++i) {
            Rectangle itemRec = { dd.x + 2.0f, dd.y + 4.0f + i * DROPDOWN_ROW_HEIGHT, dd.width - 4.0f, DROPDOWN_ROW_HEIGHT - 2.0f };
            if (CheckCollisionPointRec(mouse, itemRec)) {
                completionIndex = i;
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    AcceptCompletion();
                }
                break;
            }
        }
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && RunButtonHitTest(GetMousePosition())) {
        RunCurrentInput();
    }

    if (!barFocused) {
        if (IsKeyPressed(KEY_GRAVE)) {
            FocusBar();
            while (GetCharPressed() > 0) {} // discard the pending '`' character
        }
        return;
    }

    if (IsKeyPressed(KEY_GRAVE)) {
        BlurBar();
        return;
    }

    // History navigation (only when the autocomplete list is closed)
    if (completions.empty()) {
        if (IsKeyPressed(KEY_UP) && !history.empty()) {
            if (historyIndex < 0) historyIndex = (int)history.size() - 1;
            else historyIndex = std::max(0, historyIndex - 1);
            input = history[historyIndex];
            cursorPos = (int)input.size();
            RefreshCompletions();
        }
        if (IsKeyPressed(KEY_DOWN) && historyIndex >= 0) {
            historyIndex++;
            if (historyIndex >= (int)history.size()) {
                historyIndex = -1;
                input.clear();
                cursorPos = 0;
            } else {
                input = history[historyIndex];
                cursorPos = (int)input.size();
            }
            RefreshCompletions();
        }
    } else {
        // Arrow keys move the autocomplete highlight
        if (IsKeyPressed(KEY_UP)) {
            completionIndex = (completionIndex - 1 + (int)completions.size()) % (int)completions.size();
        }
        if (IsKeyPressed(KEY_DOWN)) {
            completionIndex = (completionIndex + 1) % (int)completions.size();
        }
    }

    bool inputChanged = false;
    int key = GetCharPressed();
    while (key > 0) {
        if (key >= 32 && key <= 126 && (int)input.size() < MAX_INPUT) {
            input.insert(input.begin() + cursorPos, (char)key);
            cursorPos++;
            inputChanged = true;
        }
        key = GetCharPressed();
    }
    if (inputChanged) RefreshCompletions();

    if (IsKeyPressed(KEY_TAB) && !completions.empty()) {
        AcceptCompletion();
    }

    if (IsKeyPressed(KEY_BACKSPACE) && cursorPos > 0) {
        input.erase(cursorPos - 1, 1);
        cursorPos--;
        RefreshCompletions();
    }
    if (IsKeyPressed(KEY_DELETE) && cursorPos < (int)input.size()) {
        input.erase(cursorPos, 1);
        RefreshCompletions();
    }
    if (IsKeyPressed(KEY_LEFT) && cursorPos > 0) cursorPos--;
    if (IsKeyPressed(KEY_RIGHT) && cursorPos < (int)input.size()) cursorPos++;
    if (IsKeyPressed(KEY_HOME)) cursorPos = 0;
    if (IsKeyPressed(KEY_END)) cursorPos = (int)input.size();

    if (IsKeyPressed(KEY_ENTER)) {
        // Enter accepts the highlighted completion if one is open and doesn't
        // exactly match the input yet; otherwise it runs the command.
        if (!completions.empty() && completions[completionIndex] != input) {
            AcceptCompletion();
        } else {
            RunCurrentInput();
        }
    }

    if (IsKeyPressed(KEY_ESCAPE)) BlurBar();
}

void Draw() {
    Rectangle bar = GetBarBounds();
    Font font = ui::GetFont();
    bool over = BarHitTest(GetMousePosition());

    Color bg = barFocused ? Color{ 40, 45, 55, 255 } : Color{ 25, 25, 30, 255 };
    Color border = barFocused ? Color{ 0, 120, 215, 255 } : (over ? Color{ 100, 100, 110, 255 } : Color{ 70, 70, 80, 255 });

    DrawRectangleRec(bar, bg);
    DrawRectangleLinesEx(bar, barFocused ? 2 : 1, border);

    float promptX = bar.x + 8.0f;
    float textY = bar.y + (bar.height - FONT_SIZE) * 0.5f;

    if (!barFocused) {
        DrawTextEx(font, ">", { promptX, textY }, FONT_SIZE, 1.0f, Color{ 100, 200, 120, 255 });
        DrawTextEx(font, "Press ` to quick open and execute commands.",
            { promptX + 18.0f, textY }, FONT_SIZE, 1.0f, Color{ 150, 150, 160, 255 });
    } else {
        float promptWidth = MeasureTextEx(font, ">", FONT_SIZE, 1.0f).x + 4.0f;
        float textX = promptX + promptWidth;

        DrawTextEx(font, ">", { promptX, textY }, FONT_SIZE, 1.0f, Color{ 100, 200, 120, 255 });

        BeginScissorMode((int)textX, (int)bar.y, (int)(bar.width - textX - 8.0f - 76.0f), (int)bar.height);
        DrawTextEx(font, input.c_str(), { textX, textY }, FONT_SIZE, 1.0f, Color{ 230, 230, 230, 255 });

        std::string before = input.substr(0, cursorPos);
        float cursorX = textX + MeasureTextEx(font, before.c_str(), FONT_SIZE, 1.0f).x;
        if ((int)(GetTime() * 2.0) % 2 == 0) {
            DrawLine((int)cursorX, (int)bar.y + 5, (int)cursorX, (int)bar.y + (int)bar.height - 5, Color{ 230, 230, 230, 255 });
        }
        EndScissorMode();
    }

    // Run button on the right edge of the bar
    Rectangle runBtn = GetRunButtonBounds();
    bool overRun = RunButtonHitTest(GetMousePosition());
    Color btnBg = overRun ? Color{ 0, 120, 215, 255 } : Color{ 45, 50, 60, 255 };
    DrawRectangleRounded(runBtn, 0.25f, 4, btnBg);
    Vector2 btnLabel = MeasureTextEx(font, "Run", FONT_SIZE, 1.0f);
    DrawTextEx(font, "Run", { runBtn.x + (runBtn.width - btnLabel.x) * 0.5f,
                              runBtn.y + (runBtn.height - FONT_SIZE) * 0.5f },
        FONT_SIZE, 1.0f, Color{ 230, 230, 230, 255 });

    // Autocomplete dropdown (drawn last so it sits on top of the bar)
    if (barFocused && !completions.empty()) {
        Rectangle dd = GetDropdownBounds();
        int n = std::min((int)completions.size(), DROPDOWN_MAX_ROWS);

        DrawRectangleRounded(dd, 0.15f, 4, Color{ 55, 60, 72, 255 });
        DrawRectangleLinesEx(dd, 1, Color{ 90, 100, 120, 255 });

        Vector2 mouse = GetMousePosition();
        for (int i = 0; i < n; ++i) {
            Rectangle itemRec = { dd.x + 2.0f, dd.y + 4.0f + i * DROPDOWN_ROW_HEIGHT, dd.width - 4.0f, DROPDOWN_ROW_HEIGHT - 2.0f };
            bool hovered = CheckCollisionPointRec(mouse, itemRec);
            bool selected = (i == completionIndex);

            if (selected) {
                DrawRectangleRec(itemRec, Color{ 0, 120, 215, 255 });
            } else if (hovered) {
                DrawRectangleRec(itemRec, Color{ 75, 82, 100, 255 });
            }

            DrawTextEx(font, completions[i].c_str(),
                { itemRec.x + 10.0f, itemRec.y + (itemRec.height - FONT_SIZE) * 0.5f + 1.0f },
                FONT_SIZE, 1.0f, selected ? Color{ 255, 255, 255, 255 } : Color{ 210, 215, 225, 255 });
        }
    }

    // Feedback toast above the bar
    if (!feedback.text.empty()) {
        double age = GetTime() - feedback.time;
        bool sticky = feedback.error; // errors stay until the next command
        if (age < 5.0 || sticky) {
            float alpha = 1.0f;
            if (!sticky && age > 4.5f) alpha = 1.0f - (float)(age - 4.5f) / 0.5f;

            Vector2 size = MeasureTextEx(font, feedback.text.c_str(), FEEDBACK_FONT_SIZE, 1.0f);
            float panelW = std::min(size.x + 20.0f, (float)GetScreenWidth() - 20.0f);
            Rectangle panel = { bar.x + 10.0f, bar.y - 28.0f, panelW, 24.0f };

            Color panelBg = feedback.error ? ColorAlpha(Color{ 90, 25, 25, 255 }, alpha) : ColorAlpha(Color{ 25, 50, 30, 255 }, alpha);
            Color textCol = feedback.error ? ColorAlpha(Color{ 255, 140, 140, 255 }, alpha) : ColorAlpha(Color{ 180, 230, 190, 255 }, alpha);

            DrawRectangleRounded(panel, 0.2f, 4, panelBg);
            DrawTextEx(font, feedback.text.c_str(), { panel.x + 10.0f, panel.y + 4.0f }, FEEDBACK_FONT_SIZE, 1.0f, textCol);
        }
    }
}

} // namespace console
