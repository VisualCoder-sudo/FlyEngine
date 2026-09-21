#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CloseWindow Win32CloseWindow
#define ShowCursor Win32ShowCursor
#define Rectangle Win32Rectangle
#include <windows.h>
#include <shellapi.h>
#undef CloseWindow
#undef ShowCursor
#undef Rectangle
#undef LoadImage
#undef DrawText
#undef DrawTextEx
#undef PlaySound

#include "../../../include/Engine/Scripts/ScriptLauncher.hpp"
#include "../../../include/Engine/Frontend/ProjectManager.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"

#include "imgui.h"
#include "raylib.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>

namespace fs = std::filesystem;

namespace scriptLauncher {

// Builds a valid C# identifier (class name) from an arbitrary object/script
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

enum class EditorKind { None, VSCode, VisualStudio, Rider, Notepad };
constexpr const char* kScriptDirName = "Scripts";

// Builds the source of a C# script stub with the given class name. The class
// implements FlyScript.IScript (IEnumerable<object> Run()) so the hosted C#
// ScriptHost can instantiate it by type name.
std::string MakeScriptStub(const std::string& className) {
    std::string body;
    body += "using System;\n";
    body += "using System.Collections;\n";
    body += "using System.Collections.Generic;\n";
    body += "using FlyScript;\n";
    body += "\n";
    body += "public class " + className + " : IScript\n";
    body += "{\n";
    body += "    public IEnumerable<object> Run()\n";
    body += "    {\n";
    body += "        // Logic runs once per frame while the game is playing.\n";
    body += "        // Use \"self\" for the attached object, e.g.:\n";
    body += "        //   var self = GameObject.Self;\n";
    body += "        //   self.Position = new Vec3(0, 2, 0);\n";
    body += "        while (true)\n";
    body += "        {\n";
    body += "            yield return Tick.Wait(0f);\n";
    body += "        }\n";
    body += "    }\n";
    body += "}\n";
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

std::string FindVSCode() {
    if (const char* path = std::getenv("PATH")) {
        std::string envPath(path);
        size_t start = 0;
        while (start <= envPath.size()) {
            size_t sep = envPath.find(';', start);
            std::string dir = envPath.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
            fs::path probe = fs::path(dir) / "code.cmd";
            if (fs::exists(probe)) return probe.string();
            if (sep == std::string::npos) break;
            start = sep + 1;
        }
    }
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
}

std::string RunCommandCaptureOutput(const std::string& command, size_t capacity) {
    std::vector<char> out(capacity);
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return {};
    STARTUPINFOA si{ sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = nullptr;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, const_cast<char*>(command.c_str()), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return {};
    }
    CloseHandle(writePipe);
    DWORD total = 0;
    char buf[1024];
    DWORD got = 0;
    while (ReadFile(readPipe, buf, sizeof(buf) - 1, &got, nullptr) && got > 0) {
        buf[got] = '\0';
        size_t toCopy = (total + got) < out.size() - 1 ? got : out.size() - 1 - total;
        memcpy(out.data() + total, buf, toCopy);
        total += static_cast<DWORD>(toCopy);
        if (total >= out.size() - 1) break;
    }
    CloseHandle(readPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (total == 0) return {};
    out[total] = '\0';
    std::string result(out.data());
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' ||
                               result.back() == ' ')) {
        result.pop_back();
    }
    return result;
}

std::string FindVisualStudio() {
    const std::string vswhere =
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    if (!FileIsExecutable(vswhere)) return {};
    std::string installPath =
        RunCommandCaptureOutput("\"" + vswhere + "\" -latest -property installationPath", 4096);
    if (installPath.empty()) return {};
    std::string devenv = installPath + "\\Common7\\IDE\\devenv.exe";
    return FileIsExecutable(devenv) ? devenv : std::string{};
}

std::string FindRider() {
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
            std::string candidate = entry.path().string() + "\\bin\\rider64.exe";
            if (FileIsExecutable(candidate)) best = candidate;
        }
        if (!best.empty()) return best;
    }
    return {};
}

// Resolve an editor kind to an executable path. Empty indicates the OS default
// association (or Notepad, which requires no path).
std::string ResolveEditorPath(EditorKind kind) {
    switch (kind) {
        case EditorKind::VSCode:        return FindVSCode();
        case EditorKind::VisualStudio:  return FindVisualStudio();
        case EditorKind::Rider:         return FindRider();
        default:                        return {};
    }
}

std::wstring Utf8ToWide(const std::string& s) {
    std::wstring w;
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len > 0) {
        w.resize(static_cast<size_t>(len) - 1);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    }
    return w;
}

bool ShellOpen(const std::wstring& exe, const std::wstring& args) {
    HINSTANCE r = ShellExecuteW(nullptr, L"open",
                                exe.empty() ? nullptr : exe.c_str(),
                                args.empty() ? nullptr : args.c_str(),
                                nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

// Actually launch an editor on `file`.
bool LaunchEditor(EditorKind kind, const std::string& exePath, const std::string& file) {
    std::wstring wideFile = Utf8ToWide(file);
    if (kind == EditorKind::VSCode) {
        return ShellOpen(Utf8ToWide(exePath), L"\"" + wideFile + L"\"");
    }
    if (kind == EditorKind::VisualStudio) {
        return ShellOpen(Utf8ToWide(exePath), L"/edit \"" + wideFile + L"\"");
    }
    if (kind == EditorKind::Rider) {
        return ShellOpen(Utf8ToWide(exePath), L"\"" + wideFile + L"\"");
    }
    if (kind == EditorKind::Notepad) {
        // notepad.exe is always on the system PATH.
        return ShellOpen(L"notepad.exe", L"\"" + wideFile + L"\"");
    }
    // Auto / default: OS association.
    return ShellOpen(wideFile, L"");
}

// --- Picker state ----------------------------------------------------------

std::string g_pendingFile;          // file queued to open behind the picker
bool        g_editorSet = false;    // a remembered editor is available
EditorKind  g_editorKind = EditorKind::None;
std::string g_editorPath;           // cached resolved exe (empty for notepad/default)

const char* EditorKindLabel(EditorKind k) {
    switch (k) {
        case EditorKind::VSCode:       return "VS Code";
        case EditorKind::VisualStudio: return "Visual Studio";
        case EditorKind::Rider:        return "Rider";
        case EditorKind::Notepad:      return "Notepad";
        default:                       return "Auto-Detect";
    }
}

// --- IDE picker icons (black silhouette -> white for the dark theme) --------

struct IdeIcons {
    Texture2D rider;
    Texture2D vscode;
    Texture2D visualStudio;
    Texture2D notepad;
};

Texture2D LoadIconOrNull(const char* fileName) {
    std::string path = std::string("assets/EditorIcons/ideicons/") + fileName;
    if (!FileExists(path.c_str())) path = std::string("../assets/EditorIcons/ideicons/") + fileName;
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
        v.rider          = LoadIconOrNull("jbrider.png");
        v.vscode         = LoadIconOrNull("vscode.png");
        v.visualStudio   = LoadIconOrNull("vstudio.png");
        v.notepad        = LoadIconOrNull("npad.png");
        return v;
    }();
    return icons;
}

ImTextureID TexId(const Texture2D& t) {
    return t.id != 0 ? static_cast<ImTextureID>(t.id) : ImTextureID{};
}

} // namespace

// ---------------------------------------------------------------------------

std::string EnsureScriptsDir() {
    const std::string projectFolder = ProjectFolder();
    if (projectFolder.empty()) return {};
    return (fs::path(projectFolder) / kScriptDirName).string();
}

std::string MaterializeScript(const std::string& className) {
    const std::string dir = EnsureScriptsDir();
    const std::string classIdent = MakeClassName(className);
    const std::string stem = SanitizeStem(classIdent);
    const std::string fileName = stem + ".cs";
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
    }
    return filePath;
}

void RequestOpen(const std::string& file) {
    if (file.empty()) return;
    if (g_editorSet) {
        if (LaunchEditor(g_editorKind, g_editorPath, file)) return;
        // Remembered editor didn't launch (uninstalled/moved) — forget it
        // and fall through to asking again below.
        g_editorSet = false;
        g_editorPath.clear();
    }
    g_pendingFile = file;
}

// Always shows the picker, even when an editor is already remembered — used
// by the right-click "Choose Editor..." action so the user can override or
// reset their default at any time.
void RequestChooseEditor(const std::string& file) {
    if (file.empty()) return;
    g_pendingFile = file;
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
            { "Rider",          EditorKind::Rider,        &icons.rider },
            { "VS Code",        EditorKind::VSCode,       &icons.vscode },
            { "Visual Studio",  EditorKind::VisualStudio, &icons.visualStudio },
            { "Notepad",        EditorKind::Notepad,      &icons.notepad },
            { "Auto-Detect",    EditorKind::None,         nullptr },
        };

        const float iconSize = 22.0f;
        for (const Option& o : options) {
            ImGui::PushID(o.label);
            bool clicked = false;
            if (o.icon) {
                clicked = ImGui::ImageButton("##ide", TexId(*o.icon), ImVec2(iconSize, iconSize));
            } else {
                clicked = ImGui::Button("##ide", ImVec2(iconSize + 22.0f, iconSize + 5.0f));
            }
            ImGui::SameLine();
            clicked = ImGui::Selectable(o.label) || clicked;
            if (clicked) {
                std::string file = g_pendingFile;
                std::string exePath = ResolveEditorPath(o.kind);
                // Only remember a choice that actually launched successfully.
                if (LaunchEditor(o.kind, exePath, file)) {
                    g_editorSet = true;
                    g_editorKind = o.kind;
                    g_editorPath = exePath;
                }
                g_pendingFile.clear();
                closed = true;
            }
            ImGui::PopID();
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
    // `obj->script` holds the C# IScript type name (e.g. "RotateScript"); the
    // materialized file is the .cs source for that class. Auto-bind the class
    // from the object name the first time a script is added, and mark it to run
    // on Play so the C# host actually starts it.
    std::string className = MakeClassName(obj->script.empty() ? obj->GetName() : obj->script);
    if (obj->script.empty() || !obj->runOnPlay) {
        obj->script = className;
        obj->runOnPlay = true;
    }
    std::string file = MaterializeScript(className);
    if (!file.empty()) RequestOpen(file);
    return file;
}

std::string EditStandaloneScript(const std::string& scriptName,
                                 const std::string& typeName,
                                 std::string* outClassName /* = nullptr */) {
    // `typeName` is the C# IScript class to edit; fall back to the display name.
    std::string className = MakeClassName(typeName.empty() ? scriptName : typeName);
    if (outClassName) *outClassName = className;
    std::string file = MaterializeScript(className);
    if (!file.empty()) RequestOpen(file);
    return file;
}

// Right-click "Choose Editor..." for an object's script — always shows the
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