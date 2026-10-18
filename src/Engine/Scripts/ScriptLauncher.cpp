#include "../../../include/Engine/Scripts/ScriptLauncher.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine/Frontend/ProjectManager.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Backend/VisualScript.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"

#include "imgui.h"
#include "raylib.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include "Engine/U8Path.hpp"

namespace fs = std::filesystem;

namespace scriptLauncher {

// Builds a valid C++ identifier (class name) from an arbitrary object/script
// name: non-identifier characters become underscores, and a leading digit is
// prefixed so the class still compiles. Falls back to "Script".
std::string MakeClassName(const std::string& name) {
    std::string cls;
    cls.reserve(name.size());
    for (char c : name) {
        bool isAsciiAlnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9');
        cls.push_back(isAsciiAlnum || c == '_' ? c : '_');
    }
    if (cls.empty() || (cls[0] >= '0' && cls[0] <= '9')) cls.insert(cls.begin(), '_');
    return cls.empty() ? std::string("Script") : cls;
}

namespace {

enum class EditorKind { None, VSCode, VisualStudio, Rider, Notepad, CLion, QtCreator, CodeBlocks };
constexpr const char* kScriptDirName = "Scripts";

// Builds the source of a C++ script stub with the given class name. The class
// derives from fly::Script and is registered with FLY_SCRIPT so the native
// script host can instantiate it by name.
std::string MakeScriptStub(const std::string& className) {
    std::string body;
    body += "#include \"fly.hpp\"\n";
    body += "\n";
    body += "struct " + className + " : fly::Script {\n";
    body += "    fly::Task Run() override {\n";
    body += "        // Runs while the game is playing; each co_await waits a frame.\n";
    body += "        // Use Self() for the attached object, e.g.:\n";
    body += "        //   fly::Object self = fly::Object::Self();\n";
    body += "        //   self.SetPosition({0, 2, 0});\n";
    body += "        while (true) {\n";
    body += "            co_await fly::NextFrame();\n";
    body += "        }\n";
    body += "    }\n";
    body += "};\n";
    body += "FLY_SCRIPT(" + className + ")\n";
    return body;
}

// Removes characters that are illegal in Windows filenames from a script/object
// name so it can be used as a file stem. Falls back to "Script" if empty.
std::string SanitizeStem(const std::string& name) {
    std::string stem;
    stem.reserve(name.size());
    for (char c : name) {
        switch (c) {
            case '<': case '>': case ':': case '"':
            case '/': case '\\': case '|': case '?': case '*':
                stem.push_back('_');
                break;
            default:
                stem.push_back(c);
        }
    }
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) stem.pop_back();
    if (stem.empty() || stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") {
        stem = "Script";
    }
    return stem;
}

std::string ProjectFolder() {
    return project::GetCurrentProject().path;
}

// --- Editor detection ------------------------------------------------------

bool FileIsExecutable(const std::string& path) {
    return fs::exists(path) && fs::is_regular_file(path);
}

std::string FindKnownExe(const std::vector<std::string>& candidates) {
    for (const auto& c : candidates) {
        if (FileIsExecutable(c)) return c;
    }
    return {};
}

using platform::FindOnPath;

std::string FindVSCode() {
    // The official Linux packages (apt/deb, rpm, snap, AUR, tarball) all
    // install a `code` (or `code-insiders`) launcher on PATH.
    if (const std::string onPath = FindOnPath({"code-insiders", "code"}); !onPath.empty()) {
        return onPath;
    }
#if defined(_WIN32)
    const char* localAppData = std::getenv("LOCALAPPDATA");
    const char* programFiles = std::getenv("ProgramFiles");
    std::vector<std::string> candidates;
    if (localAppData) {
        candidates.push_back(std::string(localAppData) + "\\Programs\\Microsoft VS Code\\Code.exe");
        candidates.push_back(std::string(localAppData) + "\\Programs\\Microsoft VS Code Insiders\\Code - Insiders.exe");
    }
    if (programFiles) {
        candidates.push_back(std::string(programFiles) + "\\Microsoft VS Code\\Code.exe");
        candidates.push_back(std::string(programFiles) + "\\Microsoft VS Code Insiders\\Code - Insiders.exe");
    }
    return FindKnownExe(candidates);
#else
    // Snap and some distro packages install outside PATH when the app is
    // confined; these are the conventional locations.
    return FindKnownExe({
        "/snap/bin/code",
        "/var/lib/flatpak/exports/bin/com.visualstudio.code",
        "/usr/lib/code/code",
        "/opt/visual-studio-code/code",
    });
#endif
}

std::string RunCommandCaptureOutput(const std::string& exe,
                                    const std::vector<std::string>& args,
                                    size_t capacity) {
    const platform::ProcessResult r = platform::RunProcessCapture(exe, args, 10000);
    if (!r.launched) return {};
    std::string result = r.output;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' ||
                               result.back() == ' ')) {
        result.pop_back();
    }
    if (result.size() >= capacity) result.resize(capacity - 1);
    return result;
}

std::string FindVisualStudio() {
    // Windows only. There is no Linux build of Visual Studio, so the picker
    // does not offer it (see AvailableEditorKinds).
#if defined(_WIN32)
    const std::string vswhere =
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    if (!FileIsExecutable(vswhere)) return {};
    std::string installPath = RunCommandCaptureOutput(
        vswhere, {"-latest", "-property", "installationPath"}, 4096);
    if (installPath.empty()) return {};
    const std::string devenv = (fs::path(installPath) / "Common7" / "IDE" / "devenv.exe").string();
    return FileIsExecutable(devenv) ? devenv : std::string{};
#else
    return {};
#endif
}

#if !defined(_WIN32)
// <product>.sh in `dir`/bin, or in a folder up to `depth` levels below it (Toolbox: apps/<product>/<channel>/<build>/).
std::string FindLauncherUnder(const fs::path& dir, const std::string& product, int depth) {
    for (const std::string& launcher : {"bin/" + product + ".sh", product + ".sh"}) {
        const fs::path candidate = dir / launcher;
        if (FileIsExecutable(candidate.string())) return candidate.string();
    }
    if (depth <= 0) return {};
    std::error_code ec;
    for (const auto& child : fs::directory_iterator(dir, ec)) {
        const std::string name = child.path().filename().string();
        if (!child.is_directory(ec) || name == "bin" || name == "jbr" || name == "lib" || name == "plugins") continue;
        const std::string found = FindLauncherUnder(child.path(), product, depth - 1);
        if (!found.empty()) return found;
    }
    return {};
}
#endif

// A JetBrains IDE ("rider", "clion"): the launcher on PATH (the Toolbox's generated scripts live there too), else a scan
// of the conventional install roots.
std::string FindJetBrainsIde(const std::string& product) {
    if (const std::string onPath = FindOnPath({product}); !onPath.empty()) {
        return onPath;
    }

#if defined(_WIN32)
    const char* programFiles = std::getenv("ProgramFiles");
    if (!programFiles) return {};
    const std::vector<std::string> roots = {
        std::string(programFiles) + "\\JetBrains",
        std::string(programFiles) + " (x86)\\JetBrains",
        "C:\\JetBrains",
    };
    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        std::string best;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (!entry.is_directory(ec)) continue;
            std::string candidate = (entry.path() / "bin" / (product + "64.exe")).string();
            if (FileIsExecutable(candidate)) best = candidate;
        }
        if (!best.empty()) return best;
    }
    return {};
#else
    // Toolbox installs land in ~/.local/share/JetBrains/Toolbox/apps/<product>/[channel/[build/]];
    // tarballs in /opt/JetBrains/*/ or ~/opt/JetBrains/*/, and distro packages in /opt/<product>.
    std::vector<fs::path> roots;
    const std::string home = platform::UserHomeDir();
    if (!home.empty()) {
        roots.emplace_back(U8Path(home) / ".local" / "share" / "JetBrains" / "Toolbox" / "apps");
        roots.emplace_back(U8Path(home) / "opt" / "JetBrains");
    }
    roots.emplace_back("/opt/JetBrains");
    roots.emplace_back("/usr/local/JetBrains");
    roots.emplace_back("/opt");

    for (const fs::path& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (!entry.is_directory(ec)) continue;
            // /opt holds everything: only look into folders named for the product.
            std::string folder = entry.path().filename().string();
            std::transform(folder.begin(), folder.end(), folder.begin(), [](unsigned char c) { return std::tolower(c); });
            if (root == "/opt" && folder.find(product) == std::string::npos) continue;
            const std::string found = FindLauncherUnder(entry.path(), product, 3);
            if (!found.empty()) return found;
        }
    }
    return {};
#endif
}

std::string FindRider() { return FindJetBrainsIde("rider"); }
std::string FindCLion() { return FindJetBrainsIde("clion"); }

// Other C/C++ IDEs: the launcher on PATH, or the usual install spot.
std::string FindQtCreator() {
    if (const std::string onPath = FindOnPath({"qtcreator"}); !onPath.empty()) return onPath;
#if defined(_WIN32)
    const char* programFiles = std::getenv("ProgramFiles");
    if (programFiles) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(fs::path(programFiles) / "Qt", ec)) {
            for (const auto& v : fs::directory_iterator(e.path(), ec)) {
                const std::string c = (v.path() / "bin" / "qtcreator.exe").string();
                if (FileIsExecutable(c)) return c;
            }
        }
    }
    return {};
#else
    return FindKnownExe({"/usr/bin/qtcreator", "/opt/Qt/Tools/QtCreator/bin/qtcreator", "/var/lib/flatpak/exports/bin/org.qt_project.qtcreator"});
#endif
}

std::string FindCodeBlocks() {
    if (const std::string onPath = FindOnPath({"codeblocks"}); !onPath.empty()) return onPath;
#if defined(_WIN32)
    const char* programFiles = std::getenv("ProgramFiles");
    return programFiles ? FindKnownExe({std::string(programFiles) + "\\CodeBlocks\\codeblocks.exe"}) : std::string{};
#else
    return FindKnownExe({"/usr/bin/codeblocks"});
#endif
}

// The plain text editor behind EditorKind::Notepad. Notepad is a Windows
// program; on Linux the equivalent role is played by whichever simple text
// editor the desktop already ships.
std::string FindTextEditor() {
    if (const std::string onPath =
            FindOnPath({"mousepad", "gedit", "xed", "kate", "pluma", "leafpad"});
        !onPath.empty()) {
        return onPath;
    }
#if defined(_WIN32)
    return "notepad.exe";
#else
    return "notepad";   // resolved on PATH by the desktop's own compat shim
#endif
}

// Resolve an editor kind to an executable path. Empty indicates the OS default
// association (or Notepad, which requires no path).
std::string ResolveEditorPath(EditorKind kind) {
    switch (kind) {
        case EditorKind::VSCode:        return FindVSCode();
        case EditorKind::VisualStudio:  return FindVisualStudio();
        case EditorKind::Rider:         return FindRider();
        case EditorKind::CLion:         return FindCLion();
        case EditorKind::QtCreator:     return FindQtCreator();
        case EditorKind::CodeBlocks:    return FindCodeBlocks();
        case EditorKind::Notepad:       return FindTextEditor();
        default:                        return {};   // Auto-Detect
    }
}

bool ShellOpen(const std::string& exe, const std::vector<std::string>& args) {
    return platform::LaunchDetached(exe, args);
}

// Actually launch an editor on `file`, at `line`:`column` (1-based) when the
// editor supports it; line 0 just opens the file.
bool LaunchEditor(EditorKind kind, const std::string& exePath, const std::string& file,
                  int line = 0, int column = 0) {
    if (file.empty()) return false;

    if (kind == EditorKind::VSCode ||
        kind == EditorKind::Rider ||
        kind == EditorKind::CLion ||
        kind == EditorKind::QtCreator ||
        kind == EditorKind::CodeBlocks ||
        kind == EditorKind::VisualStudio ||
        kind == EditorKind::Notepad) {
        std::vector<std::string> args;
        if (kind == EditorKind::VSCode && line > 0) {
            // code -g <file>:<line>:<column>
            args.push_back("-g");
            args.push_back(file + ":" + std::to_string(line) + ":" + std::to_string(std::max(column, 1)));
        } else if (kind == EditorKind::QtCreator && line > 0) {
            // qtcreator <file>:<line>:<column>
            args.push_back(file + ":" + std::to_string(line) + ":" + std::to_string(std::max(column, 1)));
        } else if ((kind == EditorKind::Rider || kind == EditorKind::CLion) && line > 0) {
            // rider / clion --line <n> --column <n> <file>
            args.push_back("--line");
            args.push_back(std::to_string(line));
            if (column > 0) {
                args.push_back("--column");
                args.push_back(std::to_string(column));
            }
            args.push_back(file);
        } else {
            // Visual Studio needs /edit to open an existing file rather than
            // a new project, and has no line argument.
            if (kind == EditorKind::VisualStudio) args.push_back("/edit");
            args.push_back(file);
        }
        return ShellOpen(exePath, args);
    }

    // Auto / default: hand the file to the desktop's own handler. xdg-open
    // (ShellExecuteW's counterpart) picks the right .cpp association, or falls
    // back to a generic text editor for unknown types. It cannot take a line.
    platform::OpenWithDefaultApp(file);
    return true;
}

// --- Picker state ----------------------------------------------------------

std::string g_pendingFile;          // file queued to open behind the picker
int         g_pendingLine = 0;      // 1-based position to jump to (0 = none)
int         g_pendingColumn = 0;
bool        g_editorSet = false;    // a remembered editor is available
EditorKind  g_editorKind = EditorKind::None;
std::string g_editorPath;           // cached resolved exe (empty for notepad/default)

// Which editors are installed, looked up when the picker opens (the scans touch the disk, so not every frame).
std::map<EditorKind, std::string> g_detected;
bool g_detectedValid = false;

bool EditorInstalled(EditorKind kind) {
    if (kind == EditorKind::None || kind == EditorKind::Notepad) return true;   // nothing to find
    if (!g_detectedValid) { g_detected.clear(); g_detectedValid = true; }
    auto it = g_detected.find(kind);
    if (it == g_detected.end()) it = g_detected.emplace(kind, ResolveEditorPath(kind)).first;
    return !it->second.empty();
}

const char* EditorKindLabel(EditorKind k) {
    switch (k) {
        case EditorKind::VSCode:       return "VS Code";
        case EditorKind::VisualStudio: return "Visual Studio";
        case EditorKind::Rider:        return "Rider";
        case EditorKind::CLion:        return "CLion";
        case EditorKind::QtCreator:    return "Qt Creator";
        case EditorKind::CodeBlocks:   return "Code::Blocks";
        case EditorKind::Notepad:      return "Notepad";
        default:                       return "Auto-Detect";
    }
}

// --- IDE picker icons (black silhouette -> white for the dark theme) --------

struct IdeIcons {
    Texture2D rider;
    Texture2D clion;
    Texture2D qtCreator;
    Texture2D codeBlocks;
    Texture2D vscode;
    Texture2D visualStudio;
    Texture2D notepad;
};

Texture2D LoadIconOrNull(const char* fileName) {
    const std::string path =
        platform::ResolveAsset(std::string("assets/EditorIcons/ideicons/") + fileName);
    Image img = LoadImage(path.c_str());
    Texture2D tex{};
    if (img.data != nullptr) {
        // Brand icons keep their original colors; do not whiten.
        tex = LoadTextureFromImage(img);
        UnloadImage(img);
    }
    return tex;
}

const IdeIcons& GetIdeIcons() {
    static IdeIcons icons = [] {
        IdeIcons v{};
        v.rider          = LoadIconOrNull("rider.png");
        v.clion          = LoadIconOrNull("clion.png");
        v.qtCreator      = LoadIconOrNull("qtc.png");
        v.codeBlocks     = LoadIconOrNull("codeblocks.png");
        v.vscode         = LoadIconOrNull("vscode.png");
        v.visualStudio   = LoadIconOrNull("vstudio.png");
        v.notepad        = LoadIconOrNull("npad.png");
        return v;
    }();
    return icons;
}

ImTextureID TexId(const Texture2D& t) {
    return t.id != 0 ? static_cast<ImTextureID>(GetTextureImGuiId(t)) : ImTextureID{};
}

} // namespace

// ---------------------------------------------------------------------------

std::string EnsureScriptsDir() {
    const std::string projectFolder = ProjectFolder();
    if (projectFolder.empty()) return {};
    return (fs::path(projectFolder) / kScriptDirName).string();
}

// Files this session created and the scene has not been saved with yet: "path<TAB>class" per line, journaled
// in the project folder so a crash or quit without saving doesn't leave them behind.
static std::vector<std::pair<std::string, std::string>> g_pendingScripts;

static std::string PendingJournalPath() {
    const std::string folder = ProjectFolder();
    return folder.empty() ? std::string() : (fs::path(folder) / ".pending_scripts").generic_string();
}

static void WritePendingJournal() {
    const std::string jp = PendingJournalPath();
    if (jp.empty()) return;
    std::error_code ec;
    if (g_pendingScripts.empty()) { fs::remove(jp, ec); return; }
    std::ofstream out(jp, std::ios::trunc);
    for (const auto& p : g_pendingScripts) out << p.first << '\t' << p.second << '\n';
}

static void DeleteIfUntouched(const std::string& file, const std::string& cls) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::error_code ec;
    if (content == MakeScriptStub(cls)) fs::remove(file, ec);
}

void CommitPendingScripts() {
    g_pendingScripts.clear();
    WritePendingJournal();
}

void DiscardPendingScripts() {
    const std::string jp = PendingJournalPath();
    if (!jp.empty()) {
        std::ifstream in(jp);
        std::string line;
        while (std::getline(in, line)) {
            const size_t tab = line.find('\t');
            if (tab != std::string::npos) g_pendingScripts.emplace_back(line.substr(0, tab), line.substr(tab + 1));
        }
    }
    for (const auto& p : g_pendingScripts) DeleteIfUntouched(p.first, p.second);
    g_pendingScripts.clear();
    WritePendingJournal();
}

bool DeleteScriptFile(const std::string& className) {
    const std::string dir = EnsureScriptsDir();
    if (dir.empty()) return false;
    const std::string filePath = (fs::path(dir) / (SanitizeStem(MakeClassName(className)) + ".cpp")).string();
    for (auto it = g_pendingScripts.begin(); it != g_pendingScripts.end(); ++it)
        if (it->first == filePath) { g_pendingScripts.erase(it); WritePendingJournal(); break; }
    std::error_code ec;
    return fs::remove(filePath, ec);
}

void DiscardPendingScript(const std::string& file) {
    for (auto it = g_pendingScripts.begin(); it != g_pendingScripts.end(); ++it)
        if (it->first == file) {
            DeleteIfUntouched(it->first, it->second);
            g_pendingScripts.erase(it);
            WritePendingJournal();
            return;
        }
}

std::string MaterializeScript(const std::string& className) {
    const std::string dir = EnsureScriptsDir();
    const std::string classIdent = MakeClassName(className);
    const std::string stem = SanitizeStem(classIdent);
    const std::string fileName = stem + ".cpp";
    const std::string filePath =
        dir.empty() ? fileName : (fs::path(dir) / fileName).string();

    std::error_code ec;
    if (!dir.empty()) fs::create_directories(dir, ec);

    if (!fs::exists(filePath)) {
        std::string source = MakeScriptStub(classIdent);
        std::ofstream out(filePath, std::ios::binary);
        if (!out) return {};
        out.write(source.data(), static_cast<std::streamsize>(source.size()));
        out.close();
        g_pendingScripts.emplace_back(filePath, classIdent);
        WritePendingJournal();
    }
    return filePath;
}

void RequestOpenAt(const std::string& file, int line, int column) {
    if (file.empty()) return;
    if (g_editorSet) {
        if (LaunchEditor(g_editorKind, g_editorPath, file, line, column)) return;
        // Remembered editor didn't launch (uninstalled/moved) - forget it
        // and fall through to asking again below.
        g_editorSet = false;
        g_editorPath.clear();
    }
    g_pendingFile = file;
    g_pendingLine = line;
    g_pendingColumn = column;
    g_detectedValid = false;
}

void RequestOpen(const std::string& file) {
    RequestOpenAt(file, 0, 0);
}

// Always shows the picker, even when an editor is already remembered - used
// by the right-click "Choose Editor..." action so the user can override or
// reset their default at any time.
void RequestChooseEditor(const std::string& file) {
    if (file.empty()) return;
    g_pendingFile = file;
    g_pendingLine = 0;
    g_pendingColumn = 0;
    g_detectedValid = false;
}

bool HasPending() {
    return !g_pendingFile.empty();
}

void CancelPending() {
    g_pendingFile.clear();
}

bool DrawImGuiModal() {
    if (g_pendingFile.empty()) return false;

    bool closed = false;
    if (ImGui::Begin("Open Script In", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Choose an editor to open the script in:");
        ImGui::TextWrapped("This becomes your default - right-click a script and choose \"Choose Editor...\" to change it later.");
        ImGui::Spacing();

        const IdeIcons& icons = GetIdeIcons();
        struct Option { const char* label; EditorKind kind; const Texture2D* icon; };
        const Option options[] = {
            { "CLion",          EditorKind::CLion,        &icons.clion },
            { "Rider",          EditorKind::Rider,        &icons.rider },
            { "VS Code",        EditorKind::VSCode,       &icons.vscode },
#if defined(_WIN32)
            // No Linux build of Visual Studio exists, so offering it would
            // only ever produce a launch failure.
            { "Visual Studio",  EditorKind::VisualStudio, &icons.visualStudio },
            { "Notepad",        EditorKind::Notepad,      &icons.notepad },
#else
            { "Text Editor",    EditorKind::Notepad,      &icons.notepad },
#endif
            { "Qt Creator",     EditorKind::QtCreator,    &icons.qtCreator },
            { "Code::Blocks",   EditorKind::CodeBlocks,   &icons.codeBlocks },
            { "Auto-Detect",    EditorKind::None,         nullptr },
        };

        const float iconSize = 22.0f;
        for (const Option& o : options) {
            const bool installed = EditorInstalled(o.kind);
            ImGui::PushID(o.label);
            ImGui::BeginDisabled(!installed);   // an editor that isn't on this machine can't be picked
            bool clicked = false;
            if (o.icon && o.icon->width > 0 && o.icon->height > 0) {
                // Fit the picture into the square without stretching it (not every icon is square).
                const float fit = std::min(iconSize / o.icon->width, iconSize / o.icon->height);
                const ImVec2 shown(o.icon->width * fit, o.icon->height * fit);
                const ImVec2 pad((iconSize - shown.x) * 0.5f, (iconSize - shown.y) * 0.5f);
                const ImVec2 cursor = ImGui::GetCursorScreenPos();
                const ImVec2 framePad = ImGui::GetStyle().FramePadding;
                clicked = ImGui::Button("##ide", ImVec2(iconSize + framePad.x * 2.0f, iconSize + framePad.y * 2.0f));
                ImGui::GetWindowDrawList()->AddImage(TexId(*o.icon),
                    ImVec2(cursor.x + framePad.x + pad.x, cursor.y + framePad.y + pad.y),
                    ImVec2(cursor.x + framePad.x + pad.x + shown.x, cursor.y + framePad.y + pad.y + shown.y));
            } else {
                clicked = ImGui::Button("##ide", ImVec2(iconSize + 22.0f, iconSize + 5.0f));
            }
            ImGui::SameLine();
            clicked = ImGui::Selectable(installed ? o.label : (std::string(o.label) + "  (not installed)").c_str()) || clicked;
            ImGui::EndDisabled();
            if (clicked && installed) {
                std::string file = g_pendingFile;
                std::string exePath = ResolveEditorPath(o.kind);
                // Only remember a choice that actually launched successfully.
                if (LaunchEditor(o.kind, exePath, file, g_pendingLine, g_pendingColumn)) {
                    g_editorSet = true;
                    g_editorKind = o.kind;
                    g_editorPath = exePath;
                }
                g_pendingFile.clear();
                closed = true;
            }
            ImGui::PopID();
        }

        // Not a code editor: opens the script as a node graph in the Visual Script tab.
        // Same row layout as the editors above: an icon-sized button (a little node graph), then the label.
        ImGui::PushID("vscript");
        bool vsClicked = ImGui::Button("##vsicon", ImVec2(iconSize + 22.0f, iconSize + 5.0f));
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const ImVec2 n1(a.x + (b.x - a.x) * 0.28f, a.y + (b.y - a.y) * 0.35f), n2(a.x + (b.x - a.x) * 0.72f, a.y + (b.y - a.y) * 0.65f);
            dl->AddBezierCubic(n1, ImVec2(n2.x, n1.y), ImVec2(n1.x, n2.y), n2, IM_COL32(235, 235, 235, 255), 1.6f);
            dl->AddRectFilled(ImVec2(n1.x - 5, n1.y - 4), ImVec2(n1.x + 5, n1.y + 4), IM_COL32(60, 170, 90, 255), 2.0f);
            dl->AddRectFilled(ImVec2(n2.x - 5, n2.y - 4), ImVec2(n2.x + 5, n2.y + 4), IM_COL32(70, 130, 220, 255), 2.0f);
        }
        ImGui::SameLine();
        vsClicked = ImGui::Selectable("Visual Script") || vsClicked;
        ImGui::PopID();
        if (vsClicked) {
            vscript::OpenByName(std::filesystem::path(g_pendingFile).stem().string());
            DiscardPendingScript(g_pendingFile);   // a stub made just to offer the choice is not wanted
            ui::SelectBottomTab(2);
            g_pendingFile.clear();
            closed = true;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(220 + 22.0f, 0))) {
            g_pendingFile.clear();
            closed = true;
        }
    }
    ImGui::End();

    return !closed;
}



std::string EditObjectScript(ScatteredObject* obj) {
    if (!obj) return {};
    // `obj->script` holds the FLY_SCRIPT class name (e.g. "RotateScript"); the
    // materialized file is the .cpp source for that class. Auto-bind the class
    // from the object name the first time a script is added, and mark it to run
    // on Play so the script host actually starts it.
    std::string className = MakeClassName(obj->script.empty() ? obj->GetName() : obj->script);
    if (obj->script.empty() || !obj->runOnPlay) {
        obj->script = className;
        obj->runOnPlay = true;
    }
    std::string file = MaterializeScript(className);
    if (!file.empty()) RequestOpen(file);
    return file;
}

std::string AddObjectScript(ScatteredObject* obj) {
    if (!obj) return {};
    if (obj->script.empty()) {
        EditObjectScript(obj);
        return obj->script;
    }
    // "Cube", "Cube2", "Cube3"...: the first free class name on this object.
    const std::string base = MakeClassName(obj->GetName());
    std::string className = base;
    for (int n = 2; obj->HasScript(className); ++n) className = base + std::to_string(n);
    obj->AddScript(className);
    obj->runOnPlay = true;
    std::string file = MaterializeScript(className);
    if (!file.empty()) RequestOpen(file);
    return className;
}

std::string EditObjectScriptNamed(ScatteredObject* obj, const std::string& className) {
    if (!obj || className.empty()) return {};
    std::string file = MaterializeScript(MakeClassName(className));
    if (!file.empty()) RequestOpen(file);
    return file;
}

std::string ChooseEditorForObjectScriptNamed(const std::string& className) {
    if (className.empty()) return {};
    std::string file = MaterializeScript(MakeClassName(className));
    if (!file.empty()) RequestChooseEditor(file);
    return file;
}

std::string EditStandaloneScript(const std::string& scriptName,
                                 const std::string& typeName,
                                 std::string* outClassName /* = nullptr */) {
    // `typeName` is the FLY_SCRIPT class to edit; fall back to the display name.
    std::string className = MakeClassName(typeName.empty() ? scriptName : typeName);
    if (outClassName) *outClassName = className;
    std::string file = MaterializeScript(className);
    if (!file.empty()) RequestOpen(file);
    return file;
}

// Right-click "Choose Editor..." for an object's script - always shows the
// picker so the user can override or reset their remembered default.
std::string ChooseEditorForObjectScript(ScatteredObject* obj) {
    if (!obj || obj->script.empty()) return {};
    std::string className = MakeClassName(obj->script);
    std::string file = MaterializeScript(className);
    if (!file.empty()) RequestChooseEditor(file);
    return file;
}

// Same, for a standalone script.
std::string ChooseEditorForStandaloneScript(const std::string& scriptName,
                                            const std::string& typeName) {
    std::string className = MakeClassName(typeName.empty() ? scriptName : typeName);
    std::string file = MaterializeScript(className);
    if (!file.empty()) RequestChooseEditor(file);
    return file;
}

} // namespace scriptLauncher