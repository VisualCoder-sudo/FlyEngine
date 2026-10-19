// raylib core module on sokol_app: window lifecycle, frame pacing, input,
// timing, files and misc utilities.
#include "flyvfs.h"
#include "rl_internal.hpp"
#include "rl_shader_hotreload.hpp"

#include "flyapp.h"
#include "sokol_app.h"
#include "sokol_log.h"
#include "sokol_time.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#if defined(__linux__)
#include <unistd.h>
#endif
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <thread>

namespace fs = std::filesystem;

static fs::path RlU8Path(const char* s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s)));
}

namespace rli {

CoreState& Core() {
    static CoreState s;
    return s;
}

namespace {

FlyEventHook g_eventHook = nullptr;
int g_traceLogLevel = LOG_INFO;
bool g_windowFocused = true;   // kept from sokol_app FOCUSED/UNFOCUSED events
std::mt19937 g_rng{ std::random_device{}() };

double NowSeconds() {
    return stm_sec(stm_now());
}

void ClearInputState() {
    CoreState& c = Core();
    std::fill(std::begin(c.keyDown), std::end(c.keyDown), false);
    std::fill(std::begin(c.keyPrev), std::end(c.keyPrev), false);
    std::fill(std::begin(c.mouseDown), std::end(c.mouseDown), false);
    std::fill(std::begin(c.mousePrev), std::end(c.mousePrev), false);
}

// Start of a new input frame: what was "current" becomes "previous", and the
// per-frame accumulators reset. Mirrors raylib's PollInputEvents().
void BeginInputFrame() {
    CoreState& c = Core();
    std::copy(std::begin(c.keyDown), std::end(c.keyDown), std::begin(c.keyPrev));
    std::copy(std::begin(c.mouseDown), std::end(c.mouseDown), std::begin(c.mousePrev));
    std::memcpy(c.gamepadButtonPrev, c.gamepadButtonDown, sizeof(c.gamepadButtonPrev));
    std::fill(std::begin(c.keyRepeat), std::end(c.keyRepeat), false);
    std::fill(std::begin(c.keyPressedEvt), std::end(c.keyPressedEvt), false);
    std::fill(std::begin(c.keyReleasedEvt), std::end(c.keyReleasedEvt), false);
    std::fill(std::begin(c.mousePressedEvt), std::end(c.mousePressedEvt), false);
    std::fill(std::begin(c.mouseReleasedEvt), std::end(c.mouseReleasedEvt), false);
    // Typed characters and key presses are per-frame like raylib's queues. Without this, keys pressed while nothing
    // reads them (flying with WASD) pile up and all land in the command bar the moment it gets focus.
    c.charQueue.clear();
    c.keyQueue.clear();
    c.mouseDelta = { 0.0f, 0.0f };
    c.wheel = { 0.0f, 0.0f };
    c.filesDropped = false;
}

void PollEvents() {
    CoreState& c = Core();
    BeginInputFrame();
    GamepadPoll();
    for (int g = 0; g < kMaxGamepads; ++g)
        for (int b = 0; b < kMaxGamepadButtons; ++b)
            if (c.gamepadButtonDown[g][b] && !c.gamepadButtonPrev[g][b]) c.gamepadLastButtonPressed = b;
    if (!flyapp_poll()) c.shouldClose = true;
    if (c.cursorLocked) flyapp_recenter_locked_pointer();
    if (c.exitKey != KEY_NULL && c.exitKey > 0 && c.exitKey < kMaxKeys &&
        c.keyDown[c.exitKey] && !c.keyPrev[c.exitKey]) {
        c.shouldClose = true;
    }
    TickShaderHotReload();
}

// Diagnostics: FLYENGINE_LOG_INPUT=1 prints mouse-lock requests and the first mouse events to stderr.
static bool InputLogEnabled() {
    static const bool on = std::getenv("FLYENGINE_LOG_INPUT") != nullptr;
    return on;
}

// FLYENGINE_LOG_INPUT=1 logs to stderr; a value starting with '/' is a file to append to, so that the player the
// editor starts (its output is discarded) can log too. Each line carries the process id.
static FILE* InputLog() {
    static FILE* f = [] {
        const char* v = std::getenv("FLYENGINE_LOG_INPUT");
        if (v && v[0] == '/') {
            if (FILE* out = std::fopen(v, "a")) {
                setvbuf(out, nullptr, _IOLBF, 0);
#if defined(__linux__)
                std::fprintf(out, "[input] ---- log opened by pid %d ----\n", (int)getpid());
#endif
                return out;
            }
        }
        return stderr;
    }();
    return f;
}

void OnEvent(const sapp_event* ev) {
    DispatchEvent(ev);
}

} // namespace

void DispatchEvent(const sapp_event* ev) {
    if (g_eventHook) g_eventHook(ev);

    CoreState& c = Core();
    switch (ev->type) {
        case SAPP_EVENTTYPE_KEY_DOWN: {
            const int k = (int)ev->key_code;
            if (k > 0 && k < kMaxKeys) {
                if (ev->key_repeat) {
                    c.keyRepeat[k] = true;
                } else {
                    c.keyDown[k] = true;
                    c.keyPressedEvt[k] = true;
                    c.keyQueue.push_back(k);
                }
                // The numpad Enter also counts as Enter, so every KEY_ENTER check (and script/plugin key) accepts both.
                if (k == KEY_KP_ENTER) {
                    if (ev->key_repeat) c.keyRepeat[KEY_ENTER] = true;
                    else { c.keyDown[KEY_ENTER] = true; c.keyPressedEvt[KEY_ENTER] = true; }
                }
            }
            break;
        }
        case SAPP_EVENTTYPE_KEY_UP: {
            const int k = (int)ev->key_code;
            if (k > 0 && k < kMaxKeys) {
                c.keyDown[k] = false;
                c.keyReleasedEvt[k] = true;
                if (k == KEY_KP_ENTER) { c.keyDown[KEY_ENTER] = false; c.keyReleasedEvt[KEY_ENTER] = true; }
            }
            break;
        }
        case SAPP_EVENTTYPE_CHAR:
            if (ev->char_code >= 32 && ev->char_code != 127) c.charQueue.push_back((int)ev->char_code);
            break;
        case SAPP_EVENTTYPE_MOUSE_DOWN: {
            const int b = (int)ev->mouse_button;
            if (InputLogEnabled()) std::fprintf(InputLog(), "[input] button %d down at %.0f,%.0f\n", b, ev->mouse_x, ev->mouse_y);
            if (b >= 0 && b < kMaxMouseButtons) {
                c.mouseDown[b] = true;
                c.mousePressedEvt[b] = true;
            }
            break;
        }
        case SAPP_EVENTTYPE_MOUSE_UP: {
            const int b = (int)ev->mouse_button;
            if (InputLogEnabled()) std::fprintf(InputLog(), "[input] button %d up\n", b);
            if (b >= 0 && b < kMaxMouseButtons) {
                c.mouseDown[b] = false;
                c.mouseReleasedEvt[b] = true;
            }
            break;
        }
        case SAPP_EVENTTYPE_MOUSE_MOVE:
            // Only moves that matter for the lock: while it is requested or active (the picker's plain moves would
            // otherwise use up the log).
            if (InputLogEnabled() && (c.cursorLocked || sapp_mouse_locked())) {
                static int logged = 0;
                if (logged++ < 300) {
                    std::fprintf(InputLog(), "[input] move  want-lock=%d real-lock=%d pos=%.0f,%.0f delta=%.1f,%.1f\n",
                                 (int)c.cursorLocked, (int)sapp_mouse_locked(), ev->mouse_x, ev->mouse_y, ev->mouse_dx, ev->mouse_dy);
                }
            }
            // sokol_app applies lock/unlock requests asynchronously (at the end of
            // a frame, and only while the window has focus). Until the real state
            // matches the requested one, the deltas are position differences from
            // before the change -- a single stale one can be hundreds of pixels
            // and whips a first-person camera off screen -- so drop them.
            if (c.cursorLocked != sapp_mouse_locked()) break;
            if (!c.cursorLocked) c.mousePos = { ev->mouse_x, ev->mouse_y };
            c.mouseDelta.x += ev->mouse_dx;
            c.mouseDelta.y += ev->mouse_dy;
            break;
        case SAPP_EVENTTYPE_MOUSE_SCROLL: {
#if defined(_WIN32)
            // sokol_app reports 4 units per wheel notch on Windows, 1 elsewhere;
            // raylib (and the engine's zoom speeds) expect 1.
            const float scale = 0.25f;
#else
            const float scale = 1.0f;
#endif
            c.wheel.x += ev->scroll_x * scale;
            c.wheel.y += ev->scroll_y * scale;
            break;
        }
        case SAPP_EVENTTYPE_FOCUSED:
            g_windowFocused = true;
            break;
        case SAPP_EVENTTYPE_UNFOCUSED:
            g_windowFocused = false;
            // Key-up events are lost while unfocused; release everything so no
            // key or button stays stuck down.
            std::fill(std::begin(c.keyDown), std::end(c.keyDown), false);
            std::fill(std::begin(c.mouseDown), std::end(c.mouseDown), false);
            break;
        case SAPP_EVENTTYPE_FILES_DROPPED: {
            c.droppedFiles.clear();
            const int n = sapp_get_num_dropped_files();
            for (int i = 0; i < n; ++i) c.droppedFiles.emplace_back(sapp_get_dropped_file_path(i));
            c.filesDropped = !c.droppedFiles.empty();
            break;
        }
        default:
            break;
    }
}

} // namespace rli

using namespace rli;

//------------------------------------------------------------------------------------
// Window
//------------------------------------------------------------------------------------
void SetConfigFlags(unsigned int flags) { Core().configFlags |= flags; }

void InitWindow(int width, int height, const char* title) {
    CoreState& c = Core();
    if (c.windowOpen) {
        SetWindowSize(width, height);
        SetWindowTitle(title);
        c.shouldClose = false;
        return;
    }
    stm_setup();

    // Vulkan backend: make sure this machine can run it before sokol_app tries
    // (and aborts with a panic). Checked once per process.
    static bool graphicsChecked = false;
    if (!graphicsChecked) {
        graphicsChecked = true;
        char why[512] = {};
        // FLYENGINE_FORCE_FALLBACK only means something to the Vulkan build (the
        // one that has a fallback); the fallback executable itself ignores it.
        bool forced = false;
#if defined(FLY_FALLBACK_SUFFIX)
        forced = std::getenv("FLYENGINE_FORCE_FALLBACK") != nullptr;
        if (forced) std::snprintf(why, sizeof(why), "FLYENGINE_FORCE_FALLBACK is set");
#endif
#if defined(FLY_FALLBACK_SUFFIX)
        // Vulkan on a CPU renderer (a VM without GPU acceleration) is far slower than the OpenGL fallback.
        // If there is no fallback executable this returns false and Vulkan runs as before.
        if (!forced && std::getenv("FLYENGINE_FORCE_VULKAN") == nullptr && flyapp_graphics_is_software()) {
            std::fprintf(stderr, "Flyengine: Vulkan is a software renderer here; starting the OpenGL build instead "
                                 "(set FLYENGINE_FORCE_VULKAN=1 to keep Vulkan).\n");
            if (flyapp_start_fallback(FLY_FALLBACK_SUFFIX)) return;   // (does not return on success)
        }
#endif
        if (forced || !flyapp_graphics_usable(why, sizeof(why))) {
            std::fprintf(stderr, "Flyengine: the Vulkan backend cannot run here: %s\n", why);
#if defined(FLY_FALLBACK_SUFFIX)
            if (flyapp_start_fallback(FLY_FALLBACK_SUFFIX)) return;   // (does not return on success)
#endif
            std::fprintf(stderr, "Flyengine: no fallback executable found next to this one. "
                                 "Install a Vulkan 1.3 driver (with VK_EXT_descriptor_buffer) or use the OpenGL build.\n");
            std::exit(1);
        }
    }

    sapp_desc desc{};
    desc.width = width;
    desc.height = height;
    desc.window_title = title ? title : "";
    desc.event_cb = OnEvent;
    desc.high_dpi = false;
    // Everything renders into an offscreen target that has its own depth
    // buffer; the swapchain only receives the final copy.
    desc.depth_format = SAPP_PIXELFORMAT_NONE;
    desc.swap_interval = 1;
#if defined(SOKOL_GLCORE)
    // sokol asks for GL 4.3 by default, but the shaders are GLSL 4.10 (see cmake/FlySokol.cmake). Requesting only what is
    // needed lets the OpenGL build start on drivers that stop at 4.1 (VM graphics, older Mesa).
    desc.gl.major_version = 4;
    desc.gl.minor_version = 1;
#endif
    // raylib only syncs to the display when FLAG_VSYNC_HINT is set; otherwise
    // SetTargetFPS() alone paces the loop.
    desc.disable_vsync = (c.configFlags & FLAG_VSYNC_HINT) == 0;
    desc.fullscreen = (c.configFlags & FLAG_FULLSCREEN_MODE) != 0;
    desc.enable_clipboard = true;
    desc.clipboard_size = 1 << 20;
    desc.enable_dragndrop = true;
    desc.max_dropped_files = 32;
    desc.icon.sokol_default = true;
    desc.logger.func = slog_func;
    flyapp_gl_retry_arm();
    flyapp_open(&desc);
    flyapp_gl_retry_disarm();
    if (InputLogEnabled()) InputLog();   // creates the log file now, so it exists even before any mouse event
    // Matches StartupWMClass in packaging/flyengine.desktop, so the window
    // groups under the launcher's icon (X11; no-op elsewhere).
    flyapp_set_window_class("flyengine");

    c.title = title ? title : "";
    c.windowOpen = true;
    c.shouldClose = false;
    c.exitKey = KEY_ESCAPE;
    c.timeStart = NowSeconds();
    c.frameStart = c.prevFrameStart = 0.0;
    c.frameTime = 0.0;
    c.charQueue.clear();
    c.keyQueue.clear();
    ClearInputState();

    GfxInit(sapp_width(), sapp_height());
    TextInit();
    GamepadInit();

    TraceLog(LOG_INFO, "DISPLAY: Window created (%ix%i) using %s", sapp_width(), sapp_height(), GetGraphicsBackendName());
    PollEvents();
}

void CloseWindow(void) {
    CoreState& c = Core();
    if (!c.windowOpen) return;
    TextShutdown();
    ModelsShutdown();
    ShadersShutdown();
    GfxShutdown();
    GamepadShutdown();
    flyapp_close();
    c.windowOpen = false;
    c.cursorLocked = false;
    c.cursorHidden = false;
    c.cursorShape = -1;   // new window starts with the default cursor
    c.cursorWanted = 0;
    // Flags apply to the next window only, like raylib.
    c.configFlags = 0;
    TraceLog(LOG_INFO, "Window closed successfully");
}

bool WindowShouldClose(void) {
    const CoreState& c = Core();
    return !c.windowOpen || c.shouldClose;
}

bool IsWindowReady(void) { return Core().windowOpen; }
bool IsWindowFullscreen(void) { return Core().windowOpen && sapp_is_fullscreen(); }
bool IsWindowFocused(void) { return Core().windowOpen && rli::g_windowFocused; }

void ToggleFullscreen(void) {
    if (Core().windowOpen) sapp_toggle_fullscreen();
}

void SetWindowTitle(const char* title) {
    CoreState& c = Core();
    c.title = title ? title : "";
    if (c.windowOpen) sapp_set_window_title(c.title.c_str());
}

void SetWindowSize(int width, int height) { flyapp_set_window_size(width, height); }
void SetWindowMinSize(int width, int height) { flyapp_set_min_size(width, height); }
void SetWindowClass(const char* name) { flyapp_set_window_class(name); }

void SetWindowIcons(Image* images, int count) {
    if (!Core().windowOpen || !images || count <= 0) return;
    sapp_icon_desc icon{};
    std::vector<Image> converted;
    count = std::min(count, (int)SAPP_MAX_ICONIMAGES);
    for (int i = 0; i < count; ++i) {
        Image img = ImageCopy(images[i]);
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        converted.push_back(img);
        icon.images[i].width = img.width;
        icon.images[i].height = img.height;
        icon.images[i].pixels = { img.data, (size_t)img.width * (size_t)img.height * 4 };
    }
    sapp_set_icon(&icon);
    for (Image& img : converted) UnloadImage(img);
}

void SetWindowIcon(Image image) { SetWindowIcons(&image, 1); }

void* GetWindowHandle(void) { return flyapp_native_window(); }

int GetScreenWidth(void) { return Core().windowOpen ? sapp_width() : 0; }
int GetScreenHeight(void) { return Core().windowOpen ? sapp_height() : 0; }
int GetRenderWidth(void) { return GetScreenWidth(); }
int GetRenderHeight(void) { return GetScreenHeight(); }
int GetMonitorCount(void) { return 1; }
int GetCurrentMonitor(void) { return 0; }
// Only the monitor the window is on is queried (GetMonitorCount() is 1), so any other index reports 0 like raylib does.
int GetMonitorWidth(int monitor) { return monitor == 0 ? flyapp_monitor_width() : 0; }
int GetMonitorHeight(int monitor) { return monitor == 0 ? flyapp_monitor_height() : 0; }

void SetEventHook(FlyEventHook hook) { g_eventHook = hook; }
void SetShaderHotReload(bool enabled) { rli::SetShaderHotReloadEnabled(enabled); }

//------------------------------------------------------------------------------------
// Frame
//------------------------------------------------------------------------------------
void BeginDrawing(void) {
    CoreState& c = Core();
    c.frameStart = GetTime();
    GfxBeginFrame();
}

static void ApplyMouseCursor();

void EndDrawing(void) {
    CoreState& c = Core();
    GfxEndFrame();
    flyapp_present();
    ApplyMouseCursor();

    // Frame pacing (SetTargetFPS), like raylib's WaitTime-based limiter.
    if (c.targetFrameTime > 0.0) {
        const double target = c.frameStart + c.targetFrameTime;
        double now = GetTime();
        if (now < target) {
            const double remaining = target - now;
            // Sleep for all but the last ~200 us; the short yield loop below absorbs the sleep overshoot.
            if (remaining > 0.0005) {
                std::this_thread::sleep_for(std::chrono::duration<double>(remaining - 0.0002));
            }
            while (GetTime() < target) {
                std::this_thread::yield();
            }
        }
    }

    const double now = GetTime();
    c.frameTime = c.prevFrameStart > 0.0 ? now - c.prevFrameStart : now - c.frameStart;
    c.prevFrameStart = now;
    c.fpsAccum += c.frameTime;
    c.fpsFrames++;
    if (c.fpsAccum >= 0.5) {
        c.fps = (int)std::lround(c.fpsFrames / c.fpsAccum);
        static const bool logFps = std::getenv("FLYENGINE_LOG_FPS") != nullptr;   // diagnostics: FLYENGINE_LOG_FPS=1
        if (logFps) std::fprintf(stderr, "FPS %d (%.1f ms/frame)\n", c.fps, 1000.0 * c.fpsAccum / c.fpsFrames);
        c.fpsAccum = 0.0;
        c.fpsFrames = 0;
    }

    PollEvents();
}

void SetTargetFPS(int fps) {
    Core().targetFrameTime = fps > 0 ? 1.0 / (double)fps : 0.0;
}

int GetFPS(void) { return Core().fps; }
float GetFrameTime(void) { return (float)Core().frameTime; }

double GetTime(void) {
    if (!Core().windowOpen && Core().timeStart == 0.0) {
        stm_setup();
    }
    return NowSeconds() - Core().timeStart;
}

void WaitTime(double seconds) {
    if (seconds > 0.0) std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

//------------------------------------------------------------------------------------
// Input: keyboard
//------------------------------------------------------------------------------------
static bool ValidKey(int key) { return key > 0 && key < kMaxKeys; }

bool IsKeyPressed(int key) {
    if (!ValidKey(key)) return false;
    const CoreState& c = Core();
    return (c.keyDown[key] && !c.keyPrev[key]) || c.keyPressedEvt[key];
}

bool IsKeyPressedRepeat(int key) {
    return ValidKey(key) && Core().keyRepeat[key];
}

bool IsKeyDown(int key) {
    return ValidKey(key) && Core().keyDown[key];
}

bool IsKeyReleased(int key) {
    if (!ValidKey(key)) return false;
    const CoreState& c = Core();
    return (!c.keyDown[key] && c.keyPrev[key]) || c.keyReleasedEvt[key];
}

bool IsKeyUp(int key) { return !IsKeyDown(key); }

int GetKeyPressed(void) {
    CoreState& c = Core();
    if (c.keyQueue.empty()) return 0;
    const int k = c.keyQueue.front();
    c.keyQueue.erase(c.keyQueue.begin());
    return k;
}

int GetCharPressed(void) {
    CoreState& c = Core();
    if (c.charQueue.empty()) return 0;
    const int ch = c.charQueue.front();
    c.charQueue.erase(c.charQueue.begin());
    return ch;
}

void SetExitKey(int key) { Core().exitKey = key; }

//------------------------------------------------------------------------------------
// Input: mouse
//------------------------------------------------------------------------------------
static bool ValidButton(int b) { return b >= 0 && b < kMaxMouseButtons; }

bool IsMouseButtonPressed(int button) {
    if (!ValidButton(button)) return false;
    const CoreState& c = Core();
    return (c.mouseDown[button] && !c.mousePrev[button]) || c.mousePressedEvt[button];
}

bool IsMouseButtonDown(int button) { return ValidButton(button) && Core().mouseDown[button]; }

bool IsMouseButtonReleased(int button) {
    if (!ValidButton(button)) return false;
    const CoreState& c = Core();
    return (!c.mouseDown[button] && c.mousePrev[button]) || c.mouseReleasedEvt[button];
}

bool IsMouseButtonUp(int button) { return !IsMouseButtonDown(button); }

int GetMouseX(void) { return (int)Core().mousePos.x; }
int GetMouseY(void) { return (int)Core().mousePos.y; }
Vector2 GetMousePosition(void) { return Core().mousePos; }
Vector2 GetMouseDelta(void) { return Core().mouseDelta; }

void SetMousePosition(int x, int y) {
    flyapp_set_mouse_position(x, y);
    Core().mousePos = { (float)x, (float)y };
}

float GetMouseWheelMove(void) {
    const Vector2 w = Core().wheel;
    return fabsf(w.x) > fabsf(w.y) ? w.x : w.y;
}

Vector2 GetMouseWheelMoveV(void) { return Core().wheel; }

void SetMouseCursor(int cursor) {
    // Only record the request: callers reset to DEFAULT then set HAND in the same
    // frame, and pushing both to the OS made the cursor flicker.
    Core().cursorWanted = cursor;
}

static void ApplyMouseCursor() {
    CoreState& c = Core();
    const int cursor = c.cursorWanted;
    if (!c.windowOpen || c.cursorShape == cursor) return;
    c.cursorShape = cursor;
    sapp_mouse_cursor mc = SAPP_MOUSECURSOR_DEFAULT;
    switch (cursor) {
        case MOUSE_CURSOR_ARROW: mc = SAPP_MOUSECURSOR_ARROW; break;
        case MOUSE_CURSOR_IBEAM: mc = SAPP_MOUSECURSOR_IBEAM; break;
        case MOUSE_CURSOR_CROSSHAIR: mc = SAPP_MOUSECURSOR_CROSSHAIR; break;
        case MOUSE_CURSOR_POINTING_HAND: mc = SAPP_MOUSECURSOR_POINTING_HAND; break;
        case MOUSE_CURSOR_RESIZE_EW: mc = SAPP_MOUSECURSOR_RESIZE_EW; break;
        case MOUSE_CURSOR_RESIZE_NS: mc = SAPP_MOUSECURSOR_RESIZE_NS; break;
        case MOUSE_CURSOR_RESIZE_NWSE: mc = SAPP_MOUSECURSOR_RESIZE_NWSE; break;
        case MOUSE_CURSOR_RESIZE_NESW: mc = SAPP_MOUSECURSOR_RESIZE_NESW; break;
        case MOUSE_CURSOR_RESIZE_ALL: mc = SAPP_MOUSECURSOR_RESIZE_ALL; break;
        case MOUSE_CURSOR_NOT_ALLOWED: mc = SAPP_MOUSECURSOR_NOT_ALLOWED; break;
        default: break;
    }
    sapp_set_mouse_cursor(mc);
}

void ShowCursor(void) {
    Core().cursorHidden = false;
    if (Core().windowOpen) sapp_show_mouse(true);
}

void HideCursor(void) {
    Core().cursorHidden = true;
    if (Core().windowOpen) sapp_show_mouse(false);
}

bool IsCursorHidden(void) { return Core().cursorHidden || Core().cursorLocked; }

// Mouse lock (raw relative motion) does not work with the absolute pointer a virtual machine gives its guest:
// the camera moves once and then stops. There, "locking" only hides the cursor and the camera follows the
// cursor's position deltas instead. FLYENGINE_MOUSE_LOCK=0/1 overrides the detection.
static bool MouseLockUsable() {
    static const bool usable = [] {
        if (const char* env = std::getenv("FLYENGINE_MOUSE_LOCK")) return env[0] != '0';
#if defined(__linux__)
        for (const char* path : { "/sys/class/dmi/id/sys_vendor", "/sys/class/dmi/id/product_name" }) {
            if (FILE* f = std::fopen(path, "r")) {
                char buf[128] = {};
                std::fread(buf, 1, sizeof(buf) - 1, f);
                std::fclose(f);
                for (const char* vm : { "QEMU", "KVM", "VirtualBox", "innotek", "VMware", "Xen", "Parallels", "Hyper-V",
                                        "Virtual Machine", "Bochs" }) {
                    if (std::strstr(buf, vm)) return false;
                }
            }
        }
#endif
        return true;
    }();
    return usable;
}

void EnableCursor(void) {
    CoreState& c = Core();
    if (InputLogEnabled()) std::fprintf(InputLog(), "[input] EnableCursor\n");
    c.cursorLocked = false;
    c.cursorHidden = false;
    if (c.windowOpen) {
        sapp_lock_mouse(false);
        if (!MouseLockUsable()) sapp_show_mouse(true);   // DisableCursor only hid it there
    }
}

void DisableCursor(void) {
    CoreState& c = Core();
    if (InputLogEnabled()) std::fprintf(InputLog(), "[input] DisableCursor (real lock usable: %d)\n", (int)MouseLockUsable());
    if (!MouseLockUsable()) {
        c.cursorHidden = true;
        if (c.windowOpen) sapp_show_mouse(false);
        return;
    }
    c.cursorLocked = true;
    if (c.windowOpen) sapp_lock_mouse(true);
}

//------------------------------------------------------------------------------------
// Input: gamepad (sokol_app has no gamepad API; polled separately, see rl_gamepad.cpp)
//------------------------------------------------------------------------------------
bool IsGamepadAvailable(int gamepad) {
    return gamepad >= 0 && gamepad < kMaxGamepads && Core().gamepadAvailable[gamepad];
}
bool IsGamepadButtonDown(int gamepad, int button) {
    if (gamepad < 0 || gamepad >= kMaxGamepads || button < 0 || button >= kMaxGamepadButtons) return false;
    return Core().gamepadButtonDown[gamepad][button];
}
bool IsGamepadButtonPressed(int gamepad, int button) {
    if (gamepad < 0 || gamepad >= kMaxGamepads || button < 0 || button >= kMaxGamepadButtons) return false;
    const CoreState& c = Core();
    return c.gamepadButtonDown[gamepad][button] && !c.gamepadButtonPrev[gamepad][button];
}
bool IsGamepadButtonReleased(int gamepad, int button) {
    if (gamepad < 0 || gamepad >= kMaxGamepads || button < 0 || button >= kMaxGamepadButtons) return false;
    const CoreState& c = Core();
    return !c.gamepadButtonDown[gamepad][button] && c.gamepadButtonPrev[gamepad][button];
}
bool IsGamepadButtonUp(int gamepad, int button) { return !IsGamepadButtonDown(gamepad, button); }
int GetGamepadButtonPressed(void) { return Core().gamepadLastButtonPressed; }
int GetGamepadAxisCount(int gamepad) { return IsGamepadAvailable(gamepad) ? kMaxGamepadAxes : 0; }
float GetGamepadAxisMovement(int gamepad, int axis) {
    if (gamepad < 0 || gamepad >= kMaxGamepads || axis < 0 || axis >= kMaxGamepadAxes) return 0.0f;
    return Core().gamepadAxisVal[gamepad][axis];
}
const char* GetGamepadName(int gamepad) {
    if (gamepad < 0 || gamepad >= kMaxGamepads) return "";
    return Core().gamepadName[gamepad].c_str();
}

//------------------------------------------------------------------------------------
// Clipboard, drag & drop
//------------------------------------------------------------------------------------
void SetClipboardText(const char* text) {
    if (Core().windowOpen && text) sapp_set_clipboard_string(text);
}

const char* GetClipboardText(void) {
    return Core().windowOpen ? sapp_get_clipboard_string() : "";
}

bool IsFileDropped(void) { return Core().filesDropped; }

FilePathList LoadDroppedFiles(void) {
    FilePathList list{};
    const auto& files = Core().droppedFiles;
    list.count = (unsigned int)files.size();
    list.paths = (char**)RL_CALLOC(files.size() + 1, sizeof(char*));
    for (size_t i = 0; i < files.size(); ++i) {
        list.paths[i] = (char*)RL_CALLOC(files[i].size() + 1, 1);
        std::memcpy(list.paths[i], files[i].c_str(), files[i].size());
    }
    Core().droppedFiles.clear();
    Core().filesDropped = false;
    return list;
}

void UnloadDroppedFiles(FilePathList files) {
    for (unsigned int i = 0; i < files.count; ++i) RL_FREE(files.paths[i]);
    RL_FREE(files.paths);
}

//------------------------------------------------------------------------------------
// Misc: logging, memory, random, files
//------------------------------------------------------------------------------------
void SetTraceLogLevel(int logLevel) { g_traceLogLevel = logLevel; }

void TraceLog(int logLevel, const char* text, ...) {
    if (logLevel < g_traceLogLevel) return;
    const char* prefix = "";
    switch (logLevel) {
        case LOG_TRACE: prefix = "TRACE: "; break;
        case LOG_DEBUG: prefix = "DEBUG: "; break;
        case LOG_INFO: prefix = "INFO: "; break;
        case LOG_WARNING: prefix = "WARNING: "; break;
        case LOG_ERROR: prefix = "ERROR: "; break;
        case LOG_FATAL: prefix = "FATAL: "; break;
        default: break;
    }
    char buffer[2048];
    va_list args;
    va_start(args, text);
    std::vsnprintf(buffer, sizeof(buffer), text, args);
    va_end(args);
    std::fprintf(logLevel >= LOG_WARNING ? stderr : stdout, "%s%s\n", prefix, buffer);
    if (logLevel == LOG_FATAL) std::exit(EXIT_FAILURE);
}

void* MemAlloc(unsigned int size) { return RL_CALLOC(size, 1); }
void* MemRealloc(void* ptr, unsigned int size) { return RL_REALLOC(ptr, size); }
void MemFree(void* ptr) { RL_FREE(ptr); }

void SetRandomSeed(unsigned int seed) { g_rng.seed(seed); }

int GetRandomValue(int min, int max) {
    if (min > max) std::swap(min, max);
    std::uniform_int_distribution<int> dist(min, max);
    return dist(g_rng);
}

static const FlyVfsHooks* g_vfs = nullptr;
void FlySetVfsHooks(const FlyVfsHooks* hooks) { g_vfs = hooks; }
static bool VfsOwns(const char* path) { return g_vfs && path && g_vfs->handles && g_vfs->handles(path) != 0; }

bool FileExists(const char* fileName) {
    if (!fileName || !*fileName) return false;
    if (VfsOwns(fileName)) return g_vfs->fileExists(fileName) != 0;
    std::error_code ec;
    return fs::is_regular_file(RlU8Path(fileName), ec);
}

bool DirectoryExists(const char* dirPath) {
    if (!dirPath || !*dirPath) return false;
    if (VfsOwns(dirPath)) return g_vfs->dirExists(dirPath) != 0;
    std::error_code ec;
    return fs::is_directory(RlU8Path(dirPath), ec);
}

const char* GetDirectoryPath(const char* filePath) {
    static char buffer[4096];
    buffer[0] = '\0';
    if (!filePath) return buffer;
    const std::string s = filePath;
    const size_t slash = s.find_last_of("/\\");
    if (slash == std::string::npos) {
        std::snprintf(buffer, sizeof(buffer), ".");
    } else if (slash == 0) {
        std::snprintf(buffer, sizeof(buffer), "%c", s[0]);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%s", s.substr(0, slash).c_str());
    }
    return buffer;
}

const char* GetFileName(const char* filePath) {
    if (!filePath) return nullptr;
    const char* a = std::strrchr(filePath, '/');
    const char* b = std::strrchr(filePath, '\\');
    const char* p = a > b ? a : b;
    return p ? p + 1 : filePath;
}

const char* GetFileExtension(const char* fileName) {
    if (!fileName) return nullptr;
    const char* dot = std::strrchr(fileName, '.');
    if (!dot || dot == fileName) return nullptr;
    return dot;
}

const char* GetWorkingDirectory(void) {
    static char buffer[4096];
    std::error_code ec;
    const std::string cwd = fs::current_path(ec).string();
    std::snprintf(buffer, sizeof(buffer), "%s", cwd.c_str());
    return buffer;
}

unsigned char* LoadFileData(const char* fileName, int* dataSize) {
    if (dataSize) *dataSize = 0;
    if (VfsOwns(fileName)) {
        int size = 0;
        unsigned char* data = g_vfs->readFile ? g_vfs->readFile(fileName, &size) : nullptr;
        if (!data) TraceLog(LOG_WARNING, "FILEIO: [%s] Not found in the embedded archive", fileName);
        if (dataSize) *dataSize = data ? size : 0;
        return data;
    }
    FILE* f = fileName ? std::fopen(fileName, "rb") : nullptr;
    if (!f) {
        TraceLog(LOG_WARNING, "FILEIO: [%s] Failed to open file", fileName ? fileName : "(null)");
        return nullptr;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    unsigned char* data = nullptr;
    if (size > 0) {
        data = (unsigned char*)RL_MALLOC((size_t)size);
        const size_t read = std::fread(data, 1, (size_t)size, f);
        if (dataSize) *dataSize = (int)read;
    }
    std::fclose(f);
    return data;
}

void UnloadFileData(unsigned char* data) { RL_FREE(data); }

bool SaveFileData(const char* fileName, void* data, int dataSize) {
    FILE* f = fileName ? std::fopen(fileName, "wb") : nullptr;
    if (!f) return false;
    const size_t written = std::fwrite(data, 1, (size_t)dataSize, f);
    std::fclose(f);
    return written == (size_t)dataSize;
}

void OpenURL(const char* url) {
    if (!url) return;
    if (std::strchr(url, '\'') != nullptr) return;   // refuse to build a broken shell command
#if defined(_WIN32)
    std::string cmd = std::string("explorer \"") + url + "\"";
#else
    std::string cmd = std::string("xdg-open '") + url + "' >/dev/null 2>&1 &";
#endif
    int rc = std::system(cmd.c_str());
    (void)rc;
}

bool IsFileExtension(const char* fileName, const char* ext) {
    // raylib semantics: `ext` may list several extensions separated by ';'
    // (".png;.jpg"), compared case-insensitively.
    const char* fileExt = GetFileExtension(fileName);
    if (!fileExt || !ext) return false;
    std::string lowerFile = fileExt;
    for (char& ch : lowerFile) ch = (char)std::tolower((unsigned char)ch);
    std::string list = ext;
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(';', start);
        if (end == std::string::npos) end = list.size();
        std::string e = list.substr(start, end - start);
        for (char& ch : e) ch = (char)std::tolower((unsigned char)ch);
        if (!e.empty() && e == lowerFile) return true;
        start = end + 1;
    }
    return false;
}

const char* GetFileNameWithoutExt(const char* filePath) {
    static char buffer[1024];
    buffer[0] = '\0';
    if (!filePath) return buffer;
    std::snprintf(buffer, sizeof(buffer), "%s", GetFileName(filePath));
    char* dot = std::strrchr(buffer, '.');
    if (dot && dot != buffer) *dot = '\0';
    return buffer;
}

char* LoadFileText(const char* fileName) {
    int size = 0;
    unsigned char* data = LoadFileData(fileName, &size);
    if (!data && size == 0) {
        if (VfsOwns(fileName)) {
            if (!g_vfs->fileExists(fileName)) return nullptr;
        } else {
            FILE* f = fileName ? std::fopen(fileName, "rb") : nullptr;
            if (!f) return nullptr;
            std::fclose(f);
        }
    }
    char* text = (char*)RL_MALLOC((size_t)size + 1);
    if (size > 0) std::memcpy(text, data, (size_t)size);
    text[size] = '\0';
    UnloadFileData(data);
    return text;
}

void UnloadFileText(char* text) { RL_FREE(text); }

bool SaveFileText(const char* fileName, const char* text) {
    return text && SaveFileData(fileName, (void*)text, (int)std::strlen(text));
}

// Audio runs through miniaudio (include/Engine/Backend/audio.h), not raylib;
// the master volume is only stored for the plugin API.
void SetMasterVolume(float volume) { Core().masterVolume = volume; }
float GetMasterVolume(void) { return Core().masterVolume; }

// Only reachable from raylib's ExportFontAsCode/ExportImageAsCode, which the
// engine does not use.
unsigned char* CompressData(const unsigned char*, int, int* compDataSize) {
    if (compDataSize) *compDataSize = 0;
    TraceLog(LOG_WARNING, "SYSTEM: CompressData() is not available in this build");
    return nullptr;
}


// Test hook: puts the mouse somewhere with a button state, as if the user had, without a real device.
void flyrl_test_set_mouse(float x, float y, bool leftDown) {
    Core().mousePos = { x, y };
    Core().mouseDown[MOUSE_BUTTON_LEFT] = leftDown;
}
