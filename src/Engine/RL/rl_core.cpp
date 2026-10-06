// raylib core module on sokol_app: window lifecycle, frame pacing, input,
// timing, files and misc utilities.
#include "rl_internal.hpp"

#include "flyapp.h"
#include "sokol_app.h"
#include "sokol_log.h"
#include "sokol_time.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <thread>

namespace fs = std::filesystem;

namespace rli {

CoreState& Core() {
    static CoreState s;
    return s;
}

namespace {

FlyEventHook g_eventHook = nullptr;
int g_traceLogLevel = LOG_INFO;
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
    std::fill(std::begin(c.keyRepeat), std::end(c.keyRepeat), false);
    std::fill(std::begin(c.keyPressedEvt), std::end(c.keyPressedEvt), false);
    std::fill(std::begin(c.keyReleasedEvt), std::end(c.keyReleasedEvt), false);
    std::fill(std::begin(c.mousePressedEvt), std::end(c.mousePressedEvt), false);
    std::fill(std::begin(c.mouseReleasedEvt), std::end(c.mouseReleasedEvt), false);
    c.mouseDelta = { 0.0f, 0.0f };
    c.wheel = { 0.0f, 0.0f };
    c.filesDropped = false;
}

void PollEvents() {
    CoreState& c = Core();
    BeginInputFrame();
    if (!flyapp_poll()) c.shouldClose = true;
    if (c.exitKey != KEY_NULL && c.exitKey > 0 && c.exitKey < kMaxKeys &&
        c.keyDown[c.exitKey] && !c.keyPrev[c.exitKey]) {
        c.shouldClose = true;
    }
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
            }
            break;
        }
        case SAPP_EVENTTYPE_KEY_UP: {
            const int k = (int)ev->key_code;
            if (k > 0 && k < kMaxKeys) {
                c.keyDown[k] = false;
                c.keyReleasedEvt[k] = true;
            }
            break;
        }
        case SAPP_EVENTTYPE_CHAR:
            if (ev->char_code >= 32 && ev->char_code != 127) c.charQueue.push_back((int)ev->char_code);
            break;
        case SAPP_EVENTTYPE_MOUSE_DOWN: {
            const int b = (int)ev->mouse_button;
            if (b >= 0 && b < kMaxMouseButtons) {
                c.mouseDown[b] = true;
                c.mousePressedEvt[b] = true;
            }
            break;
        }
        case SAPP_EVENTTYPE_MOUSE_UP: {
            const int b = (int)ev->mouse_button;
            if (b >= 0 && b < kMaxMouseButtons) {
                c.mouseDown[b] = false;
                c.mouseReleasedEvt[b] = true;
            }
            break;
        }
        case SAPP_EVENTTYPE_MOUSE_MOVE:
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
        case SAPP_EVENTTYPE_UNFOCUSED:
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
    flyapp_open(&desc);
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
    flyapp_close();
    c.windowOpen = false;
    c.cursorLocked = false;
    c.cursorHidden = false;
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
bool IsWindowFocused(void) { return Core().windowOpen; }

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
int GetMonitorWidth(int) { return flyapp_monitor_width(); }
int GetMonitorHeight(int) { return flyapp_monitor_height(); }

void SetEventHook(FlyEventHook hook) { g_eventHook = hook; }

//------------------------------------------------------------------------------------
// Frame
//------------------------------------------------------------------------------------
void BeginDrawing(void) {
    CoreState& c = Core();
    c.frameStart = GetTime();
    GfxBeginFrame();
}

void EndDrawing(void) {
    CoreState& c = Core();
    GfxEndFrame();
    flyapp_present();

    // Frame pacing (SetTargetFPS), like raylib's WaitTime-based limiter.
    if (c.targetFrameTime > 0.0) {
        const double target = c.frameStart + c.targetFrameTime;
        double now = GetTime();
        if (now < target) {
            const double remaining = target - now;
            if (remaining > 0.002) {
                std::this_thread::sleep_for(std::chrono::duration<double>(remaining - 0.001));
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
    CoreState& c = Core();
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

void EnableCursor(void) {
    CoreState& c = Core();
    c.cursorLocked = false;
    c.cursorHidden = false;
    if (c.windowOpen) sapp_lock_mouse(false);
}

void DisableCursor(void) {
    CoreState& c = Core();
    c.cursorLocked = true;
    if (c.windowOpen) sapp_lock_mouse(true);
}

//------------------------------------------------------------------------------------
// Input: gamepad (sokol_app has no gamepad support; stubbed)
//------------------------------------------------------------------------------------
bool IsGamepadAvailable(int) { return false; }
bool IsGamepadButtonPressed(int, int) { return false; }
bool IsGamepadButtonDown(int, int) { return false; }
bool IsGamepadButtonReleased(int, int) { return false; }
float GetGamepadAxisMovement(int, int) { return 0.0f; }

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

bool FileExists(const char* fileName) {
    if (!fileName || !*fileName) return false;
    std::error_code ec;
    return fs::is_regular_file(fs::u8path(fileName), ec);
}

bool DirectoryExists(const char* dirPath) {
    if (!dirPath || !*dirPath) return false;
    std::error_code ec;
    return fs::is_directory(fs::u8path(dirPath), ec);
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
        FILE* f = fileName ? std::fopen(fileName, "rb") : nullptr;
        if (!f) return nullptr;
        std::fclose(f);
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
