// Windows guard macros: winsock2.h (pulled in by curl.h) includes windows.h,
// whose wingdi.h declares a GDI function named Rectangle that breaks raylib.h.
#ifdef _WIN32
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "../../../include/Engine/Backend/fcloudint.hpp"

#include "../../../include/Engine/Frontend/ui.hpp"

#if FLYENGINE_ENABLE_FLYCLOUD

#include "../../../include/Engine/Backend/FlyCloudModule.hpp"
#include "../include/Engine/Frontend/ProjectManager.hpp"

#include <curl/curl.h>

#include <mutex>
#include <string>
#include <vector>

namespace fcloud {

namespace {

struct LogMsg {
    std::string text;
    ui::LogSeverity severity = ui::LogSeverity::Neutral;
};

std::mutex g_mutex;
std::vector<LogMsg> g_pending;
std::vector<LogMsg> g_swap;
std::once_flag g_curlInit;

void QueueLog(const std::string& text, ui::LogSeverity severity) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending.push_back({ text, severity });
}

// Success lines are tagged green by their leading marker; everything else that
// isn't flagged as an error stays neutral.
ui::LogSeverity Classify(const std::string& text, bool is_error) {
    if (is_error) return ui::LogSeverity::Error;
    static const char* const kSuccessPrefixes[] = {
        "✨ SUCCESS", "✓ ", "🗑 ", "🔑 API key successfully", "🧹 API key successfully"
    };
    for (const char* p : kSuccessPrefixes) {
        if (text.rfind(p, 0) == 0) return ui::LogSeverity::Success;
    }
    return ui::LogSeverity::Neutral;
}

void EnsureCurlInitialized() {
    std::call_once(g_curlInit, []() { curl_global_init(CURL_GLOBAL_ALL); });
}

} // namespace

void DispatchCommand(const std::string& full_command) {
    if (full_command.empty()) return;

    // Each fcloud invocation starts with a clean output panel so results
    // don't stack on top of previous runs.
    ui::ClearLog();

    const project::Info& info = project::GetCurrentProject();
    if (info.path.empty()) {
        QueueLog("[fcloud] No project is currently open. Open a project first.", ui::LogSeverity::Error);
        return;
    }

    QueueLog("[fcloud] > " + full_command, ui::LogSeverity::Neutral);

    EnsureCurlInitialized();

    FlyEngine::FlyCloudModule::DispatchCommandAsync(
        full_command,
        info.path,
        [](const std::string& message, bool is_error) {
            QueueLog("[fcloud] " + message, Classify(message, is_error));
        },
        info.name);
}

void Update() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pending.empty()) return;
        g_swap.swap(g_pending);
    }

    for (const LogMsg& msg : g_swap) {
        ui::LogWithSeverity(msg.severity, "%s", msg.text.c_str());
    }
    g_swap.clear();
}

} // namespace fcloud

#else // FLYENGINE_ENABLE_FLYCLOUD

namespace fcloud {

void DispatchCommand(const std::string& full_command) {
    (void)full_command;
    ui::ClearLog();
    ui::LogWithSeverity(ui::LogSeverity::Error,
        "[fcloud] FlyCloud support is disabled in this build (-DFLYENGINE_ENABLE_FLYCLOUD=OFF).");
}

void Update() {
}

} // namespace fcloud

#endif // FLYENGINE_ENABLE_FLYCLOUD