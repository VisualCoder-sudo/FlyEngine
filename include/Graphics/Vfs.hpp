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

// Files that ship beside the executable instead of inside the archive (Export > Assets folder). The archive carries
// only their names and sizes; once registered here they are served like any other file of the archive: Exists, Read and
// List see them under the same archive-relative path ("assets/Audio/boom.wav"), whatever folder they really sit in.
struct ExternalFile {
    std::string path;        // archive-relative, '/'-separated
    uint64_t size = 0;       // size it had at export; a file of another size counts as missing
};
struct ExternalReport {
    bool folderFound = false;               // does the folder exist at all?
    size_t expected = 0;
    std::vector<std::string> missing;       // archive-relative paths that are absent or the wrong size
    bool Complete() const { return missing.empty(); }
};
// `folder` holds the files with `prefix` ("assets/") removed from their archive path: "assets/Audio/a.wav" is read from
// <folder>/Audio/a.wav. Needs a mounted archive. Missing files are left out of the file system, and reported.
ExternalReport MountExternal(const std::string& folder, const std::string& prefix, const std::vector<ExternalFile>& files);

} // namespace vfs
