// flyexport -- the editor's Export, without the editor: packs a project into a single executable.
//   flyexport <projectDir> --out <dir> [--name <exe>] [--title <text>] [--icon <png>] [--launcher] [--fps <n>] [--size WxH] [--fullscreen] [--embed-assets|--assets-folder]
//             [--player <FlyPlayer>] [--target <id>]
#include "Engine/ExportManager.hpp"
#include "Engine/PostFX.hpp"

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
    exporter::Config cfg = exporter::LoadConfig(projectDir, std::filesystem::path(projectDir).filename().string());
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--out") cfg.outputDir = next();
        else if (a == "--name") cfg.exeName = next();
        else if (a == "--title") cfg.windowTitle = next();
        else if (a == "--icon") cfg.iconPath = std::filesystem::absolute(next()).string();   // relative to the shell, not the project
        else if (a == "--launcher") cfg.linuxLauncher = true;
        else if (a == "--fps") cfg.fpsCap = std::atoi(next().c_str());
        else if (a == "--size") { const std::string s = next(); std::sscanf(s.c_str(), "%dx%d", &cfg.windowWidth, &cfg.windowHeight); }
        else if (a == "--fullscreen") cfg.fullscreen = true;
        else if (a == "--embed-assets") cfg.assetsFolder = false;   // one executable, assets inside it
        else if (a == "--assets-folder") cfg.assetsFolder = true;    // assets/ written to an Assets folder beside the game (default)
        else if (a == "--player") cfg.playerExe = next();
        else if (a == "--target") cfg.target = next();
    }
    gfx::LoadQualitySettings();   // the settings this machine's editor saved (Preferences > Rendering)
    const exporter::Result r = exporter::Run(projectDir, cfg,
        [](const std::string& line) { std::printf("%s\n", line.c_str()); std::fflush(stdout); },
        [](float) { return true; });
    return r.ok ? 0 : 1;
}
