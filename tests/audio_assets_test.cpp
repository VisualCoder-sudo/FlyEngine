// Imported sounds (include/Engine/Backend/AudioAssets.hpp): copied into assets/audio once, deduplicated by content,
// kept while the saved project mentions them, deleted when nothing does or when they were never saved. No window.
#include "Engine/Backend/AudioAssets.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void Write(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

// A file that passes for a sound as far as importing goes (it is never decoded here).
static std::string Fake(const std::string& tag) { return "RIFF....WAVEfmt " + tag; }

static int FilesIn(const fs::path& dir) {
    int n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file() && it->path().filename().string()[0] != '.') ++n;
    return n;
}

int main() {
    const fs::path root = fs::temp_directory_path() / "fly_audio_assets_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path proj = root / "proj", outside = root / "outside";
    fs::create_directories(proj);
    const std::string P = proj.string();
    const fs::path audio = proj / "assets" / "audio";

    Write(outside / "boom.wav", Fake("A"));
    Write(outside / "Other Name (2).WAV", Fake("A"));          // the same bytes under another name
    Write(outside / "sub" / "boom.wav", Fake("B"));            // another sound with the same name
    Write(outside / "notes.txt", "not a sound");

    CHECK(audioassets::IsSoundFile("a.OGG") && audioassets::IsSoundFile("b.mp3") && !audioassets::IsSoundFile("c.png"));
    CHECK(!audioassets::Import(P, (outside / "notes.txt").string()).ok);
    CHECK(!audioassets::Import(P, (outside / "missing.wav").string()).ok);
    CHECK(!audioassets::Import("", (outside / "boom.wav").string()).ok);

    // First import: copied, pending.
    auto a = audioassets::Import(P, (outside / "boom.wav").string());
    CHECK(a.ok && !a.reused && a.rel == "assets/audio/boom.wav");
    CHECK(fs::exists(proj / a.rel));
    CHECK(fs::exists(audio / ".pending") && fs::exists(audio / ".managed"));

    // The same file five more times, and the same bytes under another name: one copy.
    for (int i = 0; i < 5; ++i) {
        auto again = audioassets::Import(P, (outside / "boom.wav").string());
        CHECK(again.ok && again.reused && again.rel == a.rel);
    }
    auto renamed = audioassets::Import(P, (outside / "Other Name (2).WAV").string());
    CHECK(renamed.ok && renamed.reused && renamed.rel == a.rel);
    CHECK(FilesIn(audio) == 1);

    // Same name, different sound: kept apart.
    auto b = audioassets::Import(P, (outside / "sub" / "boom.wav").string());
    CHECK(b.ok && !b.reused && b.rel != a.rel && b.rel.rfind("assets/audio/boom_", 0) == 0);
    CHECK(FilesIn(audio) == 2);

    // Names are made safe to write into a scene file.
    Write(outside / "Big Bang (final).mp3", Fake("C"));
    auto c = audioassets::Import(P, (outside / "Big Bang (final).mp3").string());
    CHECK(c.ok && c.rel == "assets/audio/Big_Bang_final.mp3");

    // A sound already in the project is referred to, not copied.
    Write(proj / "assets" / "Sounds" / "mine.ogg", Fake("D"));
    auto d = audioassets::Import(P, (proj / "assets" / "Sounds" / "mine.ogg").string());
    CHECK(d.ok && d.reused && d.rel == "assets/Sounds/mine.ogg");
    CHECK(FilesIn(audio) == 3);

    // A sound the user put in the folder by hand is never managed.
    Write(audio / "handmade.wav", Fake("E"));

    // Save: the scene mentions boom.wav; a C++ script mentions Big_Bang_final.mp3; the Play copy mentions boom_<hash>.
    Write(proj / "p.flyproj", "VNODE 3 34 0 0 \"" + a.rel + "\" \"\" \"1\"\n");
    Write(proj / "Scripts" / "Boss.cpp", "fly::PlaySound(\"" + c.rel + "\");\n");
    Write(proj / "p_temp.flyproj", "VNODE 3 34 0 0 \"" + b.rel + "\" \"\" \"1\"\n");
    auto gone = audioassets::Commit(P);
    CHECK(gone.size() == 1 && gone[0] == b.rel);              // only the one nothing saved refers to
    CHECK(fs::exists(proj / a.rel) && fs::exists(proj / c.rel) && !fs::exists(proj / b.rel));
    CHECK(fs::exists(audio / "handmade.wav"));
    CHECK(!fs::exists(audio / ".pending"));

    // Edit the scene so it no longer plays boom.wav; the next save deletes it.
    Write(proj / "p.flyproj", "VNODE 3 34 0 0 \"\" \"\" \"1\"\n");
    gone = audioassets::Commit(P);
    CHECK(gone.size() == 1 && gone[0] == a.rel && !fs::exists(proj / a.rel));
    CHECK(fs::exists(proj / c.rel) && fs::exists(audio / "handmade.wav"));

    // Imported but never saved: discarded. Saved ones stay.
    auto e = audioassets::Import(P, (outside / "boom.wav").string());
    CHECK(e.ok && !e.reused && fs::exists(proj / e.rel));      // boom.wav was deleted above, so this is new again
    auto discarded = audioassets::DiscardPending(P);
    CHECK(discarded.size() == 1 && discarded[0] == e.rel && !fs::exists(proj / e.rel));
    CHECK(fs::exists(proj / c.rel) && fs::exists(audio / "handmade.wav"));
    CHECK(audioassets::DiscardPending(P).empty());

    // Importing the saved one again reuses it and leaves nothing pending.
    auto f = audioassets::Import(P, (outside / "Big Bang (final).mp3").string());
    CHECK(f.ok && f.reused && f.rel == c.rel);
    CHECK(audioassets::DiscardPending(P).empty() && fs::exists(proj / c.rel));

    // Sounds an earlier version put in assets/Audio are still found (no second copy) and still cleaned up.
    {
        const fs::path legacy = proj / "assets" / "Audio";
        Write(legacy / "old.wav", Fake("L"));
        Write(legacy / ".managed", "old.wav\n");
        Write(outside / "again.wav", Fake("L"));
        const bool sameDir = fs::equivalent(legacy, audio, ec);   // one folder on a case-insensitive file system
        auto g = audioassets::Import(P, (outside / "again.wav").string());
        CHECK(g.ok && g.reused && g.rel.size() > 14 && g.rel.substr(g.rel.size() - 7) == "old.wav");
        CHECK(sameDir || g.rel == "assets/Audio/old.wav");
        auto cleaned = audioassets::Commit(P);                 // nothing mentions old.wav
        CHECK(cleaned.size() == 1 && !fs::exists(legacy / "old.wav"));
        CHECK(fs::exists(proj / c.rel));
    }

    fs::remove_all(root, ec);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
