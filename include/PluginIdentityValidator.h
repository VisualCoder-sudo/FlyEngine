/*
 * PluginIdentityValidator.h - Hard-fail validation for plugin identity
 * 
 * This module validates id.json and icon.png with hard-fail semantics:
 * - Missing/corrupt/invalid = plugin silently rejected (no dialog, no warning)
 * - Returns detailed error for logging/debugging only
 */

#ifndef PLUGIN_IDENTITY_VALIDATOR_H
#define PLUGIN_IDENTITY_VALIDATOR_H

#include "PluginManifest.h"
#include <stdbool.h>

typedef enum {
    VALIDATOR_OK = 0,
    VALIDATOR_ERR_NO_ID_JSON,
    VALIDATOR_ERR_INVALID_JSON,
    VALIDATOR_ERR_MISSING_REQUIRED_FIELD,
    VALIDATOR_ERR_INVALID_DATE_FORMAT,
    VALIDATOR_ERR_VERSION_MISMATCH,
    VALIDATOR_ERR_TYPE_MISMATCH,
    VALIDATOR_ERR_API_VERSION_MISMATCH,
    VALIDATOR_ERR_NO_SUPPORTED_VERSIONS,
    VALIDATOR_ERR_DUPLICATE_ID,
    VALIDATOR_ERR_NO_ICON,
    VALIDATOR_ERR_ICON_NOT_PNG,
    VALIDATOR_ERR_ICON_NOT_SQUARE,
    VALIDATOR_ERR_ICON_SIZE_VIOLATION,
    VALIDATOR_ERR_NO_MANIFEST,
    VALIDATOR_ERR_MANIFEST_PARSE,
    VALIDATOR_ERR_MANIFEST_VALIDATION,
} ValidatorError;

// Validate plugin identity (id.json + icon.png + manifest)
// Returns VALIDATOR_OK on success, error code on failure
// If error_out is provided, sets a human-readable error message (caller must free)
ValidatorError PluginIdentityValidator_Validate(
    const char* plugin_root,
    PluginType expected_type,
    PluginIdentity** out_identity,
    PluginManifest** out_manifest,
    char** error_out
);

// Quick check if plugin directory has valid identity (for scanning)
// Returns true if plugin should be loaded, false if rejected
bool PluginIdentityValidator_QuickCheck(const char* plugin_root, PluginType expected_type);

// Get error string for validator error code
const char* PluginIdentityValidator_ErrorString(ValidatorError err);

#endif // PLUGIN_IDENTITY_VALIDATOR_H