// ExportDialog.cpp -- File > Export...: choose a target and an output folder, then pack the project into one
// executable (src/Engine/Export/ExportManager.cpp does the work, on a worker thread).
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Engine/Frontend/ProjectManager.hpp"
#include "../../../include/Engine/ExportManager.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ui {
namespace {

constexpr const char* kPopupId = "Export game##exportdialog";

struct Job {
    std::thread worker;
    std::atomic<bool> done{false};
    std::atomic<bool> cancel{false};
    std::atomic<float> progress{0.0f};
    std::mutex logMutex;
    std::vector<std::string> pendingLog;   // worker -> main thread
    exporter::Result result;
};

struct State {
    bool openRequested = false;
    bool exportRequested = false;
    bool configLoaded = false;
    std::string loadedForProject;
    exporter::Config cfg;
    char outDir[1024] = {0};
    char exeName[128] = {0};
    char title[128] = {0};
    char icon[1024] = {0};
    std::unique_ptr<Job> job;
    std::vector<std::string> log;
    std::string status;            // last outcome, shown under the log
    bool lastOk = false;
};
State g;

void SyncFieldsFromConfig() {
    std::snprintf(g.outDir, sizeof g.outDir, "%s", g.cfg.outputDir.c_str());
    std::snprintf(g.exeName, sizeof g.exeName, "%s", g.cfg.exeName.c_str());
    std::snprintf(g.title, sizeof g.title, "%s", g.cfg.windowTitle.c_str());
    std::snprintf(g.icon, sizeof g.icon, "%s", g.cfg.iconPath.c_str());
}

void SyncConfigFromFields() {
    g.cfg.outputDir = g.outDir;
    g.cfg.exeName = g.exeName;
    g.cfg.windowTitle = g.title;
    g.cfg.iconPath = g.icon;
}

void AddLog(const std::string& line) {
    g.log.push_back(line);
    if (g.log.size() > 400) g.log.erase(g.log.begin(), g.log.begin() + 100);
    LogAlways("[Export] %s", line.c_str());
}

// Moves what the worker wrote to the dialog's log and finishes the job when it is over.
void PumpJob() {
    if (!g.job) return;
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(g.job->logMutex);
        lines.swap(g.job->pendingLog);
    }
    for (const std::string& l : lines) AddLog(l);
    if (g.job->done.load()) {
        if (g.job->worker.joinable()) g.job->worker.join();
        g.lastOk = g.job->result.ok;
        g.status = g.job->result.ok ? "Exported to " + g.job->result.outputPath
                                    : "Export failed: " + g.job->result.error;
        g.job.reset();
    }
}

void PumpFolderDialog() {
    std::string picked;
    if (platform::PollDialogResult({ platform::DialogPurpose::ExportFolder }, picked) && !picked.empty()) {
        std::snprintf(g.outDir, sizeof g.outDir, "%s", picked.c_str());
    }
}

void PumpIconDialog() {
    std::string picked;
    if (platform::PollDialogResult({ platform::DialogPurpose::ExportIcon }, picked) && !picked.empty()) {
        std::snprintf(g.icon, sizeof g.icon, "%s", picked.c_str());
    }
}

} // namespace

void OpenExportDialog() { g.openRequested = true; }
bool IsExporting() { return g.job != nullptr; }

bool ConsumeExportRequest() {
    const bool requested = g.exportRequested;
    g.exportRequested = false;
    return requested;
}

void StartExport(const std::string& projectDir) {
    if (g.job) return;
    SyncConfigFromFields();
    exporter::SaveConfig(projectDir, g.cfg);
    g.log.clear();
    g.status.clear();

    g.job = std::make_unique<Job>();
    Job* job = g.job.get();
    const exporter::Config cfg = g.cfg;
    job->worker = std::thread([job, cfg, projectDir]() {
        job->result = exporter::Run(projectDir, cfg,
            [job](const std::string& line) {
                std::lock_guard<std::mutex> lock(job->logMutex);
                job->pendingLog.push_back(line);
            },
            [job](float f) { job->progress.store(f); return !job->cancel.load(); });
        job->done.store(true);
    });
}

void DrawExportDialog() {
    PumpJob();
    PumpFolderDialog();
    PumpIconDialog();

    if (g.openRequested) {
        g.openRequested = false;
        const project::Info& current = project::GetCurrentProject();
        if (!g.configLoaded || g.loadedForProject != current.path) {
            g.cfg = exporter::LoadConfig(current.path, current.name);
            SyncFieldsFromConfig();
            g.configLoaded = true;
            g.loadedForProject = current.path;
            g.log.clear();
            g.status.clear();
        }
        ImGui::OpenPopup(kPopupId);
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::BeginPopupModal(kPopupId, &open, ImGuiWindowFlags_AlwaysAutoResize)) return;

    const bool busy = g.job != nullptr;
    const project::Info& current = project::GetCurrentProject();
    ImGui::TextWrapped("Packs '%s' into one executable: the player, your scenes, models and textures. Your project is saved first.",
                       current.name.c_str());
    ImGui::Separator();

    if (busy) ImGui::BeginDisabled();

    // Target
    const std::vector<exporter::Target> targets = exporter::Targets();
    std::string currentLabel = "(choose)";
    for (const auto& t : targets) if (t.id == g.cfg.target) currentLabel = t.label;
    if (ImGui::BeginCombo("Target", currentLabel.c_str())) {
        for (const auto& t : targets) {
            const bool selected = t.id == g.cfg.target;
            if (!t.available) ImGui::BeginDisabled();
            if (ImGui::Selectable(t.label.c_str(), selected)) g.cfg.target = t.id;
            if (!t.available) {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", t.reason.c_str());
            }
        }
        ImGui::EndCombo();
    }

    // Output folder
    ImGui::SetNextItemWidth(380.0f);
    ImGui::InputText("##outdir", g.outDir, sizeof g.outDir);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        platform::BeginChooseFolderDialog(platform::DialogPurpose::ExportFolder, "Export to", g.outDir);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Output folder");

    ImGui::InputText("File name", g.exeName, sizeof g.exeName);
    ImGui::InputText("Window title", g.title, sizeof g.title);
    ImGui::SetNextItemWidth(380.0f);
    ImGui::InputText("##icon", g.icon, sizeof g.icon);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##icon")) {
        platform::BeginOpenFileDialog(platform::DialogPurpose::ExportIcon, "Game icon", current.path,
                                      { { "PNG image", "*.png" } });
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) g.icon[0] = '\0';
    ImGui::SameLine();
    ImGui::TextUnformatted("Icon");
    ImGui::TextDisabled("Square PNG, 256x256 or larger. Empty = default icon.");
    ImGui::Checkbox("Also write .desktop + .png launcher files (Linux)", &g.cfg.linuxLauncher);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Linux executables cannot hold an icon. Off = a single file; the window icon still works.");
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputInt("##w", &g.cfg.windowWidth, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputInt("Window size##h", &g.cfg.windowHeight, 0, 0);
    g.cfg.windowWidth = std::clamp(g.cfg.windowWidth, 320, 7680);
    g.cfg.windowHeight = std::clamp(g.cfg.windowHeight, 240, 4320);
    ImGui::Checkbox("Start fullscreen", &g.cfg.fullscreen);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragInt("FPS cap", &g.cfg.fpsCap, 1.0f, 0, 1000, g.cfg.fpsCap == 0 ? "Unlimited" : "%d");
    g.cfg.fpsCap = std::clamp(g.cfg.fpsCap, 0, 1000);
    ImGui::SameLine();
    ImGui::TextDisabled("0 = unlimited");

    if (busy) ImGui::EndDisabled();

    ImGui::Separator();
    if (busy) {
        ImGui::ProgressBar(g.job->progress.load(), ImVec2(-1.0f, 0.0f));
    }
    if (!g.log.empty()) {
        ImGui::BeginChild("##exportlog", ImVec2(0.0f, 130.0f), true);
        for (const std::string& line : g.log) ImGui::TextUnformatted(line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    if (!g.status.empty()) {
        ImGui::TextColored(g.lastOk ? ImVec4(0.4f, 0.9f, 0.5f, 1.0f) : ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", g.status.c_str());
        if (g.lastOk) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Show in folder")) platform::RevealInFileManager(g.cfg.outputDir);
        }
    }

    const bool canExport = !busy && g.outDir[0] != '\0' && !current.path.empty();
    if (!canExport) ImGui::BeginDisabled();
    if (ImGui::Button("Export", ImVec2(120.0f, 0.0f))) {
        SyncConfigFromFields();
        g.status.clear();
        g.exportRequested = true;   // ObjectInteractionManager saves the project, then calls StartExport()
    }
    if (!canExport) ImGui::EndDisabled();
    ImGui::SameLine();
    if (busy) {
        if (ImGui::Button("Cancel export", ImVec2(120.0f, 0.0f))) g.job->cancel.store(true);
    } else if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) {
        SyncConfigFromFields();
        ImGui::CloseCurrentPopup();
    }
    if (!open && !busy) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace ui
