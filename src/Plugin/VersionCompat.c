/*
 * VersionCompat.c - Semantic version parsing and compatibility
 */

#include "VersionCompat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

// ============================================================================
// SEMVER PARSING
// ============================================================================

static char* strndup_safe(const char* s, size_t n) {
    char* r = malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static const char* skip_digits(const char* s) {
    while (*s && isdigit(*s)) s++;
    return s;
}

SemVer* SemVer_Parse(const char* version_str) {
    if (!version_str) return NULL;
    
    SemVer* v = calloc(1, sizeof(SemVer));
    if (!v) return NULL;
    
    const char* p = version_str;
    
    // Parse major
    v->major = strtol(p, (char**)&p, 10);
    if (p == version_str || *p != '.') goto error;
    p++;
    
    // Parse minor
    v->minor = strtol(p, (char**)&p, 10);
    if (p == version_str + 1 || *p != '.') goto error;
    p++;
    
    // Parse patch
    v->patch = strtol(p, (char**)&p, 10);
    
    // Parse prerelease
    if (*p == '-') {
        p++;
        const char* start = p;
        p = skip_digits(p);
        while (*p && (*p == '.' || isalnum(*p) || *p == '-')) p++;
        v->prerelease = strndup_safe(start, p - start);
    }
    
    // Parse build
    if (*p == '+') {
        p++;
        const char* start = p;
        while (*p && (isalnum(*p) || *p == '.' || *p == '-')) p++;
        v->build = strndup_safe(start, p - start);
    }
    
    return v;

error:
    SemVer_Free(v);
    return NULL;
}

void SemVer_Free(SemVer* v) {
    if (!v) return;
    free(v->prerelease);
    free(v->build);
    free(v);
}

int SemVer_Compare(const SemVer* a, const SemVer* b) {
    if (!a || !b) return 0;
    
    if (a->major != b->major) return (a->major > b->major) ? 1 : -1;
    if (a->minor != b->minor) return (a->minor > b->minor) ? 1 : -1;
    if (a->patch != b->patch) return (a->patch > b->patch) ? 1 : -1;
    
    // Prerelease comparison (simplified: release > prerelease)
    if (a->prerelease && !b->prerelease) return -1;
    if (!a->prerelease && b->prerelease) return 1;
    if (a->prerelease && b->prerelease) {
        return strcmp(a->prerelease, b->prerelease);
    }
    
    return 0;
}

// ============================================================================
// RANGE CHECKING
// ============================================================================

static bool check_simple_range(const SemVer* v, const char* op, const char* range_ver) {
    SemVer* rv = SemVer_Parse(range_ver);
    if (!rv) return false;
    
    int cmp = SemVer_Compare(v, rv);
    bool result = false;
    
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) result = (cmp == 0);
    else if (strcmp(op, "!=") == 0) result = (cmp != 0);
    else if (strcmp(op, ">") == 0) result = (cmp > 0);
    else if (strcmp(op, ">=") == 0) result = (cmp >= 0);
    else if (strcmp(op, "<") == 0) result = (cmp < 0);
    else if (strcmp(op, "<=") == 0) result = (cmp <= 0);
    
    SemVer_Free(rv);
    return result;
}

static bool check_caret_range(const SemVer* v, const char* range) {
    // ^1.2.3 means >=1.2.3 <2.0.0
    SemVer* rv = SemVer_Parse(range);
    if (!rv) return false;
    
    bool result = false;
    if (rv->major == 0) {
        // 0.x.y: only patch changes allowed
        if (v->major == rv->major && v->minor == rv->minor && v->patch >= rv->patch) {
            result = true;
        }
    } else {
        // >=1.2.3 <2.0.0
        if (v->major == rv->major && v->minor >= rv->minor) {
            if (v->minor == rv->minor && v->patch < rv->patch) result = false;
            else result = true;
        }
    }
    SemVer_Free(rv);
    return result;
}

static bool check_tilde_range(const SemVer* v, const char* range) {
    // ~1.2.3 means >=1.2.3 <1.3.0
    SemVer* rv = SemVer_Parse(range);
    if (!rv) return false;
    
    bool result = false;
    if (v->major == rv->major && v->minor == rv->minor && v->patch >= rv->patch) {
        result = true;
    }
    SemVer_Free(rv);
    return result;
}

static bool check_wildcard_range(const SemVer* v, const char* range) {
    // 1.x or 1.2.x
    int major = -1, minor = -1;
    sscanf(range, "%d.%d", &major, &minor);
    
    if (major >= 0 && v->major == major) {
        if (minor < 0) return true;  // 1.x
        return v->minor == minor;    // 1.2.x
    }
    return false;
}

bool SemVer_Satisfies(const SemVer* version, const char* range) {
    if (!version || !range) return false;
    
    // Handle operators
    if (strncmp(range, ">=", 2) == 0) return check_simple_range(version, ">=", range + 2);
    if (strncmp(range, "<=", 2) == 0) return check_simple_range(version, "<=", range + 2);
    if (strncmp(range, ">", 1) == 0) return check_simple_range(version, ">", range + 1);
    if (strncmp(range, "<", 1) == 0) return check_simple_range(version, "<", range + 1);
    if (strncmp(range, "==", 2) == 0) return check_simple_range(version, "==", range + 2);
    if (strncmp(range, "!=", 2) == 0) return check_simple_range(version, "!=", range + 2);
    if (strncmp(range, "=", 1) == 0) return check_simple_range(version, "=", range + 1);
    
    // Caret
    if (range[0] == '^') return check_caret_range(version, range + 1);
    
    // Tilde
    if (range[0] == '~') return check_tilde_range(version, range + 1);
    
    // Wildcard (1.x, 1.2.x)
    if (strchr(range, 'x') || strchr(range, 'X') || strchr(range, '*')) {
        return check_wildcard_range(version, range);
    }
    
    // Exact version
    return check_simple_range(version, "=", range);
}

// ============================================================================
// SUPPORTED VERSIONS LIST
// ============================================================================

bool SemVer_InSupportedList(const SemVer* engine_version, char** supported_versions, int count) {
    if (!engine_version || !supported_versions || count <= 0) return false;
    
    for (int i = 0; i < count; i++) {
        if (SemVer_Satisfies(engine_version, supported_versions[i])) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// COMPATIBILITY CHECKS
// ============================================================================

int VersionCompat_CheckAPI(const char* plugin_api_version, const char* engine_api_version) {
    if (!plugin_api_version || !engine_api_version) return 0;
    
    SemVer* pv = SemVer_Parse(plugin_api_version);
    SemVer* ev = SemVer_Parse(engine_api_version);
    
    if (!pv || !ev) {
        if (pv) SemVer_Free(pv);
        if (ev) SemVer_Free(ev);
        return 0;  // Parse error - assume compatible
    }
    
    int result = 0;
    // Plugin API must be <= Engine API (forward compatible)
    if (SemVer_Compare(pv, ev) > 0) {
        result = 1;  // Plugin requires newer API than engine has
    }
    
    SemVer_Free(pv);
    SemVer_Free(ev);
    return result;
}

int VersionCompat_CheckEngine(const char* engine_version, char** supported_versions, int count) {
    if (!engine_version || !supported_versions || count <= 0) return 0;
    
    SemVer* ev = SemVer_Parse(engine_version);
    if (!ev) return 0;
    
    bool found = SemVer_InSupportedList(ev, supported_versions, count);
    SemVer_Free(ev);
    
    return found ? 0 : 1;  // 0 = OK, 1 = Warning (not in supported list)
}

VersionCompatResult VersionCompat_CheckAll(
    const char* plugin_api_version,
    const char* engine_api_version,
    const char* engine_version,
    char** supported_versions,
    int supported_count
) {
    // Check API version
    int api_result = VersionCompat_CheckAPI(plugin_api_version, engine_api_version);
    if (api_result > 0) return VERSION_COMPAT_WARN_API;
    
    // Check engine version
    int eng_result = VersionCompat_CheckEngine(engine_version, supported_versions, supported_count);
    if (eng_result > 0) return VERSION_COMPAT_WARN_ENGINE;
    
    return VERSION_COMPAT_OK;
}