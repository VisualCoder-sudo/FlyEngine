#pragma once
#include "raylib.h"

#include <string>

// The screen shown while a project loads, in the editor and in the player.
//
//   * The editor shows a percentage, what it is doing now, and the log lines the load writes
//     (script build, terrain, city, ...).
//   * The player shows a percentage (and, if asked, the stage and the log).
//
// Both can be switched off and restyled (title, colours, what is shown). The choices are kept per
// machine in <config dir>/loading.cfg (Preferences > Loading); a project can carry its own player look
// in <project>/loading.cfg, which wins over the machine's for that project.
namespace loading {

enum class Target { Editor, Player };

struct Look {
    bool enabled = true;
    bool showPercent = true;        // the big percentage
    bool showStage = true;          // "Compiling scripts", "Loading objects (120 / 400)", ...
    bool showBar = true;
    bool showLogs = false;          // the log lines the load writes
    int logLines = 8;               // how many are shown (3..16)
    char title[96] = "{name}";      // {name} becomes the project's name
    Color background = { 25, 27, 32, 255 };
    Color accent = { 0, 190, 200, 255 };
};

struct Settings {
    Look editor;
    Look player;
    Settings();
};

Settings& GetSettings();

// Reads the machine's settings, then (for projectFolder != "") the project's own player look over them.
void Load(const std::string& projectFolder = "");
void Save();                                                   // the machine's settings
bool SaveProjectOverride(const std::string& projectFolder);    // the player look, into <project>/loading.cfg
bool RemoveProjectOverride(const std::string& projectFolder);
bool HasProjectOverride(const std::string& projectFolder);

// A load: Begin(), then Progress() as it goes, then End(). Everything is a no-op (and cheap) when
// the loading screen is switched off or no window is open.
void Begin(Target target, const std::string& projectName);
// The part of the bar the following Progress() calls cover: 0.2..0.6 maps their 0..1 onto 20%..60%.
void Range(float from, float to);
// fraction 0..1 within the Range(); the screen is redrawn at most about 30 times a second.
void Progress(float fraction, const char* stage = nullptr);
void Log(const char* fmt, ...);
void AddLogLine(const char* text);      // for the editor's log (ui::LogImpl): only kept while loading
void Redraw();                          // draws the current state now (Progress() skips draws that come too soon after one)
void End();
bool Active();
float Fraction();                       // how far the load has got, 0..1 (tests)

} // namespace loading
