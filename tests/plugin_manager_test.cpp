// Plugin Manager against the real NativeScriptHost and real shared libraries: discovery, load failures, disable/enable,
// restart, hot reload, import, remove, rescan, and a seeded random sequence of all of them. No window.
#include "test_unique.hpp"
#include "Engine/Scripts/NativeScriptHost.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;
using NativeScript::NativeScriptHost;
using NativeScript::PluginModule;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static fs::path g_root, g_log;

static std::string Slurp(const fs::path& p) { std::ifstream in(p); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }
static int Count(const std::string& what) {
    std::istringstream in(Slurp(g_log));
    int n = 0;
    for (std::string l; std::getline(in, l);) n += (l == what);
    return n;
}
static void ClearLog() { std::error_code ec; fs::remove(g_log, ec); }

// plugins/nat/<name>/{id.json,build/<name>.so}
static void MakePlugin(const fs::path& project, const std::string& name, const char* lib, bool withId = true, const char* type = "nat") {
    const fs::path dir = project / "plugins" / type / name;
    fs::create_directories(dir / "build");
    fs::copy_file(lib, dir / "build" / (name + ".so"), fs::copy_options::overwrite_existing);
    if (withId) {
        std::ofstream(dir / "id.json") << "{\"Name\":\"" << name << "\",\"DateCreated\":\"2026-01-01\",\"Publisher\":\"Tester\","
            "\"Version\":\"2.3.4\",\"Type\":\"" << type << "\",\"CPluginAPIVersion\":\"1.2.0\","
            "\"SupportedEngineVersions\":[\">=0.0.0\"],\"Description\":\"A test plugin.\"}";
    }
}
static void MakeGarbage(const fs::path& project, const std::string& name) {
    fs::create_directories(project / "plugins" / "nat" / name / "build");
    std::ofstream(project / "plugins" / "nat" / name / "build" / (name + ".so")) << "not a library";
}
static const PluginModule* Find(NativeScriptHost& h, const std::string& n) {
    for (const auto& p : h.GetPlugins()) if (p->name == n) return p.get();
    return nullptr;
}
static int Running(NativeScriptHost& h) { int n = 0; for (const auto& p : h.GetPlugins()) n += p->loaded; return n; }
static void Touch(const fs::path& f) { fs::last_write_time(f, fs::last_write_time(f) + std::chrono::seconds(5)); }

static void TestDiscoveryAndFailures() {
    const fs::path proj = g_root / "discovery";
    MakePlugin(proj, "alpha", GOOD_PLUGIN);
    MakePlugin(proj, "noid", GOOD_PLUGIN, /*withId=*/false);
    MakePlugin(proj, "failing", FAILING_PLUGIN);
    MakeGarbage(proj, "garbage");
    ClearLog();
    {
        NativeScriptHost h;
        CHECK(h.Initialize(proj.string()));
        CHECK(h.GetPlugins().size() == 4);
        const auto* a = Find(h, "alpha");
        CHECK(a && a->loaded && a->enabled && !a->loadFailed);
        CHECK(a && a->version == "2.3.4" && a->publisher == "Tester" && a->description == "A test plugin.");
        CHECK(a && a->type == "nat" && a->hasIdentity);
        const auto* n = Find(h, "noid");
        CHECK(n && n->loaded && !n->hasIdentity && n->displayName == "noid");
        const auto* f = Find(h, "failing");
        CHECK(f && !f->loaded && f->loadFailed && f->errorMessage.find("error code: 7") != std::string::npos);
        CHECK(f && f->entry == nullptr && f->handle == nullptr);
        const auto* g = Find(h, "garbage");
        CHECK(g && !g->loaded && g->loadFailed && g->errorMessage == "Failed to load library");
        CHECK(Running(h) == 2);
        CHECK(Count("load") == 2);
        // A failing plugin is still unloaded cleanly when the host goes away.
    }
    CHECK(Count("unload") == 2);   // only the two that loaded; the failing one must not get on_unload
}

static void TestDisableEnableRestart() {
    const fs::path proj = g_root / "toggle";
    MakePlugin(proj, "alpha", GOOD_PLUGIN);
    MakePlugin(proj, "beta", GOOD_PLUGIN);
    ClearLog();
    {
        NativeScriptHost h;
        h.Initialize(proj.string());
        CHECK(Running(h) == 2);
        CHECK(h.SetPluginEnabled("alpha", false));
        CHECK(!Find(h, "alpha")->loaded && !Find(h, "alpha")->enabled && Find(h, "alpha")->entry == nullptr);
        CHECK(Count("unload") == 1);
        CHECK(Slurp(proj / "plugins" / "disabled.txt") == "alpha\n");
        CHECK(!h.SetPluginEnabled("alpha", false));        // already off: nothing happens
        CHECK(Count("unload") == 1);
        CHECK(!h.SetPluginEnabled("nope", true));          // unknown name
        // Hot reload must leave a disabled plugin alone even when its file changes.
        Touch(proj / "plugins/nat/alpha/build/alpha.so");
        h.CheckHotReload();
        CHECK(!Find(h, "alpha")->loaded);
        CHECK(Count("load") == 2);
        h.SetPluginEnabled("beta", false);
        h.SetPluginEnabled("alpha", true);
        CHECK(Find(h, "alpha")->loaded && Find(h, "alpha")->enabled);
        CHECK(Slurp(proj / "plugins" / "disabled.txt") == "beta\n");
        h.SetPluginEnabled("beta", true);
        CHECK(!fs::exists(proj / "plugins" / "disabled.txt"));   // nothing disabled: no file
        h.SetPluginEnabled("beta", false);
    }
    // Restart: beta stays off and is not loaded.
    ClearLog();
    {
        NativeScriptHost h;
        h.Initialize(proj.string());
        CHECK(Find(h, "beta") && !Find(h, "beta")->enabled && !Find(h, "beta")->loaded);
        CHECK(Find(h, "alpha")->loaded);
        CHECK(Count("load") == 1);
    }
}

static void TestHotReloadAndRecovery() {
    const fs::path proj = g_root / "hot";
    MakePlugin(proj, "alpha", GOOD_PLUGIN);
    MakePlugin(proj, "flaky", FAILING_PLUGIN);
    ClearLog();
    NativeScriptHost h;
    h.Initialize(proj.string());
    CHECK(Find(h, "flaky")->loadFailed);
    Touch(proj / "plugins/nat/alpha/build/alpha.so");
    h.CheckHotReload();
    CHECK(Count("load") == 2 && Count("unload") == 1 && Find(h, "alpha")->loaded);
    h.CheckHotReload();   // unchanged since: no second reload
    CHECK(Count("load") == 2);
    // Replace the failing library with a good one: the next change reloads it successfully.
    fs::copy_file(GOOD_PLUGIN, proj / "plugins/nat/flaky/build/flaky.so", fs::copy_options::overwrite_existing);
    Touch(proj / "plugins/nat/flaky/build/flaky.so");
    h.CheckHotReload();
    CHECK(Find(h, "flaky")->loaded && !Find(h, "flaky")->loadFailed && Find(h, "flaky")->errorMessage.empty());
}

static void TestImport() {
    const fs::path proj = g_root / "import_dest";
    const fs::path src = g_root / "import_src";
    fs::create_directories(proj);
    MakePlugin(src, "gamma", GOOD_PLUGIN);
    ClearLog();
    NativeScriptHost h;
    h.Initialize(proj.string());
    CHECK(h.GetPlugins().empty());

    auto r = h.ImportPlugin((src / "plugins/nat/gamma").string());
    CHECK(r.ok && r.message == "Imported \"gamma\".");
    CHECK(Find(h, "gamma") && Find(h, "gamma")->loaded);
    CHECK(fs::exists(proj / "plugins/nat/gamma/id.json") && fs::exists(proj / "plugins/nat/gamma/build/gamma.so"));
    CHECK(!fs::exists(proj / "plugins/nat/gamma.importing"));

    r = h.ImportPlugin((src / "plugins/nat/gamma").string());        // again: refused, nothing changes
    CHECK(!r.ok && r.message.find("already installed") != std::string::npos);
    CHECK(h.GetPlugins().size() == 1 && Count("load") == 1);

    r = h.ImportPlugin((src / "plugins/nat/gamma/build").string());   // picking build/ finds the plugin folder
    CHECK(!r.ok && r.message.find("already installed") != std::string::npos);

    r = h.ImportPlugin((proj / "plugins/nat/gamma").string());        // already inside the project
    CHECK(!r.ok);

    // A bare library.
    fs::create_directories(g_root / "lib_src");
    fs::copy_file(GOOD_PLUGIN, g_root / "lib_src" / "delta.so", fs::copy_options::overwrite_existing);
    r = h.ImportPlugin((g_root / "lib_src" / "delta.so").string());
    CHECK(r.ok && Find(h, "delta") && Find(h, "delta")->loaded && fs::exists(proj / "plugins/nat/delta/build/delta.so"));

    // Things that must be refused without touching the project.
    const auto before = h.GetPlugins().size();
    fs::create_directories(g_root / "empty_dir");
    CHECK(!h.ImportPlugin((g_root / "empty_dir").string()).ok);
    CHECK(!h.ImportPlugin((g_root / "does_not_exist").string()).ok);
    std::ofstream(g_root / "readme.txt") << "x";
    CHECK(!h.ImportPlugin((g_root / "readme.txt").string()).ok);
    fs::create_directories(g_root / "dotted/.hidden/build");
    CHECK(!h.ImportPlugin((g_root / "dotted/.hidden").string()).ok);
    MakePlugin(g_root / "fpc_src", "scripty", GOOD_PLUGIN, true, "fpc");
    CHECK(!h.ImportPlugin((g_root / "fpc_src/plugins/fpc/scripty").string()).ok);
    CHECK(h.GetPlugins().size() == before);
    CHECK(!fs::exists(proj / "plugins/nat/scripty"));
    for (const auto& e : fs::directory_iterator(proj / "plugins/nat"))
        CHECK(e.path().extension() != ".importing");

    // Folder without a built library: copied, with a hint.
    fs::create_directories(g_root / "nolib/epsilon");
    std::ofstream(g_root / "nolib/epsilon/id.json") << "{\"Name\":\"epsilon\",\"DateCreated\":\"2026-01-01\",\"Publisher\":\"T\",\"Version\":\"1.0.0\","
        "\"Type\":\"nat\",\"CPluginAPIVersion\":\"1.2.0\",\"SupportedEngineVersions\":[\">=0.0.0\"]}";
    r = h.ImportPlugin((g_root / "nolib/epsilon").string());
    CHECK(r.ok && r.message.find("no built library") != std::string::npos);
    CHECK(!Find(h, "epsilon"));
}

static void TestRemoveAndRescan() {
    const fs::path proj = g_root / "remove";
    MakePlugin(proj, "alpha", GOOD_PLUGIN);
    MakePlugin(proj, "beta", GOOD_PLUGIN);
    ClearLog();
    NativeScriptHost h;
    h.Initialize(proj.string());
    h.SetPluginEnabled("beta", false);
    auto r = h.RemovePlugin("beta");                    // disabled plugins can be removed too
    CHECK(r.ok && !Find(h, "beta") && !fs::exists(proj / "plugins/nat/beta"));
    CHECK(!fs::exists(proj / "plugins" / "disabled.txt"));
    r = h.RemovePlugin("alpha");
    CHECK(r.ok && !Find(h, "alpha") && !fs::exists(proj / "plugins/nat/alpha"));
    CHECK(Count("load") == 2 && Count("unload") == 2);
    CHECK(!h.RemovePlugin("alpha").ok);                 // already gone

    // Rescan finds only what is new, once.
    CHECK(h.RescanPlugins() == 0);
    MakePlugin(proj, "zeta", GOOD_PLUGIN);
    CHECK(h.RescanPlugins() == 1 && Find(h, "zeta") && Find(h, "zeta")->loaded);
    CHECK(h.RescanPlugins() == 0);
    CHECK(Count("load") == 3);                          // zeta loaded once; rescan never reloads the known ones

    // A removed name can be imported again.
    MakePlugin(g_root / "again_src", "zeta2", GOOD_PLUGIN);
    CHECK(h.RemovePlugin("zeta").ok);
    CHECK(h.ImportPlugin((g_root / "again_src/plugins/nat/zeta2").string()).ok);
}

// Random operations; after every step the number of loaded libraries must equal loads - unloads in the log, no plugin
// may be both loaded and disabled, and the disabled file must match the in-memory state.
static void TestRandomSequence(unsigned seed) {
    const fs::path proj = g_root / ("random" + std::to_string(seed));
    const fs::path pool = g_root / ("pool" + std::to_string(seed));
    const char* names[] = { "p0", "p1", "p2", "p3", "p4" };
    for (const char* n : names) MakePlugin(pool, n, (n[1] % 2) ? FAILING_PLUGIN : GOOD_PLUGIN);
    fs::create_directories(proj);
    ClearLog();
    std::mt19937 rng(seed);
    auto host = std::make_unique<NativeScriptHost>();
    host->Initialize(proj.string());
    for (int step = 0; step < 300; ++step) {
        const std::string name = names[rng() % 5];
        switch (rng() % 8) {
            case 0: host->ImportPlugin((pool / "plugins/nat" / name).string()); break;
            case 1: host->SetPluginEnabled(name, false); break;
            case 2: host->SetPluginEnabled(name, true); break;
            case 3: host->RemovePlugin(name); break;
            case 4: host->RescanPlugins(); break;
            case 5: if (Find(*host, name)) { Touch(proj / "plugins/nat" / name / "build" / (name + ".so")); host->CheckHotReload(); } break;
            case 6: if (Find(*host, name)) host->ReloadPlugin(name); break;
            case 7:   // restart the host, as closing and reopening the project does
                host.reset();
                host = std::make_unique<NativeScriptHost>();
                host->Initialize(proj.string());
                break;
        }
        const int live = Count("load") - Count("unload");
        if (live != Running(*host)) { std::printf("  seed %u step %d: log says %d live, host says %d\n", seed, step, live, Running(*host)); }
        CHECK(live == Running(*host));
        std::set<std::string> disabled;
        for (const auto& p : host->GetPlugins()) {
            CHECK(!(p->loaded && !p->enabled));
            CHECK(!(p->loaded && p->entry == nullptr));
            CHECK(!(!p->loaded && p->handle != nullptr));
            if (!p->enabled) disabled.insert(p->name);
        }
        std::set<std::string> onDisk;
        std::ifstream in(proj / "plugins" / "disabled.txt");
        for (std::string l; std::getline(in, l);) onDisk.insert(l);
        for (const auto& d : disabled) CHECK(onDisk.count(d));   // removed plugins may leave no stale entry either:
        for (const auto& d : onDisk) CHECK(Find(*host, d) || !fs::exists(proj / "plugins/nat" / d));
        if (g_fail) { std::printf("  stopped at seed %u step %d\n", seed, step); return; }
    }
    host.reset();
    CHECK(Count("load") == Count("unload"));
}

int main() {
    g_root = fs::temp_directory_path() / TestUnique("fly_plugin_manager_test");
    fs::remove_all(g_root);
    fs::create_directories(g_root);
    g_log = g_root / "events.log";
    setenv("FLY_TEST_PLUGIN_LOG", g_log.string().c_str(), 1);

    TestDiscoveryAndFailures();
    TestDisableEnableRestart();
    TestHotReloadAndRecovery();
    TestImport();
    TestRemoveAndRescan();
    for (unsigned seed = 1; seed <= 20 && !g_fail; ++seed) TestRandomSequence(seed);

    fs::remove_all(g_root);
    if (g_fail) { std::printf("plugin_manager_test: %d failure(s)\n", g_fail); return 1; }
    std::printf("plugin_manager_test: OK\n");
    return 0;
}
