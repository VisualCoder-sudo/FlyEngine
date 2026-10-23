#include "Engine/Platform/EmergencyMode.hpp"

#if defined(__linux__)

#include <dlfcn.h>
#include <dirent.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern char** environ;

namespace fs = std::filesystem;

namespace platform {
namespace {

const char* kChildGuard = "FLYENGINE_EMERGENCY_CHILD";

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

bool IsSocket(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

// libX11 is loaded by hand so this works (and says something useful) even where it is not installed.
struct XLib {
    void* handle = nullptr;
    void* (*open)(const char*) = nullptr;
    int (*close)(void*) = nullptr;
    XLib() {
        handle = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
        if (!handle) return;
        open = reinterpret_cast<void* (*)(const char*)>(dlsym(handle, "XOpenDisplay"));
        close = reinterpret_cast<int (*)(void*)>(dlsym(handle, "XCloseDisplay"));
        if (!open || !close) open = nullptr;
    }
    ~XLib() { if (handle) dlclose(handle); }
    bool Connect(const char* display) const {
        if (!open) return false;
        void* d = open(display);
        if (!d) return false;
        close(d);
        return true;
    }
};

// Display names (":0", ":1") with a live-looking X socket, for when DISPLAY is unset or stale.
std::vector<std::string> X11Sockets() {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/tmp/.X11-unix", ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() > 1 && name[0] == 'X' && IsSocket(e.path().string())) out.push_back(":" + name.substr(1));
    }
    return out;
}

std::string WaylandSocket() {
    const std::string name = Env("WAYLAND_DISPLAY");
    if (name.empty()) return "";
    const std::string path = name[0] == '/' ? name : Env("XDG_RUNTIME_DIR") + "/" + name;
    return IsSocket(path) ? path : "";
}

// Finds an X server this process can open a window on; sets DISPLAY to it if it was not the one in the environment.
bool FindDisplay(const XLib& x) {
    const std::string cur = Env("DISPLAY");
    if (!cur.empty() && x.Connect(cur.c_str())) return true;
    for (const std::string& d : X11Sockets()) {
        if (d == cur) continue;
        if (x.Connect(d.c_str())) {
            std::fprintf(stderr, "Flyengine: DISPLAY was '%s'; using the X server on %s\n", cur.c_str(), d.c_str());
            setenv("DISPLAY", d.c_str(), 1);
            return true;
        }
    }
    return false;
}

void PrintDiagnostics(const XLib& x) {
    auto show = [](const char* k) { const std::string v = Env(k); std::printf("  %-18s %s\n", k, v.empty() ? "(unset)" : v.c_str()); };
    std::printf("\nDisplay diagnostics\n");
    show("DISPLAY"); show("WAYLAND_DISPLAY"); show("XDG_SESSION_TYPE"); show("XDG_RUNTIME_DIR"); show("XAUTHORITY");
    std::printf("  %-18s %s\n", "libX11", x.handle ? "found" : "NOT found (install libx11-6)");
    const auto socks = X11Sockets();
    std::printf("  %-18s ", "X11 sockets");
    if (socks.empty()) std::printf("none in /tmp/.X11-unix\n");
    else { for (const auto& s : socks) std::printf("%s ", s.c_str()); std::printf("\n"); }
    const std::string wl = WaylandSocket();
    std::printf("  %-18s %s\n", "Wayland socket", wl.empty() ? "none" : wl.c_str());
    if (!wl.empty() && socks.empty())
        std::printf("\n  A Wayland compositor is running but without XWayland. Flyengine opens its window through X11,\n"
                    "  so enable XWayland in your compositor (or start an X session).\n");
    else if (socks.empty() && wl.empty())
        std::printf("\n  No graphical session found. On a text console or over SSH, start one (startx, or a desktop\n"
                    "  session) or use `ssh -X`. Project export still works from this console.\n");
    std::printf("\n");
}

std::string Prompt(const char* text) {
    std::printf("%s", text);
    std::fflush(stdout);
    char buf[1024];
    if (!std::fgets(buf, sizeof(buf), stdin)) return "\x04";   // EOF
    std::string s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string ExeDir() {
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string(".") : p.parent_path().string();
}

int Run(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void ExportProject() {
    std::string tool = ExeDir() + "/flyexport";
    if (access(tool.c_str(), X_OK) != 0) tool = "flyexport";
    const std::string proj = Prompt("Project folder: ");
    if (proj.empty() || proj == "\x04") return;   // empty line or EOF
    const std::string out = Prompt("Output folder: ");
    if (out.empty() || out == "\x04") return;
    std::printf("\n");
    const int rc = Run({ tool, proj, "--out", out });
    if (rc == -1) std::printf("Could not run '%s'.\n\n", tool.c_str());
    else std::printf("\nflyexport finished with exit code %d.\n\n", rc);
}

[[noreturn]] void Relaunch(char** argv) {
    // Back to normal startup; without --emergency, so a working display opens the real window.
    std::vector<char*> args;
    for (char** a = argv; *a; ++a) if (std::strcmp(*a, "--emergency") != 0) args.push_back(*a);
    args.push_back(nullptr);
    unsetenv(kChildGuard);
    execv("/proc/self/exe", args.data());
    std::perror("Flyengine: could not restart");
    std::exit(1);
}

[[noreturn]] void Console(const XLib& x, char** argv, const std::string& why) {
    std::printf("\n=== Flyengine Emergency Mode ===\n%s\n", why.c_str());
    PrintDiagnostics(x);
    for (;;) {
        std::printf("  [r] retry opening the window   [d] diagnostics   [e] export a project\n"
                    "  [s] shell                      [q] quit\n");
        const std::string c = Prompt("> ");
        if (c == "q" || c == "Q" || c == "\x04") std::exit(1);
        if (c == "r" || c == "R") {
            if (FindDisplay(x)) { std::printf("Display found, starting...\n"); Relaunch(argv); }
            std::printf("Still no display.\n\n");
        } else if (c == "d" || c == "D") PrintDiagnostics(x);
        else if (c == "e" || c == "E") ExportProject();
        else if (c == "s" || c == "S") {
            const std::string sh = Env("SHELL").empty() ? "/bin/sh" : Env("SHELL");
            std::printf("Starting %s -- exit it to return here.\n", sh.c_str());
            Run({ sh });
        }
    }
}

// No terminal to talk on: ask a terminal emulator for one (some, such as foot, kitty or gnome-terminal, work on a
// Wayland-only session). Returns true once one has been started.
bool SpawnInTerminal(char** argv) {
    setenv(kChildGuard, "1", 1);
    struct Term { const char* exe; const char* execFlag; };
    static const Term terms[] = {
        { "x-terminal-emulator", "-e" }, { "foot", "" }, { "kitty", "" }, { "alacritty", "-e" }, { "wezterm", "start --" },
        { "gnome-terminal", "--" }, { "konsole", "-e" }, { "xfce4-terminal", "-x" }, { "xterm", "-e" }, { "mate-terminal", "-x" },
    };
    for (const Term& t : terms) {
        std::vector<std::string> args{ t.exe };
        std::string flag = t.execFlag;
        for (size_t p; (p = flag.find(' ')) != std::string::npos || !flag.empty();) {
            args.push_back(flag.substr(0, p));
            if (p == std::string::npos) break;
            flag.erase(0, p + 1);
        }
        args.push_back("/proc/self/exe");
        for (char** a = argv + 1; *a; ++a) args.push_back(*a);
        args.push_back("--emergency");
        std::vector<char*> cargs;
        for (auto& a : args) cargs.push_back(a.data());
        cargs.push_back(nullptr);
        pid_t pid = 0;
        if (posix_spawnp(&pid, t.exe, nullptr, nullptr, cargs.data(), environ) == 0) {
            usleep(400 * 1000);   // an emulator that cannot reach a display exits at once
            int status = 0;
            if (waitpid(pid, &status, WNOHANG) == 0 || (WIFEXITED(status) && WEXITSTATUS(status) == 0)) return true;
        }
    }
    unsetenv(kChildGuard);
    return false;
}

} // namespace

void RunEmergencyModeIfNoDisplay(int argc, char** argv) {
    if (std::getenv("FLYENGINE_NO_EMERGENCY")) return;
    bool forced = false;
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--emergency") == 0) forced = true;

    // Headless tools (flyexport-style flags) never open a window.
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--create-project") || !std::strcmp(argv[i], "--testwater") || !std::strcmp(argv[i], "--testscene")) return;

    XLib x;
    if (!forced && FindDisplay(x)) return;

    const bool haveTty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    const std::string why = forced ? "Emergency mode was requested with --emergency."
                                   : "Flyengine could not connect to an X11 display (Wayland needs XWayland).";
    if (haveTty) Console(x, argv, why);

    if (!std::getenv(kChildGuard) && SpawnInTerminal(argv)) std::exit(0);

    // Nothing to talk to: leave the reason in the log instead of a bare abort.
    std::fprintf(stderr, "Flyengine: %s No terminal available either. Run it from a terminal, or set DISPLAY.\n", why.c_str());
    std::exit(1);
}

} // namespace platform

#else

namespace platform {
void RunEmergencyModeIfNoDisplay(int, char**) {}
}

#endif
