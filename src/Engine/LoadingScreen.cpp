#include "../../include/Engine/LoadingScreen.hpp"
#include "../../include/Engine/Platform/Platform.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace loading {

Settings::Settings() {
    editor.showLogs = true;
    std::snprintf(editor.title, sizeof editor.title, "Opening {name}");
    editor.background = { 36, 38, 44, 255 };
    player.showLogs = false;
    player.showStage = false;
}

namespace {

Settings g_settings;

struct State {
    bool active = false;
    Target target = Target::Editor;
    std::string name;
    float rangeFrom = 0.0f, rangeTo = 1.0f;
    float fraction = 0.0f;
    std::string stage;
    std::deque<std::string> logs;
    double lastDraw = 0.0;
};
State g_state;

const Look& CurrentLook() { return g_state.target == Target::Editor ? g_settings.editor : g_settings.player; }

std::string ConfigFile() {
    const std::string dir = platform::ConfigDir();
    return dir.empty() ? std::string() : (std::filesystem::u8path(dir) / "loading.cfg").string();
}
std::string ProjectFile(const std::string& folder) {
    return (std::filesystem::u8path(folder) / "loading.cfg").string();
}

// Writes one Look as "<prefix>.<key> value" lines.
void WriteLook(std::ostream& out, const char* prefix, const Look& l) {
    out << prefix << ".enabled " << (l.enabled ? 1 : 0) << "\n"
        << prefix << ".percent " << (l.showPercent ? 1 : 0) << "\n"
        << prefix << ".stage " << (l.showStage ? 1 : 0) << "\n"
        << prefix << ".bar " << (l.showBar ? 1 : 0) << "\n"
        << prefix << ".logs " << (l.showLogs ? 1 : 0) << "\n"
        << prefix << ".logLines " << l.logLines << "\n"
        << prefix << ".background " << (int)l.background.r << ' ' << (int)l.background.g << ' ' << (int)l.background.b << "\n"
        << prefix << ".accent " << (int)l.accent.r << ' ' << (int)l.accent.g << ' ' << (int)l.accent.b << "\n"
        << prefix << ".title " << l.title << "\n";      // to the end of the line
}

Look* LookFor(const std::string& prefix, Settings& s) {
    if (prefix == "editor") return &s.editor;
    if (prefix == "player") return &s.player;
    return nullptr;
}

unsigned char Byte(int v) { return (unsigned char)std::clamp(v, 0, 255); }

void ReadFile(const std::string& path, Settings& s, bool playerOnly) {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        const size_t dot = line.find('.'), sp = line.find(' ');
        if (dot == std::string::npos || sp == std::string::npos || dot > sp) continue;
        const std::string prefix = line.substr(0, dot), key = line.substr(dot + 1, sp - dot - 1), rest = line.substr(sp + 1);
        if (playerOnly && prefix != "player") continue;
        Look* l = LookFor(prefix, s);
        if (!l) continue;
        std::istringstream v(rest);
        int a = 0, b = 0, c = 0;
        if (key == "title") {
            std::snprintf(l->title, sizeof l->title, "%s", rest.c_str());
        } else if (key == "background" || key == "accent") {
            if (!(v >> a >> b >> c)) continue;
            (key == "background" ? l->background : l->accent) = Color{ Byte(a), Byte(b), Byte(c), 255 };
        } else if (v >> a) {
            if (key == "enabled") l->enabled = a != 0;
            else if (key == "percent") l->showPercent = a != 0;
            else if (key == "stage") l->showStage = a != 0;
            else if (key == "bar") l->showBar = a != 0;
            else if (key == "logs") l->showLogs = a != 0;
            else if (key == "logLines") l->logLines = std::clamp(a, 3, 16);
        }
    }
}

// --- drawing ---------------------------------------------------------------------------------------------------
Font g_font{};
bool g_fontTried = false;

const Font* UiFont() {
    if (!g_fontTried) {
        g_fontTried = true;
        const std::string& path = platform::ResolveFontPath();
        if (!path.empty()) {
            Font f = LoadFontEx(path.c_str(), 64, nullptr, 0);
            if (f.texture.id != 0) { g_font = f; SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR); }
        }
    }
    return g_font.texture.id != 0 ? &g_font : nullptr;
}

float TextWidth(const std::string& s, float size) {
    if (const Font* f = UiFont()) return MeasureTextEx(*f, s.c_str(), size, 0.5f).x;
    return (float)MeasureText(s.c_str(), (int)size);
}
void Text(const std::string& s, float x, float y, float size, Color c) {
    if (const Font* f = UiFont()) DrawTextEx(*f, s.c_str(), { x, y }, size, 0.5f, c);
    else DrawText(s.c_str(), (int)x, (int)y, (int)size, c);
}
void TextCentered(const std::string& s, float cx, float y, float size, Color c) { Text(s, cx - TextWidth(s, size) * 0.5f, y, size, c); }

std::string Title(const Look& l) {
    std::string t = l.title;
    const size_t at = t.find("{name}");
    if (at != std::string::npos) t.replace(at, 6, g_state.name);
    return t;
}

Color Mix(Color a, Color b, float t) {
    return Color{ (unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t), (unsigned char)(a.b + (b.b - a.b) * t), 255 };
}

void Draw() {
    const Look& l = CurrentLook();
    const float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    if (W < 8.0f || H < 8.0f) return;
    const Color bg = l.background;
    // Light backgrounds get dark text.
    const bool lightBg = (bg.r * 0.3f + bg.g * 0.59f + bg.b * 0.11f) > 140.0f;
    const Color ink = lightBg ? Color{ 20, 22, 26, 255 } : Color{ 235, 240, 245, 255 };
    const Color dim = Mix(bg, ink, 0.55f);
    const Color track = Mix(bg, ink, 0.14f);

    BeginDrawing();
    ClearBackground(bg);

    const float cx = W * 0.5f;
    const bool logs = l.showLogs && !g_state.logs.empty();
    float y = H * (logs ? 0.26f : 0.40f);
    const float scale = std::clamp(H / 720.0f, 0.8f, 1.8f);

    const std::string title = Title(l);
    if (!title.empty()) { TextCentered(title, cx, y, 30.0f * scale, ink); y += 52.0f * scale; }
    if (l.showPercent) {
        char pct[16];
        std::snprintf(pct, sizeof pct, "%d%%", (int)(g_state.fraction * 100.0f + 0.5f));
        TextCentered(pct, cx, y, 64.0f * scale, l.accent);
        y += 84.0f * scale;
    }
    if (l.showBar) {
        const float bw = std::min(W * 0.5f, 620.0f * scale), bh = 8.0f * scale;
        const Rectangle r = { cx - bw * 0.5f, y, bw, bh };
        DrawRectangleRounded(r, 1.0f, 8, track);
        const float fill = std::max(bw * g_state.fraction, bh);
        if (g_state.fraction > 0.001f) DrawRectangleRounded({ r.x, r.y, fill, bh }, 1.0f, 8, l.accent);
        y += bh + 16.0f * scale;
    }
    if (l.showStage && !g_state.stage.empty()) { TextCentered(g_state.stage, cx, y, 18.0f * scale, dim); y += 30.0f * scale; }

    if (logs) {
        const int n = std::clamp(l.logLines, 3, 16);
        const float lh = 19.0f * scale, size = 14.0f * scale;
        const float boxW = std::min(W * 0.76f, 900.0f * scale), boxH = lh * (float)n + 18.0f * scale;
        const float bx = cx - boxW * 0.5f, by = std::max(y + 14.0f * scale, H - boxH - 36.0f * scale);
        DrawRectangleRounded({ bx, by, boxW, boxH }, 0.06f, 8, Mix(bg, lightBg ? Color{ 255, 255, 255, 255 } : Color{ 0, 0, 0, 255 }, 0.35f));
        const int first = std::max(0, (int)g_state.logs.size() - n);
        float ly = by + 9.0f * scale;
        for (int i = first; i < (int)g_state.logs.size(); ++i) {
            const bool newest = i == (int)g_state.logs.size() - 1;
            std::string line = g_state.logs[(size_t)i];
            while (line.size() > 4 && TextWidth(line, size) > boxW - 24.0f * scale) line.resize(line.size() - 4), line += "...";
            Text(line, bx + 12.0f * scale, ly, size, newest ? ink : dim);
            ly += lh;
        }
    }
    EndDrawing();
    g_state.lastDraw = GetTime();
}

} // namespace

Settings& GetSettings() { return g_settings; }

void Load(const std::string& projectFolder) {
    g_settings = Settings{};
    const std::string machine = ConfigFile();
    if (!machine.empty()) ReadFile(machine, g_settings, false);
    if (!projectFolder.empty()) ReadFile(ProjectFile(projectFolder), g_settings, true);
}

void Save() {
    const std::string path = ConfigFile();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    WriteLook(out, "editor", g_settings.editor);
    WriteLook(out, "player", g_settings.player);
}

bool SaveProjectOverride(const std::string& folder) {
    if (folder.empty()) return false;
    std::ofstream out(ProjectFile(folder), std::ios::trunc);
    if (!out) return false;
    out << "# The loading screen of this project's player (Preferences > Loading in the editor).\n";
    WriteLook(out, "player", g_settings.player);
    return static_cast<bool>(out);
}

bool RemoveProjectOverride(const std::string& folder) {
    std::error_code ec;
    return !folder.empty() && std::filesystem::remove(std::filesystem::u8path(ProjectFile(folder)), ec);
}

bool HasProjectOverride(const std::string& folder) {
    std::error_code ec;
    return !folder.empty() && std::filesystem::exists(std::filesystem::u8path(ProjectFile(folder)), ec);
}

void Begin(Target target, const std::string& projectName) {
    g_state = State{};
    g_state.active = true;
    g_state.target = target;
    g_state.name = projectName;
    if (CurrentLook().enabled && IsWindowReady()) Draw();
}

void Range(float from, float to) {
    g_state.rangeFrom = std::clamp(from, 0.0f, 1.0f);
    g_state.rangeTo = std::clamp(to, g_state.rangeFrom, 1.0f);
}

void Progress(float fraction, const char* stage) {
    if (!g_state.active) return;
    const float f = g_state.rangeFrom + (g_state.rangeTo - g_state.rangeFrom) * std::clamp(fraction, 0.0f, 1.0f);
    g_state.fraction = std::max(g_state.fraction, f);       // never goes back
    const bool newStage = stage && g_state.stage != stage;
    if (stage) g_state.stage = stage;
    if (!CurrentLook().enabled || !IsWindowReady()) return;
    // A new stage is shown at once; the fine steps within one only about 30 times a second.
    if (GetTime() - g_state.lastDraw >= (newStage ? 0.008 : 0.033)) Draw();
}

void AddLogLine(const char* text) {
    if (!g_state.active || !text) return;
    g_state.logs.emplace_back(text);
    while (g_state.logs.size() > 64) g_state.logs.pop_front();
}

void Log(const char* fmt, ...) {
    if (!g_state.active) return;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    AddLogLine(buf);
    std::printf("%s\n", buf);
    if (CurrentLook().enabled && CurrentLook().showLogs && IsWindowReady() && GetTime() - g_state.lastDraw >= 0.033) Draw();
}

void Redraw() {
    if (g_state.active && CurrentLook().enabled && IsWindowReady()) Draw();
}

void End() {
    if (!g_state.active) return;
    g_state.fraction = 1.0f;
    g_state.stage.clear();
    if (CurrentLook().enabled && IsWindowReady()) Draw();
    g_state.active = false;
}

bool Active() { return g_state.active; }
float Fraction() { return g_state.fraction; }

} // namespace loading
