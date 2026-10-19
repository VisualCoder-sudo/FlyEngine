#include "../../include/Engine/GameUI.hpp"
#include "../../include/Graphics/Vfs.hpp"
#include "../../include/Engine/Platform/Platform.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <map>
#include <vector>

namespace gameui {
namespace {

struct Widget {
    std::string id;
    Kind kind = Kind::Panel;
    Anchor anchor = Anchor::TopLeft;
    float x = 0, y = 0, w = 0, h = 0;
    Color color = { 255, 255, 255, 255 };
    Color color2 = { 0, 0, 0, 160 };
    std::string text;
    std::string font;
    float fontSize = 24.0f;
    float value = 0.0f;
    std::string image;
    bool visible = true;
};

std::vector<Widget> g_widgets;                 // creation order is draw order
std::map<std::string, Texture2D> g_textures;   // loaded when first drawn
std::map<std::string, Font> g_fonts;           // by file path; id 0 = could not be loaded
std::string g_projectPath;
std::map<std::string, std::string> g_fontPaths;   // font name -> resolved file (also misses); cleared when the project path changes

Widget* Find(const std::string& id) {
    for (Widget& w : g_widgets) if (w.id == id) return &w;
    return nullptr;
}

Texture2D TextureFor(const std::string& file) {
    auto it = g_textures.find(file);
    if (it != g_textures.end()) return it->second;
    std::string path = file;
    if (!std::filesystem::path(path).is_absolute() && !vfs::Handles(path) && !g_projectPath.empty())
        path = g_projectPath + "/" + path;
    Texture2D tex = LoadTexture(path.c_str());   // a missing file gives id 0; remembered so it is not retried every frame
    g_textures[file] = tex;
    return tex;
}

std::string FirstExisting(std::initializer_list<const char*> files) {
    for (const char* f : files) if (FileExists(f)) return f;
    return {};
}

// The file a font name stands for ("" = the blocky built-in font).
std::string ResolveFontPathFor(const std::string& name) {
    if (name == "Pixel") return {};
    if (name.empty() || name == "Sans") return platform::ResolveFontPath();
    std::string generic;
    if (name == "Serif") {
#if defined(_WIN32)
        generic = FirstExisting({ "C:/Windows/Fonts/times.ttf", "C:/Windows/Fonts/georgia.ttf" });
#else
        generic = FirstExisting({ "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf", "/usr/share/fonts/TTF/DejaVuSerif.ttf", "/usr/share/fonts/dejavu/DejaVuSerif.ttf",
                                  "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf", "/usr/share/fonts/liberation-serif/LiberationSerif-Regular.ttf" });
#endif
        return generic.empty() ? platform::ResolveFontPath() : generic;
    }
    if (name == "Mono") {
#if defined(_WIN32)
        generic = FirstExisting({ "C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/cour.ttf" });
#else
        generic = FirstExisting({ "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf", "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
                                  "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf", "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf" });
#endif
        return generic.empty() ? platform::ResolveFontPath() : generic;
    }
    if (!std::filesystem::path(name).is_absolute() && !vfs::Handles(name) && !g_projectPath.empty()) {
        if (name.find('/') == std::string::npos && name.find('\\') == std::string::npos) {   // "Roboto.ttf": from assets/Fonts
            for (const char* dir : { "assets/Fonts/", "assets/fonts/" })
                if (FileExists((g_projectPath + "/" + dir + name).c_str())) return g_projectPath + "/" + dir + name;
        }
        return g_projectPath + "/" + name;
    }
    return name;
}

// Cached: resolving stats the disk, and this runs for every text widget every frame.
std::string FontPathFor(const std::string& name) {
    const auto it = g_fontPaths.find(name);
    if (it != g_fontPaths.end()) return it->second;
    return g_fontPaths.emplace(name, ResolveFontPathFor(name)).first->second;
}

// The loaded font for a name, or null for the built-in blocky one. Letters, digits and Latin-1 (accents) are built in.
const Font* FontFor(const std::string& name) {
    const std::string path = FontPathFor(name);
    if (path.empty()) return nullptr;
    auto it = g_fonts.find(path);
    if (it == g_fonts.end()) {
        std::vector<int> codepoints;
        for (int c = 32; c <= 126; ++c) codepoints.push_back(c);
        for (int c = 160; c <= 255; ++c) codepoints.push_back(c);
        Font f = LoadFontEx(path.c_str(), 64, codepoints.data(), static_cast<int>(codepoints.size()));
        if (f.texture.id != 0) SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
        else f = Font{};
        it = g_fonts.emplace(path, f).first;
    }
    return it->second.texture.id != 0 ? &it->second : nullptr;
}

} // namespace

float ScaleFor(float areaHeight) { return std::clamp(areaHeight / kReferenceHeight, 0.25f, 4.0f); }

Rectangle LayoutRect(Anchor anchor, float offsetX, float offsetY, float width, float height, Rectangle area, float scale) {
    if (anchor == Anchor::Fill)
        return { area.x + offsetX * scale, area.y + offsetY * scale, area.width - 2.0f * offsetX * scale, area.height - 2.0f * offsetY * scale };
    const float w = width * scale, h = height * scale;
    const int a = static_cast<int>(anchor);
    const int col = a % 3, row = a / 3;   // 0 left/top, 1 center, 2 right/bottom
    float x = area.x, y = area.y;
    if (col == 1) x += (area.width - w) * 0.5f; else if (col == 2) x += area.width - w;
    if (row == 1) y += (area.height - h) * 0.5f; else if (row == 2) y += area.height - h;
    return { x + offsetX * scale, y + offsetY * scale, w, h };
}

bool Create(const std::string& id, Kind kind) {
    if (id.empty()) return false;
    if (Widget* w = Find(id)) {
        if (w->kind == kind) return true;
        const size_t at = static_cast<size_t>(w - g_widgets.data());
        g_widgets[at] = Widget{};
        g_widgets[at].id = id;
        g_widgets[at].kind = kind;
        return true;
    }
    Widget w;
    w.id = id;
    w.kind = kind;
    if (kind == Kind::Panel) w.color = { 0, 0, 0, 140 };
    g_widgets.push_back(std::move(w));
    return true;
}

void SetLayout(const std::string& id, Anchor anchor, float x, float y, float w, float h) {
    if (Widget* wd = Find(id)) { wd->anchor = anchor; wd->x = x; wd->y = y; wd->w = w; wd->h = h; }
}
void SetColor(const std::string& id, Color color, Color color2) {
    if (Widget* wd = Find(id)) { wd->color = color; wd->color2 = color2; }
}
void SetPrimaryColor(const std::string& id, Color color) {
    if (Widget* wd = Find(id)) wd->color = color;
}
void SetText(const std::string& id, const std::string& text, float fontSize) {
    if (Widget* wd = Find(id)) { wd->text = text; if (fontSize > 0.0f) wd->fontSize = fontSize; }
}
void SetFont(const std::string& id, const std::string& font) {
    if (Widget* wd = Find(id)) wd->font = font;
}
void SetValue(const std::string& id, float value) {
    if (Widget* wd = Find(id)) wd->value = std::clamp(value, 0.0f, 1.0f);
}
void SetImage(const std::string& id, const std::string& file) {
    if (Widget* wd = Find(id)) wd->image = file;
}
void SetVisible(const std::string& id, bool visible) {
    if (Widget* wd = Find(id)) wd->visible = visible;
}
void Remove(const std::string& id) {
    g_widgets.erase(std::remove_if(g_widgets.begin(), g_widgets.end(), [&](const Widget& w) { return w.id == id; }), g_widgets.end());
}
size_t Count() { return g_widgets.size(); }

void Clear() {
    g_widgets.clear();
    if (IsWindowReady()) for (auto& [file, tex] : g_textures) if (tex.id != 0) UnloadTexture(tex);
    g_textures.clear();
    if (IsWindowReady()) for (auto& [file, font] : g_fonts) if (font.texture.id != 0) UnloadFont(font);
    g_fonts.clear();
}

bool Get(const std::string& id, Info& out) {
    const Widget* w = Find(id);
    if (!w) return false;
    out.kind = w->kind; out.anchor = w->anchor;
    out.x = w->x; out.y = w->y; out.w = w->w; out.h = w->h;
    out.color = w->color; out.color2 = w->color2;
    out.text = w->text; out.font = w->font; out.image = w->image;
    out.fontSize = w->fontSize; out.value = w->value; out.visible = w->visible;
    return true;
}

void SetProjectPath(const std::string& path) { g_projectPath = path; g_fontPaths.clear(); }

std::vector<std::string> ListFonts(const std::string& projectPath) {
    static std::string cachedFor;
    static std::vector<std::string> cached;
    static std::chrono::steady_clock::time_point cachedAt;
    const auto now = std::chrono::steady_clock::now();
    if (!cached.empty() && cachedFor == projectPath && now - cachedAt < std::chrono::seconds(2)) return cached;

    std::vector<std::string> out = { "Sans", "Serif", "Mono", "Pixel" };
    std::vector<std::string> files;
    std::error_code ec;
    const std::filesystem::path assets = std::filesystem::path(projectPath) / "assets";
    if (!projectPath.empty() && std::filesystem::is_directory(assets, ec)) {
        for (const auto& dir : std::filesystem::directory_iterator(assets, ec)) {
            std::string folder = dir.path().filename().string();
            std::transform(folder.begin(), folder.end(), folder.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (folder != "fonts" || !dir.is_directory(ec)) continue;
            for (const auto& f : std::filesystem::directory_iterator(dir.path(), ec)) {
                std::string ext = f.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (f.is_regular_file(ec) && (ext == ".ttf" || ext == ".otf")) files.push_back(f.path().filename().string());
            }
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    out.insert(out.end(), files.begin(), files.end());
    cached = out;
    cachedFor = projectPath;
    cachedAt = now;
    return out;
}

void Draw(float x, float y, float w, float h) {
    if (g_widgets.empty()) return;
    const Rectangle area = { x, y, w, h };
    const float scale = ScaleFor(h);
    BeginScissorMode(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h));
    for (const Widget& wd : g_widgets) {
        if (!wd.visible) continue;
        switch (wd.kind) {
            case Kind::Panel:
                DrawRectangleRec(LayoutRect(wd.anchor, wd.x, wd.y, wd.w, wd.h, area, scale), wd.color);
                break;
            case Kind::Bar: {
                const Rectangle r = LayoutRect(wd.anchor, wd.x, wd.y, wd.w, wd.h, area, scale);
                DrawRectangleRec(r, wd.color2);
                DrawRectangleRec({ r.x, r.y, r.width * wd.value, r.height }, wd.color);
                break;
            }
            case Kind::Image: {
                if (wd.image.empty()) break;
                const Texture2D tex = TextureFor(wd.image);
                if (tex.id == 0) break;
                const Rectangle r = LayoutRect(wd.anchor, wd.x, wd.y, wd.w > 0 ? wd.w : (float)tex.width, wd.h > 0 ? wd.h : (float)tex.height, area, scale);
                DrawTexturePro(tex, { 0, 0, (float)tex.width, (float)tex.height }, r, { 0, 0 }, 0.0f, wd.color);
                break;
            }
            case Kind::Text: {
                if (wd.text.empty()) break;
                const int size = std::max(1, static_cast<int>(wd.fontSize * scale));
                const Font* font = FontFor(wd.font);
                const float spacing = size * 0.02f;
                const float tw = font ? MeasureTextEx(*font, wd.text.c_str(), static_cast<float>(size), spacing).x
                                      : static_cast<float>(MeasureText(wd.text.c_str(), size));
                const Rectangle r = LayoutRect(wd.anchor, wd.x, wd.y, tw / scale, static_cast<float>(size) / scale, area, scale);
                const int shadow = std::max(1, size / 14);
                const Color shade = { 0, 0, 0, static_cast<unsigned char>(wd.color.a * 160 / 255) };
                if (font) {
                    DrawTextEx(*font, wd.text.c_str(), { r.x + shadow, r.y + shadow }, static_cast<float>(size), spacing, shade);
                    DrawTextEx(*font, wd.text.c_str(), { r.x, r.y }, static_cast<float>(size), spacing, wd.color);
                } else {
                    DrawText(wd.text.c_str(), static_cast<int>(r.x) + shadow, static_cast<int>(r.y) + shadow, size, shade);
                    DrawText(wd.text.c_str(), static_cast<int>(r.x), static_cast<int>(r.y), size, wd.color);
                }
                break;
            }
        }
    }
    EndScissorMode();
}

} // namespace gameui
