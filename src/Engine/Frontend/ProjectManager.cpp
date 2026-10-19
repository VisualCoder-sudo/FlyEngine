// ProjectManager.cpp - the "Projects" hub window.
#include "../../../include/Engine/Scripts/ScriptLauncher.hpp"
#include "../../../include/Engine/Platform/WinBegin.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#endif
#include "../../../include/Engine/Platform/WinEnd.h"

#include "../../../include/Engine/Backend/TextureManager.hpp"
#include "../../../include/Graphics/Vfs.hpp"
#include "../../../include/Engine/Frontend/ProjectManager.hpp"
#include "../../../include/Engine/Backend/AudioAssets.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Engine/Backend/ScenePersistence.hpp"
#include "../../../include/Graphics/Graphics.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine.hpp"
#include "raylib.h"
#include "cJSON.h"

#include <algorithm>
#include <cmath>
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
#include "../../../include/Engine/U8Path.hpp"

namespace fs = std::filesystem;

namespace project {

namespace detail {

    constexpr int kW = 900;
    constexpr int kH = 560;
    constexpr int kMinW = 700;    // below this the two panels no longer fit side by side
    constexpr int kMinH = 460;

    const char* kSignature = "FLYENGINE_PROJECT";
    constexpr int kVersion = 1;
    constexpr int kMaxSceneVersion = 30;   // keep in step with ScenePersistence.cpp

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

    // Shortens `text` to fit `maxW` with a trailing "...", cutting only at UTF-8 code point boundaries.
    // Binary search over the cut point: O(log n) measurements instead of one per removed character.
    std::string EllipsizeU(const std::string& text, float size, float maxW) {
        if (MeasureTextU(text.c_str(), size) <= maxW) return text;
        std::vector<size_t> cuts;   // byte offsets where a code point starts
        for (size_t i = 0; i < text.size(); ++i) if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) cuts.push_back(i);
        size_t lo = 0, hi = cuts.size();   // largest k in [0, hi) such that the first k code points + "..." fit
        while (lo + 1 < hi) {
            const size_t mid = (lo + hi) / 2;
            if (MeasureTextU((text.substr(0, cuts[mid]) + "...").c_str(), size) <= maxW) lo = mid; else hi = mid;
        }
        return text.substr(0, cuts.empty() ? 0 : cuts[lo]) + "...";
    }

    std::string TrimCopy(std::string s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return "";
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    // Last path component, ignoring trailing separators ("/a/b/" -> "b").
    std::string LastComponent(std::string path) {
        while (path.size() > 1 && (path.back() == '\\' || path.back() == '/')) path.pop_back();
        return path.substr(path.find_last_of("\\/") + 1);
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

    // An input stream over a project file: the file on disk, or (in an exported game) the copy inside the
    // executable's archive. Derived from std::istream so it can go straight into LoadSceneFromStream.
    class SceneSource : public std::istream {
    public:
        SceneSource() : std::istream(nullptr) {}
        void open(const std::string& path, std::ios::openmode) {
            opened = false;
            clear();
            if (vfs::Handles(path)) {
                std::vector<unsigned char> bytes;
                if (!vfs::Read(path, bytes)) return;
                // Windows writes project files with CRLF line endings. Reading from disk in text mode folds
                // them back to LF, but archive bytes are raw: left alone, every line would keep a trailing
                // '\r' and "---SCENE---" would never match. So fold them here too.
                std::string text;
                text.reserve(bytes.size());
                for (size_t i = 0; i < bytes.size(); ++i) {
                    if (bytes[i] == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n') continue;
                    text.push_back((char)bytes[i]);
                }
                mem.str(std::move(text));
                rdbuf(&mem);
                opened = true;
            } else {
                if (!file.open(path, std::ios::in)) return;
                rdbuf(&file);
                opened = true;
            }
        }
        bool is_open() const { return opened; }
    private:
        std::filebuf file;
        std::stringbuf mem;
        bool opened = false;
    };

    // Opens the .flyproj at `projectFile` and parses the header lines. On success
    // `scene` is left open and positioned just past the "---SCENE---" marker, ready
    // for the scene blob.
    bool ReadProjectFileAt(const std::string& projectFile, HeaderFields& out, SceneSource& scene) {
        if (projectFile.empty()) return false;
        scene.open(projectFile, std::ios::in);
        if (!scene.is_open()) return false;
        std::string line;
        if (!std::getline(scene, line)) return false;
        std::istringstream head(line);
        std::string sig;
        int ver = 0;
        if (!(head >> sig >> ver) || sig != kSignature) return false;
        if (ver > kVersion) {
            ui::LogAlways("[project] %s was made with a newer Flyengine (project format %d, this build reads %d).",
                          projectFile.c_str(), ver, kVersion);
            return false;
        }
        if (ver != kVersion) return false;
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
        if (!inScene) return false;
        // Cheap sanity check of the scene signature line, so a truncated or foreign file is not listed as healthy.
        // The stream is put back where it was; if it cannot seek, the check is skipped.
        const std::istream::pos_type scenePos = scene.tellg();
        if (scenePos != std::istream::pos_type(-1)) {
            std::string sceneSig;
            int sceneVer = 0;
            const bool ok = static_cast<bool>(scene >> sceneSig >> sceneVer);
            scene.clear();
            scene.seekg(scenePos);
            if (!ok || sceneSig != "SIMPLE_ENGINE_BUILD" || sceneVer < 1) return false;
            if (sceneVer > kMaxSceneVersion) {
                ui::LogAlways("[project] %s was made with a newer Flyengine (scene format %d, this build reads %d).",
                              projectFile.c_str(), sceneVer, kMaxSceneVersion);
                return false;
            }
        }
        return true;
    }

    // Locates the .flyproj that makes `folder` a project. The canonical name is
    // <folder>/<folder>.flyproj, but the recent-projects list stores folders, so
    // a user who renames either the folder or the file would otherwise see their
    // project silently go "missing". Fall back to a lone *.flyproj in the folder.
    // Returns an empty string when there is none, or more than one to choose from.
    std::string FindProjectFile(const std::string& folder) {
        if (folder.empty()) return std::string();
        const std::string name = LastComponent(folder);
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
    bool ReadProjectFile(const std::string& projectFolder, HeaderFields& out, SceneSource& scene) {
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
#ifdef _WIN32
        // HOME is normally unset on Windows; without this the delete guard compared against the working directory.
        if (const char* profile = std::getenv("USERPROFILE"); profile && *profile) return std::string(profile);
#endif
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
    //
    // Refuses anything that isn't plainly a project folder: the recent list can hold any folder the user opened,
    // and a name like ".." or a parent folder that merely contains one .flyproj must never take the surrounding
    // directory with it.
    bool DeleteFolderRecursive(const std::string& rawPath) {
        std::string path = rawPath;
        while (path.size() > 1 && (path.back() == '\\' || path.back() == '/')) path.pop_back();
        if (path.empty()) return false;
        std::error_code ec;
        const fs::path folder = fs::weakly_canonical(U8Path(path), ec);
        if (ec || !folder.has_filename() || !folder.has_parent_path()) return false;
        const fs::path name = folder.filename();
        if (name == "." || name == "..") return false;
        // Never the home folder, the projects directory, or a filesystem root.
        const fs::path home = fs::weakly_canonical(U8Path(HomeDirectory()), ec);
        const fs::path projects = fs::weakly_canonical(U8Path(GetProjectsDirectory()), ec);
        if (folder == home || folder == projects || folder == folder.root_path()) return false;
        // Only a folder that holds its own canonical <name>/<name>.flyproj counts as a project folder.
        if (!fs::is_regular_file(folder / (name.string() + ".flyproj"), ec)) return false;
        const std::uintmax_t removed = fs::remove_all(folder, ec);
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

    void DrawTextField(Rectangle rec, NameField& field, bool enabled = true) {
        Vector2 mouse = GetMousePosition();
        const bool hovered = enabled && CheckCollisionPointRec(mouse, rec);
        if (!enabled) field.focused = false;
        if (enabled && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
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

    // ---- New Project templates ---------------------------------------------------
    // Every folder in assets/templates/ with a <Name>/<Name>.flyproj is a template: data.json gives its name,
    // description and author, thumbnail.png its card image, and deps/ is copied into the new project.

    constexpr float kCardW = 288.0f;
    constexpr float kCardH = 110.0f;
    constexpr float kCardRound = 0.16f;          // DrawRectangleRounded roundness of a card
    constexpr float kThumbBlurMax = 10.0f;       // blur radius (px) right under the text
    constexpr float kThumbBlurReach = 34.0f;     // distance (px) from the text over which the blur fades to 0

    struct TemplateInfo {
        std::string id;     // folder name
        std::string name;
        std::string desc;
        std::string author;
        Texture2D thumb = { 0 };
        std::string thumbFile;   // baked lazily, the first time the picker opens
    };

    std::string TemplatesRoot() { return platform::ResolveAsset("assets/templates"); }

    // Where DrawTemplateCard puts its two text lines, relative to the card.
    Rectangle TitleRect(const std::string& name) { return { 14.0f, 10.0f, MeasureTextU(name.c_str(), 20.0f), 22.0f }; }
    Rectangle DescRect(const std::string& desc) { return { 14.0f, kCardH - 26.0f, MeasureTextU(desc.c_str(), 13.0f), 16.0f }; }

    // Bakes thumbnail.png into a card-sized texture: cover-cropped, blurred by a radius that is largest behind the
    // title and description and fades to 0 with distance from them, with rounded corners cut out of the alpha.
    Texture2D BakeThumbnail(const std::string& file, const std::string& name, const std::string& desc) {
        Texture2D tex = { 0 };
        Image img = LoadImage(file.c_str());
        if (img.data == nullptr) return tex;
        const int w = static_cast<int>(kCardW), h = static_cast<int>(kCardH);
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        const float want = kCardW / kCardH;
        const float have = static_cast<float>(img.width) / static_cast<float>(img.height);
        Rectangle crop = { 0.0f, 0.0f, static_cast<float>(img.width), static_cast<float>(img.height) };
        // Whole numbers only: ImageCrop sizes its buffer from (int)(width*height) but copies (int)width-wide rows.
        if (have > want) { crop.width = std::floor(crop.height * want); crop.x = std::floor((img.width - crop.width) * 0.5f); }
        else { crop.height = std::floor(crop.width / want); crop.y = std::floor((img.height - crop.height) * 0.5f); }
        ImageCrop(&img, crop);
        ImageResize(&img, w, h);

        Color* px = static_cast<Color*>(img.data);
        const Rectangle texts[2] = { TitleRect(name), DescRect(desc) };
        std::vector<int> radius(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float d = 1e9f;
                for (const Rectangle& r : texts) {
                    const float dx = std::max(std::max(r.x - x, 0.0f), x - (r.x + r.width));
                    const float dy = std::max(std::max(r.y - y, 0.0f), y - (r.y + r.height));
                    d = std::min(d, std::hypot(dx, dy));
                }
                const float t = std::min(1.0f, d / kThumbBlurReach);
                radius[static_cast<size_t>(y) * w + x] = static_cast<int>(std::lround(kThumbBlurMax * (1.0f - t * t * (3.0f - 2.0f * t))));
            }
        }
        const auto radiusAt = [&](int x, int y) { return radius[static_cast<size_t>(y) * w + x]; };
        std::vector<Color> tmp(px, px + static_cast<size_t>(w) * h);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const int r = radiusAt(x, y), x0 = std::max(0, x - r), x1 = std::min(w - 1, x + r);
                int sum[3] = { 0, 0, 0 };
                for (int i = x0; i <= x1; i++) { const Color& c = px[y * w + i]; sum[0] += c.r; sum[1] += c.g; sum[2] += c.b; }
                const int n = x1 - x0 + 1;
                tmp[y * w + x] = { static_cast<unsigned char>(sum[0] / n), static_cast<unsigned char>(sum[1] / n), static_cast<unsigned char>(sum[2] / n), 255 };
            }
        }
        for (int x = 0; x < w; x++) {
            for (int y = 0; y < h; y++) {
                const int r = radiusAt(x, y);
                const int y0 = std::max(0, y - r), y1 = std::min(h - 1, y + r);
                int sum[3] = { 0, 0, 0 };
                for (int i = y0; i <= y1; i++) { const Color& c = tmp[i * w + x]; sum[0] += c.r; sum[1] += c.g; sum[2] += c.b; }
                const int n = y1 - y0 + 1;
                px[y * w + x] = { static_cast<unsigned char>(sum[0] / n), static_cast<unsigned char>(sum[1] / n), static_cast<unsigned char>(sum[2] / n), 255 };
            }
        }

        // Rounded corners: antialiased coverage against a circle in each corner.
        const float cr = kCardRound * std::min(kCardW, kCardH) * 0.5f;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const float cx = std::min(std::max(x + 0.5f, cr), kCardW - cr);
                const float cy = std::min(std::max(y + 0.5f, cr), kCardH - cr);
                const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
                const float cover = std::min(1.0f, std::max(0.0f, cr - d + 0.5f));
                px[y * w + x].a = static_cast<unsigned char>(cover * 255.0f);
            }
        }
        tex = LoadTextureFromImage(img);
        UnloadImage(img);
        return tex;
    }

    // Reads data.json's string fields (name, description, author); missing ones stay as they were.
    void ReadTemplateData(const std::string& file, TemplateInfo& t) {
        std::ifstream in(file, std::ios::binary);
        if (!in.is_open()) return;
        std::stringstream buf;
        buf << in.rdbuf();
        const std::string text = buf.str();
        cJSON* root = cJSON_Parse(text.c_str());
        if (root == nullptr) return;
        const auto str = [&](const char* key, std::string& out) {
            const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
            if (cJSON_IsString(item) && item->valuestring) out = item->valuestring;
        };
        str("name", t.name);
        str("description", t.desc);
        str("author", t.author);
        cJSON_Delete(root);
    }

    std::vector<TemplateInfo> LoadTemplates() {
        std::vector<TemplateInfo> list;
        const std::string root = TemplatesRoot();
        if (root.empty()) return list;
        std::error_code ec;
        for (fs::directory_iterator it(U8Path(root), ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            TemplateInfo t;
            t.id = it->path().filename().string();
            const fs::path dir = it->path();
            if (!fs::exists(dir / (t.id + ".flyproj"), ec)) continue;
            t.name = t.id;
            ReadTemplateData((dir / "data.json").string(), t);
            t.thumbFile = (dir / "thumbnail.png").string();
            list.push_back(std::move(t));
        }
        std::sort(list.begin(), list.end(), [](const TemplateInfo& a, const TemplateInfo& b) { return a.name < b.name; });
        return list;
    }

    // Decoding and blurring the thumbnails is the slow part of LoadTemplates, so it waits until they are shown.
    void BakeTemplateThumbnails(std::vector<TemplateInfo>& list) {
        for (TemplateInfo& t : list) {
            if (t.thumb.id == 0 && !t.thumbFile.empty()) t.thumb = BakeThumbnail(t.thumbFile, t.name, t.desc);
            t.thumbFile.clear();
        }
    }

    void UnloadTemplates(std::vector<TemplateInfo>& list) {
        for (TemplateInfo& t : list) if (t.thumb.id > 0) UnloadTexture(t.thumb);
        list.clear();
    }

    void DrawShadowedText(const char* text, float x, float y, float size, Color color) {
        DrawTextU(text, x + 1.0f, y + 1.0f, size, Color{ 0, 0, 0, 200 });
        DrawTextU(text, x, y, size, color);
    }

    // One thumbnail card. True when clicked.
    bool TemplateCard(Rectangle rec, const TemplateInfo& t, bool selected, bool clickable) {
        const bool hovered = clickable && Hovered(rec);
        if (t.thumb.id > 0) DrawTexture(t.thumb, static_cast<int>(rec.x), static_cast<int>(rec.y), WHITE);
        else DrawRectangleRounded(rec, kCardRound, 6, hovered ? ROWHoverColor() : theme::WIDGET);
        DrawRectangleRoundedLinesEx(rec, kCardRound, 6, selected ? 2.0f : 1.0f,
            selected ? theme::ACCENT : (hovered ? theme::BORDER_STRONG : theme::BORDER));
        DrawShadowedText(t.name.c_str(), rec.x + 14.0f, rec.y + 10.0f, 20.0f, theme::TEXT);
        DrawShadowedText(t.desc.c_str(), rec.x + 14.0f, rec.y + rec.height - 26.0f, 13.0f, theme::TEXT);
        if (hovered) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
        return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    }

    // Modal grid of template cards. Returns 1 when `chosen` changed (clicking the chosen card clears it), 2 when closed
    // without a pick (Esc, the Close button or a click outside), 0 while it stays open.
    int DrawTemplatePicker(const std::vector<TemplateInfo>& list, std::string& chosen, int& scroll, bool interactive) {
        const float sw = static_cast<float>(GetScreenWidth()), sh = static_cast<float>(GetScreenHeight());
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{ 0, 0, 0, 160 });
        constexpr float gap = 14.0f;
        // As many columns as fit (1 to 3), and a panel just tall enough for the cards, capped to the window.
        const int cols = std::max(1, std::min(3, static_cast<int>((sw - 40.0f - 32.0f + gap) / (kCardW + gap))));
        const int rows = std::max(1, (static_cast<int>(list.size()) + cols - 1) / cols);
        const float contentH = rows * kCardH + (rows - 1) * gap;
        const float panelW = 32.0f + cols * kCardW + (cols - 1) * gap;
        const float panelH = std::min(sh - 40.0f, std::max(150.0f, 64.0f + contentH));
        const Rectangle panel = { std::floor((sw - panelW) * 0.5f), std::floor((sh - panelH) * 0.5f), panelW, panelH };
        DrawRectangleRounded(panel, 0.04f, 6, theme::PANEL);
        DrawRectangleRoundedLinesEx(panel, 0.04f, 6, 1.0f, theme::BORDER_STRONG);
        DrawTextU("CHOOSE A TEMPLATE", panel.x + 16, panel.y + 14, 13, theme::ACCENT);
        DrawLine(static_cast<int>(panel.x) + 16, static_cast<int>(panel.y) + 38,
            static_cast<int>(panel.x + panel.width) - 16, static_cast<int>(panel.y) + 38, theme::DIVIDER);

        const bool close = Button({ panel.x + panel.width - 88, panel.y + 10, 72, 22 }, "Close") && interactive;
        const Rectangle area = { panel.x + 16, panel.y + 48, panel.width - 32, panel.height - 64 };
        const float maxScroll = std::max(0.0f, contentH - area.height);
        float off = static_cast<float>(scroll);
        if (CheckCollisionPointRec(GetMousePosition(), area)) off -= GetMouseWheelMove() * 36.0f;
        off = std::min(std::max(off, 0.0f), maxScroll);
        scroll = static_cast<int>(off);

        int result = 0;
        BeginScissorMode(static_cast<int>(area.x), static_cast<int>(area.y), static_cast<int>(area.width), static_cast<int>(area.height));
        if (list.empty()) DrawTextU("No templates found in assets/templates", area.x, area.y + 8, 13, theme::TEXT_DIM);
        for (size_t i = 0; i < list.size(); i++) {
            const Rectangle rec = { area.x + (i % cols) * (kCardW + gap), area.y + (i / cols) * (kCardH + gap) - off, kCardW, kCardH };
            const bool visible = rec.y + rec.height > area.y && rec.y < area.y + area.height;
            if (!visible) continue;
            const bool selected = list[i].id == chosen;
            if (TemplateCard(rec, list[i], selected, interactive && CheckCollisionPointRec(GetMousePosition(), area))) {
                chosen = selected ? std::string() : list[i].id;
                result = 1;
            }
        }
        EndScissorMode();

        if (result == 0 && interactive && (close || IsKeyPressed(KEY_ESCAPE)
            || (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(GetMousePosition(), panel)))) result = 2;
        return result;
    }

    // Modal confirmation shown before a project is deleted from disk.
    // Returns 1 if the user confirmed deletion, 2 if they cancelled, 0 otherwise.
    int DrawDeleteConfirmDialog(const std::string& name, const std::string& path) {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{ 0, 0, 0, 140 });

        Rectangle panel = { GetScreenWidth() * 0.5f - 200.0f, GetScreenHeight() * 0.5f - 85.0f, 400.0f, 170.0f };
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

        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{ 0, 0, 0, 140 });

        Rectangle panel = { GetScreenWidth() * 0.5f - 240.0f, GetScreenHeight() * 0.5f - 80.0f, 480.0f, 160.0f };
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
    if (name.empty() || name.size() > 100) return false;
    // "." and ".." would resolve the project folder to the projects directory or its parent (the home folder),
    // and a leading dot hides the folder. Windows silently drops a trailing dot or space, so the folder would
    // no longer match the name.
    if (name[0] == '.' || name.back() == '.' || name.back() == ' ') return false;
    for (char c : name) {
        if (strchr("<>:\"/\\|?*", c)) return false;
        if ((unsigned char)c < 32) return false;
    }
    // Windows reserved device names, with or without an extension ("con", "NUL.txt", "com1").
    std::string stem = name.substr(0, name.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (char& c : stem) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return false;
    if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) && stem[3] >= '1' && stem[3] <= '9') return false;
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

static std::string TutorialMarkerPath() {
    return detail::JoinPath(GetAppDataDirectory(), "tutorial_done");
}

bool TutorialDone() {
    std::error_code ec;
    return fs::exists(U8Path(TutorialMarkerPath()), ec);
}

void SetTutorialDone(bool done) {
    if (done) {
        detail::EnsureDirectory(GetAppDataDirectory());
        std::ofstream(TutorialMarkerPath(), std::ios::trunc) << "1\n";
    } else {
        std::error_code ec;
        fs::remove(U8Path(TutorialMarkerPath()), ec);
    }
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

void ApplyWindowIconImage(const Image& source) {
    Image icon = ImageCopy(source);
    ImageFormat(&icon, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    Image iconSmall = ImageCopy(icon);
    ImageResize(&iconSmall, 32, 32);
    Image iconLarge = ImageCopy(icon);
    ImageResize(&iconLarge, 256, 256);

    Image icons[2] = { iconSmall, iconLarge };
    SetWindowIcons(icons, 2);

    UnloadImage(iconSmall);
    UnloadImage(iconLarge);
    UnloadImage(icon);
}

void ApplyWindowIcon() {
    // The engine's data root first (works from any working directory, e.g. inside an AppImage), then the cwd guesses.
    const std::string resolved = platform::ResolveAsset("assets/FlyengineLogo.png");
    const char* iconPaths[] = { resolved.c_str(), "assets/FlyengineLogo.png", "../assets/FlyengineLogo.png" };
    bool found = false;
    for (const char* path : iconPaths) {
        if (path[0] == '\0') continue;
        if (FileExists(path)) {
            found = true;
            TraceLog(LOG_INFO, "ApplyWindowIcon: found %s", path);
            Image icon = LoadImage(path);
            if (icon.data != nullptr) {
                TraceLog(LOG_INFO, "ApplyWindowIcon: loaded %dx%d, format=%d",
                         icon.width, icon.height, icon.format);
                ApplyWindowIconImage(icon);
                TraceLog(LOG_INFO, "ApplyWindowIcon: SetWindowIcons called");
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
    detail::SceneSource scene;
    if (!ReadProjectFile(projectFolder, fields, scene)) return false;
    outInfo.name = fields.name;
    outInfo.path = projectFolder;
    outInfo.templateName = fields.templateName;
    return true;
}

bool ReadProjectHeaderFromPath(const std::string& projectFilePath, Info& outInfo) {
    HeaderFields fields;
    detail::SceneSource scene;
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
    detail::SceneSource scene;
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
    detail::SceneSource scene;
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
    // Play never writes the project: whatever scripts do while it runs stays in the running copy.
    if (ui::IsPlayActive()) {
        ui::LogAlways("[Play] Not saving: the project is read-only while Play is running.");
        return false;
    }
    // Saving makes imports permanent: dedupe textures into assets/shared and
    // drop unused imported folders *before* the scene records their paths.
    textureManager::CommitAssets(objects);
    scriptLauncher::CommitPendingScripts();

    HeaderFields fields;
    // Write back to the file this project already lives in. Falling back to the
    // canonical name only matters for a folder we can't find one in, and picking
    // a *different* name would leave the old file behind as a stale second copy.
    const std::string existing = detail::FindProjectFile(projectFolder);
    const std::string projectFile = existing.empty()
        ? detail::JoinPath(projectFolder,
              detail::LastComponent(projectFolder) + ".flyproj")
        : existing;
    {
        detail::SceneSource scene;
        if (ReadProjectFile(projectFolder, fields, scene)) {
        } else {
            const Info& current = GetCurrentProject();
            fields.name = current.name.empty() ? "Untitled" : current.name;
            fields.templateName = current.templateName.empty() ? "Blank" : current.templateName;
            fields.created = detail::TimeNow();
        }
    }
    fields.modified = detail::TimeNow();

    // Write next to the project file and rename over it, so a failed or interrupted save leaves the old file intact.
    const std::string writeFile = projectFile + ".tmp";
    std::ofstream out(writeFile, std::ios::trunc);
    if (!out.is_open()) return false;
    WriteProjectHeader(out, fields);
    bool saved = SaveSceneToStream(out, objects, models, detail::BaseDirOf(projectFile), terrain);
    out.close();
    saved = saved && !out.fail();
    std::error_code renameEc;
    if (saved) fs::rename(U8Path(writeFile), U8Path(projectFile), renameEc);
    if (!saved || renameEc) {
        fs::remove(U8Path(writeFile), renameEc);
        return false;
    }
    if (saved) {
        // The scene is on disk, so what it refers to is settled: imported sounds are permanent now, and the ones
        // nothing mentions any more go.
        for (const std::string& gone : audioassets::Commit(projectFolder)) ui::LogAlways("[audio] removed unused %s", gone.c_str());
    }
    return saved;
}

std::string SaveProjectFileTemp(const std::string& projectFolder, const std::vector<ScatteredObject*>& objects,
                                const std::vector<std::unique_ptr<ModelGroup>>& models,
                                terrain::Terrain* terrain) {
    HeaderFields fields;
    const std::string name = detail::LastComponent(projectFolder);
    const std::string tempFile = detail::JoinPath(projectFolder, name + "_temp.flyproj");
    {
        detail::SceneSource scene;
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

static void InstallTutorialPlugin(const std::string& projectFolder);

Info ShowProjectManager() {
    InitWindow(kW, kH, "Flyengine - Project Manager");
    SetWindowMinSize(kMinW, kMinH);
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

    Image folderImg = {};
    if (const std::string resolved = platform::ResolveAsset("assets/EditorIcons/Folder.png"); !resolved.empty()) folderImg = LoadImage(resolved.c_str());
    if (folderImg.data == nullptr) folderImg = LoadImage("assets/EditorIcons/Folder.png");
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
    struct Entry { std::string path; std::string name; bool missing; std::string shownName, shownPath; float shownW = -1.0f; };
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
    std::string chosenTemplate;   // empty: no template, the project starts with an empty scene
    std::vector<TemplateInfo> templates = LoadTemplates();
    bool pickerOpen = false;
    int pickerScroll = 0;
    bool pickerJustOpened = false;   // the click that opened the picker must not also pick a card under it
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
        SetExitKey(pickerOpen ? KEY_NULL : KEY_ESCAPE);   // Esc closes the template picker instead of the window
        const bool modal = confirmDelete || pickerOpen;
        pickerJustOpened = false;

        Vector2 mouse = GetMousePosition();
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);

        // The layout follows the window: the Recent panel takes what the fixed-ish New Project panel leaves.
        const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
        const float rw = std::min(420.0f, std::max(320.0f, W * 0.32f));            // New Project panel width
        const float rx = W - 20.0f - rw;                                            // its left edge
        const float lw = rx - 20.0f - 20.0f;                                        // Recent panel width
        const float bottom = H - 20.0f;                                             // both panels end here
        const float btnW = (lw - 32.0f - 16.0f) * 0.5f;
        const float btnY2 = bottom - 44.0f, btnY1 = bottom - 82.0f;                 // two rows of buttons under the list
        const float ix = rx + 16.0f, iw = rw - 32.0f;                               // New Project panel contents

        constexpr float rowH = 40.0f;
        const int visible = std::max(1, static_cast<int>((btnY1 - 12.0f - 132.0f) / rowH));
        const int maxScroll = std::max(0, static_cast<int>(recent.size()) - visible);
        if (scroll > maxScroll) scroll = maxScroll;

        // The folder picker is asynchronous (native dialog on another thread, or the
        // editor's text prompt), so its result only shows up here on a later frame.
        if (!modal) {
            std::string chosenFolder, chosenError;
            if (PollChosenProjectFolder(chosenFolder, chosenError)) {
                if (!chosenFolder.empty()) openProject(chosenFolder);
                else if (!chosenError.empty()) fail(chosenError.c_str());
            }
        }

        // ---- recent list interaction ----
        if (!modal) {
            const Rectangle listRec = { 36.0f, 132.0f, lw - 32.0f, static_cast<float>(visible) * rowH };
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
        DrawRectangleRec({ 0, 0, W, 64 }, theme::TITLE);
        DrawLine(0, 64, static_cast<int>(W), 64, theme::ACCENT);

        // =========================================================================
        // 2. DRAW TEXTURE IN THE FRAME LOOP
        // =========================================================================
        if (folderTex.id > 0) {
            DrawTextureEx(folderTex, Vector2{ 20.0f, 10.0f }, 0.0f, folderScale, WHITE);
        }

        DrawTextU("Flyengine", 80, 10, 26, theme::TEXT);
        DrawTextU("Project Manager", 80, 40, 14, theme::TEXT_MUTED);

        // Left panel
        DrawRectangleRec({ 20, 84, lw, bottom - 84.0f }, theme::PANEL);
        DrawRectangleLinesEx({ 20, 84, lw, bottom - 84.0f }, 1.0f, theme::BORDER);
        DrawTextU("Recent", 36, 96, 13, theme::ACCENT);
        DrawLine(36, 120, static_cast<int>(20.0f + lw - 16.0f), 120, theme::DIVIDER);

        if (recent.empty()) {
            DrawTextU("No recent projects yet.", 36, 150, 14, theme::TEXT_DIM);
            DrawTextU("Create one on the right, or open an", 36, 172, 12, theme::TEXT_DIM);
            DrawTextU("existing .flyproj file.", 36, 188, 12, theme::TEXT_DIM);
        } else {
            for (int i = 0; i < visible; ++i) {
                const int idx = i + scroll;
                if (idx >= static_cast<int>(recent.size())) break;
                const Rectangle row = { 36, 132 + static_cast<float>(i) * rowH, lw - 32.0f, rowH - 4 };
                const bool hovered = CheckCollisionPointRec(mouse, row);
                if (idx == selected) {
                    DrawRectangleRec(row, theme::ACCENT_SOFT);
                    DrawRectangleRec({ row.x - 3, row.y, 3, row.height }, theme::ACCENT);
                } else if (hovered) {
                    DrawRectangleRec(row, ROWHoverColor());
                }

                Entry& e = recent[idx];
                // Truncated text is cached per row and only recomputed when the panel width changes.
                const float maxW = lw - 48.0f;
                if (e.shownW != maxW) {
                    e.shownName = EllipsizeU(e.name, 15, maxW);
                    e.shownPath = EllipsizeU(e.path, 11, maxW);
                    e.shownW = maxW;
                }
                DrawTextU(e.shownName.c_str(), row.x + 8, row.y + 4, 15,
                    e.missing ? theme::DANGER : theme::TEXT);
                DrawTextU(e.shownPath.c_str(), row.x + 8, row.y + 22, 11,
                    e.missing ? theme::DANGER : theme::TEXT_MUTED);

                if (hovered) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
            }
        }

        // Right panel
        DrawRectangleRec({ rx, 84, rw, 270 }, theme::PANEL);
        DrawRectangleLinesEx({ rx, 84, rw, 270 }, 1.0f, theme::BORDER);
        DrawTextU("NEW PROJECT", ix, 96, 13, theme::ACCENT);
        DrawLine(static_cast<int>(ix), 120, static_cast<int>(ix + iw), 120, theme::DIVIDER);

        DrawTextU("Project name", ix, 128, 12, theme::TEXT_MUTED);
        DrawTextU("Template", ix, 192, 12, theme::TEXT_MUTED);
        const std::string baseDir = GetProjectsDirectory();
        std::string shownDir = baseDir;
        const float dirMaxW = iw - 12.0f;
        while (!shownDir.empty() && MeasureTextU(shownDir.c_str(), 12) > dirMaxW) {
            shownDir = shownDir.substr(1);
        }
        if (shownDir != baseDir) shownDir = "..." + shownDir;
        DrawRectangleRounded({ ix, 256, iw, 26 }, 0.15f, 4, theme::INPUT);
        DrawRectangleLinesEx({ ix, 256, iw, 26 }, 1.0f, theme::BORDER);
        DrawTextU(shownDir.c_str(), ix + 6, 261, 12, theme::TEXT_MUTED);

        // ---- widgets ----
        const bool openClicked = !modal && Button({ 36, btnY1, btnW, 28 }, "Open");
        const bool browseClicked = !modal && Button({ 36 + btnW + 16, btnY1, btnW, 28 }, "Open Project folder");
        const bool removeClicked = !modal && Button({ 36, btnY2, btnW, 28 }, "Remove from list");
        const bool deleteClicked = !modal && DangerButton({ 36 + btnW + 16, btnY2, btnW, 28 }, "Delete project from disk");

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
            // Compare canonical paths (trailing separators stripped) so a trailing slash cannot sneak past the check.
            const auto canon = [](std::string q) {
                while (q.size() > 1 && (q.back() == '\\' || q.back() == '/')) q.pop_back();
                std::error_code cec;
                return fs::weakly_canonical(U8Path(q), cec);
            };
            const std::string& curPath = GetCurrentProject().path;
            if (recent[selected].path == curPath || (!curPath.empty() && canon(recent[selected].path) == canon(curPath))) {
                fail("This project is currently open, Close it first");
            } else {
                deletePath = recent[selected].path;
                deleteName = recent[selected].name;
                confirmDelete = true;
            }
        }

        if (!confirmDelete) {
            DrawTextField({ ix, 148, iw, 26 }, nameField, !pickerOpen);

            std::string templateLabel = "Choose from template";
            for (const TemplateInfo& t : templates) if (t.id == chosenTemplate) templateLabel = "Template: " + t.name;
            if (!modal && Button({ ix, 212, iw, 30 }, templateLabel.c_str())) { pickerOpen = true; pickerJustOpened = true; }
            else if (modal) Button({ ix, 212, iw, 30 }, templateLabel.c_str());

            const bool createClicked = !pickerOpen && PrimaryButton({ ix, 298, iw, 30 }, "Create Project");
            const bool enterPressed = !pickerOpen && nameField.focused && IsKeyPressed(KEY_ENTER);

            if (createClicked || enterPressed) {
                const std::string name = TrimCopy(nameField.text);
                if (name.empty()) {
                    fail("Enter a project name first");
                } else if (!IsValidProjectName(name)) {
                    fail("Project name contains invalid characters");
                } else if (std::error_code existsEc; fs::exists(U8Path(GetProjectFilePath(name)), existsEc)) {
                    fail("A project with that name already exists");
                } else {
                    if (CreateProjectFileInternal(name, chosenTemplate)) {
                        const std::string projectFolder = GetProjectFolder(name);
                        result.name = name;
                        result.path = projectFolder;
                        result.templateName = chosenTemplate.empty() ? "Blank" : chosenTemplate;
                        PushRecent(projectFolder, recentPaths);
                        done = true;
                    } else {
                        fail("Failed to create project");
                    }
                }
            }
        }

        if (!errorMsg.empty()) {
            DrawTextU(errorMsg.c_str(), ix, 336, 12, theme::DANGER);
        }

        // "Get started" card in the space under the New Project panel; pulses until the tutorial is finished.
        {
            const float cy = 354.0f + 16.0f;
            const Rectangle card = { rx, cy, rw, bottom - cy };
            if (card.height >= 110.0f) {
                const bool fresh = !TutorialDone();
                const float pulse = fresh ? 0.5f + 0.5f * std::sin(static_cast<float>(GetTime()) * 3.0f) : 0.0f;
                DrawRectangleRec(card, theme::PANEL);
                Color border = fresh ? theme::ACCENT : theme::BORDER;
                border.a = static_cast<unsigned char>(fresh ? 120 + 135 * pulse : 255);
                DrawRectangleLinesEx(card, fresh ? 2.0f : 1.0f, border);
                DrawTextU("GET STARTED", ix, card.y + 12, 13, theme::ACCENT);
                if (fresh) {
                    const float bw = static_cast<float>(MeasureTextU("NEW", 11)) + 12.0f;
                    DrawRectangleRounded({ ix + iw - bw, card.y + 10, bw, 18 }, 0.3f, 4, theme::ACCENT);
                    DrawTextU("NEW", ix + iw - bw + 6, card.y + 14, 11, theme::BG);
                }
                DrawTextU("Get started with Flyengine!", ix, card.y + 36, 15, theme::TEXT);
                DrawTextU("A guided demo: meshes, properties,", ix, card.y + 60, 12, theme::TEXT_MUTED);
                DrawTextU("worldbuilding, plugins, C++ and visual", ix, card.y + 76, 12, theme::TEXT_MUTED);
                DrawTextU("scripts, and exporting your game.", ix, card.y + 92, 12, theme::TEXT_MUTED);
                const float by = card.y + card.height - 46.0f;
                const bool startClicked = !modal && PrimaryButton({ ix, by, iw, 30 }, "Start Tutorial");
                if (startClicked) {
                    // Reuse an earlier tutorial project, otherwise make one from a template that has a scene and scripts.
                    const std::string name = "Tutorial";
                    const std::string folder = GetProjectFolder(name);
                    std::error_code ec;
                    const bool exists = fs::exists(U8Path(GetProjectFilePath(name)), ec);
                    if (exists || CreateProjectFileInternal(name, "CoinDash")) {
                        InstallTutorialPlugin(folder);
                        result.name = name;
                        result.path = folder;
                        result.templateName = "CoinDash";
                        result.startTutorial = true;
                        PushRecent(folder, recentPaths);
                        done = true;
                    } else {
                        fail("Failed to create the tutorial project");
                    }
                }
            }
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
        if (!modal) DrawFolderPromptFallback(promptField);

        if (pickerOpen) {
            BakeTemplateThumbnails(templates);
            const int action = DrawTemplatePicker(templates, chosenTemplate, pickerScroll, !pickerJustOpened);
            if (action != 0) pickerOpen = false;
        }

        EndDrawing();
    }

    // =========================================================================
    // 3. UNLOAD GPU TEXTURE ON WINDOW CLOSE
    // =========================================================================
    if (folderTex.id > 0) UnloadTexture(folderTex);
    UnloadTemplates(templates);

    if (g_font.texture.id != GetFontDefault().texture.id) UnloadFont(g_font);
    g_font = { 0 };
    CloseWindow();

    return result;
}

// =========================================================================
// CreateProjectFileInternal implementation
// =========================================================================

// A template is the folder assets/templates/<Id>/ holding <Id>.flyproj, data.json, thumbnail.png and deps/.
// Empty when `templateName` is not one of them (Blank, or an unknown name).
static std::string TemplateProjectFile(const std::string& templateName) {
    if (templateName.empty() || templateName.find_first_of("/\\.") != std::string::npos) return std::string();
    return platform::ResolveAsset("assets/templates/" + templateName + "/" + templateName + ".flyproj");
}

// Copies a template's deps/ (scripts, terrain, foliage) into the root of a new project folder.
static bool CopyTemplateDeps(const std::string& templateProjectFile, const std::string& projectFolder) {
    std::error_code ec;
    const fs::path from = fs::path(U8Path(templateProjectFile)).parent_path() / "deps";
    const fs::path to = U8Path(projectFolder);
    if (!fs::exists(from, ec)) return true;
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path rel = fs::relative(it->path(), from, ec);
        if (ec) return false;
        if (it->is_directory(ec)) { fs::create_directories(to / rel, ec); continue; }
        fs::create_directories((to / rel).parent_path(), ec);
        // A file already in the folder is the user's; the template never replaces it.
        fs::copy_file(it->path(), to / rel, fs::copy_options::skip_existing, ec);
        if (ec) return false;
    }
    return !ec;
}

// The tutorial's sample plugin: its source, the API header and a README go into <project>/plugins/nat/HelloPlugin/,
// and it is built so the editor loads it on startup. A missing compiler is not an error: the tour says how to build it.
static void InstallTutorialPlugin(const std::string& projectFolder) {
    const fs::path dir = fs::path(U8Path(projectFolder)) / "plugins" / "nat" / "HelloPlugin";
    std::error_code ec;
    if (fs::exists(dir / "src" / "hello_plugin.c", ec)) return;
    const std::string src = platform::ResolveAsset("assets/tutorial/HelloPlugin/hello_plugin.c");
    const std::string hdr = platform::ResolveAsset("assets/tutorial/HelloPlugin/include/CPluginAPI.h");
    const std::string readme = platform::ResolveAsset("assets/tutorial/HelloPlugin/README.md");
    if (src.empty() || hdr.empty()) return;
    fs::create_directories(dir / "src", ec);
    fs::create_directories(dir / "include", ec);
    fs::create_directories(dir / "build", ec);
    fs::copy_file(U8Path(src), dir / "src" / "hello_plugin.c", fs::copy_options::overwrite_existing, ec);
    fs::copy_file(U8Path(hdr), dir / "include" / "CPluginAPI.h", fs::copy_options::overwrite_existing, ec);
    if (!readme.empty()) fs::copy_file(U8Path(readme), dir / "README.md", fs::copy_options::overwrite_existing, ec);
    // id.json, plugin.json and icon.png make it a valid plugin for `flyplugin validate`.
    for (const char* meta : { "id.json", "plugin.json", "icon.png" }) {
        const std::string from = platform::ResolveAsset(std::string("assets/tutorial/HelloPlugin/") + meta);
        if (!from.empty()) fs::copy_file(U8Path(from), dir / meta, fs::copy_options::overwrite_existing, ec);
    }
#ifdef _WIN32
    const std::string cc = "gcc", lib = "hello_plugin.dll";
    const std::vector<std::string> args = { "-shared", "-I", (dir / "include").string(), "-o", (dir / "build" / lib).string(), (dir / "src" / "hello_plugin.c").string() };
#else
    const std::string cc = "cc", lib = "hello_plugin.so";
    const std::vector<std::string> args = { "-shared", "-fPIC", "-I", (dir / "include").string(), "-o", (dir / "build" / lib).string(), (dir / "src" / "hello_plugin.c").string() };
#endif
    platform::RunProcessCapture(cc, args, 20000);
}

bool CreateProjectFileInternal(const std::string& name, const std::string& templateName) {
    if (!IsValidProjectName(name)) return false;

    const std::string projectFolder = GetProjectFolder(name);
    const std::string projectFile = GetProjectFilePath(name);
    const std::string assetsFolder = GetAssetsFolder(name);

    // Never replace a project that is already there: the create flow checks first, this keeps other callers safe.
    {
        std::error_code ec;
        if (fs::exists(U8Path(projectFile), ec) || !detail::FindProjectFile(projectFolder).empty()) return false;
    }

    // A named template must exist; check before anything is written.
    const std::string templateId = templateName.empty() ? "Blank" : templateName;
    const std::string templateFile = TemplateProjectFile(templateId);
    if (templateFile.empty() && templateId != "Blank") {
        TraceLog(LOG_WARNING, "PROJECT: unknown template '%s'", templateId.c_str());
        return false;
    }

    // Create directories (project root, assets, assets/3D)
    detail::EnsureDirectory(projectFolder);
    detail::EnsureDirectory(assetsFolder);
    detail::EnsureDirectory(detail::JoinPath(assetsFolder, "3D"));

    // Create .flyproj file
    std::ofstream out(projectFile, std::ios::trunc);
    if (!out.is_open()) return false;

    HeaderFields fields;
    fields.name = name;
    fields.templateName = templateId;
    fields.created = detail::TimeNow();
    fields.modified = fields.created;

    WriteProjectHeader(out, fields);
    // A template starts from its scene and deps/.
    if (const std::string& sample = templateFile; !sample.empty()) {
        HeaderFields sampleFields;
        SceneSource scene;
        if (!ReadProjectFileAt(sample, sampleFields, scene)) return false;
        out << scene.rdbuf();
        out.close();
        if (!CopyTemplateDeps(sample, projectFolder)) return false;
        std::vector<std::string> recent = LoadRecentPaths();
        PushRecent(projectFolder, recent);
        return true;
    }
    // A new project starts with an empty (but valid) scene, so opening it right
    // away goes through the same path as any other project.
    const std::unique_ptr<ScatteredObject> baseplate = MakeBaseplate();
    const std::vector<ScatteredObject*> empty{ baseplate.get() };
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

