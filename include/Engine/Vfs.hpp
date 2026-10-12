#pragma once
// Read-only virtual file system over the archive appended to an exported game's executable (see
// PackArchive.hpp for the format). Inside it every project/engine file lives under the synthetic root
// "/__embedded__", so a path that starts with that prefix is served from the archive instead of the disk.
#include <cstdint>
#include <string>
#include <vector>

namespace vfs {

constexpr const char* kRoot = "/__embedded__";

// Looks for an archive footer at the end of this program's own executable and mounts it. False if there is none
// (a normal editor/player run), in which case nothing else in this header does anything.
bool MountSelfExecutable();
// Mounts the archive appended to any file (tests, tools).
bool MountFile(const std::string& path);
void Unmount();
bool Active();

// Does `path` belong to the archive (i.e. start with kRoot)?
bool Handles(const std::string& path);

// All paths below may be virtual ("/__embedded__/assets/a.png") or archive-relative ("assets/a.png").
bool Exists(const std::string& path);                              // regular file
bool IsDir(const std::string& path);
bool Read(const std::string& path, std::vector<uint8_t>& out);     // false if missing or the data is corrupt
std::vector<std::string> List(const std::string& dirPath);         // names of the immediate children

// "/__embedded__/<rel>"
std::string VirtualPath(const std::string& relative);

} // namespace vfs
