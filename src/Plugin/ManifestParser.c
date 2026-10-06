/*
 * ManifestParser.c - cJSON-based parser for plugin manifests
 */

#include "PluginManifest.h"
#include "../../extern/cjson/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

// strdup is POSIX, not C standard
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

static char* cJSON_GetString(cJSON* obj, const char* name, bool required) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!item) {
        if (required) return NULL;
        return NULL;
    }
    if (!cJSON_IsString(item)) return NULL;
    return strdup(item->valuestring);
}

static int cJSON_GetInt(cJSON* obj, const char* name, int default_val) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!item || !cJSON_IsNumber(item)) return default_val;
    return item->valueint;
}

static bool cJSON_GetBool(cJSON* obj, const char* name, bool default_val) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!item) return default_val;
    if (cJSON_IsTrue(item)) return true;
    if (cJSON_IsFalse(item)) return false;
    return default_val;
}

static char** cJSON_GetStringArray(cJSON* obj, const char* name, int* count_out) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!item || !cJSON_IsArray(item)) {
        *count_out = 0;
        return NULL;
    }
    int count = cJSON_GetArraySize(item);
    char** arr = calloc(count, sizeof(char*));
    for (int i = 0; i < count; i++) {
        cJSON* elem = cJSON_GetArrayItem(item, i);
        if (cJSON_IsString(elem)) {
            arr[i] = strdup(elem->valuestring);
        }
    }
    *count_out = count;
    return arr;
}

static void free_string_array(char** arr, int count) {
    if (!arr) return;
    for (int i = 0; i < count; i++) free(arr[i]);
    free(arr);
}

// ============================================================================
// PLUGIN IDENTITY PARSING
// ============================================================================

static bool parse_cross_comm(cJSON* obj, PluginIdentity* id) {
    cJSON* cc = cJSON_GetObjectItemCaseSensitive(obj, "CrossCommunication");
    if (!cc) return true;  // Optional
    
    id->cross_comm.allow_outgoing = cJSON_GetStringArray(cc, "allow_outgoing", &id->cross_comm.allow_outgoing_count);
    id->cross_comm.allow_incoming = cJSON_GetStringArray(cc, "allow_incoming", &id->cross_comm.allow_incoming_count);
    return true;
}

static bool parse_dependencies(cJSON* obj, PluginIdentity* id) {
    cJSON* deps = cJSON_GetObjectItemCaseSensitive(obj, "Dependencies");
    if (!deps) return true;  // Optional
    
    if (!cJSON_IsArray(deps)) return false;
    
    int count = cJSON_GetArraySize(deps);
    id->dependencies = calloc(count, sizeof(*id->dependencies));
    id->dependencies_count = count;
    
    for (int i = 0; i < count; i++) {
        cJSON* dep = cJSON_GetArrayItem(deps, i);
        if (!cJSON_IsObject(dep)) continue;
        
        cJSON* name = cJSON_GetObjectItemCaseSensitive(dep, "name");
        cJSON* ver = cJSON_GetObjectItemCaseSensitive(dep, "version");
        if (cJSON_IsString(name)) id->dependencies[i].name = strdup(name->valuestring);
        if (cJSON_IsString(ver)) id->dependencies[i].version = strdup(ver->valuestring);
    }
    return true;
}

static PluginType parse_type(const char* str) {
    if (strcmp(str, "nat") == 0) return PLUGIN_TYPE_NAT;
    if (strcmp(str, "fpc") == 0) return PLUGIN_TYPE_FPC;
    if (strcmp(str, "lib") == 0) return PLUGIN_TYPE_LIB;
    return PLUGIN_TYPE_UNKNOWN;
}

PluginIdentity* PluginIdentity_ParseFromFile(const char* filepath, char** error_out) {
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        if (error_out) *error_out = strdup("Failed to open id.json");
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* data = malloc(sz + 1);
    fread(data, 1, sz, f);
    data[sz] = '\0';
    fclose(f);
    
    cJSON* root = cJSON_Parse(data);
    free(data);
    if (!root) {
        if (error_out) *error_out = strdup("Invalid JSON in id.json");
        return NULL;
    }
    
    PluginIdentity* id = calloc(1, sizeof(PluginIdentity));
    
    // Required fields
    id->name = cJSON_GetString(root, "Name", true);
    id->date_created = cJSON_GetString(root, "DateCreated", true);
    id->publisher = cJSON_GetString(root, "Publisher", true);
    id->version = cJSON_GetString(root, "Version", true);
    
    // Type
    char* type_str = cJSON_GetString(root, "Type", true);
    id->type = parse_type(type_str);
    free(type_str);
    
    // CPluginAPI version
    id->cplugin_api_version = cJSON_GetString(root, "CPluginAPIVersion", true);
    
    // SupportedEngineVersions (required array)
    id->supported_engine_versions = cJSON_GetStringArray(root, "SupportedEngineVersions", &id->supported_engine_versions_count);
    
    // Optional fields
    id->description = cJSON_GetString(root, "Description", false);
    id->category = cJSON_GetString(root, "Category", false);
    id->license = cJSON_GetString(root, "License", false);
    
    // Dependencies
    if (!parse_dependencies(root, id)) {
        PluginIdentity_Free(id);
        cJSON_Delete(root);
        if (error_out) *error_out = strdup("Invalid Dependencies array");
        return NULL;
    }
    
    // CrossCommunication
    if (!parse_cross_comm(root, id)) {
        PluginIdentity_Free(id);
        cJSON_Delete(root);
        if (error_out) *error_out = strdup("Invalid CrossCommunication");
        return NULL;
    }
    
    cJSON_Delete(root);
    
    // Validation
    if (!id->name || !id->date_created || !id->publisher || !id->version ||
        !id->cplugin_api_version || id->supported_engine_versions_count == 0 ||
        id->type == PLUGIN_TYPE_UNKNOWN) {
        PluginIdentity_Free(id);
        if (error_out) *error_out = strdup("Missing required fields in id.json");
        return NULL;
    }
    
    // Validate date format (basic YYYY-MM-DD check)
    if (strlen(id->date_created) != 10 || id->date_created[4] != '-' || id->date_created[7] != '-') {
        PluginIdentity_Free(id);
        if (error_out) *error_out = strdup("DateCreated must be YYYY-MM-DD format");
        return NULL;
    }
    
    return id;
}

void PluginIdentity_Free(PluginIdentity* id) {
    if (!id) return;
    free(id->name);
    free(id->description);
    free(id->category);
    free(id->license);
    free(id->date_created);
    free(id->publisher);
    free(id->version);
    free(id->cplugin_api_version);
    free_string_array(id->supported_engine_versions, id->supported_engine_versions_count);
    for (int i = 0; i < id->dependencies_count; i++) {
        free(id->dependencies[i].name);
        free(id->dependencies[i].version);
    }
    free(id->dependencies);
    free_string_array(id->cross_comm.allow_outgoing, id->cross_comm.allow_outgoing_count);
    free_string_array(id->cross_comm.allow_incoming, id->cross_comm.allow_incoming_count);
    free(id->plugin_root_path);
    free(id->icon_path);
    // NOTE: Do NOT free(id) - the struct is embedded in PluginInfo and freed with list->plugins
}

// ============================================================================
// PLUGIN MANIFEST PARSING
// ============================================================================

static void parse_nat_manifest(cJSON* root, PluginManifest* m) {
    m->nat.entry_point = cJSON_GetString(root, "entry_point", true);
    m->nat.exports = cJSON_GetStringArray(root, "exports", &m->nat.exports_count);
    m->nat.permissions = cJSON_GetStringArray(root, "permissions", &m->nat.permissions_count);
    
    cJSON* sandbox = cJSON_GetObjectItemCaseSensitive(root, "sandbox");
    if (sandbox) {
        m->nat.sandbox.isolate_process = cJSON_GetBool(sandbox, "isolate_process", true);
        m->nat.sandbox.max_memory_mb = cJSON_GetInt(sandbox, "max_memory_mb", 256);
        m->nat.sandbox.allow_threads = cJSON_GetBool(sandbox, "allow_threads", false);
    } else {
        m->nat.sandbox.isolate_process = true;
        m->nat.sandbox.max_memory_mb = 256;
        m->nat.sandbox.allow_threads = false;
    }
}

static void parse_fpc_manifest(cJSON* root, PluginManifest* m) {
    m->fpc.entry_point = cJSON_GetString(root, "entry_point", true);
    m->fpc.exports = cJSON_GetStringArray(root, "exports", &m->fpc.exports_count);
    m->fpc.permissions = cJSON_GetStringArray(root, "permissions", &m->fpc.permissions_count);
}

static void parse_lib_manifest(cJSON* root, PluginManifest* m) {
    m->lib.entry_point = cJSON_GetString(root, "entry_point", true);
    m->lib.exports = cJSON_GetStringArray(root, "exports", &m->lib.exports_count);
    m->lib.header_path = cJSON_GetString(root, "header_path", true);
    
    char* link_type = cJSON_GetString(root, "link_type", false);
    if (link_type && strcmp(link_type, "dynamic") == 0) {
        m->lib.link_type = LINK_DYNAMIC;
    } else {
        m->lib.link_type = LINK_STATIC;
    }
    free(link_type);
}

static void parse_metadata(cJSON* root, PluginManifest* m) {
    cJSON* meta = cJSON_GetObjectItemCaseSensitive(root, "metadata");
    if (!meta) return;
    
    m->metadata.description = cJSON_GetString(meta, "description", false);
    m->metadata.license = cJSON_GetString(meta, "license", false);
    m->metadata.icon = cJSON_GetString(meta, "icon", false);
}

PluginManifest* PluginManifest_ParseFromFile(const char* filepath, PluginType type, char** error_out) {
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        if (error_out) *error_out = strdup("Failed to open manifest file");
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* data = malloc(sz + 1);
    fread(data, 1, sz, f);
    data[sz] = '\0';
    fclose(f);
    
    cJSON* root = cJSON_Parse(data);
    free(data);
    if (!root) {
        if (error_out) *error_out = strdup("Invalid JSON in manifest");
        return NULL;
    }
    
    PluginManifest* m = calloc(1, sizeof(PluginManifest));
    
    switch (type) {
        case PLUGIN_TYPE_NAT: parse_nat_manifest(root, m); break;
        case PLUGIN_TYPE_FPC: parse_fpc_manifest(root, m); break;
        case PLUGIN_TYPE_LIB: parse_lib_manifest(root, m); break;
        default:
            if (error_out) *error_out = strdup("Unknown plugin type");
            PluginManifest_Free(m);
            cJSON_Delete(root);
            return NULL;
    }
    
    parse_metadata(root, m);
    cJSON_Delete(root);
    return m;
}

void PluginManifest_Free(PluginManifest* m) {
    if (!m) return;
    free(m->nat.entry_point);
    free_string_array(m->nat.exports, m->nat.exports_count);
    free_string_array(m->nat.permissions, m->nat.permissions_count);
    
    free(m->fpc.entry_point);
    free_string_array(m->fpc.exports, m->fpc.exports_count);
    free_string_array(m->fpc.permissions, m->fpc.permissions_count);
    
    free(m->lib.entry_point);
    free_string_array(m->lib.exports, m->lib.exports_count);
    free(m->lib.header_path);
    
    free(m->metadata.description);
    free(m->metadata.license);
    free(m->metadata.icon);
    // NOTE: Do NOT free(m) - the struct is embedded in PluginInfo and freed with list->plugins
}

// ============================================================================
// VALIDATION
// ============================================================================

bool PluginIdentity_Validate(const PluginIdentity* id, const PluginManifest* manifest, char** error_out) {
    if (!id || !manifest) {
        if (error_out) *error_out = strdup("Null identity or manifest");
        return false;
    }
    
    // Version must match (would need manifest version - for now just check manifest exists)
    if (id->type == PLUGIN_TYPE_NAT && !manifest->nat.entry_point) {
        if (error_out) *error_out = strdup("NAT plugin missing entry_point in manifest");
        return false;
    }
    if (id->type == PLUGIN_TYPE_FPC && !manifest->fpc.entry_point) {
        if (error_out) *error_out = strdup("FPC plugin missing entry_point in manifest");
        return false;
    }
    if (id->type == PLUGIN_TYPE_LIB && !manifest->lib.entry_point) {
        if (error_out) *error_out = strdup("LIB plugin missing entry_point in manifest");
        return false;
    }
    
    return true;
}

// ============================================================================
// ICON VALIDATION
// ============================================================================

// Simple PNG header check (first 8 bytes)
static bool is_png_header(const unsigned char* data, size_t size) {
    if (size < 8) return false;
    static const unsigned char png_sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    return memcmp(data, png_sig, 8) == 0;
}

// Read PNG dimensions from IHDR chunk
static bool get_png_dimensions(const char* path, int* width, int* height) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    
    // Skip signature
    fseek(f, 8, SEEK_SET);
    
    while (!feof(f)) {
        unsigned char len_bytes[4];
        if (fread(len_bytes, 1, 4, f) != 4) break;
        uint32_t chunk_len = (len_bytes[0] << 24) | (len_bytes[1] << 16) | (len_bytes[2] << 8) | len_bytes[3];
        
        unsigned char type_bytes[4];
        if (fread(type_bytes, 1, 4, f) != 4) break;
        
        if (type_bytes[0] == 'I' && type_bytes[1] == 'H' && type_bytes[2] == 'D' && type_bytes[3] == 'R') {
            unsigned char dim_bytes[8];
            if (fread(dim_bytes, 1, 8, f) == 8) {
                *width = (dim_bytes[0] << 24) | (dim_bytes[1] << 16) | (dim_bytes[2] << 8) | dim_bytes[3];
                *height = (dim_bytes[4] << 24) | (dim_bytes[5] << 16) | (dim_bytes[6] << 8) | dim_bytes[7];
                fclose(f);
                return true;
            }
        }
        
        // Skip chunk data + CRC
        fseek(f, chunk_len + 4, SEEK_CUR);
    }
    
    fclose(f);
    return false;
}

bool PluginIcon_Validate(const char* icon_path, char** error_out) {
    struct stat st;
    if (stat(icon_path, &st) != 0) {
        if (error_out) *error_out = strdup("icon.png not found");
        return false;
    }
    
    FILE* f = fopen(icon_path, "rb");
    if (!f) {
        if (error_out) *error_out = strdup("Cannot open icon.png");
        return false;
    }
    
    unsigned char header[8];
    if (fread(header, 1, 8, f) != 8) {
        fclose(f);
        if (error_out) *error_out = strdup("icon.png too small");
        return false;
    }
    fclose(f);
    
    if (!is_png_header(header, 8)) {
        if (error_out) *error_out = strdup("icon.png is not a valid PNG file");
        return false;
    }
    
    int width, height;
    if (!get_png_dimensions(icon_path, &width, &height)) {
        if (error_out) *error_out = strdup("Could not read PNG dimensions");
        return false;
    }
    
    if (width != height) {
        if (error_out) *error_out = strdup("icon.png must be square (width == height)");
        return false;
    }
    
    if (width < 64 || width > 500) {
        if (error_out) *error_out = strdup("icon.png must be between 64x64 and 500x500");
        return false;
    }
    
    return true;
}

// ============================================================================
// DIRECTORY SCANNING
// ============================================================================

static bool is_plugin_dir(const char* dirname) {
    return dirname[0] != '.' && dirname[0] != '_';
}

static PluginInfo* scan_plugin_dir(const char* base_path, const char* type_dir, const char* plugin_name) {
    char path[1024];
    
    // Build paths
    snprintf(path, sizeof(path), "%s/%s/%s", base_path, type_dir, plugin_name);
    
    // Check id.json
    char id_path[1024];
    snprintf(id_path, sizeof(id_path), "%s/id.json", path);
    
    char* error = NULL;
    PluginIdentity* id = PluginIdentity_ParseFromFile(id_path, &error);
    if (!id) {
        // Silent reject - no error message needed, just return NULL
        if (error) free(error);
        return NULL;
    }
    
    // Check icon.png (hard fail)
    char icon_path[1024];
    snprintf(icon_path, sizeof(icon_path), "%s/icon.png", path);
    if (!PluginIcon_Validate(icon_path, &error)) {
        if (error) free(error);
        PluginIdentity_Free(id);
        return NULL;
    }
    
    id->plugin_root_path = strdup(path);
    id->icon_path = strdup(icon_path);
    
    // Parse manifest
    char manifest_path[1024];
    const char* manifest_name = (id->type == PLUGIN_TYPE_FPC) ? "plugin.fpc.json" : "plugin.json";
    snprintf(manifest_path, sizeof(manifest_path), "%s/%s", path, manifest_name);
    
    PluginManifest* manifest = PluginManifest_ParseFromFile(manifest_path, id->type, &error);
    if (!manifest) {
        if (error) free(error);
        PluginIdentity_Free(id);
        return NULL;
    }
    
    // Validate identity vs manifest
    if (!PluginIdentity_Validate(id, manifest, &error)) {
        if (error) free(error);
        PluginIdentity_Free(id);
        PluginManifest_Free(manifest);
        return NULL;
    }
    
    PluginInfo* info = calloc(1, sizeof(PluginInfo));
    // Transfer ownership - deep copy strings
    info->identity = *id;
    info->manifest = *manifest;
    info->is_valid = true;
    
    // Null out pointers in source to prevent double-free
    id->name = id->description = id->category = id->license = NULL;
    id->date_created = id->publisher = id->version = NULL;
    id->cplugin_api_version = NULL;
    id->supported_engine_versions = NULL;
    id->supported_engine_versions_count = 0;
    id->dependencies = NULL;
    id->dependencies_count = 0;
    id->cross_comm.allow_outgoing = NULL;
    id->cross_comm.allow_outgoing_count = 0;
    id->cross_comm.allow_incoming = NULL;
    id->cross_comm.allow_incoming_count = 0;
    id->plugin_root_path = NULL;
    id->icon_path = NULL;
    
    manifest->nat.entry_point = NULL;
    manifest->nat.exports = NULL;
    manifest->nat.permissions = NULL;
    manifest->nat.exports_count = 0;
    manifest->nat.permissions_count = 0;
    manifest->fpc.entry_point = NULL;
    manifest->fpc.exports = NULL;
    manifest->fpc.permissions = NULL;
    manifest->fpc.exports_count = 0;
    manifest->fpc.permissions_count = 0;
    manifest->lib.entry_point = NULL;
    manifest->lib.exports = NULL;
    manifest->lib.header_path = NULL;
    manifest->lib.exports_count = 0;
    manifest->metadata.description = NULL;
    manifest->metadata.license = NULL;
    manifest->metadata.icon = NULL;
    
    free(id);
    free(manifest);
    return info;
}

static void scan_type_dir(const char* base_path, const char* type_dir, PluginType type, PluginList* list) {
    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s/%s", base_path, type_dir);
    
    struct stat st;
    if (stat(full_path, &st) != 0 || !S_ISDIR(st.st_mode)) return;
    
    // Use opendir/readdir
    DIR* dir = opendir(full_path);
    if (!dir) return;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_plugin_dir(entry->d_name)) continue;
        
        PluginInfo* info = scan_plugin_dir(base_path, type_dir, entry->d_name);
        if (info) {
            if (list->count >= list->capacity) {
                list->capacity = list->capacity ? list->capacity * 2 : 8;
                list->plugins = realloc(list->plugins, list->capacity * sizeof(PluginInfo));
            }
            list->plugins[list->count++] = *info;
            free(info);
        }
    }
    closedir(dir);
}

PluginList* PluginList_Scan(const char* project_root, char** error_out) {
    (void)error_out;
    
    char plugins_path[1024];
    snprintf(plugins_path, sizeof(plugins_path), "%s/plugins", project_root);
    
    struct stat st;
    if (stat(plugins_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        // No plugins directory - return empty list
        PluginList* list = calloc(1, sizeof(PluginList));
        return list;
    }
    
    PluginList* list = calloc(1, sizeof(PluginList));
    
    scan_type_dir(plugins_path, "nat", PLUGIN_TYPE_NAT, list);
    scan_type_dir(plugins_path, "fpc", PLUGIN_TYPE_FPC, list);
    scan_type_dir(plugins_path, "lib", PLUGIN_TYPE_LIB, list);
    
    return list;
}

void PluginList_Free(PluginList* list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        PluginIdentity_Free(&list->plugins[i].identity);
        PluginManifest_Free(&list->plugins[i].manifest);
        free(list->plugins[i].error_message);
    }
    free(list->plugins);
    free(list);
}

PluginType PluginType_FromString(const char* str) {
    if (strcmp(str, "nat") == 0) return PLUGIN_TYPE_NAT;
    if (strcmp(str, "fpc") == 0) return PLUGIN_TYPE_FPC;
    if (strcmp(str, "lib") == 0) return PLUGIN_TYPE_LIB;
    return PLUGIN_TYPE_UNKNOWN;
}

const char* PluginType_ToString(PluginType type) {
    switch (type) {
        case PLUGIN_TYPE_NAT: return "nat";
        case PLUGIN_TYPE_FPC: return "fpc";
        case PLUGIN_TYPE_LIB: return "lib";
        default: return "unknown";
    }
}