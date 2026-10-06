/*
 * VersionCompat.h - Semantic version parsing and compatibility
 */

#ifndef VERSION_COMPAT_H
#define VERSION_COMPAT_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int major;
    int minor;
    int patch;
    char* prerelease;   // e.g., "alpha.1", "beta.2"
    char* build;        // e.g., "20240101"
} SemVer;

// Parse semantic version string (e.g., "1.2.3", "1.2.3-alpha.1+build.456")
SemVer* SemVer_Parse(const char* version_str);

// Compare two versions: -1 if a < b, 0 if equal, 1 if a > b
// Ignores prerelease/build for ordering (standard semver)
int SemVer_Compare(const SemVer* a, const SemVer* b);

// Check if version satisfies a range (e.g., ">=1.0.0", "^1.2.0", "~1.2.3", "1.x")
// Returns true if version matches range
bool SemVer_Satisfies(const SemVer* version, const char* range);

// Check if engine version is in supported array
// supported_versions is array of version strings
bool SemVer_InSupportedList(const SemVer* engine_version, char** supported_versions, int count);

// Free semver
void SemVer_Free(SemVer* v);

// ============================================================================
// ENGINE VERSION COMPATIBILITY
// ============================================================================

// Check if plugin's CPluginAPI version is compatible with engine
// Returns: 0 = compatible, -1 = too old, 1 = too new
int VersionCompat_CheckAPI(const char* plugin_api_version, const char* engine_api_version);

// Check if engine version is in plugin's SupportedEngineVersions
int VersionCompat_CheckEngine(const char* engine_version, char** supported_versions, int count);

// Combined check
typedef enum {
    VERSION_COMPAT_OK = 0,
    VERSION_COMPAT_WARN_ENGINE = 1,      // Engine version not in supported list
    VERSION_COMPAT_WARN_API = 2,         // API version mismatch
    VERSION_COMPAT_ERROR = -1,           // Parse error
} VersionCompatResult;

VersionCompatResult VersionCompat_CheckAll(
    const char* plugin_api_version,
    const char* engine_api_version,
    const char* engine_version,
    char** supported_versions,
    int supported_count
);

#endif // VERSION_COMPAT_H