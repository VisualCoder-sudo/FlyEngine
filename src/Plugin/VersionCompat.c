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

// One numeric field: must start with a digit (strtol alone would accept " 1" and "-1").
static bool parse_num(const char** pp, int* out) {
    if (!isdigit((unsigned char)**pp)) return false;
    *out = (int)strtol(*pp, (char**)pp, 10);
    return true;
}

SemVer* SemVer_Parse(const char* version_str) {
    if (!version_str) return NULL;
    
    SemVer* v = calloc(1, sizeof(SemVer));
    if (!v) return NULL;
    
    const char* p = version_str;
    
    // Parse major
    if (!parse_num(&p, &v->major) || *p != '.') goto error;
    p++;
    
    // Parse minor
    if (!parse_num(&p, &v->minor) || *p != '.') goto error;
    p++;
    
    // Parse patch
    if (!parse_num(&p, &v->patch)) goto error;
    
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

// SemVer precedence of two prerelease strings: dot-separated identifiers left to right, numeric ones by value and
// below alphanumeric ones, alphanumeric ones in ASCII order; if all shared identifiers are equal, more fields win.
static int compare_prerelease(const char* a, const char* b) {
    while (*a || *b) {
        if (!*a) return -1;
        if (!*b) return 1;
        size_t la = strcspn(a, "."), lb = strcspn(b, ".");
        bool na = true, nb = true;
        for (size_t i = 0; i < la; i++) if (!isdigit((unsigned char)a[i])) na = false;
        for (size_t i = 0; i < lb; i++) if (!isdigit((unsigned char)b[i])) nb = false;
        int c = 0;
        if (na && nb) {
            const char* ta = a; size_t xa = la;
            const char* tb = b; size_t xb = lb;
            while (xa > 1 && *ta == '0') { ta++; xa--; }
            while (xb > 1 && *tb == '0') { tb++; xb--; }
            if (xa != xb) c = xa < xb ? -1 : 1;
            else c = memcmp(ta, tb, xa);
        } else if (na != nb) {
            c = na ? -1 : 1;
        } else {
            size_t m = la < lb ? la : lb;
            c = memcmp(a, b, m);
            if (c == 0 && la != lb) c = la < lb ? -1 : 1;
        }
        if (c != 0) return c < 0 ? -1 : 1;
        a += la; b += lb;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
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
        return compare_prerelease(a->prerelease, b->prerelease);
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
    // "*", "x" (anything), 1.x or 1.2.x
    if (strcmp(range, "*") == 0 || strcmp(range, "x") == 0 || strcmp(range, "X") == 0) return true;
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
        return 1;  // Parse error - cannot vouch for compatibility, so report incompatible
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