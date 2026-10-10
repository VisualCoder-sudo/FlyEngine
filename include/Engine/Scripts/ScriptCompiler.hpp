#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ScriptCompiler - builds a project's C++ scripts (Scripts/*.cpp) into one
// shared library with the system compiler. The SDK's fly_module.cpp is
// compiled in alongside the user's files; the resulting .so/.dll is loaded
// by NativeScriptHost through the C ABI in ScriptingSDK/include/FlyScriptABI.h.
//
// Every build writes a uniquely named library under Scripts/.build/, so a
// rebuild never overwrites a library that is still loaded (Windows locks it,
// and dlopen would hand back the cached copy).
namespace scriptCompiler {

// One compiler message, parsed from clang/gcc ("file:line:col: error: msg") or
// MSVC ("file(line,col): error C1234: msg") output.
struct Diagnostic {
    enum class Severity { Note, Warning, Error };
    Severity severity = Severity::Error;
    std::string file;   // empty for messages without a location (linker errors, "no compiler")
    int line = 0;       // 1-based, 0 = unknown
    int column = 0;
    std::string message;
};

struct Result {
    bool ok = false;
    std::string libraryPath; // the new library on success
    std::string log;         // compiler output (errors and warnings)
    // Parsed from `log`. A failed build always has at least one Error entry,
    // even when the output wasn't in a recognised format.
    std::vector<Diagnostic> diagnostics;
};

// Extracts the diagnostics from raw compiler output. Pure; exposed for testing.
std::vector<Diagnostic> ParseDiagnostics(const std::string& log);

// Compiles Scripts/*.cpp for `projectFolder`. BLOCKING; the host runs it on a
// worker thread for hot reload. Does not touch any engine state.
Result Build(const std::string& projectFolder);

// True if the project has at least one Scripts/*.cpp file.
bool HasSources(const std::string& projectFolder);

// A fingerprint of every script source (names, sizes, write times). Changes
// whenever a .cpp/.hpp/.h under Scripts/ is added, removed or edited.
uint64_t SourcesStamp(const std::string& projectFolder);

// The newest library left in Scripts/.build/ by an earlier build, or "".
// Used when no compiler is available (e.g. a shipped player).
std::string LatestBuiltLibrary(const std::string& projectFolder);

// Deletes old libraries in Scripts/.build/ except `keep`. Files that are still
// locked (Windows) are skipped silently.
void RemoveStaleLibraries(const std::string& projectFolder, const std::string& keep);

// A human-readable description of the compiler that will be used, or "" if
// none was found.
std::string CompilerDescription();

// The SDK's include directory (contains fly.hpp), or "" if not found.
std::string SdkIncludeDir();

} // namespace scriptCompiler
