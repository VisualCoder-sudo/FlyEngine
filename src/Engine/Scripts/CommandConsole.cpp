#include "../../../include/Engine/Scripts/CommandConsole.hpp"
#include "../../../include/Engine/Graphics.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "../../../include/Engine/Scripts/NativeScriptHost.hpp"
#include "../../../include/Engine/Scripts/FlyScriptApi.hpp"
#include "../../../include/Engine/Backend/fcloudint.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace console {

namespace {

constexpr int MAX_INPUT = 512;
constexpr float BAR_HEIGHT = 30.0f;
constexpr float FONT_SIZE = 16.0f;
constexpr float FEEDBACK_FONT_SIZE = 14.0f;
constexpr double FEEDBACK_SUCCESS_SECONDS = 5.0;
constexpr double FEEDBACK_ERROR_SECONDS = 8.0;

struct Feedback {
    std::string text;
    bool error = false;
    double time = 0.0;
};

bool barFocused = false;
std::string input;
int cursorPos = 0;
int selectionAnchor = -1; // -1 = no selection; otherwise anchor of the [anchor, caret) range
std::vector<std::string> history;
int historyIndex = -1;
Feedback feedback;

Rectangle GetBarBounds() {
    Rectangle area = ui::GetConsoleBarArea();
    return { area.x, (float)GetScreenHeight() - BAR_HEIGHT, area.width, BAR_HEIGHT };
}

bool BarHitTest(Vector2 point) {
    return CheckCollisionPointRec(point, GetBarBounds());
}

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
    selectionAnchor = -1;
}

void BlurBar() {
    barFocused = false;
    historyIndex = -1;
    selectionAnchor = -1;
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool HasSelection() {
    return selectionAnchor >= 0 && selectionAnchor != cursorPos;
}

void DeleteSelection() {
    if (!HasSelection()) return;
    int s = std::min(selectionAnchor, cursorPos);
    int e = std::max(selectionAnchor, cursorPos);
    input.erase(s, e - s);
    cursorPos = s;
    selectionAnchor = -1;
}

void InsertCharAtCursor(char ch) {
    if (HasSelection()) DeleteSelection();
    else selectionAnchor = -1;
    input.insert(input.begin() + cursorPos, ch);
    cursorPos++;
}

void MoveCursor(int newPos, bool extend) {
    newPos = std::clamp(newPos, 0, (int)input.size());
    if (extend) {
        if (selectionAnchor < 0) selectionAnchor = cursorPos;
        cursorPos = newPos;
    } else {
        cursorPos = newPos;
        selectionAnchor = -1;
    }
}

void Backspace() {
    if (HasSelection()) {
        DeleteSelection();
        return;
    }
    selectionAnchor = -1;
    if (cursorPos > 0) {
        input.erase(cursorPos - 1, 1);
        cursorPos--;
    }
}

void DeleteForward() {
    if (HasSelection()) {
        DeleteSelection();
        return;
    }
    selectionAnchor = -1;
    if (cursorPos < (int)input.size()) input.erase(cursorPos, 1);
}

bool ShiftDown() {
    return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
}

// Maps the mouse pointer to a character index in the (possibly scrolled) input.
int CharIndexAtMouse(Font font, float textX, float visibleW, float scroll) {
    int idx = 0;
    for (int i = 0; i < (int)input.size(); ++i) {
        float mid = textX - scroll + MeasureTextEx(font, input.substr(0, i + 1).c_str(), FONT_SIZE, 1.0f).x;
        if (GetMousePosition().x <= mid) break;
        idx = i + 1;
    }
    return idx;
}

// Horizontal scroll offset (subtracted from the text origin) that keeps the
// caret, and the text around it, visible when the input is wider than the bar.
// The text region ends just left of the Run button, so the tail of a long
// command scrolls almost all the way to the button.
float ComputeScrollOffset(const std::string& s, int cursor, Font font, float visibleW) {
    const float pad = 4.0f;
    float caretX = MeasureTextEx(font, s.substr(0, cursor).c_str(), FONT_SIZE, 1.0f).x;
    float fullW = MeasureTextEx(font, s.c_str(), FONT_SIZE, 1.0f).x;
    if (fullW <= visibleW || caretX < pad) return 0.0f;
    float offset = 0.0f;
    if (caretX - offset < pad) offset = caretX - pad;
    else if (caretX - offset > visibleW - pad) offset = caretX - (visibleW - pad);
    return std::clamp(offset, 0.0f, fullW - visibleW);
}

// ---- built-in command language --------------------------------------------
//
//   help                         list commands
//   print <text>                 echo to the log
//   <Object>.<Prop>              show a property ("Model.Part.Prop" for parts)
//   <Object>.<Prop> = <value>    set it, e.g. Cube.Position = (0, 5, 0)
//   Lighting|Environment|Rendering|Camera|Physics.<Prop> [= <value>]   world settings
//   scripts                      list compiled script classes
//   run <Class> / stop <Class>   start/stop a standalone script
//   rebuild                      recompile Scripts/*.cpp
//   fcloud ...                   cloud commands
//
// An optional "game." / "game.Workspace." prefix is accepted, as in Flyscript.

constexpr const char* kHelpText =
    "Commands: help, print <text>, <Object>.<Prop> [= value], "
    "Lighting|Environment|Rendering|Camera|Physics.<Prop> [= value], scripts, run <Class>, "
    "stop <Class>, rebuild, fcloud --help";

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Case-insensitive lookup of `name` in `names`; returns the canonical spelling or "".
std::string Canonical(const std::string& name, std::initializer_list<const char*> names) {
    const std::string want = Lower(name);
    for (const char* n : names)
        if (Lower(n) == want) return n;
    return {};
}

bool StripPrefix(std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (s.size() > n && Lower(s.substr(0, n)) == Lower(prefix)) { s.erase(0, n); return true; }
    return false;
}

bool ParseBoolValue(const std::string& s, bool& out) {
    const std::string l = Lower(s);
    if (l == "true" || l == "1") { out = true; return true; }
    if (l == "false" || l == "0") { out = false; return true; }
    return false;
}

bool ParseFloatValue(const std::string& s, float& out) {
    char* end = nullptr;
    out = std::strtof(s.c_str(), &end);
    return end && end != s.c_str() && *end == '\0';
}

std::string FormatFloat(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

std::string FormatVec(float x, float y, float z) {
    return "(" + FormatFloat(x) + ", " + FormatFloat(y) + ", " + FormatFloat(z) + ")";
}

struct CommandResult {
    std::string text;
    bool error = false;
};

CommandResult WorldProperty(const std::string& group, const std::string& prop,
                            const std::string* rhs) {
    auto setBool = [&](void (*set)(short)) -> CommandResult {
        bool v;
        if (!ParseBoolValue(*rhs, v)) return { "'" + *rhs + "' is not a valid boolean (true/false)", true };
        set(v ? 1 : 0);
        return { "OK" };
    };
    auto setFloat = [&](void (*set)(float)) -> CommandResult {
        float v;
        if (!ParseFloatValue(*rhs, v)) return { "'" + *rhs + "' is not a number", true };
        set(v);
        return { "OK" };
    };
    auto boolText = [](short v) { return std::string(v ? "true" : "false"); };

    if (group == "Lighting") {
        const std::string p = Canonical(prop, { "GlobalShadows", "ShadowQuality", "Ambient" });
        if (p == "GlobalShadows")
            return rhs ? setBool(FlyNative_SetShadowsEnabled) : CommandResult{ boolText(FlyNative_GetShadowsEnabled()) };
        if (p == "ShadowQuality") {
            if (!rhs) return { std::to_string(FlyNative_GetShadowQuality()) };
            float v;
            if (!ParseFloatValue(*rhs, v)) return { "'" + *rhs + "' is not a number", true };
            FlyNative_SetShadowQuality(static_cast<int>(v));
            return { "OK" };
        }
        if (p == "Ambient") {
            if (rhs) return setFloat(FlyNative_SetAmbient);
            float a = 0.0f;
            FlyNative_GetAmbient(&a);
            return { FormatFloat(a) };
        }
    } else if (group == "Environment") {
        // The same names and numbers as Game::Environment in fly.hpp.
        struct EnvProp { const char* name; int what; };
        static const EnvProp props[] = {
            { "TimeOfDay", FLY_ENV_TIME_OF_DAY }, { "DayLength", FLY_ENV_DAY_LENGTH }, { "Overcast", FLY_ENV_OVERCAST },
            { "Rain", FLY_ENV_RAIN }, { "WetGround", FLY_ENV_WET_GROUND }, { "Fog", FLY_ENV_FOG }, { "CloudCover", FLY_ENV_CLOUD_COVER },
            { "WindSpeed", FLY_ENV_WIND_SPEED }, { "WindDirection", FLY_ENV_WIND_DIRECTION }, { "Exposure", FLY_ENV_EXPOSURE },
            { "SunIntensity", FLY_ENV_SUN_INTENSITY }, { "Bloom", FLY_ENV_BLOOM },
        };
        const std::string p = Canonical(prop, { "TimeOfDay", "DayLength", "Overcast", "Rain", "WetGround", "Fog", "CloudCover",
                                                "WindSpeed", "WindDirection", "Exposure", "SunIntensity", "Bloom" });
        for (const EnvProp& e : props) {
            if (p != e.name) continue;
            if (!rhs) return { FormatFloat(FlyNative_GetEnvironment(e.what)) };
            float v;
            if (!ParseFloatValue(*rhs, v)) return { "'" + *rhs + "' is not a number", true };
            FlyNative_SetEnvironment(e.what, v);
            return { "OK" };
        }
    } else if (group == "Rendering") {
        const std::string p = Canonical(prop, { "Grid", "Wireframe" });
        if (p == "Grid")
            return rhs ? setBool(FlyNative_SetGridVisible) : CommandResult{ boolText(FlyNative_GetGridVisible()) };
        if (p == "Wireframe")
            return rhs ? setBool(FlyNative_SetWireframe) : CommandResult{ boolText(FlyNative_GetWireframe()) };
    } else if (group == "Camera") {
        if (Canonical(prop, { "FOV" }) == "FOV") {
            if (rhs) return setFloat(FlyNative_SetFov);
            float f = 0.0f;
            FlyNative_GetFov(&f);
            return { FormatFloat(f) };
        }
    } else if (group == "Physics") {
        const std::string p = Canonical(prop, { "Gravity", "Friction", "Restitution" });
        if (p == "Gravity")
            return rhs ? setFloat(FlyNative_SetGravity) : CommandResult{ FormatFloat(FlyNative_GetGravity()) };
        if (p == "Friction")
            return rhs ? setFloat(FlyNative_SetFriction) : CommandResult{ FormatFloat(FlyNative_GetFriction()) };
        if (p == "Restitution")
            return rhs ? setFloat(FlyNative_SetRestitution) : CommandResult{ FormatFloat(FlyNative_GetRestitution()) };
    }
    return { "Unknown property '" + group + "." + prop + "'", true };
}

CommandResult ObjectProperty(ScriptRuntime& rt, const std::string& objectName,
                             const std::string& propName, const std::string* rhs) {
    const unsigned long long h = FlyNative_FindObject(objectName.c_str());
    if (!h) return { "No object named '" + objectName + "'", true };

    const std::string prop = Canonical(propName, {
        "Position", "Size", "Rotation", "Origin", "Velocity", "AngularVelocity", "Color",
        "Anchored", "CanCollide", "Mass", "Transparency", "CollisionAccuracy" });
    if (prop.empty()) return { "Unknown property '" + propName + "'", true };

    if (rhs) {
        // The handle is the object pointer, already validated by FindObject.
        auto* obj = reinterpret_cast<ScatteredObject*>(static_cast<uintptr_t>(h));
        std::string error;
        if (!rt.SetObjectProperty(obj, prop, *rhs, error)) return { error, true };
        return { "OK" };
    }

    float x = 0, y = 0, z = 0;
    if (prop == "Position")        { FlyNative_GetPosition(h, &x, &y, &z); return { FormatVec(x, y, z) }; }
    if (prop == "Size")            { FlyNative_GetSize(h, &x, &y, &z); return { FormatVec(x, y, z) }; }
    if (prop == "Rotation")        { FlyNative_GetRotation(h, &x, &y, &z); return { FormatVec(x, y, z) }; }
    if (prop == "Origin")          { FlyNative_GetOrigin(h, &x, &y, &z); return { FormatVec(x, y, z) }; }
    if (prop == "Velocity")        { FlyNative_GetVelocity(h, &x, &y, &z); return { FormatVec(x, y, z) }; }
    if (prop == "AngularVelocity") { FlyNative_GetAngularVelocity(h, &x, &y, &z); return { FormatVec(x, y, z) }; }
    if (prop == "Color") {
        unsigned char r = 0, g = 0, b = 0;
        FlyNative_GetColor(h, &r, &g, &b);
        return { "(" + std::to_string(r) + ", " + std::to_string(g) + ", " + std::to_string(b) + ")" };
    }
    if (prop == "Anchored")     return { FlyNative_GetAnchored(h) ? "true" : "false" };
    if (prop == "CanCollide")   return { FlyNative_GetCanCollide(h) ? "true" : "false" };
    if (prop == "Mass")         return { FormatFloat(FlyNative_GetMass(h)) };
    if (prop == "Transparency") return { FormatFloat(FlyNative_GetTransparency(h)) };
    static const char* kAccuracy[] = { "Box", "Hull", "Default", "Precise" };
    const int acc = FlyNative_GetCollisionAccuracy(h);
    return { acc >= 0 && acc < 4 ? kAccuracy[acc] : "?" };
}

// Finds the standalone entry running `className`, adding one if needed.
int StandaloneIndexFor(ScriptRuntime& rt, const std::string& className, bool create) {
    auto& scripts = rt.StandaloneScripts();
    for (int i = 0; i < static_cast<int>(scripts.size()); ++i)
        if (scripts[i].typeName == className) return i;
    if (!create) return -1;
    ScriptRuntime::StandaloneScript s;
    s.name = className;
    s.typeName = className;
    s.runOnPlay = false;
    scripts.push_back(std::move(s));
    return static_cast<int>(scripts.size()) - 1;
}

CommandResult Evaluate(const std::string& line) {
    if (line == "help" || line == "?") return { kHelpText };

    if (line.rfind("print ", 0) == 0) {
        ui::Log("%s", line.substr(6).c_str());
        return { "OK" };
    }

    ScriptRuntime* rt = GetActiveRuntime();
    if (!rt || !rt->HasWorld()) return { "No scene is loaded", true };
    NativeScript::NativeScriptHost* host = rt->GetHost();

    // Script management.
    if (line == "scripts") {
        if (!host || !host->ScriptsReady()) return { "No script library loaded (see the compiler output)", true };
        std::string names;
        for (const auto& n : host->GetScriptClassNames()) names += (names.empty() ? "" : ", ") + n;
        return { names.empty() ? "(no FLY_SCRIPT classes)" : names };
    }
    if (line == "rebuild") {
        if (!host) return { "Script host not available", true };
        host->RequestScriptRebuild();
        return { "Recompiling scripts..." };
    }
    if (line.rfind("run ", 0) == 0 || line.rfind("stop ", 0) == 0) {
        const bool run = line[0] == 'r';
        const std::string cls = Trim(line.substr(run ? 4 : 5));
        if (!host) return { "Script host not available", true };
        const int idx = StandaloneIndexFor(*rt, cls, run);
        if (idx < 0) return { "'" + cls + "' is not running", true };
        if (!run) { host->StopStandaloneScriptAt(idx); return { "OK" }; }
        return host->StartStandaloneScriptAt(idx) ? CommandResult{ "OK" }
                                                  : CommandResult{ "Could not start '" + cls + "' (see the log)", true };
    }

    // Property get/set.
    std::string lhs = line;
    std::string rhs;
    const size_t eq = line.find('=');
    const bool assign = eq != std::string::npos;
    if (assign) {
        lhs = Trim(line.substr(0, eq));
        rhs = Trim(line.substr(eq + 1));
        if (rhs.empty()) return { "Missing value after '='", true };
    }

    if (StripPrefix(lhs, "game.")) StripPrefix(lhs, "Workspace.");

    const size_t dot = lhs.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= lhs.size())
        return { "Unknown command '" + line + "'. Type 'help'.", true };
    const std::string target = lhs.substr(0, dot);
    const std::string prop = lhs.substr(dot + 1);

    const std::string group = Canonical(target, { "Lighting", "Environment", "Rendering", "Camera", "Physics" });
    if (!group.empty()) return WorldProperty(group, prop, assign ? &rhs : nullptr);
    return ObjectProperty(*rt, target, prop, assign ? &rhs : nullptr);
}

// Runs one console line through the built-in command language.
void ExecuteCommand(const std::string& line) {
    std::string trimmed = Trim(line);
    if (trimmed.empty()) return;

    // Native fcloud commands are dispatched to the cloud client.
    if (trimmed.rfind("fcloud", 0) == 0) {
        fcloud::DispatchCommand(trimmed);
        return;
    }

    CommandResult r = Evaluate(trimmed);
    const std::string& result = r.text;

    if (r.error) {
        feedback = { result, true, GetTime() };
        ui::Log("[console] %s", trimmed.c_str());
        ui::Log("[console] %s", result.c_str());
    } else {
        std::string text = (result == "OK") ? ("> " + trimmed) : ("> " + trimmed + "  ->  " + result);
        feedback = { text, false, GetTime() };
        ui::Log("[console] %s", trimmed.c_str());
        if (result != "OK") ui::Log("[console] -> %s", result.c_str());
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
    selectionAnchor = -1;
    historyIndex = -1;
}

} // namespace

Rectangle GetBounds() {
    return GetBarBounds();
}

bool IsActive() {
    return barFocused;
}

bool IsOverBar(Vector2 point) {
    return BarHitTest(point);
}

void Focus() {
    FocusBar();
}

void Blur() {
    BlurBar();
}

void AttachCamera(Camera3D&) {
    // Retained for API compatibility; commands reach the camera through the
    // ScriptRuntime world binding instead.
}

void Update() {
    fcloud::Update();

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && BarHitTest(GetMousePosition())) {
        FocusBar();
        // Click-to-position the cursor
        Font font = ui::GetFont();
        float promptWidth = MeasureTextEx(font, ">", FONT_SIZE, 1.0f).x + 4.0f;
        Rectangle barB = GetBarBounds();
        float textX = barB.x + 8.0f + promptWidth;
        float visibleW = (barB.width - 74.0f) - textX;
        float scroll = ComputeScrollOffset(input, cursorPos, font, visibleW);
        cursorPos = CharIndexAtMouse(font, textX, visibleW, scroll);
        selectionAnchor = cursorPos;
    }

    // Drag on the bar to extend a selection.
    if (barFocused && selectionAnchor >= 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        Font font = ui::GetFont();
        float promptWidth = MeasureTextEx(font, ">", FONT_SIZE, 1.0f).x + 4.0f;
        Rectangle barB = GetBarBounds();
        float textX = barB.x + 8.0f + promptWidth;
        float visibleW = (barB.width - 74.0f) - textX;
        float scroll = ComputeScrollOffset(input, cursorPos, font, visibleW);
        cursorPos = CharIndexAtMouse(font, textX, visibleW, scroll);
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

    // History navigation
    if (IsKeyPressed(KEY_UP) && !history.empty()) {
        if (historyIndex < 0) historyIndex = (int)history.size() - 1;
        else historyIndex = std::max(0, historyIndex - 1);
        input = history[historyIndex];
        cursorPos = (int)input.size();
        selectionAnchor = -1;
    }
    if (IsKeyPressed(KEY_DOWN) && historyIndex >= 0) {
        historyIndex++;
        if (historyIndex >= (int)history.size()) {
            historyIndex = -1;
            input.clear();
            cursorPos = 0;
            selectionAnchor = -1;
        } else {
            input = history[historyIndex];
            cursorPos = (int)input.size();
            selectionAnchor = -1;
        }
    }

    // Paste from clipboard at the cursor (Ctrl+V)
    if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && IsKeyPressed(KEY_V)) {
        const char* clip = GetClipboardText();
        if (clip && clip[0] != '\0') {
            for (const char* p = clip; *p != '\0' && (int)input.size() < MAX_INPUT; ++p) {
                if (*p == '\r' || *p == '\n' || (unsigned char)*p < 32) continue;
                InsertCharAtCursor(*p);
            }
        }
    }

    int key = GetCharPressed();
    while (key > 0) {
        if (key >= 32 && key <= 126 && (int)input.size() < MAX_INPUT &&
            !(IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))) {
            InsertCharAtCursor((char)key);
        }
        key = GetCharPressed();
    }

    bool shift = ShiftDown();
    if (IsKeyDown(KEY_BACKSPACE) && (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE))) Backspace();
    if (IsKeyDown(KEY_DELETE) && (IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE))) DeleteForward();
    if (IsKeyDown(KEY_LEFT) && (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT))) MoveCursor(cursorPos - 1, shift);
    if (IsKeyDown(KEY_RIGHT) && (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT))) MoveCursor(cursorPos + 1, shift);
    if (IsKeyPressed(KEY_HOME)) MoveCursor(0, shift);
    if (IsKeyPressed(KEY_END)) MoveCursor((int)input.size(), shift);

    if (IsKeyPressed(KEY_ENTER)) {
        RunCurrentInput();
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

    // Feedback auto-clears (errors included) and is drawn INSIDE the bar -
    // the area directly above the bar is the ImGui Asset Browser, which would
    // otherwise cover an error toast and make it look permanently stuck.
    double feedbackAge = GetTime() - feedback.time;
    double feedbackLimit = feedback.error ? FEEDBACK_ERROR_SECONDS : FEEDBACK_SUCCESS_SECONDS;
    bool feedbackVisible = !feedback.text.empty()
        && feedbackAge < feedbackLimit
        && input.empty();
    float feedbackAlpha = 1.0f;
    if (feedbackVisible && feedbackAge > feedbackLimit - 0.5) {
        feedbackAlpha = 1.0f - (float)(feedbackAge - (feedbackLimit - 0.5)) / 0.5f;
    }
    Color feedbackCol = feedback.error
        ? ColorAlpha(Color{ 255, 140, 140, 255 }, feedbackAlpha)
        : ColorAlpha(Color{ 180, 230, 190, 255 }, feedbackAlpha);

    if (!barFocused) {
        DrawTextEx(font, ">", { promptX, textY }, FONT_SIZE, 1.0f, Color{ 100, 200, 120, 255 });
        if (feedbackVisible) {
            BeginScissorMode((int)(promptX + 18.0f), (int)bar.y, (int)(bar.width - 92.0f - 18.0f), (int)bar.height);
            DrawTextEx(font, feedback.text.c_str(), { promptX + 18.0f, textY }, FEEDBACK_FONT_SIZE, 1.0f, feedbackCol);
            EndScissorMode();
        } else {
            DrawTextEx(font, "Press ` to enter a command (type help).",
                { promptX + 18.0f, textY }, FONT_SIZE, 1.0f, Color{ 150, 150, 160, 255 });
        }
    } else {
        float promptWidth = MeasureTextEx(font, ">", FONT_SIZE, 1.0f).x + 4.0f;
        float textX = promptX + promptWidth;
        float visibleW = (bar.width - 74.0f) - textX;
        float scroll = ComputeScrollOffset(input, cursorPos, font, visibleW);
        float drawX = textX - scroll;

        DrawTextEx(font, ">", { promptX, textY }, FONT_SIZE, 1.0f, Color{ 100, 200, 120, 255 });

        BeginScissorMode((int)textX, (int)bar.y, (int)visibleW, (int)bar.height);
        if (feedbackVisible) {
            DrawTextEx(font, feedback.text.c_str(), { textX, textY }, FEEDBACK_FONT_SIZE, 1.0f, feedbackCol);
        } else if (HasSelection()) {
            int s = std::min(selectionAnchor, cursorPos);
            int e = std::max(selectionAnchor, cursorPos);
            float selX0 = drawX + MeasureTextEx(font, input.substr(0, s).c_str(), FONT_SIZE, 1.0f).x;
            float selX1 = drawX + MeasureTextEx(font, input.substr(0, e).c_str(), FONT_SIZE, 1.0f).x;
            DrawRectangle((int)selX0, (int)bar.y + 4, (int)(selX1 - selX0), (int)bar.height - 8, Color{ 45, 95, 175, 255 });
            DrawTextEx(font, input.c_str(), { drawX, textY }, FONT_SIZE, 1.0f, Color{ 230, 230, 230, 255 });
            int s2 = std::min(selectionAnchor, cursorPos);
            int e2 = std::max(selectionAnchor, cursorPos);
            float selX02 = drawX + MeasureTextEx(font, input.substr(0, s2).c_str(), FONT_SIZE, 1.0f).x;
            DrawTextEx(font, input.substr(s2, e2 - s2).c_str(), { selX02, textY }, FONT_SIZE, 1.0f, Color{ 255, 255, 255, 255 });
        } else {
            DrawTextEx(font, input.c_str(), { drawX, textY }, FONT_SIZE, 1.0f, Color{ 230, 230, 230, 255 });
            std::string before = input.substr(0, cursorPos);
            float cursorX = drawX + MeasureTextEx(font, before.c_str(), FONT_SIZE, 1.0f).x;
            if ((int)(GetTime() * 2.0) % 2 == 0) {
                DrawLine((int)cursorX, (int)bar.y + 5, (int)cursorX, (int)bar.y + (int)bar.height - 5, Color{ 230, 230, 230, 255 });
            }
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
}

} // namespace console