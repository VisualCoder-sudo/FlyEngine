// Single implementation unit for the sokol headers, plus the stepwise
// sokol_app driver declared in flyapp.h.
//
// The backend (SOKOL_GLCORE / SOKOL_VULKAN / SOKOL_D3D11) comes from CMake.
#define SOKOL_IMPL
#define SOKOL_NO_ENTRY

// Debug builds turn on sokol_gfx's validation layer (FLY_SOKOL_DEBUG; sokol
// also defaults SOKOL_DEBUG on when NDEBUG is unset). For sokol_app, SOKOL_DEBUG
// means "use VK_LAYER_KHRONOS_validation if installed" (see the FLYENGINE PATCH
// in sokol_app.h); FLYENGINE_VULKAN_VALIDATION forces that in release builds.
#if defined(FLY_VULKAN_VALIDATION)
#define SOKOL_DEBUG
#endif
#include "sokol_app.h"
#if defined(FLY_SOKOL_DEBUG) && !defined(SOKOL_DEBUG)
#define SOKOL_DEBUG
#endif
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"

#include "flyapp.h"

static bool _flyapp_open = false;

#if defined(_SAPP_LINUX)

static int _flyapp_min_w = 0;
static int _flyapp_min_h = 0;
static bool _flyapp_was_locked = false;

static void _flyapp_x11_apply_size_hints(void) {
    XSizeHints* hints = XAllocSizeHints();
    hints->flags = PWinGravity;
    hints->win_gravity = CenterGravity;
    if (_flyapp_min_w > 0 && _flyapp_min_h > 0) {
        hints->flags |= PMinSize;
        hints->min_width = _flyapp_min_w;
        hints->min_height = _flyapp_min_h;
    }
    XSetWMNormalHints(_sapp.x11.display, _sapp.x11.window, hints);
    XFree(hints);
}

bool flyapp_open(const struct sapp_desc* desc) {
    if (_flyapp_open) {
        return true;
    }
    _sapp_init_state(desc);
    _sapp.x11.window_state = NormalState;

    XInitThreads();
    XrmInitialize();
    _sapp.x11.display = XOpenDisplay(NULL);
    if (!_sapp.x11.display) {
        _SAPP_PANIC(LINUX_X11_OPEN_DISPLAY_FAILED);
    }
    _sapp.x11.screen = DefaultScreen(_sapp.x11.display);
    _sapp.x11.root = DefaultRootWindow(_sapp.x11.display);
    _sapp_x11_query_system_dpi();
    _sapp.dpi_scale = _sapp.x11.dpi / 96.0f;
    _sapp_x11_init_extensions();
    _sapp_x11_create_standard_cursors();
    XkbSetDetectableAutoRepeat(_sapp.x11.display, true, NULL);
    _sapp_x11_init_keytable();
    #if defined(_SAPP_GLX)
        _sapp_glx_init();
        Visual* visual = 0;
        int depth = 0;
        _sapp_glx_choose_visual(&visual, &depth);
        _sapp_x11_create_window(visual, depth);
        _sapp_glx_create_context();
        if (_sapp.desc.disable_vsync) {
            _sapp_glx_swapinterval(0);
        } else {
            _sapp_glx_swapinterval(_sapp.desc.swap_interval);
        }
    #elif defined(_SAPP_EGL)
        _sapp_egl_init();
    #elif defined(SOKOL_VULKAN)
        _sapp_x11_create_window(0, 0);
        _sapp_vk_init();
    #else
        #error "flyapp: unsupported Linux backend"
    #endif
    sapp_set_icon(&desc->icon);
    _sapp.valid = true;
    _sapp_x11_show_window();
    if (_sapp.fullscreen) {
        _sapp_x11_set_fullscreen(true);
    }
    XFlush(_sapp.x11.display);

    // Events are only delivered once the init callback has run; the engine has
    // no init callback, so mark it as done right away.
    _sapp.first_frame = false;
    _sapp_call_init();
    _flyapp_open = true;
    return true;
}

bool flyapp_poll(void) {
    if (!_flyapp_open) {
        return false;
    }
    _sapp_timing_update(&_sapp.timing, 0.0);
    int count = XPending(_sapp.x11.display);
    while (count--) {
        XEvent event;
        XNextEvent(_sapp.x11.display, &event);
        _sapp_x11_process_event(&event);
    }
    _sapp_x11_update_dimensions_from_window_size();
    return !_sapp.quit_ordered;
}

void flyapp_present(void) {
    if (!_flyapp_open) {
        return;
    }
    // frame_cb is unset, so this only advances the frame counter and presents.
    _sapp_linux_frame();
    _sapp_x11_update_mouse_lock();
    // Keep the locked pointer near the window centre. sokol grabs the pointer
    // wherever it happens to be, so a short sweep would reach the window edge:
    // the camera stops turning there, and under XWayland the pointer can leave
    // the window altogether, which drops the grab for seconds. Relative motion
    // comes from raw events, so re-centring does not disturb it.
    if (_sapp.mouse.locked) {
        Window root_ret, child_ret;
        int root_x, root_y, win_x, win_y;
        unsigned int mask;
        if (XQueryPointer(_sapp.x11.display, _sapp.x11.window, &root_ret, &child_ret,
                          &root_x, &root_y, &win_x, &win_y, &mask)) {
            XWindowAttributes attr;
            if (XGetWindowAttributes(_sapp.x11.display, _sapp.x11.window, &attr)) {
                const int cx = attr.width / 2;
                const int cy = attr.height / 2;
                if (!_flyapp_was_locked || abs(win_x - cx) > attr.width / 4 || abs(win_y - cy) > attr.height / 4) {
                    XWarpPointer(_sapp.x11.display, None, _sapp.x11.window, 0, 0, 0, 0, cx, cy);
                }
            }
        }
    }
    _flyapp_was_locked = _sapp.mouse.locked;
    XFlush(_sapp.x11.display);
    if (_sapp.quit_requested && !_sapp.quit_ordered) {
        _sapp_x11_app_event(SAPP_EVENTTYPE_QUIT_REQUESTED);
        if (_sapp.quit_requested) {
            _sapp.quit_ordered = true;
        }
    }
}

void flyapp_close(void) {
    if (!_flyapp_open) {
        return;
    }
    _sapp_call_cleanup();
    #if defined(_SAPP_GLX)
        _sapp_glx_destroy_context();
    #elif defined(_SAPP_EGL)
        _sapp_egl_destroy();
    #elif defined(SOKOL_VULKAN)
        _sapp_vk_discard();
    #endif
    _sapp_x11_destroy_window();
    _sapp_x11_destroy_standard_cursors();
    XCloseDisplay(_sapp.x11.display);
    _sapp_discard_state();
    _flyapp_open = false;
    _flyapp_min_w = _flyapp_min_h = 0;
    _flyapp_was_locked = false;
}

void flyapp_set_window_size(int width, int height) {
    if (!_flyapp_open || width <= 0 || height <= 0) {
        return;
    }
    XResizeWindow(_sapp.x11.display, _sapp.x11.window, (unsigned int)width, (unsigned int)height);
    XFlush(_sapp.x11.display);
}

void flyapp_set_min_size(int width, int height) {
    _flyapp_min_w = width;
    _flyapp_min_h = height;
    if (_flyapp_open) {
        _flyapp_x11_apply_size_hints();
        XFlush(_sapp.x11.display);
    }
}

void flyapp_set_window_class(const char* name) {
    if (!_flyapp_open || !name) {
        return;
    }
    XClassHint* hint = XAllocClassHint();
    hint->res_name = (char*)name;
    hint->res_class = (char*)name;
    XSetClassHint(_sapp.x11.display, _sapp.x11.window, hint);
    XFree(hint);
    XFlush(_sapp.x11.display);
}

int flyapp_monitor_width(void) {
    return _flyapp_open ? DisplayWidth(_sapp.x11.display, _sapp.x11.screen) : 0;
}

int flyapp_monitor_height(void) {
    return _flyapp_open ? DisplayHeight(_sapp.x11.display, _sapp.x11.screen) : 0;
}

void flyapp_set_mouse_position(int x, int y) {
    if (!_flyapp_open) {
        return;
    }
    XWarpPointer(_sapp.x11.display, None, _sapp.x11.window, 0, 0, 0, 0, x, y);
    XFlush(_sapp.x11.display);
    _sapp.mouse.x = (float)x;
    _sapp.mouse.y = (float)y;
}

void flyapp_recenter_locked_pointer(void) {
    if (!_flyapp_open || !_sapp.mouse.locked) {
        return;
    }
    Window root_ret, child_ret;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;
    if (!XQueryPointer(_sapp.x11.display, _sapp.x11.window, &root_ret, &child_ret,
                       &root_x, &root_y, &win_x, &win_y, &mask)) {
        return;
    }
    const int w = _sapp.window_width, h = _sapp.window_height;
    if (win_x < w / 4 || win_x > w - w / 4 || win_y < h / 4 || win_y > h - h / 4) {
        XWarpPointer(_sapp.x11.display, None, _sapp.x11.window, 0, 0, 0, 0, w / 2, h / 2);
        XFlush(_sapp.x11.display);
    }
}

void* flyapp_native_window(void) {
    return _flyapp_open ? (void*)(uintptr_t)_sapp.x11.window : 0;
}

#elif defined(_SAPP_WIN32)

static int _flyapp_min_w = 0;
static int _flyapp_min_h = 0;
static WNDPROC _flyapp_orig_wndproc = 0;
static bool _flyapp_done = false;

// Subclassed window procedure: adds the minimum-size constraint that sokol_app
// has no option for, then defers to sokol's own window procedure.
static LRESULT CALLBACK _flyapp_wndproc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_GETMINMAXINFO && _flyapp_min_w > 0 && _flyapp_min_h > 0) {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        RECT rect = { 0, 0, _flyapp_min_w, _flyapp_min_h };
        AdjustWindowRectEx(&rect, (DWORD)GetWindowLongPtrW(hWnd, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(hWnd, GWL_EXSTYLE));
        mmi->ptMinTrackSize.x = rect.right - rect.left;
        mmi->ptMinTrackSize.y = rect.bottom - rect.top;
        return 0;
    }
    return CallWindowProcW(_flyapp_orig_wndproc, hWnd, uMsg, wParam, lParam);
}

bool flyapp_open(const struct sapp_desc* desc) {
    if (_flyapp_open) {
        return true;
    }
    _sapp_init_state(desc);
    _sapp_win32_init_console();
    _sapp.win32.is_win10_or_greater = _sapp_win32_is_win10_or_greater();
    _sapp_win32_init_keytable();
    _sapp_win32_utf8_to_wide(_sapp.window_title, _sapp.window_title_wide, sizeof(_sapp.window_title_wide));
    _sapp_win32_init_dpi();
    _sapp_win32_init_cursors();
    _sapp_win32_create_window();
    sapp_set_icon(&desc->icon);
    #if defined(SOKOL_D3D11)
        _sapp_d3d11_create_device_and_swapchain();
        _sapp_d3d11_create_default_render_target();
    #elif defined(SOKOL_GLCORE)
        _sapp_wgl_init();
        _sapp_wgl_load_extensions();
        _sapp_wgl_create_context();
    #elif defined(SOKOL_VULKAN)
        _sapp_vk_init();
    #else
        #error "flyapp: unsupported Windows backend"
    #endif
    _sapp.valid = true;
    _flyapp_orig_wndproc = (WNDPROC)SetWindowLongPtrW(_sapp.win32.hwnd, GWLP_WNDPROC, (LONG_PTR)_flyapp_wndproc);
    _sapp.first_frame = false;
    _sapp_call_init();
    _flyapp_done = false;
    _flyapp_open = true;
    return true;
}

bool flyapp_poll(void) {
    if (!_flyapp_open) {
        return false;
    }
    _sapp_timing_update(&_sapp.timing, 0.0);
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (WM_QUIT == msg.message) {
            _flyapp_done = true;
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !(_flyapp_done || _sapp.quit_ordered);
}

void flyapp_present(void) {
    if (!_flyapp_open) {
        return;
    }
    _sapp_win32_frame(false);
    if (_sapp_win32_update_dimensions()) {
        #if defined(SOKOL_D3D11)
        _sapp_d3d11_resize_default_render_target();
        #endif
        _sapp_win32_app_event(SAPP_EVENTTYPE_RESIZED);
    }
    if (_sapp.quit_requested) {
        PostMessage(_sapp.win32.hwnd, WM_CLOSE, 0, 0);
    }
    _sapp_win32_update_mouse_lock();
}

void flyapp_close(void) {
    if (!_flyapp_open) {
        return;
    }
    _sapp_call_cleanup();
    #if defined(SOKOL_D3D11)
        _sapp_d3d11_destroy_default_render_target();
        _sapp_d3d11_destroy_device_and_swapchain();
    #elif defined(SOKOL_GLCORE)
        _sapp_wgl_destroy_context();
        _sapp_wgl_shutdown();
    #elif defined(SOKOL_VULKAN)
        _sapp_vk_discard();
    #endif
    if (_flyapp_orig_wndproc) {
        SetWindowLongPtrW(_sapp.win32.hwnd, GWLP_WNDPROC, (LONG_PTR)_flyapp_orig_wndproc);
        _flyapp_orig_wndproc = 0;
    }
    _sapp_win32_destroy_window();
    _sapp_win32_destroy_icons();
    _sapp_win32_restore_console();
    _sapp_win32_free_raw_input_data();
    _sapp_discard_state();
    // Drain the WM_QUIT posted by the closing window so the next window's
    // message loop does not see it and quit immediately.
    MSG msg;
    while (PeekMessageW(&msg, NULL, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
    _flyapp_open = false;
    _flyapp_min_w = _flyapp_min_h = 0;
}

void flyapp_set_window_size(int width, int height) {
    if (!_flyapp_open || width <= 0 || height <= 0) {
        return;
    }
    HWND hwnd = _sapp.win32.hwnd;
    RECT rect = { 0, 0, width, height };
    AdjustWindowRectEx(&rect, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    SetWindowPos(hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void flyapp_set_min_size(int width, int height) {
    _flyapp_min_w = width;
    _flyapp_min_h = height;
}

void flyapp_set_window_class(const char* name) {
    (void)name;
}

int flyapp_monitor_width(void) {
    if (!_flyapp_open) {
        return GetSystemMetrics(SM_CXSCREEN);
    }
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(_sapp.win32.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcMonitor.right - mi.rcMonitor.left;
}

int flyapp_monitor_height(void) {
    if (!_flyapp_open) {
        return GetSystemMetrics(SM_CYSCREEN);
    }
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(_sapp.win32.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcMonitor.bottom - mi.rcMonitor.top;
}

void flyapp_set_mouse_position(int x, int y) {
    if (!_flyapp_open) {
        return;
    }
    POINT pt = { x, y };
    ClientToScreen(_sapp.win32.hwnd, &pt);
    SetCursorPos(pt.x, pt.y);
    _sapp.mouse.x = (float)x;
    _sapp.mouse.y = (float)y;
}

void flyapp_recenter_locked_pointer(void) {}

void* flyapp_native_window(void) {
    return _flyapp_open ? (void*)_sapp.win32.hwnd : 0;
}

#else
#error "flyapp: only Linux and Windows are supported"
#endif

bool flyapp_is_open(void) {
    return _flyapp_open;
}

// ---------------------------------------------------------------------------
// Image readback (screenshots). sokol_gfx has no readback API, so this reaches
// into its internals -- which is why it lives in the implementation unit. Must
// be called outside a render pass, after sg_commit(). Writes width*height RGBA8
// pixels, top row first.
// ---------------------------------------------------------------------------
#include <string.h>
#include <stdlib.h>

#if defined(SOKOL_GLCORE) && defined(_WIN32)
// On Windows sokol loads GL through its own prefixed loader, so the plain gl*
// entry points used below do not exist; screenshots are not implemented there.
bool flyapp_read_image_rgba8(uint32_t image_id, int width, int height, void* dst) {
    (void)image_id; (void)width; (void)height; (void)dst;
    return false;
}
#elif defined(SOKOL_GLCORE)
bool flyapp_read_image_rgba8(uint32_t image_id, int width, int height, void* dst) {
    _sg_image_t* img = _sg_lookup_image(image_id);
    if (!img || !dst) return false;
    GLuint tex = img->gl.tex[img->cmn.active_slot];
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, dst);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    sg_reset_state_cache();
    // GL stores render targets bottom-up.
    const size_t row = (size_t)width * 4;
    uint8_t* p = (uint8_t*)dst;
    uint8_t* tmp = (uint8_t*)malloc(row);
    for (int y = 0; y < height / 2; y++) {
        memcpy(tmp, p + (size_t)y * row, row);
        memcpy(p + (size_t)y * row, p + (size_t)(height - 1 - y) * row, row);
        memcpy(p + (size_t)(height - 1 - y) * row, tmp, row);
    }
    free(tmp);
    return true;
}
#elif defined(SOKOL_VULKAN)
bool flyapp_read_image_rgba8(uint32_t image_id, int width, int height, void* dst) {
    _sg_image_t* img = _sg_lookup_image(image_id);
    if (!img || !dst) return false;
    VkDevice dev = _sg.vk.dev;
    vkDeviceWaitIdle(dev);
    const VkDeviceSize size = (VkDeviceSize)width * (VkDeviceSize)height * 4;

    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = 0;
    if (vkCreateBuffer(dev, &bci, 0, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(_sg.vk.phys_dev, &props);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) { type = i; break; }
    }
    VkDeviceMemory mem = 0;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(dev, &mai, 0, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(dev, buf, 0);
        return false;
    }
    vkBindBufferMemory(dev, buf, mem, 0);

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = _sg.vk.queue_family_index;
    VkCommandPool pool = 0;
    vkCreateCommandPool(dev, &pci, 0, &pool);
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = 0;
    vkAllocateCommandBuffers(dev, &cai, &cmd);
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbi);

    // Keep sokol's tracked layout valid: transition back afterwards.
    const VkImageLayout layout = _sg_vk_image_layout(img->vk.cur_access);
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = layout;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img->vk.img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &b);
    VkBufferImageCopy region = { 0 };
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = (uint32_t)width;
    region.imageExtent.height = (uint32_t)height;
    region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, img->vk.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = (layout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : layout;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
    if (layout == VK_IMAGE_LAYOUT_UNDEFINED) img->vk.cur_access = _SG_VK_ACCESS_TEXTURE;
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(_sg.vk.queue, 1, &si, 0);
    vkQueueWaitIdle(_sg.vk.queue);

    void* mapped = 0;
    bool ok = vkMapMemory(dev, mem, 0, size, 0, &mapped) == VK_SUCCESS;
    if (ok) {
        memcpy(dst, mapped, (size_t)size);
        vkUnmapMemory(dev, mem);
    }
    vkDestroyCommandPool(dev, pool, 0);
    vkFreeMemory(dev, mem, 0);
    vkDestroyBuffer(dev, buf, 0);
    return ok;
}
#elif defined(SOKOL_D3D11)
bool flyapp_read_image_rgba8(uint32_t image_id, int width, int height, void* dst) {
    _sg_image_t* img = _sg_lookup_image(image_id);
    if (!img || !dst || !img->d3d11.tex2d) return false;
    D3D11_TEXTURE2D_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = 0;
    if (FAILED(_sg_d3d11_CreateTexture2D(_sg.d3d11.dev, &desc, 0, &staging))) return false;
    #if defined(__cplusplus)
        _sg.d3d11.ctx->CopyResource((ID3D11Resource*)staging, (ID3D11Resource*)img->d3d11.tex2d);
    #else
        _sg.d3d11.ctx->lpVtbl->CopyResource(_sg.d3d11.ctx, (ID3D11Resource*)staging, (ID3D11Resource*)img->d3d11.tex2d);
    #endif
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = SUCCEEDED(_sg_d3d11_Map(_sg.d3d11.ctx, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        for (int y = 0; y < height; y++) {
            memcpy((uint8_t*)dst + (size_t)y * (size_t)width * 4, (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)width * 4);
        }
        _sg_d3d11_Unmap(_sg.d3d11.ctx, (ID3D11Resource*)staging, 0);
    }
    _sg_d3d11_Release(staging);
    return ok;
}
#endif
