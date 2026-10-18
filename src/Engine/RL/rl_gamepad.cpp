// Gamepad backend. sokol_app has no gamepad API, so controllers are polled
// outside it, once a frame from PollEvents() (see rl_core.cpp):
//   - Linux: evdev joystick devices, discovered through libudev.
//   - Windows: XInput (up to 4 controllers; DirectInput-only devices are not
//     supported, matching modern raylib/SDL defaults).
// Both sides fill the same CoreState arrays (rl_internal.hpp) that
// IsGamepadButtonDown/GetGamepadAxisMovement/etc. (rl_core.cpp) read from.
#include "rl_internal.hpp"

#include <cstring>

namespace rli {

// Normalizes a raw axis sample to raylib's [-1, 1], where triggers rest at -1
// and read 1 at full press (matches GetGamepadAxisMovement's documented range).
static float NormalizeAxis(long raw, long lo, long hi) {
    if (hi <= lo) return 0.0f;
    float v = (static_cast<float>(raw - lo) / static_cast<float>(hi - lo)) * 2.0f - 1.0f;
    if (v < -1.0f) v = -1.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

} // namespace rli

#if defined(_WIN32)

#include "Engine/Platform/WinBegin.h"
#include <windows.h>
#include <xinput.h>
#include "Engine/Platform/WinEnd.h"

namespace rli {
namespace {

bool ButtonBit(WORD buttons, WORD mask) { return (buttons & mask) != 0; }

} // namespace

void GamepadInit() {}
void GamepadShutdown() {}

void GamepadPoll() {
    CoreState& c = Core();
    for (DWORD i = 0; i < static_cast<DWORD>(kMaxGamepads) && i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE state{};
        const DWORD result = XInputGetState(i, &state);
        const bool available = result == ERROR_SUCCESS;
        c.gamepadAvailable[i] = available;
        if (!available) {
            std::memset(c.gamepadButtonDown[i], 0, sizeof(c.gamepadButtonDown[i]));
            std::memset(c.gamepadAxisVal[i], 0, sizeof(c.gamepadAxisVal[i]));
            c.gamepadName[i].clear();
            continue;
        }
        c.gamepadName[i] = "Xbox Controller";
        bool down[kMaxGamepadButtons] = {};
        const WORD b = state.Gamepad.wButtons;
        down[GAMEPAD_BUTTON_LEFT_FACE_UP] = ButtonBit(b, XINPUT_GAMEPAD_DPAD_UP);
        down[GAMEPAD_BUTTON_LEFT_FACE_RIGHT] = ButtonBit(b, XINPUT_GAMEPAD_DPAD_RIGHT);
        down[GAMEPAD_BUTTON_LEFT_FACE_DOWN] = ButtonBit(b, XINPUT_GAMEPAD_DPAD_DOWN);
        down[GAMEPAD_BUTTON_LEFT_FACE_LEFT] = ButtonBit(b, XINPUT_GAMEPAD_DPAD_LEFT);
        down[GAMEPAD_BUTTON_RIGHT_FACE_UP] = ButtonBit(b, XINPUT_GAMEPAD_Y);
        down[GAMEPAD_BUTTON_RIGHT_FACE_RIGHT] = ButtonBit(b, XINPUT_GAMEPAD_B);
        down[GAMEPAD_BUTTON_RIGHT_FACE_DOWN] = ButtonBit(b, XINPUT_GAMEPAD_A);
        down[GAMEPAD_BUTTON_RIGHT_FACE_LEFT] = ButtonBit(b, XINPUT_GAMEPAD_X);
        down[GAMEPAD_BUTTON_LEFT_TRIGGER_1] = ButtonBit(b, XINPUT_GAMEPAD_LEFT_SHOULDER);
        down[GAMEPAD_BUTTON_RIGHT_TRIGGER_1] = ButtonBit(b, XINPUT_GAMEPAD_RIGHT_SHOULDER);
        down[GAMEPAD_BUTTON_LEFT_TRIGGER_2] = state.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
        down[GAMEPAD_BUTTON_RIGHT_TRIGGER_2] = state.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
        down[GAMEPAD_BUTTON_MIDDLE_LEFT] = ButtonBit(b, XINPUT_GAMEPAD_BACK);
        down[GAMEPAD_BUTTON_MIDDLE_RIGHT] = ButtonBit(b, XINPUT_GAMEPAD_START);
        down[GAMEPAD_BUTTON_LEFT_THUMB] = ButtonBit(b, XINPUT_GAMEPAD_LEFT_THUMB);
        down[GAMEPAD_BUTTON_RIGHT_THUMB] = ButtonBit(b, XINPUT_GAMEPAD_RIGHT_THUMB);
        std::memcpy(c.gamepadButtonDown[i], down, sizeof(down));

        c.gamepadAxisVal[i][GAMEPAD_AXIS_LEFT_X] = NormalizeAxis(state.Gamepad.sThumbLX, -32768, 32767);
        c.gamepadAxisVal[i][GAMEPAD_AXIS_LEFT_Y] = -NormalizeAxis(state.Gamepad.sThumbLY, -32768, 32767);
        c.gamepadAxisVal[i][GAMEPAD_AXIS_RIGHT_X] = NormalizeAxis(state.Gamepad.sThumbRX, -32768, 32767);
        c.gamepadAxisVal[i][GAMEPAD_AXIS_RIGHT_Y] = -NormalizeAxis(state.Gamepad.sThumbRY, -32768, 32767);
        c.gamepadAxisVal[i][GAMEPAD_AXIS_LEFT_TRIGGER] = NormalizeAxis(state.Gamepad.bLeftTrigger, 0, 255);
        c.gamepadAxisVal[i][GAMEPAD_AXIS_RIGHT_TRIGGER] = NormalizeAxis(state.Gamepad.bRightTrigger, 0, 255);
    }
}

} // namespace rli

#elif defined(__linux__)

#include <libudev.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <array>
#include <string>

namespace rli {
namespace {

struct AxisRange { int lo = 0, hi = 0; bool known = false; };

struct Device {
    int fd = -1;
    std::string node;              // /dev/input/eventN, identifies the device across rescans
    std::array<AxisRange, ABS_MAX + 1> axisRange{};
};

Device g_devices[kMaxGamepads];
udev* g_udev = nullptr;
double g_lastScan = -1.0;

int ButtonFor(int code) {
    switch (code) {
        case BTN_SOUTH: return GAMEPAD_BUTTON_RIGHT_FACE_DOWN;
        case BTN_EAST: return GAMEPAD_BUTTON_RIGHT_FACE_RIGHT;
        case BTN_NORTH: return GAMEPAD_BUTTON_RIGHT_FACE_UP;
        case BTN_WEST: return GAMEPAD_BUTTON_RIGHT_FACE_LEFT;
        case BTN_DPAD_UP: return GAMEPAD_BUTTON_LEFT_FACE_UP;
        case BTN_DPAD_RIGHT: return GAMEPAD_BUTTON_LEFT_FACE_RIGHT;
        case BTN_DPAD_DOWN: return GAMEPAD_BUTTON_LEFT_FACE_DOWN;
        case BTN_DPAD_LEFT: return GAMEPAD_BUTTON_LEFT_FACE_LEFT;
        case BTN_TL: return GAMEPAD_BUTTON_LEFT_TRIGGER_1;
        case BTN_TR: return GAMEPAD_BUTTON_RIGHT_TRIGGER_1;
        case BTN_TL2: return GAMEPAD_BUTTON_LEFT_TRIGGER_2;
        case BTN_TR2: return GAMEPAD_BUTTON_RIGHT_TRIGGER_2;
        case BTN_SELECT: return GAMEPAD_BUTTON_MIDDLE_LEFT;
        case BTN_MODE: return GAMEPAD_BUTTON_MIDDLE;
        case BTN_START: return GAMEPAD_BUTTON_MIDDLE_RIGHT;
        case BTN_THUMBL: return GAMEPAD_BUTTON_LEFT_THUMB;
        case BTN_THUMBR: return GAMEPAD_BUTTON_RIGHT_THUMB;
        default: return -1;
    }
}

int AxisFor(int code) {
    switch (code) {
        case ABS_X: return GAMEPAD_AXIS_LEFT_X;
        case ABS_Y: return GAMEPAD_AXIS_LEFT_Y;
        case ABS_RX: return GAMEPAD_AXIS_RIGHT_X;
        case ABS_RY: return GAMEPAD_AXIS_RIGHT_Y;
        case ABS_Z: return GAMEPAD_AXIS_LEFT_TRIGGER;
        case ABS_RZ: return GAMEPAD_AXIS_RIGHT_TRIGGER;
        default: return -1;
    }
}

void CloseDevice(Device& d) {
    if (d.fd >= 0) close(d.fd);
    d.fd = -1;
    d.node.clear();
    d.axisRange = {};
}

bool SlotTaken(const std::string& node) {
    for (const Device& d : g_devices) if (d.fd >= 0 && d.node == node) return true;
    return false;
}

// Caches each absolute axis' reported range (EVIOCGABS), used to normalize samples later.
void CacheAxisRanges(Device& d) {
    for (int code = 0; code <= ABS_MAX; ++code) {
        if (AxisFor(code) < 0) continue;
        input_absinfo info{};
        if (ioctl(d.fd, EVIOCGABS(code), &info) == 0) d.axisRange[code] = { info.minimum, info.maximum, true };
    }
}

// Opens newly plugged joysticks (ID_INPUT_JOYSTICK=1) into free slots, and drops ones that vanished.
// A full rescan every second (rather than a udev_monitor epoll) keeps this a plain per-frame poll.
void RescanDevices() {
    for (Device& d : g_devices) {
        if (d.fd < 0) continue;
        if (access(d.node.c_str(), F_OK) != 0) CloseDevice(d);
    }
    if (!g_udev) return;
    udev_enumerate* en = udev_enumerate_new(g_udev);
    if (!en) return;
    udev_enumerate_add_match_subsystem(en, "input");
    udev_enumerate_add_match_property(en, "ID_INPUT_JOYSTICK", "1");
    udev_enumerate_scan_devices(en);
    udev_list_entry* entry = udev_enumerate_get_list_entry(en);
    for (; entry; entry = udev_list_entry_get_next(entry)) {
        const char* path = udev_list_entry_get_name(entry);
        udev_device* dev = udev_device_new_from_syspath(g_udev, path);
        if (!dev) continue;
        const char* node = udev_device_get_devnode(dev);
        if (node && std::strncmp(node, "/dev/input/event", 17) == 0 && !SlotTaken(node)) {
            for (Device& d : g_devices) {
                if (d.fd >= 0) continue;
                const int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (fd >= 0) {
                    d.fd = fd;
                    d.node = node;
                    CacheAxisRanges(d);
                }
                break;
            }
        }
        udev_device_unref(dev);
    }
    udev_enumerate_unref(en);
}

} // namespace

void GamepadInit() {
    g_udev = udev_new();
    g_lastScan = -1.0;
    RescanDevices();
}

void GamepadShutdown() {
    for (Device& d : g_devices) CloseDevice(d);
    if (g_udev) { udev_unref(g_udev); g_udev = nullptr; }
}

void GamepadPoll() {
    CoreState& c = Core();
    const double now = GetTime();
    if (g_lastScan < 0.0 || now - g_lastScan >= 1.0) { RescanDevices(); g_lastScan = now; }

    for (int i = 0; i < kMaxGamepads; ++i) {
        Device& d = g_devices[i];
        c.gamepadAvailable[i] = d.fd >= 0;
        if (d.fd < 0) {
            std::memset(c.gamepadButtonDown[i], 0, sizeof(c.gamepadButtonDown[i]));
            std::memset(c.gamepadAxisVal[i], 0, sizeof(c.gamepadAxisVal[i]));
            c.gamepadName[i].clear();
            continue;
        }
        if (c.gamepadName[i].empty()) {
            char name[128] = {};
            if (ioctl(d.fd, EVIOCGNAME(sizeof(name)), name) >= 0) c.gamepadName[i] = name;
            else c.gamepadName[i] = "Gamepad";
        }
        input_event ev;
        for (;;) {
            const ssize_t n = read(d.fd, &ev, sizeof(ev));
            if (n != static_cast<ssize_t>(sizeof(ev))) break;   // EAGAIN (nothing pending) or a read error either way
            if (ev.type == EV_KEY) {
                const int button = ButtonFor(ev.code);
                if (button >= 0 && button < kMaxGamepadButtons) c.gamepadButtonDown[i][button] = ev.value != 0;
            } else if (ev.type == EV_ABS) {
                const int axis = AxisFor(ev.code);
                const AxisRange& r = d.axisRange[ev.code];
                if (axis >= 0 && axis < kMaxGamepadAxes && r.known) {
                    float v = NormalizeAxis(ev.value, r.lo, r.hi);
                    if (axis == GAMEPAD_AXIS_LEFT_Y || axis == GAMEPAD_AXIS_RIGHT_Y) v = -v;   // raylib: stick up = positive Y
                    c.gamepadAxisVal[i][axis] = v;
                } else if (ev.code == ABS_HAT0X) {
                    c.gamepadButtonDown[i][GAMEPAD_BUTTON_LEFT_FACE_LEFT] = ev.value < 0;
                    c.gamepadButtonDown[i][GAMEPAD_BUTTON_LEFT_FACE_RIGHT] = ev.value > 0;
                } else if (ev.code == ABS_HAT0Y) {
                    c.gamepadButtonDown[i][GAMEPAD_BUTTON_LEFT_FACE_UP] = ev.value < 0;
                    c.gamepadButtonDown[i][GAMEPAD_BUTTON_LEFT_FACE_DOWN] = ev.value > 0;
                }
            }
        }
    }
}

} // namespace rli

#else

namespace rli {
void GamepadInit() {}
void GamepadShutdown() {}
void GamepadPoll() {}
} // namespace rli

#endif
