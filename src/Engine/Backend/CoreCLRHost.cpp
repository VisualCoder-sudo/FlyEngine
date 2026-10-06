// CoreCLRHost.cpp - embeds CoreCLR in the engine and drives C# scripts.

#include "../../../include/Engine/Scripts/CoreCLRHost.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "../include/Engine/Scripts/FlyScriptApi.hpp"
#include "../include/Engine/Frontend/ProjectManager.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Engine.hpp"
#include "../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <memory>
#include <set>
#include <iostream>
#include <sstream>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace fs = std::filesystem;

struct CoreCLRHost::Impl
{
    // Handle to the loaded coreclr module. HMODULE on Windows, a dlopen()
    // handle elsewhere; nothing outside LoadCoreCLR/Shutdown touches it.
    void* m_coreclr = nullptr;

    // CoreCLR function pointers
    CoreCLRHost::coreclr_initialize_fn      m_init = nullptr;
    CoreCLRHost::coreclr_create_delegate_fn m_create_delegate = nullptr;
    CoreCLRHost::coreclr_shutdown_fn        m_shutdown = nullptr;

    // Runtime state
    void* m_clr_handle = nullptr;
    unsigned int m_domain_id = 0;
    bool m_ready = false;
    std::string m_error;
    std::string m_project_path;
    fs::path m_coreclr_path;
    fs::path m_runtime_dir;

    // C# delegates for running one frame of all scripts and script lifecycle
    CoreCLRHost::ScriptRunDelegate m_script_run = nullptr;
    CoreCLRHost::ObjectScriptDelegate m_start_object = nullptr;
    CoreCLRHost::ObjectScriptDelegate m_stop_object = nullptr;
    CoreCLRHost::StandaloneScriptDelegate m_start_standalone = nullptr;
    CoreCLRHost::StandaloneScriptDelegate m_stop_standalone = nullptr;
    CoreCLRHost::VoidScriptDelegate m_clear_all = nullptr;
    CoreCLRHost::VoidScriptDelegate m_on_play_started = nullptr;
    CoreCLRHost::VoidScriptDelegate m_on_play_stopped = nullptr;
    CoreCLRHost::ScriptTypesDelegate m_script_types = nullptr;
    CoreCLRHost::ConsoleExecuteDelegate m_console_execute = nullptr;
    bool m_pending_play = false;
    bool m_pending_stop = false;

    // Last assembly load time (for hot-reload detection)
    fs::file_time_type m_last_asm_write{};

    Impl() = default;
    ~Impl() { if (m_ready) Shutdown(); }

    // Compares two "8.0.3"-style runtime directory names numerically on each
    // dot-separated component, so 10.0.0 correctly beats 8.0.0.
    static int CompareVersionDir(const std::string& a, const std::string& b)
    {
        size_t ai = 0, bi = 0;
        for (;;) {
            size_t ae = a.find('.', ai), be = b.find('.', bi);
            const std::string as = a.substr(ai, ae == std::string::npos ? std::string::npos : ae - ai);
            const std::string bs = b.substr(bi, be == std::string::npos ? std::string::npos : be - bi);
            const long av = std::strtol(as.c_str(), nullptr, 10);
            const long bv = std::strtol(bs.c_str(), nullptr, 10);
            if (av != bv) return av < bv ? -1 : 1;
            const bool aDone = ae == std::string::npos;
            const bool bDone = be == std::string::npos;
            if (aDone || bDone) return aDone && bDone ? 0 : (aDone ? -1 : 1);
            ai = ae + 1;
            bi = be + 1;
        }
    }

    static bool VersionDirGreater(const std::string& a, const std::string& b) {
        return CompareVersionDir(a, b) > 0;
    }

    void Log(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        ui::LogAlways("[CoreCLRHost] %s", buf);
    }

    // Locate and load the CoreCLR runtime, then resolve its three entry points.
    //
    // Windows-only for now: this embeds coreclr.dll via LoadLibraryW and binds
    // the exported coreclr_initialize/create_delegate/shutdown by name. Linux
    // has an equivalent (libcoreclr.so via dlopen), but it also needs a
    // different runtime search path, and shipping the editor without C#
    // scripting is better than shipping it with a half-tested CLR host. Until
    // that is done the host reports a clear reason and the editor runs with
    // scripting disabled -- see the soft-fail in Initialize().
    bool LoadCoreCLR()
    {
#if defined(_WIN32)
        // Try multiple locations for the runtime
        std::vector<fs::path> candidates;

        // 1. DOTNET_ROOT env var
        if (const char* dotnet_home = std::getenv("DOTNET_ROOT"); dotnet_home && *dotnet_home) {
            candidates.push_back(fs::u8path(dotnet_home) / "shared" / "Microsoft.NETCore.App");
        }

        // 2. User profile .dotnet (from our SDK install)
        if (const char* user_profile = std::getenv("USERPROFILE"); user_profile && *user_profile) {
            candidates.push_back(fs::u8path(user_profile) / ".dotnet" / "shared" / "Microsoft.NETCore.App");
        }

        // 3. Program Files dotnet (system-wide install)
        if (const char* program_files = std::getenv("ProgramFiles"); program_files && *program_files) {
            candidates.push_back(fs::u8path(program_files) / "dotnet" / "shared" / "Microsoft.NETCore.App");
        }

        m_runtime_dir.clear();
        m_coreclr_path.clear();

        for (const auto& base : candidates) {
            std::error_code ec;
            if (!fs::exists(base, ec)) continue;
            // Find the latest version directory. .NET uses SemVer, so a plain
            // string compare orders 8.0.0 before 10.0.0 wrongly; the runtime
            // directory names are all the same length in practice, so this is
            // good enough, but sort numerically on the leading component to be
            // safe.
            fs::path best;
            for (const auto& ent : fs::directory_iterator(base, ec)) {
                if (!ent.is_directory(ec)) continue;
                if (best.empty() || VersionDirGreater(ent.path().filename().string(),
                                                     best.filename().string())) {
                    best = ent.path();
                }
            }
            if (!best.empty() && fs::exists(best / "coreclr.dll")) {
                m_runtime_dir = best;
                m_coreclr_path = best / "coreclr.dll";
                break;
            }
        }

        if (m_coreclr_path.empty()) {
            std::ostringstream oss;
            oss << "CoreCLR runtime not found. Tried: ";
            for (const auto& c : candidates) oss << c.string() << "; ";
            m_error = oss.str();
            Log("ERROR: %s", m_error.c_str());
            return false;
        }

        Log("Using runtime: %s", m_runtime_dir.string().c_str());
        Log("Loading coreclr.dll from: %s", m_coreclr_path.string().c_str());

        m_coreclr = LoadLibraryW(m_coreclr_path.c_str());
        if (!m_coreclr) {
            m_error = "LoadLibrary(coreclr.dll) failed, error=" + std::to_string(GetLastError());
            Log("ERROR: %s", m_error.c_str());
            return false;
        }

        m_init = (CoreCLRHost::coreclr_initialize_fn)GetProcAddress(m_coreclr, "coreclr_initialize");
        m_create_delegate = (CoreCLRHost::coreclr_create_delegate_fn)GetProcAddress(m_coreclr, "coreclr_create_delegate");
        m_shutdown = (CoreCLRHost::coreclr_shutdown_fn)GetProcAddress(m_coreclr, "coreclr_shutdown");

        if (!m_init || !m_create_delegate || !m_shutdown) {
            m_error = "coreclr_* export resolution failed";
            Log("ERROR: %s", m_error.c_str());
            return false;
        }

        Log("CoreCLR functions resolved successfully");
        return true;
#else
        m_error =
            "C# scripting requires Windows: the CoreCLR host is not implemented on this "
            "platform. Build with -DFLYENGINE_ENABLE_CSHARP=OFF on Windows, or run without "
            "scripting on this one.";
        Log("WARNING: %s", m_error.c_str());
        return false;
#endif
    }

    // Build the Trusted Platform Assemblies (TPA) list.
    std::string BuildTpaList(const fs::path& scripts_dir)
    {
        std::vector<std::string> tpa;
        tpa.reserve(256);

        // Runtime DLLs
        if (!m_runtime_dir.empty()) {
            for (auto& ent : fs::directory_iterator(m_runtime_dir)) {
                if (ent.is_regular_file() && ent.path().extension() == ".dll") {
                    tpa.push_back(ent.path().string());
                }
            }
            Log("TPA: added %zu runtime DLLs from %s", tpa.size(), m_runtime_dir.string().c_str());
        }

        // FlyScript.dll (the C# SDK + user scripts compiled together)
        fs::path flyscript_dll = scripts_dir / "FlyScript.dll";
        if (fs::exists(flyscript_dll)) {
            tpa.push_back(flyscript_dll.string());
            m_last_asm_write = fs::last_write_time(flyscript_dll);
            Log("TPA: added FlyScript.dll: %s", flyscript_dll.string().c_str());
        } else {
            Log("WARNING: FlyScript.dll not found at %s", flyscript_dll.string().c_str());
        }

        // Any other DLLs the dotnet build copied next to FlyScript.dll (e.g. the
        // Microsoft.CodeAnalysis*.dll Roslyn assemblies used by the command bar).
        // Skip names the framework already provides so the native copy wins.
        if (fs::exists(scripts_dir)) {
            std::set<std::string> runtimeNames;
            for (auto& ent : fs::directory_iterator(m_runtime_dir))
                if (ent.is_regular_file() && ent.path().extension() == ".dll")
                    runtimeNames.insert(ent.path().filename().string());

            int added = 0;
            for (auto& ent : fs::directory_iterator(scripts_dir)) {
                if (!ent.is_regular_file() || ent.path().extension() != ".dll") continue;
                if (ent.path().filename() == fs::path("FlyScript.dll")) continue;
                if (runtimeNames.count(ent.path().filename().string())) continue;
                tpa.push_back(ent.path().string());
                ++added;
            }
            if (added > 0) Log("TPA: added %d project DLL(s) from %s", added, scripts_dir.string().c_str());
        }

        // Join with semicolons
        std::string joined;
        for (size_t i = 0; i < tpa.size(); ++i) {
            if (i) joined += ";";
            joined += tpa[i];
        }
        Log("TPA list built, total %zu entries", tpa.size());
        return joined;
    }

    // Initialize CoreCLR with the given TPA list.
    bool InitializeCoreCLR(const std::string& tpa)
    {
        std::string app_base = m_project_path;

        const char* prop_keys[]   = { "TRUSTED_PLATFORM_ASSEMBLIES" };
        const char* prop_vals[]   = { tpa.c_str() };

        Log("Initializing CoreCLR with app_base=%s", app_base.c_str());

        int hr = CallCoreCLRInit(m_init, app_base.c_str(), "FlyScriptHost", 1, prop_keys, prop_vals, &m_clr_handle, &m_domain_id);
        if (hr != 0 || m_clr_handle == nullptr) {
            m_error = "coreclr_initialize failed: 0x" + std::to_string(hr);
            Log("ERROR: %s", m_error.c_str());
            return false;
        }

        Log("CoreCLR initialized, handle=%p, domain=%u", m_clr_handle, m_domain_id);

        // Resolve the ScriptHost delegates: Run (per-frame tick) plus the
        // object/standalone lifecycle hooks.
        auto resolve = [&](const char* method, void** out) -> bool {
            int hr = CallCreateDelegate(m_create_delegate, m_clr_handle, m_domain_id,
                                        "FlyScript", "FlyScript.ScriptHost", method, out);
            if (hr != 0 || !*out) {
                m_error = std::string("coreclr_create_delegate(ScriptHost.") + method + ") failed: 0x" + std::to_string(hr);
                Log("ERROR: %s", m_error.c_str());
                return false;
            }
            Log("Delegate FlyScript.ScriptHost.%s acquired: %p", method, *out);
            return true;
        };

        if (!resolve("Run", (void**)&m_script_run)) return false;
        if (!resolve("StartObjectScript", (void**)&m_start_object)) return false;
        if (!resolve("StopObjectScript", (void**)&m_stop_object)) return false;
        if (!resolve("StartStandalone", (void**)&m_start_standalone)) return false;
        if (!resolve("StopStandalone", (void**)&m_stop_standalone)) return false;
        if (!resolve("ClearAll", (void**)&m_clear_all)) return false;
        if (!resolve("OnPlayStarted", (void**)&m_on_play_started)) return false;
        if (!resolve("OnPlayStopped", (void**)&m_on_play_stopped)) return false;
        if (!resolve("GetScriptTypeNames", (void**)&m_script_types)) return false;
        if (!resolve("ExecuteConsole", (void**)&m_console_execute)) return false;

        return true;
    }

    void Shutdown()
    {
        Log("Shutting down CoreCLR...");
        if (m_shutdown && m_clr_handle) {
            CallCoreCLRShutdown(m_shutdown, m_clr_handle);
            m_clr_handle = nullptr;
        }
        if (m_coreclr) {
#if defined(_WIN32)
            FreeLibrary(m_coreclr);
#endif
            m_coreclr = nullptr;
        }
        m_ready = false;
    }

    // SEH-protected wrappers for CoreCLR calls (must be static to avoid C++ unwinding issues)
    static int CallCoreCLRInit(coreclr_initialize_fn fn, const char* exePath, const char* appDomainFriendlyName,
                               int propertyCount, const char* propertyKeys[], const char* propertyValues[],
                               void** hostHandle, unsigned int* domainId)
    {
        FLY_TRY { return fn(exePath, appDomainFriendlyName, propertyCount, propertyKeys, propertyValues, hostHandle, domainId); } FLY_CATCH(return 0xC0000005;)  // Access violation code
    }

    static int CallCreateDelegate(coreclr_create_delegate_fn fn, void* hostHandle, unsigned int domainId,
                                  const char* entryPointAssemblyName, const char* entryPointTypeName,
                                  const char* entryPointMethodName, void** delegate)
    {
        FLY_TRY { return fn(hostHandle, domainId, entryPointAssemblyName, entryPointTypeName, entryPointMethodName, delegate); } FLY_CATCH(return 0xC0000005;)
    }

    static void CallCoreCLRShutdown(coreclr_shutdown_fn fn, void* hostHandle)
    {
        // Ignore shutdown crashes
        FLY_TRY { fn(hostHandle); } FLY_CATCH()
    }

    bool ShouldReloadAssembly(const fs::path& scripts_dir) const
    {
        fs::path flyscript_dll = scripts_dir / "FlyScript.dll";
        if (!fs::exists(flyscript_dll)) return false;
        auto current = fs::last_write_time(flyscript_dll);
        return current != m_last_asm_write;
    }
};

namespace {
CoreCLRHost* g_activeHost = nullptr;
}

CoreCLRHost::CoreCLRHost()
    : m_impl(std::make_unique<Impl>())
    , m_script_runtime(std::make_unique<ScriptRuntime>())
{
    m_script_runtime->SetHost(this);
    SetActiveRuntime(m_script_runtime.get());
    g_activeHost = this;
    SetPlayStateHook([](bool playing) {
        if (g_activeHost) g_activeHost->SetPlayActive(playing);
    });
}
CoreCLRHost::~CoreCLRHost() { Shutdown(); }

bool CoreCLRHost::Initialize(const std::string& projectPath)
{
    // Normalize path: if it's a .flyproj file, use its directory
    fs::path p(projectPath);
    if (p.extension() == ".flyproj") {
        p = p.parent_path();
    }

    // Validate path exists
    if (!fs::exists(p)) {
        m_impl->m_error = "Project path does not exist: " + p.string();
        m_impl->Log("ERROR: %s", m_impl->m_error.c_str());
        return false;
    }

    m_impl->m_project_path = p.string();

    m_impl->Log("Initializing CoreCLR for project: %s", m_impl->m_project_path.c_str());

    fs::path scripts_dir = fs::path(m_impl->m_project_path) / "Scripts";

    // Ensure Scripts directory exists
    if (!fs::exists(scripts_dir)) {
        m_impl->Log("Scripts directory not found, creating: %s", scripts_dir.string().c_str());
        std::error_code ec;
        fs::create_directories(scripts_dir, ec);
        if (ec) {
            m_impl->m_error = "Failed to create Scripts directory: " + ec.message();
            m_impl->Log("ERROR: %s", m_impl->m_error.c_str());
            return false;
        }
    }

    // Do not boot the CLR at all when there is no script assembly to run.
    // Booting CoreCLR and then failing to resolve the entry-point delegate
    // (and shutting the runtime back down) corrupts the process heap and can
    // crash the engine later on the next frame. Skip it entirely instead so
    // the editor runs cleanly with scripting disabled.
    fs::path flyscript_dll = scripts_dir / "FlyScript.dll";
    if (!fs::exists(flyscript_dll)) {
        m_impl->m_error = "No Scripts/FlyScript.dll found; C# scripting disabled.";
        m_impl->Log("WARNING: %s", m_impl->m_error.c_str());
        return false;
    }

    if (!m_impl->LoadCoreCLR()) return false;

    std::string tpa = m_impl->BuildTpaList(scripts_dir);

    if (!m_impl->InitializeCoreCLR(tpa)) {
        m_impl->Shutdown();
        return false;
    }

    m_impl->m_ready = true;
    m_impl->Log("CoreCLR host ready");
    return true;
}

// SEH wrapper for calling the script run delegate
static void CallScriptRun(CoreCLRHost::ScriptRunDelegate fn, void* host, float dt, int scriptId)
{
    // Logged by caller
    FLY_TRY { fn(host, dt, scriptId); } FLY_CATCH()
}

// SEH wrappers for arg-less and index-based lifecycle delegates.
static void CallVoidDelegate(CoreCLRHost::VoidScriptDelegate fn, void* host)
{
    FLY_TRY { fn(host); } FLY_CATCH()
}
static void CallObjectDelegate(CoreCLRHost::ObjectScriptDelegate fn, void* host, unsigned long long handle, const char* typeName)
{
    FLY_TRY { fn(host, handle, typeName); } FLY_CATCH()
}
static void CallStandaloneDelegate(CoreCLRHost::StandaloneScriptDelegate fn, void* host, int index, const char* typeName)
{
    FLY_TRY { fn(host, index, typeName); } FLY_CATCH()
}

// Call C# to enumerate IScript type names into `buffer`. Returns bytes written
// or -1 on failure/severe (SEH). The delegate writes NUL-terminated UTF-8.
static int CallScriptTypes(CoreCLRHost::ScriptTypesDelegate fn, void* host, void* buffer, int capacity)
{
    FLY_TRY { return fn(host, buffer, capacity); } FLY_CATCH(return -1;)
}

// Call C# to evaluate a console snippet, writing the result/error text into
// `buffer`. Returns 1 on success, 0 on C# error, -1 on severe failure (SEH).
static int CallConsoleExecute(CoreCLRHost::ConsoleExecuteDelegate fn, void* host, const char* code, void* buffer, int capacity)
{
    FLY_TRY { return fn(host, const_cast<char*>(code), buffer, capacity); } FLY_CATCH(return -1;)
}

void CoreCLRHost::Update(float dt)
{
    if (!m_impl->m_ready || !m_impl->m_script_run) return;

    // Check for hot-reload of FlyScript.dll
    fs::path scripts_dir = fs::path(m_impl->m_project_path) / "Scripts";
    if (m_impl->ShouldReloadAssembly(scripts_dir)) {
        m_impl->Log("FlyScript.dll changed, reloading...");
        LoadScriptsAssembly((scripts_dir / "FlyScript.dll").string());
        return;
    }

    // The pointer the managed host binds back (FlyNative_BindRuntime) is the
    // ScriptRuntime* world context, so the FlyNative_* bridge can resolve it.
    void* runtime = m_script_runtime.get();

// Process pending play/stop edge before ticking.
        if (m_impl->m_pending_play) {
            m_impl->m_pending_play = false;
            if (m_impl->m_on_play_started && m_script_runtime)
                m_script_runtime->SetSimPlaying(true);
            CallVoidDelegate(m_impl->m_on_play_started, runtime);
        } else if (m_impl->m_pending_stop) {
        m_impl->m_pending_stop = false;
        CallVoidDelegate(m_impl->m_on_play_stopped, runtime);
        if (m_script_runtime) m_script_runtime->SetSimPlaying(false);
    }

    // Drive the C# script host for this frame.
    CallScriptRun(m_impl->m_script_run, runtime, dt, 0);
    if (!m_impl->m_ready) return;
}

void CoreCLRHost::Shutdown()
{
    m_impl->Shutdown();
}

bool CoreCLRHost::IsReady() const { return m_impl && m_impl->m_ready; }
const std::string& CoreCLRHost::GetError() const { return m_impl->m_error; }

bool CoreCLRHost::LoadScriptsAssembly(const std::string& assemblyPath)
{
    if (!m_impl->m_ready) return false;

    std::string project = m_impl->m_project_path;
    m_impl->Shutdown();
    m_impl = std::make_unique<Impl>();
    return Initialize(project);
}

void CoreCLRHost::BindWorld(std::vector<ScatteredObject*>& objects,
                            std::vector<std::unique_ptr<ModelGroup>>& models,
                            Camera3D& camera, phys::Simulation& sim, Engine& engine)
{
    if (!m_script_runtime) m_script_runtime = std::make_unique<ScriptRuntime>();
    m_script_runtime->SetHost(this);
    m_script_runtime->BindWorld(objects, models, camera, sim, engine);
}

void CoreCLRHost::StartObjectScriptOn(ScatteredObject* obj)
{
    if (!m_impl->m_ready || !obj || obj->script.empty()) return;
    unsigned long long handle = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(obj));
    CallObjectDelegate(m_impl->m_start_object, m_script_runtime.get(), handle, obj->script.c_str());
}

void CoreCLRHost::StopObjectScriptOn(ScatteredObject* obj)
{
    if (!m_impl->m_ready || !obj) return;
    unsigned long long handle = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(obj));
    CallObjectDelegate(m_impl->m_stop_object, m_script_runtime.get(), handle, "");
}

void CoreCLRHost::StartStandaloneScriptAt(int index)
{
    if (!m_impl->m_ready || !m_script_runtime) return;
    auto& scripts = m_script_runtime->StandaloneScripts();
    if (index < 0 || index >= static_cast<int>(scripts.size())) return;
    if (scripts[index].typeName.empty()) return;
    CallStandaloneDelegate(m_impl->m_start_standalone, m_script_runtime.get(), index, scripts[index].typeName.c_str());
}

void CoreCLRHost::StopStandaloneScriptAt(int index)
{
    if (!m_impl->m_ready) return;
    CallStandaloneDelegate(m_impl->m_stop_standalone, m_script_runtime.get(), index, "");
}

void CoreCLRHost::ClearAllScripts()
{
    if (!m_impl->m_ready) return;
    CallVoidDelegate(m_impl->m_clear_all, m_script_runtime.get());
}

void CoreCLRHost::SyncStandaloneScripts()
{
    if (!m_impl->m_ready || !m_impl->m_script_types || !m_script_runtime) return;

    char buffer[8192];
    buffer[0] = '\0';
    int n = CallScriptTypes(m_impl->m_script_types, m_script_runtime.get(), buffer,
                            static_cast<int>(sizeof(buffer)));
    if (n <= 0) {
        buffer[0] = '\0';
    } else if (n >= static_cast<int>(sizeof(buffer))) {
        buffer[sizeof(buffer) - 1] = '\0';
    }

    auto& scripts = m_script_runtime->StandaloneScripts();

    // Add any IScript type from the assembly that is not already listed, so a
    // .cs dropped into Scripts/ appears in the explorer and runs on Play.
    std::string line;
    std::istringstream iss(buffer);
    while (std::getline(iss, line)) {
        if (line.empty() || line.front() == '\0') continue;
        bool exists = false;
        for (const auto& s : scripts)
            if (s.typeName == line) { exists = true; break; }
        if (exists) continue;
        ScriptRuntime::StandaloneScript s;
        s.name = line;
        s.typeName = line;
        s.runOnPlay = true;
        scripts.push_back(std::move(s));
    }
}

void CoreCLRHost::SetPlayActive(bool active)
{
    if (active) {
        if (!m_impl->m_pending_play) m_impl->m_pending_play = true;
        m_impl->m_pending_stop = false;
    } else {
        if (!m_impl->m_pending_stop) m_impl->m_pending_stop = true;
        m_impl->m_pending_play = false;
    }
}

bool CoreCLRHost::ExecuteConsoleCode(const std::string& code, std::string& outText, bool& outError)
{
    outText.clear();
    outError = false;
    if (!m_impl->m_ready || !m_impl->m_console_execute) {
        outText = "C# host not ready";
        outError = true;
        return false;
    }

    constexpr int kBuffer = 32768;
    std::vector<char> buffer(kBuffer, 0);
    int rc = CallConsoleExecute(m_impl->m_console_execute, m_script_runtime.get(),
                                code.c_str(), buffer.data(), kBuffer);
    buffer[kBuffer - 1] = '\0';
    outText.assign(buffer.data());

    if (rc == 1) return true;
    outError = true;
    if (rc == 0 && !outText.empty()) return false; // C# reported an error
    outText = "Console evaluation failed (host exception)";
    return false;
}