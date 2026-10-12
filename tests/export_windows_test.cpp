// Export for another OS from a prebuilt player: a Windows x86-64 target on a non-Windows machine appends the data to
// a ready-made FlyPlayer.exe. Uses a stand-in executable with a valid PE header; checks target discovery, the
// wrong-kind-of-player guard, the script/plugin restriction, the .exe output and that the archive reads back. No window.
#include "Engine/ExportManager.hpp"
#include "Engine/Vfs.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// A file that looks like a PE executable to the exporter: MZ, e_lfanew -> "PE\0\0" + machine + characteristics.
static void WriteFakePe(const fs::path& file, unsigned machine, unsigned characteristics) {
    std::vector<unsigned char> b(0x400, 0);
    b[0] = 'M'; b[1] = 'Z';
    const unsigned peOffset = 0x80;
    b[0x3C] = (unsigned char)peOffset;
    b[peOffset] = 'P'; b[peOffset + 1] = 'E';
    b[peOffset + 4] = (unsigned char)(machine & 0xFF); b[peOffset + 5] = (unsigned char)(machine >> 8);
    b[peOffset + 22] = (unsigned char)(characteristics & 0xFF); b[peOffset + 23] = (unsigned char)(characteristics >> 8);
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary).write((const char*)b.data(), (std::streamsize)b.size());
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "fly_export_windows_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // A minimal project: a project file and one asset.
    const fs::path project = dir / "Proj";
    fs::create_directories(project / "assets");
    std::ofstream(project / "Proj.flyproj") << "FLYENGINE_PROJECT 1\nName = \"Proj\"\nTemplate = \"Blank\"\nCreated = 1\nModified = 1\n---SCENE---\nSIMPLE_ENGINE_BUILD 21\n0\n";
    std::ofstream(project / "assets" / "a.txt") << "hello from the archive";

    const fs::path players = dir / "players";
#if defined(_WIN32)
    _putenv_s("FLYENGINE_PLAYERS_DIR", players.string().c_str());
#else
    setenv("FLYENGINE_PLAYERS_DIR", players.string().c_str(), 1);
#endif

    auto windowsTarget = [] {
        for (const auto& t : exporter::Targets()) if (t.id == "windows-x64") return t;
        return exporter::Target{};
    };

    // No player yet: the target is listed but unavailable, with a hint saying what is needed.
    CHECK(!windowsTarget().available);
    CHECK(windowsTarget().reason.find("windows-x64") != std::string::npos);

    // Wrong kinds of executable are not accepted: a 32-bit PE, a DLL.
    WriteFakePe(players / "windows-x64" / "FlyPlayer.exe", 0x014C, 0x0102);
    CHECK(!windowsTarget().available);
    WriteFakePe(players / "windows-x64" / "FlyPlayer.exe", 0x8664, 0x2022);   // x86-64 but a DLL
    CHECK(!windowsTarget().available);

    // A real 64-bit executable.
    WriteFakePe(players / "windows-x64" / "FlyPlayer.exe", 0x8664, 0x0022);
    CHECK(windowsTarget().available);

    exporter::Config cfg;
    cfg.target = "windows-x64";
    cfg.outputDir = (dir / "out").string();
    cfg.exeName = "My Game";
    cfg.windowTitle = "Hello";
    cfg.fpsCap = 144;
    std::string log;
    auto logFn = [&](const std::string& s) { log += s + "\n"; };

    exporter::Result r = exporter::Run(project.string(), cfg, logFn, nullptr);
    CHECK(r.ok);
    CHECK(fs::path(r.outputPath).filename() == "My_Game.exe");
    CHECK(fs::exists(r.outputPath));
    // The result starts with the PE player and carries the archive.
    {
        unsigned char mz[2] = {0};
        std::ifstream(r.outputPath, std::ios::binary).read((char*)mz, 2);
        CHECK(mz[0] == 'M' && mz[1] == 'Z');
    }
    CHECK(vfs::MountFile(r.outputPath));
    std::vector<uint8_t> data;
    CHECK(vfs::Read("assets/a.txt", data) && std::string(data.begin(), data.end()) == "hello from the archive");
    CHECK(vfs::Read("Proj.flyproj", data));
    CHECK(vfs::Read("__meta/export.cfg", data) && std::string(data.begin(), data.end()).find("title Hello") != std::string::npos);
    CHECK(vfs::Read("__meta/graphics.cfg", data) && std::string(data.begin(), data.end()).find("fpsCap 144") != std::string::npos);
    vfs::Unmount();

    // A project with C++ scripts cannot be built for Windows from here: it has to say so, not export something broken.
    fs::create_directories(project / "Scripts");
    std::ofstream(project / "Scripts" / "x.cpp") << "int main2() { return 0; }\n";
    exporter::Result withScripts = exporter::Run(project.string(), cfg, logFn, nullptr);
    CHECK(!withScripts.ok);
    CHECK(withScripts.error.find("scripts") != std::string::npos);
    fs::remove_all(project / "Scripts");

    // An explicit base player of the wrong OS is refused (a Linux executable as a Windows game).
    {
        const fs::path elf = dir / "elf_player";
        std::ofstream(elf, std::ios::binary).write("\x7f" "ELF....", 8);
        exporter::Config bad = cfg;
        bad.playerExe = elf.string();
        exporter::Result wrong = exporter::Run(project.string(), bad, logFn, nullptr);
        CHECK(!wrong.ok && wrong.error.find("not a") != std::string::npos);
    }

    // Unknown target and missing output folder are reported.
    exporter::Config none = cfg; none.outputDir.clear();
    CHECK(!exporter::Run(project.string(), none, logFn, nullptr).ok);
    exporter::Config unknown = cfg; unknown.target = "amiga";
    CHECK(!exporter::Run(project.string(), unknown, logFn, nullptr).ok);

    fs::remove_all(dir);
    std::printf(g_fail ? "export_windows_test: %d FAILED\n" : "export_windows_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
