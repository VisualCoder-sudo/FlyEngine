#include "../../../include/Engine/Backend/Automation.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "raylib.h"
#include "raymath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace automation {
namespace {

struct Command {
    int frame = 0;
    std::string name;
    std::vector<std::string> args;
    std::string rest;   // everything after the command name, for `log`
};

std::vector<Command> g_commands;
std::vector<std::string> g_pendingShots;
struct Compare { std::string actual, golden; float tolerance; int frame; };
std::vector<Compare> g_pendingCompares;
int g_failures = 0;
size_t g_next = 0;
bool g_active = false;
int g_lastFrame = 0;
bool g_quitRequested = false;

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int KeyFor(const std::string& name) {
    const std::string n = Lower(name);
    if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') return KEY_A + (n[0] - 'a');
    if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') return KEY_ZERO + (n[0] - '0');
    if (n == "space") return KEY_SPACE;
    if (n == "enter") return KEY_ENTER;
    if (n == "escape" || n == "esc") return KEY_ESCAPE;
    if (n == "tab") return KEY_TAB;
    if (n == "shift") return KEY_LEFT_SHIFT;
    if (n == "ctrl") return KEY_LEFT_CONTROL;
    if (n == "up") return KEY_UP;
    if (n == "down") return KEY_DOWN;
    if (n == "left") return KEY_LEFT;
    if (n == "right") return KEY_RIGHT;
    if (n.size() >= 2 && n[0] == 'f') {
        const int f = std::atoi(n.c_str() + 1);
        if (f >= 1 && f <= 12) return KEY_F1 + (f - 1);
    }
    return -1;
}

int ButtonFor(const std::string& name) {
    const std::string n = Lower(name);
    if (n == "left") return MOUSE_BUTTON_LEFT;
    if (n == "right") return MOUSE_BUTTON_RIGHT;
    if (n == "middle") return MOUSE_BUTTON_MIDDLE;
    return -1;
}

void Say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Say(const char* fmt, ...) {
    std::printf("[automation] ");
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    std::fflush(stdout);
}

void Fail(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Fail(const char* fmt, ...) {
    std::printf("[automation] FAIL: ");
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    std::fflush(stdout);
    ++g_failures;
}

const ScatteredObject* FindObject(const std::vector<ScatteredObject*>& objects, const std::string& name) {
    for (const ScatteredObject* o : objects)
        if (o && o->GetName() == name) return o;
    return nullptr;
}

Vector3 PosOf(const ScatteredObject& o) { return *const_cast<ScatteredObject&>(o).GetPosPtr(); }

// expect exists <Name> | missing <Name> | pos <Name> x y z [tol] | dist <A> <B> <max>
void Expect(const Command& c, const std::vector<ScatteredObject*>& objects) {
    if (c.args.size() < 2) { Fail("frame %d: expect needs a kind and a name", c.frame); return; }
    const std::string kind = Lower(c.args[0]);
    const ScatteredObject* a = FindObject(objects, c.args[1]);
    if (kind == "exists") {
        if (!a) Fail("frame %d: expected object '%s' to exist", c.frame, c.args[1].c_str());
    } else if (kind == "missing") {
        if (a) Fail("frame %d: expected object '%s' to be gone", c.frame, c.args[1].c_str());
    } else if (kind == "pos" && c.args.size() >= 5) {
        if (!a) { Fail("frame %d: no object '%s'", c.frame, c.args[1].c_str()); return; }
        const Vector3 want{static_cast<float>(std::atof(c.args[2].c_str())), static_cast<float>(std::atof(c.args[3].c_str())),
                           static_cast<float>(std::atof(c.args[4].c_str()))};
        const float tol = c.args.size() >= 6 ? static_cast<float>(std::atof(c.args[5].c_str())) : 0.1f;
        const Vector3 p = PosOf(*a);
        if (Vector3Distance(p, want) > tol)
            Fail("frame %d: '%s' at (%.2f, %.2f, %.2f), expected (%.2f, %.2f, %.2f) +-%.2f", c.frame, c.args[1].c_str(), p.x, p.y, p.z,
                 want.x, want.y, want.z, tol);
    } else if (kind == "dist" && c.args.size() >= 4) {
        const ScatteredObject* b = FindObject(objects, c.args[2]);
        if (!a || !b) { Fail("frame %d: expect dist: object '%s' not found", c.frame, a ? c.args[2].c_str() : c.args[1].c_str()); return; }
        const float d = Vector3Distance(PosOf(*a), PosOf(*b));
        const float max = static_cast<float>(std::atof(c.args[3].c_str()));
        if (d > max) Fail("frame %d: '%s' and '%s' are %.2f apart, expected at most %.2f", c.frame, c.args[1].c_str(), c.args[2].c_str(), d, max);
    } else {
        Fail("frame %d: unknown expect '%s' (or missing arguments)", c.frame, kind.c_str());
    }
}

void DumpObject(const char* label, const ScatteredObject& o) {
    const Vector3 p = *const_cast<ScatteredObject&>(o).GetPosPtr();
    Say("  %s pos=(%.2f, %.2f, %.2f)", label, p.x, p.y, p.z);
}

void Run(const Command& c, const Camera3D& cam, const std::vector<ScatteredObject*>& objects) {
    if (c.name == "key" && c.args.size() >= 2) {
        const int k = KeyFor(c.args[1]);
        if (k < 0) { Say("frame %d: unknown key '%s'", c.frame, c.args[1].c_str()); return; }
        FlyInjectKey(k, Lower(c.args[0]) == "down");
    } else if (c.name == "mouse" && c.args.size() >= 2) {
        const int b = ButtonFor(c.args[1]);
        if (b < 0) { Say("frame %d: unknown mouse button '%s'", c.frame, c.args[1].c_str()); return; }
        FlyInjectMouseButton(b, Lower(c.args[0]) == "down");
    } else if (c.name == "look" && c.args.size() >= 2) {
        FlyInjectMouseDelta(static_cast<float>(std::atof(c.args[0].c_str())), static_cast<float>(std::atof(c.args[1].c_str())));
    } else if (c.name == "log") {
        Say("frame %d: %s", c.frame, c.rest.c_str());
    } else if (c.name == "screenshot" && !c.args.empty()) {
        g_pendingShots.push_back(c.rest);
    } else if (c.name == "expect") {
        Expect(c, objects);
    } else if (c.name == "compare" && c.args.size() >= 2) {
        g_pendingCompares.push_back({c.args[0], c.args[1], c.args.size() >= 3 ? static_cast<float>(std::atof(c.args[2].c_str())) : 2.0f, c.frame});
    } else if (c.name == "dump") {
        const Vector3 f = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
        Say("frame %d: camera pos=(%.2f, %.2f, %.2f) yaw=%.1f pitch=%.1f", c.frame, cam.position.x, cam.position.y, cam.position.z,
            atan2f(f.x, f.z) * RAD2DEG, asinf(f.y) * RAD2DEG);
        const std::string wanted = c.args.empty() ? std::string() : c.rest;
        bool foundPlayer = false, foundWanted = wanted.empty();
        for (const ScatteredObject* o : objects) {
            if (!o) continue;
            if (o->GetName() == "PlayerCharacter") { DumpObject("PlayerCharacter", *o); foundPlayer = true; }
            if (!wanted.empty() && o->GetName() == wanted) { DumpObject(wanted.c_str(), *o); foundWanted = true; }
        }
        if (!foundPlayer) Say("  no PlayerCharacter in the scene");
        if (!foundWanted) Say("  no object named '%s'", wanted.c_str());
    } else if (c.name == "quit") {
        // handled by BeforeUpdate
    } else {
        Say("frame %d: command '%s' ignored (wrong name or missing arguments)", c.frame, c.name.c_str());
    }
}

}   // namespace

bool Load(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "[automation] cannot open %s\n", path.c_str());
        return false;
    }
    g_commands.clear();
    g_pendingShots.clear();
    g_pendingCompares.clear();
    g_failures = 0;
    g_next = 0;
    g_lastFrame = 0;
    g_quitRequested = false;
    std::string line;
    int lineNo = 0;
    bool ok = true;
    while (std::getline(in, line)) {
        ++lineNo;
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        std::istringstream ss(line);
        Command c;
        if (!(ss >> c.frame)) {
            if (line.find_first_not_of(" \t\r") != std::string::npos) {
                std::fprintf(stderr, "[automation] %s:%d: a line starts with a frame number\n", path.c_str(), lineNo);
                ok = false;
            }
            continue;
        }
        if (!(ss >> c.name) || c.frame < 1) {
            std::fprintf(stderr, "[automation] %s:%d: expected \"<frame >= 1> <command>\"\n", path.c_str(), lineNo);
            ok = false;
            continue;
        }
        c.name = Lower(c.name);
        std::string arg;
        std::streampos afterName = ss.tellg();
        while (ss >> arg) c.args.push_back(arg);
        const std::string whole = line;
        if (afterName != std::streampos(-1)) {
            c.rest = whole.substr(static_cast<size_t>(afterName));
            const auto b = c.rest.find_first_not_of(" \t");
            const auto e = c.rest.find_last_not_of(" \t\r");
            c.rest = b == std::string::npos ? std::string() : c.rest.substr(b, e - b + 1);
        }
        g_lastFrame = std::max(g_lastFrame, c.frame);
        g_commands.push_back(std::move(c));
    }
    if (!ok) return false;
    std::stable_sort(g_commands.begin(), g_commands.end(), [](const Command& a, const Command& b) { return a.frame < b.frame; });
    g_active = true;
    Say("loaded %zu command(s) from %s", g_commands.size(), path.c_str());
    return true;
}

bool Active() { return g_active; }

bool BeforeUpdate(int frame, const Camera3D& camera, const std::vector<ScatteredObject*>& objects) {
    if (!g_active) return false;
    while (g_next < g_commands.size() && g_commands[g_next].frame <= frame) {
        const Command& c = g_commands[g_next++];
        Run(c, camera, objects);
        if (c.name == "quit") g_quitRequested = true;
    }
    if (frame > g_lastFrame) g_quitRequested = true;
    // A screenshot asked for on the last frame is taken (after this frame is drawn) before the run ends.
    if (!g_quitRequested || !g_pendingShots.empty() || !g_pendingCompares.empty()) return false;
    Say("finished at frame %d", frame);
    return true;
}

void AfterRender() {
    for (const std::string& file : g_pendingShots) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
        TakeScreenshot(file.c_str());
        Say("screenshot saved: %s", file.c_str());
    }
    g_pendingShots.clear();
    for (const Compare& cmp : g_pendingCompares) {
        Image actual = LoadImage(cmp.actual.c_str());
        if (!actual.data) { Fail("frame %d: cannot read '%s'", cmp.frame, cmp.actual.c_str()); continue; }
        if (!FileExists(cmp.golden.c_str())) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(cmp.golden).parent_path(), ec);
            ExportImage(actual, cmp.golden.c_str());
            Say("no golden image; saved '%s' as the new one", cmp.golden.c_str());
            UnloadImage(actual);
            continue;
        }
        Image golden = LoadImage(cmp.golden.c_str());
        if (!golden.data || golden.width != actual.width || golden.height != actual.height) {
            Fail("frame %d: '%s' is %dx%d, golden is %dx%d", cmp.frame, cmp.actual.c_str(), actual.width, actual.height, golden.width,
                 golden.height);
        } else {
            ImageFormat(&actual, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            ImageFormat(&golden, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            const auto* a = static_cast<const unsigned char*>(actual.data);
            const auto* g = static_cast<const unsigned char*>(golden.data);
            const size_t n = static_cast<size_t>(actual.width) * actual.height;
            double sum = 0;
            for (size_t i = 0; i < n; ++i)
                for (int ch = 0; ch < 3; ++ch) sum += std::abs(int(a[i * 4 + ch]) - int(g[i * 4 + ch]));
            const double mean = sum / (3.0 * n);
            if (mean > cmp.tolerance) Fail("frame %d: '%s' differs from '%s' (mean %.2f/255, allowed %.2f)", cmp.frame, cmp.actual.c_str(), cmp.golden.c_str(), mean, cmp.tolerance);
            else Say("image matches golden '%s' (mean diff %.2f)", cmp.golden.c_str(), mean);
        }
        UnloadImage(actual);
        if (golden.data) UnloadImage(golden);
    }
    g_pendingCompares.clear();
}

int Failures() { return g_failures; }

}   // namespace automation
