#pragma once
// Archive appended to a copy of the player executable:
//
//   [original executable][blob 0][blob 1]...[index][footer]
//
// Blobs are raw-deflate streams (or stored bytes). The index lists every file; the 64-byte footer sits at the
// very end, so detecting an archive only needs the last 64 bytes. All integers are little-endian and all offsets
// are relative to the start of the first blob, so appending to any executable works.
//
//   footer: u64 indexOffset | u64 indexSize | u64 archiveSize (blobs+index+footer) | u32 indexCrc | u32 version
//           | 24 zero bytes | "FLYPACK1"
//   index : u32 count, then per entry: u16 pathLength | path (UTF-8, '/'-separated, relative) | u64 offset
//           | u64 storedSize | u64 originalSize | u32 crc32 of the original bytes | u8 method (0 stored, 1 deflate)
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pack {

constexpr uint32_t kVersion = 1;
constexpr size_t kFooterSize = 64;
constexpr char kMagic[9] = "FLYPACK1";

struct Entry {
    std::string path;        // path inside the archive
    std::string diskPath;    // file to read it from (when `bytes` is empty and !fromBytes)
    std::vector<uint8_t> bytes;
    bool fromBytes = false;  // use `bytes` (may be empty) instead of reading diskPath
    std::string aliasOf;     // path of an EARLIER entry with identical contents: shares its stored bytes (no copy)
};

// Copies `srcExe` to `dstExe` (written through a temporary name and renamed) and appends the archive to it.
// `progress` (optional) gets (filesDone, filesTotal) and may return false to cancel. Returns false with `error` set
// on failure; nothing is left behind at dstExe in that case.
bool BuildExecutable(const std::string& srcExe, const std::string& dstExe, const std::vector<Entry>& entries,
                     std::string& error, const std::function<bool(size_t, size_t)>& progress = nullptr);

} // namespace pack
