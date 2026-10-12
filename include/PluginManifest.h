/*
 * PluginManifest.h - Plugin manifest and identity structures
 * 
 * Parses id.json (identity) and plugin.json / plugin.fpc.json (manifest)
 */

#ifndef PLUGIN_MANIFEST_H
#define PLUGIN_MANIFEST_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// PLUGIN TYPE
// ============================================================================

typedef enum {
    PLUGIN_TYPE_UNKNOWN = 0,
    PLUGIN_TYPE_NAT,    // Native C/C++ plugin (sandboxed process)
    PLUGIN_TYPE_FPC,    // FPC script plugin (editor only)
    PLUGIN_TYPE_LIB,    // Static/dynamic library (in-process)
} PluginType;

// ============================================================================
// ID.JSON - PLUGIN IDENTITY (Hard requirement)
// ============================================================================

typedef struct {
    char* name;                    // Required: Display name
    char* description;             // Optional: One-line summary
    char* category;                // Optional: "physics", "rendering", "tools", "audio", "scripting", "utility"
    char* license;                 // Optional: SPDX identifier
    char* date_created;            // Required: ISO 8601 date (YYYY-MM-DD)
    char* publisher;               // Required: Author/organization
    char* version;                 // Required: Semantic version
    PluginType type;               // Required: nat, fpc, lib
    char* cplugin_api_version;     // Required: CPluginAPI version targeted
    char** supported_engine_versions; // Required: Array of engine versions
    int supported_engine_versions_count;
    
    // Dependencies
    struct {
        char* name;
        char* version;
    } *dependencies;
    int dependencies_count;
    
    // Cross-communication permissions
    struct {
        char** allow_outgoing;     // Plugin IDs this plugin may call
        int allow_outgoing_count;
        char** allow_incoming;     // Plugin IDs that may call this plugin
        int allow_incoming_count;
    } cross_comm;
    
    // Resolved paths (filled by loader)
    char* plugin_root_path;        // Path to plugin directory
    char* icon_path;               // Path to icon.png
} PluginIdentity;

// ============================================================================
// PLUGIN MANIFEST (plugin.json / plugin.fpc.json)
// ============================================================================

typedef struct {
    // Native plugin config
    struct {
        char* entry_point;         // e.g., "src/main.c"
        char** exports;            // Function names exported
        int exports_count;
        char** permissions;        // Permission strings
        int permissions_count;
        struct {
            bool isolate_process;
            int max_memory_mb;
            bool allow_threads;
        } sandbox;
    } nat;
    
    // FPC plugin config
    struct {
        char* entry_point;         // e.g., "main.fpc"
        char** exports;            // Function names exported
        int exports_count;
        char** permissions;        // Permission strings
        int permissions_count;
    } fpc;
    
    // Library config
    struct {
        char* entry_point;         // e.g., "src/main.c"
        char** exports;            // Function names exported
        int exports_count;
        char* header_path;         // e.g., "include/texter.h"
        enum { LINK_STATIC, LINK_DYNAMIC } link_type;
    } lib;
    
    // Metadata (common)
    struct {
        char* description;
        char* license;
        char* icon;
    } metadata;
} PluginManifest;

// ============================================================================
// COMPLETE PLUGIN INFO (Identity + Manifest)
// ============================================================================

typedef struct {
    PluginIdentity identity;
    PluginManifest manifest;
    bool is_valid;
    char* error_message;
} PluginInfo;

// ============================================================================
// PARSING FUNCTIONS
// ============================================================================

// Parse id.json file
PluginIdentity* PluginIdentity_ParseFromFile(const char* filepath, char** error_out);

// Parse plugin.json or plugin.fpc.json
PluginManifest* PluginManifest_ParseFromFile(const char* filepath, PluginType type, char** error_out);

// Validate identity against manifest
bool PluginIdentity_Validate(const PluginIdentity* id, const PluginManifest* manifest, char** error_out);

// Validate icon.png (square, 64x64 to 500x500, valid PNG)
bool PluginIcon_Validate(const char* icon_path, char** error_out);

// Free functions
void PluginIdentity_Free(PluginIdentity* id);
void PluginManifest_Free(PluginManifest* manifest);
void PluginInfo_Free(PluginInfo* info);

// ============================================================================
// DIRECTORY SCANNING
// ============================================================================

typedef struct {
    PluginInfo* plugins;
    int count;
    int capacity;
} PluginList;

// Scan plugins directory (plugins/nat, plugins/fpc, plugins/lib)
PluginList* PluginList_Scan(const char* project_root, char** error_out);
void PluginList_Free(PluginList* list);

// ============================================================================
// UTILITY
// ============================================================================

PluginType PluginType_FromString(const char* str);
const char* PluginType_ToString(PluginType type);

#endif // PLUGIN_MANIFEST_H