// ProjectManager.cpp - the "Projects" hub window.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#undef CloseWindow
#undef ShowCursor
#undef Rectangle
#undef LoadImage
#undef DrawText
#undef DrawTextEx
#undef PlaySound
#endif

#include "../../../include/Engine/Backend/TextureManager.hpp"
#include "../../../include/Engine/Frontend/ProjectManager.hpp"
#include "../../../include/Engine/Backend/ScenePersistence.hpp"
#include "../../../include/Engine/Graphics.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine.hpp"
#include "raylib.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

namespace project {

namespace detail {

    constexpr int kW = 900;
    constexpr int kH = 560;

    const char* kSignature = "FLYENGINE_PROJECT";
    constexpr int kVersion = 1;

    namespace theme {
        constexpr Color BG         = Color{ 24, 26, 32, 255 };
        constexpr Color PANEL      = Color{ 30, 32, 38, 255 };
        constexpr Color TITLE      = Color{ 38, 41, 48, 255 };
        constexpr Color WIDGET     = Color{ 48, 51, 60, 255 };
        constexpr Color WIDGET_HOVER = Color{ 63, 67, 78, 255 };
        constexpr Color WIDGET_PRESSED = Color{ 40, 43, 51, 255 };
        constexpr Color ROW_HOVER  = Color{ 44, 47, 56, 255 };
        constexpr Color INPUT      = Color{ 21, 23, 28, 255 };
        constexpr Color BORDER     = Color{ 72, 77, 88, 255 };
        constexpr Color BORDER_STRONG = Color{ 104, 110, 122, 255 };
        constexpr Color DIVIDER    = Color{ 56, 60, 70, 255 };
        constexpr Color ACCENT     = Color{ 0, 190, 200, 255 };
        constexpr Color ACCENT_HOVER = Color{ 40, 210, 220, 255 };
        constexpr Color ACCENT_SOFT = Color{ 0, 190, 200, 42 };
        constexpr Color TEXT       = Color{ 232, 232, 238, 255 };
        constexpr Color TEXT_MUTED = Color{ 156, 161, 172, 255 };
        constexpr Color TEXT_DIM   = Color{ 122, 128, 140, 255 };
        constexpr Color DANGER     = Color{ 216, 84, 84, 255 };
    } // namespace theme

    Font g_font = { 0 };

    void DrawTextU(const char* text, float x, float y, float size, Color color) {
        DrawTextEx(g_font, text, Vector2{ x, y }, size, 1.0f, color);
    }

    float MeasureTextU(const char* text, float size) {
        return MeasureTextEx(g_font, text, size, 1.0f).x;
    }

    std::string TrimCopy(std::string s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return "";
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    std::string BaseDirOf(const std::string& path) {
        std::string dir = GetDirectoryPath(path.c_str());
        while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
        return dir;
    }

    // Project timestamps are stored as raw unix seconds, not formatted strings.
    long long TimeNow() {
        return static_cast<long long>(time(nullptr));
    }

    std::string PathSeparator() {
#ifdef _WIN32
        return "\\";
#else
        return "/";
#endif
    }

    std::string JoinPath(const std::string& base, const std::string& leaf) {
        if (base.empty()) return leaf;
        if (leaf.empty()) return base;
        return base + PathSeparator() + leaf;
    }

    struct HeaderFields {
        std::string name;
        std::string templateName;
        long long created = 0;
        long long modified = 0;
    };

    // Parses one "Key = value" header line. Surrounding double quotes on the value
    // are stripped so hand-edited files and older builds both load.
    bool ParseHeaderLine(const std::string& line, std::string& key, std::string& value) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) return false;
        key = TrimCopy(line.substr(0, eq));
        value = TrimCopy(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        return !key.empty();
    }

    // Opens the .flyproj at `projectFile` and parses the header lines. On success
    // `scene` is left open and positioned just past the "---SCENE---" marker, ready
    // for the scene blob.
    bool ReadProjectFileAt(const std::string& projectFile, HeaderFields& out, std::ifstream& scene) {
        if (projectFile.empty()) return false;
        scene.open(projectFile, std::ios::in);
        if (!scene.is_open()) return false;
        std::string line;
        if (!std::getline(scene, line)) return false;
        std::istringstream head(line);
        std::string sig;
        int ver = 0;
        if (!(head >> sig >> ver) || sig != kSignature || ver != kVersion) return false;
        // Header fields are dispatched by key rather than by line position, so a
        // file with a reordered or hand-added field still loads.
        bool inScene = false;
        while (std::getline(scene, line)) {
            if (line == "---SCENE---") { inScene = true; break; }
            std::string key, value;
            if (!ParseHeaderLine(line, key, value)) continue;
            if (key == "Name") out.name = value;
            else if (key == "Template") out.templateName = value;
            else if (key == "Created") out.created = atoll(value.c_str());
            else if (key == "Modified") out.modified = atoll(value.c_str());
        }
        return inScene;
    }

    // Locates the .flyproj that makes `folder` a project. The canonical name is
    // <folder>/<folder>.flyproj, but the recent-projects list stores folders, so
    // a user who renames either the folder or the file would otherwise see their
    // project silently go "missing". Fall back to a lone *.flyproj in the folder.
    // Returns an empty string when there is none, or more than one to choose from.
    std::string FindProjectFile(const std::string& folder) {
        if (folder.empty()) return std::string();
        const std::string name = folder.substr(folder.find_last_of("\\/") + 1);
        const std::string preferred = JoinPath(folder, name + ".flyproj");
        if (FileExists(preferred.c_str())) return preferred;

        std::error_code ec;
        std::string found;
        int matches = 0;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (ec) break;
            const fs::path& p = it->path();
            if (!it->is_regular_file(ec) || ec) continue;
            if (p.extension() != ".flyproj") continue;
            // The playtest scratch file lives here too and must not be picked up.
            const std::string stem = p.stem().string();
            if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, "_temp") == 0) continue;
            found = p.string();
            ++matches;
        }
        return matches == 1 ? found : std::string();
    }

    // Same as above, but resolves the file from the project folder.
    bool ReadProjectFile(const std::string& projectFolder, HeaderFields& out, std::ifstream& scene) {
        return ReadProjectFileAt(FindProjectFile(projectFolder), out, scene);
    }

    // The on-disk header format. Kept byte-compatible with projects written by
    // earlier builds: capitalised keys, quoted Name/Template, unquoted timestamps.
    void WriteProjectHeader(std::ostream& out, const HeaderFields& fields) {
        out << kSignature << " " << kVersion << "\n";
        out << "Name = \"" << fields.name << "\"\n";
        out << "Template = \"" << fields.templateName << "\"\n";
        out << "Created = " << fields.created << "\n";
        out << "Modified = " << fields.modified << "\n";
        out << "---SCENE---\n";
    }

    // ---- small filesystem helpers (cross-platform) ----------------------------

    // Windows: resolve a CSIDL to its filesystem path.
    // Other platforms: no equivalent concept, callers fall back to $HOME.
    std::string GetSpecialFolder(int csidl) {
#ifdef _WIN32
        char buf[MAX_PATH] = { 0 };
        if (SUCCEEDED(SHGetFolderPathA(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buf))) {
            return std::string(buf);
        }
        return std::string();
#else
        (void)csidl;
        return std::string();
#endif
    }

    // $HOME with a sane fallback so the app never builds paths off an empty root.
    std::string HomeDirectory() {
        if (const char* home = std::getenv("HOME"); home && *home) return std::string(home);
        return std::string(".");
    }

    // Creates `path` and any missing parents. Returns false only if it is missing afterwards.
    bool EnsureDirectory(const std::string& path) {
        if (path.empty()) return false;
#ifdef _WIN32
        // CreateDirectoryA only creates one level, so walk up until we hit an
        // existing directory and then build the tail back down.
        if (CreateDirectoryA(path.c_str(), nullptr)) return true;
        const DWORD attr = GetFileAttributesA(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;

        const size_t slash = path.find_last_of("\\/");
        if (slash == std::string::npos || slash == 0) return false;
        if (!EnsureDirectory(path.substr(0, slash))) return false;
        return CreateDirectoryA(path.c_str(), nullptr) != 0;
#else
        std::error_code ec;
        fs::create_directories(path, ec);
        return fs::is_directory(path);
#endif
    }

    // Recursively removes a project folder. This used to go through
    // SHFileOperationA on Windows, but remove_all is portable and reports
    // failure through its return value, so the delete is a hard delete on
    // every platform (which is what the confirm dialog promises).
    bool DeleteFolderRecursive(const std::string& path) {
        if (path.empty()) return false;
        std::error_code ec;
        const std::uintmax_t removed = fs::remove_all(path, ec);
        return removed != static_cast<std::uintmax_t>(-1) && !ec;
    }

    // ---- folder pickers ---------------------------------------------------------

    // The "<folder>.flyproj" file that identifies `folder` as a Flyengine project.
    // Returns an empty string when the folder isn't a project.
    std::string ProjectFileOf(const std::string& folder) {
        return FindProjectFile(folder);
    }

    // Asks for a project folder. Uses platform::BeginChooseFolderDialog so the
    // picker is native on every platform (zenity/kdialog helper, or the in-editor
    // path prompt) and, crucially, non-blocking - the result is picked up by
    // PollChosenProjectFolder() on a later frame.
    bool BeginChooseProjectOpenPath() {
        return platform::BeginChooseFolderDialog(platform::DialogPurpose::ChooseProjectFolder,
                                                 "Open Project folder", GetProjectsDirectory());
    }

    // Returns true exactly once, when a folder has been chosen (an empty
    // `outFolder` means the user cancelled). Folders without a .flyproj are
    // reported as a hard failure through `outError`.
    bool PollChosenProjectFolder(std::string& outFolder, std::string& outError) {
        std::string picked;
        if (!platform::PollDialogResult({platform::DialogPurpose::ChooseProjectFolder}, picked)) return false;
        if (picked.empty()) { outFolder.clear(); return true; }   // cancelled
        if (ProjectFileOf(picked).empty()) {
            outFolder.clear();
            outError = "Folder is not a Flyengine project";
            return true;
        }
        outFolder = picked;
        return true;
    }

    // ---- tiny widget helpers ----------------------------------------------------

    bool Hovered(Rectangle rec) {
        return CheckCollisionPointRec(GetMousePosition(), rec);
    }

    bool Button(Rectangle rec, const char* label) {
        const bool hovered = Hovered(rec);
        const bool pressed = hovered && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        Color bg = pressed ? theme::WIDGET_PRESSED : (hovered ? theme::WIDGET_HOVER : theme::WIDGET);
        DrawRectangleRounded(rec, 0.16f, 4, bg);
        DrawRectangleLinesEx(rec, 1.0f, hovered ? theme::BORDER_STRONG : theme::BORDER);
        float tw = MeasureTextU(label, 14.0f);
        DrawTextU(label, rec.x + (rec.width - tw) * 0.5f, rec.y + (rec.height - 14.0f) * 0.5f + 1.0f, 14.0f,
            hovered ? theme::TEXT : theme::TEXT_MUTED);
        if (hovered) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
        return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    }

    bool PrimaryButton(Rectangle rec, const char* label) {
        const bool hovered = Hovered(rec);
        const bool pressed = hovered && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        Color bg = pressed ? Color{ 0, 148, 158, 255 } : (hovered ? theme::ACCENT_HOVER : theme::ACCENT);
        DrawRectangleRounded(rec, 0.16f, 4, bg);
        DrawRectangleLinesEx(rec, 1.0f, hovered ? Color{ 255, 255, 255, 60 } : theme::BORDER_STRONG);
        float tw = MeasureTextU(label, 14.0f);
        DrawTextU(label, rec.x + (rec.width - tw) * 0.5f, rec.y + (rec.height - 14.0f) * 0.5f + 1.0f, 14.0f, WHITE);
        if (hovered) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
        return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    }

    bool DangerButton(Rectangle rec, const char* label) {
        const bool hovered = Hovered(rec);
        const bool pressed = hovered && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        Color bg = pressed ? Color{ 120, 30, 30, 255 }
                           : (hovered ? Color{ 170, 45, 45, 255 } : Color{ 100, 35, 35, 255 });
        DrawRectangleRounded(rec, 0.16f, 4, bg);
        DrawRectangleLinesEx(rec, 1.0f, hovered ? Color{ 255, 110, 110, 180 } : Color{ 110, 45, 45, 255 });
        float tw = MeasureTextU(label, 14.0f);
        DrawTextU(label, rec.x + (rec.width - tw) * 0.5f, rec.y + (rec.height - 14.0f) * 0.5f + 1.0f, 14.0f,
            hovered ? WHITE : Color{ 240, 205, 205, 255 });
        if (hovered) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
        return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    }

    struct NameField {
        std::string text;
        int cursor = 0;
        bool focused = false;
    };

    void DrawTextField(Rectangle rec, NameField& field) {
        Vector2 mouse = GetMousePosition();
        const bool hovered = CheckCollisionPointRec(mouse, rec);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            field.focused = CheckCollisionPointRec(mouse, rec);
            if (field.focused) field.cursor = static_cast<int>(field.text.size());
        }
        if (hovered) SetMouseCursor(MOUSE_CURSOR_IBEAM);

        if (field.focused) {
            int key = GetCharPressed();
            while (key > 0) {
                if (key >= 32 && key <= 126 && static_cast<int>(field.text.size()) < 127) {
                    field.text.insert(static_cast<size_t>(field.cursor), 1, static_cast<char>(key));
                    field.cursor++;
                }
                key = GetCharPressed();
            }
            if (IsKeyPressed(KEY_BACKSPACE) && field.cursor > 0) {
                field.text.erase(static_cast<size_t>(field.cursor) - 1, 1);
                field.cursor--;
            }
            if (IsKeyPressed(KEY_DELETE) && field.cursor < static_cast<int>(field.text.size())) {
                field.text.erase(static_cast<size_t>(field.cursor), 1);
            }
            if (IsKeyPressed(KEY_LEFT) && field.cursor > 0) field.cursor--;
            if (IsKeyPressed(KEY_RIGHT) && field.cursor < static_cast<int>(field.text.size())) field.cursor++;
            if (IsKeyPressed(KEY_HOME)) field.cursor = 0;
            if (IsKeyPressed(KEY_END)) field.cursor = static_cast<int>(field.text.size());
            if (field.cursor > static_cast<int>(field.text.size())) {
                field.cursor = static_cast<int>(field.text.size());
            }
        }

        DrawRectangleRounded(rec, 0.15f, 4, theme::INPUT);
        DrawRectangleLinesEx(rec, field.focused ? 2.0f : 1.0f,
            field.focused ? theme::ACCENT : (hovered ? theme::BORDER_STRONG : theme::BORDER));

        // Scroll the visible text horizontally so the cursor never leaves the box.
        const float maxTextW = rec.width - 12.0f;
        int hidden = 0;
        std::string visiblePrefix = field.text.substr(0, static_cast<size_t>(field.cursor));
        while (!visiblePrefix.empty() && MeasureTextU(visiblePrefix.c_str(), 14.0f) > maxTextW) {
            visiblePrefix.erase(0, 1);
            hidden++;
        }
        const std::string display = field.text.substr(static_cast<size_t>(hidden));
        const float textStartX = rec.x + 6.0f;
        if (field.text.empty() && !field.focused) {
            DrawTextU("MyGame", textStartX, rec.y + 3.0f, 14.0f, theme::TEXT_DIM);
        } else {
            DrawTextU(display.c_str(), textStartX, rec.y + 3.0f, 14.0f,
                field.focused ? theme::TEXT : theme::TEXT_MUTED);
        }
        if (field.focused && (static_cast<int>(GetTime() * 2.0) % 2) == 0) {
            const float cursorX = textStartX + MeasureTextU(visiblePrefix.c_str(), 14.0f);
            DrawRectangle(static_cast<int>(cursorX), static_cast<int>(rec.y) + 3, 1,
                static_cast<int>(rec.height) - 6, theme::ACCENT);
        }
    }

    Color ROWHoverColor() { return theme::ROW_HOVER; }

    bool TemplateCard(Rectangle rec, const char* title, const char* desc, bool selected) {
        const bool hovered = Hovered(rec);
        Color bg = selected ? theme::ACCENT_SOFT : (hovered ? ROWHoverColor() : theme::WIDGET);
        DrawRectangleRounded(rec, 0.14f, 4, bg);
        DrawRectangleLinesEx(rec, selected ? 2.0f : 1.0f,
            selected ? theme::ACCENT : (hovered ? theme::BORDER_STRONG : theme::BORDER));
        DrawTextU(title, rec.x + 10.0f, rec.y + 7.0f, 15.0f, selected ? theme::TEXT : theme::TEXT_MUTED);
        DrawTextU(desc, rec.x + 10.0f, rec.y + 27.0f, 11.0f, theme::TEXT_DIM);
        if (hovered) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
        return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    }

    // Modal confirmation shown before a project is deleted from disk.
    // Returns 1 if the user confirmed deletion, 2 if they cancelled, 0 otherwise.
    int DrawDeleteConfirmDialog(const std::string& name, const std::string& path) {
        DrawRectangle(0, 0, kW, kH, Color{ 0, 0, 0, 140 });

        Rectangle panel = { kW * 0.5f - 200.0f, kH * 0.5f - 85.0f, 400.0f, 170.0f };
        DrawRectangleRounded(panel, 0.12f, 4, theme::PANEL);
        DrawRectangleLinesEx(panel, 1.0f, theme::BORDER_STRONG);

        DrawTextU("Delete project?", panel.x + 16, panel.y + 14, 18, theme::TEXT);
        DrawTextU(name.c_str(), panel.x + 16, panel.y + 46, 14, theme::TEXT);
        DrawTextU("This will permanently delete the project folder.", panel.x + 16, panel.y + 70, 12, theme::DANGER);

        std::string p = path;
        while (!p.empty() && MeasureTextU(p.c_str(), 11) > 368.0f) p.pop_back();
        if (p != path) p = "..." + p;
        DrawTextU(p.c_str(), panel.x + 16, panel.y + 92, 11, theme::TEXT_DIM);

        const bool cancel = Button({ panel.x + 16, panel.y + 126, 100, 28 }, "Cancel");
        const bool del = DangerButton({ panel.x + panel.width - 116, panel.y + 126, 100, 28 }, "Delete");

        if (del) return 1;
        if (cancel) return 2;
        return 0;
    }

    // Fallback for hosts with neither zenity nor kdialog: the platform layer parks
    // the request in a PathPrompt and expects somebody to render it. The editor
    // does that in ImGui, but the project manager is a plain raylib window, so it
    // has to draw the prompt itself - otherwise the picker would hang with no way
    // to answer it. Writing accept/cancel back into the same PathPrompt keeps
    // PollDialogResult() working unchanged.
    void DrawFolderPromptFallback(NameField& field) {
        platform::PathPrompt& p = platform::GetPathPrompt();
        if (!p.active) return;

        DrawRectangle(0, 0, kW, kH, Color{ 0, 0, 0, 140 });

        Rectangle panel = { kW * 0.5f - 240.0f, kH * 0.5f - 80.0f, 480.0f, 160.0f };
        DrawRectangleRounded(panel, 0.12f, 4, theme::PANEL);
        DrawRectangleLinesEx(panel, 1.0f, theme::BORDER_STRONG);

        DrawTextU(p.title.empty() ? "Open Project folder" : p.title.c_str(),
            panel.x + 16, panel.y + 14, 16, theme::TEXT);
        DrawTextU("Type the full path to the project folder:", panel.x + 16, panel.y + 42, 12, theme::TEXT_MUTED);

        // Keep the platform-owned buffer and the text field in sync: the buffer is
        // what PollDialogResult() hands back, the field is what the user types into.
        if (field.text != std::string(p.buffer)) {
            field.text = p.buffer;
            field.cursor = static_cast<int>(field.text.size());
        }
        DrawTextField({ panel.x + 16, panel.y + 62, panel.width - 32, 26 }, field);
        std::snprintf(p.buffer, sizeof(p.buffer), "%s", field.text.c_str());

        const bool ok = PrimaryButton({ panel.x + panel.width - 216, panel.y + 112, 100, 28 }, "Open") ||
                        (field.focused && IsKeyPressed(KEY_ENTER));
        const bool cancel = Button({ panel.x + panel.width - 106, panel.y + 112, 90, 28 }, "Cancel");

        if (ok) { p.accept = true; }
        else if (cancel) { p.cancel = true; }
    }

    } // namespace detail

// Cross-platform function implementations
std::string GetAppDataDirectory() {
#ifdef _WIN32
    std::string base = detail::GetSpecialFolder(CSIDL_APPDATA);
    if (base.empty()) base = "C:/Flyengine";
    return base + "\\Flyengine";
#else
    // XDG would be $XDG_CONFIG_HOME; $HOME/.config is the fallback most tooling
    // expects when that variable is unset.
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return detail::JoinPath(xdg, "flyengine");
    }
    return detail::JoinPath(detail::JoinPath(detail::HomeDirectory(), ".config"), "flyengine");
#endif
}

std::string GetRecentFilePath() {
    return detail::JoinPath(GetAppDataDirectory(), "recent_projects.txt");
}

std::string GetProjectsDirectory() {
#ifdef _WIN32
    std::string base = detail::GetSpecialFolder(CSIDL_PERSONAL);
    if (base.empty()) base = "C:/";
    return base + "\\FlyengineProjects";
#else
    return detail::JoinPath(detail::HomeDirectory(), "FlyengineProjects");
#endif
}

std::string GetProjectFolder(const std::string& name) {
    return detail::JoinPath(GetProjectsDirectory(), name);
}

std::string GetProjectFilePath(const std::string& name) {
    return detail::JoinPath(GetProjectFolder(name), name + ".flyproj");
}

std::string GetAssetsFolder(const std::string& name) {
    return detail::JoinPath(GetProjectFolder(name), "assets");
}

bool IsValidProjectName(const std::string& name) {
    if (name.empty()) return false;
    for (char c : name) {
        if (strchr("<>:\"/\\|?*", c)) return false;
        if ((unsigned char)c < 32) return false;
    }
    return true;
}

std::vector<std::string> LoadRecentPaths() {
    std::vector<std::string> out;
    std::ifstream in(GetRecentFilePath());
    std::string line;
    while (std::getline(in, line)) {
        std::string p = detail::TrimCopy(line);
        if (!p.empty()) out.push_back(p);
    }
    return out;
}

void SaveRecentPaths(const std::vector<std::string>& paths) {
    detail::EnsureDirectory(GetAppDataDirectory());
    std::ofstream out(GetRecentFilePath(), std::ios::trunc);
    for (const auto& p : paths) out << p << "\n";
}

void PushRecent(const std::string& path, std::vector<std::string>& recent) {
    if (path.empty()) return;
    auto it = std::find(recent.begin(), recent.end(), path);
    if (it != recent.end()) recent.erase(it);
    recent.insert(recent.begin(), path);
    constexpr size_t kMax = 10;
    if (recent.size() > kMax) recent.resize(kMax);
    // Persist here rather than leaving it to every caller: the common path is
    // "user opens/creates a project", which immediately tears down this window and
    // would otherwise never write the list back.
    SaveRecentPaths(recent);
}

// Implementation details (theme, widget helpers, .flyproj parsing) live in
// project::detail; the public API below uses them unqualified.
using namespace detail;

// =========================================================================
// Public API functions (inside project namespace)
// =========================================================================

bool CreateProjectFileInternal(const std::string& name, const std::string& templateName);

bool CreateProject(const std::string& name, const std::string& templateName) {
    return CreateProjectFileInternal(name, templateName);
}

void ApplyWindowIcon() {
    constexpr const char* iconPaths[] = { "assets/FlyengineLogo.png", "../assets/FlyengineLogo.png" };
    bool found = false;
    for (const char* path : iconPaths) {
        if (FileExists(path)) {
            found = true;
            TraceLog(LOG_INFO, "ApplyWindowIcon: found %s", path);
            Image icon = LoadImage(path);
            if (icon.data != nullptr) {
                TraceLog(LOG_INFO, "ApplyWindowIcon: loaded %dx%d, format=%d",
                         icon.width, icon.height, icon.format);
                ImageFormat(&icon, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

                Image iconSmall = ImageCopy(icon);
                ImageResize(&iconSmall, 32, 32);
                Image iconLarge = ImageCopy(icon);
                ImageResize(&iconLarge, 256, 256);

                Image icons[2] = { iconSmall, iconLarge };
                SetWindowIcons(icons, 2);
                TraceLog(LOG_INFO, "ApplyWindowIcon: SetWindowIcons called");

                UnloadImage(iconSmall);
                UnloadImage(iconLarge);
                UnloadImage(icon);
            }
        }
        if (!found) {
            TraceLog(LOG_WARNING, "ApplyWindowIcon: FlyengineLogo.png not found in either path; cwd=%s",
                     GetWorkingDirectory());
        }
    }
}

bool ReadProjectHeader(const std::string& projectFolder, Info& outInfo) {
    HeaderFields fields;
    std::ifstream scene;
    if (!ReadProjectFile(projectFolder, fields, scene)) return false;
    outInfo.name = fields.name;
    outInfo.path = projectFolder;
    outInfo.templateName = fields.templateName;
    return true;
}

bool ReadProjectHeaderFromPath(const std::string& projectFilePath, Info& outInfo) {
    HeaderFields fields;
    std::ifstream scene;
    if (!detail::ReadProjectFileAt(projectFilePath, fields, scene)) return false;
    // Report the containing folder as the project path - that's what the rest of
    // the engine treats as the project's identity.
    std::string folder = detail::BaseDirOf(projectFilePath);
    outInfo.name = fields.name;
    outInfo.path = folder;
    outInfo.templateName = fields.templateName;
    return true;
}

bool OpenProjectFileFromPath(const std::string& projectFilePath, Engine& engine,
                             std::vector<ScatteredObject*>& objects,
                             std::vector<std::unique_ptr<ModelGroup>>& models, Info& outInfo,
                             phys::Simulation* physicsSim,
                             terrain::Terrain** outTerrain) {
    HeaderFields fields;
    std::ifstream scene;
    if (!detail::ReadProjectFileAt(projectFilePath, fields, scene)) return false;
    outInfo.name = fields.name;
    outInfo.path = detail::BaseDirOf(projectFilePath);
    outInfo.templateName = fields.templateName;
    return LoadSceneFromStream(scene, engine, objects, models, detail::BaseDirOf(projectFilePath),
                               physicsSim, outTerrain);
}

bool OpenProjectFile(const std::string& projectFolder, Engine& engine,
                     std::vector<ScatteredObject*>& objects,
                     std::vector<std::unique_ptr<ModelGroup>>& models, Info& outInfo,
                     phys::Simulation* physicsSim,
                     terrain::Terrain** outTerrain) {
    HeaderFields fields;
    std::ifstream scene;
    if (!ReadProjectFile(projectFolder, fields, scene)) return false;
    outInfo.name = fields.name;
    outInfo.path = projectFolder;
    outInfo.templateName = fields.templateName;
    // projectFolder is already a directory (both callers hand us the folder), so
    // it IS the base directory for the sidecars (terrain.terrain, city.city,
    // basicterrain.bt). Running it through BaseDirOf() would strip the project
    // folder's own name and point every sidecar load at the parent directory,
    // which is how a freshly saved and reopened project silently lost its
    // terrain/city/basic-terrain on reload while the files were fine on disk.
    return LoadSceneFromStream(scene, engine, objects, models, projectFolder,
                               physicsSim, outTerrain);
}

bool SaveProjectFile(const std::string& projectFolder, const std::vector<ScatteredObject*>& objects,
                     const std::vector<std::unique_ptr<ModelGroup>>& models,
                     terrain::Terrain* terrain) {
    // Saving makes imports permanent: dedupe textures into assets/shared and
    // drop unused imported folders *before* the scene records their paths.
    textureManager::CommitAssets(objects);

    HeaderFields fields;
    // Write back to the file this project already lives in. Falling back to the
    // canonical name only matters for a folder we can't find one in, and picking
    // a *different* name would leave the old file behind as a stale second copy.
    const std::string existing = detail::FindProjectFile(projectFolder);
    const std::string projectFile = existing.empty()
        ? detail::JoinPath(projectFolder,
              projectFolder.substr(projectFolder.find_last_of("\\/") + 1) + ".flyproj")
        : existing;
    {
        std::ifstream scene;
        if (ReadProjectFile(projectFolder, fields, scene)) {
        } else {
            const Info& current = GetCurrentProject();
            fields.name = current.name.empty() ? "Untitled" : current.name;
            fields.templateName = current.templateName.empty() ? "Blank" : current.templateName;
            fields.created = detail::TimeNow();
        }
    }
    fields.modified = detail::TimeNow();

    std::ofstream out(projectFile, std::ios::trunc);
    if (!out.is_open()) return false;
    WriteProjectHeader(out, fields);
    return SaveSceneToStream(out, objects, models, detail::BaseDirOf(projectFile), terrain);
}

std::string SaveProjectFileTemp(const std::string& projectFolder, const std::vector<ScatteredObject*>& objects,
                                const std::vector<std::unique_ptr<ModelGroup>>& models,
                                terrain::Terrain* terrain) {
    HeaderFields fields;
    const std::string name = projectFolder.substr(projectFolder.find_last_of("\\/") + 1);
    const std::string tempFile = detail::JoinPath(projectFolder, name + "_temp.flyproj");
    {
        std::ifstream scene;
        if (ReadProjectFile(projectFolder, fields, scene)) {
        } else {
            const Info& current = GetCurrentProject();
            fields.name = current.name.empty() ? "Untitled" : current.name;
            fields.templateName = current.templateName.empty() ? "Blank" : current.templateName;
            fields.created = detail::TimeNow();
        }
    }
    fields.modified = detail::TimeNow();

    std::ofstream out(tempFile, std::ios::trunc);
    if (!out.is_open()) return "";
    WriteProjectHeader(out, fields);
    if (!SaveSceneToStream(out, objects, models, detail::BaseDirOf(tempFile), terrain)) {
        return "";
    }
    return tempFile;
}

namespace {
    Info g_currentProject;
} // namespace

void SetCurrentProject(const Info& info) { g_currentProject = info; }
const Info& GetCurrentProject() { return g_currentProject; }

Info ShowProjectManager() {
    InitWindow(kW, kH, "Flyengine - Project Manager");
    SetTargetFPS(60);
    ApplyWindowIcon();

    // Arial gives a much crisper UI than raylib's bitmap default. raylib's default
    // font is a 10px bitmap atlas, so anything larger than ~12pt looks blocky -
    // look for a scalable TTF on whichever platform we're on and fall back to the
    // default font if none is found.
    g_font = GetFontDefault();
    const char* fontCandidates[] = {
        "C:/Windows/Fonts/arial.ttf",   // Windows
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",          // Debian/Ubuntu
        "/usr/share/fonts/TTF/DejaVuSans.ttf",                      // Arch
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",                   // Fedora
        "/System/Library/Fonts/Supplemental/Arial.ttf",             // macOS
        "assets/Flyengine.ttf",                                      // bundled
    };
    for (const char* path : fontCandidates) {
        if (!FileExists(path)) continue;
        Font candidate = LoadFontEx(path, 32, nullptr, 0);
        if (candidate.texture.id != 0 && candidate.texture.id != GetFontDefault().texture.id) {
            g_font = candidate;
            SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);
        } else if (candidate.texture.id != 0) {
            UnloadFont(candidate);
        }
        break;
    }

    // =========================================================================
    // 1. LOAD ICON TEXTURE ONCE (Before render loop)
    // =========================================================================
    Texture2D folderTex = { 0 };
    float folderScale = 1.0f;

    Image folderImg = LoadImage("assets/EditorIcons/Folder.png");
    if (folderImg.data == nullptr) folderImg = LoadImage("../assets/EditorIcons/Folder.png");
    if (folderImg.data != nullptr) {
        if (folderImg.height > 0) ImageColorBrightness(&folderImg, 180);
        folderScale = folderImg.height > 0 ? 44.0f / static_cast<float>(folderImg.height) : 1.0f;
        folderTex = LoadTextureFromImage(folderImg);
        UnloadImage(folderImg); // Free CPU RAM immediately after GPU upload
    }

    // recentPaths is the on-disk truth; `recent` is the same list decorated with the
    // header info needed for rendering (display name + whether the folder still exists).
    std::vector<std::string> recentPaths = LoadRecentPaths();
    struct Entry { std::string path; std::string name; bool missing; };
    auto refreshRecent = [&]() {
        std::vector<Entry> entries;
        entries.reserve(recentPaths.size());
        for (const auto& p : recentPaths) {
            Entry e;
            e.path = p;
            Info header;
            if (ReadProjectHeader(p, header)) { e.name = header.name; e.missing = false; }
            else { e.name = p; e.missing = true; }
            entries.push_back(e);
        }
        return entries;
    };
    std::vector<Entry> recent = refreshRecent();

    int selected = -1;
    int scroll = 0;
    std::string lastClickPath;
    double lastClickTime = 0.0;

    NameField nameField;
    NameField promptField;
    std::string chosenTemplate = "Blank";
    EnsureDirectory(GetProjectsDirectory());
    std::string errorMsg;
    double errorUntil = 0.0;

    bool confirmDelete = false;
    std::string deletePath;
    std::string deleteName;

    Info result;
    bool done = false;

    const auto fail = [&](const char* msg) {
        errorMsg = msg;
        errorUntil = GetTime() + 4.0;
    };

    const auto openProject = [&](const std::string& path) -> bool {
        Info info;
        if (!ReadProjectHeader(path, info)) {
            fail("Failed to open project");
            return false;
        }
        result = info;
        PushRecent(path, recentPaths);
        done = true;
        return true;
    };

    while (!done && !WindowShouldClose()) {
        if (GetTime() > errorUntil) errorMsg.clear();

        Vector2 mouse = GetMousePosition();
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);

        constexpr float rowH = 40.0f;
        constexpr int visible = 6;
        const int maxScroll = std::max(0, static_cast<int>(recent.size()) - visible);
        if (scroll > maxScroll) scroll = maxScroll;

        // The folder picker is asynchronous (native dialog on another thread, or the
        // editor's text prompt), so its result only shows up here on a later frame.
        if (!confirmDelete) {
            std::string chosenFolder, chosenError;
            if (PollChosenProjectFolder(chosenFolder, chosenError)) {
                if (!chosenFolder.empty()) openProject(chosenFolder);
                else if (!chosenError.empty()) fail(chosenError.c_str());
            }
        }

        // ---- recent list interaction ----
        if (!confirmDelete) {
            const Rectangle listRec = { 36.0f, 132.0f, 484.0f, static_cast<float>(visible) * rowH };
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, listRec)) {
                const int row = static_cast<int>((mouse.y - listRec.y) / rowH) + scroll;
                if (row >= 0 && row < static_cast<int>(recent.size())) {
                    selected = row;
                    // Double-click (two clicks on the same row within 350 ms) opens it.
                    const double now = GetTime();
                    if (recent[row].path == lastClickPath && now - lastClickTime < 0.35) {
                        openProject(recent[row].path);
                    }
                    lastClickPath = recent[row].path;
                    lastClickTime = now;
                } else {
                    selected = -1;
                }
            }
            const int wheel = static_cast<int>(GetMouseWheelMove());
            if (wheel != 0 && CheckCollisionPointRec(mouse, listRec)) {
                scroll -= wheel;
                scroll = std::max(0, std::min(scroll, maxScroll));
            }
        }

        // ---- draw ----
        BeginDrawing();
        ClearBackground(theme::BG);

        // Header bar
        DrawRectangleRec({ 0, 0, static_cast<float>(kW), 64 }, theme::TITLE);
        DrawLine(0, 64, kW, 64, theme::ACCENT);

        // =========================================================================
        // 2. DRAW TEXTURE IN THE FRAME LOOP
        // =========================================================================
        if (folderTex.id > 0) {
            DrawTextureEx(folderTex, Vector2{ 20.0f, 10.0f }, 0.0f, folderScale, WHITE);
        }

        DrawTextU("Flyengine", 80, 10, 26, theme::TEXT);
        DrawTextU("Project Manager", 80, 40, 14, theme::TEXT_MUTED);

        // Left panel
        DrawRectangleRec({ 20, 84, 520, 456 }, theme::PANEL);
        DrawRectangleLinesEx({ 20, 84, 520, 456 }, 1.0f, theme::BORDER);
        DrawTextU("Recent", 36, 96, 13, theme::ACCENT);
        DrawLine(36, 120, 524, 120, theme::DIVIDER);

        if (recent.empty()) {
            DrawTextU("No recent projects yet.", 36, 150, 14, theme::TEXT_DIM);
            DrawTextU("Create one on the right, or open an", 36, 172, 12, theme::TEXT_DIM);
            DrawTextU("existing .flyproj file.", 36, 188, 12, theme::TEXT_DIM);
        } else {
            for (int i = 0; i < visible; ++i) {
                const int idx = i + scroll;
                if (idx >= static_cast<int>(recent.size())) break;
                const Rectangle row = { 36, 132 + static_cast<float>(i) * rowH, 484, rowH - 4 };
                const bool hovered = CheckCollisionPointRec(mouse, row);
                if (idx == selected) {
                    DrawRectangleRec(row, theme::ACCENT_SOFT);
                    DrawRectangleRec({ row.x - 3, row.y, 3, row.height }, theme::ACCENT);
                } else if (hovered) {
                    DrawRectangleRec(row, ROWHoverColor());
                }

                const Entry& e = recent[idx];
                DrawTextU(e.name.c_str(), row.x + 8, row.y + 4, 15,
                    e.missing ? theme::DANGER : theme::TEXT);

                // Truncate the long path from the right so the tail stays readable.
                std::string p = e.path;
                const float maxW = 468.0f;
                while (!p.empty() && MeasureTextU(p.c_str(), 11) > maxW) p.pop_back();
                if (p != e.path) p += "...";
                DrawTextU(p.c_str(), row.x + 8, row.y + 22, 11,
                    e.missing ? theme::DANGER : theme::TEXT_MUTED);

                if (hovered) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
            }
        }

        // Right panel
        DrawRectangleRec({ 560, 84, 320, 456 }, theme::PANEL);
        DrawRectangleLinesEx({ 560, 84, 320, 456 }, 1.0f, theme::BORDER);
        DrawTextU("NEW PROJECT", 576, 96, 13, theme::ACCENT);
        DrawLine(576, 120, 864, 120, theme::DIVIDER);

        DrawTextU("Project name", 576, 128, 12, theme::TEXT_MUTED);
        DrawTextU("Template", 576, 192, 12, theme::TEXT_MUTED);
        const std::string baseDir = GetProjectsDirectory();
        std::string shownDir = baseDir;
        const float dirMaxW = 276.0f;
        while (!shownDir.empty() && MeasureTextU(shownDir.c_str(), 12) > dirMaxW) {
            shownDir = shownDir.substr(1);
        }
        if (shownDir != baseDir) shownDir = "..." + shownDir;
        DrawRectangleRounded({ 576, 312, 288, 26 }, 0.15f, 4, theme::INPUT);
        DrawRectangleLinesEx({ 576, 312, 288, 26 }, 1.0f, theme::BORDER);
        DrawTextU(shownDir.c_str(), 582, 317, 12, theme::TEXT_MUTED);

        // ---- widgets ----
        const bool openClicked = !confirmDelete && Button({ 36, 458, 234, 28 }, "Open");
        const bool browseClicked = !confirmDelete && Button({ 286, 458, 234, 28 }, "Open Project folder");
        const bool removeClicked = !confirmDelete && Button({ 36, 496, 234, 28 }, "Remove from list");
        const bool deleteClicked = !confirmDelete && DangerButton({ 286, 496, 234, 28 }, "Delete project from disk");

        if (openClicked && selected >= 0 && selected < static_cast<int>(recent.size())) {
            if (recent[selected].missing) fail("Cannot find project on Disk");
            else openProject(recent[selected].path);
        }
        if (browseClicked) {
            BeginChooseProjectOpenPath();
        }
        if (removeClicked && selected >= 0 && selected < static_cast<int>(recentPaths.size())) {
            recentPaths.erase(recentPaths.begin() + selected);
            SaveRecentPaths(recentPaths);
            recent = refreshRecent();
            selected = -1;
        }
        if (deleteClicked && selected >= 0 && selected < static_cast<int>(recent.size())
            && !recent[selected].missing) {
            if (recent[selected].path == GetCurrentProject().path) {
                fail("This project is currently open, Close it first");
            } else {
                deletePath = recent[selected].path;
                deleteName = recent[selected].name;
                confirmDelete = true;
            }
        }

        if (!confirmDelete) {
            DrawTextField({ 576, 148, 288, 26 }, nameField);

            if (TemplateCard({ 576, 212, 139, 58 }, "Blank", "Empty scene", chosenTemplate == "Blank")) {
                chosenTemplate = "Blank";
            }
            if (TemplateCard({ 725, 212, 139, 58 }, "Sample Scene", "Basic Template", chosenTemplate == "Sample")) {
                chosenTemplate = "Sample";
            }

            const bool createClicked = PrimaryButton({ 576, 392, 288, 30 }, "Create Project");
            const bool enterPressed = nameField.focused && IsKeyPressed(KEY_ENTER);

            if (createClicked || enterPressed) {
                const std::string name = TrimCopy(nameField.text);
                if (name.empty()) {
                    fail("Enter a project name first");
                } else if (!IsValidProjectName(name)) {
                    fail("Project name contains invalid characters");
                } else {
                    if (CreateProjectFileInternal(name, chosenTemplate)) {
                        const std::string projectFolder = GetProjectFolder(name);
                        result.name = name;
                        result.path = projectFolder;
                        result.templateName = chosenTemplate;
                        PushRecent(projectFolder, recentPaths);
                        done = true;
                    } else {
                        fail("Failed to create project");
                    }
                }
            }
        }

        if (!errorMsg.empty()) {
            DrawTextU(errorMsg.c_str(), 576, 436, 12, theme::DANGER);
        }

        if (confirmDelete) {
            const int action = DrawDeleteConfirmDialog(deleteName, deletePath);
            if (action == 1) {
                if (DeleteFolderRecursive(deletePath)) {
                    recentPaths.erase(
                        std::remove(recentPaths.begin(), recentPaths.end(), deletePath),
                        recentPaths.end());
                    SaveRecentPaths(recentPaths);
                    recent = refreshRecent();
                    selected = -1;
                    confirmDelete = false;
                } else {
                    fail("Failed to delete project folder");
                    confirmDelete = false;
                }
            } else if (action == 2) {
                confirmDelete = false;
            }
        }

        // No native picker on this host: collect the path by hand. Drawn last so
        // it sits on top of everything else.
        if (!confirmDelete) DrawFolderPromptFallback(promptField);

        EndDrawing();
    }

    // =========================================================================
    // 3. UNLOAD GPU TEXTURE ON WINDOW CLOSE
    // =========================================================================
    if (folderTex.id > 0) UnloadTexture(folderTex);

    if (g_font.texture.id != GetFontDefault().texture.id) UnloadFont(g_font);
    g_font = { 0 };
    CloseWindow();

    return result;
}

// =========================================================================
// CreateProjectFileInternal implementation
// =========================================================================

bool CreateProjectFileInternal(const std::string& name, const std::string& templateName) {
    if (!IsValidProjectName(name)) return false;

    const std::string projectFolder = GetProjectFolder(name);
    const std::string projectFile = GetProjectFilePath(name);
    const std::string assetsFolder = GetAssetsFolder(name);

    // Create directories (project root, assets, assets/3D)
    detail::EnsureDirectory(projectFolder);
    detail::EnsureDirectory(assetsFolder);
    detail::EnsureDirectory(detail::JoinPath(assetsFolder, "3D"));

    // Create .flyproj file
    std::ofstream out(projectFile, std::ios::trunc);
    if (!out.is_open()) return false;

    HeaderFields fields;
    fields.name = name;
    fields.templateName = templateName.empty() ? "Blank" : templateName;
    fields.created = detail::TimeNow();
    fields.modified = fields.created;

    WriteProjectHeader(out, fields);
    // A new project starts with an empty (but valid) scene, so opening it right
    // away goes through the same path as any other project.
    const std::vector<ScatteredObject*> empty;
    const std::vector<std::unique_ptr<ModelGroup>> models;
    // (The scene is written with the new-project lighting; the lighting of whatever is open now is kept.)
    const gfx::LightingSettings openLighting = gfx::Lighting();
    gfx::Lighting() = gfx::NewSceneLighting();
    const bool saved = SaveSceneToStream(out, empty, models, detail::BaseDirOf(projectFile));
    gfx::Lighting() = openLighting;
    if (!saved) return false;
    out.close();

    // Add to recent projects (PushRecent persists the list)
    std::vector<std::string> recent = LoadRecentPaths();
    PushRecent(projectFolder, recent);

    return true;
}

} // namespace project
