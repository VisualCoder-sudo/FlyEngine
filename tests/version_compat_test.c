// VersionCompat.c: SemVer parsing, prerelease ordering, ranges and API compatibility. No window, no engine.
#include "VersionCompat.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_total = 0;
#define CHECK(cond) do { ++g_total; if (!(cond)) { ++g_fail; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static int cmp(const char* a, const char* b) {
    SemVer* x = SemVer_Parse(a); SemVer* y = SemVer_Parse(b);
    int r = (x && y) ? SemVer_Compare(x, y) : 99;
    SemVer_Free(x); SemVer_Free(y);
    return r;
}

static int sat(const char* v, const char* range) {
    SemVer* x = SemVer_Parse(v);
    int r = x ? (int)SemVer_Satisfies(x, range) : -1;
    SemVer_Free(x);
    return r;
}

int main(void) {
    // Parsing
    SemVer* v = SemVer_Parse("1.2.3-alpha.1+b5");
    CHECK(v && v->major == 1 && v->minor == 2 && v->patch == 3);
    CHECK(v && v->prerelease && strcmp(v->prerelease, "alpha.1") == 0);
    CHECK(v && v->build && strcmp(v->build, "b5") == 0);
    SemVer_Free(v);
    CHECK(SemVer_Parse("1..0") == NULL);
    CHECK(SemVer_Parse("-1.0.0") == NULL);
    CHECK(SemVer_Parse("1.-1.0") == NULL);
    CHECK(SemVer_Parse(" 1.0.0") == NULL);
    CHECK(SemVer_Parse("1.0") == NULL);
    CHECK(SemVer_Parse("1.0.") == NULL);
    CHECK(SemVer_Parse("") == NULL);
    CHECK(SemVer_Parse(NULL) == NULL);

    // Ordering
    CHECK(cmp("1.0.0", "1.0.0") == 0);
    CHECK(cmp("1.2.0", "1.10.0") == -1);
    CHECK(cmp("1.0.0-alpha", "1.0.0") == -1);
    CHECK(cmp("1.0.0-alpha.9", "1.0.0-alpha.10") == -1);
    CHECK(cmp("1.0.0-alpha.10", "1.0.0-alpha.9") == 1);
    CHECK(cmp("1.0.0-alpha", "1.0.0-alpha.1") == -1);
    CHECK(cmp("1.0.0-alpha.1", "1.0.0-alpha.beta") == -1);
    CHECK(cmp("1.0.0-alpha.beta", "1.0.0-beta") == -1);
    CHECK(cmp("1.0.0-beta.2", "1.0.0-beta.11") == -1);
    CHECK(cmp("1.0.0-rc.1", "1.0.0-rc.1") == 0);

    // Ranges
    CHECK(sat("1.2.3", "*") == 1);
    CHECK(sat("2.0.0", "x") == 1);
    CHECK(sat("1.2.3", "1.x") == 1);
    CHECK(sat("2.2.3", "1.x") == 0);
    CHECK(sat("1.2.3", "1.2.x") == 1);
    CHECK(sat("1.3.3", "1.2.x") == 0);
    CHECK(sat("1.2.3", ">=1.0.0") == 1);
    CHECK(sat("1.2.3", "^1.2.0") == 1);
    CHECK(sat("2.0.0", "^1.2.0") == 0);
    CHECK(sat("1.2.9", "~1.2.3") == 1);
    CHECK(sat("1.2.3", "1.2.3") == 1);
    CHECK(sat("1.2.3", "garbage") == 0);
    {
        char* list[] = { "0.9.0", "*" };
        SemVer* e = SemVer_Parse("3.4.5");
        CHECK(e && SemVer_InSupportedList(e, list, 2));
        SemVer_Free(e);
    }

    // API compatibility: 0 compatible, 1 not
    CHECK(VersionCompat_CheckAPI("1.0.0", "1.2.0") == 0);
    CHECK(VersionCompat_CheckAPI("1.3.0", "1.2.0") == 1);
    CHECK(VersionCompat_CheckAPI("garbage", "1.2.0") == 1);
    CHECK(VersionCompat_CheckAPI("1..0", "1.2.0") == 1);
    CHECK(VersionCompat_CheckAll("garbage", "1.0.0", "1.0.0", NULL, 0) == VERSION_COMPAT_WARN_API);

    printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail == 0 ? 0 : 1;
}
