#pragma once
// Export: packs a project into a single executable (the player with the project's data appended, see
// PackArchive.hpp). Used by the editor's Export dialog and by the headless `flyexport` tool.
#include <functional>
#include <string>
#include <vector>

namespace exporter {

struct Config {
    std::string target;          // Target::id
    std::string outputDir;       // where the finished game goes
    std::string exeName;         // file name without extension
    std::string windowTitle;
    std::string iconPath;        // PNG for the game's window/file icon; project-relative or absolute; empty = default
    bool linuxLauncher = false;  // Linux: also write <name>.desktop + <name>.png beside the game (off: one file only)
    int windowWidth = 1280;
    int windowHeight = 720;
    bool fullscreen = false;
    int fpsCap = 180;            // 0 = unlimited
    std::string playerExe;       // player to build on; empty = the FlyPlayer next to this program
};

struct Target {
    std::string id;              // "linux-arch", "linux-debian", "windows-x64"
    std::string label;           // "Linux (Arch)"
    bool available = false;
    std::string reason;          // why it is not available
};

struct Result {
    bool ok = false;
    std::string outputPath;
    std::string error;
    unsigned long long bytes = 0;
};

using LogFn = std::function<void(const std::string&)>;
using ProgressFn = std::function<bool(float)>;   // 0..1; return false to cancel

std::vector<Target> Targets();
std::string DefaultTarget();                     // the target native to this machine, or the first available

// <projectDir>/export.cfg. A missing file gives defaults taken from the project (name) and the current quality settings.
Config LoadConfig(const std::string& projectDir, const std::string& projectName);
bool SaveConfig(const std::string& projectDir, const Config& cfg);

// Finds the player executable next to this program (FlyPlayer, in any capitalisation). Empty if none.
std::string FindPlayerExecutable();

// Packs the project at `projectDir` (its files on disk as they are now: save first). Safe to call from a worker thread.
Result Run(const std::string& projectDir, const Config& cfg, const LogFn& log, const ProgressFn& progress);

} // namespace exporter
