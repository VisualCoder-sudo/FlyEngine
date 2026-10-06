// Known-answer test for the texture manager's SHA-256 (textures are
// de-duplicated by this hash, so a wrong hash can merge different images).
#include "Engine/Backend/TextureManager.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

static int g_failures = 0;

static void Check(const std::string& content, const char* expected, const char* label) {
    const char* path = "texture_hash_test.tmp";
    FILE* f = std::fopen(path, "wb");
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    const std::string got = textureManager::ComputeSHA256(path);
    std::remove(path);
    if (got != expected) {
        ++g_failures;
        std::printf("FAIL %s: got %s, expected %s\n", label, got.c_str(), expected);
    }
}

int main() {
    Check("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    Check("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty file");
    // 130 bytes: spans three 64-byte blocks including the padding block.
    std::string multi;
    for (int i = 0; i < 130; ++i) multi += (char)('a' + (i % 26));
    Check(multi, "06f9b1a7ac97bc8e6a835c08986fe538f0478b03826efb4eed35dc517b433b8a", "multi-block");
    // Same length, different content must not collide (the old implementation hashed only the length).
    std::string a(1000, 'x'), b(1000, 'y');
    Check(a, "44f8354494a5ba03ba1792a8d3e9c534c47a9181980fde7a3f44b06ef2ae7c7f", "1000 x");
    Check(b, "7e33ae3f1e88ddf3291109cc366b12dcd8bf8fe77bec53009f200a76e4649c07", "1000 y");
    std::printf(g_failures ? "texture_hash_test: %d FAILED\n" : "texture_hash_test: all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
