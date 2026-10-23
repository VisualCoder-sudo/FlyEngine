// Snap steps persist to the per-user config dir and load back.
#include "test_unique.hpp"
#include "../src/Engine/Frontend/ui_internal.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / TestUnique("fly_snap_test_home");
    fs::remove_all(dir);
    setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
    unsetenv("APPDATA");

    ui::LoadSnapSettings(); // no file yet: defaults stay
    CHECK(ui::g_snap.move == 0.5f);

    ui::g_snap.move = 2.0f; ui::g_snap.rotate = 45.0f; ui::g_snap.scale = 0.25f; ui::g_snap.enabled = false;
    ui::SaveSnapSettingsIfChanged();
    CHECK(fs::exists(dir / "FlyEngine" / "snap.ini"));

    ui::g_snap.move = 0.5f; ui::g_snap.rotate = 15.0f; ui::g_snap.scale = 0.1f; ui::g_snap.enabled = true;
    ui::LoadSnapSettings();
    CHECK(ui::g_snap.move == 2.0f && ui::g_snap.rotate == 45.0f && ui::g_snap.scale == 0.25f);
    CHECK(!ui::g_snap.enabled);

    fs::remove_all(dir);
    std::printf("PASS\n");
    return 0;
}
