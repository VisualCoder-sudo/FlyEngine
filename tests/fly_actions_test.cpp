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

    // In-game UI and prefabs: calls reach the engine table with the right arguments; an older engine ignores them.
    FlyApi ui{};
    ui.abi_version = FLY_SCRIPT_ABI_VERSION;
    ui.size = sizeof(FlyApi);
    ui.SpawnPrefab = [](const char* f, float x, float, float z) -> FlyHandle { Log(std::string("prefab:") + f + ":" + std::to_string((int)x) + "," + std::to_string((int)z)); return 55; };
    ui.UICreate = [](const char* id, int k) -> short { Log(std::string("make:") + id + ":" + std::to_string(k)); return 1; };
    ui.UISetLayout = [](const char* id, int a, float x, float y, float w, float h) { Log(std::string("layout:") + id + ":" + std::to_string(a) + ":" + std::to_string((int)x) + "," + std::to_string((int)y) + "," + std::to_string((int)w) + "," + std::to_string((int)h)); };
    ui.UISetColor = [](const char* id, unsigned char r, unsigned char, unsigned char, unsigned char, unsigned char r2, unsigned char, unsigned char, unsigned char) { Log(std::string("color:") + id + ":" + std::to_string(r) + "," + std::to_string(r2)); };
    ui.UISetText = [](const char* id, const char* t, float size) { Log(std::string("text:") + id + ":" + t + ":" + std::to_string((int)size)); };
    ui.UISetValue = [](const char* id, float v) { Log(std::string("value:") + id + ":" + std::to_string((int)(v * 100))); };
    ui.UISetImage = [](const char* id, const char* f) { Log(std::string("image:") + id + ":" + f); };
    ui.UISetVisible = [](const char* id, short v) { Log(std::string("show:") + id + ":" + std::to_string(v)); };
    ui.UIRemove = [](const char* id) { Log(std::string("remove:") + id); };
    ui.UISetFont = [](const char* id, const char* f) { Log(std::string("font:") + id + ":" + f); };
    fly::detail::Api() = &ui;
    g_log.clear();
    CHECK(fly::Prefab::Spawn("assets/Prefabs/Tree.flyprefab", {3, 0, 4}).Handle() == 55);
    fly::UI::Text("score", "Score: 0", { fly::UI::Anchor::TopRight, -20, 20 }, { 255, 0, 0 }, 30, "Serif");
    fly::UI::Bar("hp", 0.5f, { fly::UI::Anchor::BottomLeft, 20, -20, 240, 22 });
    fly::UI::Panel("bg", { fly::UI::Anchor::Fill, 8, 8 });
    fly::UI::Image("icon", "ui/heart.png", { fly::UI::Anchor::Center });
    fly::UI::SetText("score", "Score: 5");
    fly::UI::SetFont("score", "Mono");
    fly::UI::SetValue("hp", 0.25f);
    fly::UI::Hide("bg");
    fly::UI::Remove("icon");
    CHECK(g_log == "prefab:assets/Prefabs/Tree.flyprefab:3,4;"
                   "make:score:1;layout:score:2:-20,20,0,0;color:score:255,255;text:score:Score: 0:30;font:score:Serif;"
                   "make:hp:2;layout:hp:6:20,-20,240,22;color:hp:80,0;value:hp:50;"
                   "make:bg:0;layout:bg:9:8,8,0,0;color:bg:0,0;"
                   "make:icon:3;layout:icon:4:0,0,0,0;color:icon:255,255;image:icon:ui/heart.png;"
                   "text:score:Score: 5:0;font:score:Mono;value:hp:25;show:bg:0;remove:icon;");
    // Widgets read back and change one part at a time.
    ui.UIExists = [](const char* id) -> short { return std::string(id) == "hp"; };
    ui.UIGetValue = [](const char*) -> float { return 0.75f; };
    ui.UIGetText = [](const char*, char* out, int cap) -> int { if (out && cap > 0) std::snprintf(out, (size_t)cap, "%s", "Hello"); return 5; };
    ui.UIGetVisible = [](const char*) -> short { return 1; };
    ui.UIGetLayout = [](const char*, float* v) -> short { v[0] = 6; v[1] = 20; v[2] = -20; v[3] = 240; v[4] = 22; v[5] = 28; return 1; };
    ui.UIGetColor = [](const char*, unsigned char* v) -> short { const unsigned char c[8] = { 80, 200, 90, 255, 0, 0, 0, 160 }; std::memcpy(v, c, 8); return 1; };
    g_log.clear();
    fly::UI::Widget hp = fly::UI::Find("hp");
    CHECK(hp.Exists() && !fly::UI::Find("nope").Exists());
    CHECK(hp.Text() == "Hello" && hp.Value() == 0.75f && hp.Visible() && hp.FontSize() == 28.0f);
    CHECK(hp.GetLayout().anchor == fly::UI::Anchor::BottomLeft && hp.GetLayout().w == 240.0f);
    CHECK(hp.Color().g == 200 && hp.Background().a == 160);
    hp.SetOffset(5, 6).SetColor({ 1, 2, 3 }).SetFontSize(40);
    CHECK(g_log == "layout:hp:6:5,6,240,22;color:hp:1,0;text:hp:Hello:40;");
    g_log.clear();
    CHECK(fly::UI::Text("t", "x").Id() == "t");
    fly::detail::Api() = &old;
    g_log.clear();
    CHECK(!fly::UI::Find("hp").Exists() && fly::UI::Find("hp").Text().empty() && fly::UI::Find("hp").Value() == 0.0f);
    fly::UI::Find("hp").SetOffset(1, 2).SetColor({ 1, 2, 3 }).SetFontSize(9);
    CHECK(g_log.empty());
    fly::detail::Api() = &old;
    g_log.clear();
    CHECK(!fly::Prefab::Spawn("x", {}).IsValid());
    fly::UI::Text("a", "b"); fly::UI::Bar("c", 1, {}); fly::UI::SetText("a", "c"); fly::UI::Remove("a");
    CHECK(g_log.empty());

    std::printf(g_fail ? "fly_actions_test: %d FAILED\n" : "fly_actions_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
