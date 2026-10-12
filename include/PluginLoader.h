/*
 * PluginLoader.h - Plugin discovery, validation, and loading
 */

#ifndef PLUGIN_LOADER_H
#define PLUGIN_LOADER_H

#include "PluginManifest.h"
#include <stdbool.h>

// ============================================================================
// PLUGIN LOADER STATE
// ============================================================================

typedef struct PluginLoader PluginLoader;

typedef enum {
    PLUGIN_STATE_UNLOADED = 0,
    PLUGIN_STATE_LOADED,
    PLUGIN_STATE_RUNNING,
    PLUGIN_STATE_ERROR,
    PLUGIN_STATE_DISABLED,
} PluginState;

typedef struct {
    PluginInfo info;
    PluginState state;
    void* handle;              // dlopen handle for nat/lib
    void* plugin_api;          // Plugin's CP_PluginEntry*
    int process_id;            // For sandboxed nat plugins
    char* error_message;
} LoadedPlugin;

// ============================================================================
// LOADER API
// ============================================================================

// Create loader with project root path
PluginLoader* PluginLoader_Create(const char* project_root);

// Destroy loader and unload all plugins
void PluginLoader_Destroy(PluginLoader* loader);

// Discover all plugins (id.json + icon.png + manifest)
// Returns PluginList from ManifestParser
PluginList* PluginLoader_Discover(PluginLoader* loader);

// Validate a specific plugin (for CLI validate command)
bool PluginLoader_Validate(PluginLoader* loader, const char* plugin_path, char** error_out);

// Load all valid plugins
bool PluginLoader_LoadAll(PluginLoader* loader);

// Load a specific plugin by name
bool PluginLoader_Load(PluginLoader* loader, const char* plugin_name);

// Unload a plugin
void PluginLoader_Unload(PluginLoader* loader, const char* plugin_name);

// Unload all plugins
void PluginLoader_UnloadAll(PluginLoader* loader);

// Get loaded plugin by name
LoadedPlugin* PluginLoader_GetPlugin(PluginLoader* loader, const char* name);

// Get all loaded plugins
LoadedPlugin** PluginLoader_GetAllPlugins(PluginLoader* loader, int* count_out);

// Update all running plugins (call per frame)
void PluginLoader_Update(PluginLoader* loader, float delta_time);

// Send event to all plugins
void PluginLoader_SendEvent(PluginLoader* loader, const void* event);

// Enable/disable plugin
void PluginLoader_SetEnabled(PluginLoader* loader, const char* name, bool enabled);

// Set engine API pointers (called by engine on startup)
void PluginLoader_SetEngineAPI(PluginLoader* loader, void* engine_api);

// ============================================================================
// PLUGIN PROCESS MANAGEMENT (for nat plugins)
// ============================================================================

// Spawn plugin in isolated process (Linux: fork+exec, Windows: CreateProcess)
// Returns process ID or -1 on failure
int PluginLoader_SpawnPluginProcess(PluginLoader* loader, const char* plugin_name);

// Check if plugin process is alive
bool PluginLoader_IsPluginProcessAlive(PluginLoader* loader, int process_id);

// Terminate plugin process
void PluginLoader_TerminatePluginProcess(PluginLoader* loader, int process_id);

#endif // PLUGIN_LOADER_H