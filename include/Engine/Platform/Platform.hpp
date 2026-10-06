// Platform.hpp -- thin cross-platform shims for the handful of things the
// engine used to reach for Win32 directly.
//
// The dialogs here are deliberately NON-BLOCKING. They are started from
// ImGui button handlers, i.e. from inside the middle of a frame, so blocking on
// a subprocess would freeze the render loop and mark the window "not
// responding". Instead a request is started with Begin*Dialog() (which returns
// immediately) and the answer is collected later with PollDialogResult() from
// the per-frame update.
#pragma once

#include <string>
#include <vector>

// Export/import decoration for the FlyNative_* C ABI (FlyScriptApi.cpp).
// On Windows the managed side resolves these through
// NativeLibrary.GetMainProgramHandle(); on Linux that is dlopen(NULL), which
// only sees symbols the executable exported (see ENABLE_EXPORTS in
// CMakeLists.txt).
#if defined(_WIN32)
    #if defined(FLYENGINE_CSHARP_EXPORTS)
        #define FLY_API __declspec(dllexport)
    #else
        #define FLY_API __declspec(dllimport)
    #endif
#else
    #define FLY_API __attribute__((visibility("default")))
#endif

// Structured-exception guard for the FlyNative_* C ABI.
//
// On MSVC these expand to a real __try/__except pair, so a malformed or stale
// handle from managed code returns a fallback value instead of taking the whole
// editor down. GCC and Clang have no __try, and cannot mix one with C++
// unwinding, so there the guard degrades to a bare scope: the impl::*_Impl
// functions already null-check the bound runtime and every handle, which is
// what makes the guard a safety net rather than the actual validation.
//
//   FLY_TRY { do_work(); } FLY_CATCH(/* nothing */)
//
// The trailing FLY_CATCH() is required so both branches close cleanly.
#if defined(_MSC_VER)
    #define FLY_TRY  __try {
    #define FLY_CATCH(fallback) } __except (EXCEPTION_EXECUTE_HANDLER) { fallback; }
#else
    #define FLY_TRY  {
    #define FLY_CATCH(fallback) }
#endif

namespace platform {

// One entry in a file dialog's filter list, e.g. {"3D Models", "*.obj;*.fbx"}.
struct FileFilter {
    std::string label;
    std::string extensions;
};

// --- Native dialogs (non-blocking) ----------------------------------------

// Tags who owns an in-flight dialog. Several subsystems (the scene menu in
// ObjectInteractionManager, the asset browser in ui) each poll once per frame;
// the purpose tag makes sure only the owner consumes the result.
enum class DialogPurpose {
    None,
    OpenScene,
    SaveScene,
    SaveSceneAs,
    ImportModel,
    ImportTexture,
    ImportMesh,
    ChooseProjectFolder,
    ChooseFolder,
};

// Only one dialog can be in flight at a time; the editor UI is modal anyway.
// Each Begin* returns true if it actually started a request. If a dialog is
// already open, or a helper binary is missing and a text prompt is already up,
// they return false and do nothing. Pass the purpose you will poll for.

// `filters` may be empty for "all files".
bool BeginOpenFileDialog(DialogPurpose purpose,
                         const std::string& title,
                         const std::string& startDir,
                         const std::vector<FileFilter>& filters);

bool BeginSaveFileDialog(DialogPurpose purpose,
                         const std::string& title,
                         const std::string& startDir,
                         const std::vector<FileFilter>& filters,
                         const std::string& defaultName);

bool BeginChooseFolderDialog(DialogPurpose purpose,
                             const std::string& title,
                             const std::string& startDir);

// True while a dialog is open (helper subprocess running, or text prompt up).
bool DialogPending();

// Call once per frame. Returns true exactly once per completed request, with
// the chosen path in `outPath`. An empty `outPath` means the user cancelled.
//
// Passing a purpose list lets several subsystems poll every frame: the result
// is only handed to the caller whose purposes include the pending dialog's
// owner, and is left for someone else otherwise.
bool PollDialogResult(const std::vector<DialogPurpose>& purposes, std::string& outPath);

// The purpose of the dialog currently in flight (None if there is none).
DialogPurpose PendingDialogPurpose();

// --- Text-prompt fallback -------------------------------------------------
// Used when neither zenity nor kdialog is installed. Platform owns the state
// (so it stays free of ImGui); the editor draws it and writes the answer back.

struct PathPrompt {
    bool active = false;
    bool directoryOnly = false;
    std::string title;
    char buffer[1024] = {0};
    bool accept = false;   // set by the UI when the user confirms
    bool cancel = false;   // set by the UI when the user dismisses
};

PathPrompt& GetPathPrompt();

// --- Process execution ----------------------------------------------------

struct ProcessResult {
    bool launched = false;   // false if the executable could not be started
    int exitCode = -1;
    std::string output;      // stdout and stderr, interleaved
    std::string error;       // why it could not be launched
};

// Runs `exe` with `args` (argv[0] is supplied for you) and captures its
// combined output. Returns after the process exits or `timeoutMs` elapses.
// BLOCKING -- fine for one-shot work like `dotnet build`, not for dialogs.
ProcessResult RunProcessCapture(const std::string& exe,
                                const std::vector<std::string>& args,
                                int timeoutMs);

// Starts `exe` and returns immediately without waiting for it or capturing its
// output -- the ShellExecuteW replacement for handing a file to an external
// application. The child's stdio is detached from ours so it cannot write over
// the editor's console. Returns false if the executable could not be started.
bool LaunchDetached(const std::string& exe, const std::vector<std::string>& args);

// --- Well-known locations -------------------------------------------------

std::string ExecutablePath();   // GetModuleFileNameW / readlink(/proc/self/exe)
std::string UserHomeDir();      // %USERPROFILE% / $HOME
std::string ConfigDir();        // %APPDATA%    / $XDG_CONFIG_HOME or ~/.config
std::string DocumentsDir();     // %USERPROFILE%\Documents / xdg-user-dir DOCUMENTS

// Candidate UI font paths in preference order. The first that exists wins.
// Replaces the hardcoded "C:/Windows/Fonts/arial.ttf" in several places.
std::vector<std::string> FontCandidates();

// Returns the first font path that actually exists, or an empty string if
// none do. A font dropped in the working directory (or one level up) wins over
// the system candidates, so an in-tree build can be tweaked without touching
// installed fonts.
//
// This is the function call sites want: every one of them used to open with
// its own "arial.ttf", then "../arial.ttf", then loop FontCandidates()
// probing FileExists -- four copies of the same ordering, which had already
// drifted (Benchmark.cpp probed "../arial.ttf" where the others did not).
// The result is cached, so calling it per frame is free.
const std::string& ResolveFontPath();

// True if a UI font from FontCandidates() can actually be loaded.
bool HasUsableFont();

// --- Locating the engine's own data ---------------------------------------
//
// assets/ and shaders/ used to be found by probing a hand-written list of
// relative paths ("assets/x.png", "../assets/x.png", ...). That only ever
// worked when the process happened to start in a directory inside the source
// tree. An installed engine starts from /usr/bin with no useful working
// directory at all, and a .desktop launcher is usually worse.
//
// Everything now goes through the two resolvers below. Callers keep writing
// paths the way they appear in the repository ("assets/EditorIcons/cube.png")
// and hand the result straight to raylib.

// Returns an existing path for `repoRelative` -- a path relative to the
// engine's data root, i.e. the repository root in a dev tree and the install
// prefix's share/flyengine in a packaged build. Returns an empty string when
// the file is not present under any search root, which callers pass on to
// raylib unchanged so the usual "FILEIO: failed to open" warning still fires.
std::string ResolveAsset(const std::string& repoRelative);

// Same as ResolveAsset but takes a bare shader file name ("terrain.vert").
// CMake flattens the shaders next to the executable in a dev build, while an
// install keeps them under share/flyengine/shaders/, so both layouts are
// tried.
std::string ResolveShader(const std::string& fileName);

// The roots searched by ResolveAsset/ResolveShader, in priority order, with
// duplicates removed. Exposed for the "could not find the engine data"
// diagnostic and for tests.
std::vector<std::string> DataSearchRoots();

// Picks the first search root that actually contains engine data, so a single
// warning can name the one place the data was expected. Empty if none do.
std::string FoundDataRoot();

// --- Misc -----------------------------------------------------------------

// Opens the system file manager with `path` selected (or its parent folder on
// platforms that cannot preselect a file). No-op if no handler is available.
void RevealInFileManager(const std::string& path);

// Opens `path` with the desktop's default handler.
void OpenWithDefaultApp(const std::string& path);

// Recursively deletes `path`. Replaces SHFileOperationA/FO_DELETE.
bool RemoveTree(const std::string& path);

} // namespace platform
