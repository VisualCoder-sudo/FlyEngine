#pragma once
#include "raylib.h"

#include <string>
#include <vector>

// The game's own on-screen interface: panels, text, bars and images that scripts create by id (fly::UI) and the
// engine draws over the 3D view. Separate from the editor's ImGui, so it is in the exported player too.
//
// Sizes and offsets are in "UI pixels" of a 1280 x 720 screen; everything scales with the window's height, so a
// layout looks the same at any resolution.
namespace gameui {

enum class Kind { Panel = 0, Text = 1, Bar = 2, Image = 3 };

// Where a widget sits in the screen area. Offset moves it right and down from there (negative x pulls a right-anchored
// widget in from the edge). Fill stretches over the whole area, offset used as a margin on every side.
enum class Anchor { TopLeft = 0, TopCenter, TopRight, Left, Center, Right, BottomLeft, BottomCenter, BottomRight, Fill, Count };

constexpr float kReferenceHeight = 720.0f;

// The size of one UI pixel on an area of this height.
float ScaleFor(float areaHeight);

// Where a widget of `size` (UI pixels) lands in `area`. Pure arithmetic, no drawing.
Rectangle LayoutRect(Anchor anchor, float offsetX, float offsetY, float width, float height, Rectangle area, float scale);

// Creates the widget, or does nothing if `id` already is one of that kind (a different kind replaces it).
bool Create(const std::string& id, Kind kind);
void SetLayout(const std::string& id, Anchor anchor, float x, float y, float w, float h);
void SetColor(const std::string& id, Color color, Color color2);   // panel/text/image tint: color; bar: fill and background
void SetPrimaryColor(const std::string& id, Color color);        // the same, leaving a bar's background alone
void SetText(const std::string& id, const std::string& text, float fontSize);   // fontSize <= 0 keeps the size
// Text widgets: "" or "Sans" = the engine's normal font, "Serif" / "Mono" = the system's, "Pixel" = the blocky built-in
// one, anything else = a .ttf / .otf file (project-relative like "assets/Fonts/Roboto.ttf", or absolute). A font
// that cannot be loaded falls back to the blocky one. Ship the file with the project: system fonts differ per machine.
// A bare file name ("Roboto.ttf") is looked up in the project's assets/Fonts folder.
void SetFont(const std::string& id, const std::string& font);
void SetValue(const std::string& id, float value);                // bars: 0..1
void SetImage(const std::string& id, const std::string& file);    // project-relative or absolute
void SetVisible(const std::string& id, bool visible);
void Remove(const std::string& id);
size_t Count();

// Everything a widget currently holds, for scripts that read a value back or change one part of it.
struct Info {
    Kind kind = Kind::Panel;
    Anchor anchor = Anchor::TopLeft;
    float x = 0, y = 0, w = 0, h = 0;
    Color color = WHITE, color2 = BLANK;   // bars: fill and background
    std::string text, font, image;
    float fontSize = 24.0f, value = 0.0f;
    bool visible = true;
};
bool Get(const std::string& id, Info& out);   // false if there is no such widget

// The names a font picker offers: Sans, Serif, Mono, Pixel, then every .ttf / .otf in <project>/assets/Fonts (a folder
// named "fonts" in any case), by bare file name. Cached for a couple of seconds, so it is cheap to call every frame.
std::vector<std::string> ListFonts(const std::string& projectPath);

// Removes every widget (and frees the images) when play starts and stops.
void Clear();
// Where relative image paths are looked up.
void SetProjectPath(const std::string& path);

// Draws the widgets over `area`, in the order they were created.
void Draw(float x, float y, float w, float h);

} // namespace gameui
