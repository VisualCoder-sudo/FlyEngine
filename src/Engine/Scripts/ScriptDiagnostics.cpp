// ScriptDiagnostics.cpp - turns raw compiler output into Diagnostic entries.
// Kept apart from ScriptCompiler.cpp (which needs the platform layer) so it can
// be unit-tested on its own; see tests/script_diagnostics_test.cpp.

#include "../../../include/Engine/Scripts/ScriptCompiler.hpp"

#include <cstdlib>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace scriptCompiler {

std::vector<Diagnostic> ParseDiagnostics(const std::string& log) {
    // file:line[:col]: severity: message          (clang, gcc, MinGW)
    static const std::regex gnu(
        R"(^(.+?):(\d+):(?:(\d+):)?\s*(fatal error|error|warning|note):\s*(.*)$)");
    // file(line[,col]) : severity [Cnnnn]: message  (MSVC)
    static const std::regex msvc(
        R"(^(.+?)\((\d+)(?:,(\d+))?\)\s*:\s*(fatal error|error|warning|note)\s*(?:[A-Za-z]+\d+)?\s*:\s*(.*)$)");
    // No location: "clang++: error: ...", "ld: ...", "/usr/bin/ld: ... undefined reference"
    static const std::regex bare(R"(^(?:[^\s:]+:\s*)?(fatal error|error):\s*(.*)$)");
    static const std::regex linker(R"(undefined reference|undefined symbol|multiple definition|cannot find -l)");

    constexpr size_t kMaxDiagnostics = 200;
    auto severityOf = [](const std::string& s) {
        if (s == "warning") return Diagnostic::Severity::Warning;
        if (s == "note") return Diagnostic::Severity::Note;
        return Diagnostic::Severity::Error;
    };

    std::vector<Diagnostic> out;
    std::istringstream in(log);
    std::string line;
    while (std::getline(in, line) && out.size() < kMaxDiagnostics) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        std::smatch m;
        Diagnostic d;
        if (std::regex_match(line, m, gnu) || std::regex_match(line, m, msvc)) {
            d.file = m[1].str();
            d.line = std::atoi(m[2].str().c_str());
            d.column = m[3].matched ? std::atoi(m[3].str().c_str()) : 0;
            d.severity = severityOf(m[4].str());
            d.message = m[5].str();
        } else if (std::regex_match(line, m, bare)) {
            d.severity = Diagnostic::Severity::Error;
            d.message = m[2].str();
        } else if (std::regex_search(line, linker)) {
            d.severity = Diagnostic::Severity::Error;
            d.message = line;
        } else {
            continue;
        }
        // "N errors generated." summaries and "In file included from" are noise.
        out.push_back(std::move(d));
    }
    return out;
}

} // namespace scriptCompiler
