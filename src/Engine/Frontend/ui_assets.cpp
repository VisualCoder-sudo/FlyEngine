// ui_assets.cpp: Asset Browser and bottom-dock tabs, split out of ui.cpp.
#include "../../../include/Engine/Frontend/ui.hpp"
#include <map>
#include <set>
#include "../../../include/Engine/ExportManager.hpp"
#include "../../../include/Engine/Backend/VisualScript.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Terrain/Water/WaterBody.hpp"
#include "../../../include/Terrain/BasicTerrain.hpp"
#include "../../../include/Engine/Scripts/CommandConsole.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "../../../include/Engine/Graphics.hpp"
#include "../../../include/Engine/PostFX.hpp"
#include "../../../include/Engine/LoadingScreen.hpp"
#include "../include/Engine/Scripts/ScriptLauncher.hpp"
#include "../../../include/Engine/Scripts/NativeScriptHost.hpp"
#include "../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../../../include/Engine/Backend/TextureManager.hpp"
#include "../include/Engine/Frontend/ProjectManager.hpp"
#include "../../../include/Engine/Backend/ModelImport.hpp"
#include "../include/Terrain/Terrain.hpp"
#include "../../../include/Terrain/TerrainEditor.hpp"
#include "../../../include/Terrain/TerrainRegistry.hpp"
#include "../../../include/CityGen/CityEditor.hpp"
#include "raymath.h"
#include "imgui.h"
#include "rlimgui.h"
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <algorithm>
#include "../../../include/Engine/Backend/AudioAssets.hpp"
#include "ui_internal.hpp"

namespace ui {

// ---------------------------------------------------------------------------

// Asset Browser (bottom dock, middle strip)
//
// A plain, real filesystem view rooted at <project>/assets/. Nothing is
// invented: a brand new project (no assets/ folder yet) shows an empty
// browser. Folders can be created here, files/models can be imported via
// the right-click menu or by dragging them in from Windows Explorer, and
// everything else (navigate, search, rename, delete, reveal in Explorer)
// operates on real files/directories on disk. Search matches recursively
// downward from wherever you currently are, never upward or sideways.
// ---------------------------------------------------------------------------

enum class AssetKind { RealFolder, RealFile, Model };

struct AssetEntry {
    AssetKind kind = AssetKind::RealFile;
    std::string name;
    std::string fsPath; // absolute
};

static bool IsMeshFilePath(const std::string& path) {
    std::string ext;
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* kMeshExts[] = { ".obj", ".gltf", ".glb", ".fbx", ".iqm", ".vox", ".m3d" };
    for (const char* me : kMeshExts) if (ext == me) return true;
    return false;
}

// A folder counts as an imported model (rather than a plain browsable
// folder) when it directly contains a recognized mesh file -- exactly the
// shape ModelImport.cpp produces under assets/3D/<name>/.
static bool DirectoryHasMeshFile(const std::filesystem::path& dir) {
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        std::error_code fec;
        if (entry.is_regular_file(fec) && IsMeshFilePath(entry.path().string())) return true;
    }
    return false;
}

static std::vector<std::string> g_assetPath;      // segments from assets root ("" = root)
static char g_assetPathBuf[256] = "~/";
static char g_assetSearchBuf[128] = "";
static std::vector<AssetEntry> g_assetEntries;
static bool g_assetEntriesDirty = true;
static double g_assetLastRefresh = 0.0;
static int g_assetSelectedIndex = -1;
static int g_assetContextIndex = -1;
static int g_assetRenameIndex = -1;
static char g_assetRenameBuf[128] = "";
static int g_assetPendingDeleteIndex = -1;
static std::string g_assetPendingAutoRenameName; // set right after creating a folder
// OpenPopup()/BeginPopup() hash their ID against whatever window is "current"
// at the call site. The grid lives inside a BeginChild(), so calling
// OpenPopup() there and BeginPopup() later at the top level (from
// DrawImGuiAssetBrowserPopups, outside any child) hash to two different IDs
// and the popup silently never opens. Deferring the actual OpenPopup() call
// to run from that same top-level spot fixes it.
static bool g_assetContextMenuPending = false;
static bool g_assetBgContextMenuPending = false;

static std::string AssetsRootDir() {
    const auto& proj = ::project::GetCurrentProject();
    if (proj.path.empty()) return {};
    return proj.path + "/assets";
}

static std::string JoinAssetPath(const std::vector<std::string>& segs) {
    std::string p;
    for (const auto& s : segs) {
        if (!p.empty()) p += "/";
        p += s;
    }
    return p;
}

static std::string CurrentAssetDir() {
    std::string dir = AssetsRootDir();
    if (dir.empty()) return {};
    if (!g_assetPath.empty()) dir += "/" + JoinAssetPath(g_assetPath);
    return dir;
}

static void SyncAssetPathBuf() {
    std::string disp = "~/";
    disp += JoinAssetPath(g_assetPath);
    snprintf(g_assetPathBuf, sizeof(g_assetPathBuf), "%s", disp.c_str());
    g_assetPathBuf[sizeof(g_assetPathBuf) - 1] = '\0';
}

// Folders other engine systems create on their own (not something the user
// put there) -- filtered out so a fresh project's browser stays genuinely
// empty instead of showing internal housekeeping folders.
static bool IsReservedAssetName(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower == "presettextures";
}

static void RefreshAssetEntries() {
    g_assetEntries.clear();

    std::string search = g_assetSearchBuf;
    std::transform(search.begin(), search.end(), search.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    bool searching = !search.empty();

    auto matches = [&](const std::string& name) {
        if (IsReservedAssetName(name)) return false;
        if (!searching) return true;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find(search) != std::string::npos;
    };

    std::string dir = CurrentAssetDir();
    if (dir.empty()) {
        g_assetEntriesDirty = false;
        g_assetLastRefresh = GetTime();
        return; // no project open -- nothing to show
    }

    std::error_code existsEc;
    if (!std::filesystem::exists(dir, existsEc) || existsEc) {
        // Folder doesn't exist on disk yet (e.g. a brand new project that has
        // never imported/created anything under assets/) -- an empty browser
        // is the correct, honest result here, not an error.
        g_assetEntriesDirty = false;
        g_assetLastRefresh = GetTime();
        return;
    }

    if (searching) {
        std::error_code itEc;
        auto it = std::filesystem::recursive_directory_iterator(
            dir, std::filesystem::directory_options::skip_permission_denied, itEc);
        auto endIt = std::filesystem::recursive_directory_iterator();
        for (; !itEc && it != endIt; it.increment(itEc)) {
            std::string name = it->path().filename().string();
            if (!matches(name)) continue;
            std::error_code isDirEc;
            AssetEntry e;
            if (it->is_directory(isDirEc)) {
                e.kind = DirectoryHasMeshFile(it->path()) ? AssetKind::Model : AssetKind::RealFolder;
            } else {
                e.kind = AssetKind::RealFile;
            }
            e.name = name;
            e.fsPath = it->path().generic_string();
            g_assetEntries.push_back(e);
        }
    } else {
        std::error_code dirEc;
        for (auto& entry : std::filesystem::directory_iterator(dir, dirEc)) {
            std::string name = entry.path().filename().string();
            if (!matches(name)) continue;
            std::error_code isDirEc;
            AssetEntry e;
            if (entry.is_directory(isDirEc)) {
                e.kind = DirectoryHasMeshFile(entry.path()) ? AssetKind::Model : AssetKind::RealFolder;
            } else {
                e.kind = AssetKind::RealFile;
            }
            e.name = name;
            e.fsPath = entry.path().generic_string();
            g_assetEntries.push_back(e);
        }
    }

    std::sort(g_assetEntries.begin(), g_assetEntries.end(), [](const AssetEntry& a, const AssetEntry& b) {
        auto rank = [](AssetKind k) {
            switch (k) {
                case AssetKind::RealFolder: return 0;
                case AssetKind::Model: return 1;
                default: return 2;
            }
        };
        int ra = rank(a.kind), rb = rank(b.kind);
        if (ra != rb) return ra < rb;
        return a.name < b.name;
    });

    g_assetEntriesDirty = false;
    g_assetLastRefresh = GetTime();
}

static Texture2D AssetTextureFor(const AssetEntry& e) {
    if (e.kind == AssetKind::RealFolder) return g_folderIcon;
    if (e.kind == AssetKind::Model) return g_rowIcons[ROWICON_OBJECT];
    std::string ext;
    size_t dot = e.name.find_last_of('.');
    if (dot != std::string::npos) ext = e.name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* kMeshExts[] = { ".obj", ".gltf", ".glb", ".fbx", ".iqm", ".vox", ".m3d" };
    for (const char* me : kMeshExts) {
        if (ext == me) return g_rowIcons[ROWICON_OBJECT];
    }
    return Texture2D{};
}

static void HandleAssetActivate(const AssetEntry& e) {
    if (e.kind == AssetKind::RealFolder) {
        std::error_code ec;
        std::filesystem::path rel = std::filesystem::relative(e.fsPath, AssetsRootDir(), ec);
        g_assetPath.clear();
        if (!ec) {
            for (auto& part : rel) {
                std::string s = part.string();
                if (!s.empty() && s != ".") g_assetPath.push_back(s);
            }
        } else {
            g_assetPath.push_back(e.name);
        }
        g_assetSearchBuf[0] = '\0';
        g_assetEntriesDirty = true;
        g_assetSelectedIndex = -1;
        SyncAssetPathBuf();
    } else {
        // RealFile and Model (an imported model's folder) both act as a
        // single leaf asset: reveal it rather than drilling in.
        platform::RevealInFileManager(e.fsPath);
    }
}

static void PerformAssetRename(int idx) {
    if (idx < 0 || idx >= static_cast<int>(g_assetEntries.size())) return;
    AssetEntry& e = g_assetEntries[idx];
    std::string newName = g_assetRenameBuf;
    if (newName.empty()) return;

    std::filesystem::path oldPath(e.fsPath);
    std::filesystem::path newPath = oldPath.parent_path() / newName;
    std::error_code ec;
    std::filesystem::rename(oldPath, newPath, ec);
    if (ec) LogAlways("[assets] rename failed: %s", ec.message().c_str());
    else LogAlways("[assets] renamed \"%s\" to \"%s\" (note: this does not update any scene references)",
                   e.name.c_str(), newName.c_str());
    g_assetEntriesDirty = true;
}

static void PerformAssetDelete(int idx) {
    if (idx < 0 || idx >= static_cast<int>(g_assetEntries.size())) return;
    AssetEntry& e = g_assetEntries[idx];
    std::error_code ec;
    if (e.kind == AssetKind::RealFile) std::filesystem::remove(e.fsPath, ec);
    else std::filesystem::remove_all(e.fsPath, ec);

    if (ec) LogAlways("[assets] delete failed: %s", ec.message().c_str());
    else LogAlways("[assets] deleted \"%s\" (note: this does not update any scene references)", e.name.c_str());
    g_assetEntriesDirty = true;
    if (g_assetSelectedIndex == idx) g_assetSelectedIndex = -1;
}

static void PerformCreateFolder() {
    std::string dir = CurrentAssetDir();
    if (dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec); // covers the very first folder in a brand new project too

    std::string base = "New Folder";
    std::string candidate = base;
    int n = 1;
    while (std::filesystem::exists(std::filesystem::path(dir) / candidate, ec)) {
        candidate = base + " (" + std::to_string(n++) + ")";
    }
    std::filesystem::create_directory(std::filesystem::path(dir) / candidate, ec);
    if (ec) {
        LogAlways("[assets] create folder failed: %s", ec.message().c_str());
        return;
    }
    LogAlways("[assets] created folder \"%s\"", candidate.c_str());
    g_assetEntriesDirty = true;
    g_assetPendingAutoRenameName = candidate; // pick it up once entries refresh, open rename for it
}

// Moves a real file/folder (or an imported-model folder) into destFolder.
// Used by the browser's own drag-and-drop (dragging a cell onto a folder
// cell, or onto the up/breadcrumb button).
static void MoveAssetIntoFolder(const std::string& srcPath, const std::string& destFolder) {
    if (srcPath.empty() || destFolder.empty()) return;
    std::filesystem::path src(srcPath);
    std::filesystem::path dstDir(destFolder);
    std::error_code ec;

    std::string srcNorm = src.lexically_normal().generic_string();
    std::string dstNorm = dstDir.lexically_normal().generic_string();
    if (dstNorm == srcNorm || dstNorm.rfind(srcNorm + "/", 0) == 0) {
        LogAlways("[assets] can't move \"%s\" into itself", src.filename().string().c_str());
        return;
    }
    if (dstNorm == src.parent_path().lexically_normal().generic_string()) {
        return; // already there -- dropped back into its own folder, no-op
    }

    std::filesystem::path dst = dstDir / src.filename();
    if (std::filesystem::exists(dst, ec)) {
        LogAlways("[assets] a file named \"%s\" already exists there", dst.filename().string().c_str());
        return;
    }
    std::filesystem::create_directories(dstDir, ec);
    std::filesystem::rename(src, dst, ec);
    if (ec) LogAlways("[assets] move failed: %s", ec.message().c_str());
    else LogAlways("[assets] moved \"%s\"", src.filename().string().c_str());
    g_assetEntriesDirty = true;
}

static void ImportModelIntoCurrentFolder(const std::string& sourcePath) {
    const auto& proj = ::project::GetCurrentProject();
    if (proj.path.empty()) return;
    ImportResult res = ImportModel(sourcePath, proj.path, proj.name, CurrentAssetDir());
    if (res.ok) {
        LogAlways("[assets] imported %s", res.storedPath.c_str());
        if (!res.warning.empty()) LogAlways("[assets] %s", res.warning.c_str());
        g_assetEntriesDirty = true;
    } else {
        LogAlways("[assets] import failed: %s", res.error.c_str());
    }
}

// Anything dropped that ImportModel doesn't handle (not a recognized 3D
// format) is just copied straight into the folder currently open.
static void CopyFileIntoCurrentFolder(const std::string& sourcePath) {
    // Sounds go to assets/audio, stored once however often they are added (see AudioAssets.hpp).
    if (audioassets::IsSoundFile(sourcePath)) {
        const audioassets::ImportResult imp = audioassets::Import(::project::GetCurrentProject().path, sourcePath);
        if (imp.ok) LogAlways(imp.reused ? "[audio] using %s (already in the project)" : "[audio] imported %s", imp.rel.c_str());
        else LogAlways("[audio] import failed: %s", imp.error.c_str());
        g_assetEntriesDirty = true;
        return;
    }
    std::string dir = CurrentAssetDir();
    if (dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path dst = std::filesystem::path(dir) / std::filesystem::path(sourcePath).filename();
    std::filesystem::copy_file(sourcePath, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) LogAlways("[assets] copy failed: %s", ec.message().c_str());
    else LogAlways("[assets] added \"%s\"", dst.filename().string().c_str());
    g_assetEntriesDirty = true;
}

static void OpenAssetContextMenu(int idx) {
    g_assetContextIndex = idx;
    g_assetContextMenuPending = true;
}

static void DrawAssetGridItem(int index, const AssetEntry& e, float cellW, float cellH,
                              bool& outActivated, bool& outRightClicked) {
    ImGui::PushID(index);
    // Screen-space (not window-local) so the draw-list calls below don't
    // depend on/disturb the layout cursor.
    ImVec2 screenPos = ImGui::GetCursorScreenPos();
    bool selected = (g_assetSelectedIndex == index);

    // Selectable is the ONLY layout-affecting call for this cell. Everything
    // visual is drawn on top via the draw list at fixed screen coordinates --
    // using SetCursorPos/Image/TextWrapped here instead would itself advance
    // the layout cursor and corrupt where the *next* cell in the row lands.
    bool clicked = ImGui::Selectable("##cell", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(cellW, cellH));
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) outActivated = true;
    if (clicked) g_assetSelectedIndex = index;
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        g_assetSelectedIndex = index;
        outRightClicked = true;
    }

    // Drag source: any entry can be picked up (files, folders, imported
    // models). Payload carries the source path so it stays valid even if the
    // entry list gets rebuilt mid-drag (e.g. the periodic refresh).
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        ImGui::SetDragDropPayload("ASSET_FSPATH", e.fsPath.c_str(), e.fsPath.size() + 1);
        Texture2D dragTex = AssetTextureFor(e);
        if (dragTex.id != 0) ImGui::Image(static_cast<ImTextureID>(GetTextureImGuiId(dragTex)), ImVec2(20.0f, 20.0f));
        ImGui::SameLine();
        ImGui::TextUnformatted(e.name.c_str());
        ImGui::EndDragDropSource();
    }
    // Drag target: only plain real folders accept drops (dropping onto a
    // model or a file doesn't make sense).
    if (e.kind == AssetKind::RealFolder && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FSPATH")) {
            std::string srcPath(static_cast<const char*>(payload->Data));
            MoveAssetIntoFolder(srcPath, e.fsPath);
        }
        ImGui::EndDragDropTarget();
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr float iconSize = 40.0f;
    Texture2D iconTex = AssetTextureFor(e);
    if (iconTex.id != 0) {
        float aw = static_cast<float>(iconTex.width);
        float ah = static_cast<float>(iconTex.height);
        float scale = (aw > 0.0f && ah > 0.0f) ? std::min(iconSize / aw, iconSize / ah) : 1.0f;
        float dw = aw * scale, dh = ah * scale;
        float iconX = screenPos.x + (cellW - dw) * 0.5f;
        float iconY = screenPos.y + 6.0f + (iconSize - dh) * 0.5f;
        dl->AddImage(static_cast<ImTextureID>(GetTextureImGuiId(iconTex)), ImVec2(iconX, iconY), ImVec2(iconX + dw, iconY + dh));
    }

    float wrapW = cellW - 4.0f;
    ImVec2 textSize = ImGui::CalcTextSize(e.name.c_str(), nullptr, false, wrapW);
    float textX = screenPos.x + std::max(2.0f, (cellW - textSize.x) * 0.5f);
    float textY = screenPos.y + iconSize + 10.0f;
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(textX, textY),
               ImGui::GetColorU32(ImGuiCol_Text), e.name.c_str(), nullptr, wrapW);

    ImGui::PopID();
}

static void DrawAssetBackgroundContextMenu() {
    if (!ImGui::BeginPopup("##AssetBgContextMenu")) return;
    if (ImGui::MenuItem("New Folder")) {
        PerformCreateFolder();
    }
    if (ImGui::MenuItem("Import 3D Model...")) {
        // Non-blocking: the chosen path arrives via PollDialogResult below.
        // Start in the project's assets folder so the picker opens somewhere
        // sensible; this popup has no local `currentProject` of its own.
        BeginChooseModelOpenPath(::project::GetCurrentProject().path);
    }
    ImGui::EndPopup();
}

// Fallback file picker, shown only when neither zenity nor kdialog is
// installed. Platform owns the state (see platform::GetPathPrompt) so it stays
// free of ImGui; this just draws it and records the answer.
void DrawPathPromptModal() {
    platform::PathPrompt& p = platform::GetPathPrompt();
    if (!p.active) return;

    ImGui::OpenPopup("##PathPrompt");
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
    // No ImGuiWindowFlags_AlwaysAutoResize here, and that is deliberate.
    //
    // AlwaysAutoResize forces AutoFitFramesX/Y = 2 and AutoFitOnlyGrows=false
    // on every Begin (imgui.cpp:6746), overriding the width this call just
    // asked for, so the window resizes to its content each frame. Combined with
    // SetNextItemWidth(-1.0f) below -- which makes the InputText exactly as wide
    // as the window -- that is a feedback loop: the window shrinks to fit the
    // input, and the input is sized from the window. It converged on the width
    // of the longest text, one frame at a time, which is the "frame slowly
    // reduces its width until it reaches the text" symptom.
    //
    // Without the flag, x stays at the requested 560 and only y auto-fits
    // (size.y <= 0 -> AutoFitFramesY = 2), which is what was wanted.
    if (ImGui::BeginPopupModal("##PathPrompt", nullptr)) {
        ImGui::TextUnformatted(p.title.c_str());
        ImGui::TextDisabled("No zenity/kdialog found -- type or paste the full path.");
        ImGui::Separator();

        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##path", p.buffer, sizeof(p.buffer))) {
            p.accept = false;
            p.cancel = false;
        }

        ImGui::Separator();
        bool ok = ImGui::Button("OK", ImVec2(120.0f, 0.0f));
        ImGui::SameLine();
        bool cancel = ImGui::Button("Cancel", ImVec2(120.0f, 0.0f));

        if (ok) {
            // Reject an empty or non-existent path so a mistyped entry does not
            // silently become the selected file.
            const bool valid = p.buffer[0] != '\0' && FileExists(p.buffer);
            if (!valid) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Enter an existing path.");
            else p.accept = true;
        }
        if (cancel) p.cancel = true;

        // Enter submits, Escape dismisses.
        if (ImGui::IsItemHovered() && ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            if (p.buffer[0] != '\0' && FileExists(p.buffer)) p.accept = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) p.cancel = true;

        ImGui::EndPopup();
    }
}

// Delivers a completed "Import 3D Model" dialog to the asset browser. Called
// from the ImGui frame so the browser is fully built by the time we act on it.
void PumpAssetBrowserDialogs() {
    std::string picked;
    if (platform::PollDialogResult({platform::DialogPurpose::ImportModel}, picked)
        && !picked.empty()) {
        ImportModelIntoCurrentFolder(picked);
    }

    // Texture picked from the inspector's [F] button -> apply to selected meshes.
    if (platform::PollDialogResult({platform::DialogPurpose::ImportTexture}, picked)
        && !picked.empty()) {
        const std::string& projectDir = ::project::GetCurrentProject().path;
        if (!projectDir.empty()) {
            for (auto* obj : g_selection) {
                if (obj && obj->HasModel()) {
                    obj->SetTexturePath(
                        textureManager::RegisterTexture(picked, obj->GetModelPath()),
                        projectDir);
                }
            }
        }
    }
}

static void DrawAssetBrowserGrid() {
    if (g_assetEntries.empty()) {
        ImGui::TextDisabled("(empty -- right-click to import or create a folder)");
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            g_assetContextIndex = -1;
            g_assetBgContextMenuPending = true;
        }
        return;
    }

    float availW = ImGui::GetContentRegionAvail().x;
    constexpr float cellW = 72.0f;
    constexpr float cellH = 86.0f;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    // Column pitch is the cell plus the spacing ImGui inserts automatically
    // between SameLine() items -- using just cellW here (as before) undercounts
    // the true width per column, so columns drift out of alignment row to row.
    float pitch = cellW + spacing;
    int perRow = std::max(1, static_cast<int>((availW + spacing) / pitch));

    int activatedIndex = -1;
    int rightClickedIndex = -1;
    for (int i = 0; i < static_cast<int>(g_assetEntries.size()); ++i) {
        bool activated = false, rightClicked = false;
        DrawAssetGridItem(i, g_assetEntries[i], cellW, cellH, activated, rightClicked);
        if (activated) activatedIndex = i;
        if (rightClicked) rightClickedIndex = i;
        if ((i + 1) % perRow != 0 && i + 1 < static_cast<int>(g_assetEntries.size())) ImGui::SameLine();
    }

    if (rightClickedIndex < 0 && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        g_assetContextIndex = -1;
        g_assetBgContextMenuPending = true;
    }

    if (activatedIndex >= 0) HandleAssetActivate(g_assetEntries[activatedIndex]);
    if (rightClickedIndex >= 0) OpenAssetContextMenu(rightClickedIndex);
}

static void DrawAssetBrowserToolbar() {
    ImTextureID upTex = g_terrainToolIcons[TTOOLICON_LOWER].id
        ? static_cast<ImTextureID>(GetTextureImGuiId(g_terrainToolIcons[TTOOLICON_LOWER])) : static_cast<ImTextureID>(0);
    const float frameH = ImGui::GetFrameHeight();
    if (ImGui::Button("##assetBreadcrumbBtn", ImVec2(frameH + 4.0f, frameH))) {
        ImGui::OpenPopup("##AssetBreadcrumb");
    }
    if (upTex) {
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
        ImGui::GetWindowDrawList()->AddImage(upTex, ImVec2(c.x - 6.0f, c.y - 6.0f), ImVec2(c.x + 6.0f, c.y + 6.0f));
    }
    if (!g_assetPath.empty() && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FSPATH")) {
            std::string srcPath(static_cast<const char*>(payload->Data));
            std::vector<std::string> parentSegs(g_assetPath.begin(), g_assetPath.end() - 1);
            std::string parentDir = AssetsRootDir();
            if (!parentSegs.empty()) parentDir += "/" + JoinAssetPath(parentSegs);
            MoveAssetIntoFolder(srcPath, parentDir);
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopup("##AssetBreadcrumb")) {
        if (ImGui::MenuItem("~ (assets root)")) {
            g_assetPath.clear();
            g_assetSearchBuf[0] = '\0';
            g_assetEntriesDirty = true;
            g_assetSelectedIndex = -1;
            SyncAssetPathBuf();
        }
        std::string running;
        for (size_t i = 0; i < g_assetPath.size(); ++i) {
            running += (i == 0 ? "" : "/") + g_assetPath[i];
            if (ImGui::MenuItem(running.c_str())) {
                g_assetPath.assign(g_assetPath.begin(), g_assetPath.begin() + static_cast<long>(i) + 1);
                g_assetSearchBuf[0] = '\0';
                g_assetEntriesDirty = true;
                g_assetSelectedIndex = -1;
                SyncAssetPathBuf();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("New Folder")) {
        PerformCreateFolder();
    }

    ImGui::SameLine();
    if (ImGui::Button("Import")) {
        BeginChooseModelOpenPath(::project::GetCurrentProject().path);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Import a 3D model (or drop files onto the browser)");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 190.0f);
    bool submitted = ImGui::InputText("##assetPathBar", g_assetPathBuf, sizeof(g_assetPathBuf),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
    if (submitted) {
        std::string typed = g_assetPathBuf;
        size_t start = 0;
        if (!typed.empty() && typed[0] == '~') start = 1;
        while (start < typed.size() && (typed[start] == '/' || typed[start] == '\\')) start++;
        std::string rest = typed.substr(start);

        std::vector<std::string> segs;
        std::stringstream ss(rest);
        std::string tok;
        while (std::getline(ss, tok, '/')) if (!tok.empty()) segs.push_back(tok);

        if (segs.empty()) {
            g_assetPath.clear();
        } else {
            std::string dir = AssetsRootDir();
            for (auto& s : segs) dir += "/" + s;
            std::error_code ec;
            if (!dir.empty() && std::filesystem::is_directory(dir, ec)) {
                g_assetPath = segs;
            } else {
                LogAlways("[assets] path not found: %s", rest.c_str());
            }
        }
        g_assetSearchBuf[0] = '\0';
        g_assetEntriesDirty = true;
        g_assetSelectedIndex = -1;
        SyncAssetPathBuf();
    }

    ImGui::SameLine();
    ImTextureID searchTex = g_assetSearchIcon.id ? static_cast<ImTextureID>(GetTextureImGuiId(g_assetSearchIcon)) : static_cast<ImTextureID>(0);
    if (searchTex) {
        ImGui::Dummy(ImVec2(14.0f, frameH));
        const ImVec2 mn = ImGui::GetItemRectMin();
        const float y = mn.y + (frameH - 14.0f) * 0.5f;
        ImGui::GetWindowDrawList()->AddImage(searchTex, ImVec2(mn.x, y), ImVec2(mn.x + 14.0f, y + 14.0f));
        ImGui::SameLine(0.0f, 4.0f);
    }
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::InputTextWithHint("##assetSearch", "Search", g_assetSearchBuf, sizeof(g_assetSearchBuf))) {
        g_assetEntriesDirty = true;
        g_assetSelectedIndex = -1;
    }
}

static void DrawAssetContextMenu() {
    if (!ImGui::BeginPopup("##AssetContextMenu")) return;
    if (g_assetContextIndex < 0 || g_assetContextIndex >= static_cast<int>(g_assetEntries.size())) {
        ImGui::EndPopup();
        return;
    }
    const AssetEntry& e = g_assetEntries[g_assetContextIndex];

    if (ImGui::MenuItem("Select")) {
        g_assetSelectedIndex = g_assetContextIndex;
    }
    if (ImGui::MenuItem("Reveal in Explorer")) {
        platform::RevealInFileManager(e.fsPath);
    }
    if (ImGui::MenuItem("Rename")) {
        g_assetRenameIndex = g_assetContextIndex;
        snprintf(g_assetRenameBuf, sizeof(g_assetRenameBuf), "%s", e.name.c_str());
        g_assetRenameBuf[sizeof(g_assetRenameBuf) - 1] = '\0';
        ImGui::OpenPopup("##AssetRenamePopup");
    }
    if (ImGui::MenuItem("Delete")) {
        g_assetPendingDeleteIndex = g_assetContextIndex;
        ImGui::OpenPopup("##AssetDeleteConfirm");
    }
    ImGui::EndPopup();
}

static void DrawAssetRenamePopup() {
    if (!ImGui::BeginPopup("##AssetRenamePopup")) return;
    ImGui::SetNextItemWidth(220.0f);
    bool submit = ImGui::InputText("##assetRenameField", g_assetRenameBuf, sizeof(g_assetRenameBuf),
                                   ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    bool okClicked = ImGui::Button("Rename");
    ImGui::SameLine();
    bool cancelClicked = ImGui::Button("Cancel");
    if (submit || okClicked) {
        PerformAssetRename(g_assetRenameIndex);
        g_assetRenameIndex = -1;
        ImGui::CloseCurrentPopup();
    } else if (cancelClicked) {
        g_assetRenameIndex = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static void DrawAssetDeleteConfirm() {
    ImGui::SetNextWindowSize(ImVec2(300.0f, 0.0f));
    if (!ImGui::BeginPopupModal("##AssetDeleteConfirm", nullptr,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar)) return;
    std::string label = "Delete this asset? This cannot be undone.";
    if (g_assetPendingDeleteIndex >= 0 && g_assetPendingDeleteIndex < static_cast<int>(g_assetEntries.size())) {
        label = "Delete \"" + g_assetEntries[g_assetPendingDeleteIndex].name + "\"? This cannot be undone.";
    }
    ImGui::TextWrapped("%s", label.c_str());
    ImGui::Spacing();
    if (ImGui::Button("Delete", ImVec2(130.0f, 0.0f))) {
        PerformAssetDelete(g_assetPendingDeleteIndex);
        g_assetPendingDeleteIndex = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(130.0f, 0.0f))) {
        g_assetPendingDeleteIndex = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Lets you drag files straight from Windows Explorer onto the panel to bring
// them into the project: recognized 3D formats go through ImportModel (same
// path as the right-click action, deps and all); anything else is just
// copied into whatever folder is currently open.
static void HandleAssetBrowserFileDrop(Rectangle panelRec) {
    if (!IsFileDropped()) return;
    Vector2 mouse = GetMousePosition();
    if (!CheckCollisionPointRec(mouse, panelRec)) return;

    const auto& proj = ::project::GetCurrentProject();
    if (proj.path.empty()) return;

    FilePathList dropped = LoadDroppedFiles();
    for (unsigned int i = 0; i < dropped.count; ++i) {
        std::string path = dropped.paths[i];
        if (IsMeshFilePath(path)) ImportModelIntoCurrentFolder(path);
        else CopyFileIntoCurrentFolder(path);
    }
    UnloadDroppedFiles(dropped);
}

// Where a viewport drop should land: intersects the mouse ray with the
// y = 0 ground plane. Good enough for a flat scene; if there's a proper
// ground/terrain height query available it'd be worth swapping in instead.
static bool RaycastGroundPlane(Vector2 mousePos, const Camera3D& camera, Vector3& outPos) {
    Ray ray = GetMouseRay(mousePos, camera);
    if (fabsf(ray.direction.y) < 0.0001f) return false; // parallel to the ground
    float t = -ray.position.y / ray.direction.y;
    if (t < 0.0f) return false; // ground is behind the camera
    outPos = Vector3{ ray.position.x + ray.direction.x * t, 0.0f, ray.position.z + ray.direction.z * t };
    return true;
}

static bool IsPrefabFilePath(const std::string& path) {
    return std::filesystem::path(path).extension() == ".flyprefab";
}

static void SpawnImportedModelAt(const std::string& modelPath, Vector3 worldPos) {
    if (!IsMeshFilePath(modelPath) && !IsPrefabFilePath(modelPath)) return;   // a prefab rides the same request
    g_spawnPath = modelPath;
    g_spawnPos = worldPos;
    g_spawnRequested = true;
}

// Fires when a browser drag (see BeginDragDropSource in DrawAssetGridItem) is
// released over the game viewport rather than over an in-browser drop target.
static void HandleAssetBrowserViewportDrop(const Camera3D& camera) {
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    if (!payload || !payload->IsDataType("ASSET_FSPATH")) return;
    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;
    // Any in-browser drop target (a folder cell, the up button) will have
    // already consumed the payload via AcceptDragDropPayload this same
    // frame; WantCaptureMouse being false here means we're over the bare
    // viewport, not any docked panel.
    if (ImGui::GetIO().WantCaptureMouse) return;

    std::string srcPath(static_cast<const char*>(payload->Data));
    Vector3 dropPos;
    if (RaycastGroundPlane(GetMousePosition(), camera, dropPos)) {
        SpawnImportedModelAt(srcPath, dropPos);
    }
}

// The bottom panel's tabs: 0 = Assets, 1 = Output, 2 = Visual Script. g_bottomTabRequest switches tab for one frame (-1 = none).
int g_bottomTabActive = 0;
int g_bottomTabRequest = -1;

// The Output tab: the log, oldest line first, in the line's severity colour. It follows the newest line while
// the view is at the bottom; scroll up to read back. "Clear" empties it.
static void DrawOutputTab() {
    if (ImGui::SmallButton("Clear")) ClearLog();
    ImGui::SameLine();
    ImGui::TextDisabled("%d line%s", g_logCount, g_logCount == 1 ? "" : "s");
    ImGui::Separator();
    ImGui::BeginChild("##OutputLines", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NoSavedSettings);
    for (int i = 0; i < g_logCount; ++i) {
        const int idx = (g_logWrite - g_logCount + i + kLogCapacity) % kLogCapacity;
        Color c = theme::TEXT;
        if (g_log[idx].severity == LogSeverity::Error) c = theme::DANGER;
        else if (g_log[idx].severity == LogSeverity::Success) c = theme::SUCCESS;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f));
        ImGui::TextWrapped("%s", g_log[idx].text);
        ImGui::PopStyleColor();
    }
    static unsigned long long followedTotal = 0;
    if (g_logTotal != followedTotal) {          // a new line since last frame: stay at the bottom if we were there
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f) ImGui::SetScrollHereY(1.0f);
        followedTotal = g_logTotal;
    }
    ImGui::EndChild();
}

static void DrawVisualScriptTab() {
    vscript::SceneListing scene;
    scene.projectDir = ::project::GetCurrentProject().path;
    scene.deleteScript = [](const std::string& key) { DeleteScriptEverywhere(key); };
    // Objects can share a name, so a repeat gets "#2", "#3"... in scene order. The script runner counts the same way.
    std::map<const ScatteredObject*, std::string> labels;
    if (g_sceneObjects) {
        std::map<std::string, int> seen;
        for (auto* o : *g_sceneObjects) {
            if (!o) continue;
            const int n = ++seen[o->GetName()];
            labels[o] = n > 1 ? o->GetName() + "#" + std::to_string(n) : o->GetName();
        }
    }
    if (g_sceneModels)
        for (const auto& m : *g_sceneModels) {
            if (!m) continue;
            vscript::SceneListing::Group grp;
            grp.name = m->name;
            for (auto* o : m->members) if (o) grp.members.push_back(labels.count(o) ? labels[o] : o->GetName());
            scene.groups.push_back(std::move(grp));
        }
    if (g_sceneObjects)
        for (auto* o : *g_sceneObjects) if (o && !o->parentModel) scene.loose.push_back(labels[o]);
    if (ScriptRuntime* rt = GetActiveRuntime()) {
        for (const auto& sc : rt->StandaloneScripts()) scene.existingScripts.push_back(ScriptKey(sc));
    }
    if (g_sceneObjects)
        for (auto* o : *g_sceneObjects)
            if (o) for (const auto& sc : o->AllScripts()) scene.existingScripts.push_back(scriptLauncher::MakeClassName(sc));
    vscript::DrawEditor(scene);
}

void DrawImGuiAssetBrowser(const Camera3D& camera) {
    Rectangle panelRec = GetAssetBrowserPanelBounds();
    HandleAssetBrowserFileDrop(panelRec);
    HandleAssetBrowserViewportDrop(camera);

    if (g_assetEntriesDirty || (GetTime() - g_assetLastRefresh > 1.5)) {
        RefreshAssetEntries();
        if (!g_assetPendingAutoRenameName.empty()) {
            for (int i = 0; i < static_cast<int>(g_assetEntries.size()); ++i) {
                if (g_assetEntries[i].kind == AssetKind::RealFolder &&
                    g_assetEntries[i].name == g_assetPendingAutoRenameName) {
                    g_assetRenameIndex = i;
                    snprintf(g_assetRenameBuf, sizeof(g_assetRenameBuf), "%s", g_assetEntries[i].name.c_str());
                    g_assetRenameBuf[sizeof(g_assetRenameBuf) - 1] = '\0';
                    ImGui::OpenPopup("##AssetRenamePopup");
                    break;
                }
            }
            g_assetPendingAutoRenameName.clear();
        }
    }

    ImGui::SetNextWindowPos(ImVec2(panelRec.x, panelRec.y));
    ImGui::SetNextWindowSize(ImVec2(panelRec.width, panelRec.height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings;
    // Flush with the console bar below it: no padding or border that would leave a strip of viewport showing.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##AssetBrowser", nullptr, flags);
    ImGui::PopStyleVar();

    // Two tabs of one panel: the asset browser, and the output (log). The Output tab carries a small red dot
    // while lines have been written that it has not shown yet.
    if (ImGui::BeginTabBar("##BottomTabs", ImGuiTabBarFlags_NoCloseWithMiddleMouseButton)) {
        ImGuiTabItemFlags assetsFlags = ImGuiTabItemFlags_None, outputFlags = ImGuiTabItemFlags_None;
        if (g_bottomTabRequest == 0) assetsFlags |= ImGuiTabItemFlags_SetSelected;
        if (g_bottomTabRequest == 1) outputFlags |= ImGuiTabItemFlags_SetSelected;
        ImGuiTabItemFlags scriptFlags = g_bottomTabRequest == 2 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        g_bottomTabRequest = -1;

        if (ImGui::BeginTabItem("Assets", nullptr, assetsFlags)) {
            g_bottomTabActive = 0;
            const auto& proj = ::project::GetCurrentProject();
            if (proj.path.empty()) {
                ImGui::TextDisabled("No project open");
            } else {
                DrawAssetBrowserToolbar();
                ImGui::Separator();
                ImGui::BeginChild("##AssetBrowserGrid", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NoSavedSettings);
                DrawAssetBrowserGrid();
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }

        const bool unread = g_logTotal != g_logSeenTotal;
        const bool outputOpen = ImGui::BeginTabItem("Output", nullptr, outputFlags);
        const ImVec2 outMin = ImGui::GetItemRectMin(), outMax = ImGui::GetItemRectMax();
        if (outputOpen) {
            g_bottomTabActive = 1;
            g_logSeenTotal = g_logTotal;
            DrawOutputTab();
            ImGui::EndTabItem();
        } else if (unread) {
            // The dot sits at the top-right of the tab's label.
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(outMax.x - 6.0f, outMin.y + 6.0f), 4.0f, IM_COL32(235, 64, 52, 255), 12);
        }
        if (ImGui::BeginTabItem("Visual Script", nullptr, scriptFlags)) {
            g_bottomTabActive = 2;
            DrawVisualScriptTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

// Popups are drawn as their own top-level calls (mirrors DrawImGuiExplorer /
// DrawImGuiExplorerMenu). The actual OpenPopup() calls for the two context
// menus are deferred to happen right here too -- see the comment by
// g_assetContextMenuPending for why (the grid lives inside a BeginChild(),
// and OpenPopup()/BeginPopup() must share the same "current window" context
// or their computed popup IDs won't match and the popup never appears).
void DrawImGuiAssetBrowserPopups() {
    if (g_assetContextMenuPending) {
        g_assetContextMenuPending = false;
        ImGui::OpenPopup("##AssetContextMenu");
    }
    if (g_assetBgContextMenuPending) {
        g_assetBgContextMenuPending = false;
        ImGui::OpenPopup("##AssetBgContextMenu");
    }
    DrawAssetContextMenu();
    DrawAssetBackgroundContextMenu();
    DrawAssetRenamePopup();
    DrawAssetDeleteConfirm();
}


} // namespace ui
