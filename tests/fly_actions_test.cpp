// The native (C++) scripting API offers what the Visual Script nodes do: destroy, clone, names, force, raycast, keys,
// sound, screen text, time. Runs the fly.hpp wrappers against a fake engine table. Registered with ctest.
#include "fly.hpp"
#include <cstdio>
#include <cstring>

namespace fly::detail {
const FlyApi*& Api() { static const FlyApi* a = nullptr; return a; }
float& Dt() { static float d = 0; return d; }
std::vector<ClassEntry>& Registry() { static std::vector<ClassEntry> r; return r; }
}

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static std::string g_log;
static void Log(const std::string& s) { g_log += s + ";"; }

static FlyApi MakeApi(bool withActions) {
    FlyApi a{};
    a.abi_version = FLY_SCRIPT_ABI_VERSION;
    a.size = withActions ? sizeof(FlyApi) : offsetof(FlyApi, DestroyObject);   // an older engine stops before the new part
    a.DestroyObject = [](FlyHandle h) { Log("destroy" + std::to_string(h)); };
    a.CloneObject = [](FlyHandle h) -> FlyHandle { Log("clone" + std::to_string(h)); return h + 100; };
    a.GetName = [](FlyHandle) -> const char* { return "Crate"; };
    a.SetName = [](FlyHandle, const char* n) { Log(std::string("name=") + n); };
    a.AddForce = [](FlyHandle, float x, float y, float z) -> short { Log("force" + std::to_string((int)x) + std::to_string((int)y) + std::to_string((int)z)); return 1; };
    a.AddImpulse = [](FlyHandle, float x, float, float) -> short { Log("impulse" + std::to_string((int)x)); return 1; };
    a.Raycast = [](float, float, float, float, float, float, float d) -> const char* { Log("ray" + std::to_string((int)d)); return "Wall"; };
    a.StartObjectScript = [](FlyHandle h, const char* t) -> FlyHandle { Log(std::string("start:") + (t ? t : "attached")); return h; };
    a.StopObjectScript = [](FlyHandle) { Log("stop"); };
    a.KeyDown = [](const char* k) -> short { return std::strcmp(k, "W") == 0; };
    a.KeyPressed = [](const char* k) -> short { return std::strcmp(k, "Space") == 0; };
    a.PlaySound = [](const char* f, float v) { Log(std::string("sound:") + f + ":" + std::to_string((int)(v * 10))); };
    a.SetMasterVolume = [](float v) { Log("vol" + std::to_string((int)(v * 10))); };
    a.ShowText = [](const char* t, const char* p, float s) { Log(std::string("text:") + t + "@" + p + ":" + std::to_string((int)s)); };
    a.TimeSincePlay = []() -> float { return 12.5f; };
    return a;
}

int main() {
    const FlyApi full = MakeApi(true);
    fly::detail::Api() = &full;
    fly::Object o(7);
    o.AddForce({1, 2, 3});
    o.AddImpulse({4, 0, 0});
    fly::Object c = o.Clone();
    CHECK(c.Handle() == 107);
    CHECK(o.Name() == "Crate");
    o.SetName("Box");
    CHECK(o.StartScript() && o.StartScript("Bouncer"));
    o.StopScript();
    CHECK(fly::Raycast({0, 0, 0}, {0, -1, 0}, 50.0f) == "Wall");
    CHECK(fly::KeyDown("W") && !fly::KeyDown("S"));
    CHECK(fly::KeyPressed("Space") && !fly::KeyPressed("W"));
    fly::PlaySound("sfx/hit.wav", 0.5f);
    fly::SetVolume(0.8f);
    fly::ShowText("Hello", "Center", 3.0f);
    CHECK(fly::TimeSincePlay() == 12.5f);
    o.Destroy();
    CHECK(!o.IsValid());
    CHECK(g_log == "force123;impulse4;clone7;name=Box;start:attached;start:Bouncer;stop;ray50;sound:sfx/hit.wav:5;vol8;text:Hello@Center:3;destroy7;");

    // An engine from before these existed: nothing crashes, nothing happens.
    const FlyApi old = MakeApi(false);
    fly::detail::Api() = &old;
    g_log.clear();
    fly::Object p(9);
    p.AddForce({1, 1, 1}); p.Destroy(); p.SetName("x");
    CHECK(!p.Clone().IsValid() && p.Name().empty() && !p.StartScript());
    CHECK(fly::Raycast({}, {}).empty() && !fly::KeyDown("W") && fly::TimeSincePlay() == 0.0f);
    fly::PlaySound("a"); fly::SetVolume(1); fly::ShowText("t");
    CHECK(g_log.empty());

    std::printf(g_fail ? "fly_actions_test: %d FAILED\n" : "fly_actions_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
