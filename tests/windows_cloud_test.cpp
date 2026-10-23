// Regression tests for the Windows / FlyCloud review pass. Everything here runs offline on any OS, no window:
//   - internal::QuoteWindowsArg (the CreateProcess command-line quoting the Windows ProcessRunner uses),
//   - the exported game's file name for Windows reserved device names,
//   - FlyCloudModule's pure helpers: tokenising, server URL cleanup, version check, push exclusions,
//     the response size cap and config load/save.
#include "Engine/ExportManager.hpp"
#include "Engine/Platform/PlatformInternal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#if FLYENGINE_ENABLE_FLYCLOUD
// The helpers are private; the standard and third-party headers go in first so only the module is affected.
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include "miniz.h"
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#define private public
#include "Engine/Backend/FlyCloudModule.hpp"
#undef private
#endif

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void WriteFakePe(const fs::path& file) {
    std::vector<unsigned char> b(0x400, 0);
    b[0] = 'M'; b[1] = 'Z';
    b[0x3C] = 0x80;
    b[0x80] = 'P'; b[0x81] = 'E';
    b[0x84] = 0x64; b[0x85] = 0x86;   // x86-64
    b[0x96] = 0x22; b[0x97] = 0x00;   // executable, not a DLL
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary).write((const char*)b.data(), (std::streamsize)b.size());
}

static void TestQuoting() {
    using platform::internal::QuoteWindowsArg;
    CHECK(QuoteWindowsArg("plain") == "plain");
    CHECK(QuoteWindowsArg("-IC:\\a\\b") == "-IC:\\a\\b");                    // backslashes alone stay literal
    CHECK(QuoteWindowsArg("") == "\"\"");                                      // an empty argument must survive
    CHECK(QuoteWindowsArg("a b") == "\"a b\"");
    CHECK(QuoteWindowsArg("say \"hi\"") == "\"say \\\"hi\\\"\"");
    CHECK(QuoteWindowsArg("C:\\my dir\\") == "\"C:\\my dir\\\\\"");           // trailing backslash must not eat the closing quote
    CHECK(QuoteWindowsArg("a\\\"b c") == "\"a\\\\\\\"b c\"");                  // backslash before a quote: doubled, quote escaped
    CHECK(QuoteWindowsArg("/Fo C:\\x\\\\") == "\"/Fo C:\\x\\\\\\\\\"");        // the compiler's doubled separator keeps its meaning
}

static void TestExeNames(const fs::path& dir) {
    const fs::path project = dir / "Proj";
    fs::create_directories(project / "assets");
    std::ofstream(project / "Proj.flyproj") << "FLYENGINE_PROJECT 1\nName = \"Proj\"\nTemplate = \"Blank\"\nCreated = 1\nModified = 1\n---SCENE---\nSIMPLE_ENGINE_BUILD 21\n0\n";
    std::ofstream(project / "assets" / "a.txt") << "x";
    const fs::path players = dir / "players";
#if defined(_WIN32)
    _putenv_s("FLYENGINE_PLAYERS_DIR", players.string().c_str());
#else
    setenv("FLYENGINE_PLAYERS_DIR", players.string().c_str(), 1);
#endif
    WriteFakePe(players / "windows-x64" / "FlyPlayer.exe");

    auto exportAs = [&](const std::string& name) {
        exporter::Config cfg;
        cfg.target = "windows-x64";
        cfg.outputDir = (dir / ("out_" + std::to_string(std::hash<std::string>{}(name)))).string();
        cfg.exeName = name;
        cfg.assetsFolder = false;
        exporter::Result r = exporter::Run(project.string(), cfg, nullptr, nullptr);
        return r.ok ? fs::path(r.outputPath).filename().string() : std::string("<failed>");
    };
    CHECK(exportAs("My Game") == "My_Game.exe");
    CHECK(exportAs("CON") == "_CON.exe");        // CON.exe cannot be created on Windows
    CHECK(exportAs("com1") == "_com1.exe");
    CHECK(exportAs("nul.txt") == "_nul.txt.exe");
    CHECK(exportAs("..") == "_...exe");
    CHECK(exportAs("console") == "console.exe"); // only the exact device names are reserved
}

#if FLYENGINE_ENABLE_FLYCLOUD
using FlyEngine::FlyCloudModule;

static void TestCloudHelpers(const fs::path& dir) {
    // Tokenizer: quotes group, an empty pair is an empty argument, unbalanced quotes do not crash.
    auto t = FlyCloudModule::Tokenize("fcloud push -v 1.2.3 -m \"two words\" \"\"");
    CHECK(t.size() == 7 && t[0] == "fcloud" && t[5] == "two words" && t[6].empty());
    CHECK(!FlyCloudModule::Tokenize("fcloud push -m \"unterminated").empty());

    CHECK(FlyCloudModule::NormalizeServerUrl("https://host/") == "https://host");
    CHECK(FlyCloudModule::NormalizeServerUrl("  https://host:8080//\n") == "https://host:8080");
    CHECK(FlyCloudModule::NormalizeServerUrl("").empty());

    CHECK(FlyCloudModule::IsValidVersion("1.0.0") && FlyCloudModule::IsValidVersion("12.3.45"));
    CHECK(!FlyCloudModule::IsValidVersion("-m") && !FlyCloudModule::IsValidVersion("1.0") && !FlyCloudModule::IsValidVersion("1.0.0-beta"));

    CHECK(FlyCloudModule::IsSecureOrLocalUrl("https://x.example") && FlyCloudModule::IsSecureOrLocalUrl("http://localhost:8000"));
    CHECK(!FlyCloudModule::IsSecureOrLocalUrl("http://localhost:x@evil.example") && !FlyCloudModule::IsSecureOrLocalUrl("http://localhost@evil.example"));
    CHECK(!FlyCloudModule::IsSecureOrLocalUrl("http://localhost.evil.example") && !FlyCloudModule::IsSecureOrLocalUrl("http://x.example"));

    // What a push must never upload, in any folder and any letter case.
    CHECK(FlyCloudModule::IsUploadExcluded("fcloud_config.json"));
    CHECK(FlyCloudModule::IsUploadExcluded("sub/FCloud_Config.JSON"));
    CHECK(FlyCloudModule::IsUploadExcluded("certs/server.PEM") && FlyCloudModule::IsUploadExcluded("a/b/id.key"));
    CHECK(FlyCloudModule::IsUploadExcluded(".git/config") && FlyCloudModule::IsUploadExcluded("tex.png.import"));
    CHECK(!FlyCloudModule::IsUploadExcluded("assets/hero.png") && !FlyCloudModule::IsUploadExcluded("Scripts/keyboard.cpp"));
    CHECK(!FlyCloudModule::IsUploadExcluded("monkey"));   // a name that merely ends in "key" is fine

    // Response bodies are capped at 8 MB.
    std::string body;
    std::string chunk(1 << 20, 'x');
    size_t accepted = 0;
    for (int i = 0; i < 10; ++i) accepted += FlyCloudModule::WriteBody(chunk.data(), 1, chunk.size(), &body) != 0 ? 1 : 0;
    CHECK(accepted == 8 && body.size() == (size_t)8 << 20);
    CHECK(FlyCloudModule::WriteBody(chunk.data(), 1, 1, &body) == 0);

    // Config: missing = empty, garbage / non-object = not ok and never overwritten, round trip is atomic.
    const std::string cfgPath = (dir / "fcloud_config.json").string();
    bool ok = false;
    CHECK(FlyCloudModule::LoadConfig(cfgPath, ok).empty() && ok);
    { std::ofstream(cfgPath) << "{ \"server_url\": \"https://a\", "; }
    FlyCloudModule::LoadConfig(cfgPath, ok);
    CHECK(!ok);
    { std::ofstream(cfgPath) << "[1,2]"; }
    FlyCloudModule::LoadConfig(cfgPath, ok);
    CHECK(!ok);
    nlohmann::json cfg = nlohmann::json::object();
    cfg["api_key"] = "k-123";
    cfg["server_url"] = "https://a";
    FlyCloudModule::SaveConfig(cfgPath, cfg);
    nlohmann::json back = FlyCloudModule::LoadConfig(cfgPath, ok);
    CHECK(ok && back.value("api_key", "") == "k-123");
    CHECK(!fs::exists(cfgPath + ".tmp"));

    // A second command while one runs is refused instead of racing it.
    std::unique_lock<std::mutex> held(FlyCloudModule::CommandMutex());
    std::mutex logMutex;
    std::string firstLog;
    std::thread th([&] {
        FlyCloudModule::DispatchCommandAsync("fcloud --help", dir.string(), [&](const std::string& m, bool) {
            std::lock_guard<std::mutex> l(logMutex);
            firstLog += m + "\n";
        });
    });
    th.join();
    // The command thread is detached; give it a moment, then check it was turned away.
    for (int i = 0; i < 200; ++i) {
        { std::lock_guard<std::mutex> l(logMutex); if (!firstLog.empty()) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    {
        std::lock_guard<std::mutex> l(logMutex);
        CHECK(firstLog.find("still running") != std::string::npos);
    }
    held.unlock();
}
#endif

int main() {
    const fs::path dir = fs::temp_directory_path() / "fly_windows_cloud_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    TestQuoting();
    TestExeNames(dir / "export");
#if FLYENGINE_ENABLE_FLYCLOUD
    TestCloudHelpers(dir);
#endif

    fs::remove_all(dir);
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("windows_cloud_test: all checks passed\n");
    return 0;
}
