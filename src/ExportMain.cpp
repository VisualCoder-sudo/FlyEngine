// flyexport -- the editor's Export, without the editor: packs a project into a single executable.
//   flyexport <projectDir> --out <dir> [--name <exe>] [--title <text>] [--icon <png>] [--launcher] [--fps <n>] [--size WxH] [--fullscreen] [--embed-assets|--assets-folder]
//             [--player <FlyPlayer>] [--target <id>]
#include "Engine/ExportManager.hpp"
#include "../include/Graphics/PostFX.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
        std::printf("Usage: flyexport <projectDir> --out <dir> [--name <exe>] [--title <text>] [--icon <png>] [--fps <n>] [--size WxH]\n"
                    "                 [--fullscreen] [--player <FlyPlayer>] [--target <id>]\n");
        return argc < 2 ? 1 : 0;
    }
    const std::string projectDir = argv[1];
    // "proj/" has an empty filename(); take the last real component so the default exe name is not "Game".
    std::filesystem::path projPath(projectDir);
    if (!projPath.has_filename()) projPath = projPath.parent_path();
    exporter::Config cfg = exporter::LoadConfig(projectDir, projPath.filename().string());
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        bool bad = false;
        auto next = [&]() -> std::string {
            if (i + 1 < argc) return argv[++i];
            std::fprintf(stderr, "flyexport: %s needs a value\n", a.c_str());
            bad = true;
            return std::string();
        };
        if (a == "--out") cfg.outputDir = next();
        else if (a == "--name") cfg.exeName = next();
        else if (a == "--title") cfg.windowTitle = next();
        else if (a == "--icon") cfg.iconPath = std::filesystem::absolute(next()).string();   // relative to the shell, not the project
        else if (a == "--launcher") cfg.linuxLauncher = true;
        else if (a == "--fps") {
            const std::string s = next();
            char* end = nullptr;
            const long v = std::strtol(s.c_str(), &end, 10);
            if (!bad && (s.empty() || *end || v < 0 || v > 1000)) { std::fprintf(stderr, "flyexport: bad --fps '%s'\n", s.c_str()); bad = true; }
            else cfg.fpsCap = (int)v;
        }
        else if (a == "--size") {
            const std::string s = next();
            int w = 0, h = 0, n = 0;
            if (!bad && (std::sscanf(s.c_str(), "%dx%d%n", &w, &h, &n) != 2 || s[n] != '\0' || w <= 0 || h <= 0)) {
                std::fprintf(stderr, "flyexport: bad --size '%s' (expected WxH, both above 0)\n", s.c_str());
                bad = true;
            } else { cfg.windowWidth = w; cfg.windowHeight = h; }
        }
        else if (a == "--fullscreen") cfg.fullscreen = true;
        else if (a == "--embed-assets") cfg.assetsFolder = false;   // one executable, assets inside it
        else if (a == "--assets-folder") cfg.assetsFolder = true;    // assets/ written to an Assets folder beside the game (default)
        else if (a == "--player") cfg.playerExe = next();
        else if (a == "--target") cfg.target = next();
        else { std::fprintf(stderr, "flyexport: unknown option '%s'\n", a.c_str()); bad = true; }
        if (bad) return 2;
    }
    gfx::LoadQualitySettings();   // the settings this machine's editor saved (Preferences > Rendering)
    const exporter::Result r = exporter::Run(projectDir, cfg,
        [](const std::string& line) { std::printf("%s\n", line.c_str()); std::fflush(stdout); },
        [](float) { return true; });
    return r.ok ? 0 : 1;
}
