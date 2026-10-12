// ProcessRunnerInternal.hpp -- shared helpers between the Platform translation
// units. Not part of the engine's public surface.
#pragma once

#include <string>

namespace platform {
namespace internal {

// True if `name` can be found on PATH (or is an existing executable path).
bool HelperAvailable(const std::string& name);

// Strips a single trailing newline/CR, which every helper helper prints after
// the chosen path.
std::string TrimNewline(std::string s);

} // namespace internal
} // namespace platform
