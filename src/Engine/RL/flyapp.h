#pragma once
// Stepwise driver for sokol_app.
//
// sokol_app normally owns the main loop (sapp_run + frame callback). The engine
// is written around raylib's imperative model instead -- several blocking
// `while (!WindowShouldClose())` loops run one after another (benchmark, splash,
// project manager, editor), each with its own InitWindow/CloseWindow. These
// functions split sokol_app's per-platform run loop into open / poll / present /
// close so that model keeps working on top of sokol_app. They are implemented in
// sokol_impl.c, inside the sokol_app implementation unit, because they call
// sokol_app's private platform functions.
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct sapp_desc;

#ifdef __cplusplus
extern "C" {
#endif

// Creates the window and graphics device. desc->init_cb/frame_cb/cleanup_cb are
// ignored; desc->event_cb receives input events from flyapp_poll().
bool flyapp_open(const struct sapp_desc* desc);
// Pumps window-system events (dispatching them to desc->event_cb) and updates
// frame timing. Returns false once the window has been asked to close.
bool flyapp_poll(void);
// Presents the frame rendered since the last flyapp_poll().
void flyapp_present(void);
// Destroys the window and graphics device.
void flyapp_close(void);
bool flyapp_is_open(void);

void flyapp_set_window_size(int width, int height);
void flyapp_set_min_size(int width, int height);
// X11 WM_CLASS (res_name/res_class); no-op elsewhere.
void flyapp_set_window_class(const char* name);
// While the mouse is locked (first-person look) the hidden pointer is only confined, so a hard push
// leaves it pinned to the window edge, where a screen edge or corner can trigger desktop actions
// that steal focus and free the pointer. Call once per frame to keep it near the window centre.
// X11 only; no-op elsewhere. Raw motion (what drives the camera) is unaffected by the warp.
void flyapp_recenter_locked_pointer(void);
int flyapp_monitor_width(void);
int flyapp_monitor_height(void);
// Warps the mouse cursor to window-relative coordinates.
void flyapp_set_mouse_position(int x, int y);
// Native window handle (HWND on Windows, X11 Window on Linux).
void* flyapp_native_window(void);
// Reads back an RGBA8 sg_image (by id) into dst, top row first. Call between
// frames (outside any pass). Returns false if unsupported or on failure.
bool flyapp_read_image_rgba8(uint32_t image_id, int width, int height, void* dst);

// Vulkan backend only: whether this machine's GPU/driver can run it (always
// true on the other backends). On false, `why` holds a short reason.
bool flyapp_graphics_usable(char* why, size_t why_size);
// Starts the executable next to this one whose name is ours plus `suffix`
// (e.g. "-fallback"), with the same arguments, and does not return on success
// (Linux: exec; Windows: runs it to completion, then exits with its code).
bool flyapp_start_fallback(const char* suffix);

#ifdef __cplusplus
}
#endif
