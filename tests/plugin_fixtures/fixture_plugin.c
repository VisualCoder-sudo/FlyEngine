/* Test plugin for plugin_manager_test: logs load/unload to $FLY_TEST_PLUGIN_LOG, or fails on_load when built with
 * FIXTURE_FAIL_LOAD. Uses no engine API, so it can load with no engine running. */
#include "Engine/Plugin/CPluginAPI.h"
#include <stdio.h>
#include <stdlib.h>

static void note(const char* what) {
    const char* path = getenv("FLY_TEST_PLUGIN_LOG");
    if (!path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s\n", what);
    fclose(f);
}

#ifdef FIXTURE_FAIL_LOAD
static int on_load(const CP_EngineAPI* api) { (void)api; note("fail"); return 7; }
#else
static int on_load(const CP_EngineAPI* api) { (void)api; note("load"); return 0; }
#endif
static void on_unload(void) { note("unload"); }

static CP_PluginEntry entry = {
    {1, 2, 0}, "Fixture", "1.0.0", on_load, on_unload, NULL, NULL, NULL
};
CP_REGISTER_PLUGIN(entry)
