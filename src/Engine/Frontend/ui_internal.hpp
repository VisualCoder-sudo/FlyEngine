// Private to src/Engine/Frontend: state and helpers shared between the ui_*.cpp files.
#pragma once
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "raylib.h"
#include "imgui.h"
#include <cstdint>
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

// ui_preferences.cpp: the Preferences window. The top bar's Edit menu opens it.
extern bool g_showPreferences;
extern int g_prefsCategory;
void DrawImGuiPreferencesWindow();

// Small drawing and cursor helpers defined in ui.cpp.
float HoverProgress(uint64_t key, bool hovered);
Color Mix(Color from, Color to, float t);
void MarkHand();
void DrawTextArial(const char* text, float posX, float posY, float fontSize, Color color);
float MeasureTextArial(const char* text, float fontSize);
void DrawDropdownBox(const Rectangle& r, bool active, float alpha = 1.0f);
extern bool g_clickConsumedThisFrame;

// ui_terrain_panel.cpp: the terrain tool panel (bottom-right dock).
extern bool g_terrainToolDropdownListPending;
extern Rectangle g_terrainToolDropdownListRect;
extern int g_terrainToolDropdownActiveTool;
extern bool g_terrainToolDropdownOpen;
void DrawTerrainToolIcon(int kind, Rectangle ir, Color col);
Rectangle GetTerrainPanelBounds();
void DrawTerrainToolPanel();

// ui.cpp / ui_properties.cpp
enum FieldID {
    FIELD_NONE = 0,
    FIELD_POS_X, FIELD_POS_Y, FIELD_POS_Z,
    FIELD_SIZE_W, FIELD_SIZE_H, FIELD_SIZE_L,
    FIELD_ROT_X, FIELD_ROT_Y, FIELD_ROT_Z,
    FIELD_ORIGIN_X, FIELD_ORIGIN_Y, FIELD_ORIGIN_Z,
    FIELD_VEL_X, FIELD_VEL_Y, FIELD_VEL_Z,
    FIELD_ANG_X, FIELD_ANG_Y, FIELD_ANG_Z,
    FIELD_MASS,
    FIELD_TRANSPARENCY,
    FIELD_OCEAN_SPECTRUM_RESOLUTION,
    FIELD_OCEAN_SHALLOW_R, FIELD_OCEAN_SHALLOW_G, FIELD_OCEAN_SHALLOW_B,
    FIELD_OCEAN_DEEP_R, FIELD_OCEAN_DEEP_G, FIELD_OCEAN_DEEP_B,
    FIELD_OCEAN_FOG_R, FIELD_OCEAN_FOG_G, FIELD_OCEAN_FOG_B,
    FIELD_OCEAN_FRESNEL,
    FIELD_OCEAN_FOG_DENSITY,
    FIELD_OCEAN_SPECULAR,
    FIELD_OCEAN_REFRACT_EN,
    FIELD_OCEAN_REFRACT_STR,
    FIELD_OCEAN_AUTO_RES,
    FIELD_OCEAN_BASE_RES,
    FIELD_OCEAN_WAVE_SPACING,
    // Wave-collision tuning
    FIELD_OCEAN_REFLECT,
    FIELD_OCEAN_PILEUP,
    FIELD_OCEAN_FORCE_GAIN,
    FIELD_OCEAN_FORCE_BAND,
    // Water body fields
    FIELD_WATER_POS_X, FIELD_WATER_WATER_H, FIELD_WATER_POS_Z,
    FIELD_WATER_SIZE_W, FIELD_WATER_SIZE_D,
    FIELD_WATER_TRANSPARENCY,
    FIELD_WATER_AMPLITUDE, FIELD_WATER_FREQUENCY, FIELD_WATER_SPEED, FIELD_WATER_OCTAVES,
    FIELD_WATER_FOAM_INTENSITY, FIELD_WATER_FOAM_SCALE, FIELD_WATER_FOAM_THRESHOLD,
    FIELD_WATER_DETAIL_INTENSITY, FIELD_WATER_DETAIL_SCALE,
    FIELD_WATER_REFLECT_STRENGTH, FIELD_WATER_REFLECT_DISTORTION,
    FIELD_WATER_WAKE_STRENGTH, FIELD_WATER_SPLASH_STRENGTH, FIELD_WATER_FOAM_LIFE, FIELD_WATER_SPRAY_AMOUNT
};

constexpr float kScriptPanelHeaderH = 26.0f;
constexpr float kScriptPanelRowH = 19.0f;
constexpr int kScriptPanelMaxRows = 7;

extern FieldID g_activeField;
extern Rectangle g_activeFieldRect;
extern int g_activeSlider;
extern bool g_angularVelocityOpen;
extern Rectangle g_collisionPopupAnchor;
extern bool g_collisionPopupOpen;
extern double g_collisionPopupOpenTime;
extern Color* g_color;
extern float g_colorPickerAnchorY;
extern int g_cursorPos;
extern Texture2D g_deleteIcon;
extern bool g_generalOpen;
extern Vector3 g_hsv;
extern bool g_linearVelocityOpen;
extern bool g_mouseDrag;
extern Vector3* g_origin;
extern bool g_originOpen;
extern bool g_playActive;
extern Vector3* g_pos;
extern bool g_positionOpen;
extern Rectangle g_presetDropdownHeaderRect;
extern std::vector<std::string> g_presetDropdownItems;
extern bool g_presetDropdownItemsCached;
extern double g_presetDropdownLastRefresh;
extern std::vector<std::string> g_presetDropdownListItems;
extern bool g_presetDropdownListPending;
extern std::vector<std::string> g_presetDropdownListPresets;
extern std::string g_presetDropdownListProjectDir;
extern Rectangle g_presetDropdownListRect;
extern bool g_presetDropdownOpen;
extern std::string g_presetDropdownProjectDir;
extern int g_presetDropdownSelected;
extern float g_propertiesScrollY;
extern Vector3* g_rotation;
extern bool g_rotationOpen;
extern ScatteredObject* g_selectedObject;
extern WaterBody* g_selectedWater;
extern int g_selStart;
extern bool g_showColorPickerWindow;
extern phys::Simulation* g_simulation;
extern Vector3* g_size;
extern bool g_sizeOpen;
extern char g_textBuffer[32];
extern bool g_waterDetailOpen;
extern bool g_waterFoamOpen;
extern bool g_waterInteractOpen;
extern bool g_waterNoiseOpen;
extern bool g_waterReflectOpen;
extern bool g_waterWakeOpen;
extern ScriptBuildState g_scriptBuildState;
extern std::vector<ScriptDiagnostic> g_scriptDiagnostics;
extern double g_scriptBuildStarted;
extern bool g_scriptPanelDismissed;
extern int g_scriptPanelScroll;

void DrawCenteredTextArial(const char* text, const Rectangle& rec, float fontSize, Color color);
void GetColorPickerWindowBounds(Rectangle& popupRec);
Rectangle GetPropertiesPanelBounds();
Rectangle GetScriptPanelBounds();
void MarkIbeam();
bool TextEditKeys(char* buf, int bufSize, int& cursor, int& sel, bool& commit);
void TextEditMouse(const char* buf, int& cursor, int& sel, Rectangle rec,
                   float textStartX, float fontSize, bool active);
void DrawTextSel(const char* buf, int cursor, int sel, float textStartX,
                 float y, float height, float fontSize, Color color);
std::string FitTextToWidth(const std::string& text, float fontSize, float maxWidth);
void DrawCollisionPopup();
void DrawColorPickerPopupWindow();
void DrawPropertiesPanel();
void DrawScriptErrorPanel();

// ui_explorer.cpp / ui.cpp (Explorer)
enum class ExplorerMenuMode { Object, Empty, Script, Model };
struct ExplorerEntry {
    ScatteredObject* obj = nullptr;
    ModelGroup* model = nullptr;
    class WaterBody* water = nullptr;
    BasicTerrain* basicTerrain = nullptr;
    class terrain::Terrain* legacyTerrain = nullptr;
    bool isHeader = false;
    int objRowIndex = -1;
};
extern int g_explorerAnchorIndex;
extern bool g_explorerDragActive;
extern std::vector<ScatteredObject*> g_explorerDragObjects;
extern bool g_explorerDragPending;
extern Vector2 g_explorerDragStart;
extern ModelGroup* g_explorerDropTarget;
extern bool g_explorerDropValid;
extern ExplorerMenuMode g_explorerMenuMode;
extern ScatteredObject* g_explorerMenuObject;
extern bool g_explorerMenuOpen;
extern double g_explorerMenuOpenTime;
extern Vector2 g_explorerMenuPos;
extern int g_explorerMenuScript;
extern ScatteredObject* g_lastExplorerClick;
extern double g_lastExplorerClickTime;
extern ModelGroup* g_lastExplorerModelClick;
extern int g_lightingSel;
extern char g_nameBuffer[64];
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
std::vector<ExplorerEntry> BuildExplorerEntries();
int ExplorerMenuItemCount();
Rectangle GetExplorerPanelBounds();
ScatteredObject* KeepPrimary(const std::vector<ScatteredObject*>& selection, ScatteredObject* candidate);
ImTextureID RowTex(RowIconType i);
const char* ExplorerMenuItemLabel(int i);
void PerformExplorerDrop(const std::vector<ScatteredObject*>& dragged, ModelGroup* target);
void DrawImGuiExplorer();
void DrawImGuiExplorerMenu();

} // namespace ui
