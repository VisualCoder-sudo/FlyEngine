/*
 * PluginLoader.c - Plugin discovery, validation, and loading
 */

#include "PluginLoader.h"
#include "PluginManifest.h"
#include "VersionCompat.h"
#include "../../include/CPluginAPI.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

// ============================================================================
// INTERNAL STRUCTURE
// ============================================================================

struct PluginLoader {
    char* project_root;
    PluginList* discovered;
    LoadedPlugin* loaded_plugins;
    int loaded_count;
    int loaded_capacity;
    void* engine_api;  // CP_EngineAPI* from engine
};

// ============================================================================
// CREATION / DESTRUCTION
// ============================================================================

PluginLoader* PluginLoader_Create(const char* project_root) {
    PluginLoader* loader = calloc(1, sizeof(PluginLoader));
    loader->project_root = strdup(project_root);
    loader->loaded_capacity = 8;
    loader->loaded_plugins = calloc(loader->loaded_capacity, sizeof(LoadedPlugin));
    return loader;
}

void PluginLoader_Destroy(PluginLoader* loader) {
    if (!loader) return;
    PluginLoader_UnloadAll(loader);
    free(loader->project_root);
    if (loader->discovered) PluginList_Free(loader->discovered);
    free(loader->loaded_plugins);
    free(loader);
}

// ============================================================================
// DISCOVERY
// ============================================================================

PluginList* PluginLoader_Discover(PluginLoader* loader) {
    if (!loader) return NULL;
    
    if (loader->discovered) {
        PluginList_Free(loader->discovered);
    }
    
    loader->discovered = PluginList_Scan(loader->project_root, NULL);
    return loader->discovered;
}

// ============================================================================
// VALIDATION
// ============================================================================

bool PluginLoader_Validate(PluginLoader* loader, const char* plugin_path, char** error_out) {
    (void)loader;
    
    // Check id.json
    char id_path[1024];
    snprintf(id_path, sizeof(id_path), "%s/id.json", plugin_path);
    
    char* error = NULL;
    PluginIdentity* id = PluginIdentity_ParseFromFile(id_path, &error);
    if (!id) {
        if (error_out && error) *error_out = error;
        else if (error) free(error);
        return false;
    }
    
    // Check icon.png
    char icon_path[1024];
    snprintf(icon_path, sizeof(icon_path), "%s/icon.png", plugin_path);
    if (!PluginIcon_Validate(icon_path, &error)) {
        if (error_out && error) *error_out = error;
        else if (error) free(error);
        PluginIdentity_Free(id);
        return false;
    }
    
    // Check manifest
    char manifest_path[1024];
    const char* manifest_name = (id->type == PLUGIN_TYPE_FPC) ? "plugin.fpc.json" : "plugin.json";
    snprintf(manifest_path, sizeof(manifest_path), "%s/%s", plugin_path, manifest_name);
    
    PluginManifest* manifest = PluginManifest_ParseFromFile(manifest_path, id->type, &error);
    if (!manifest) {
        if (error_out && error) *error_out = error;
        else if (error) free(error);
        PluginIdentity_Free(id);
        return false;
    }
    
    // Validate identity vs manifest
    bool valid = PluginIdentity_Validate(id, manifest, &error);
    if (!valid && error_out && error) *error_out = error;
    else if (error) free(error);
    
    PluginIdentity_Free(id);
    PluginManifest_Free(manifest);
    return valid;
}

// ============================================================================
// PLUGIN LOADING
// ============================================================================

static LoadedPlugin* find_loaded(PluginLoader* loader, const char* name) {
    for (int i = 0; i < loader->loaded_count; i++) {
        if (strcmp(loader->loaded_plugins[i].info.identity.name, name) == 0) {
            return &loader->loaded_plugins[i];
        }
    }
    return NULL;
}

static int find_loaded_index(PluginLoader* loader, const char* name) {
    for (int i = 0; i < loader->loaded_count; i++) {
        if (strcmp(loader->loaded_plugins[i].info.identity.name, name) == 0) {
            return i;
        }
    }
    return -1;
}

// Find plugin info in discovered list
static PluginInfo* find_discovered(PluginLoader* loader, const char* name) {
    if (!loader->discovered) return NULL;
    for (int i = 0; i < loader->discovered->count; i++) {
        if (strcmp(loader->discovered->plugins[i].identity.name, name) == 0) {
            return &loader->discovered->plugins[i];
        }
    }
    return NULL;
}

// Load a native (.so) plugin
static bool load_native_plugin(PluginLoader* loader, PluginInfo* info, char** error_out) {
    char so_path[1024];
    snprintf(so_path, sizeof(so_path), "%s/build/%s.so", info->identity.plugin_root_path, info->identity.name);
    
    // Try to load the shared library
    void* handle = dlopen(so_path, RTLD_LAZY);
    if (!handle) {
        // Try alternative path
        snprintf(so_path, sizeof(so_path), "%s/%s.so", info->identity.plugin_root_path, info->identity.name);
        handle = dlopen(so_path, RTLD_LAZY);
    }
    if (!handle) {
        if (error_out) *error_out = strdup(dlerror());
        return false;
    }
    
    // Get plugin entry point
    CP_PluginEntry* (*get_entry)(void) = dlsym(handle, "CP_GetPluginEntry");
    if (!get_entry) {
        dlclose(handle);
        if (error_out) *error_out = strdup("Plugin missing CP_GetPluginEntry symbol");
        return false;
    }
    
    CP_PluginEntry* entry = get_entry();
    if (!entry) {
        dlclose(handle);
        if (error_out) *error_out = strdup("CP_GetPluginEntry returned NULL");
        return false;
    }
    
    // Verify API version compatibility
    CP_API_Version engine_ver = CP_API_CURRENT_VERSION;
    CP_API_Version plugin_ver = entry->api_version;
    if (!CP_API_VersionAtLeast(engine_ver, plugin_ver)) {
        dlclose(handle);
        if (error_out) *error_out = strdup("Plugin API version newer than engine supports");
        return false;
    }
    
    // Call on_load
    if (entry->on_load) {
        int result = entry->on_load((const CP_EngineAPI*)loader->engine_api);
        if (result != 0) {
            dlclose(handle);
            if (error_out) *error_out = strdup("Plugin on_load returned error");
            return false;
        }
    }
    
    // Store loaded plugin
    LoadedPlugin* lp = &loader->loaded_plugins[loader->loaded_count++];
    lp->info = *info;
    lp->state = PLUGIN_STATE_LOADED;
    lp->handle = handle;
    lp->plugin_api = entry;
    lp->process_id = -1;
    lp->error_message = NULL;
    
    return true;
}

// Load a library (in-process)
static bool load_lib_plugin(PluginLoader* loader, PluginInfo* info, char** error_out) {
    // For lib type, we just load the symbols into the engine's address space
    // This is similar to native but without process isolation
    
    char so_path[1024];
    snprintf(so_path, sizeof(so_path), "%s/build/%s.so", info->identity.plugin_root_path, info->identity.name);
    
    void* handle = dlopen(so_path, RTLD_LAZY | RTLD_GLOBAL);
    if (!handle) {
        if (error_out) *error_out = strdup(dlerror());
        return false;
    }
    
    CP_PluginEntry* (*get_entry)(void) = dlsym(handle, "CP_GetPluginEntry");
    if (!get_entry) {
        dlclose(handle);
        if (error_out) *error_out = strdup("Library missing CP_GetPluginEntry");
        return false;
    }
    
    CP_PluginEntry* entry = get_entry();
    if (!entry) {
        dlclose(handle);
        if (error_out) *error_out = strdup("Library CP_GetPluginEntry returned NULL");
        return false;
    }
    
    if (entry->on_load) {
        int result = entry->on_load((const CP_EngineAPI*)loader->engine_api);
        if (result != 0) {
            dlclose(handle);
            if (error_out) *error_out = strdup("Library on_load returned error");
            return false;
        }
    }
    
    LoadedPlugin* lp = &loader->loaded_plugins[loader->loaded_count++];
    lp->info = *info;
    lp->state = PLUGIN_STATE_LOADED;
    lp->handle = handle;
    lp->plugin_api = entry;
    lp->process_id = -1;
    lp->error_message = NULL;
    
    return true;
}

// Load FPC plugin (stub - will be implemented in Phase 4)
static bool load_fpc_plugin(PluginLoader* loader, PluginInfo* info, char** error_out) {
    (void)loader; (void)info;
    if (error_out) *error_out = strdup("FPC loading not yet implemented");
    return false;
}

bool PluginLoader_Load(PluginLoader* loader, const char* plugin_name) {
    if (!loader || !plugin_name) return false;
    
    // Check already loaded
    if (find_loaded(loader, plugin_name)) return true;
    
    // Find in discovered
    PluginInfo* info = find_discovered(loader, plugin_name);
    if (!info) {
        // Try to discover if not done yet
        PluginLoader_Discover(loader);
        info = find_discovered(loader, plugin_name);
        if (!info) return false;
    }
    
    // Check capacity
    if (loader->loaded_count >= loader->loaded_capacity) {
        loader->loaded_capacity *= 2;
        loader->loaded_plugins = realloc(loader->loaded_plugins, 
            loader->loaded_capacity * sizeof(LoadedPlugin));
    }
    
    char* error = NULL;
    bool success = false;
    
    switch (info->identity.type) {
        case PLUGIN_TYPE_NAT:
            success = load_native_plugin(loader, info, &error);
            break;
        case PLUGIN_TYPE_LIB:
            success = load_lib_plugin(loader, info, &error);
            break;
        case PLUGIN_TYPE_FPC:
            success = load_fpc_plugin(loader, info, &error);
            break;
        default:
            error = strdup("Unknown plugin type");
            success = false;
    }
    
    if (!success && error) {
        // Store error in a temporary loaded plugin for reporting
        LoadedPlugin* lp = &loader->loaded_plugins[loader->loaded_count++];
        lp->info = *info;
        lp->state = PLUGIN_STATE_ERROR;
        lp->error_message = error;
    } else if (error) {
        free(error);
    }
    
    return success;
}

bool PluginLoader_LoadAll(PluginLoader* loader) {
    if (!loader) return false;
    
    // Discover if not done
    if (!loader->discovered) {
        PluginLoader_Discover(loader);
    }
    
    bool all_ok = true;
    for (int i = 0; i < loader->discovered->count; i++) {
        if (!PluginLoader_Load(loader, loader->discovered->plugins[i].identity.name)) {
            all_ok = false;
        }
    }
    
    return all_ok;
}

void PluginLoader_Unload(PluginLoader* loader, const char* plugin_name) {
    if (!loader || !plugin_name) return;
    
    int idx = find_loaded_index(loader, plugin_name);
    if (idx < 0) return;
    
    LoadedPlugin* lp = &loader->loaded_plugins[idx];
    
    // Call on_unload
    if (lp->plugin_api && lp->state == PLUGIN_STATE_LOADED) {
        CP_PluginEntry* entry = (CP_PluginEntry*)lp->plugin_api;
        if (entry->on_unload) entry->on_unload();
    }
    
    // Close handle
    if (lp->handle) {
        dlclose(lp->handle);
        lp->handle = NULL;
    }
    
    // Remove from array (swap with last)
    loader->loaded_count--;
    if (idx < loader->loaded_count) {
        loader->loaded_plugins[idx] = loader->loaded_plugins[loader->loaded_count];
    }
    memset(&loader->loaded_plugins[loader->loaded_count], 0, sizeof(LoadedPlugin));
}

void PluginLoader_UnloadAll(PluginLoader* loader) {
    if (!loader) return;
    
    for (int i = loader->loaded_count - 1; i >= 0; i--) {
        LoadedPlugin* lp = &loader->loaded_plugins[i];
        if (lp->plugin_api && lp->state == PLUGIN_STATE_LOADED) {
            CP_PluginEntry* entry = (CP_PluginEntry*)lp->plugin_api;
            if (entry->on_unload) entry->on_unload();
        }
        if (lp->handle) {
            dlclose(lp->handle);
        }
        free(lp->error_message);
    }
    loader->loaded_count = 0;
}

// ============================================================================
// RUNTIME
// ============================================================================

void PluginLoader_Update(PluginLoader* loader, float delta_time) {
    if (!loader) return;
    
    for (int i = 0; i < loader->loaded_count; i++) {
        LoadedPlugin* lp = &loader->loaded_plugins[i];
        if (lp->state != PLUGIN_STATE_LOADED) continue;
        
        CP_PluginEntry* entry = (CP_PluginEntry*)lp->plugin_api;
        if (entry->on_update) {
            entry->on_update(delta_time);
        }
    }
}

void PluginLoader_SendEvent(PluginLoader* loader, const void* event) {
    if (!loader) return;
    
    const CP_Event* ev = (const CP_Event*)event;
    
    for (int i = 0; i < loader->loaded_count; i++) {
        LoadedPlugin* lp = &loader->loaded_plugins[i];
        if (lp->state != PLUGIN_STATE_LOADED) continue;
        
        CP_PluginEntry* entry = (CP_PluginEntry*)lp->plugin_api;
        if (entry->on_event) {
            entry->on_event(ev);
        }
    }
}

LoadedPlugin* PluginLoader_GetPlugin(PluginLoader* loader, const char* name) {
    if (!loader || !name) return NULL;
    return find_loaded(loader, name);
}

LoadedPlugin** PluginLoader_GetAllPlugins(PluginLoader* loader, int* count_out) {
    if (!loader || !count_out) return NULL;
    
    LoadedPlugin** arr = malloc(loader->loaded_count * sizeof(LoadedPlugin*));
    for (int i = 0; i < loader->loaded_count; i++) {
        arr[i] = &loader->loaded_plugins[i];
    }
    *count_out = loader->loaded_count;
    return arr;
}

void PluginLoader_SetEnabled(PluginLoader* loader, const char* name, bool enabled) {
    LoadedPlugin* lp = PluginLoader_GetPlugin(loader, name);
    if (!lp) return;
    
    if (enabled && lp->state == PLUGIN_STATE_DISABLED) {
        // Re-enable by calling on_load again
        CP_PluginEntry* entry = (CP_PluginEntry*)lp->plugin_api;
        if (entry->on_load) entry->on_load((const CP_EngineAPI*)loader->engine_api);
        lp->state = PLUGIN_STATE_LOADED;
    } else if (!enabled && lp->state == PLUGIN_STATE_LOADED) {
        // Disable by calling on_unload
        CP_PluginEntry* entry = (CP_PluginEntry*)lp->plugin_api;
        if (entry->on_unload) entry->on_unload();
        lp->state = PLUGIN_STATE_DISABLED;
    }
}

void PluginLoader_SetEngineAPI(PluginLoader* loader, void* engine_api) {
    if (!loader) return;
    loader->engine_api = engine_api;
}

// ============================================================================
// PROCESS MANAGEMENT (STUBS - Phase 2)
// ============================================================================

int PluginLoader_SpawnPluginProcess(PluginLoader* loader, const char* plugin_name) {
    (void)loader; (void)plugin_name;
    // TODO: Phase 2 - implement process isolation
    return -1;
}

bool PluginLoader_IsPluginProcessAlive(PluginLoader* loader, int process_id) {
    (void)loader; (void)process_id;
    return false;
}

void PluginLoader_TerminatePluginProcess(PluginLoader* loader, int process_id) {
    (void)loader; (void)process_id;
}