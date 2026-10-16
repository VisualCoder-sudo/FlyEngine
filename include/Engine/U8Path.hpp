/*
 * U8Path.hpp - UTF-8 std::string -> std::filesystem::path.
 * Replaces std::filesystem::u8path, which is deprecated in C++20.
 */

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

inline std::filesystem::path U8Path(std::string_view s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
