/*
 * PluginIdentityValidator.c - Hard-fail validation for plugin identity
 */

#include "PluginIdentityValidator.h"
#include "PluginManifest.h"
#include "VersionCompat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

// ============================================================================
// INTERNAL
// ============================================================================

static void set_error(char** error_out, const char* fmt, ...) {
    if (!error_out) return;
    va_list args;
    va_start(args, fmt);
    char* buf = NULL;
    vasprintf(&buf, fmt, args);
    va_end(args);
    *error_out = buf;
}

static ValidatorError check_required_fields(PluginIdentity* id, char** error_out) {
    if (!id->name || strlen(id->name) == 0) {
        set_error(error_out, "Missing required field: Name");
        return VALIDATOR_ERR_MISSING_REQUIRED_FIELD;
    }
    if (!id->date_created || strlen(id->date_created) == 0) {
        set_error(error_out, "Missing required field: DateCreated");
        return VALIDATOR_ERR_MISSING_REQUIRED_FIELD;
    }
    if (!id->publisher || strlen(id->publisher) == 0) {
        set_error(error_out, "Missing required field: Publisher");
        return VALIDATOR_ERR_MISSING_REQUIRED_FIELD;
    }
    if (!id->version || strlen(id->version) == 0) {
        set_error(error_out, "Missing required field: Version");
        return VALIDATOR_ERR_MISSING_REQUIRED_FIELD;
    }
    if (!id->cplugin_api_version || strlen(id->cplugin_api_version) == 0) {
        set_error(error_out, "Missing required field: CPluginAPIVersion");
        return VALIDATOR_ERR_MISSING_REQUIRED_FIELD;
    }
    if (id->supported_engine_versions_count == 0) {
        set_error(error_out, "Missing required field: SupportedEngineVersions (must have at least one)");
        return VALIDATOR_ERR_NO_SUPPORTED_VERSIONS;
    }
    return VALIDATOR_OK;
}

static ValidatorError check_date_format(const char* date, char** error_out) {
    if (strlen(date) != 10) return VALIDATOR_ERR_INVALID_DATE_FORMAT;
    if (date[4] != '-' || date[7] != '-') return VALIDATOR_ERR_INVALID_DATE_FORMAT;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (!isdigit(date[i])) return VALIDATOR_ERR_INVALID_DATE_FORMAT;
    }
    return VALIDATOR_OK;
}

// ============================================================================
// MAIN VALIDATION
// ============================================================================

ValidatorError PluginIdentityValidator_Validate(
    const char* plugin_root,
    PluginType expected_type,
    PluginIdentity** out_identity,
    PluginManifest** out_manifest,
    char** error_out
) {
    // Parse id.json
    char id_path[1024];
    snprintf(id_path, sizeof(id_path), "%s/id.json", plugin_root);
    
    PluginIdentity* id = PluginIdentity_ParseFromFile(id_path, error_out);
    if (!id) {
        return VALIDATOR_ERR_NO_ID_JSON;  // Already set error
    }
    
    // Check required fields
    ValidatorError err = check_required_fields(id, error_out);
    if (err != VALIDATOR_OK) {
        PluginIdentity_Free(id);
        return err;
    }
    
    // Check date format
    err = check_date_format(id->date_created, error_out);
    if (err != VALIDATOR_OK) {
        PluginIdentity_Free(id);
        return err;
    }
    
    // Check type matches directory
    if (id->type != expected_type) {
        set_error(error_out, "Type mismatch: id.json Type='%s' but in '%s' directory", 
            PluginType_ToString(id->type), PluginType_ToString(expected_type));
        PluginIdentity_Free(id);
        return VALIDATOR_ERR_TYPE_MISMATCH;
    }
    
    // Check icon.png
    char icon_path[1024];
    snprintf(icon_path, sizeof(icon_path), "%s/icon.png", plugin_root);
    if (!PluginIcon_Validate(icon_path, error_out)) {
        ValidatorError icon_err = VALIDATOR_ERR_NO_ICON;
        const char* msg = *error_out;
        if (msg) {
            if (strstr(msg, "not a valid PNG")) icon_err = VALIDATOR_ERR_ICON_NOT_PNG;
            else if (strstr(msg, "square")) icon_err = VALIDATOR_ERR_ICON_NOT_SQUARE;
            else if (strstr(msg, "between 64x64 and 500x500")) icon_err = VALIDATOR_ERR_ICON_SIZE_VIOLATION;
        }
        PluginIdentity_Free(id);
        return icon_err;
    }
    
    // Parse manifest
    char manifest_path[1024];
    const char* manifest_name = (expected_type == PLUGIN_TYPE_FPC) ? "plugin.fpc.json" : "plugin.json";
    snprintf(manifest_path, sizeof(manifest_path), "%s/%s", plugin_root, manifest_name);
    
    PluginManifest* manifest = PluginManifest_ParseFromFile(manifest_path, expected_type, error_out);
    if (!manifest) {
        PluginIdentity_Free(id);
        if (error_out && *error_out && strstr(*error_out, "Failed to open")) {
            return VALIDATOR_ERR_NO_MANIFEST;
        }
        return VALIDATOR_ERR_MANIFEST_PARSE;
    }
    
    // Validate identity vs manifest
    if (!PluginIdentity_Validate(id, manifest, error_out)) {
        PluginIdentity_Free(id);
        PluginManifest_Free(manifest);
        return VALIDATOR_ERR_MANIFEST_VALIDATION;
    }
    
    // Success
    if (out_identity) *out_identity = id;
    else PluginIdentity_Free(id);
    
    if (out_manifest) *out_manifest = manifest;
    else PluginManifest_Free(manifest);
    
    return VALIDATOR_OK;
}

// ============================================================================
// QUICK CHECK (for scanning)
// ============================================================================

bool PluginIdentityValidator_QuickCheck(const char* plugin_root, PluginType expected_type) {
    char id_path[1024];
    snprintf(id_path, sizeof(id_path), "%s/id.json", plugin_root);
    
    // Quick file existence check
    FILE* f = fopen(id_path, "r");
    if (!f) return false;
    fclose(f);
    
    // Quick icon check
    char icon_path[1024];
    snprintf(icon_path, sizeof(icon_path), "%s/icon.png", plugin_root);
    f = fopen(icon_path, "r");
    if (!f) return false;
    fclose(f);
    
    // Quick manifest check
    const char* manifest_name = (expected_type == PLUGIN_TYPE_FPC) ? "plugin.fpc.json" : "plugin.json";
    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/%s", plugin_root, manifest_name);
    f = fopen(manifest_path, "r");
    if (!f) return false;
    fclose(f);
    
    return true;
}

// ============================================================================
// ERROR STRINGS
// ============================================================================

const char* PluginIdentityValidator_ErrorString(ValidatorError err) {
    switch (err) {
        case VALIDATOR_OK: return "OK";
        case VALIDATOR_ERR_NO_ID_JSON: return "Missing or unreadable id.json";
        case VALIDATOR_ERR_INVALID_JSON: return "id.json contains invalid JSON";
        case VALIDATOR_ERR_MISSING_REQUIRED_FIELD: return "id.json missing required field";
        case VALIDATOR_ERR_INVALID_DATE_FORMAT: return "DateCreated must be YYYY-MM-DD format";
        case VALIDATOR_ERR_VERSION_MISMATCH: return "Version mismatch between id.json and manifest";
        case VALIDATOR_ERR_TYPE_MISMATCH: return "Plugin type mismatch with directory";
        case VALIDATOR_ERR_API_VERSION_MISMATCH: return "CPluginAPI version incompatible";
        case VALIDATOR_ERR_NO_SUPPORTED_VERSIONS: return "SupportedEngineVersions must have at least one entry";
        case VALIDATOR_ERR_DUPLICATE_ID: return "Duplicate plugin ID detected";
        case VALIDATOR_ERR_NO_ICON: return "Missing icon.png";
        case VALIDATOR_ERR_ICON_NOT_PNG: return "icon.png is not a valid PNG file";
        case VALIDATOR_ERR_ICON_NOT_SQUARE: return "icon.png must be square (width == height)";
        case VALIDATOR_ERR_ICON_SIZE_VIOLATION: return "icon.png must be between 64x64 and 500x500";
        case VALIDATOR_ERR_NO_MANIFEST: return "Missing plugin.json / plugin.fpc.json";
        case VALIDATOR_ERR_MANIFEST_PARSE: return "Manifest contains invalid JSON";
        case VALIDATOR_ERR_MANIFEST_VALIDATION: return "Manifest validation failed";
        default: return "Unknown validation error";
    }
}