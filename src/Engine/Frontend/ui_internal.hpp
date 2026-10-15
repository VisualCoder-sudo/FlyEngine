// Private to src/Engine/Frontend: state and helpers shared between the ui_*.cpp files.
#pragma once
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "raylib.h"
#include "imgui.h"
#include <memory>
#include <string>
#include <vector>

class ModelGroup;

namespace ui {

// ---------------------------------------------------------------------------
// Dark theme palette
// ---------------------------------------------------------------------------
namespace theme {

constexpr Color BG_TOP_BAR       = Color{ 25, 27, 32, 255 };
constexpr Color BG_PANEL         = Color{ 30, 32, 38, 255 };
constexpr Color BG_TITLE         = Color{ 38, 41, 48, 255 };
constexpr Color BG_WIDGET        = Color{ 48, 51, 60, 255 };
constexpr Color BG_WIDGET_HOVER  = Color{ 63, 67, 78, 255 };
constexpr Color BG_WIDGET_PRESSED= Color{ 40, 43, 51, 255 };
constexpr Color BG_INPUT         = Color{ 21, 23, 28, 255 };
constexpr Color BG_INPUT_HOVER   = Color{ 27, 29, 36, 255 };
constexpr Color BG_MENU          = Color{ 35, 37, 43, 250 };
constexpr Color BG_ROW_HOVER     = Color{ 44, 47, 56, 255 };
constexpr Color BORDER           = Color{ 72, 77, 88, 255 };
constexpr Color BORDER_STRONG    = Color{ 104, 110, 122, 255 };
constexpr Color DIVIDER          = Color{ 56, 60, 70, 255 };
// The accent family is mutable so a plugin can re-skin the editor; see
// ui::SetAccentColor. Everything else here is fixed.
inline Color ACCENT              = Color{ 0, 190, 200, 255 };
inline Color ACCENT_HOVER        = Color{ 40, 210, 220, 255 };
inline Color ACCENT_PRESSED      = Color{ 0, 148, 158, 255 };
inline Color ACCENT_SOFT         = Color{ 0, 190, 200, 42 };
constexpr Color TEXT             = Color{ 232, 232, 238, 255 };
constexpr Color TEXT_MUTED       = Color{ 156, 161, 172, 255 };
constexpr Color TEXT_DIM         = Color{ 122, 128, 140, 255 };
constexpr Color DANGER           = Color{ 216, 84, 84, 255 };
constexpr Color DANGER_HOVER     = Color{ 236, 112, 112, 255 };
constexpr Color SUCCESS          = Color{ 74, 176, 104, 255 };
constexpr Color SHADOW           = Color{ 0, 0, 0, 92 };

// The accent as ImGui types, with an optional alpha (0..1 / 0..255).
inline ImVec4 AccentVec(float alpha = 1.0f) {
    return ImVec4(ACCENT.r / 255.0f, ACCENT.g / 255.0f, ACCENT.b / 255.0f, alpha);
}
inline ImU32 AccentU32(int alpha = 255) {
    return IM_COL32(ACCENT.r, ACCENT.g, ACCENT.b, alpha);
}

} // namespace theme

// Explorer row-type icons: object/model, water, terrain, script.
enum RowIconType { ROWICON_OBJECT = 0, ROWICON_WATER, ROWICON_TERRAIN, ROWICON_SCRIPT, ROWICON_COUNT };
// Terrain sculpt tool icons (Raise/Lower/Smooth/Flatten/Paint/None), matching
// BasicTerrain::Tool's ordering.
enum TerrainToolIconType { TTOOLICON_RAISE = 0, TTOOLICON_LOWER, TTOOLICON_SMOOTH, TTOOLICON_FLATTEN, TTOOLICON_PAINT, TTOOLICON_ERODE, TTOOLICON_NOISE, TTOOLICON_RAMP, TTOOLICON_NONE, TTOOLICON_COUNT };

// Output log (ui.cpp owns the ring buffer; the Output tab reads it).
constexpr int kLogCapacity = 64;
constexpr int kLogLineLen = 200;

struct LogEntry {
    char text[kLogLineLen];
    LogSeverity severity = LogSeverity::Neutral;
    double time;
};
extern LogEntry g_log[kLogCapacity];
extern int g_logWrite;
extern int g_logCount;
extern unsigned long long g_logTotal;
extern unsigned long long g_logSeenTotal;

// Textures loaded in ui.cpp's Init().
extern Texture2D g_rowIcons[ROWICON_COUNT];
extern Texture2D g_terrainToolIcons[TTOOLICON_COUNT];
extern Texture2D g_folderIcon;
extern Texture2D g_assetSearchIcon;

// Scene and selection state.
extern const std::vector<ScatteredObject*>* g_sceneObjects;
extern std::vector<std::unique_ptr<ModelGroup>>* g_sceneModels;
extern std::vector<ScatteredObject*> g_selection;

// Pending model-spawn request (Asset Browser viewport drop), consumed by ConsumeModelSpawnRequest().
extern bool g_spawnRequested;
extern std::string g_spawnPath;
extern Vector3 g_spawnPos;

std::string ScriptKey(const ScriptRuntime::StandaloneScript& s);
void DeleteScriptEverywhere(const std::string& key, int index = -1);
Rectangle GetAssetBrowserPanelBounds();

// ui_assets.cpp: the bottom dock (Assets, Output and Script tabs).
extern int g_bottomTabActive;   // the tab showing: 0 Assets, 1 Output, 2 Script
extern int g_bottomTabRequest;  // tab to switch to next frame, or -1
void DrawImGuiAssetBrowser(const Camera3D& camera);
void DrawImGuiAssetBrowserPopups();
void PumpAssetBrowserDialogs();
void DrawPathPromptModal();

} // namespace ui
