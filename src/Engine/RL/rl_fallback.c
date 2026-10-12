// Startup check for the Vulkan backend, and hand-off to the fallback executable.
//
// sokol's Vulkan backend needs Vulkan 1.3 plus dynamic rendering,
// synchronization2, dual-source blending and VK_EXT_descriptor_buffer. On a
// machine without them sokol_app aborts with an unhelpful panic, so we check
// first (without creating a window) and, if the GPU does not qualify, start the
// "-fallback" executable (OpenGL / D3D11) that sits next to this one.
#include "flyapp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <unistd.h>
#endif

#if defined(SOKOL_VULKAN)
#include <vulkan/vulkan.h>

static bool has_ext(const VkExtensionProperties* props, uint32_t n, const char* name) {
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(props[i].extensionName, name) == 0) return true;
    }
    return false;
}

bool flyapp_graphics_usable(char* why, size_t why_size) {
    if (why_size) why[0] = '\0';
    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst = 0;
    const VkResult res = vkCreateInstance(&ici, 0, &inst);
    if (res != VK_SUCCESS) {
        snprintf(why, why_size, "no Vulkan 1.3 driver found (vkCreateInstance returned %d)", (int)res);
        return false;
    }
    VkPhysicalDevice devs[16];
    uint32_t n = 16;
    if (vkEnumeratePhysicalDevices(inst, &n, devs) < 0 || n == 0) {
        snprintf(why, why_size, "no Vulkan-capable GPU found");
        vkDestroyInstance(inst, 0);
        return false;
    }
    bool ok = false;
    for (uint32_t i = 0; i < n && !ok; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[i], &props);
        if (props.apiVersion < VK_API_VERSION_1_3) {
            snprintf(why, why_size, "%s only supports Vulkan %u.%u (1.3 is required)", props.deviceName,
                     VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion));
            continue;
        }
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(devs[i], 0, &ne, 0);
        VkExtensionProperties* exts = (VkExtensionProperties*)calloc(ne ? ne : 1, sizeof(VkExtensionProperties));
        vkEnumerateDeviceExtensionProperties(devs[i], 0, &ne, exts);
        const bool swapchain = has_ext(exts, ne, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        const bool descbuf = has_ext(exts, ne, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
        free(exts);
        if (!swapchain || !descbuf) {
            snprintf(why, why_size, "%s does not support %s", props.deviceName,
                     !swapchain ? "VK_KHR_swapchain" : "VK_EXT_descriptor_buffer");
            continue;
        }
        VkPhysicalDeviceDescriptorBufferFeaturesEXT db;
        memset(&db, 0, sizeof(db));
        db.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
        VkPhysicalDeviceVulkan13Features f13;
        memset(&f13, 0, sizeof(f13));
        f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        f13.pNext = &db;
        VkPhysicalDeviceFeatures2 f2;
        memset(&f2, 0, sizeof(f2));
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = &f13;
        vkGetPhysicalDeviceFeatures2(devs[i], &f2);
        if (!db.descriptorBuffer || !f13.dynamicRendering || !f13.synchronization2 || !f2.features.dualSrcBlend) {
            snprintf(why, why_size, "%s lacks required Vulkan features (descriptor buffer / dynamic rendering / synchronization2 / dual-source blending)",
                     props.deviceName);
            continue;
        }
        ok = true;
    }
    vkDestroyInstance(inst, 0);
    if (ok && why_size) why[0] = '\0';
    return ok;
}

// True when the only Vulkan device is a CPU renderer (llvmpipe/lavapipe, e.g. a VM without GPU acceleration).
bool flyapp_graphics_is_software(void) {
    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst = 0;
    if (vkCreateInstance(&ici, 0, &inst) != VK_SUCCESS) return false;
    VkPhysicalDevice devs[16];
    uint32_t n = 16;
    bool anyReal = false, anySoftware = false;
    if (vkEnumeratePhysicalDevices(inst, &n, devs) >= 0) {
        for (uint32_t i = 0; i < n; i++) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(devs[i], &props);
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) anySoftware = true;
            else anyReal = true;
        }
    }
    vkDestroyInstance(inst, 0);
    return anySoftware && !anyReal;
}
#else
bool flyapp_graphics_usable(char* why, size_t why_size) {
    if (why_size) why[0] = '\0';
    return true;
}
bool flyapp_graphics_is_software(void) { return false; }
#endif

bool flyapp_start_fallback(const char* suffix) {
    if (!suffix || !*suffix) return false;
#if defined(_WIN32)
    wchar_t exe[MAX_PATH];
    const DWORD len = GetModuleFileNameW(0, exe, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wchar_t path[MAX_PATH + 32];
    wcscpy(path, exe);
    wchar_t* dot = wcsrchr(path, L'.');
    if (dot) *dot = L'\0';
    wchar_t wsuffix[32];
    mbstowcs(wsuffix, suffix, 31);
    wsuffix[31] = L'\0';
    wchar_t base[MAX_PATH + 32];
    wcscpy(base, path);
    wcscat(path, wsuffix);
    wcscat(path, L".exe");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        // Tagged copies (Flyengine_win.exe) keep the tag last: Flyengine-fallback_win.exe.
        wchar_t* name = wcsrchr(base, L'\\');
        wchar_t* us = wcsrchr(name ? name : base, L'_');
        if (!us) return false;
        wchar_t tag[64];
        wcsncpy(tag, us, 63);
        tag[63] = L'\0';
        *us = L'\0';
        wcscpy(path, base);
        wcscat(path, wsuffix);
        wcscat(path, tag);
        wcscat(path, L".exe");
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
    }
    // Same arguments as we were started with.
    const wchar_t* cmd = GetCommandLineW();
    wchar_t* cmdcopy = _wcsdup(cmd);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(path, cmdcopy, 0, 0, FALSE, 0, 0, 0, &si, &pi)) {
        free(cmdcopy);
        return false;
    }
    free(cmdcopy);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    ExitProcess(code);
    return true;
#else
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return false;
    exe[n] = '\0';
    char path[4096 + 64];
    snprintf(path, sizeof(path), "%s%s", exe, suffix);
    if (access(path, X_OK) != 0) {
        // Tagged copies (Flyengine_deb) keep the tag last: Flyengine-fallback_deb.
        char* slash = strrchr(exe, '/');
        char* us = strrchr(slash ? slash : exe, '_');
        if (!us) return false;
        char tagbuf[64];
        snprintf(tagbuf, sizeof(tagbuf), "%s", us);
        *us = '\0';
        snprintf(path, sizeof(path), "%s%s%s", exe, suffix, tagbuf);
        if (access(path, X_OK) != 0) return false;
    }

    // Rebuild argv from /proc/self/cmdline (NUL-separated).
    FILE* f = fopen("/proc/self/cmdline", "rb");
    if (!f) return false;
    static char buf[65536];
    const size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';
    char* argv[256];
    int argc = 0;
    for (size_t off = 0; off < got && argc < 255;) {
        argv[argc++] = buf + off;
        off += strlen(buf + off) + 1;
    }
    if (argc == 0) return false;
    argv[0] = path;
    argv[argc] = 0;
    execv(path, argv);
    return false;   // exec failed
#endif
}

// --- OpenGL software retry ---------------------------------------------------
// If creating the OpenGL context aborts (a VM whose accelerated 3D tops out below 4.1), start the same program
// again with Mesa's CPU renderer, which provides GL 4.5. Armed only while the window is being created.
#if defined(__linux__) && defined(SOKOL_GLCORE)
#include <signal.h>

extern char** environ;
static char* g_retry_argv[256];
static char* g_retry_envp[512];
static char g_retry_cmdline[65536];

static void gl_retry_handler(int sig) {
    (void)sig;
    static const char msg[] = "Flyengine: could not create an OpenGL context; retrying with the software renderer.\n";
    if (write(2, msg, sizeof(msg) - 1) < 0) { /* ignore */ }
    execve("/proc/self/exe", g_retry_argv, g_retry_envp);
    _exit(134);
}

void flyapp_gl_retry_arm(void) {
    if (getenv("FLY_GL_SOFTWARE_RETRY")) return;   // already the retry: let a second failure abort normally
    FILE* f = fopen("/proc/self/cmdline", "rb");
    if (!f) return;
    const size_t got = fread(g_retry_cmdline, 1, sizeof(g_retry_cmdline) - 1, f);
    fclose(f);
    g_retry_cmdline[got] = '\0';
    int argc = 0;
    for (size_t off = 0; off < got && argc < 255;) {
        g_retry_argv[argc++] = g_retry_cmdline + off;
        off += strlen(g_retry_cmdline + off) + 1;
    }
    if (argc == 0) return;
    g_retry_argv[argc] = 0;
    int n = 0;
    for (char** e = environ; *e && n < 508; e++) g_retry_envp[n++] = *e;
    g_retry_envp[n++] = (char*)"LIBGL_ALWAYS_SOFTWARE=1";
    g_retry_envp[n++] = (char*)"GALLIUM_DRIVER=llvmpipe";
    g_retry_envp[n++] = (char*)"FLY_GL_SOFTWARE_RETRY=1";
    g_retry_envp[n] = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = gl_retry_handler;
    sigaction(SIGABRT, &sa, 0);
}

void flyapp_gl_retry_disarm(void) {
    signal(SIGABRT, SIG_DFL);
}
#else
void flyapp_gl_retry_arm(void) {}
void flyapp_gl_retry_disarm(void) {}
#endif
