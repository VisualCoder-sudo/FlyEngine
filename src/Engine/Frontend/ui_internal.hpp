// Private to src/Engine/Frontend: state and helpers shared between the ui_*.cpp files.
#pragma once
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "raylib.h"
#include "imgui.h"
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

class ModelGroup;

namespace ui {

// C++-friendly constexpr color constants
constexpr Color COL_WHITE     = Color{ 255, 255, 255, 255 };
constexpr Color COL_RED       = Color{ 255, 0,   0,   255 };
constexpr Color COL_ORANGE    = Color{ 255, 165, 0,   255 };
constexpr Color COL_GOLD      = Color{ 255, 215, 0,   255 };
constexpr Color COL_LIME      = Color{ 0,   255, 0,   255 };
constexpr Color COL_GREEN     = Color{ 0,   128, 0,   255 };
constexpr Color COL_BLUE      = Color{ 0,   0,   255, 255 };
constexpr Color COL_PURPLE    = Color{ 128, 0,   128, 255 };
constexpr Color COL_MAGENTA   = Color{ 255, 0,   255, 255 };

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

// A palette colour as an ImGui colour; `alpha` (0..255) overrides the colour's own when given.
inline ImVec4 Vec(Color c, int alpha = -1) {
    return ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (alpha < 0 ? c.a : alpha) / 255.0f);
}

// The accent as ImGui types, with an optional alpha (0..1 / 0..255).
inline ImVec4 AccentVec(float alpha = 1.0f) {
    return ImVec4(ACCENT.r / 255.0f, ACCENT.g / 255.0f, ACCENT.b / 255.0f, alpha);
}
inline ImU32 AccentU32(int alpha = 255) {
    return IM_COL32(ACCENT.r, ACCENT.g, ACCENT.b, alpha);
}

} // namespace theme

// Explorer row-type icons: object, water, terrain, script, group.
enum RowIconType { ROWICON_OBJECT = 0, ROWICON_WATER, ROWICON_TERRAIN, ROWICON_SCRIPT, ROWICON_GROUP, ROWICON_LIGHT, ROWICON_DECAL, ROWICON_TEXTURE, ROWICON_COUNT };
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

// ui_layout.cpp: where the panels sit (docking side, size, visibility), editable by dragging or in Preferences > Layout.
enum class PanelId : int { Explorer = 0, Properties, Terrain, Bottom };
constexpr int kPanelCount = 4;
enum class DockSide : int { Left = 0, Right = 1 };
struct PanelState {
    bool visible = true;
    DockSide side = DockSide::Left;   // Explorer / Properties / Terrain; the bottom panel always spans the middle
    int order = 0;                    // position in its column, top to bottom
    float weight = 1.0f;              // share of the column's height
};
struct LayoutState {
    PanelState panel[kPanelCount];
    float leftW = 220.0f, rightW = 240.0f, bottomH = 230.0f;
    bool locked = false;              // no dragging or resizing
};
LayoutState& Layout();
const char* PanelName(PanelId id);
Rectangle GetPanelBounds(PanelId id);   // zero-size while the panel is hidden
bool IsPanelVisible(PanelId id);
void SetPanelVisible(PanelId id, bool visible);
DockSide GetPanelSide(PanelId id);
void SetPanelSide(PanelId id, DockSide side);
void GetColumnWidths(float& left, float& right);   // 0 for a side with no visible panel
void ResetLayout();
void SaveLayout();
void UpdatePanelLayout();         // once per frame before the panels draw: edge resizing and title dragging
void DrawPanelLayoutOverlay();    // once per frame inside the ImGui pass: drop preview and the title context menu

// ui_assets.cpp: the bottom dock (Assets, Output and Script tabs).
extern int g_bottomTabActive;   // the tab showing: 0 Assets, 1 Output, 2 Script
extern int g_bottomTabRequest;  // tab to switch to next frame, or -1
void DrawImGuiAssetBrowser(const Camera3D& camera);
void DrawImGuiAssetBrowserPopups();
void PumpAssetBrowserDialogs();
void DrawPathPromptModal();

// ui_preferences.cpp: the Preferences window. The top bar's Edit menu opens it.
extern bool g_showPreferences;
extern int g_prefsCategory;
void DrawImGuiPreferencesWindow();

// ui_topbar.cpp: the top bar. Its state lives in ui.cpp, next to the accessors that read it.
extern Texture2D g_toolIcons[5];
extern TransformTool g_activeTool;
extern SnapSettings g_snap;
void LoadSnapSettings();
void SaveSnapSettingsIfChanged(); // writes snap steps to the user config dir when they changed
extern MenuAction g_pendingFileAction;  // set by the File menu, consumed by ProcessContextMenu()
extern bool g_playTogglePending;
extern bool g_importRequested;
Rectangle GetTopBarBounds();
void DrawImGuiTopBar();

// ui_lighting.cpp: settings of the Lighting item selected in the explorer.
void DrawImGuiLightingPanel();

// ui_nodes.cpp: settings of the object child node selected in the explorer.
void DrawImGuiNodePanel();

// ui_theme.cpp: Dear ImGui styling.
void ApplyImGuiTheme();
void ApplyAccentToImGuiStyle();

// Small drawing and cursor helpers defined in ui.cpp.
float HoverProgress(uint64_t key, bool hovered);
Color Mix(Color from, Color to, float t);
void MarkHand();
void MarkResize(bool vertical);   // resize cursor for this frame: up-down when `vertical`, else left-right
void DrawTextArial(const char* text, float posX, float posY, float fontSize, Color color);
float MeasureTextArial(const char* text, float fontSize);
extern bool g_clickConsumedThisFrame;

// ui_terrain_panel.cpp: the terrain tool panel (bottom-right dock).
Rectangle GetTerrainPanelBounds();
void DrawImGuiTerrainPanel();

// ui.cpp / ui_properties.cpp

constexpr float kScriptPanelHeaderH = 26.0f;
constexpr float kScriptPanelRowH = 19.0f;
constexpr int kScriptPanelMaxRows = 7;

extern Color* g_color;
extern Texture2D g_deleteIcon;
extern Vector3* g_origin;
extern bool g_playActive;
extern Vector3* g_pos;
extern Vector3* g_rotation;
extern ScatteredObject* g_selectedObject;
extern WaterBody* g_selectedWater;
extern phys::Simulation* g_simulation;
extern Vector3* g_size;
extern ScriptBuildState g_scriptBuildState;
extern std::vector<ScriptDiagnostic> g_scriptDiagnostics;
extern double g_scriptBuildStarted;
extern bool g_scriptPanelDismissed;
extern int g_scriptPanelScroll;

void DrawCenteredTextArial(const char* text, const Rectangle& rec, float fontSize, Color color);
Rectangle GetPropertiesPanelBounds();
Rectangle GetScriptPanelBounds();
// The last component of a path, split on either slash.
inline std::string FileName(const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}
// A row of the Properties and Terrain panels: a muted label, then the next widget filling the rest of the row.
float PropertyLabelWidth();
// Height of the Properties and Terrain title strips, which ui_layout.cpp uses as drag handles: widgets start below.
constexpr float kPanelTitleH = 38.0f;
void PropertyRow(const char* label);
void DrawImGuiPropertiesPanel();
void DrawScriptErrorPanel();

// ui_explorer.cpp / ui.cpp (Explorer)
enum class ExplorerMenuMode { Object, Empty, Script, Model, Node };
extern int g_explorerAnchorIndex;
extern ExplorerMenuMode g_explorerMenuMode;
extern ScatteredObject* g_explorerMenuObject;
extern bool g_explorerMenuOpen;
extern double g_explorerMenuOpenTime;
extern Vector2 g_explorerMenuPos;
extern int g_explorerMenuScript;
extern int g_lightingSel;
// The selected child node (light / decal / texture) of an object, shown by ui_nodes.cpp. Null object = none.
extern ScatteredObject* g_selectedNodeObj;
extern int g_selectedNodeIdx;
extern char g_nameBuffer[256];
// Copies `name` into g_nameBuffer; a name that does not fit is cut at a UTF-8 character boundary.
inline void SetNameBuffer(const std::string& name) {
    size_t n = name.size() < sizeof(g_nameBuffer) ? name.size() : sizeof(g_nameBuffer) - 1;
    while (n > 0 && n < name.size() && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) --n;
    memcpy(g_nameBuffer, name.data(), n);
    g_nameBuffer[n] = '\0';
}
extern ScatteredObject* g_primarySelection;
extern terrain::Terrain* g_renameLegacyTerrain;
extern ModelGroup* g_renameModel;
extern ScatteredObject* g_renameObject;
extern BasicTerrain* g_renameTerrain;
extern char g_scriptNameBuffer[64];
extern int g_scriptRenameIndex;
extern std::set<std::string> g_scriptSel;
extern int g_scriptSelAnchor;
extern BasicTerrain* g_selectedTerrain;
Rectangle GetExplorerPanelBounds();
ScatteredObject* KeepPrimary(const std::vector<ScatteredObject*>& selection, ScatteredObject* candidate);
ImTextureID RowTex(RowIconType i);
// A plugin's own PNG (absolute path), loaded on first use and kept for the session; 0 when it cannot be loaded.
ImTextureID PluginIconTex(const std::string& path);
ModelGroup* CreateEmptyGroup(ModelGroup* parent);
void UngroupModel(ModelGroup* model);
std::vector<ScatteredObject*> DissolveModelTree(ModelGroup* model);
void MoveObjectsToGroup(const std::vector<ScatteredObject*>& dragged, ModelGroup* target, ScatteredObject* ref, bool after);
bool MoveGroupTo(ModelGroup* g, ModelGroup* newParent, ModelGroup* ref, bool after);
void DrawImGuiExplorer();
void DrawImGuiExplorerMenu();

} // namespace ui
