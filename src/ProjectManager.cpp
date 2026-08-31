// ProjectManager.cpp — the "Projects" hub window.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CloseWindow Win32CloseWindow
#define ShowCursor Win32ShowCursor
#define Rectangle Win32Rectangle
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

#include "ProjectManager.hpp"
#include "ScenePersistence.hpp"
#include "raylib.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace project {
    namespace {

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

        // Directory containing `path`, with trailing separators stripped so it
        // composes cleanly with relative model paths (GetDirectoryPath keeps one).
        std::string BaseDirOf(const std::string& path) {
            std::string dir = GetDirectoryPath(path.c_str());
            while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
            return dir;
        }

        std::string GetSpecialFolder(int csidl) {
            char buf[MAX_PATH] = { 0 };
            if (SUCCEEDED(SHGetFolderPathA(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buf))) {
                return std::string(buf);
            }
            return std::string();
        }

        std::string GetAppDataDirectory() {
            std::string base = GetSpecialFolder(CSIDL_APPDATA);
            if (base.empty()) base = "C:/Flyengine";
            return base + "\\Flyengine";
        }

        std::string GetRecentFilePath() {
            return GetAppDataDirectory() + "\\recent_projects.txt";
        }

        std::string GetProjectsDirectory() {
            std::string base = GetSpecialFolder(CSIDL_PERSONAL);
            if (base.empty()) base = "C:/";
            return base + "\\FlyengineProjects";
        }

        std::string GetProjectFolder(const std::string& name) {
            return GetProjectsDirectory() + "\\" + name;
        }

        std::string GetProjectFilePath(const std::string& name) {
            return GetProjectFolder(name) + "\\" + name + ".flyproj";
        }

        std::string GetAssetsFolder(const std::string& name) {
            return GetProjectFolder(name) + "\\assets";
        }

        std::string ProjectFileName(const std::string& name) {
            std::string n = TrimCopy(name);
            const std::string ext = ".flyproj";
            if (n.size() >= ext.size() && n.compare(n.size() - ext.size(), ext.size(), ext) == 0) {
                n = n.substr(0, n.size() - ext.size());
            }
            return TrimCopy(n) + ext;
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
                std::string p = TrimCopy(line);
                if (!p.empty()) out.push_back(p);
            }
            return out;
        }

        void SaveRecentPaths(const std::vector<std::string>& paths) {
            CreateDirectoryA(GetAppDataDirectory().c_str(), nullptr);
            std::ofstream out(GetRecentFilePath(), std::ios::trunc);
            for (const auto& p : paths) out << p << "\n";
        }

        void PushRecent(const std::string& path, std::vector<std::string>& recent) {
            auto it = std::find(recent.begin(), recent.end(), path);
            if (it != recent.end()) recent.erase(it);
            recent.insert(recent.begin(), path);
            constexpr size_t kMax = 10;
            if (recent.size() > kMax) recent.resize(kMax);
            SaveRecentPaths(recent);
        }

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

        long long TimeNow() {
            return static_cast<long long>(time(nullptr));
        }

        struct HeaderFields {
            std::string name;
            std::string templateName;
            long long created = 0;
            long long modified = 0;
        };

// Opens the .flyproj inside `projectFolder` and parses the header lines. On success `scene` is left open
// and positioned just past the "---SCENE---" marker, ready for the scene blob.
        bool ReadProjectFile(const std::string& projectFolder, HeaderFields& out, std::ifstream& scene) {
            std::string name = projectFolder.substr(projectFolder.find_last_of("\\/") + 1);
            std::string path = projectFolder + "\\" + name + ".flyproj";
            scene.open(path, std::ios::in);
            if (!scene.is_open()) return false;
            std::string line;
            if (!std::getline(scene, line)) return false;
            std::istringstream head(line);
            std::string sig;
            int ver = 0;
            if (!(head >> sig >> ver) || sig != kSignature || ver != kVersion) return false;
            bool inScene = false;
            while (std::getline(scene, line)) {
                if (line == "---SCENE---") { inScene = true; break; }
                std::string key, value;
                if (ParseHeaderLine(line, key, value)) {
                    if (key == "Name") out.name = value;
                    else if (key == "Template") out.templateName = value;
                    else if (key == "Created") out.created = atoll(value.c_str());
                    else if (key == "Modified") out.modified = atoll(value.c_str());
                }
            }
            return inScene;
        }

        void WriteProjectHeader(std::ostream& out, const HeaderFields& fields) {
            out << kSignature << " " << kVersion << "\n";
            out << "Name = \"" << fields.name << "\"\n";
            out << "Template = \"" << fields.templateName << "\"\n";
            out << "Created = " << fields.created << "\n";
            out << "Modified = " << fields.modified << "\n";
            out << "---SCENE---\n";
        }

        void GenerateSampleObjects(std::vector<std::unique_ptr<ScatteredObject>>& out) {
            constexpr Color palette[] = { BLUE, GREEN, GOLD, PURPLE, ORANGE, LIME, DARKBLUE, MAGENTA };
            constexpr int paletteSize = static_cast<int>(sizeof(palette) / sizeof(palette[0]));
            for (int i = 0; i < 30; ++i) {
                const float x = static_cast<float>(GetRandomValue(-30, 30));
                const float z = static_cast<float>(GetRandomValue(-30, 30));
                if (x * x + z * z < 9.0f) continue;
                const float w = static_cast<float>(GetRandomValue(8, 20)) * 0.1f;
                const float h = static_cast<float>(GetRandomValue(8, 30)) * 0.1f;
                const float l = static_cast<float>(GetRandomValue(8, 20)) * 0.1f;
                out.push_back(std::make_unique<ScatteredObject>(
                    Vector3{ x, h * 0.5f, z }, Vector3{ w, h, l },
                    palette[GetRandomValue(0, paletteSize - 1)], ShapeType::Cube));
            }
        }

        std::vector<ScatteredObject*> RawPointers(std::vector<std::unique_ptr<ScatteredObject>>& owned) {
            std::vector<ScatteredObject*> raw;
            raw.reserve(owned.size());
            for (auto& o : owned) raw.push_back(o.get());
            return raw;
        }

        bool CreateProjectFile(const std::string& name, const std::string& templateName) {
            std::string projectFolder = GetProjectFolder(name);
            std::string projectFile = GetProjectFilePath(name);

            CreateDirectoryA(projectFolder.c_str(), nullptr);
            CreateDirectoryA((projectFolder + "\\assets").c_str(), nullptr);
            CreateDirectoryA((projectFolder + "\\assets\\3D").c_str(), nullptr);

            HeaderFields fields;
            fields.name = name;
            fields.templateName = templateName;
            fields.created = TimeNow();
            fields.modified = fields.created;

            std::ofstream out(projectFile, std::ios::trunc);
            if (!out.is_open()) return false;
            WriteProjectHeader(out, fields);

            std::vector<std::unique_ptr<ModelGroup>> models;
            std::vector<std::unique_ptr<ScatteredObject>> sample;
            if (templateName == "Sample") {
                GenerateSampleObjects(sample);
                std::vector<ScatteredObject*> raw = RawPointers(sample);
                return SaveSceneToStream(out, raw, models, BaseDirOf(projectFile));
            }
            std::vector<ScatteredObject*> empty;
            return SaveSceneToStream(out, empty, models, BaseDirOf(projectFile));
        }

        struct FolderState { std::string initial; };

        int CALLBACK BrowseCallbackProc(HWND hwnd, UINT uMsg, LPARAM, LPARAM lpData) {
            if (uMsg == BFFM_INITIALIZED) {
                auto* st = reinterpret_cast<FolderState*>(lpData);
                if (st && !st->initial.empty()) {
                    SendMessageA(hwnd, BFFM_SETSELECTION, TRUE, reinterpret_cast<LPARAM>(st->initial.c_str()));
                }
            }
            return 0;
        }

        std::string ChooseProjectOpenPath() {
            FolderState st{ GetProjectsDirectory() };
            BROWSEINFOA bi{};
            bi.lpfn = BrowseCallbackProc;
            bi.lParam = reinterpret_cast<LPARAM>(&st);
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
            if (!pidl) return std::string();
            char buf[MAX_PATH] = { 0 };
            const bool ok = SHGetPathFromIDListA(pidl, buf);
            CoTaskMemFree(pidl);
            if (!ok) return std::string();

            std::string folder = std::string(buf);
            std::string name = folder.substr(folder.find_last_of("\\/") + 1);
            std::string projectFile = folder + "\\" + name + ".flyproj";
            if (FileExists(projectFile.c_str())) {
                return folder;
            }
            return std::string();
        }

        std::string ChooseFolder(const std::string& initial) {
            FolderState st{ initial };
            BROWSEINFOA bi{};
            bi.lpfn = BrowseCallbackProc;
            bi.lParam = reinterpret_cast<LPARAM>(&st);
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
            if (!pidl) return std::string();
            char buf[MAX_PATH] = { 0 };
            const bool ok = SHGetPathFromIDListA(pidl, buf);
            CoTaskMemFree(pidl);
            return ok ? std::string(buf) : std::string();
        }

        // ---- tiny widget helpers ---------------------------------------------------

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
            Color bg = pressed ? Color{ 120, 30, 30, 255 } : (hovered ? Color{ 170, 45, 45, 255 } : Color{ 100, 35, 35, 255 });
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
                if (field.cursor > static_cast<int>(field.text.size())) field.cursor = static_cast<int>(field.text.size());
            }

            DrawRectangleRounded(rec, 0.15f, 4, theme::INPUT);
            DrawRectangleLinesEx(rec, field.focused ? 2.0f : 1.0f,
                field.focused ? theme::ACCENT : (hovered ? theme::BORDER_STRONG : theme::BORDER));

            const float maxTextW = rec.width - 12.0f;
            int hidden = 0;
            std::string visiblePrefix = field.text.substr(0, static_cast<size_t>(field.cursor));
            while (!visiblePrefix.empty() && MeasureTextU(visiblePrefix.c_str(), 14.0f) > maxTextW) {
                visiblePrefix.erase(0, 1);
                hidden++;
            }
            const std::string display = field.text.substr(static_cast<size_t>(hidden));
            float textStartX = rec.x + 6.0f;
            if (field.text.empty() && !field.focused) {
                DrawTextU("MyGame", textStartX, rec.y + 3.0f, 14.0f, theme::TEXT_DIM);
            } else {
                DrawTextU(display.c_str(), textStartX, rec.y + 3.0f, 14.0f,
                    field.focused ? theme::TEXT : theme::TEXT_MUTED);
            }
            if (field.focused && (static_cast<int>(GetTime() * 2.0) % 2) == 0) {
                float cursorX = textStartX + MeasureTextU(visiblePrefix.c_str(), 14.0f);
                DrawRectangle(static_cast<int>(cursorX), static_cast<int>(rec.y) + 3, 1, static_cast<int>(rec.height) - 6, theme::ACCENT);
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

    } // namespace

    // ---- public API ------------------------------------------------------------

    void ApplyWindowIcon() {
        constexpr const char* iconPaths[] = { "assets/FlyengineLogo.png", "../assets/FlyengineLogo.png" };
        for (const char* path : iconPaths) {
            if (FileExists(path)) {
                Image icon = LoadImage(path);
                if (icon.data != nullptr) {
                    SetWindowIcon(icon);
                    UnloadImage(icon);
                }
                break;
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
    std::string name = projectFolder.substr(projectFolder.find_last_of("\\/") + 1);
    std::string projectFile = projectFolder + "\\" + name + ".flyproj";
    return LoadSceneFromStream(scene, engine, objects, models, BaseDirOf(projectFile), physicsSim, outTerrain);
}

bool SaveProjectFile(const std::string& projectFolder, const std::vector<ScatteredObject*>& objects,
                     const std::vector<std::unique_ptr<ModelGroup>>& models,
                     terrain::Terrain* terrain) {
        HeaderFields fields;
        std::string name = projectFolder.substr(projectFolder.find_last_of("\\/") + 1);
        std::string projectFile = projectFolder + "\\" + name + ".flyproj";
        {
            std::ifstream scene;
            if (ReadProjectFile(projectFolder, fields, scene)) {
            } else {
                const Info& current = GetCurrentProject();
                fields.name = current.name.empty() ? "Untitled" : current.name;
                fields.templateName = current.templateName.empty() ? "Blank" : current.templateName;
                fields.created = TimeNow();
            }
        }
        fields.modified = TimeNow();

        std::ofstream out(projectFile, std::ios::trunc);
        if (!out.is_open()) return false;
        WriteProjectHeader(out, fields);
        return SaveSceneToStream(out, objects, models, BaseDirOf(projectFile), terrain);
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

        g_font = GetFontDefault();
        if (FileExists("C:/Windows/Fonts/arial.ttf")) {
            g_font = LoadFontEx("C:/Windows/Fonts/arial.ttf", 32, nullptr, 0);
        } else if (FileExists("arial.ttf")) {
            g_font = LoadFontEx("arial.ttf", 32, nullptr, 0);
        }
        SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);

        // =========================================================================
        // 1. LOAD ICON TEXTURE ONCE (Before render loop)
        // =========================================================================
        Texture2D folderTex = { 0 };
        float folderScale = 1.0f;

        Image folderImg = LoadImage("assets/EditorIcons/Folder.png");
        if (folderImg.data == nullptr) folderImg = LoadImage("../assets/EditorIcons/Folder.png");
        if (folderImg.data != nullptr) {
            ImageColorBrightness(&folderImg, 180);
            folderScale = 44.0f / static_cast<float>(folderImg.height);
            folderTex = LoadTextureFromImage(folderImg);
            UnloadImage(folderImg); // Free CPU RAM immediately after GPU upload
        }

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
        std::string chosenTemplate = "Blank";
        std::string baseDir = GetProjectsDirectory();
        CreateDirectoryA(baseDir.c_str(), nullptr);
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

            // ---- recent list interaction ----
            if (!confirmDelete) {
                const Rectangle listRec = { 36.0f, 132.0f, 484.0f, static_cast<float>(visible) * rowH };
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, listRec)) {
                    const int row = static_cast<int>((mouse.y - listRec.y) / rowH) + scroll;
                    if (row >= 0 && row < static_cast<int>(recent.size())) {
                        selected = row;
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
            std::string baseDir = GetProjectsDirectory();
            CreateDirectoryA(baseDir.c_str(), nullptr);
            std::string shownDir = baseDir;
            const float dirMaxW = 276.0f;
            while (!shownDir.empty() && MeasureTextU(shownDir.c_str(), 12) > dirMaxW) {
                shownDir = shownDir.substr(1);
            }
            if (shownDir != baseDir) shownDir = "?" + shownDir;
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
                const std::string path = ChooseProjectOpenPath();
                if (!path.empty()) openProject(path);
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
                        if (CreateProjectFile(name, chosenTemplate)) {
                            std::string projectFolder = GetProjectFolder(name);
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
                    SHFILEOPSTRUCTA fileOp{};
                    fileOp.wFunc = FO_DELETE;
                    std::string from = deletePath + '\0' + '\0';
                    fileOp.pFrom = from.c_str();
                    fileOp.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
                    const int result = SHFileOperationA(&fileOp);
                    if (result == 0) {
                        recentPaths.erase(std::remove(recentPaths.begin(), recentPaths.end(), deletePath), recentPaths.end());
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
}
