// ProcessRunnerInternal.hpp -- shared helpers between the Platform translation
// units. Not part of the engine's public surface.
#pragma once

#include <string>

namespace platform {
namespace internal {

// True if `name` can be found on PATH (or is an existing executable path).
bool HelperAvailable(const std::string& name);

// Quotes one argument for a Windows command line (the CommandLineToArgvW / MSVCRT rules): empty arguments and
// ones with blanks or quotes are wrapped in quotes, quotes are escaped, and backslashes before a quote (or
// before the closing quote) are doubled so they cannot escape it. Pure string code, so it is testable anywhere.
inline std::string QuoteWindowsArg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : a) {
        if (c == '\\') { ++backslashes; continue; }
        if (c == '"') out.append(backslashes * 2 + 1, '\\');
        else out.append(backslashes, '\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

// Strips a single trailing newline/CR, which every helper helper prints after
// the chosen path.
std::string TrimNewline(std::string s);

} // namespace internal
} // namespace platform
