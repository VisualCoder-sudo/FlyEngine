/*
 * Hello Plugin - the Flyengine tutorial's sample plugin.
 *
 * A plugin is a shared library (.so / .dll) built against include/CPluginAPI.h.
 * The editor finds it at   <project>/plugins/nat/<name>/build/<anything>.so
 * loads it on startup, and reloads it when the file changes.
 *
 * What this one does:
 *   - adds Plugins > "Hello Plugin" to the top bar
 *   - clicking it opens a small window with a counter and a slider
 *   - writes a line to the Output tab every time you press the button
 *
 * Build it (Linux):
 *   cc -shared -fPIC -Iinclude -o build/hello_plugin.so src/hello_plugin.c
 * Build it (Windows, MinGW):
 *   gcc -shared -Iinclude -o build/hello_plugin.dll src/hello_plugin.c
 *
 * Edit this file, rebuild, and the editor reloads the plugin - no restart.
 */
#include "CPluginAPI.h"
#include <stdio.h>

static const CP_EngineAPI* g_api;    // the engine's function table, handed to us in on_load
static bool  g_windowOpen = false;
static int   g_clicks = 0;
static float g_speed = 1.0f;

/* Called when the user picks Plugins > Hello Plugin. */
static void OnMenuClick(void) { g_windowOpen = true; }

/* on_load: runs once. Register menu items here. Return 0 for success. */
static int OnLoad(const CP_EngineAPI* api) {
    g_api = api;
    api->ui_add_menu_item("Plugins", "Hello Plugin", OnMenuClick);
    api->log(CP_LOG_INFO, "HelloPlugin", "Loaded! Engine API %s", CP_API_VERSION_STRING);
    return 0;
}

/* on_update: runs every frame. Draw UI here. */
static void OnUpdate(float dt) {
    (void)dt;
    if (!g_windowOpen) return;
    CP_Vec2 pos = { 120.0f, 120.0f }, size = { 320.0f, 200.0f };
    if (g_api->ui_begin_window("Hello Plugin", pos, size, &g_windowOpen)) {
        g_api->ui_text("This window is drawn by a plugin.");
        g_api->ui_separator();
        CP_Vec2 autoSize = { 0.0f, 0.0f };
        if (g_api->ui_button("Click me", autoSize)) {
            ++g_clicks;
            g_api->log(CP_LOG_INFO, "HelloPlugin", "Clicked %d time(s)", g_clicks);
        }
        char line[64];
        snprintf(line, sizeof line, "Clicks: %d", g_clicks);
        g_api->ui_text(line);
        g_api->ui_slider_float("Speed", &g_speed, 0.0f, 10.0f);
        g_api->ui_text("Edit src/hello_plugin.c, rebuild, and it reloads.");
    }
    g_api->ui_end_window();   /* always pair with ui_begin_window */
}

/* on_unload: runs on shutdown or before a reload. */
static void OnUnload(void) { g_api->log(CP_LOG_INFO, "HelloPlugin", "Bye!"); }

static CP_PluginEntry g_entry = {
    CP_API_CURRENT_VERSION,
    "HelloPlugin",    /* plugin_name */
    "1.0.0",          /* plugin_version */
    OnLoad,
    OnUnload,
    OnUpdate,
    0,                /* on_event: not used */
    0,                /* on_draw_overlay3d: not used */
};

CP_REGISTER_PLUGIN(g_entry)
