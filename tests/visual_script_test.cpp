// Tests for the Visual Script graph runtime (include/Engine/Backend/VisualScript.hpp,
// src/Engine/Backend/VisualScript.cpp): execution, value wires, save/load and the node rules.
//
// Like math_backend_parity_test this does NOT link FlyEngineCore. VisualScript.cpp is compiled
// directly, and everything it reaches (ScriptRuntime, the console, ui::Log, the few raylib audio and
// text calls) is replaced by the recording stubs below, so the test needs no window, GL or audio device.
// Run: ctest -R visual_script  (requires FLYENGINE_BUILD_TESTS=ON)

#include "Engine/Backend/VisualScript.hpp"
#include "Engine/Backend/ScatteredObject.hpp"
#include "Engine/Scripts/CommandConsole.hpp"
#include "Engine/Scripts/ScriptLauncher.hpp"
#include "Engine/Scripts/ScriptRuntime.hpp"
#include "Engine/Frontend/ui.hpp"

#include <cstdarg>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <process.h>
#define vs_getpid _getpid
#else
#include <unistd.h>
#define vs_getpid getpid
#endif
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------- recording stubs

namespace {
std::vector<std::string> g_log;                 // ui::Log lines
std::vector<std::string> g_console;             // console::RunLine lines
struct SetCall { std::string object, prop, value; };
std::vector<SetCall> g_sets;                    // ScriptRuntime::SetObjectProperty
std::vector<std::string> g_forces;              // "force|impulse <vec>"
std::vector<std::string> g_created;             // CreateObject shapes
int g_destroyed = 0, g_cloned = 0, g_scriptsStarted = 0;
std::map<const void*, std::string> g_names;     // SetName on the fake objects
std::string g_rayHit;                           // what RayCastName returns
std::vector<std::string> g_rays;                // "origin|dir|dist"
std::vector<std::string> g_snippetsStarted;     // class names handed to StartSnippet
bool g_snippetsKnown = true;
std::map<std::string, std::vector<std::string>> g_snippetInputs;    // what the node handed the snippet
std::map<std::string, std::vector<std::string>> g_snippetOutputs;   // what the "snippet" set (tests seed these)

struct alignas(16) FakeObject { char c; };
FakeObject g_objects[3];                        // fake objects: only their addresses matter
const char* ObjectName(const void* p) { return p == &g_objects[0] ? "Cube" : p == &g_objects[1] ? "Ball" : "?"; }

// raylib stand-ins
int g_keyPressed = -1;
float g_masterVolume = -1.0f;
std::vector<std::string> g_played;              // "path|volume" from the sound player hook
std::vector<std::string> g_drawn;               // DrawText strings that are not the shadow pass
int g_drawTextCalls = 0;

void ResetStubs() {
    g_log.clear(); g_console.clear(); g_sets.clear(); g_forces.clear(); g_created.clear(); g_rays.clear(); g_drawn.clear();
    g_destroyed = g_cloned = g_scriptsStarted = 0;
    g_snippetsStarted.clear(); g_snippetsKnown = true; g_snippetInputs.clear(); g_snippetOutputs.clear();
    g_names.clear(); g_rayHit.clear();
    g_keyPressed = -1; g_masterVolume = -1.0f; g_drawTextCalls = 0; g_played.clear();
    vscript::SetSoundPlayer(nullptr);
}
}

namespace ui {
void Log(const char* fmt, ...) {
    char b[512];
    va_list ap; va_start(ap, fmt); std::vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    g_log.push_back(b);
}
}
namespace console {
bool RunLine(const std::string& line, std::string*) { g_console.push_back(line); return true; }
}
namespace scriptLauncher {
std::string MakeClassName(const std::string& name) { return name; }
}

ScriptRuntime::ScriptRuntime() {}
ScriptRuntime::~ScriptRuntime() {}
void ScriptRuntime::Update(float) {}
ScatteredObject* ScriptRuntime::FindByNameNth(const std::string& name, int occurrence) const {
    if (occurrence != 1) return nullptr;
    if (name == "Cube") return reinterpret_cast<ScatteredObject*>(&g_objects[0]);
    if (name == "Ball") return reinterpret_cast<ScatteredObject*>(&g_objects[1]);
    return nullptr;
}
bool ScriptRuntime::SetObjectProperty(ScatteredObject* t, const std::string& prop, const std::string& rhs, std::string&) {
    g_sets.push_back({ ObjectName(t), prop, rhs });
    return true;
}
bool ScriptRuntime::GetObjectProperty(ScatteredObject*, const std::string& prop, std::string& out) {
    if (prop == "Position") { out = "(1, 2, 3)"; return true; }
    if (prop == "Mass") { out = "4"; return true; }
    return false;
}
ScatteredObject* ScriptRuntime::CreateObject(const std::string& shape) {
    g_created.push_back(shape);
    return reinterpret_cast<ScatteredObject*>(&g_objects[2]);
}
void ScriptRuntime::DestroyObject(ScatteredObject*) { ++g_destroyed; }
ScatteredObject* ScriptRuntime::CloneObject(ScatteredObject*) { ++g_cloned; return reinterpret_cast<ScatteredObject*>(&g_objects[1]); }
bool ScriptRuntime::AddForce(ScatteredObject*, const std::string& vec, bool impulse, std::string&) {
    g_forces.push_back(std::string(impulse ? "impulse " : "force ") + vec);
    return true;
}
std::string ScriptRuntime::RayCastName(const std::string& o, const std::string& d, float dist, std::string&) {
    char b[32]; std::snprintf(b, sizeof(b), "%g", dist);
    g_rays.push_back(o + "|" + d + "|" + b);
    return g_rayHit;
}
void ScriptRuntime::StartObjectScript(ScatteredObject*) { ++g_scriptsStarted; }
void ScriptRuntime::SetSnippetInputs(const std::string& cls, std::vector<std::string> v) { g_snippetInputs[cls] = std::move(v); }
std::string ScriptRuntime::SnippetOutput(const std::string& cls, int pin) const {
    const auto it = g_snippetOutputs.find(cls);
    return it != g_snippetOutputs.end() && pin >= 0 && pin < static_cast<int>(it->second.size()) ? it->second[pin] : std::string();
}
bool ScriptRuntime::StartSnippet(const std::string& cls) {
    g_snippetsStarted.push_back(cls);
    return g_snippetsKnown;                          // false = "not compiled yet"
}
void ScatteredObject::SetName(const std::string& n) { g_names[this] = n; }

extern "C" {
bool IsKeyPressed(int key) { return key == g_keyPressed; }
void SetMasterVolume(float v) { g_masterVolume = v; }
int MeasureText(const char* text, int) { return static_cast<int>(std::strlen(text)) * 10; }
void DrawText(const char* text, int, int, int, Color c) {
    ++g_drawTextCalls;
    if (c.a == 255) g_drawn.push_back(text);   // the shadow pass is translucent
}
}

// ---------------------------------------------------------------- tiny harness

static int g_failures = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; std::printf("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { ++g_checks; const std::string _a = (a), _b = (b); if (_a != _b) { ++g_failures; \
    std::printf("  FAIL line %d: %s == \"%s\", wanted \"%s\"\n", __LINE__, #a, _a.c_str(), _b.c_str()); } } while (0)

using namespace vscript;

// Fresh scene with one empty script.
static void Fresh() { Clear(); ResetStubs(); NewScript("T"); }
static int Add(NodeType t) { return AddNode(t, 0, 0).id; }
static Node& N(int id) { return *Find(id); }
static void Link_(int from, int to, int pin = 0) { Connect(from, to, pin); }
static bool Logged(const std::string& text) {
    for (const auto& l : g_log) if (l.find(text) != std::string::npos) return true;
    return false;
}
static int LogCount(const std::string& text) {
    int n = 0;
    for (const auto& l : g_log) if (l.find(text) != std::string::npos) ++n;
    return n;
}
static std::string SaveAll() { std::ostringstream o; Save(o); return o.str(); }
static ScriptRuntime& RT() { static ScriptRuntime rt; return rt; }

// Start -> Print(text)
static int StartPrint(const std::string& text) {
    const int s = Add(NodeType::Start), p = Add(NodeType::Print);
    N(p).value = text;
    Link_(s, p);
    return p;
}

// ---------------------------------------------------------------- flow

static void TestStartPrintEnd() {
    Fresh();
    const int p = StartPrint("hello");
    const int e = Add(NodeType::End);
    Link_(p, e);
    Run(RT());
    CHECK(LogCount("hello") == 1);
}

static void TestSequenceOrder() {
    Fresh();
    const int s = Add(NodeType::Start), q = Add(NodeType::Sequence);
    N(q).value = "3";
    Link_(s, q);
    for (int i = 0; i < 3; ++i) {
        const int p = Add(NodeType::Print);
        N(p).value = "step" + std::to_string(i);
        Link_(q, p, i);
    }
    Run(RT());
    CHECK(g_log.size() == 3);
    if (g_log.size() == 3) {
        CHECK(g_log[0].find("step0") != std::string::npos);
        CHECK(g_log[1].find("step1") != std::string::npos);
        CHECK(g_log[2].find("step2") != std::string::npos);
    }
}

static void TestBranchOnVariable() {
    Fresh();
    const int s = Add(NodeType::Start), set = Add(NodeType::SetVariable), br = Add(NodeType::Branch);
    const int yes = Add(NodeType::Print), no = Add(NodeType::Print);
    N(set).target = "flag"; N(set).value = "true";
    N(br).target = "flag";
    N(yes).value = "YES"; N(no).value = "NO";
    Link_(s, set); Link_(set, br); Link_(br, yes, 0); Link_(br, no, 1);
    Run(RT());
    CHECK(Logged("YES") && !Logged("NO"));

    N(set).value = "false";
    g_log.clear();
    Run(RT());
    CHECK(Logged("NO") && !Logged("YES"));
}

static void TestRepeatAndDone() {
    Fresh();
    const int s = Add(NodeType::Start), r = Add(NodeType::Repeat);
    const int body = Add(NodeType::Print), done = Add(NodeType::Print);
    N(r).value = "4"; N(body).value = "tick"; N(done).value = "finished";
    Link_(s, r); Link_(r, body, 0); Link_(r, done, 1);
    Run(RT());
    CHECK(LogCount("tick") == 4);
    CHECK(LogCount("finished") == 1);
    CHECK(g_log.back().find("finished") != std::string::npos);   // Done comes after every pass
}

static void TestDoOnce() {
    Fresh();
    const int u = Add(NodeType::OnUpdate), d = Add(NodeType::DoOnce), p = Add(NodeType::Print);
    N(p).value = "once";
    Link_(u, d); Link_(d, p);
    Run(RT());
    for (int i = 0; i < 5; ++i) Tick(RT(), 0.1f);
    CHECK(LogCount("once") == 1);
    Run(RT());                       // a new Play re-arms it
    Tick(RT(), 0.1f);
    CHECK(LogCount("once") == 2);
}

static void TestDelayWaitsForTicks() {
    Fresh();
    const int s = Add(NodeType::Start), d = Add(NodeType::Delay), p = Add(NodeType::Print);
    N(d).value = "1"; N(p).value = "later";
    Link_(s, d); Link_(d, p);
    Run(RT());
    CHECK(!Logged("later"));
    Tick(RT(), 0.4f);
    Tick(RT(), 0.4f);
    CHECK(!Logged("later"));
    Tick(RT(), 0.4f);
    CHECK(LogCount("later") == 1);
    Tick(RT(), 5.0f);
    CHECK(LogCount("later") == 1);   // fires once
}

static void TestStopCancelsDelayAndFiresOnStop() {
    Fresh();
    const int s = Add(NodeType::Start), d = Add(NodeType::Delay), p = Add(NodeType::Print);
    const int st = Add(NodeType::OnStop), bye = Add(NodeType::Print);
    N(d).value = "1"; N(p).value = "never"; N(bye).value = "bye";
    Link_(s, d); Link_(d, p); Link_(st, bye);
    Run(RT());
    Stop(RT());
    CHECK(Logged("bye"));
    Tick(RT(), 5.0f);
    CHECK(!Logged("never"));
}

static void TestOnUpdateEveryTick() {
    Fresh();
    const int u = Add(NodeType::OnUpdate), p = Add(NodeType::Print);
    N(p).value = "frame";
    Link_(u, p);
    Run(RT());
    for (int i = 0; i < 6; ++i) Tick(RT(), 0.016f);
    CHECK(LogCount("frame") == 6);
}

static void TestOnKey() {
    Fresh();
    const int k = Add(NodeType::OnKey), p = Add(NodeType::Print);
    N(k).prop = "Space"; N(p).value = "jump";
    Link_(k, p);
    Run(RT());
    Tick(RT(), 0.016f);
    CHECK(!Logged("jump"));
    g_keyPressed = KEY_SPACE;
    Tick(RT(), 0.016f);
    CHECK(LogCount("jump") == 1);
    g_keyPressed = KEY_W;
    Tick(RT(), 0.016f);
    CHECK(LogCount("jump") == 1);    // other keys do nothing
}

static void TestLoopIsCapped() {
    // Connect() gives a node one input, so a loop can't be drawn in the editor; a hand-edited or corrupt
    // scene file can still hold one. Start -> a -> b -> a -> ...
    Clear(); ResetStubs();
    std::istringstream in(
        "VSCRIPTS 1\nVGRAPH \"Loop\"\nVSCRIPT 4 3 3\n"
        "VNODE 1 0 0 0 \"\" \"\" \"\"\n"
        "VNODE 2 10 0 0 \"\" \"\" \"a\"\n"
        "VNODE 3 10 0 0 \"\" \"\" \"b\"\n"
        "VLINK 1 2\nVLINK 2 3\nVLINK 3 2\n");
    Load(in);
    Run(RT());                               // must come back
    CHECK(Logged("ran over"));
}

static void TestMultipleStartsAndScripts() {
    Fresh();
    StartPrint("one"); StartPrint("two");
    NewScript("Other");
    StartPrint("three");
    Run(RT());
    CHECK(LogCount("one") == 1 && LogCount("two") == 1 && LogCount("three") == 1);
}

// ---------------------------------------------------------------- variables, math, logic

// Builds Start -> Print and returns Print's id; the caller wires a value node into its Text pin.
static int PrintWithWire(int valueNode) {
    const int p = StartPrint("fallback");
    ConnectData(valueNode, p, 0);
    return p;
}

static void TestMath() {
    struct Case { NodeType t; const char* a; const char* b; const char* want; };
    const Case cases[] = {
        { NodeType::Add, "2", "3", "5" }, { NodeType::Sub, "2", "3", "-1" },
        { NodeType::Mul, "4", "2.5", "10" }, { NodeType::Div, "9", "3", "3" },
        { NodeType::Div, "9", "0", "0" },     // divide by zero is 0, not a crash
        { NodeType::Add, "abc", "3", "3" },   // non-numbers count as 0
    };
    for (const Case& c : cases) {
        Fresh();
        const int m = Add(c.t);
        N(m).target = c.a; N(m).value = c.b;
        PrintWithWire(m);
        Run(RT());
        CHECK_STR(g_log.empty() ? "" : g_log[0], std::string("[script] ") + c.want);
    }
}

static void TestMathChainsThroughWires() {
    Fresh();
    const int add = Add(NodeType::Add), mul = Add(NodeType::Mul);
    N(add).target = "2"; N(add).value = "3";        // 5
    N(mul).value = "10";                            // (wired 5) * 10
    ConnectData(add, mul, 0);
    PrintWithWire(mul);
    Run(RT());
    CHECK(g_log.size() == 1 && g_log[0].find("50") != std::string::npos);
}

static void TestLerpAndRandom() {
    Fresh();
    const int l = Add(NodeType::Lerp);
    N(l).target = "10"; N(l).value = "20"; N(l).prop = "0.25";
    PrintWithWire(l);
    Run(RT());
    CHECK(g_log.size() == 1 && g_log[0].find("12.5") != std::string::npos);

    Fresh();
    const int r = Add(NodeType::Random);
    N(r).target = "5"; N(r).value = "7";
    const int s = Add(NodeType::Start), rep = Add(NodeType::Repeat), p = Add(NodeType::Print);
    N(rep).value = "50";
    Link_(s, rep); Link_(rep, p, 0);
    ConnectData(r, p, 0);
    Run(RT());
    bool inRange = g_log.size() == 50;
    for (const auto& l2 : g_log) {
        const double v = std::atof(l2.c_str() + std::strlen("[script] "));
        if (v < 5.0 || v > 7.0) inRange = false;
    }
    CHECK(inRange);
}

static void TestCompareAndLogic() {
    struct Case { NodeType t; const char* op; const char* a; const char* b; const char* want; };
    const Case cases[] = {
        { NodeType::Compare, "==", "1", "1.0", "true" },  { NodeType::Compare, "!=", "1", "2", "true" },
        { NodeType::Compare, "<", "1", "2", "true" },     { NodeType::Compare, ">", "1", "2", "false" },
        { NodeType::Compare, "<=", "2", "2", "true" },    { NodeType::Compare, ">=", "1", "2", "false" },
        { NodeType::Compare, "==", "cat", "cat", "true" },{ NodeType::Compare, "==", "cat", "dog", "false" },
        { NodeType::Compare, "<", "cat", "dog", "false" },   // text only supports equality
        { NodeType::And, "", "true", "false", "false" },  { NodeType::And, "", "true", "true", "true" },
        { NodeType::Or, "", "false", "true", "true" },    { NodeType::Or, "", "0", "false", "false" },
        { NodeType::Not, "", "false", "", "true" },       { NodeType::Not, "", "true", "", "false" },
    };
    for (const Case& c : cases) {
        Fresh();
        const int n = Add(c.t);
        N(n).target = c.a;
        if (c.t != NodeType::Not) N(n).value = c.b;
        if (c.t == NodeType::Compare) N(n).prop = c.op;
        PrintWithWire(n);
        Run(RT());
        CHECK_STR(g_log.empty() ? "" : g_log[0], std::string("[script] ") + c.want);
    }
}

static void TestBranchWiredToCompare() {
    Fresh();
    const int s = Add(NodeType::Start), br = Add(NodeType::Branch), cmp = Add(NodeType::Compare);
    const int yes = Add(NodeType::Print), no = Add(NodeType::Print);
    N(cmp).target = "3"; N(cmp).value = "5"; N(cmp).prop = "<";
    N(yes).value = "less"; N(no).value = "notless";
    Link_(s, br); Link_(br, yes, 0); Link_(br, no, 1);
    ConnectData(cmp, br, 0);
    Run(RT());
    CHECK(Logged("less") && !Logged("notless"));
}

static void TestGetVariableSeesLaterSets() {
    Fresh();
    const int s = Add(NodeType::Start), set1 = Add(NodeType::SetVariable), p1 = Add(NodeType::Print);
    const int set2 = Add(NodeType::SetVariable), p2 = Add(NodeType::Print), get = Add(NodeType::GetVariable);
    N(set1).target = "score"; N(set1).value = "1";
    N(set2).target = "score"; N(set2).value = "9";
    N(get).target = "score";
    ConnectData(get, p1, 0); ConnectData(get, p2, 0);
    Link_(s, set1); Link_(set1, p1); Link_(p1, set2); Link_(set2, p2);
    Run(RT());
    CHECK(g_log.size() == 2);
    if (g_log.size() == 2) {
        CHECK(g_log[0].find("1") != std::string::npos && g_log[0].find("9") == std::string::npos);
        CHECK(g_log[1].find("9") != std::string::npos);
    }
}

static void TestSetVariableFromWireAndDefault() {
    // Wire gives a value: the wire wins over the field.
    Fresh();
    const int s = Add(NodeType::Start), set = Add(NodeType::SetVariable), p = Add(NodeType::Print);
    const int add = Add(NodeType::Add), get = Add(NodeType::GetVariable);
    N(set).target = "x"; N(set).value = "default";
    N(add).target = "2"; N(add).value = "2";
    N(get).target = "x";
    ConnectData(add, set, 0); ConnectData(get, p, 0);
    Link_(s, set); Link_(set, p);
    Run(RT());
    CHECK(g_log.size() == 1 && g_log[0].find("4") != std::string::npos);

    // Wire gives nothing (variable never set): the field is the default.
    Fresh();
    const int s2 = Add(NodeType::Start), set2 = Add(NodeType::SetVariable), p2 = Add(NodeType::Print);
    const int never = Add(NodeType::GetVariable), get2 = Add(NodeType::GetVariable);
    N(set2).target = "y"; N(set2).value = "fallback"; N(never).target = "nope";
    N(get2).target = "y";
    ConnectData(never, set2, 0); ConnectData(get2, p2, 0);
    Link_(s2, set2); Link_(set2, p2);
    Run(RT());
    CHECK(g_log.size() == 1 && g_log[0].find("fallback") != std::string::npos);
}

static void TestVariablesResetOnRun() {
    Fresh();
    const int s = Add(NodeType::Start), br = Add(NodeType::Branch), p = Add(NodeType::Print);
    N(br).target = "carry"; N(p).value = "leaked";
    Link_(s, br); Link_(br, p, 0);
    // Second graph sets the variable only in the first run.
    Run(RT());
    CHECK(!Logged("leaked"));
}

static void TestValueWireLoopStops() {
    Fresh();
    const int a = Add(NodeType::Add), b = Add(NodeType::Add);
    ConnectData(a, b, 0); ConnectData(b, a, 0);
    PrintWithWire(a);
    Run(RT());                                  // must come back
    CHECK(Logged("loop back"));
}

// ---------------------------------------------------------------- objects, world, hud, audio

static void TestChangeObjectAndWorld() {
    Fresh();
    const int s = Add(NodeType::Start), c1 = Add(NodeType::Change), c2 = Add(NodeType::Change), c3 = Add(NodeType::Change);
    N(c1).target = "Workspace/Group/Cube"; N(c1).prop = "Mass"; N(c1).value = "7";
    N(c2).target = "Workspace/Missing"; N(c2).prop = "Mass"; N(c2).value = "1";
    N(c3).target = "Lighting"; N(c3).prop = "GlobalShadows"; N(c3).value = "false";
    Link_(s, c1); Link_(c1, c2); Link_(c2, c3);
    Run(RT());
    CHECK(g_sets.size() == 1);
    if (!g_sets.empty()) { CHECK_STR(g_sets[0].object, "Cube"); CHECK_STR(g_sets[0].prop, "Mass"); CHECK_STR(g_sets[0].value, "7"); }
    CHECK(Logged("No object 'Workspace/Missing'"));          // a missing object does not stop the chain
    CHECK(g_console.size() == 1 && g_console[0] == "game.Lighting.GlobalShadows = false");
}

static void TestChangeValueFromWire() {
    Fresh();
    const int s = Add(NodeType::Start), c = Add(NodeType::Change), add = Add(NodeType::Add);
    N(c).target = "Workspace/Cube"; N(c).prop = "Mass"; N(c).value = "1";
    N(add).target = "1"; N(add).value = "2";
    ConnectData(add, c, 0);
    Link_(s, c);
    Run(RT());
    CHECK(g_sets.size() == 1 && g_sets[0].value == "3");
}

static void TestSetWorld() {
    Fresh();
    const int s = Add(NodeType::Start), w = Add(NodeType::SetWorld);
    N(w).target = "Camera"; N(w).prop = "FOV"; N(w).value = "75";
    Link_(s, w);
    Run(RT());
    CHECK(g_console.size() == 1 && g_console[0] == "game.Camera.FOV = 75");

    Fresh();                                    // an object path is not a world group: nothing runs
    const int s2 = Add(NodeType::Start), w2 = Add(NodeType::SetWorld);
    N(w2).target = "Workspace/Cube";
    Link_(s2, w2);
    Run(RT());
    CHECK(g_console.empty());
}

static void TestGetProperty() {
    Fresh();
    const int g = Add(NodeType::GetProperty);
    N(g).target = "Workspace/Cube"; N(g).prop = "Position";
    PrintWithWire(g);
    Run(RT());
    CHECK(g_log.size() == 1 && g_log[0].find("(1, 2, 3)") != std::string::npos);

    Fresh();
    const int g2 = Add(NodeType::GetProperty);
    N(g2).target = "Workspace/Nobody"; N(g2).prop = "Position";
    PrintWithWire(g2);
    Run(RT());
    CHECK(Logged("No object 'Workspace/Nobody'"));
}

static void TestObjectActions() {
    Fresh();
    const int s = Add(NodeType::Start);
    const int cr = Add(NodeType::CreateObject), de = Add(NodeType::Destroy), cl = Add(NodeType::Clone);
    const int tp = Add(NodeType::Teleport), sv = Add(NodeType::SetVelocity);
    const int af = Add(NodeType::AddForce), ai = Add(NodeType::AddImpulse), rs = Add(NodeType::RunScript);
    N(cr).prop = "Sphere"; N(cr).value = "Ball2"; N(cl).value = "Copy";
    for (int id : { de, cl, tp, sv, af, ai, rs }) N(id).target = "Workspace/Ball";
    N(tp).value = "(1, 1, 1)"; N(sv).value = "(0, 9, 0)"; N(af).value = "(5, 0, 0)"; N(ai).value = "(0, 0, 5)";
    const int chain[] = { s, cr, cl, tp, sv, af, ai, rs, de };
    for (int i = 0; i + 1 < 9; ++i) Link_(chain[i], chain[i + 1]);
    Run(RT());
    CHECK(g_created.size() == 1 && g_created[0] == "Sphere");
    CHECK(g_names.size() == 2);                  // created + cloned objects both got the name asked for
    CHECK(g_names[&g_objects[2]] == "Ball2" && g_names[&g_objects[1]] == "Copy");
    CHECK(g_cloned == 1 && g_destroyed == 1 && g_scriptsStarted == 1);
    CHECK(g_sets.size() == 2);
    if (g_sets.size() == 2) {
        CHECK_STR(g_sets[0].prop, "Position"); CHECK_STR(g_sets[0].value, "(1, 1, 1)");
        CHECK_STR(g_sets[1].prop, "Velocity"); CHECK_STR(g_sets[1].value, "(0, 9, 0)");
    }
    CHECK(g_forces.size() == 2 && g_forces[0] == "force (5, 0, 0)" && g_forces[1] == "impulse (0, 0, 5)");
}

static void TestObjectActionOnMissingTarget() {
    Fresh();
    const int s = Add(NodeType::Start), de = Add(NodeType::Destroy), p = Add(NodeType::Print);
    N(de).target = "Workspace/Ghost"; N(p).value = "after";
    Link_(s, de); Link_(de, p);
    Run(RT());
    CHECK(g_destroyed == 0);
    CHECK(Logged("No object") && Logged("after"));    // logs, then carries on
}

static void TestVectorFromWire() {
    Fresh();
    const int s = Add(NodeType::Start), tp = Add(NodeType::Teleport), get = Add(NodeType::GetProperty);
    N(tp).target = "Workspace/Cube"; N(tp).value = "(0, 0, 0)";
    N(get).target = "Workspace/Ball"; N(get).prop = "Position";
    ConnectData(get, tp, 0);
    Link_(s, tp);
    Run(RT());
    CHECK(g_sets.size() == 1 && g_sets[0].value == "(1, 2, 3)");
}

static void TestRaycast() {
    Fresh();
    const int s = Add(NodeType::Start), br = Add(NodeType::Branch), ray = Add(NodeType::Raycast);
    const int hit = Add(NodeType::Print), miss = Add(NodeType::Print);
    N(ray).target = "(0, 5, 0)"; N(ray).value = "(0, -1, 0)"; N(ray).prop = "30";
    N(hit).value = "hit"; N(miss).value = "miss";
    Link_(s, br); Link_(br, hit, 0); Link_(br, miss, 1);
    ConnectData(ray, br, 0);

    g_rayHit = "Cube";
    Run(RT());
    CHECK(Logged("hit") && !Logged("miss"));
    CHECK(g_rays.size() == 1 && g_rays[0] == "(0, 5, 0)|(0, -1, 0)|30");

    g_log.clear(); g_rayHit = "";
    Run(RT());
    CHECK(Logged("miss") && !Logged("hit"));         // empty name = no hit = false
}

static void TestShowText() {
    Fresh();
    const int s = Add(NodeType::Start), t = Add(NodeType::ShowText);
    N(t).value = "Score 1"; N(t).prop = "Top Left"; N(t).target = "2";
    Link_(s, t);
    Run(RT());
    DrawHud(0, 0, 800, 600);
    CHECK(g_drawn.size() == 1 && g_drawn[0] == "Score 1");
    CHECK(g_drawTextCalls == 2);                    // text + shadow

    Tick(RT(), 1.0f);
    g_drawn.clear();
    DrawHud(0, 0, 800, 600);
    CHECK(g_drawn.size() == 1);                     // still there after 1s of 2s
    Tick(RT(), 1.5f);
    g_drawn.clear();
    DrawHud(0, 0, 800, 600);
    CHECK(g_drawn.empty());                         // expired
}

static void TestShowTextReplaceClearAndStop() {
    Fresh();
    const int s = Add(NodeType::Start), a = Add(NodeType::ShowText), b = Add(NodeType::ShowText);
    const int c = Add(NodeType::ShowText), d = Add(NodeType::ShowText);
    N(a).value = "first"; N(a).prop = "Center"; N(a).target = "0";
    N(b).value = "second"; N(b).prop = "Center"; N(b).target = "0";          // replaces first
    N(c).value = "corner"; N(c).prop = "Bottom Right"; N(c).target = "0";
    N(d).value = ""; N(d).prop = "Bottom Right"; N(d).target = "0";          // clears corner
    Link_(s, a); Link_(a, b); Link_(b, c); Link_(c, d);
    Run(RT());
    DrawHud(0, 0, 800, 600);
    CHECK(g_drawn.size() == 1 && g_drawn[0] == "second");
    Tick(RT(), 100.0f);
    g_drawn.clear();
    DrawHud(0, 0, 800, 600);
    CHECK(g_drawn.size() == 1);                     // 0 seconds means until replaced
    Stop(RT());
    g_drawn.clear();
    DrawHud(0, 0, 800, 600);
    CHECK(g_drawn.empty());                         // Play stopped
}

static void TestSound() {
    Fresh();
    SetSoundPlayer([](const std::string& path, float vol) { char b[32]; std::snprintf(b, sizeof(b), "%g", vol); g_played.push_back(path + "|" + b); });
    const int s = Add(NodeType::Start), v = Add(NodeType::SetVolume), p = Add(NodeType::PlaySound), p2 = Add(NodeType::PlaySound);
    N(v).value = "0.5"; N(p).target = "boom.wav"; N(p).value = "3"; N(p2).target = "/abs/hit.wav"; N(p2).value = "0.25";
    Link_(s, v); Link_(v, p); Link_(p, p2);
    Run(RT());
    CHECK(g_masterVolume == 0.5f);
    CHECK(g_played.size() == 2);
    if (g_played.size() == 2) {
        CHECK(g_played[0] == "boom.wav|1");          // volume above 1 is clamped; no project path, so the path is as typed
        CHECK(g_played[1] == "/abs/hit.wav|0.25");   // absolute paths are left alone
    }

    Fresh();                                        // master volume is clamped too
    const int s2 = Add(NodeType::Start), v2 = Add(NodeType::SetVolume);
    N(v2).value = "9";
    Link_(s2, v2);
    Run(RT());
    CHECK(g_masterVolume == 1.0f);
}

static void TestSoundWithoutBackend() {
    Fresh();                                        // no player installed: logs, and the chain carries on
    const int s = Add(NodeType::Start), p = Add(NodeType::PlaySound), after = Add(NodeType::Print);
    N(p).target = "boom.wav"; N(after).value = "still running";
    Link_(s, p); Link_(p, after);
    Run(RT());
    CHECK(Logged("no audio backend") && Logged("still running"));
}

// ---------------------------------------------------------------- connection rules

static void TestConnectRules() {
    Fresh();
    const int s = Add(NodeType::Start), p = Add(NodeType::Print), e = Add(NodeType::End);
    const int u = Add(NodeType::OnUpdate), br = Add(NodeType::Branch), add = Add(NodeType::Add);
    Connect(p, s);   CHECK(Active().links.empty());                     // nothing flows into an event
    Connect(p, u);   CHECK(Active().links.empty());
    Connect(e, p);   CHECK(Active().links.empty());                     // End has no output
    Connect(p, p);   CHECK(Active().links.empty());                     // no self link
    Connect(add, p); CHECK(Active().links.empty());                     // value nodes have no exec pins
    Connect(p, add); CHECK(Active().links.empty());
    Connect(br, p, 2); CHECK(Active().links.empty());                   // Branch only has pins 0 and 1
    Connect(br, p, 1); CHECK(Active().links.size() == 1);

    Connect(s, p);                                                      // an input takes one wire: replaces br's
    CHECK(Active().links.size() == 1 && Active().links[0].from == s);
    Connect(s, e);                                                      // events fan out: s keeps s->p and gains s->e
    CHECK(Active().links.size() == 2);
    Connect(s, e);                                                      // reconnecting the same pair adds no duplicate
    CHECK(Active().links.size() == 2);
    const int q = Add(NodeType::Print), r = Add(NodeType::Print);
    const size_t before = Active().links.size();
    Connect(p, q); Connect(p, r);                                       // a normal output pin takes one wire: q is replaced by r
    size_t fromP = 0; int toP = 0;
    for (const Link& l : Active().links) if (l.from == p) { ++fromP; toP = l.to; }
    CHECK(fromP == 1 && toP == r && Active().links.size() == before + 1);

    Fresh();
    const int a1 = Add(NodeType::Add), a2 = Add(NodeType::Add), pr = Add(NodeType::Print), st = Add(NodeType::Start);
    ConnectData(st, pr, 0);  CHECK(Active().dataLinks.empty());         // only value nodes feed data pins
    ConnectData(a1, pr, 1);  CHECK(Active().dataLinks.empty());         // Print has one data pin
    ConnectData(a1, pr, 0);  CHECK(Active().dataLinks.size() == 1);
    ConnectData(a2, pr, 0);                                             // pin takes one wire
    CHECK(Active().dataLinks.size() == 1 && Active().dataLinks[0].from == a2);
    ConnectData(a2, a2, 0);  CHECK(Active().dataLinks.size() == 1);     // no self link
    ConnectData(a1, a2, 1);  CHECK(Active().dataLinks.size() == 2);     // value nodes chain
}

// ---------------------------------------------------------------- code snippets

static std::string ReadFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void TestSnippetStartsClass() {
    Fresh();
    const int s = Add(NodeType::Start), c = Add(NodeType::CodeSnippet), p = Add(NodeType::Print);
    N(p).value = "after";
    Link_(s, c); Link_(c, p);
    Run(RT());
    CHECK(g_snippetsStarted.size() == 1 && g_snippetsStarted[0] == "VSnip_T_" + std::to_string(c));
    CHECK(Logged("after"));                          // the flow carries straight on; the snippet runs on its own

    g_log.clear(); g_snippetsKnown = false;          // library not built yet: say so, keep going
    Run(RT());
    CHECK(Logged("not compiled yet") && Logged("after"));
    CHECK(Logged("[script]") && !Logged("[visual script]"));   // the output prefix
}

static std::string SnipName(int id) { return "VSnip_T_" + std::to_string(id); }

static void TestSnippetInputs() {
    Fresh();
    const int s = Add(NodeType::Start), c = Add(NodeType::CodeSnippet), add = Add(NodeType::Add), get = Add(NodeType::GetProperty);
    N(c).target = "3";                                        // three value inputs
    N(add).target = "2"; N(add).value = "3";
    N(get).target = "Workspace/Cube"; N(get).prop = "Position";
    ConnectData(add, c, 0); ConnectData(get, c, 2);           // pin 1 stays unwired
    Link_(s, c);
    Run(RT());
    const auto& in = g_snippetInputs[SnipName(c)];
    CHECK(in.size() == 3);
    if (in.size() == 3) { CHECK(in[0] == "5"); CHECK(in[1].empty()); CHECK(in[2] == "(1, 2, 3)"); }

    Fresh();                                                  // no inputs: an empty list
    const int s2 = Add(NodeType::Start), c2 = Add(NodeType::CodeSnippet);
    Link_(s2, c2);
    Run(RT());
    CHECK(g_snippetInputs[SnipName(c2)].empty());
}

static void TestSnippetOutputs() {
    Fresh();
    const int s = Add(NodeType::Start), c = Add(NodeType::CodeSnippet), p = Add(NodeType::Print), q = Add(NodeType::Print);
    N(c).prop = "2";                                          // two value outputs
    N(p).value = "none"; N(q).value = "none";
    Link_(s, c); Link_(c, p); Link_(p, q);
    ConnectData(c, p, 0, 0); ConnectData(c, q, 0, 1);
    CHECK(Active().dataLinks.size() == 2);
    g_snippetOutputs[SnipName(c)] = { "alpha", "beta" };      // what the snippet's Out(0), Out(1) would have set
    Run(RT());
    CHECK(g_log.size() == 2);
    if (g_log.size() == 2) {
        CHECK(g_log[0].find("alpha") != std::string::npos);
        CHECK(g_log[1].find("beta") != std::string::npos);
    }

    g_log.clear(); g_snippetOutputs.clear();                  // nothing set yet: the pin's own literal takes over
    Run(RT());
    CHECK(g_log.size() == 2 && g_log[0].find("none") != std::string::npos);
}

static void TestSnippetPinRules() {
    Fresh();
    const int c = Add(NodeType::CodeSnippet), p = Add(NodeType::Print), add = Add(NodeType::Add);
    CHECK(SnippetInputs(N(c)) == 0 && SnippetOutputs(N(c)) == 0);   // starts with none
    ConnectData(c, p, 0);  CHECK(Active().dataLinks.empty());       // no output pins yet
    ConnectData(add, c, 0); CHECK(Active().dataLinks.empty());      // no input pins yet
    N(c).target = "2"; N(c).prop = "2";
    ConnectData(c, p, 0, 1);  CHECK(Active().dataLinks.size() == 1);
    ConnectData(c, p, 0, 2);                                       // there is no output 2: nothing changes
    CHECK(Active().dataLinks.size() == 1 && Active().dataLinks[0].fromPin == 1);
    ConnectData(add, c, 1);   CHECK(Active().dataLinks.size() == 2);
    ConnectData(add, c, 2);   CHECK(Active().dataLinks.size() == 2);   // no input 2
    ConnectData(c, c, 0);     CHECK(Active().dataLinks.size() == 2);   // no self wire

    N(c).prop = "1";                                               // drop to one output: the wire from output 1 goes
    SanitizeDataLinks();
    CHECK(Active().dataLinks.size() == 1 && Active().dataLinks[0].to == c);
    N(c).target = "1";
    SanitizeDataLinks();                                           // and the one into input 1
    CHECK(Active().dataLinks.empty());

    N(c).target = "99"; N(c).prop = "-4";                          // counts are clamped
    CHECK(SnippetInputs(N(c)) == 6 && SnippetOutputs(N(c)) == 0);
}

static void TestSnippetWiresSaveLoad() {
    Fresh();
    const int s = Add(NodeType::Start), c = Add(NodeType::CodeSnippet), p = Add(NodeType::Print), add = Add(NodeType::Add);
    N(c).target = "2"; N(c).prop = "3"; N(c).value = "Out(2, InNum(0) + 1);";
    N(add).target = "4";
    Link_(s, c); Link_(c, p);
    ConnectData(add, c, 0); ConnectData(c, p, 0, 2);
    const std::string saved = SaveAll();
    CHECK(saved.find("VDLINKP") != std::string::npos);             // a non-zero output pin needs the longer line
    Clear(); ResetStubs();
    std::istringstream in(saved);
    Load(in);
    CHECK(Active().dataLinks.size() == 2);
    bool pin2 = false;
    for (const DataLink& d : Active().dataLinks) if (d.fromPin == 2) pin2 = true;
    CHECK(pin2);
    CHECK(SaveAll() == saved);

    // Old scenes, whose value wires had no output pin, still load.
    Clear();
    std::istringstream old("VSCRIPTS 1\nVGRAPH \"Old\"\nVSCRIPT 3 2 0\nVNODE 1 17 0 0 \"1\" \"\" \"2\"\nVNODE 2 10 0 0 \"\" \"\" \"x\"\nVDLINKS 1\nVDLINK 1 2 0\n");
    Load(old);
    CHECK(Count() == 1 && Active().dataLinks.size() == 1 && Active().dataLinks[0].fromPin == 0);
}

static void TestSnippetGeneratorHelpers() {
    Fresh();
    const int a = Add(NodeType::CodeSnippet);
    N(a).value = "Out(0, InNum(0) * 2);";
    const std::string src = GenerateSnippetSource();
    CHECK(src.find("struct VSnip_T_" + std::to_string(a) + " : vsnip::Base") != std::string::npos);
    CHECK(src.find("const char* Key() const override { return \"VSnip_T_" + std::to_string(a) + "\"; }") != std::string::npos);
    CHECK(src.find("struct Base : fly::Script") != std::string::npos);
    CHECK(src.find("std::string In(int pin)") != std::string::npos && src.find("void Out(int pin") != std::string::npos);
    CHECK(src.find("InNum") != std::string::npos && src.find("InBool") != std::string::npos && src.find("InVec3") != std::string::npos);
    CHECK(src.find("struct Base") < src.find("struct VSnip_T_"));   // the helpers come first
}

static void TestSnippetFanOutFromEvent() {
    Fresh();
    const int s = Add(NodeType::Start), a = Add(NodeType::CodeSnippet), b = Add(NodeType::CodeSnippet);
    Link_(s, a); Link_(s, b);
    Run(RT());
    CHECK(g_snippetsStarted.size() == 2);            // two things from one Start
}

static void TestSnippetGenerator() {
    Fresh();
    CHECK(GenerateSnippetSource().empty());          // no snippet, no file
    const int a = Add(NodeType::CodeSnippet);
    N(a).value = "#include <algorithm>\nint x = 1;\nfly::Print(x);";
    NewScript("Other");
    const int b = Add(NodeType::CodeSnippet);
    N(b).value = "co_await fly::Wait(2.0f);";
    const std::string src = GenerateSnippetSource();
    CHECK(src.find("#include \"fly.hpp\"") != std::string::npos);
    CHECK(src.find("struct VSnip_T_" + std::to_string(a) + " : vsnip::Base") != std::string::npos);
    CHECK(src.find("FLY_SCRIPT(VSnip_T_" + std::to_string(a) + ")") != std::string::npos);
    CHECK(src.find("struct VSnip_Other_" + std::to_string(b) + " : vsnip::Base") != std::string::npos);
    CHECK(src.find("co_await fly::Wait(2.0f);") != std::string::npos);
    CHECK(src.find("#line 1 \"Code Snippet T/" + std::to_string(a) + "\"") != std::string::npos);
    CHECK(src.find("co_return;") != std::string::npos);
    // the include is hoisted above the classes, and no longer sits inside Run()
    const size_t inc = src.find("#include <algorithm>"), cls = src.find("struct VSnip_");
    CHECK(inc != std::string::npos && inc < cls);
    CHECK(src.find("#include <algorithm>", inc + 1) == std::string::npos);
    CHECK(src.find("int x = 1;") > cls);
}

static void TestSnippetWriteFile() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("vs_snippet_test_" + std::to_string(vs_getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path file = dir / "Scripts" / ".visualscript" / "snippets.cpp";

    Fresh();
    CHECK(!WriteSnippetSources(dir.string(), false));              // no Scripts folder and not allowed to make one
    CHECK(!fs::exists(file));
    const int a = Add(NodeType::CodeSnippet);
    N(a).value = "fly::Print(1);";
    CHECK(!WriteSnippetSources(dir.string(), false) && !fs::exists(file));
    CHECK(WriteSnippetSources(dir.string()));                      // creates the folders and the file
    CHECK(fs::exists(file) && ReadFile(file) == GenerateSnippetSource());
    const auto stamp = fs::last_write_time(file);
    CHECK(!WriteSnippetSources(dir.string()));                     // unchanged: not rewritten
    CHECK(fs::last_write_time(file) == stamp);
    N(a).value = "fly::Print(2);";
    CHECK(WriteSnippetSources(dir.string()) && ReadFile(file).find("fly::Print(2);") != std::string::npos);
    CHECK(WriteSnippetSources(dir.string(), false) == false);      // already current

    if (const char* dump = std::getenv("VS_SNIPPET_DUMP")) fs::copy_file(file, dump, fs::copy_options::overwrite_existing);

    RemoveNode(a);
    CHECK(WriteSnippetSources(dir.string()) && !fs::exists(file)); // the last snippet is gone: so is the file
    fs::remove_all(dir);
}

static void TestSnippetFingerprint() {
    Fresh();
    const size_t none = SnippetFingerprint();
    const int a = Add(NodeType::CodeSnippet);
    const size_t one = SnippetFingerprint();
    CHECK(one != none);
    N(a).value = "fly::Print(3);";
    CHECK(SnippetFingerprint() != one);
    const size_t edited = SnippetFingerprint();
    Add(NodeType::Print);
    CHECK(SnippetFingerprint() == edited);                         // other nodes don't count
    RemoveNode(a);
    CHECK(SnippetFingerprint() == none);
}

static void TestSnippetSaveLoadMultiline() {
    Fresh();
    const int a = Add(NodeType::CodeSnippet);
    const std::string code = "int n = 0;\nfor (int i = 0; i < 3; ++i) {\n    n += i;\n}\nfly::Print(\"sum: \\\"\", n);\n\tdone();";
    N(a).value = code;
    const std::string saved = SaveAll();
    Clear();
    std::istringstream in(saved + "AFTER 7\n");
    Load(in);
    CHECK(Count() == 1 && Active().nodes.size() == 1 && Active().nodes[0].value == code);
    std::string tag; int v = 0; in >> tag >> v;
    CHECK(tag == "AFTER" && v == 7);                               // multi-line text doesn't confuse the reader
}

static void TestEventFanOut() {
    Fresh();
    const int s = Add(NodeType::Start), a = Add(NodeType::Print), b = Add(NodeType::Print), c = Add(NodeType::Print);
    N(a).value = "A"; N(b).value = "B"; N(c).value = "C";
    Link_(s, a); Link_(s, b); Link_(s, c);
    Run(RT());
    CHECK(g_log.size() == 3);
    if (g_log.size() == 3) {          // wires run in the order they were made
        CHECK(g_log[0].find("A") != std::string::npos);
        CHECK(g_log[1].find("B") != std::string::npos);
        CHECK(g_log[2].find("C") != std::string::npos);
    }

    // Each branch runs to its end before the next wire starts.
    Fresh();
    const int s2 = Add(NodeType::Start), a1 = Add(NodeType::Print), a2 = Add(NodeType::Print), b1 = Add(NodeType::Print);
    N(a1).value = "a1"; N(a2).value = "a2"; N(b1).value = "b1";
    Link_(s2, a1); Link_(s2, b1); Link_(a1, a2);
    Run(RT());
    CHECK(g_log.size() == 3 && g_log[0].find("a1") != std::string::npos && g_log[1].find("a2") != std::string::npos &&
          g_log[2].find("b1") != std::string::npos);

    // Other events fan out too, every tick.
    Fresh();
    const int u = Add(NodeType::OnUpdate), x = Add(NodeType::Print), y = Add(NodeType::Print);
    N(x).value = "x"; N(y).value = "y";
    Link_(u, x); Link_(u, y);
    Run(RT());
    Tick(RT(), 0.1f); Tick(RT(), 0.1f);
    CHECK(LogCount("x") == 2 && LogCount("y") == 2);

    // A fan-out graph survives save and load.
    Fresh();
    const int s3 = Add(NodeType::Start), p1 = Add(NodeType::Print), p2 = Add(NodeType::Print);
    N(p1).value = "one"; N(p2).value = "two";
    Link_(s3, p1); Link_(s3, p2);
    const std::string saved = SaveAll();
    Clear(); ResetStubs();
    std::istringstream in(saved);
    Load(in);
    Run(RT());
    CHECK(Logged("one") && Logged("two"));
}

static void TestEndFanIn() {
    Fresh();
    const int s = Add(NodeType::Start), a = Add(NodeType::Print), b = Add(NodeType::Print), e = Add(NodeType::End);
    N(a).value = "A"; N(b).value = "B";
    Link_(s, a); Link_(s, b);
    Link_(a, e); Link_(b, e);                        // both chains finish at the one End
    int intoEnd = 0;
    for (const Link& l : Active().links) if (l.to == e) ++intoEnd;
    CHECK(intoEnd == 2);
    Link_(a, e);                                     // the same wire again adds nothing
    intoEnd = 0;
    for (const Link& l : Active().links) if (l.to == e) ++intoEnd;
    CHECK(intoEnd == 2);
    Run(RT());
    CHECK(g_log.size() == 2);

    const std::string saved = SaveAll();             // and the fan-in graph round-trips
    Clear(); ResetStubs();
    std::istringstream in(saved);
    Load(in);
    int again = 0;
    for (const Node& n : Active().nodes) if (n.type == NodeType::End) for (const Link& l : Active().links) if (l.to == n.id) ++again;
    CHECK(again == 2);

    Fresh();                                         // other nodes still take one input
    const int s2 = Add(NodeType::Start), a2 = Add(NodeType::Print), b2 = Add(NodeType::Print), p = Add(NodeType::Print);
    Link_(s2, a2); Link_(s2, b2); Link_(a2, p); Link_(b2, p);
    int intoP = 0;
    for (const Link& l : Active().links) if (l.to == p) ++intoP;
    CHECK(intoP == 1);
}

static void TestRemoveNodeDropsWires() {
    Fresh();
    const int s = Add(NodeType::Start), p = Add(NodeType::Print), add = Add(NodeType::Add);
    Link_(s, p); ConnectData(add, p, 0);
    RemoveNode(add);
    CHECK(Active().dataLinks.empty() && Active().links.size() == 1);
    RemoveNode(p);
    CHECK(Active().links.empty());
}

// ---------------------------------------------------------------- save / load


static void TestSaveLoadRoundTrip() {
    Fresh();
    const int s = Add(NodeType::Start), br = Add(NodeType::Branch), cmp = Add(NodeType::Compare);
    const int yes = Add(NodeType::ShowText), no = Add(NodeType::Print), sq = Add(NodeType::Sequence);
    N(cmp).target = "a b"; N(cmp).value = "say \"hi\""; N(cmp).prop = ">=";   // spaces and quotes survive
    N(yes).value = "won"; N(yes).prop = "Top Center";
    N(sq).value = "3";
    Link_(s, br); Link_(br, yes, 0); Link_(br, no, 1); Link_(no, sq); ConnectData(cmp, br, 0);
    NewScript("Second");
    StartPrint("two");
    const std::string saved = SaveAll();

    Clear();
    std::istringstream in(saved);
    std::string tag; in >> tag;
    CHECK(tag == "VSCRIPTS");
    in.seekg(0);
    Load(in);
    CHECK(Count() == 2);
    Select(0);
    Graph& g = Active();
    CHECK(g.nodes.size() == 6 && g.links.size() == 4 && g.dataLinks.size() == 1);
    const Node* c = nullptr;
    for (const Node& n : g.nodes) if (n.type == NodeType::Compare) c = &n;
    CHECK(c != nullptr);
    if (c) { CHECK_STR(c->target, "a b"); CHECK_STR(c->value, "say \"hi\""); CHECK_STR(c->prop, ">="); }
    bool pin1 = false;
    for (const Link& l : g.links) if (l.fromPin == 1) pin1 = true;
    CHECK(pin1);                                                   // output pin index survived
    CHECK(SaveAll() == saved);                                     // and saving again is byte-identical

    ResetStubs();
    Run(RT());                                                     // the loaded graph still runs
    CHECK(Logged("two"));
}

static void TestLoadKeepsFollowingData() {
    Fresh();
    StartPrint("x");
    ConnectData(Add(NodeType::Add), Find(1)->id == 1 ? 2 : 2, 0);
    const std::string saved = SaveAll() + "NEXTSECTION 42\n";
    Clear();
    std::istringstream in(saved);
    Load(in);
    std::string tag; int v = 0;
    in >> tag >> v;
    CHECK(tag == "NEXTSECTION" && v == 42);                        // the reader stops exactly at the block's end
}

static void TestLoadOldFormat() {
    // A scene from before pins and value wires: VLINK lines only, node types 0..2.
    const char* old =
        "VSCRIPTS 1\n"
        "VGRAPH \"Old\"\n"
        "VSCRIPT 4 3 2\n"
        "VNODE 1 0 10 10 \"\" \"\" \"\"\n"
        "VNODE 2 1 20 20 \"Workspace/Cube\" \"Mass\" \"5\"\n"
        "VNODE 3 2 30 30 \"\" \"\" \"\"\n"
        "VLINK 1 2\n"
        "VLINK 2 3\n";
    Clear(); ResetStubs();
    std::istringstream in(old);
    Load(in);
    CHECK(Count() == 1);
    CHECK(Active().nodes.size() == 3 && Active().links.size() == 2 && Active().dataLinks.empty());
    Run(RT());
    CHECK(g_sets.size() == 1 && g_sets[0].value == "5");

    // The very old single-graph format.
    Clear(); ResetStubs();
    std::istringstream in2("VSCRIPT 3 2 1\nVNODE 1 0 0 0 \"\" \"\" \"\"\nVNODE 2 1 0 0 \"Workspace/Ball\" \"Mass\" \"2\"\nVLINK 1 2\n");
    Load(in2);
    Run(RT());
    CHECK(Count() == 1 && g_sets.size() == 1);
}

static void TestLoadBadInput() {
    Clear();
    std::istringstream junk("SOMETHINGELSE 1 2 3\n");
    Load(junk);
    CHECK(Count() == 0);
    std::string tag; junk.clear(); junk.seekg(0); junk >> tag;
    CHECK(tag == "SOMETHINGELSE");                                 // position restored for the caller

    Clear();
    std::istringstream cut("VSCRIPTS 1\nVGRAPH \"A\"\nVSCRIPT 2 2 0\nVNODE 1 0 0 0 \"\" \"\" \"\"\n");
    Load(cut);                                                     // truncated: must not crash
    CHECK(Count() == 0);

    Clear();
    std::istringstream wild("VSCRIPTS 1\nVGRAPH \"A\"\nVSCRIPT 2 1 0\nVNODE 1 99 0 0 \"\" \"\" \"\"\n");
    Load(wild);                                                    // out-of-range node type is clamped, not trusted
    CHECK(Count() == 1 && static_cast<int>(Active().nodes[0].type) < static_cast<int>(NodeType::Count_));
}

// ---------------------------------------------------------------- main

int main() {
    struct T { const char* name; void (*fn)(); };
    const T tests[] = {
        { "StartPrintEnd", TestStartPrintEnd }, { "SequenceOrder", TestSequenceOrder },
        { "BranchOnVariable", TestBranchOnVariable }, { "RepeatAndDone", TestRepeatAndDone },
        { "DoOnce", TestDoOnce }, { "DelayWaitsForTicks", TestDelayWaitsForTicks },
        { "StopCancelsDelayAndFiresOnStop", TestStopCancelsDelayAndFiresOnStop },
        { "OnUpdateEveryTick", TestOnUpdateEveryTick }, { "OnKey", TestOnKey }, { "LoopIsCapped", TestLoopIsCapped },
        { "MultipleStartsAndScripts", TestMultipleStartsAndScripts },
        { "Math", TestMath }, { "MathChainsThroughWires", TestMathChainsThroughWires },
        { "LerpAndRandom", TestLerpAndRandom }, { "CompareAndLogic", TestCompareAndLogic },
        { "BranchWiredToCompare", TestBranchWiredToCompare }, { "GetVariableSeesLaterSets", TestGetVariableSeesLaterSets },
        { "SetVariableFromWireAndDefault", TestSetVariableFromWireAndDefault },
        { "VariablesResetOnRun", TestVariablesResetOnRun }, { "ValueWireLoopStops", TestValueWireLoopStops },
        { "ChangeObjectAndWorld", TestChangeObjectAndWorld }, { "ChangeValueFromWire", TestChangeValueFromWire },
        { "SetWorld", TestSetWorld }, { "GetProperty", TestGetProperty }, { "ObjectActions", TestObjectActions },
        { "ObjectActionOnMissingTarget", TestObjectActionOnMissingTarget }, { "VectorFromWire", TestVectorFromWire },
        { "Raycast", TestRaycast }, { "ShowText", TestShowText },
        { "ShowTextReplaceClearAndStop", TestShowTextReplaceClearAndStop }, { "Sound", TestSound }, { "SoundWithoutBackend", TestSoundWithoutBackend },
        { "ConnectRules", TestConnectRules }, { "SnippetStartsClass", TestSnippetStartsClass }, { "SnippetFanOutFromEvent", TestSnippetFanOutFromEvent },
        { "SnippetInputs", TestSnippetInputs }, { "SnippetOutputs", TestSnippetOutputs },
        { "SnippetPinRules", TestSnippetPinRules }, { "SnippetWiresSaveLoad", TestSnippetWiresSaveLoad },
        { "SnippetGeneratorHelpers", TestSnippetGeneratorHelpers },
        { "SnippetGenerator", TestSnippetGenerator }, { "SnippetWriteFile", TestSnippetWriteFile },
        { "SnippetFingerprint", TestSnippetFingerprint }, { "SnippetSaveLoadMultiline", TestSnippetSaveLoadMultiline },
        { "EventFanOut", TestEventFanOut }, { "EndFanIn", TestEndFanIn }, { "RemoveNodeDropsWires", TestRemoveNodeDropsWires },
        { "SaveLoadRoundTrip", TestSaveLoadRoundTrip }, { "LoadKeepsFollowingData", TestLoadKeepsFollowingData },
        { "LoadOldFormat", TestLoadOldFormat }, { "LoadBadInput", TestLoadBadInput },
    };
    int failedTests = 0;
    for (const T& t : tests) {
        const int before = g_failures;
        t.fn();
        const bool ok = g_failures == before;
        if (!ok) ++failedTests;
        std::printf("[%s] %s\n", ok ? " ok " : "FAIL", t.name);
    }
    std::printf("\n%d checks, %d failed; %zu tests, %d failed\n", g_checks, g_failures, sizeof(tests) / sizeof(tests[0]), failedTests);
    return g_failures == 0 ? 0 : 1;
}
