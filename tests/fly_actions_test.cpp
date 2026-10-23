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

    // Unions: the wrappers pass handles and modes through; an older engine ignores them.
    FlyApi un{};
    un.abi_version = FLY_SCRIPT_ABI_VERSION;
    un.size = sizeof(FlyApi);
    un.UnionObjects = [](const FlyHandle* p, int n, int mode) -> FlyHandle { Log("union:" + std::to_string(n) + ":" + std::to_string(mode) + ":" + std::to_string(p[0]) + "," + std::to_string(p[n - 1])); return 900; };
    un.SeparateUnion = [](FlyHandle h, FlyHandle* out, int maxOut) -> int { Log("separate" + std::to_string(h)); for (int i = 0; i < 3 && i < maxOut; ++i) out[i] = 50 + (FlyHandle)i; return 3; };
    un.UnionPartCount = [](FlyHandle h) -> int { return h == 900 ? 3 : 0; };
    un.SetNegate = [](FlyHandle, short on) -> short { Log("negate" + std::to_string(on)); return 1; };
    un.GetNegate = [](FlyHandle h) -> short { return h == 5; };
    un.SetUseGlobalColor = [](FlyHandle, short on) -> short { Log("global" + std::to_string(on)); return 1; };
    un.GetUseGlobalColor = [](FlyHandle h) -> short { return h == 900; };
    fly::detail::Api() = &un;
    g_log.clear();
    fly::Object cubeA(1), cubeB(2), cubeC(3);
    fly::Object made = fly::Union({ cubeA, cubeB, cubeC });
    CHECK(made.Handle() == 900 && made.IsUnion() && made.UnionPartCount() == 3 && !cubeA.IsUnion());
    CHECK(fly::Intersect({ cubeA, cubeB }).Handle() == 900);
    CHECK(!fly::Union({ cubeA }).IsValid() && !fly::Union({}).IsValid());   // fewer than two parts: not even asked
    CHECK(made.SetUseGlobalColor(true) && made.UseGlobalColor() && !cubeA.UseGlobalColor());
    fly::Object cutter(5);
    CHECK(cutter.SetNegate(true) && cutter.IsNegate() && !cubeA.IsNegate());
    const std::vector<fly::Object> back = made.Separate();
    CHECK(back.size() == 3 && back[0].Handle() == 50 && back[2].Handle() == 52 && !made.IsValid());
    CHECK(fly::Object(7).Separate().empty());   // not a union: nothing to separate
    CHECK(g_log == "union:3:0:1,3;union:2:1:1,2;global1;negate1;separate900;");
    {   // An engine from before unions: nothing is asked, nothing breaks.
        FlyApi before = un;
        before.size = offsetof(FlyApi, UnionObjects);
        fly::detail::Api() = &before;
        g_log.clear();
        CHECK(!fly::Union({ cubeA, cubeB }).IsValid() && !fly::Intersect({ cubeA, cubeB }).IsValid());
        CHECK(!made.IsUnion() && cutter.IsNegate() == false && !cutter.SetNegate(true) && !cutter.SetUseGlobalColor(true));
        CHECK(fly::Object(900).Separate().empty() && g_log.empty());
    }

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

    // Clickable UI and the mouse: kinds 4/5/6, the state bits, the free cursor. An engine without them ignores the calls.
    {
        static int s_state = 0;
        static short s_free = -1;
        FlyApi in{};
        in.size = sizeof(FlyApi);
        in.UICreate = [](const char* id, int k) -> short { Log(std::string("make:") + id + ":" + std::to_string(k)); return 1; };
        in.UISetLayout = [](const char*, int, float, float, float, float) {};
        in.UISetColor = [](const char*, unsigned char, unsigned char, unsigned char, unsigned char, unsigned char, unsigned char, unsigned char, unsigned char) {};
        in.UISetText = [](const char* id, const char* t, float size) { Log(std::string("text:") + id + ":" + t + ":" + std::to_string((int)size)); };
        in.UISetValue = [](const char* id, float v) { Log(std::string("value:") + id + ":" + std::to_string((int)(v * 100))); };
        in.UIGetState = [](const char*) -> int { return s_state; };
        in.GetMousePosition = [](float* o) { o[0] = 12; o[1] = 34; };
        in.SetMouseFree = [](short f) { s_free = f; };
        in.MouseOverUI = []() -> short { return 1; };
        fly::detail::Api() = &in;
        g_log.clear();
        fly::UI::Button("play", "Play", { fly::UI::Anchor::Center, 0, 0, 200, 50 });
        fly::UI::Slider("vol", 0.5f, {});
        fly::UI::Toggle("fs", "Fullscreen", true);
        CHECK(g_log == "make:play:4;text:play:Play:22;make:vol:5;value:vol:50;make:fs:6;text:fs:Fullscreen:20;value:fs:100;");
        s_state = 0;
        CHECK(!fly::UI::Find("play").Hovered() && !fly::UI::Find("play").Clicked());
        s_state = 1 | 4;
        CHECK(fly::UI::Find("play").Hovered() && fly::UI::Find("play").Clicked() && !fly::UI::Find("play").Held() && !fly::UI::Find("play").Changed());
        s_state = 2 | 8;
        CHECK(fly::UI::Find("play").Held() && fly::UI::Find("play").Changed());
        fly::UI::SetMouseFree(true);
        CHECK(s_free == 1);
        fly::UI::SetMouseFree(false);
        CHECK(s_free == 0);
        CHECK(fly::UI::MousePosition().x == 12.0f && fly::UI::MousePosition().y == 34.0f && fly::UI::MouseOverUI());

        FlyApi older{};
        older.size = offsetof(FlyApi, NavDebug) + sizeof(void*);   // has navigation, not the mouse
        fly::detail::Api() = &older;
        g_log.clear();
        s_free = -1;
        fly::UI::SetMouseFree(true);
        CHECK(s_free == -1 && !fly::UI::MouseOverUI() && !fly::UI::Find("play").Clicked() && fly::UI::MousePosition().x == 0.0f);
    }

    // Navigation: the wrappers pass points through the table; the agent follows what comes back; an older engine does nothing.
    {
        static int s_findCalls = 0;
        static float s_goalX = 0;
        FlyApi in{};
        in.size = sizeof(FlyApi);
        in.NavBuild = [](const float* v) -> short { Log("build:" + std::to_string((int)v[0]) + "," + std::to_string((int)v[3]) + "," + std::to_string((int)(v[6] * 10)) + "," + std::to_string((int)v[10])); return 1; };
        in.NavReady = []() -> short { return 1; };
        in.NavClear = []() { Log("clear"); };
        in.NavDebug = [](short on) { Log(on ? "debug:on" : "debug:off"); };
        in.NavFindPath = [](const float* from, const float* to, float* out, int max, short partial) -> int {
            ++s_findCalls;
            s_goalX = to[0];
            const float pts[9] = { from[0], 0, from[2], 5, 0, 0, to[0], 0, to[2] };
            const int n = max < 3 ? max : 3;
            for (int i = 0; i < n * 3; ++i) out[i] = pts[i];
            return partial ? -n : n;
        };
        in.NavNearest = [](const float* p, float* o) -> short { o[0] = p[0]; o[1] = 0; o[2] = p[2]; return p[0] < 100; };
        in.NavIsWalkable = [](const float* p) -> short { return p[1] < 1; };
        in.NavRandomPoint = [](const float* c, float r, float* o) -> short { o[0] = c[0] + r; o[1] = 0; o[2] = c[2]; return r > 0; };
        in.NavLineClear = [](const float*, const float* b) -> short { return b[0] < 100; };
        in.NavMoveAlong = [](const float* p, const float* d, float* o) -> short { o[0] = p[0] + d[0]; o[1] = 0; o[2] = p[2] + d[2]; return 1; };
        fly::detail::Api() = &in;
        g_log.clear();

        fly::Nav::Settings st;
        st.min = { -9, -1, -9 }; st.max = { 9, 5, 9 }; st.cellSize = 0.25f; st.maxSlope = 30.0f;
        CHECK(fly::Nav::Available() && fly::Nav::Build(st) && fly::Nav::Ready());
        fly::Nav::ShowDebug();
        fly::Nav::ShowDebug(false);
        fly::Nav::Clear();
        CHECK(g_log == "build:-9,9,2,30;debug:on;debug:off;clear;");

        const fly::Nav::Path path = fly::Nav::FindPath({ 0, 0, 0 }, { 10, 0, 10 });
        CHECK(path.Valid() && path.points.size() == 3 && !path.partial && path.points[1].x == 5.0f);
        CHECK(fly::Nav::FindPath({ 0, 0, 0 }, { 10, 0, 10 }, true).partial);
        CHECK(fly::Nav::FindPath({ 0, 0, 0 }, { 10, 0, 10 }, false, 2).points.size() == 2);
        CHECK(std::fabs(path.Length() - (5.0f + std::sqrt(25.0f + 100.0f))) < 1e-3f);
        fly::Vec3 v;
        CHECK(fly::Nav::Nearest({ 1, 2, 3 }, v) && v.y == 0.0f && !fly::Nav::Nearest({ 500, 0, 0 }, v));
        CHECK(fly::Nav::IsWalkable({ 0, 0, 0 }) && !fly::Nav::IsWalkable({ 0, 5, 0 }));
        CHECK(fly::Nav::RandomPoint({ 1, 0, 1 }, 4, v) && v.x == 5.0f && !fly::Nav::RandomPoint({ 1, 0, 1 }, 0, v));
        CHECK(fly::Nav::CanMove() && fly::Nav::LineClear({ 0, 0, 0 }, { 5, 0, 0 }) && !fly::Nav::LineClear({ 0, 0, 0 }, { 500, 0, 0 }));
        CHECK(fly::Nav::MoveAlong({ 1, 2, 3 }, { 1, 0, 1 }, v) && v.x == 2.0f && v.y == 0.0f && v.z == 4.0f);

        // The agent heads for the first corner after its own position, flat and at its speed, and does not ask for a path
        // every frame while the target barely moves.
        fly::Nav::Agent agent(2.0f, 1.0f, 0.3f);
        CHECK(agent.Step({ 0, 0, 0 }, 0.1f).x == 0.0f && !agent.HasTarget());
        s_findCalls = 0;
        agent.SetTarget({ 10, 0, 10 });
        fly::Vec3 vel = agent.Step({ 0, 0, 0 }, 0.1f);
        CHECK(s_findCalls == 1 && std::fabs(vel.x - 2.0f) < 1e-4f && vel.y == 0.0f && std::fabs(vel.z) < 1e-4f);   // towards (5,0,0)
        agent.SetTarget({ 10.2f, 0, 10 });
        agent.Step({ 1, 0, 0 }, 0.1f);
        CHECK(s_findCalls == 1);
        agent.SetTarget({ 20, 0, 10 });
        agent.Step({ 1, 0, 0 }, 0.1f);
        CHECK(s_findCalls == 2 && s_goalX == 20.0f);
        vel = agent.Step({ 5, 0, 0 }, 0.1f);                    // reached the corner, heads for the end
        CHECK(vel.x > 0.0f && vel.z > 0.0f && !agent.Arrived());
        vel = agent.Step({ 20, 0, 10 }, 0.1f);
        CHECK(agent.Arrived() && vel.x == 0.0f && vel.z == 0.0f);
        agent.SetTarget({ 20, 0, 40 });
        CHECK(!agent.Arrived());
        agent.ClearTarget();
        CHECK(!agent.HasTarget() && agent.Step({ 0, 0, 0 }, 0.1f).x == 0.0f);

        FlyApi old{};
        old.size = offsetof(FlyApi, DebugText) + sizeof(void*);
        fly::detail::Api() = &old;
        g_log.clear();
        CHECK(!fly::Nav::Available() && !fly::Nav::Build({ 0, 0, 0 }, { 1, 1, 1 }) && !fly::Nav::Ready() && !fly::Nav::FindPath({}, {}).Valid());
        CHECK(!fly::Nav::Nearest({}, v) && !fly::Nav::IsWalkable({}) && !fly::Nav::RandomPoint({}, 1, v));
        CHECK(!fly::Nav::CanMove() && !fly::Nav::MoveAlong({ 1, 2, 3 }, { 1, 0, 0 }, v) && v.x == 2.0f && v.y == 2.0f);
        fly::Nav::ShowDebug(); fly::Nav::Clear();
        fly::Nav::Agent idle;
        idle.SetTarget({ 1, 0, 1 });
        CHECK(idle.Step({}, 0.1f).x == 0.0f && g_log.empty());
    }

    std::printf(g_fail ? "fly_actions_test: %d FAILED\n" : "fly_actions_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
