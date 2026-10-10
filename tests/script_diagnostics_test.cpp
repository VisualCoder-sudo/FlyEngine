// Unit tests for scriptCompiler::ParseDiagnostics: clang/gcc, MinGW, MSVC and
// linker output. No window or engine needed.
#include "Engine/Scripts/ScriptCompiler.hpp"

#include <cstdio>
#include <string>

using scriptCompiler::Diagnostic;
using scriptCompiler::ParseDiagnostics;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
    // clang
    {
        auto d = ParseDiagnostics(
            "/p/Scripts/Foo.cpp:12:5: error: use of undeclared identifier 'x'\n"
            "   12 |     x = 1;\n"
            "      |     ^\n"
            "/p/Scripts/Foo.cpp:20:9: warning: unused variable 'y' [-Wunused-variable]\n"
            "/p/Scripts/Foo.cpp:3:1: note: declared here\n"
            "2 errors generated.\n");
        CHECK(d.size() == 3);
        CHECK(d[0].severity == Diagnostic::Severity::Error);
        CHECK(d[0].file == "/p/Scripts/Foo.cpp" && d[0].line == 12 && d[0].column == 5);
        CHECK(d[0].message == "use of undeclared identifier 'x'");
        CHECK(d[1].severity == Diagnostic::Severity::Warning && d[1].line == 20);
        CHECK(d[2].severity == Diagnostic::Severity::Note);
    }
    // gcc, without a column, and fatal errors
    {
        auto d = ParseDiagnostics(
            "In file included from a.cpp:1:\n"
            "Foo.cpp:7: error: expected ';'\n"
            "Foo.cpp:1:10: fatal error: nope.hpp: No such file or directory\n");
        CHECK(d.size() == 2);
        CHECK(d[0].line == 7 && d[0].column == 0 && d[0].file == "Foo.cpp");
        CHECK(d[1].severity == Diagnostic::Severity::Error && d[1].column == 10);
    }
    // MSVC, with and without a column
    {
        auto d = ParseDiagnostics(
            "C:\\proj\\Scripts\\Foo.cpp(12,5): error C2065: 'x': undeclared identifier\n"
            "C:\\proj\\Scripts\\Foo.cpp(30) : warning C4100: 'p': unreferenced parameter\n");
        CHECK(d.size() == 2);
        CHECK(d[0].file == "C:\\proj\\Scripts\\Foo.cpp" && d[0].line == 12 && d[0].column == 5);
        CHECK(d[0].message == "'x': undeclared identifier");
        CHECK(d[1].severity == Diagnostic::Severity::Warning && d[1].line == 30 && d[1].column == 0);
    }
    // Windows drive letters must not be mistaken for line numbers (MinGW/clang)
    {
        auto d = ParseDiagnostics("C:/proj/Scripts/Foo.cpp:4:2: error: boom\n");
        CHECK(d.size() == 1);
        CHECK(d[0].file == "C:/proj/Scripts/Foo.cpp" && d[0].line == 4 && d[0].column == 2);
    }
    // Messages with no location
    {
        auto d = ParseDiagnostics(
            "/usr/bin/ld: x.o: undefined reference to `foo()'\n"
            "clang++: error: linker command failed with exit code 1\n");
        CHECK(d.size() == 2);
        CHECK(d[0].file.empty() && d[0].severity == Diagnostic::Severity::Error);
        CHECK(d[1].file.empty() && d[1].message == "linker command failed with exit code 1");
    }
    // Nothing to report
    CHECK(ParseDiagnostics("").empty());
    CHECK(ParseDiagnostics("compiling...\nall good\n").empty());
    // CRLF line endings
    {
        auto d = ParseDiagnostics("Foo.cpp:1:1: error: bad\r\n");
        CHECK(d.size() == 1 && d[0].message == "bad");
    }

    if (g_fail == 0) std::printf("script_diagnostics_test: all passed\n");
    return g_fail ? 1 : 0;
}
