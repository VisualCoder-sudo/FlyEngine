// A city that takes long to build shows "City attempting to load [n%]" with a progress bar. The delay before the
// message is lowered to half a second here (it is 5 seconds normally). Checks the message appears, its percentage only
// goes up, it reaches the end and disappears when the city is ready, and renders pictures of it
// (city_loading_<n>.png). Opens a window, so it is not registered with ctest: build/tests/city_loading_test
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/Graphics.hpp"
#include "../include/CityGen/City.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

using namespace city;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static int PercentOf(const std::string& text) {
    const size_t a = text.find('[');
    return a == std::string::npos ? -1 : atoi(text.c_str() + a + 1);
}

int main() {
    Engine engine(900, 600, "city_loading_test", 1000);
    engine.SetPlayerBuild(true);
    auto owner = std::make_unique<City>();
    City& c = *owner;
    engine.AddEntity(std::move(owner));
    c.SetLoadingMessageDelay(0.5f);
    c.GetParams().gridX = 110; c.GetParams().gridZ = 110; c.GetParams().cars = 0;
    c.GenerateGrid({ 0.0f, 0.0f });
    CHECK(c.IsRebuilding());

    int last = -1, shown = 0, shots = 0, frames = 0;
    bool decreased = false;
    while (c.IsRebuilding() && frames < 100000) {
        engine.StepFrame(1.0f / 60.0f);
        frames++;
        if (gfx::LoadingStatusCount() > 0) {
            const std::string t = gfx::LoadingStatusText(0);
            const int pct = PercentOf(t);
            if (pct < last) decreased = true;
            if (shown == 0) std::printf("first message: \"%s\"\n", t.c_str());
            last = pct;
            shown++;
            if (shots < 2 && pct >= 20 + shots * 40) {
                TakeScreenshot(TextFormat("city_loading_%d.png", shots));
                std::printf("message at the picture: \"%s\"\n", t.c_str());
                shots++;
            }
        }
    }
    std::printf("message shown on %d frames, last percentage %d, %d frames in total\n", shown, last, frames);
    CHECK(shown > 5);
    CHECK(!decreased);
    CHECK(last >= 50 && last <= 99);
    for (int f = 0; f < 3; f++) engine.StepFrame(1.0f / 60.0f);
    CHECK(!c.IsRebuilding());
    CHECK(gfx::LoadingStatusCount() == 0);                       // gone once the city is ready
    CHECK(c.RebuildProgress() == 1.0f);

    std::printf(g_fail ? "city_loading_test: %d FAILED\n" : "city_loading_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
