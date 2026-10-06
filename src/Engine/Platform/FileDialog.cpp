// FileDialog.cpp -- open/save/folder pickers.
//
// Windows used GetOpenFileNameA and SHBrowseForFolderA, both of which are
// modal-and-blocking (which was fine, because the OS owns the modal loop). On
// Linux the equivalents are external programs, so we cannot block: we launch
// the helper on a worker thread and let the frame loop keep running, exactly
// like a native async file dialog.
//
// Helper preference: zenity, then kdialog. If neither is installed we fall back
// to an in-editor text prompt (state owned by Platform, drawn by ui.cpp).
//
// All three public entry points live behind this one file, so replacing this
// with a real ImGui browser later is a self-contained change.

#include "Engine/Platform/Platform.hpp"
#include "Engine/Platform/PlatformInternal.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <thread>

namespace platform {
namespace {

namespace fs = std::filesystem;

bool IsDirectory(const std::string& p) {
    if (p.empty()) return false;
    std::error_code ec;
    return fs::is_directory(fs::u8path(p), ec);
}

// A dialog in flight. Exactly one of these exists at a time.
struct Pending {
    std::thread worker;
    std::atomic<bool> done{false};
    std::string output;
    int exitCode = -1;
    bool launched = false;
    bool promptMode = false;   // using the text prompt instead of a helper
    DialogPurpose purpose = DialogPurpose::None;
};

std::unique_ptr<Pending> g_pending;

enum class Kind { Open, Save, Folder };

// --- Argument construction ------------------------------------------------

// zenity: one --file-filter=Label|*.obj *.fbx per entry, spaces separating them.
std::vector<std::string> ZenityFilterArgs(const std::vector<FileFilter>& filters) {
    std::vector<std::string> args;
    for (const FileFilter& f : filters) {
        if (f.extensions.empty()) continue;
        std::string exts = f.extensions;
        std::replace(exts.begin(), exts.end(), ';', ' ');
        args.push_back("--file-filter=" + f.label + "|" + exts);
    }
    if (args.empty()) args.push_back("--file-filter=All files|*");
    return args;
}

// kdialog: "Label|*.obj *.fbx;;Label2|*.*" as a single argument.
std::string KDialogFilterArg(const std::vector<FileFilter>& filters) {
    std::string out;
    for (const FileFilter& f : filters) {
        if (f.extensions.empty()) continue;
        std::string exts = f.extensions;
        std::replace(exts.begin(), exts.end(), ';', ' ');
        if (!out.empty()) out += ";;";
        out += f.label + "|" + exts;
    }
    if (out.empty()) out = "All files|*";
    return out;
}

std::string DirOrDot(const std::string& d) { return IsDirectory(d) ? d : std::string("."); }

// --- Dispatch -------------------------------------------------------------

bool Start(DialogPurpose purpose, Kind kind, const std::string& title,
           const std::string& startDir, const std::vector<FileFilter>& filters,
           const std::string& defaultName) {
    if (g_pending) return false;   // a dialog is already up

    auto p = std::make_unique<Pending>();
    p->purpose = purpose;

    // Prefer zenity.
    if (internal::HelperAvailable("zenity")) {
        std::vector<std::string> args{"--file-selection", "--title=" + title};
        if (kind == Kind::Save) {
            args.push_back("--save");
            args.push_back("--confirm-overwrite");
        } else if (kind == Kind::Folder) {
            args.push_back("--directory");
        }
        args.push_back("--start-directory=" + DirOrDot(startDir));
        if (kind == Kind::Save && !defaultName.empty()) args.push_back("--filename=" + defaultName);
        if (kind != Kind::Folder) {
            for (const std::string& a : ZenityFilterArgs(filters)) args.push_back(a);
        }
        Pending* raw = p.get();
        p->worker = std::thread([raw, args]() {
            ProcessResult r = RunProcessCapture("zenity", args, 300000);
            raw->launched = r.launched;
            raw->exitCode = r.exitCode;
            raw->output = internal::TrimNewline(r.output);
            raw->done.store(true);
        });
        g_pending = std::move(p);
        return true;
    }

    if (internal::HelperAvailable("kdialog")) {
        std::vector<std::string> args;
        switch (kind) {
            case Kind::Open:   args = {"--getopenfilename", DirOrDot(startDir), KDialogFilterArg(filters)}; break;
            case Kind::Save:   args = {"--getsavefilename", DirOrDot(startDir), KDialogFilterArg(filters),
                                       defaultName.empty() ? std::string() : defaultName}; break;
            case Kind::Folder: args = {"--getexistingdirectory", DirOrDot(startDir)}; break;
        }
        Pending* raw = p.get();
        p->worker = std::thread([raw, args]() {
            ProcessResult r = RunProcessCapture("kdialog", args, 300000);
            raw->launched = r.launched;
            raw->exitCode = r.exitCode;
            raw->output = internal::TrimNewline(r.output);
            raw->done.store(true);
        });
        g_pending = std::move(p);
        return true;
    }

    // No helper available: ask the user to type the path. The editor draws this.
    PathPrompt& prompt = GetPathPrompt();
    if (prompt.active) return false;
    prompt.active = true;
    prompt.directoryOnly = (kind == Kind::Folder);
    prompt.title = title;
    prompt.accept = false;
    prompt.cancel = false;
    prompt.buffer[0] = '\0';
    if (!defaultName.empty()) {
        std::snprintf(prompt.buffer, sizeof(prompt.buffer), "%s", defaultName.c_str());
    }
    p->promptMode = true;
    p->done.store(true);
    g_pending = std::move(p);
    return true;
}

} // namespace

bool BeginOpenFileDialog(DialogPurpose purpose, const std::string& title,
                         const std::string& startDir, const std::vector<FileFilter>& filters) {
    Start(purpose, Kind::Open, title, startDir, filters, std::string());
    return true;
}

bool BeginSaveFileDialog(DialogPurpose purpose, const std::string& title,
                         const std::string& startDir, const std::vector<FileFilter>& filters,
                         const std::string& defaultName) {
    Start(purpose, Kind::Save, title, startDir, filters, defaultName);
    return true;
}

bool BeginChooseFolderDialog(DialogPurpose purpose, const std::string& title,
                             const std::string& startDir) {
    Start(purpose, Kind::Folder, title, startDir, std::vector<FileFilter>(), std::string());
    return true;
}

bool DialogPending() {
    if (!g_pending) return false;
    if (g_pending->promptMode) {
        const PathPrompt& p = GetPathPrompt();
        return p.active && !p.accept && !p.cancel;
    }
    return !g_pending->done.load();
}

DialogPurpose PendingDialogPurpose() {
    return g_pending ? g_pending->purpose : DialogPurpose::None;
}

bool PollDialogResult(const std::vector<DialogPurpose>& purposes, std::string& outPath) {
    outPath.clear();
    if (!g_pending) return false;

    // Another subsystem owns this dialog; leave the result for it.
    if (std::find(purposes.begin(), purposes.end(), g_pending->purpose) == purposes.end()) {
        return false;
    }

    if (g_pending->promptMode) {
        PathPrompt& p = GetPathPrompt();
        if (!p.active) return false;
        if (p.cancel) {
            p.active = false;
            g_pending.reset();
            return true;   // completed, but cancelled
        }
        if (!p.accept) return false;
        p.active = false;
        outPath = p.buffer;
        g_pending.reset();
        return true;
    }

    if (!g_pending->done.load()) return false;

    // Helper finished. A non-zero exit means the user cancelled.
    if (g_pending->worker.joinable()) g_pending->worker.join();
    if (g_pending->launched && g_pending->exitCode == 0) outPath = g_pending->output;
    g_pending.reset();
    return true;
}

} // namespace platform
