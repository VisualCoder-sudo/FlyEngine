#pragma once
// Sound files that live in the project. Choosing a sound from outside the project copies it into assets/audio, once:
// a file with the same content (SHA256) is never stored twice, importing it again just returns the existing path.
//
// The copies follow the same lifecycle as model imports and new script files:
//   Import          the copy is pending (journaled in assets/audio/.pending) until the project is saved
//   Commit          (every save) keeps the pending ones, then deletes imported sounds nothing refers to any more
//   DiscardPending  (editor close, next launch) deletes the ones that were never saved
// Only files this module imported (listed in assets/audio/.managed) are ever deleted; anything else in the folder, such as a
// sound dropped in through the Asset Browser, is left alone.
#include <string>
#include <vector>

namespace audioassets {

constexpr const char* kFolder = "assets/audio";
constexpr const char* kLegacyFolder = "assets/Audio";   // where earlier versions put them; still found and still managed

bool IsSoundFile(const std::string& path);   // .wav .ogg .mp3 .flac

struct ImportResult {
    bool ok = false;
    std::string rel;        // project-relative, forward slashes: "assets/audio/boom.wav"
    bool reused = false;    // identical content was already in the project
    std::string error;
};

// Copies `source` into <projectDir>/assets/audio unless identical content is already there. A source that already is
// inside the project is not copied; it is returned as its project-relative path.
ImportResult Import(const std::string& projectDir, const std::string& source);

// To be called after the project file has been written. Keeps pending imports, then deletes managed sounds that no text file
// of the project (the scene file, the C++ scripts, ...) mentions by path. Returns the project-relative paths it deleted.
std::vector<std::string> Commit(const std::string& projectDir);

// Deletes imports that were never saved. Returns the project-relative paths it deleted.
std::vector<std::string> DiscardPending(const std::string& projectDir);

} // namespace audioassets
