// The export archive: pack files onto the end of an executable, mount it, read everything back (also through
// raylib's file functions), reject damage, and read from several threads at once. No window.
#include "test_unique.hpp"
#include "raylib.h"
#include "Engine/PackArchive.hpp"
#include "Graphics/Vfs.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static std::vector<uint8_t> RandomBytes(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = (uint8_t)rng();
    return v;
}
static std::vector<uint8_t> TextBytes(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)("fly engine export test "[i % 22]);
    return v;
}

int main() {
    const fs::path dir = fs::temp_directory_path() / TestUnique("fly_vfs_pack_test");
    fs::remove_all(dir);
    fs::create_directories(dir);

    // A stand-in for the player executable.
    const fs::path fakeExe = dir / "player";
    { std::ofstream(fakeExe, std::ios::binary).write("\x7f" "ELF-not-really", 14); }

    const auto small = RandomBytes(100, 1);
    const auto compressible = TextBytes(300000);
    const auto incompressible = RandomBytes(2'000'000, 2);
    std::vector<pack::Entry> entries;
    auto add = [&](const std::string& path, const std::vector<uint8_t>& bytes) {
        pack::Entry e; e.path = path; e.bytes = bytes; e.fromBytes = true; entries.push_back(e);
    };
    add("small.bin", small);
    add("assets/text.txt", compressible);
    add("assets/3D/model/blob.dat", incompressible);
    add("assets/empty.txt", {});
    add("assets/unicode_\xc3\xa9.txt", TextBytes(10));
    add("assets/picture.png", incompressible);   // extension that is never deflated
    // A file read from disk.
    const fs::path onDisk = dir / "disk.bin";
    { std::ofstream(onDisk, std::ios::binary).write((const char*)small.data(), (std::streamsize)small.size()); }
    pack::Entry fromDisk; fromDisk.path = "from_disk.bin"; fromDisk.diskPath = onDisk.string(); entries.push_back(fromDisk);

    // A plain executable has no archive.
    CHECK(!vfs::MountFile(fakeExe.string()));
    CHECK(!vfs::Active());

    const fs::path out = dir / "game";
    std::string err;
    size_t lastDone = 0;
    CHECK(pack::BuildExecutable(fakeExe.string(), out.string(), entries, err,
                                [&](size_t done, size_t) { lastDone = done; return true; }));
    CHECK(err.empty());
    CHECK(lastDone == entries.size());
    CHECK(!fs::exists(out.string() + ".part"));
    // Compression actually happened for the text, and the random data was stored.
    CHECK(fs::file_size(out) < fs::file_size(fakeExe) + 2'000'000 + 2'000'000 + 100000);

    CHECK(vfs::MountFile(out.string()));
    CHECK(vfs::Active());
    std::vector<uint8_t> got;
    CHECK(vfs::Read("small.bin", got) && got == small);
    CHECK(vfs::Read("/__embedded__/assets/text.txt", got) && got == compressible);
    CHECK(vfs::Read("assets/3D/model/blob.dat", got) && got == incompressible);
    CHECK(vfs::Read("assets/empty.txt", got) && got.empty());
    CHECK(vfs::Read("assets/unicode_\xc3\xa9.txt", got) && got.size() == 10);
    CHECK(vfs::Read("assets/picture.png", got) && got == incompressible);
    CHECK(vfs::Read("from_disk.bin", got) && got == small);
    CHECK(!vfs::Read("missing.bin", got));
    // Odd spellings of the same path.
    CHECK(vfs::Exists("/__embedded__/assets/../small.bin"));
    CHECK(vfs::Exists("\\__embedded__\\assets\\text.txt"));
    CHECK(vfs::Exists("./assets//text.txt"));
    CHECK(!vfs::Exists("assets"));          // a directory is not a file
    CHECK(vfs::IsDir("assets") && vfs::IsDir("/__embedded__/assets/3D"));
    CHECK(!vfs::IsDir("small.bin"));
    const auto names = vfs::List("assets");
    CHECK(names.size() == 5);   // 3D, empty.txt, picture.png, text.txt, unicode
    CHECK(vfs::List("").size() == 3);   // assets, from_disk.bin, small.bin

    // Through raylib's own file functions (what every image/model loader uses).
    CHECK(FileExists("/__embedded__/assets/text.txt"));
    CHECK(!FileExists("/__embedded__/nope.txt"));
    CHECK(DirectoryExists("/__embedded__/assets"));
    int size = 0;
    unsigned char* data = LoadFileData("/__embedded__/assets/text.txt", &size);
    CHECK(data != nullptr && size == (int)compressible.size() && std::equal(compressible.begin(), compressible.end(), data));
    UnloadFileData(data);
    char* text = LoadFileText("/__embedded__/assets/unicode_\xc3\xa9.txt");
    CHECK(text != nullptr && std::string(text).size() == 10);
    UnloadFileText(text);
    // Real files still work while the VFS is mounted.
    CHECK(FileExists(onDisk.string().c_str()));

    // Eight threads reading everything over and over.
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            std::vector<uint8_t> v;
            for (int i = 0; i < 20; ++i) {
                if (!vfs::Read("assets/text.txt", v) || v != compressible) bad++;
                if (!vfs::Read("assets/3D/model/blob.dat", v) || v != incompressible) bad++;
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(bad == 0);
    vfs::Unmount();

    // A damaged blob is refused (the CRC catches it) but the rest still reads.
    {
        std::fstream f(out, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp((std::streamoff)fs::file_size(fakeExe) + 5);   // inside the first blob ("small.bin")
        f.put('\x55'); f.put('\xaa'); f.put('\x55');
    }
    CHECK(vfs::MountFile(out.string()));
    CHECK(!vfs::Read("small.bin", got));
    CHECK(vfs::Read("assets/text.txt", got) && got == compressible);
    vfs::Unmount();

    // A damaged index or footer is refused outright.
    {
        const fs::path broken = dir / "broken";
        fs::copy_file(out, broken, fs::copy_options::overwrite_existing);
        std::fstream f(broken, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(-100, std::ios::end);
        f.put('\x01');
    }
    CHECK(!vfs::MountFile((dir / "broken").string()));
    // A truncated file too.
    fs::resize_file(dir / "broken", fs::file_size(dir / "broken") - 10);
    CHECK(!vfs::MountFile((dir / "broken").string()));

    // Failure leaves nothing behind.
    pack::Entry missing; missing.path = "x"; missing.diskPath = (dir / "does_not_exist").string();
    const fs::path out2 = dir / "game2";
    CHECK(!pack::BuildExecutable(fakeExe.string(), out2.string(), { missing }, err));
    CHECK(!err.empty() && !fs::exists(out2) && !fs::exists(out2.string() + ".part"));
    // Cancelling too.
    CHECK(!pack::BuildExecutable(fakeExe.string(), out2.string(), entries, err, [](size_t, size_t) { return false; }));
    CHECK(!fs::exists(out2) && !fs::exists(out2.string() + ".part"));

    fs::remove_all(dir);
    std::printf(g_fail ? "vfs_pack_test: %d FAILED\n" : "vfs_pack_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
