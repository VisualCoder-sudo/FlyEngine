// ui.cpp (fixed)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CloseWindow Win32CloseWindow
#define ShowCursor Win32ShowCursor
#define Rectangle Win32Rectangle
#include <windows.h>
#undef CloseWindow
#undef ShowCursor
#undef Rectangle
#undef LoadImage
#undef DrawText
#undef DrawTextEx
#undef PlaySound

#include "ui.hpp"
#include "ScatteredObject.hpp"
#include "WaterBody.hpp"
#include "BasicTerrain.hpp"
#include "CommandConsole.hpp"
#include "Flyscript.hpp"
#include "ScriptEditor.hpp"
#include "PhysicsSimulation.hpp"
#include "TextureManager.hpp"
#include "ProjectManager.hpp"
#include "ModelImport.hpp"
#include "Terrain.hpp"
#include "TerrainEditor.hpp"
#include "TerrainRegistry.hpp"
#include "raymath.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_raylib.h"
#include "rlgl.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <algorithm>
#include <utility>

namespace ui {

static Font g_arialFont = { 0 };
static Texture2D g_toolIcons[5] = {};
static Texture2D g_deleteIcon = { 0 };
static Texture2D g_warningIcon = { 0 };
// Explorer row-type icons: object/model, water, terrain, script.
enum RowIconType { ROWICON_OBJECT = 0, ROWICON_WATER, ROWICON_TERRAIN, ROWICON_SCRIPT, ROWICON_COUNT };
static Texture2D g_rowIcons[ROWICON_COUNT] = {};
// Terrain sculpt tool icons (Raise/Lower/Smooth/Flatten/Paint/None), matching
// BasicTerrain::Tool's ordering.
enum TerrainToolIconType { TTOOLICON_RAISE = 0, TTOOLICON_LOWER, TTOOLICON_SMOOTH, TTOOLICON_FLATTEN, TTOOLICON_PAINT, TTOOLICON_NONE, TTOOLICON_COUNT };
static Texture2D g_terrainToolIcons[TTOOLICON_COUNT] = {};
static bool g_scriptWarningActive = false;
static bool g_scriptWarningApproved = false;
static std::vector<flyscript::ScriptWarning> g_scriptWarnings;
static Vector3* g_pos = nullptr;
static Vector3* g_size = nullptr;
static Color* g_color = nullptr;
static Vector3* g_rotation = nullptr;
static Vector3* g_origin = nullptr;
static const std::vector<ScatteredObject*>* g_sceneObjects = nullptr;
static std::vector<std::unique_ptr<ModelGroup>>* g_sceneModels = nullptr;
static const std::vector<WaterBody*>& SceneWaters() {
    return WaterBody::GetInstances();
}
static ScatteredObject* g_selectedObject = nullptr;
static phys::Simulation* g_simulation = nullptr;
static std::vector<ScatteredObject*> g_selection;
static ScatteredObject* g_primarySelection = nullptr;
static int g_explorerAnchorIndex = -1; // index into the flattened explorer list for shift-range
static ScatteredObject* g_renameObject = nullptr;
static ModelGroup* g_renameModel = nullptr;
static BasicTerrain* g_renameTerrain = nullptr;
static terrain::Terrain* g_renameLegacyTerrain = nullptr;
static ScatteredObject* g_lastExplorerClick = nullptr;
static ModelGroup* g_lastExplorerModelClick = nullptr;
static double g_lastExplorerClickTime = 0.0;
static WaterBody* g_selectedWater = nullptr;
static BasicTerrain* g_selectedTerrain = nullptr;
static char g_nameBuffer[64] = "";
static float g_colorPickerAnchorY = -1.0f;

// Explorer drag-and-drop: press on an object row, drag it onto a model header
// (or into empty SCENE space) to reparent it into/out of a model group.
static bool g_explorerDragPending = false;
static bool g_explorerDragActive = false;
static std::vector<ScatteredObject*> g_explorerDragObjects;
static Vector2 g_explorerDragStart = { 0.0f, 0.0f };
static ModelGroup* g_explorerDropTarget = nullptr;
static bool g_explorerDropValid = false;

// Preset texture dropdown state
static bool g_presetDropdownOpen = false;
static int g_presetDropdownSelected = -1;  // -1 = none chosen, 0 = "None", 1+ = preset index
static std::vector<std::string> g_presetDropdownItems;  // project-relative paths
static bool g_presetDropdownItemsCached = false;
static std::string g_presetDropdownProjectDir;  // to detect project change
static double g_presetDropdownLastRefresh = 0.0;  // for auto-refresh

// Preset dropdown list rendering state (for drawing after scissor ends)
static bool g_presetDropdownListPending = false;
static Rectangle g_presetDropdownListRect = { 0 };
static Rectangle g_presetDropdownHeaderRect = { 0 };
static std::vector<std::string> g_presetDropdownListItems;
static std::vector<std::string> g_presetDropdownListPresets;
static std::string g_presetDropdownListProjectDir;

// Global click consumption flag - set when a dropdown/popup handles a click
// to prevent other UI elements from also processing the click
static bool g_clickConsumedThisFrame = false;

// Dear ImGui integration (debug overlay for the migration work)
static bool g_showDemoWindow = false;

// ImGui-driven rename state: signals the inline rename field to grab focus.
static bool g_imFocusNextRename = false;
static std::vector<ScatteredObject*> g_imDragPayload; // explorer drag payload buffer

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
constexpr Color ACCENT           = Color{ 0, 190, 200, 255 };
constexpr Color ACCENT_HOVER     = Color{ 40, 210, 220, 255 };
constexpr Color ACCENT_PRESSED   = Color{ 0, 148, 158, 255 };
constexpr Color ACCENT_SOFT      = Color{ 0, 190, 200, 42 };
constexpr Color TEXT             = Color{ 232, 232, 238, 255 };
constexpr Color TEXT_MUTED       = Color{ 156, 161, 172, 255 };
constexpr Color TEXT_DIM         = Color{ 122, 128, 140, 255 };
constexpr Color DANGER           = Color{ 216, 84, 84, 255 };
constexpr Color DANGER_HOVER     = Color{ 236, 112, 112, 255 };
constexpr Color SUCCESS          = Color{ 74, 176, 104, 255 };
constexpr Color SHADOW           = Color{ 0, 0, 0, 92 };

} // namespace theme

// ---------------------------------------------------------------------------
// Anonymous namespace: internal linkage definitions
// ---------------------------------------------------------------------------
namespace {

enum class ExplorerMenuMode { Object, Empty, Script, Model };

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
    FIELD_WATER_FOAM_INTENSITY, FIELD_WATER_FOAM_SCALE, FIELD_WATER_FOAM_THRESHOLD
};

std::unordered_map<uint64_t, float> g_hoverAlpha;

float HoverProgress(uint64_t key, bool hovered) {
    float dt = GetFrameTime();
    float& a = g_hoverAlpha[key];
    a += hovered ? dt * 14.0f : -dt * 12.0f;
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return a;
}

Color Mix(Color from, Color to, float t) {
    return ColorLerp(from, to, t);
}

float MenuAnim(double openTime) {
    auto anim = static_cast<float>((GetTime() - openTime) / 0.09);
    if (anim > 1.0f) anim = 1.0f;
    if (anim < 0.0f) anim = 0.0f;
    return anim;
}

bool g_cursorHand = false;
bool g_cursorIbeam = false;

void BeginCursorPass() { g_cursorHand = false; g_cursorIbeam = false; }
void MarkHand() { g_cursorHand = true; }
void MarkIbeam() { g_cursorIbeam = true; }

void EndCursorPass() {
    SetMouseCursor(g_cursorIbeam ? MOUSE_CURSOR_IBEAM
        : g_cursorHand ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_DEFAULT);
}

} // namespace

// Active Toolbar State
static TransformTool g_activeTool = TransformTool::Select;
static bool g_transformLocalSpace = true; // true = Local, false = World

// HSV Color Picker & Popup State
static Vector3 g_hsv = { 0.0f, 0.0f, 1.0f };
static int g_activeSlider = 0; // 0 = None, 1 = Hue, 2 = Saturation, 3 = Brightness
static bool g_showColorPickerWindow = false;
static Color g_vec3PickColor = BLACK;

// Collision accuracy combo popup state (properties panel)
static bool g_collisionPopupOpen = false;
static double g_collisionPopupOpenTime = 0.0;
static Rectangle g_collisionPopupAnchor = { 0.0f, 0.0f, 0.0f, 0.0f };

// Context Menu State
static bool g_menuOpen = false;
static Vector2 g_menuPos = { 0.0f, 0.0f };
static bool g_isObjectTarget = false;
static bool g_isWaterTarget = false;
static bool g_showSubMenu = false;
static double g_menuOpenTime = 0.0;
static bool g_fileMenuOpen = false;
static double g_fileMenuOpenTime = 0.0;
static MenuAction g_pendingFileAction = MenuAction::None; // set by the ImGui File menu, consumed by ProcessContextMenu()

// Play Mode State
static bool g_playActive = false;
static bool g_playTogglePending = false;
static double g_playEndedAt = 0.0;
static bool g_importRequested = false;

// Explorer right-click context menu (separate from the 3D-view menu)
static bool g_explorerMenuOpen = false;
static Vector2 g_explorerMenuPos = { 0.0f, 0.0f };
static ExplorerMenuMode g_explorerMenuMode = ExplorerMenuMode::Empty;
static ScatteredObject* g_explorerMenuObject = nullptr;
static int g_explorerMenuScript = -1;
static double g_explorerMenuOpenTime = 0.0;

// Vertical scroll offset (pixels) for the explorer panel content
static float g_explorerScrollY = 0.0f;

// Vertical scroll offset (pixels) for the properties panel content
static float g_propertiesScrollY = 0.0f;

// Standalone script rename state
static int g_scriptRenameIndex = -1;
static char g_scriptNameBuffer[64] = "";

static FieldID g_activeField = FIELD_NONE;
static char g_textBuffer[32] = "";
static int g_cursorPos = 0;
static int g_selStart = -1;  // selection anchor index, -1 = no selection
static bool g_mouseDrag = false;

// Collapsible property-group state (all open by default).
static bool g_generalOpen = true;
static bool g_positionOpen = true;
static bool g_sizeOpen = true;
static bool g_rotationOpen = true;
static bool g_originOpen = true;
static bool g_linearVelocityOpen = true;
static bool g_angularVelocityOpen = true;
static bool g_waterNoiseOpen = true;
static bool g_waterFoamOpen = true;

// Screen rect of the currently active number field
static Rectangle g_activeFieldRect = { 0.0f, 0.0f, 0.0f, 0.0f };

// Forward declarations
static bool TextEditKeys(char* buf, int bufSize, int& cursor, int& sel, bool& commit);
static void TextEditMouse(const char* buf, int& cursor, int& sel, Rectangle rec,
                          float textStartX, float fontSize, bool active);
static void DrawTextSel(const char* buf, int cursor, int sel, float textStartX,
                        float y, float height, float fontSize, Color color);
static void ApplyImGuiTheme();

void Init() {
    if (FileExists("C:/Windows/Fonts/arial.ttf")) {
        g_arialFont = LoadFontEx("C:/Windows/Fonts/arial.ttf", 32, nullptr, 0);
    } else if (FileExists("arial.ttf")) {
        g_arialFont = LoadFontEx("arial.ttf", 32, nullptr, 0);
    } else {
        g_arialFont = GetFontDefault();
    }
    SetTextureFilter(g_arialFont.texture, TEXTURE_FILTER_BILINEAR);

    constexpr const char* toolIconNames[] = { "select.png", "move.png", "scale.png", "rotate.png", "import.png" };
    for (int i = 0; i < 5; ++i) {
        std::string path = std::string("assets/EditorIcons/") + toolIconNames[i];
        if (!FileExists(path.c_str())) path = std::string("../assets/EditorIcons/") + toolIconNames[i];
        Image img = LoadImage(path.c_str());
        if (img.data != nullptr) {
            ImageColorBrightness(&img, 255); // black silhouette -> white for dark theme
            g_toolIcons[i] = LoadTextureFromImage(img);
            UnloadImage(img); // Free CPU RAM immediately after GPU upload
        }
    }

    // Load delete icon
    std::string deletePath = "assets/delete.png";
    if (!FileExists(deletePath.c_str())) deletePath = "../assets/delete.png";
    Image delImg = LoadImage(deletePath.c_str());
    if (delImg.data != nullptr) {
        ImageColorBrightness(&delImg, 255);
        g_deleteIcon = LoadTextureFromImage(delImg);
        UnloadImage(delImg);
    }

    std::string warnPath = "assets/Misc/warning.png";
    if (!FileExists(warnPath.c_str())) warnPath = "../assets/Misc/warning.png";
    Image warnImg = LoadImage(warnPath.c_str());
    if (warnImg.data != nullptr) {
        g_warningIcon = LoadTextureFromImage(warnImg);
        UnloadImage(warnImg);
    }

    constexpr const char* rowIconNames[ROWICON_COUNT] = { "cube.png", "water.png", "terrain.png", "script.png" };
    for (int i = 0; i < ROWICON_COUNT; ++i) {
        std::string path = std::string("assets/EditorIcons/") + rowIconNames[i];
        if (!FileExists(path.c_str())) path = std::string("../assets/EditorIcons/") + rowIconNames[i];
        Image img = LoadImage(path.c_str());
        if (img.data != nullptr) {
            ImageColorBrightness(&img, 255); // black silhouette -> white for dark theme
            g_rowIcons[i] = LoadTextureFromImage(img);
            UnloadImage(img);
        }
    }

    constexpr const char* terrainToolIconNames[TTOOLICON_COUNT] = {
        "arrowup.png", "arrowdown.png", "smooth.png", "linehoriz.png", "paint.png", "none.png"
    };
    for (int i = 0; i < TTOOLICON_COUNT; ++i) {
        std::string path = std::string("assets/EditorIcons/") + terrainToolIconNames[i];
        if (!FileExists(path.c_str())) path = std::string("../assets/EditorIcons/") + terrainToolIconNames[i];
        Image img = LoadImage(path.c_str());
        if (img.data != nullptr) {
            ImageColorBrightness(&img, 255); // black silhouette -> white so DrawTexturePro's tint applies
            g_terrainToolIcons[i] = LoadTextureFromImage(img);
            UnloadImage(img);
        }
    }

    scriptEditor::Init();

    // Dear ImGui setup. Runs after InitWindow created the GL context (ScopedUI
    // in main.cpp), so the OpenGL3 backend can probe the driver safely.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;               // no .ini config persistence for now
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Match the app's Arial look for ImGui text; include the symbol blocks used
    // by the explorer rows (▣ ▢ ⛰ arrows) so labels render correctly.
    ImFontConfig fontCfg;
    fontCfg.OversampleH = 1;
    fontCfg.OversampleV = 1;
    static const ImWchar glyphRanges[] = { 0x20, 0xFF, 0x2190, 0x21FF, 0x2200, 0x22FF, 0x2500, 0x25FF, 0x2600, 0x26FF, 0 };
    if (FileExists("C:/Windows/Fonts/arial.ttf")) {
        io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/arial.ttf", 15.0f, &fontCfg, glyphRanges);
    } else if (FileExists("arial.ttf")) {
        io.Fonts->AddFontFromFileTTF("arial.ttf", 15.0f, &fontCfg, glyphRanges);
    } else {
        io.Fonts->AddFontDefault();
    }

    ApplyImGuiTheme();
    ImGui_ImplRaylib_Init();
    ImGui_ImplOpenGL3_Init("#version 130");
}

void Unload() {
    // ImGui shutdown first: it still needs the GL context which is alive until
    // the Engine (and thus CloseWindow) is destroyed after ui::Unload().
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplRaylib_Shutdown();
    ImGui::DestroyContext();

    scriptEditor::Unload();
    for (int i = 0; i < 5; ++i) {
        if (g_toolIcons[i].id != 0) UnloadTexture(g_toolIcons[i]);
        g_toolIcons[i] = {};
    }
    if (g_deleteIcon.id != 0) UnloadTexture(g_deleteIcon);
    g_deleteIcon = {};
    if (g_warningIcon.id != 0) UnloadTexture(g_warningIcon);
    g_warningIcon = {};
    for (int i = 0; i < ROWICON_COUNT; ++i) {
        if (g_rowIcons[i].id != 0) UnloadTexture(g_rowIcons[i]);
        g_rowIcons[i] = {};
    }
    for (int i = 0; i < TTOOLICON_COUNT; ++i) {
        if (g_terrainToolIcons[i].id != 0) UnloadTexture(g_terrainToolIcons[i]);
        g_terrainToolIcons[i] = {};
    }
    if (g_arialFont.texture.id != GetFontDefault().texture.id) {
        UnloadFont(g_arialFont);
    }
}

void SetSelectedObject(Vector3* pos, Vector3* size, Color* color) {
    SetSelectedObject(pos, size, color, nullptr);
}

void SetSelectedObject(Vector3* pos, Vector3* size, Color* color, Vector3* rotation, Vector3* origin) {
    g_pos = pos;
    g_size = size;
    g_color = color;
    g_rotation = rotation;
    g_origin = origin;
    g_activeField = FIELD_NONE;
    g_cursorPos = 0;
    g_selStart = -1;
    g_mouseDrag = false;
    g_activeSlider = 0;
    g_showColorPickerWindow = false;

    if (g_color != nullptr) {
        g_hsv = ColorToHSV(*g_color);
    }
}

void SetSelectedObject(ScatteredObject* object) {
    g_selectedObject = object;
}

void SetSceneObjects(const std::vector<ScatteredObject*>* objects) {
    g_sceneObjects = objects;
}

void SetSceneModels(std::vector<std::unique_ptr<ModelGroup>>* models) {
    g_sceneModels = models;
}

void SetSimulation(phys::Simulation* sim) {
    g_simulation = sim;
}

// ---------------------------------------------------------------------------
// On-screen output log
// ---------------------------------------------------------------------------
namespace {

constexpr int kLogCapacity = 64;
constexpr int kLogLineLen = 200;

struct LogEntry {
    char text[kLogLineLen];
    double time;
};

LogEntry g_log[kLogCapacity];
int g_logWrite = 0;   // next slot to write
int g_logCount = 0;   // total entries currently held

} // namespace

static void LogImpl(const char* fmt, va_list args) {
    char buf[kLogLineLen];
    vsnprintf(buf, sizeof(buf), fmt, args);

    snprintf(g_log[g_logWrite].text, sizeof(g_log[g_logWrite].text), "%s", buf);
    g_log[g_logWrite].time = GetTime();
    g_logWrite = (g_logWrite + 1) % kLogCapacity;
    if (g_logCount < kLogCapacity) g_logCount++;
}

void Log(const char* fmt, ...) {
    if (!g_playActive) return;

    va_list args;
    va_start(args, fmt);
    LogImpl(fmt, args);
    va_end(args);
}

void LogAlways(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogImpl(fmt, args);
    va_end(args);
}

void ClearLog() {
    g_logWrite = 0;
    g_logCount = 0;
}

const std::vector<ScatteredObject*>& GetSelection() {
    return g_selection;
}

ScatteredObject* GetPrimarySelection() {
    return g_primarySelection;
}

void SetSelection(const std::vector<ScatteredObject*>& selection, ScatteredObject* primary) {
    // Selecting actual objects hands editor focus away from the terrain tool
    // panel. Empty selections (deselect / sculpt strokes) leave it alone.
    if (!selection.empty()) BasicTerrain::SetActive(nullptr);

    // Clear water body selection when selecting ScatteredObjects
    if (!selection.empty() && g_selectedWater) {
        WaterBody* prev = g_selectedWater;
        g_selectedWater = nullptr;
        if (prev) prev->isSelected = false;
    }

    // Clear terrain selection when selecting ScatteredObjects
    if (!selection.empty() && g_selectedTerrain) {
        BasicTerrain* prev = g_selectedTerrain;
        g_selectedTerrain = nullptr;
        if (prev) prev->isSelected = false;
    }

    // Mark/unmark the isSelected flag on every scene object so the 3D
    // wireframe highlight and explorer rows agree with the selection set.
    if (g_sceneObjects) {
        for (auto* obj : *g_sceneObjects) {
            if (obj) obj->isSelected = false;
        }
    }
    for (auto* obj : selection) {
        if (obj) obj->isSelected = true;
    }

    g_selection = selection;
    g_primarySelection = primary;
    g_selectedObject = primary;

    if (primary) {
        ui::SetSelectedObject(primary->GetPosPtr(), primary->GetSizePtr(),
            primary->GetColorPtr(), primary->GetRotationPtr(),
            primary->GetOriginPtr());
    } else {
        ui::SetSelectedObject(nullptr, nullptr, nullptr);
    }
}

void SetSelectedWater(WaterBody* w) {
    if (w == g_selectedWater) return;
    // Clear previous
    if (g_selectedWater) g_selectedWater->isSelected = false;
    // Clear ScatteredObject selection
    SetSelection({}, nullptr);
    g_selectedWater = w;
    if (w) w->isSelected = true;
    g_activeField = FIELD_NONE;
    g_showColorPickerWindow = false;
}

WaterBody* GetSelectedWater() { return g_selectedWater; }

void SetSelectedTerrain(BasicTerrain* t) {
    if (t == g_selectedTerrain) return;
    // Clear previous
    if (g_selectedTerrain) {
        g_selectedTerrain->isSelected = false;
        g_selectedTerrain->editActive = false;
    }
    // Clear ScatteredObject selection
    SetSelection({}, nullptr);
    // Clear water selection
    if (g_selectedWater) { g_selectedWater->isSelected = false; g_selectedWater = nullptr; }
    g_selectedTerrain = t;
    if (t) {
        t->isSelected = true;
        BasicTerrain::SetActive(t);
    } else {
        BasicTerrain::SetActive(nullptr);
    }
    g_activeField = FIELD_NONE;
    g_showColorPickerWindow = false;
}

BasicTerrain* GetSelectedTerrain() { return g_selectedTerrain; }

// Helpers that dissolve or build model containers. These live here (rather
// than in the interaction manager) because the explorer context menu triggers
// them, and the models vector is registered through SetSceneModels.
void GroupSelectedObjects() {
    if (!g_sceneModels || !g_sceneObjects) return;
    if (g_selection.size() < 2) return;

    auto model = std::make_unique<ModelGroup>();
    model->name = "Model";
    for (auto* obj : g_selection) {
        if (!obj) continue;
        if (obj->parentModel) {
            // A selected object already inside a model: pull it out first so
            // members never end up owned by two containers.
            for (auto& existing : *g_sceneModels) {
                auto& members = existing->members;
                members.erase(std::remove(members.begin(), members.end(), obj), members.end());
            }
            obj->parentModel = nullptr;
        }
        model->members.push_back(obj);
        obj->parentModel = model.get();
    }
    g_sceneModels->push_back(std::move(model));
}

void UngroupModelContaining(ScatteredObject* member) {
    if (!g_sceneModels) return;
    if (!member || !member->parentModel) return;

    for (auto it = g_sceneModels->begin(); it != g_sceneModels->end(); ++it) {
        if (it->get() == member->parentModel) {
            for (auto* m : (*it)->members) {
                if (m) m->parentModel = nullptr;
            }
            g_sceneModels->erase(it);
            return;
        }
    }
}

// Reparents dragged explorer objects. `target` is the model to join, or
// nullptr to drop the objects back at the workspace root (out of any model).
void PerformExplorerDrop(const std::vector<ScatteredObject*>& dragged, ModelGroup* target) {
    if (!g_sceneModels) return;

    for (auto* obj : dragged) {
        if (!obj) continue;
        if (obj->parentModel) {
            ModelGroup* model = obj->parentModel;
            model->members.erase(std::remove(model->members.begin(), model->members.end(), obj), model->members.end());
            obj->parentModel = nullptr;
        }
    }
    if (target) {
        for (auto* obj : dragged) {
            if (!obj) continue;
            target->members.push_back(obj);
            obj->parentModel = target;
        }
    }

    // Drop containers left empty (mirrors ObjectInteractionManager removal).
    g_sceneModels->erase(std::remove_if(g_sceneModels->begin(), g_sceneModels->end(),
        [](const std::unique_ptr<ModelGroup>& m) { return !m || m->members.empty(); }), g_sceneModels->end());
}

static std::vector<ScatteredObject*> g_pendingDelete;

void RequestDelete(const std::vector<ScatteredObject*>& targets) {
    for (auto* t : targets) {
        if (t && std::find(g_pendingDelete.begin(), g_pendingDelete.end(), t) == g_pendingDelete.end()) {
            g_pendingDelete.push_back(t);
        }
    }
}

std::vector<ScatteredObject*> ConsumePendingDelete() {
    std::vector<ScatteredObject*> pending = std::move(g_pendingDelete);
    g_pendingDelete.clear();
    return pending;
}

TransformTool GetActiveTool() {
    return g_activeTool;
}

bool IsTransformLocalSpace() {
    return g_transformLocalSpace;
}

void SetTransformLocalSpace(bool local) {
    g_transformLocalSpace = local;
}

void ToggleTransformSpace() {
    g_transformLocalSpace = !g_transformLocalSpace;
}

bool ConsumePlayToggle() {
    bool toggle = g_playTogglePending;
    g_playTogglePending = false;
    return toggle;
}

bool ConsumeImportRequest() {
    bool requested = g_importRequested;
    g_importRequested = false;
    return requested;
}

bool IsPlayActive() {
    return g_playActive;
}

void SetPlayActive(bool active) {
    g_playActive = active;
    if (!active) g_playEndedAt = GetTime();
    g_activeField = FIELD_NONE;
    g_cursorPos = 0;
    g_selStart = -1;
    g_mouseDrag = false;
    g_showColorPickerWindow = false;
    g_fileMenuOpen = false;
    g_explorerMenuOpen = false;
    g_scriptRenameIndex = -1;
    CloseContextMenu();
}

void ShowScriptWarning(const std::vector<flyscript::ScriptWarning>& warnings) {
    g_scriptWarnings = warnings;
    g_scriptWarningActive = true;
    g_scriptWarningApproved = false;
}

bool IsScriptWarningActive() {
    return g_scriptWarningActive;
}

void DismissScriptWarning() {
    g_scriptWarningActive = false;
    g_scriptWarnings.clear();
}

bool ConsumeScriptWarningApproved() {
    bool v = g_scriptWarningApproved;
    g_scriptWarningApproved = false;
    return v;
}

bool IsEditingText() {
    return g_activeField != FIELD_NONE || g_renameObject != nullptr || g_renameModel != nullptr
        || g_scriptRenameIndex >= 0 || g_renameTerrain != nullptr || g_renameLegacyTerrain != nullptr
        || console::IsActive() || scriptEditor::IsCapturingKeyboard()
        || ImGui::GetIO().WantTextInput;
}

void BlurAllInput() {
    g_activeField = FIELD_NONE;
    g_cursorPos = 0;
    g_selStart = -1;
    g_mouseDrag = false;
    if (g_renameObject != nullptr) {
        g_renameObject->SetName(g_nameBuffer);
        g_renameObject = nullptr;
    }
    if (g_renameModel != nullptr) {
        g_renameModel->name = g_nameBuffer;
        g_renameModel = nullptr;
    }
    if (g_renameTerrain != nullptr) {
        g_renameTerrain->SetName(g_nameBuffer);
        g_renameTerrain = nullptr;
    }
    if (g_renameLegacyTerrain != nullptr) {
        g_renameLegacyTerrain->SetName(g_nameBuffer);
        g_renameLegacyTerrain = nullptr;
    }
    if (g_scriptRenameIndex >= 0) {
        if (flyscript::IsRuntimeReady()) {
            auto& scripts = flyscript::GetRuntime().scripts;
            if (g_scriptRenameIndex >= 0 && g_scriptRenameIndex < static_cast<int>(scripts.size())) {
                scripts[g_scriptRenameIndex].name = g_scriptNameBuffer;
            }
        }
        g_scriptRenameIndex = -1;
    }
}

Font GetFont() {
    return g_arialFont;
}

static bool IsOverActiveNumberInput(Vector2 point) {
    if (g_activeField == FIELD_NONE) return false;
    return CheckCollisionPointRec(point, g_activeFieldRect);
}

// One row in the explorer's SCENE section: either a model header or an object.
// `objRowIndex` is the position of an object row in the flattened object list
// (used by shift-range selection); model headers share their first member's
// index (or -1 when the model is empty).
struct ExplorerEntry {
    ScatteredObject* obj = nullptr;
    ModelGroup* model = nullptr;
    class WaterBody* water = nullptr;
    BasicTerrain* basicTerrain = nullptr;
    class terrain::Terrain* legacyTerrain = nullptr;
    bool isHeader = false;
    int objRowIndex = -1;
};

static std::vector<ExplorerEntry> BuildExplorerEntries() {
    std::vector<ExplorerEntry> entries;
    int objRow = 0;
    if (g_sceneModels) {
        for (const auto& model : *g_sceneModels) {
            if (!model) continue;
            ExplorerEntry header;
            header.model = model.get();
            header.isHeader = true;
            header.objRowIndex = model->members.empty() ? -1 : objRow;
            entries.push_back(header);
            for (auto* m : model->members) {
                if (!m) continue;
                ExplorerEntry e;
                e.obj = m;
                e.objRowIndex = objRow++;
                entries.push_back(e);
            }
        }
    }
    if (g_sceneObjects) {
        for (auto* o : *g_sceneObjects) {
            if (!o || o->parentModel) continue;
            ExplorerEntry e;
            e.obj = o;
            e.objRowIndex = objRow++;
            entries.push_back(e);
        }
    }
    for (auto* w : SceneWaters()) {
        if (!w) continue;
        ExplorerEntry e;
        e.water = w;
        e.objRowIndex = objRow++;
        entries.push_back(e);
    }
    for (auto* t : BasicTerrain::GetInstances()) {
        if (!t) continue;
        ExplorerEntry e;
        e.basicTerrain = t;
        e.objRowIndex = objRow++;
        entries.push_back(e);
    }
    for (auto* t : terrain::GetTerrainRegistry().GetTerrains()) {
        if (!t) continue;
        ExplorerEntry e;
        e.legacyTerrain = t;
        e.objRowIndex = objRow++;
        entries.push_back(e);
    }
    return entries;
}

// Total rows (model headers + members + standalone objects + water bodies) in the SCENE
// section, for scroll clamping and the FLYSCRIPTS offset.
static int ExplorerRowCount() {
    int n = 0;
    if (g_sceneModels) {
        for (const auto& model : *g_sceneModels) {
            if (model) n += 1 + static_cast<int>(model->members.size());
        }
    }
    if (g_sceneObjects) {
        for (auto* o : *g_sceneObjects) {
            if (o && !o->parentModel) ++n;
        }
    }
    for (auto* w : SceneWaters()) {
        if (w) ++n;
    }
    for (auto* t : BasicTerrain::GetInstances()) {
        if (t) ++n;
    }
    for (auto* t : terrain::GetTerrainRegistry().GetTerrains()) {
        if (t) ++n;
    }
    return n;
}

// Keeps `candidate` as the primary when it is still selected, otherwise falls
// back to the first remaining selection.
static ScatteredObject* KeepPrimary(const std::vector<ScatteredObject*>& selection, ScatteredObject* candidate) {
    if (std::find(selection.begin(), selection.end(), candidate) != selection.end()) return candidate;
    return selection.empty() ? nullptr : selection.front();
}

void UpdateInput() {
    // Start the ImGui frame before anything else: widgets must be built every
    // frame and this runs even when the early-return below skips mouse handling.
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplRaylib_NewFrame();
    ImGui::NewFrame();

    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) return;

    Vector2 mouse = GetMousePosition();
    if (console::IsActive() && !console::IsOverBar(mouse)) {
        console::Blur();
    }
    if (g_activeField != FIELD_NONE && !IsOverActiveNumberInput(mouse)) {
        g_activeField = FIELD_NONE;
        g_selStart = -1;
        g_mouseDrag = false;
    }
    // Explorer rename (object/model/terrain/script) commit/cancel is handled by
    // the ImGui inline fields in DrawImGuiExplorer() (focus deactivation/ESC).
}

// ---------------------------------------------------------------------------
// Panel Bounding Rectangles
// ---------------------------------------------------------------------------

static Rectangle GetTopBarBounds() {
    return Rectangle{ 0.0f, 0.0f, static_cast<float>(GetScreenWidth()), 120.0f };
}

static Rectangle GetExplorerPanelBounds() {
    constexpr float topBarHeight = 120.0f;
    return Rectangle{ 0.0f, topBarHeight, 220.0f, static_cast<float>(GetScreenHeight()) - topBarHeight };
}

static Rectangle GetPropertiesPanelBounds() {
    // Top half of the right-hand column; GetTerrainPanelBounds() takes the
    // bottom half at the same width, docked bottom-right.
    constexpr float topBarHeight = 120.0f;
    constexpr float panelWidth = 240.0f;
    float totalH = static_cast<float>(GetScreenHeight()) - topBarHeight;
    return Rectangle{ static_cast<float>(GetScreenWidth()) - panelWidth, topBarHeight, panelWidth, totalH * 0.5f };
}

Rectangle GetConsoleBarArea() {
    Rectangle explorer = GetExplorerPanelBounds();
    Rectangle props = GetPropertiesPanelBounds();
    return Rectangle{ explorer.x + explorer.width, 0.0f, props.x - (explorer.x + explorer.width), static_cast<float>(GetScreenHeight()) };
}

static void GetColorPickerWindowBounds(Rectangle& popupRec) {
    Rectangle propRec = GetPropertiesPanelBounds();
    constexpr float popupW = 280.0f;
    constexpr float popupH = 210.0f;
    float popupX = propRec.x - popupW - 10.0f;
    float popupY = (g_colorPickerAnchorY >= 0.0f) ? g_colorPickerAnchorY : propRec.y + 120.0f;

    popupRec.x = popupX;
    popupRec.y = popupY;
    popupRec.width = popupW;
    popupRec.height = popupH;
}

// Shared spawn list for the "Add object" submenu. Keep in sync with
// DrawContextMenuInternal's item rendering and kSpawnMenuActions.
static constexpr const char* kSpawnMenuItems[] = { "Cube", "Sphere", "Cylinder", "Wedge", "Water Body", "Basic Terrain", "Import Terrain" };
static constexpr int kSpawnMenuItemCount = sizeof(kSpawnMenuItems) / sizeof(kSpawnMenuItems[0]);
static constexpr MenuAction kSpawnMenuActions[kSpawnMenuItemCount] = {
    MenuAction::SpawnCube, MenuAction::SpawnSphere, MenuAction::SpawnCylinder, MenuAction::SpawnWedge,
    MenuAction::SpawnWater, MenuAction::SpawnBasicTerrain, MenuAction::ImportTerrain
};

static void GetContextMenuBounds(Rectangle& mainRec, Rectangle& subRec) {
    auto screenW = static_cast<float>(GetScreenWidth());
    auto screenH = static_cast<float>(GetScreenHeight());

    if (g_isObjectTarget) {
        constexpr float mainWidth = 180.0f;
        constexpr float mainHeight = 64.0f;

        float x = g_menuPos.x;
        float y = g_menuPos.y;

        if (x + mainWidth > screenW) x = screenW - mainWidth - 4.0f;
        if (y + mainHeight > screenH) y = screenH - mainHeight - 4.0f;

        mainRec = { x, y, mainWidth, mainHeight };
        subRec = { 0.0f, 0.0f, 0.0f, 0.0f };
    } else {
        constexpr float mainWidth = 200.0f;
        constexpr float mainHeight = 64.0f;
        constexpr float subWidth = 130.0f;
        constexpr float subHeight = static_cast<float>(kSpawnMenuItemCount) * 30.0f + 8.0f;

        float x = g_menuPos.x;
        float y = g_menuPos.y;

        bool flipLeft = (x + mainWidth + subWidth > screenW);

        if (x + mainWidth > screenW) x = screenW - mainWidth - 4.0f;
        if (y + mainHeight > screenH) y = screenH - mainHeight - 4.0f;

        mainRec = { x, y, mainWidth, mainHeight };

        float subX = flipLeft ? (mainRec.x - subWidth) : (mainRec.x + mainRec.width);
        float subY = mainRec.y;

        if (subY + subHeight > screenH) subY = screenH - subHeight - 4.0f;

        subRec = { subX, subY, subWidth, subHeight };
    }
}

static const char* ExplorerMenuItemLabel(int i);

static int ExplorerMenuItemCount() {
    int n = 0;
    while (ExplorerMenuItemLabel(n) != nullptr) ++n;
    return n;
}

static Rectangle GetOutputPanelBounds() {
    constexpr double kEndSessionPanelSeconds = 5.0;
    if (g_logCount == 0) return Rectangle{ 0.0f, 0.0f, 0.0f, 0.0f };
    if (!g_playActive && GetTime() - g_playEndedAt >= kEndSessionPanelSeconds)
        return Rectangle{ 0.0f, 0.0f, 0.0f, 0.0f };
    constexpr float w = 560.0f;
    constexpr float h = 140.0f;
    float x = (static_cast<float>(GetScreenWidth()) - w) * 0.5f;
    Rectangle bar = console::GetBounds();
    return Rectangle{ x, bar.y - h - 8.0f, w, h };
}

// Terrain tool panel (defined near ui::Draw; needs bounds checks here)
static Rectangle GetTerrainPanelBounds();

bool IsMouseOverUI() {
    // The ImGui top bar + explorer panels capture the mouse whenever the
    // pointer is over one of their windows/popups.
    if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse) return true;

    Vector2 mouse = GetMousePosition();

    if (console::IsActive()) return true;

    Rectangle bounds = console::GetBounds();
    if (CheckCollisionPointRec(mouse, bounds)) return true;

    if (CheckCollisionPointRec(mouse, GetTopBarBounds())) return true;
    if (CheckCollisionPointRec(mouse, GetExplorerPanelBounds())) return true;
    if (CheckCollisionPointRec(mouse, GetPropertiesPanelBounds())) return true;

    Rectangle terrRec = GetTerrainPanelBounds();
    if (terrRec.width > 0.0f && CheckCollisionPointRec(mouse, terrRec)) return true;

    Rectangle outRec = GetOutputPanelBounds();
    if (outRec.width > 0.0f && CheckCollisionPointRec(mouse, outRec)) return true;

    if (scriptEditor::IsMouseOverWindow(mouse)) return true;

    // g_showColorPickerWindow
    if (g_showColorPickerWindow) {
        Rectangle popupRec;
        GetColorPickerWindowBounds(popupRec);
        if (CheckCollisionPointRec(mouse, popupRec)) return true;
    }

    // g_menuOpen - context menu
    if (g_menuOpen) {
        Rectangle mainRec, subRec;
        GetContextMenuBounds(mainRec, subRec);
        if (CheckCollisionPointRec(mouse, mainRec)) return true;
        if (g_showSubMenu && CheckCollisionPointRec(mouse, subRec)) return true;
    }

    return false;
}

void OpenContextMenu(Vector2 mousePos, bool isObjectTarget, bool isWaterTarget) {
    g_explorerMenuOpen = false;
    g_menuOpen = true;
    g_menuPos = mousePos;
    g_isObjectTarget = isObjectTarget;
    g_isWaterTarget = isWaterTarget;
    g_showSubMenu = !isObjectTarget && !isWaterTarget;
    g_menuOpenTime = GetTime();
}

void CloseContextMenu() {
    g_menuOpen = false;
    g_showSubMenu = false;
}

bool IsContextMenuOpen() {
    return g_menuOpen;
}

static void DrawTextArial(const char* text, float posX, float posY, float fontSize, Color color) {
    if (!text) return;
    Vector2 pos = { posX, posY };
    // Check if font is valid (texture.id != 0 means loaded)
    if (g_arialFont.texture.id == 0) {
        // Fallback to default font
        DrawText(text, static_cast<int>(posX), static_cast<int>(posY), static_cast<int>(fontSize), color);
        return;
    }
    DrawTextEx(g_arialFont, text, pos, fontSize, 1.0f, color);
}

static float MeasureTextArial(const char* text, float fontSize) {
    if (!text) return 0.0f;
    if (g_arialFont.texture.id == 0) {
        return static_cast<float>(MeasureText(text, static_cast<int>(fontSize)));
    }
    return MeasureTextEx(g_arialFont, text, fontSize, 1.0f).x;
}

static void DrawCenteredTextArial(const char* text, const Rectangle& rec, float fontSize, Color color) {
    float textW = MeasureTextArial(text, fontSize);
    float x = rec.x + (rec.width - textW) * 0.5f;
    float y = rec.y + (rec.height - fontSize) * 0.5f;
    DrawTextArial(text, x, y, fontSize, color);
}

static bool DrawDialogButton(Rectangle rec, const char* text, bool isPrimary) {
    Vector2 mouse = GetMousePosition();
    bool hovered = CheckCollisionPointRec(mouse, rec);
    bool pressed = hovered && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    bool clicked = hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    Color bg = isPrimary
        ? (pressed ? theme::ACCENT_PRESSED : (hovered ? theme::ACCENT_HOVER : theme::ACCENT))
        : (pressed ? theme::BG_WIDGET_PRESSED : (hovered ? theme::BG_WIDGET_HOVER : theme::BG_WIDGET));
    DrawRectangleRounded(rec, 0.18f, 4, bg);
    DrawRectangleLinesEx(rec, 1.0f, hovered ? theme::BORDER_STRONG : theme::BORDER);
    Color textColor = isPrimary ? Color{ 15, 17, 21, 255 } : theme::TEXT;
    DrawCenteredTextArial(text, rec, 14.0f, textColor);
    if (hovered) MarkHand();
    return clicked;
}

static void DrawScriptWarningDialog() {
    if (!g_scriptWarningActive) return;

    int sw = GetScreenWidth();
    int sh = GetScreenHeight();

    DrawRectangle(0, 0, sw, sh, Color{ 0, 0, 0, 140 });

    float panelW = 520.0f;
    float lineH = 22.0f;
    float headerH = 100.0f;
    float listH = static_cast<float>(g_scriptWarnings.size()) * lineH;
    float panelH = headerH + listH + 70.0f;
    float panelX = (sw - panelW) * 0.5f;
    float panelY = (sh - panelH) * 0.5f;
    Rectangle panelRec{ panelX, panelY, panelW, panelH };

    DrawRectangleRounded(panelRec, 0.08f, 6, theme::BG_PANEL);
    DrawRectangleLinesEx(panelRec, 1.0f, theme::BORDER_STRONG);

    if (g_warningIcon.id != 0) {
        float iconSize = 32.0f;
        float iconScale = iconSize / static_cast<float>(g_warningIcon.width);
        Vector2 iconPos = { panelX + 20.0f, panelY + 20.0f };
        DrawTextureEx(g_warningIcon, iconPos, 0.0f, iconScale, WHITE);
    }

    DrawTextArial("Warning", panelX + 62.0f, panelY + 18.0f, 18.0f, Color{ 235, 180, 60, 255 });
    DrawTextArial("Are you sure you would like to run this simulation?",
                  panelX + 62.0f, panelY + 44.0f, 14.0f, theme::TEXT);
    DrawTextArial("One of your scripts may cause the engine to crash.",
                  panelX + 62.0f, panelY + 64.0f, 13.0f, theme::TEXT_MUTED);

    float listY = panelY + headerH;
    for (size_t i = 0; i < g_scriptWarnings.size(); ++i) {
        const auto& w = g_scriptWarnings[i];
        char buf[256];
        snprintf(buf, sizeof(buf), "\"%s\" Line:%d", w.scriptName.c_str(), w.line);
        DrawTextArial(buf, panelX + 24.0f, listY + i * lineH, 13.0f, theme::DANGER);
    }

    float btnW = 120.0f;
    float btnH = 32.0f;
    float btnY = panelY + panelH - 50.0f;
    Rectangle cancelBtn{ panelX + panelW - btnW * 2 - 24.0f, btnY, btnW, btnH };
    Rectangle continueBtn{ panelX + panelW - btnW - 12.0f, btnY, btnW, btnH };

    if (DrawDialogButton(cancelBtn, "Cancel", false)) {
        DismissScriptWarning();
    }
    if (DrawDialogButton(continueBtn, "Continue", true)) {
        g_scriptWarningApproved = true;
        DismissScriptWarning();
    }
}

static const char* ExplorerMenuItemLabel(int i) {
    switch (g_explorerMenuMode) {
        case ExplorerMenuMode::Object: {
            bool singleSel = g_explorerMenuObject
                && g_selection.size() == 1
                && std::find(g_selection.begin(), g_selection.end(), g_explorerMenuObject) != g_selection.end();
            bool has = singleSel && !g_explorerMenuObject->script.empty();
            bool canGroup = g_selection.size() >= 2;
            bool inModel = g_explorerMenuObject && g_explorerMenuObject->parentModel;
            int cursor = 0;
            if (singleSel && i == cursor++) return has ? "Edit Flyscript" : "Add Flyscript";
            if (has && i == cursor++) return "Remove Flyscript";
            if (canGroup && i == cursor++) return "Group";
            if (inModel && i == cursor++) return "Ungroup";
            return nullptr;
        }
        case ExplorerMenuMode::Model: {
            if (i == 0) return "Ungroup";
            if (i == 1) return "Delete";
            return nullptr;
        }
        case ExplorerMenuMode::Empty:
            return i == 0 ? "Insert Flyscript" : nullptr;
        case ExplorerMenuMode::Script:
            return i == 0 ? "Rename" : (i == 1 ? "Delete" : nullptr);
    }
    return nullptr;
}

static void PerformExplorerMenuAction(int i) {
    if (!flyscript::IsRuntimeReady()) return;

    switch (g_explorerMenuMode) {
        case ExplorerMenuMode::Object: {
            if (!g_explorerMenuObject) return;
            bool singleSel = g_selection.size() == 1
                && std::find(g_selection.begin(), g_selection.end(), g_explorerMenuObject) != g_selection.end();
            bool has = singleSel && !g_explorerMenuObject->script.empty();
            bool canGroup = g_selection.size() >= 2;
            bool inModel = g_explorerMenuObject->parentModel != nullptr;
            int cursor = 0;
            if (singleSel && i == cursor++) {
                scriptEditor::OpenObject(g_explorerMenuObject);
                return;
            }
            if (has && i == cursor++) {
                flyscript::GetRuntime().StopScript(g_explorerMenuObject);
                scriptEditor::CloseIfTarget(g_explorerMenuObject);
                g_explorerMenuObject->script.clear();
                g_explorerMenuObject->runOnPlay = false;
                return;
            }
            if (canGroup && i == cursor++) {
                GroupSelectedObjects();
                return;
            }
            if (inModel && i == cursor++) {
                UngroupModelContaining(g_explorerMenuObject);
                return;
            }
            break;
        }
        case ExplorerMenuMode::Model: {
            if (!g_explorerMenuObject || !g_explorerMenuObject->parentModel) return;
            if (i == 0) {
                UngroupModelContaining(g_explorerMenuObject);
            } else if (i == 1) {
                ModelGroup* model = g_explorerMenuObject->parentModel;
                std::vector<ScatteredObject*> targets = model->members;
                UngroupModelContaining(g_explorerMenuObject);
                RequestDelete(targets);
                SetSelection({}, nullptr);
            }
            break;
        }
        case ExplorerMenuMode::Empty: {
            auto& scripts = flyscript::GetRuntime().scripts;
            scripts.push_back(flyscript::Script{});
            auto idx = static_cast<int>(scripts.size()) - 1;
            scripts[idx].name = "Script";
            scripts[idx].source = "-- Flyscript\n";
            scripts[idx].runOnPlay = true;
            scriptEditor::OpenScript(idx);
            break;
        }
        case ExplorerMenuMode::Script: {
            auto& scripts = flyscript::GetRuntime().scripts;
            if (g_explorerMenuScript < 0 || g_explorerMenuScript >= static_cast<int>(scripts.size())) return;
            if (i == 0) {
                g_scriptRenameIndex = g_explorerMenuScript;
                strncpy_s(g_scriptNameBuffer, scripts[g_explorerMenuScript].name.c_str(), sizeof(g_scriptNameBuffer) - 1);
                g_scriptNameBuffer[sizeof(g_scriptNameBuffer) - 1] = '\0';
                g_cursorPos = static_cast<int>(strlen(g_scriptNameBuffer));
                g_selStart = -1;
            } else {
                scriptEditor::CloseIfIndex(g_explorerMenuScript);
                flyscript::GetRuntime().RemoveScript(g_explorerMenuScript);
            }
            break;
        }
    }
}

static void OpenExplorerMenu(Vector2 pos, ExplorerMenuMode mode, ScatteredObject* obj, int scriptIndex) {
    CloseContextMenu();
    g_explorerMenuOpen = true;
    g_explorerMenuPos = pos;
    g_explorerMenuMode = mode;
    g_explorerMenuObject = obj;
    g_explorerMenuScript = scriptIndex;
    g_explorerMenuOpenTime = GetTime();
}

// ImGui popup version of the explorer's right-click menu. Runs in the ImGui
// frame (unlike the raylib 2D pass, which is always painted *under* every
// ImGui window), so it can never end up hidden behind the Explorer panel.
static void DrawImGuiExplorerMenu() {
    // g_explorerMenuOpenTime is refreshed on every OpenExplorerMenu() call,
    // including retargeting an already-open menu to a new row — use it (not
    // IsPopupOpen) to detect "new request", so right-clicking a different
    // row while the menu is open moves it there in one click instead of
    // needing a close-then-reopen.
    static double s_lastHandledOpenTime = -1.0;
    if (g_explorerMenuOpen && g_explorerMenuOpenTime != s_lastHandledOpenTime) {
        s_lastHandledOpenTime = g_explorerMenuOpenTime;
        ImGui::SetNextWindowPos(ImVec2(g_explorerMenuPos.x, g_explorerMenuPos.y));
        ImGui::OpenPopup("##ExplorerCtxMenu");
    }
    if (ImGui::BeginPopup("##ExplorerCtxMenu")) {
        int n = ExplorerMenuItemCount();
        for (int i = 0; i < n; ++i) {
            const char* label = ExplorerMenuItemLabel(i);
            if (!label) continue;
            if (ImGui::MenuItem(label)) {
                PerformExplorerMenuAction(i);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    // Mirror ImGui's own popup lifetime back into the shared flag so other
    // click handlers (which check !g_explorerMenuOpen) stay correctly gated
    // while the popup is visible, and un-gate once it closes for any reason
    // (item picked, Escape, click outside).
    g_explorerMenuOpen = ImGui::IsPopupOpen("##ExplorerCtxMenu");
}

static bool g_uiClickConsumed = false;

void MarkUIClickConsumed() { g_uiClickConsumed = true; }
bool WasUIClickConsumed() { return g_uiClickConsumed; }

MenuAction ProcessContextMenu() {
    g_uiClickConsumed = false;

    if (g_pendingFileAction != MenuAction::None) {
        MenuAction action = g_pendingFileAction;
        g_pendingFileAction = MenuAction::None;
        MarkUIClickConsumed();
        return action;
    }

    if (!g_menuOpen) return MenuAction::None;

    if (IsKeyPressed(KEY_ESCAPE)) {
        CloseContextMenu();
        return MenuAction::None;
    }

    Rectangle mainRec = {0, 0, 0, 0}, subRec = {0, 0, 0, 0};
    bool boundsValid = false;
    GetContextMenuBounds(mainRec, subRec);
    boundsValid = true;
    if (!boundsValid) return MenuAction::None;

    Vector2 mouse = GetMousePosition();
    bool inMain = CheckCollisionPointRec(mouse, mainRec);
    bool inSub = CheckCollisionPointRec(mouse, subRec);

    if (g_isObjectTarget) {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            MarkUIClickConsumed();
            CloseContextMenu();
            if (!inMain) return MenuAction::None;
            // Top row (y below menu top) = toggle transform space; bottom row = delete.
            const float topItemBottom = mainRec.y + 31.0f;
            if (mouse.y < topItemBottom) return MenuAction::ToggleTransformSpace;
            return MenuAction::DeleteObject;
        }
    }
    if (g_isWaterTarget) {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            MarkUIClickConsumed();
            CloseContextMenu();
            if (!inMain) return MenuAction::None;
            // Water body context menu: only delete option
            return MenuAction::DeleteWaterBody;
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && !inMain) {
            MarkUIClickConsumed();
            CloseContextMenu();
            return MenuAction::None;
        }
        return MenuAction::None;
    }

    if (inMain || inSub) g_showSubMenu = true;

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        if (!inMain && (!g_showSubMenu || !inSub)) {
            MarkUIClickConsumed();
            CloseContextMenu();
            return MenuAction::None;
        }
    }

    // Clicking the "Transform space" row (the second main-menu row) toggles it.
    if (g_showSubMenu && inMain && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (mouse.y >= mainRec.y + 32.0f) {
            MarkUIClickConsumed();
            CloseContextMenu();
            return MenuAction::ToggleTransformSpace;
        }
    }

    if (g_showSubMenu && inSub && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        auto itemIdx = static_cast<int>((mouse.y - subRec.y - 4.0f) / 30.0f);
        MarkUIClickConsumed();
        CloseContextMenu();
        if (itemIdx >= 0 && itemIdx < kSpawnMenuItemCount) return kSpawnMenuActions[itemIdx];
        return MenuAction::None;
    }

    return MenuAction::None;
}

static void DrawContextMenuInternal(Rectangle mainRec, Rectangle subRec, bool isObjectTarget, bool isWaterTarget, bool showSubMenu, double menuOpenTime) {
    auto anim = static_cast<float>((GetTime() - menuOpenTime) / 0.09);
    if (anim > 1.0f) anim = 1.0f;
    if (anim < 0.0f) anim = 0.0f;
    float dy = (1.0f - anim) * 6.0f;

    Vector2 mouse = GetMousePosition();
    bool hoveringMain = CheckCollisionPointRec(mouse, mainRec);

    Color shadowCol = Fade(theme::SHADOW, anim);
    Color bgCol = Fade(theme::BG_MENU, anim);
    Color borderDraw = Fade(theme::BORDER_STRONG, anim);

    if (isObjectTarget) {
        Rectangle shadowRec = { mainRec.x + 3.0f, mainRec.y + 3.0f + dy, mainRec.width, mainRec.height };
        DrawRectangleRec(shadowRec, shadowCol);
        Rectangle bodyRec = { mainRec.x, mainRec.y + dy, mainRec.width, mainRec.height };
        DrawRectangleRounded(bodyRec, 0.12f, 4, bgCol);
        DrawRectangleLinesEx(bodyRec, 1.0f, borderDraw);

        const char* spaceLabel = g_transformLocalSpace ? "Local" : "World";
        char spaceText[64];
        snprintf(spaceText, sizeof(spaceText), "Transform space: %s", spaceLabel);

        Rectangle itemTop = { bodyRec.x + 2.0f, bodyRec.y + 2.0f, bodyRec.width - 4.0f, 29.0f };
        Rectangle itemBot = { bodyRec.x + 2.0f, bodyRec.y + 2.0f + 31.0f, bodyRec.width - 4.0f, 29.0f };
        bool hoverTop = CheckCollisionPointRec(mouse, itemTop);
        bool hoverBot = CheckCollisionPointRec(mouse, itemBot);

        float tt = HoverProgress(10, hoverTop);
        if (tt > 0.0f) {
            DrawRectangleRounded(itemTop, 0.12f, 4, Fade(theme::ACCENT, tt * anim * 0.5f));
        }
        Color topCol = hoverTop ? Fade(theme::TEXT, anim) : Fade(theme::TEXT_MUTED, anim);
        DrawTextArial(spaceText, itemTop.x + 10.0f, itemTop.y + 7.0f, 14.0f, topCol);
        if (hoverTop) MarkHand();

        float dt = HoverProgress(11, hoverBot);
        if (dt > 0.0f) {
            DrawRectangleRounded(itemBot, 0.12f, 4, Fade(theme::DANGER, dt * anim * 0.5f));
        }
        Color botCol = hoverBot ? Fade(theme::TEXT, anim) : Fade(theme::DANGER, anim);
        DrawTextArial("Delete", itemBot.x + 10.0f, itemBot.y + 7.0f, 14.0f, botCol);
        if (hoverBot) MarkHand();

        DrawLine(static_cast<int>(bodyRec.x + 8.0f), static_cast<int>(itemTop.y + itemTop.height),
                 static_cast<int>(bodyRec.x + bodyRec.width - 8.0f), static_cast<int>(itemTop.y + itemTop.height),
                 Fade(theme::BORDER, anim));
        return;
    }

    if (g_isWaterTarget) {
        Rectangle shadowRec = { mainRec.x + 3.0f, mainRec.y + 3.0f + dy, mainRec.width, mainRec.height };
        DrawRectangleRec(shadowRec, shadowCol);
        Rectangle bodyRec = { mainRec.x, mainRec.y + dy, mainRec.width, mainRec.height };
        DrawRectangleRounded(bodyRec, 0.12f, 4, bgCol);
        DrawRectangleLinesEx(bodyRec, 1.0f, borderDraw);

        Rectangle itemBot = { bodyRec.x + 2.0f, bodyRec.y + 2.0f, bodyRec.width - 4.0f, 29.0f };
        bool hoverBot = CheckCollisionPointRec(mouse, itemBot);

        float dt = HoverProgress(12, hoverBot);
        if (dt > 0.0f) {
            DrawRectangleRounded(itemBot, 0.12f, 4, Fade(theme::DANGER, dt * anim * 0.5f));
        }
        Color botCol = hoverBot ? Fade(theme::TEXT, anim) : Fade(theme::DANGER, anim);
        DrawTextArial("Delete", itemBot.x + 10.0f, itemBot.y + 7.0f, 14.0f, botCol);
        if (hoverBot) MarkHand();

        return;
    }

    Rectangle mainShadow = { mainRec.x + 3.0f, mainRec.y + 3.0f + dy, mainRec.width, mainRec.height };
    DrawRectangleRec(mainShadow, shadowCol);
    Rectangle mainBody = { mainRec.x, mainRec.y + dy, mainRec.width, mainRec.height };
    DrawRectangleRounded(mainBody, 0.12f, 4, bgCol);
    DrawRectangleLinesEx(mainBody, 1.0f, borderDraw);

    Rectangle rowAdd = { mainBody.x + 2.0f, mainBody.y + 2.0f, mainBody.width - 4.0f, 29.0f };
    Rectangle rowSpace = { mainBody.x + 2.0f, mainBody.y + 2.0f + 31.0f, mainBody.width - 4.0f, 29.0f };
    bool hoverAdd = CheckCollisionPointRec(mouse, rowAdd);
    bool hoverSpace = CheckCollisionPointRec(mouse, rowSpace);

    if (hoverAdd || (showSubMenu && hoveringMain && !hoverSpace)) {
        Rectangle mainInner = rowAdd;
        DrawRectangleRounded(mainInner, 0.12f, 4, Fade(theme::BG_ROW_HOVER, 0.9f * anim));
    }
    float st = HoverProgress(21U, hoverSpace);
    if (st > 0.0f) {
        DrawRectangleRounded(rowSpace, 0.12f, 4, Fade(Mix(theme::BG_WIDGET, theme::ACCENT, st), st * anim));
    }
    if (hoverAdd) MarkHand();

    DrawTextArial("Add object", rowAdd.x + 8.0f, rowAdd.y + 7.0f, 14.0f, Fade(theme::TEXT, anim));
    DrawTextArial("▶", rowAdd.x + rowAdd.width - 20.0f, rowAdd.y + 7.0f, 12.0f,
        showSubMenu ? Fade(theme::TEXT, anim) : Fade(theme::TEXT_MUTED, anim));

    const char* spaceLabel = g_transformLocalSpace ? "Local" : "World";
    char spaceText[64];
    snprintf(spaceText, sizeof(spaceText), "Transform space: %s", spaceLabel);
    Color spaceCol = hoverSpace ? Fade(theme::TEXT, anim) : (g_transformLocalSpace ? Fade(theme::ACCENT_HOVER, anim) : Fade(theme::TEXT_MUTED, anim));
    DrawTextArial(spaceText, rowSpace.x + 8.0f, rowSpace.y + 7.0f, 14.0f, spaceCol);
    if (hoverSpace) MarkHand();

    DrawLine(static_cast<int>(rowAdd.x + 6.0f), static_cast<int>(rowAdd.y + rowAdd.height),
             static_cast<int>(rowAdd.x + rowAdd.width - 6.0f), static_cast<int>(rowAdd.y + rowAdd.height),
             Fade(theme::BORDER, anim));

    if (showSubMenu) {
        Rectangle subShadow = { subRec.x + 3.0f, subRec.y + 3.0f + dy, subRec.width, subRec.height };
        DrawRectangleRec(subShadow, shadowCol);
        Rectangle subBody = { subRec.x, subRec.y + dy, subRec.width, subRec.height };
        DrawRectangleRounded(subBody, 0.12f, 4, bgCol);
        DrawRectangleLinesEx(subBody, 1.0f, borderDraw);

        constexpr const char* const* items = kSpawnMenuItems;
        constexpr int itemCount = kSpawnMenuItemCount;

        for (int i = 0; i < itemCount; ++i) {
            Rectangle itemRec = { subBody.x + 4.0f, subBody.y + 4.0f + (static_cast<float>(i) * 30.0f), subBody.width - 8.0f, 26.0f };
            bool itemHovered = CheckCollisionPointRec(mouse, itemRec);

            float t = HoverProgress(static_cast<uint64_t>(i) * 2U + 200U, itemHovered);
            if (t > 0.0f) {
                DrawRectangleRounded(itemRec, 0.15f, 4, Fade(Mix(theme::ACCENT, theme::ACCENT_HOVER, t), t * anim));
                DrawTextArial(items[i], itemRec.x + 10.0f, itemRec.y + 4.0f, 14.0f, Fade(theme::TEXT, anim));
            } else {
                DrawTextArial(items[i], itemRec.x + 10.0f, itemRec.y + 4.0f, 14.0f, Fade(theme::TEXT_MUTED, anim));
            }
            if (itemHovered) MarkHand();
        }
    }
}

static void DrawContextMenu() {
    if (!g_menuOpen) return;

    Rectangle mainRec = {0, 0, 0, 0}, subRec = {0, 0, 0, 0};
    GetContextMenuBounds(mainRec, subRec);
    DrawContextMenuInternal(mainRec, subRec, g_isObjectTarget, g_isWaterTarget, g_showSubMenu, g_menuOpenTime);
}

// ---------------------------------------------------------------------------
// 1. Top Bar
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 1. Top Bar (Dear ImGui) — File menu, Import, tool buttons, Play/Stop.
// Called once per frame from DrawImGuiFrame(), between ImGui::NewFrame()
// (UpdateInput()) and ImGui::Render().
// ---------------------------------------------------------------------------
static ImTextureID ToolTex(int i) {
    return g_toolIcons[i].id != 0
        ? static_cast<ImTextureID>(g_toolIcons[i].id)
        : static_cast<ImTextureID>(0);
}

static ImTextureID RowTex(RowIconType i) {
    return g_rowIcons[i].id != 0
        ? static_cast<ImTextureID>(g_rowIcons[i].id)
        : static_cast<ImTextureID>(0);
}

static void DrawImGuiTopBar() {
    Rectangle barRec = GetTopBarBounds();

    ImGui::SetNextWindowPos(ImVec2(barRec.x, barRec.y));
    ImGui::SetNextWindowSize(ImVec2(barRec.width, barRec.height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(25 / 255.0f, 27 / 255.0f, 32 / 255.0f, 1.0f)); // theme::BG_TOP_BAR
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
    ImGui::Begin("##TopBar", nullptr, flags);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::BeginDisabled(g_playActive);

    // File button (toggles a popup below it)
    if (ImGui::Button("File", ImVec2(56.0f, 30.0f))) {
        ImGui::OpenPopup("##FileMenu");
    }
    ImGui::SameLine();

    // Import button
    if (ToolTex(4)) {
        ImGui::Image(ToolTex(4), ImVec2(16.0f, 16.0f));
        ImGui::SameLine(0.0f, 6.0f);
    }
    if (ImGui::Button("Import", ImVec2(0.0f, 30.0f))) {
        g_importRequested = true;
    }

    ImGui::EndDisabled();

    // File menu popup, anchored under the File button
    ImGui::SetNextWindowPos(ImVec2(barRec.x + 4.0f, barRec.y + 38.0f));
    if (ImGui::BeginPopup("##FileMenu")) {
        if (ImGui::MenuItem("Save")) { g_pendingFileAction = MenuAction::Save; ImGui::CloseCurrentPopup(); }
        if (ImGui::MenuItem("Save as")) { g_pendingFileAction = MenuAction::SaveAs; ImGui::CloseCurrentPopup(); }
        if (ImGui::MenuItem("Open scene")) { g_pendingFileAction = MenuAction::OpenScene; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // Transform tool buttons (row 2)
    ImGui::SetCursorPos(ImVec2(75.0f, 45.0f));
    constexpr const char* toolNames[] = { "Select", "Move", "Scale", "Rotate" };
    constexpr TransformTool toolEnums[] = { TransformTool::Select, TransformTool::Move, TransformTool::Scale, TransformTool::Rotate };
    for (int i = 0; i < 4; ++i) {
        bool selected = (g_activeTool == toolEnums[i]);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0 / 255.0f, 190 / 255.0f, 200 / 255.0f, 0.7f)); // theme::ACCENT
        }
        if (ToolTex(i)) {
            ImGui::Image(ToolTex(i), ImVec2(16.0f, 16.0f));
            ImGui::SameLine(0.0f, 6.0f);
        }
        if (ImGui::Button(toolNames[i], ImVec2(0.0f, 30.0f))) {
            g_activeTool = toolEnums[i];
        }
        if (selected) ImGui::PopStyleColor();
        if (i < 3) ImGui::SameLine(0.0f, 12.0f);
    }

    // Play/Stop button, right-aligned
    {
        const char* playLabel = g_playActive ? "Stop" : "Play";
        ImVec4 playCol = g_playActive
            ? ImVec4(200 / 255.0f, 40 / 255.0f, 40 / 255.0f, 1.0f)   // theme::DANGER (approx)
            : ImVec4(0 / 255.0f, 170 / 255.0f, 90 / 255.0f, 1.0f);   // theme::SUCCESS (approx)
        ImGui::PushStyleColor(ImGuiCol_Button, playCol);
        ImGui::SetCursorPos(ImVec2(barRec.width - 85.0f, 15.0f));
        if (ImGui::Button(playLabel, ImVec2(70.0f, 30.0f))) {
            g_playTogglePending = true;
        }
        ImGui::PopStyleColor();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// 2. Left Panel (Explorer Window)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 2. Left Panel (Explorer Window, Dear ImGui)
// Reuses the existing rename/selection/drag-drop state (g_renameObject,
// g_explorerDrag*, OpenExplorerMenu, PerformExplorerDrop, ...) — only the
// input/rendering is now driven by ImGui widgets instead of raylib hit tests.
// Called once per frame from DrawImGuiFrame().
// ---------------------------------------------------------------------------
static bool ExplorerRenameActive() {
    return g_renameObject || g_renameModel || g_renameTerrain || g_renameLegacyTerrain || g_scriptRenameIndex >= 0;
}

// Draws one row as an ImGui Selectable and returns true if it was clicked
// (left button, on this frame).
static bool ExplorerRow(const char* label, bool selected, bool* outRightClicked) {
    ImGuiSelectableFlags flags = ImGuiSelectableFlags_AllowDoubleClick;
    bool clicked = ImGui::Selectable(label, selected, flags, ImVec2(0.0f, 20.0f));
    if (outRightClicked) {
        *outRightClicked = ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    }
    return clicked;
}

// Same as ExplorerRow, but draws a fixed-size icon (from assets/EditorIcons/)
// before the text instead of a leading unicode glyph.
static bool ExplorerRowIcon(RowIconType icon, const char* label, bool selected, bool* outRightClicked) {
    ImVec2 rowStart = ImGui::GetCursorPos();
    ImGuiSelectableFlags flags = ImGuiSelectableFlags_AllowDoubleClick;
    bool clicked = ImGui::Selectable("##row", selected, flags, ImVec2(0.0f, 20.0f));
    if (outRightClicked) {
        *outRightClicked = ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    }
    ImGui::SetCursorPos(ImVec2(rowStart.x + 4.0f, rowStart.y + 2.0f));
    ImTextureID tex = RowTex(icon);
    if (tex) {
        ImGui::Image(tex, ImVec2(14.0f, 14.0f));
        ImGui::SameLine(0.0f, 6.0f);
    } else {
        ImGui::Dummy(ImVec2(14.0f, 14.0f));
        ImGui::SameLine(0.0f, 6.0f);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    return clicked;
}

// Small helper: draw+commit/cancel an inline rename InputText, focusing it
// the first frame it appears. Returns true once Enter/blur commits the value.
static bool ExplorerRenameField(const char* imguiId, char* buf, size_t bufSize, bool justOpened) {
    ImGui::SetNextItemWidth(-1.0f);
    if (justOpened) ImGui::SetKeyboardFocusHere();
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
    bool committed = ImGui::InputText(imguiId, buf, bufSize, flags);
    bool cancel = ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    bool blurred = ImGui::IsItemDeactivatedAfterEdit() && !committed;
    if (committed || blurred) return true;
    if (cancel) { *buf = '\0'; return false; } // caller checks g_render* pointers, not the buffer, to detect cancel
    return false;
}

static void DrawImGuiExplorer() {
    Rectangle panelRec = GetExplorerPanelBounds();

    ImGui::SetNextWindowPos(ImVec2(panelRec.x, panelRec.y));
    ImGui::SetNextWindowSize(ImVec2(panelRec.width, panelRec.height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##Explorer", nullptr, flags);

    ImGui::TextColored(ImVec4(0 / 255.0f, 190 / 255.0f, 200 / 255.0f, 1.0f), "EXPLORER");
    ImGui::Separator();

    bool rightClickHandled = false;
    bool rowClickedThisFrame = false;
    ImVec2 mouseImgui = ImGui::GetMousePos();
    Vector2 mouse = { mouseImgui.x, mouseImgui.y };

    ImGui::BeginChild("##ExplorerContent", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextDisabled("SCENE");

    if (g_sceneObjects != nullptr) {
        // F2 to rename the current selection, matching the old raylib shortcut.
        if (!ExplorerRenameActive() && IsKeyPressed(KEY_F2)) {
            if (g_selectedTerrain) {
                g_renameTerrain = g_selectedTerrain;
                strncpy_s(g_nameBuffer, g_selectedTerrain->GetName().c_str(), sizeof(g_nameBuffer) - 1);
            } else if (terrain::GetTerrainEditorState().selectedTerrainLegacy) {
                terrain::Terrain* legacy = terrain::GetTerrainEditorState().selectedTerrainLegacy;
                g_renameLegacyTerrain = legacy;
                strncpy_s(g_nameBuffer, legacy->GetName().c_str(), sizeof(g_nameBuffer) - 1);
            } else if (g_primarySelection) {
                ModelGroup* pm = g_primarySelection->parentModel;
                bool modelAllSelected = pm && !pm->members.empty();
                if (pm) {
                    for (auto* m : pm->members) {
                        if (!m || !m->isSelected) { modelAllSelected = false; break; }
                    }
                }
                if (modelAllSelected) {
                    g_renameModel = pm;
                    strncpy_s(g_nameBuffer, pm->name.c_str(), sizeof(g_nameBuffer) - 1);
                } else {
                    g_renameObject = g_primarySelection;
                    strncpy_s(g_nameBuffer, g_primarySelection->GetName().c_str(), sizeof(g_nameBuffer) - 1);
                }
            }
            g_nameBuffer[sizeof(g_nameBuffer) - 1] = '\0';
        }

        std::vector<ExplorerEntry> entries = BuildExplorerEntries();

        for (const auto& entry : entries) {
            const void* stableId = entry.isHeader ? static_cast<const void*>(entry.model)
                : entry.water ? static_cast<const void*>(entry.water)
                : entry.basicTerrain ? static_cast<const void*>(entry.basicTerrain)
                : entry.legacyTerrain ? static_cast<const void*>(entry.legacyTerrain)
                : static_cast<const void*>(entry.obj);
            ImGui::PushID(stableId);

            // --- Model header row ---
            if (entry.isHeader) {
                ModelGroup* model = entry.model;
                if (!model) { ImGui::PopID(); continue; }

                bool modelSelected = !model->members.empty();
                for (auto* m : model->members) {
                    if (!m || !m->isSelected) { modelSelected = false; break; }
                }

                if (model == g_renameModel) {
                    static ModelGroup* s_lastRenameModel = nullptr;
                    bool justOpened = (s_lastRenameModel != model);
                    s_lastRenameModel = model;
                    if (ExplorerRenameField("##rename", g_nameBuffer, sizeof(g_nameBuffer), justOpened)) {
                        model->name = g_nameBuffer;
                        g_renameModel = nullptr;
                    } else if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()) {
                        g_renameModel = nullptr; // ESC / blur-without-edit cancels
                    }
                } else {
                    char label[128];
                    snprintf(label, sizeof(label), "%s (%d)", model->name.c_str(), static_cast<int>(model->members.size()));
                    bool rightClicked = false;
                    bool clicked = ExplorerRowIcon(ROWICON_OBJECT, label, modelSelected, &rightClicked);

                    if (g_explorerDragActive && ImGui::IsItemHovered()) {
                        g_explorerDropTarget = model;
                        g_explorerDropValid = true;
                    }

                    if (clicked && !ExplorerRenameActive() && !g_explorerMenuOpen) {
                        bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
                        bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                        if (ctrl) {
                            std::vector<ScatteredObject*> next = g_selection;
                            if (modelSelected) {
                                next.erase(std::remove_if(next.begin(), next.end(),
                                    [&](ScatteredObject* o) {
                                        return std::find(model->members.begin(), model->members.end(), o) != model->members.end();
                                    }), next.end());
                            } else {
                                for (auto* m : model->members) {
                                    if (m && std::find(next.begin(), next.end(), m) == next.end()) next.push_back(m);
                                }
                            }
                            g_explorerAnchorIndex = entry.objRowIndex;
                            SetSelection(next, KeepPrimary(next, g_primarySelection));
                        } else if (shift && g_explorerAnchorIndex >= 0) {
                            int a = g_explorerAnchorIndex, b = entry.objRowIndex;
                            if (a < 0) a = b;
                            if (a > b) std::swap(a, b);
                            std::vector<ScatteredObject*> sel;
                            for (const auto& e : entries) {
                                if (!e.isHeader && e.obj && e.objRowIndex >= a && e.objRowIndex <= b) sel.push_back(e.obj);
                            }
                            ScatteredObject* primary = model->members.empty() ? nullptr : model->members.front();
                            SetSelection(sel, KeepPrimary(sel, primary));
                        } else {
                            SetSelection(model->members, model->members.empty() ? nullptr : model->members.front());
                            g_explorerAnchorIndex = entry.objRowIndex;
                            double now = GetTime();
                            if (g_lastExplorerModelClick == model && now - g_lastExplorerClickTime < 0.35) {
                                g_renameModel = model;
                                strncpy_s(g_nameBuffer, model->name.c_str(), sizeof(g_nameBuffer) - 1);
                                g_nameBuffer[sizeof(g_nameBuffer) - 1] = '\0';
                                g_lastExplorerModelClick = nullptr;
                            } else {
                                g_lastExplorerModelClick = model;
                                g_lastExplorerClickTime = now;
                            }
                        }
                        rowClickedThisFrame = true;
                    } else if (rightClicked && !ExplorerRenameActive()) {
                        if (!modelSelected) {
                            SetSelection(model->members, model->members.empty() ? nullptr : model->members.front());
                        }
                        rightClickHandled = true;
                        if (!model->members.empty()) OpenExplorerMenu(mouse, ExplorerMenuMode::Model, model->members.front(), -1);
                        rowClickedThisFrame = true;
                    }

                    if (ImGui::IsItemActivated() && !ExplorerRenameActive() && !g_explorerMenuOpen && !model->members.empty()) {
                        g_explorerDragPending = true;
                        g_explorerDragObjects = model->members;
                        g_explorerDragStart = mouse;
                    }
                }
                ImGui::PopID();
                continue;
            }

            // --- Water body row ---
            if (entry.water) {
                WaterBody* water = entry.water;
                if (!water) { ImGui::PopID(); continue; }
                char label[128];
                snprintf(label, sizeof(label), "%s", water->GetName().c_str());
                bool rightClicked = false;
                bool clicked = ExplorerRowIcon(ROWICON_WATER, label, water->isSelected, &rightClicked);
                if (clicked && !ExplorerRenameActive() && !g_explorerMenuOpen) {
                    g_explorerAnchorIndex = entry.objRowIndex;
                    SetSelectedWater(g_selectedWater == water ? nullptr : water);
                    rowClickedThisFrame = true;
                } else if (rightClicked && !ExplorerRenameActive()) {
                    SetSelectedWater(water);
                    rightClickHandled = true;
                    rowClickedThisFrame = true;
                }
                ImGui::PopID();
                continue;
            }

            // --- BasicTerrain row ---
            if (entry.basicTerrain) {
                BasicTerrain* terr = entry.basicTerrain;
                if (!terr) { ImGui::PopID(); continue; }
                bool isRenaming = (g_renameTerrain == terr);
                bool isSel = terr->isSelected || terr == BasicTerrain::GetActive();

                if (isRenaming) {
                    static BasicTerrain* s_lastRenameTerrain = nullptr;
                    bool justOpened = (s_lastRenameTerrain != terr);
                    s_lastRenameTerrain = terr;
                    if (ExplorerRenameField("##rename", g_nameBuffer, sizeof(g_nameBuffer), justOpened)) {
                        terr->SetName(g_nameBuffer);
                        g_renameTerrain = nullptr;
                    } else if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()) {
                        g_renameTerrain = nullptr;
                    }
                } else {
                    char label[128];
                    snprintf(label, sizeof(label), "%s", terr->GetName().c_str());
                    bool rightClicked = false;
                    bool clicked = ExplorerRowIcon(ROWICON_TERRAIN, label, isSel, &rightClicked);
                    if (clicked && !g_explorerMenuOpen) {
                        SetSelectedTerrain(g_selectedTerrain == terr ? nullptr : terr);
                        rowClickedThisFrame = true;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            g_renameTerrain = terr;
                            strncpy_s(g_nameBuffer, terr->GetName().c_str(), sizeof(g_nameBuffer) - 1);
                            g_nameBuffer[sizeof(g_nameBuffer) - 1] = '\0';
                        }
                    } else if (rightClicked) {
                        SetSelectedTerrain(terr);
                        rightClickHandled = true;
                        rowClickedThisFrame = true;
                    }
                }
                ImGui::PopID();
                continue;
            }

            // --- Legacy chunked terrain::Terrain row ---
            if (entry.legacyTerrain) {
                terrain::Terrain* legacy = entry.legacyTerrain;
                if (!legacy) { ImGui::PopID(); continue; }
                bool isSel = terrain::GetTerrainEditorState().selectedTerrainLegacy == legacy;
                bool isRenaming = (g_renameLegacyTerrain == legacy);

                if (isRenaming) {
                    static terrain::Terrain* s_lastRenameLegacy = nullptr;
                    bool justOpened = (s_lastRenameLegacy != legacy);
                    s_lastRenameLegacy = legacy;
                    if (ExplorerRenameField("##rename", g_nameBuffer, sizeof(g_nameBuffer), justOpened)) {
                        std::string newName = g_nameBuffer;
                        if (newName.empty()) newName = "Terrain";
                        bool taken = false;
                        for (auto* t : terrain::GetTerrainRegistry().GetTerrains()) {
                            if (t && t != legacy && t->GetName() == newName) { taken = true; break; }
                        }
                        if (taken) LogAlways("[terrain] name '%s' is already in use - rename blocked", newName.c_str());
                        else legacy->SetName(newName);
                        g_renameLegacyTerrain = nullptr;
                    } else if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()) {
                        g_renameLegacyTerrain = nullptr;
                    }
                } else {
                    char label[128];
                    snprintf(label, sizeof(label), "%s", legacy->GetName().c_str());
                    bool rightClicked = false;
                    bool clicked = ExplorerRowIcon(ROWICON_TERRAIN, label, isSel, &rightClicked);
                    if (clicked && !g_explorerMenuOpen) {
                        terrain::HandleTerrainSelection(legacy, !isSel);
                        rowClickedThisFrame = true;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            g_renameLegacyTerrain = legacy;
                            strncpy_s(g_nameBuffer, legacy->GetName().c_str(), sizeof(g_nameBuffer) - 1);
                            g_nameBuffer[sizeof(g_nameBuffer) - 1] = '\0';
                        }
                    } else if (rightClicked) {
                        terrain::HandleTerrainSelection(legacy, true);
                        rightClickHandled = true;
                        rowClickedThisFrame = true;
                    }
                }
                ImGui::PopID();
                continue;
            }

            // --- Object / model member row ---
            ScatteredObject* obj = entry.obj;
            if (!obj) { ImGui::PopID(); continue; }
            bool isMember = obj->parentModel != nullptr;

            if (g_renameObject == obj) {
                if (isMember) ImGui::Indent(14.0f);
                static ScatteredObject* s_lastRenameObj = nullptr;
                bool justOpened = (s_lastRenameObj != obj);
                s_lastRenameObj = obj;
                if (ExplorerRenameField("##rename", g_nameBuffer, sizeof(g_nameBuffer), justOpened)) {
                    obj->SetName(g_nameBuffer);
                    g_renameObject = nullptr;
                } else if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()) {
                    g_renameObject = nullptr;
                }
                if (isMember) ImGui::Unindent(14.0f);
            } else {
                if (isMember) ImGui::Indent(14.0f);
                char label[128];
                snprintf(label, sizeof(label), "%s", obj->GetName().c_str());
                bool rightClicked = false;
                bool clicked = ExplorerRowIcon(ROWICON_OBJECT, label, obj->isSelected, &rightClicked);

                if (g_explorerDragActive && ImGui::IsItemHovered()) {
                    g_explorerDropTarget = obj->parentModel;
                    g_explorerDropValid = true;
                }

                std::vector<ScatteredObject*> pressSelection;
                if (clicked && !ExplorerRenameActive() && !g_explorerMenuOpen) {
                    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
                    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                    if (shift && g_explorerAnchorIndex >= 0) {
                        int a = g_explorerAnchorIndex, b = entry.objRowIndex;
                        if (a > b) std::swap(a, b);
                        std::vector<ScatteredObject*> sel;
                        for (const auto& e : entries) {
                            if (!e.isHeader && e.obj && e.objRowIndex >= a && e.objRowIndex <= b) sel.push_back(e.obj);
                        }
                        SetSelection(sel, obj);
                        pressSelection = std::move(sel);
                    } else if (ctrl) {
                        bool wasSelected = obj->isSelected;
                        std::vector<ScatteredObject*> next;
                        for (auto* o : g_selection) if (o != obj) next.push_back(o);
                        if (!wasSelected) next.push_back(obj);
                        g_explorerAnchorIndex = entry.objRowIndex;
                        SetSelection(next, KeepPrimary(next, g_primarySelection));
                        pressSelection = std::move(next);
                    } else {
                        if (obj->isSelected && g_selection.size() > 1) {
                            pressSelection = g_selection;
                            SetSelection(pressSelection, g_primarySelection);
                        } else {
                            pressSelection = { obj };
                            SetSelection({ obj }, obj);
                        }
                        g_explorerAnchorIndex = entry.objRowIndex;

                        double now = GetTime();
                        if (g_lastExplorerClick == obj && now - g_lastExplorerClickTime < 0.35) {
                            g_renameObject = obj;
                            strncpy_s(g_nameBuffer, obj->GetName().c_str(), sizeof(g_nameBuffer) - 1);
                            g_nameBuffer[sizeof(g_nameBuffer) - 1] = '\0';
                            g_lastExplorerClick = nullptr;
                        } else {
                            g_lastExplorerClick = obj;
                            g_lastExplorerClickTime = now;
                        }
                    }
                    rowClickedThisFrame = true;
                } else if (rightClicked && !ExplorerRenameActive()) {
                    if (!obj->isSelected) {
                        SetSelection({ obj }, obj);
                        g_explorerAnchorIndex = entry.objRowIndex;
                    }
                    rightClickHandled = true;
                    OpenExplorerMenu(mouse, ExplorerMenuMode::Object, obj, -1);
                    rowClickedThisFrame = true;
                }

                if (!pressSelection.empty()) {
                    g_explorerDragPending = true;
                    g_explorerDragObjects = std::move(pressSelection);
                    g_explorerDragStart = mouse;
                }
                if (isMember) ImGui::Unindent(14.0f);
            }
            ImGui::PopID();
        }

        // Clicking blank space in the SCENE section clears the selection.
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !ExplorerRenameActive() && !g_explorerMenuOpen && !rowClickedThisFrame && !ImGui::IsAnyItemHovered()) {
            SetSelection({}, nullptr);
            g_explorerAnchorIndex = -1;
            g_lastExplorerClick = nullptr;
        }

        // Resolve drop target: hovering blank SCENE space (not over a row)
        // means "back to the workspace root".
        if (g_explorerDragActive && !g_explorerDropValid) {
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
                g_explorerDropTarget = nullptr;
                g_explorerDropValid = true;
            }
        }

        // Drag state machine: press-and-move past a threshold starts the
        // drag; release over a valid target reparents the dragged objects.
        if (g_explorerDragPending || g_explorerDragActive) {
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                if (g_explorerDragActive && g_explorerDropValid) {
                    PerformExplorerDrop(g_explorerDragObjects, g_explorerDropTarget);
                }
                g_explorerDragPending = false;
                g_explorerDragActive = false;
                g_explorerDragObjects.clear();
                g_explorerDropTarget = nullptr;
                g_explorerDropValid = false;
            } else if (!g_explorerDragActive && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                if (ExplorerRenameActive() || g_explorerMenuOpen) {
                    g_explorerDragPending = false;
                    g_explorerDragObjects.clear();
                } else {
                    Vector2 delta = Vector2Subtract(mouse, g_explorerDragStart);
                    if (fabsf(delta.x) + fabsf(delta.y) > 6.0f) {
                        g_explorerDragActive = true;
                        g_explorerDragPending = false;
                        g_explorerDropValid = false;
                    }
                }
            }
        }
    }

    if (flyscript::IsRuntimeReady()) {
        auto& scripts = flyscript::GetRuntime().scripts;
        ImGui::Spacing();
        ImGui::TextDisabled("FLYSCRIPTS");

        for (int i = 0; i < static_cast<int>(scripts.size()); ++i) {
            ImGui::PushID(i);
            if (g_scriptRenameIndex == i) {
                static int s_lastRenameScriptIdx = -1;
                bool justOpened = (s_lastRenameScriptIdx != i);
                s_lastRenameScriptIdx = i;
                if (ExplorerRenameField("##scriptrename", g_scriptNameBuffer, sizeof(g_scriptNameBuffer), justOpened)) {
                    scripts[i].name = g_scriptNameBuffer;
                    g_scriptRenameIndex = -1;
                } else if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()) {
                    g_scriptRenameIndex = -1;
                }
            } else {
                char label[128];
                snprintf(label, sizeof(label), "%s", scripts[i].name.c_str());
                bool rightClicked = false;
                bool clicked = ExplorerRowIcon(ROWICON_SCRIPT, label, false, &rightClicked);
                if (clicked && !g_explorerMenuOpen) {
                    scriptEditor::OpenScript(i);
                } else if (rightClicked) {
                    rightClickHandled = true;
                    OpenExplorerMenu(mouse, ExplorerMenuMode::Script, nullptr, i);
                }
            }
            ImGui::PopID();
        }
    }

    bool contentHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    ImGui::EndChild();

    if (g_sceneObjects && contentHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !rightClickHandled) {
        OpenExplorerMenu(mouse, ExplorerMenuMode::Empty, nullptr, -1);
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// 3. Number Input Helper
// ---------------------------------------------------------------------------
static int GetCursorIndexFromMouse(const char* text, float clickX, float textStartX, float fontSize) {
    auto len = static_cast<int>(strlen(text));
    float bestDist = 1e9f;
    int bestIndex = len;

    for (int i = 0; i <= len; ++i) {
        char sub[32] = { 0 };
        strncpy_s(sub, text, static_cast<size_t>(i));
        sub[i] = '\0';

        float charX = textStartX + MeasureTextArial(sub, fontSize);
        float dist = fabsf(clickX - charX);

        if (dist < bestDist) {
            bestDist = dist;
            bestIndex = i;
        }
    }
    return bestIndex;
}

static int TextLenCap(int idx, int len) {
    return idx < 0 ? 0 : (idx > len ? len : idx);
}

static void TextEditDeleteSel(char* buf, int& cursor, int& sel) {
    if (sel < 0 || sel == cursor) return;
    int lo = sel < cursor ? sel : cursor;
    int hi = sel > cursor ? sel : cursor;
    auto len = static_cast<int>(strlen(buf));
    int moveLen = len - hi + 1;
    memmove(buf + lo, buf + hi, static_cast<size_t>(moveLen));
    cursor = lo;
    sel = -1;
}

static void TextEditCopy(char* buf, int& cursor, int& sel, bool cut) {
    if (sel < 0 || sel == cursor) return;
    int lo = sel < cursor ? sel : cursor;
    int hi = sel > cursor ? sel : cursor;
    int n = hi - lo;
    if (n <= 0) return;
    char tmp[160];
    if (n >= static_cast<int>(sizeof(tmp))) n = static_cast<int>(sizeof(tmp)) - 1;
    memcpy(tmp, buf + lo, static_cast<size_t>(n));
    tmp[n] = '\0';
    SetClipboardText(tmp);
    if (cut) TextEditDeleteSel(buf, cursor, sel);
}

static int TextEditWordLeft(const char* buf, int pos) {
    if (pos <= 0) return 0;
    int p = pos;
    while (p > 0 && buf[p - 1] == ' ') --p;
    while (p > 0 && buf[p - 1] != ' ') --p;
    return p;
}

static int TextEditWordRight(const char* buf, int pos) {
    auto len = static_cast<int>(strlen(buf));
    if (pos >= len) return len;
    int p = pos;
    while (p < len && buf[p] != ' ') ++p;
    while (p < len && buf[p] == ' ') ++p;
    return p;
}

static bool TextEditKeys(char* buf, int bufSize, int& cursor, int& sel, bool& commit) {
    commit = false;
    bool changed = false;
    auto len = static_cast<int>(strlen(buf));
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
        commit = true;
        sel = -1;
        return changed;
    }

    if (ctrl && IsKeyPressed(KEY_A)) {
        cursor = len;
        sel = 0;
        return changed;
    }
    if (ctrl && IsKeyPressed(KEY_C)) {
        TextEditCopy(buf, cursor, sel, false);
        return changed;
    }
    if (ctrl && IsKeyPressed(KEY_X)) {
        TextEditCopy(buf, cursor, sel, true);
        changed = true;
        return changed;
    }
    if (ctrl && IsKeyPressed(KEY_V)) {
        const char* clip = GetClipboardText();
        if (clip && clip[0] != '\0') {
            if (sel >= 0 && sel != cursor) TextEditDeleteSel(buf, cursor, sel);
            len = static_cast<int>(strlen(buf));
            auto clipLen = static_cast<int>(strlen(clip));
            int room = bufSize - 1 - len;
            if (room > 0) {
                if (clipLen > room) clipLen = room;
                int moveLen = len - cursor + 1;
                memmove(buf + cursor + clipLen, buf + cursor, static_cast<size_t>(moveLen));
                memcpy(buf + cursor, clip, static_cast<size_t>(clipLen));
                cursor += clipLen;
                sel = -1;
                changed = true;
            }
        }
        return changed;
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (sel >= 0 && sel != cursor) {
            TextEditDeleteSel(buf, cursor, sel);
        } else if (cursor > 0) {
            int moveLen = len - cursor + 1;
            memmove(buf + cursor - 1, buf + cursor, static_cast<size_t>(moveLen));
            cursor--;
        } else {
            return changed;
        }
        changed = true;
        return changed;
    }
    if (ctrl && IsKeyPressed(KEY_BACKSPACE)) {
        if (sel >= 0 && sel != cursor) {
            TextEditDeleteSel(buf, cursor, sel);
        } else if (cursor > 0) {
            int start = TextEditWordLeft(buf, cursor);
            int moveLen = len - cursor + 1;
            memmove(buf + start, buf + cursor, static_cast<size_t>(moveLen));
            cursor = start;
        } else {
            return changed;
        }
        changed = true;
        return changed;
    }

    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    int oldCursor = cursor;
    bool moved = false;

    if (ctrl && (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT))) {
        cursor = TextEditWordLeft(buf, cursor);
        moved = true;
    } else if (ctrl && (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT))) {
        cursor = TextEditWordRight(buf, cursor);
        moved = true;
    } else if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) {
        if (sel >= 0 && sel != cursor && !shift) cursor = sel < cursor ? sel : cursor;
        else if (cursor > 0) --cursor;
        moved = true;
    } else if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) {
        if (sel >= 0 && sel != cursor && !shift) cursor = sel > cursor ? sel : cursor;
        else if (cursor < len) ++cursor;
        moved = true;
    } else if (IsKeyPressed(KEY_HOME) || IsKeyPressedRepeat(KEY_HOME)) {
        cursor = 0;
        moved = true;
    } else if (IsKeyPressed(KEY_END) || IsKeyPressedRepeat(KEY_END)) {
        cursor = len;
        moved = true;
    }

    if (moved) {
        if (shift) {
            if (sel < 0) sel = oldCursor;
        } else {
            sel = -1;
        }
        if (sel == cursor) sel = -1;
    }

    // ImGui_ImplRaylib_NewFrame() (called once per frame in UpdateInput())
    // already drained raylib's GetCharPressed() queue into ImGui's IO, so a
    // second GetCharPressed() call here would always come back empty. Read
    // the same characters back out of ImGui's buffered queue instead — it's
    // not cleared until the next NewFrame, so it still reflects this frame's
    // typed input. Skip entirely while an ImGui text field (e.g. an Explorer
    // rename box) wants the keyboard, so keystrokes aren't applied twice.
    ImGuiIO& imguiIO = ImGui::GetIO();
    if (!imguiIO.WantTextInput) {
        for (int i = 0; i < imguiIO.InputQueueCharacters.Size; ++i) {
            int key = imguiIO.InputQueueCharacters[i];
            if (key >= 32 && key <= 126) {
                if (sel >= 0 && sel != cursor) {
                    TextEditDeleteSel(buf, cursor, sel);
                    len = static_cast<int>(strlen(buf));
                }
                if (len < bufSize - 1) {
                    int moveLen = len - cursor + 1;
                    memmove(buf + cursor + 1, buf + cursor, static_cast<size_t>(moveLen));
                    buf[cursor] = static_cast<char>(key);
                    cursor++;
                    len++;
                    changed = true;
                }
            }
        }
    }

    return changed;
}

static void TextEditMouse(const char* buf, int& cursor, int& sel, Rectangle rec,
                          float textStartX, float fontSize, bool active) {
    if (!active) return;
    Vector2 mouse = GetMousePosition();
    auto len = static_cast<int>(strlen(buf));

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (CheckCollisionPointRec(mouse, rec)) {
            int idx = TextLenCap(GetCursorIndexFromMouse(buf, mouse.x, textStartX, fontSize), len);
            if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
                if (sel < 0) sel = cursor;
                cursor = idx;
            } else {
                cursor = idx;
                sel = idx;
            }
            g_mouseDrag = true;
        }
    } else if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && g_mouseDrag) {
        int idx = TextLenCap(GetCursorIndexFromMouse(buf, mouse.x, textStartX, fontSize), len);
        cursor = idx;
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) g_mouseDrag = false;
    if (sel == cursor) sel = -1;
}

static void DrawTextSel(const char* buf, int cursor, int sel, float textStartX,
                        float y, float height, float fontSize, Color color) {
    if (sel < 0 || sel == cursor) return;
    int lo = sel < cursor ? sel : cursor;
    int hi = sel > cursor ? sel : cursor;
    auto len = static_cast<int>(strlen(buf));
    if (lo >= len) return;
    if (hi > len) hi = len;
    char a[192], b[192];
    int al = lo; if (al >= static_cast<int>(sizeof(a))) al = static_cast<int>(sizeof(a)) - 1;
    memcpy(a, buf, static_cast<size_t>(al)); a[al] = '\0';
    int bl = hi; if (bl >= static_cast<int>(sizeof(b))) bl = static_cast<int>(sizeof(b)) - 1;
    memcpy(b, buf, static_cast<size_t>(bl)); b[bl] = '\0';
    float x0 = textStartX + MeasureTextArial(a, fontSize);
    float x1 = textStartX + MeasureTextArial(b, fontSize);
    DrawRectangleRec({ x0, y, x1 - x0, height }, color);
}

static bool DrawNumberInput(Rectangle rec, FieldID fieldId, float& value, const char* label, float labelX) {
    DrawTextArial(label, labelX, rec.y + 3.0f, 13.0f, theme::TEXT_MUTED);

    bool isActive = (g_activeField == fieldId);
    bool changed = false;
    Vector2 mouse = GetMousePosition();
    bool hovered = CheckCollisionPointRec(mouse, rec);
    float textStartX = rec.x + 6.0f;

    if (isActive) g_activeFieldRect = rec;

    if (isActive && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !hovered) {
        g_activeField = FIELD_NONE;
        g_selStart = -1;
        g_mouseDrag = false;
        isActive = false;
    }

    if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (!isActive) {
            g_activeField = fieldId;
            isActive = true;
            snprintf(g_textBuffer, sizeof(g_textBuffer), "%.2f", value);
            g_cursorPos = static_cast<int>(strlen(g_textBuffer));
            g_selStart = -1;
        }
    }

    if (isActive) {
        bool commit = false;
        if (TextEditKeys(g_textBuffer, static_cast<int>(sizeof(g_textBuffer)), g_cursorPos, g_selStart, commit)) {
            if (g_textBuffer[0] != '\0') {
                value = std::strtof(g_textBuffer, nullptr);
                changed = true;
            }
        }
        TextEditMouse(g_textBuffer, g_cursorPos, g_selStart, rec, textStartX, 13.0f, true);
        if (commit) {
            value = (g_textBuffer[0] == '\0') ? 0.0f : std::strtof(g_textBuffer, nullptr);
            changed = true;
            g_activeField = FIELD_NONE;
            g_selStart = -1;
            g_mouseDrag = false;
            isActive = false;
        }
    }

    float hoverT = HoverProgress(static_cast<uint64_t>(fieldId) << 16U, hovered);
    Color boxBg = isActive ? theme::BG_INPUT : Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, hoverT);
    Color border = isActive ? theme::ACCENT : Mix(theme::BORDER, theme::BORDER_STRONG, hoverT);

    DrawRectangleRounded(rec, 0.15f, 4, boxBg);
    DrawRectangleLinesEx(rec, isActive ? 2.0f : 1.0f, border);

    if (isActive) {
        DrawTextSel(g_textBuffer, g_cursorPos, g_selStart, textStartX, rec.y + 2.0f, rec.height - 4.0f, 13.0f, Fade(theme::ACCENT, 0.35f));
    }

    const char* textToDraw = isActive ? g_textBuffer : TextFormat("%.2f", value);
    DrawTextArial(textToDraw, textStartX, rec.y + 3.0f, 13.0f, isActive ? theme::TEXT : theme::TEXT_MUTED);

    if (hovered) MarkIbeam();

    if (isActive) {
        char sub[32] = { 0 };
        strncpy_s(sub, g_textBuffer, static_cast<size_t>(g_cursorPos));
        sub[g_cursorPos] = '\0';

        float cursorOffsetX = MeasureTextArial(sub, 13.0f);
        float cursorX = textStartX + cursorOffsetX;

        if (static_cast<int>(GetTime() * 2.5) % 2 == 0) {
            Vector2 p1 = { cursorX, rec.y + 3.0f };
            Vector2 p2 = { cursorX, rec.y + rec.height - 3.0f };
            DrawLineEx(p1, p2, 1.5f, theme::ACCENT);
        }
    }

    return changed;
}

// Draws a number field bound to the primary selection's value and, when the
// user edits it, copies the resulting value to every other selected object so
// multi-select property edits apply to the whole selection.
template <typename Apply>
static void DrawMultiNumberInput(Rectangle rec, FieldID fieldId, float& value, const char* label, float labelX, Apply apply) {
    float before = value;
    bool changed = DrawNumberInput(rec, fieldId, value, label, labelX);
    if (changed && value != before) {
        for (auto* obj : g_selection) {
            if (obj && obj != g_selectedObject) apply(obj, value);
        }
    }
}

// ---------------------------------------------------------------------------
// 4. Color Picker Pop-up
// ---------------------------------------------------------------------------
static void DrawHueBar(float x, float y, float width, float height) {
    constexpr Color rainbow[] = {
        Color{ 255, 0, 0, 255 },     Color{ 255, 255, 0, 255 },
        Color{ 0, 255, 0, 255 },     Color{ 0, 255, 255, 255 },
        Color{ 0, 0, 255, 255 },     Color{ 255, 0, 255, 255 },
        Color{ 255, 0, 0, 255 }
    };

    float segWidth = width / 6.0f;
    for (int i = 0; i < 6; ++i) {
        DrawRectangleGradientH(
            static_cast<int>(x + static_cast<float>(i) * segWidth), static_cast<int>(y),
            static_cast<int>(segWidth + 1.0f), static_cast<int>(height),
            rainbow[i], rainbow[i + 1]
        );
    }
}

static Color g_colorPickerTemp = BLACK; // temp for water body color picker

static void DrawColorPickerPopupWindow() {
    if (!g_showColorPickerWindow || (!g_color && !g_selectedWater)) return;

    // Resolve working color pointer: either g_color or water body's base color
    Color* workColor = g_color;
    if (!workColor && g_selectedWater) {
        g_colorPickerTemp = g_selectedWater->GetBaseColor();
        workColor = &g_colorPickerTemp;
    }

    Rectangle popupRec;
    GetColorPickerWindowBounds(popupRec);

    Vector2 mouse = GetMousePosition();

    DrawRectangleRec(popupRec, theme::BG_PANEL);
    DrawRectangleLinesEx(popupRec, 1.0f, theme::BORDER_STRONG);

    DrawRectangleRec({ popupRec.x, popupRec.y, popupRec.width, 26.0f }, theme::BG_TITLE);
    DrawLine(static_cast<int>(popupRec.x), static_cast<int>(popupRec.y + 26.0f), static_cast<int>(popupRec.x + popupRec.width), static_cast<int>(popupRec.y + 26.0f), theme::DIVIDER);
    DrawTextArial("Color Picker", popupRec.x + 10.0f, popupRec.y + 5.0f, 14.0f, theme::TEXT);

    Rectangle closeBtn;
    closeBtn.x = popupRec.x + popupRec.width - 22.0f;
    closeBtn.y = popupRec.y + 3.0f;
    closeBtn.width = 18.0f;
    closeBtn.height = 18.0f;

    bool closeHover = CheckCollisionPointRec(mouse, closeBtn);
    float closeT = HoverProgress(30, closeHover);
    if (closeT > 0.0f) DrawRectangleRounded(closeBtn, 0.2f, 4, Mix(theme::BG_WIDGET, theme::DANGER, closeT));
    if (closeHover) MarkHand();
    if (closeHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g_showColorPickerWindow = false;
        g_colorPickerAnchorY = -1.0f;
        return;
    }
    DrawCenteredTextArial("X", closeBtn, 12.0f, closeHover ? theme::TEXT : theme::TEXT_MUTED);

    float startY = popupRec.y + 36.0f;

    Rectangle previewBox = { popupRec.x + 15.0f, startY, 32.0f, 22.0f };
    DrawRectangleRounded(previewBox, 0.15f, 4, *workColor);
    DrawRectangleLinesEx(previewBox, 1.0f, theme::BORDER_STRONG);
    DrawTextArial(TextFormat("R:%d G:%d B:%d", workColor->r, workColor->g, workColor->b), popupRec.x + 58.0f, startY + 4.0f, 13.0f, theme::TEXT_MUTED);

    Rectangle hueTrack    = { popupRec.x + 65.0f, startY + 32.0f, 195.0f, 14.0f };
    Rectangle satTrack    = { popupRec.x + 65.0f, startY + 58.0f, 195.0f, 14.0f };
    Rectangle brightTrack = { popupRec.x + 65.0f, startY + 84.0f, 195.0f, 14.0f };

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) g_activeSlider = 0;

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (CheckCollisionPointRec(mouse, hueTrack))    g_activeSlider = 1;
        if (CheckCollisionPointRec(mouse, satTrack))    g_activeSlider = 2;
        if (CheckCollisionPointRec(mouse, brightTrack)) g_activeSlider = 3;
    }

    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && g_activeSlider > 0) {
        if (g_activeSlider == 1) {
            float t = (mouse.x - hueTrack.x) / hueTrack.width;
            g_hsv.x = Clamp(t, 0.0f, 1.0f) * 360.0f;
            if (g_hsv.x >= 360.0f) g_hsv.x = 359.9f;
        } else if (g_activeSlider == 2) {
            float t = (mouse.x - satTrack.x) / satTrack.width;
            g_hsv.y = Clamp(t, 0.0f, 1.0f);
        } else if (g_activeSlider == 3) {
            float t = (mouse.x - brightTrack.x) / brightTrack.width;
            g_hsv.z = Clamp(t, 0.0f, 1.0f);
        }

        *workColor = ColorFromHSV(g_hsv.x, g_hsv.y, g_hsv.z);
        if (!g_color && g_selectedWater) {
            uint8_t originalAlpha = g_selectedWater->GetBaseColor().a;
            workColor->a = originalAlpha;
            g_selectedWater->SetBaseColor(*workColor);
        }
    }

    DrawTextArial("Hue", popupRec.x + 15.0f, hueTrack.y + 1.0f, 13.0f, theme::TEXT_MUTED);
    DrawHueBar(hueTrack.x, hueTrack.y, hueTrack.width, hueTrack.height);
    DrawRectangleLinesEx(hueTrack, 1.0f, theme::BORDER);
    float hueHandleX = hueTrack.x + (g_hsv.x / 360.0f) * hueTrack.width;
    DrawRectangle(static_cast<int>(hueHandleX) - 3, static_cast<int>(hueTrack.y) - 2, 6, static_cast<int>(hueTrack.height) + 4, COL_WHITE);
    DrawRectangleLines(static_cast<int>(hueHandleX) - 3, static_cast<int>(hueTrack.y) - 2, 6, static_cast<int>(hueTrack.height) + 4, theme::TEXT_DIM);

    DrawTextArial("Sat", popupRec.x + 15.0f, satTrack.y + 1.0f, 13.0f, theme::TEXT_MUTED);
    Color satMin = ColorFromHSV(g_hsv.x, 0.0f, g_hsv.z);
    Color satMax = ColorFromHSV(g_hsv.x, 1.0f, g_hsv.z);
    DrawRectangleGradientH(static_cast<int>(satTrack.x), static_cast<int>(satTrack.y), static_cast<int>(satTrack.width), static_cast<int>(satTrack.height), satMin, satMax);
    DrawRectangleLinesEx(satTrack, 1.0f, theme::BORDER);
    float satHandleX = satTrack.x + g_hsv.y * satTrack.width;
    DrawRectangle(static_cast<int>(satHandleX) - 3, static_cast<int>(satTrack.y) - 2, 6, static_cast<int>(satTrack.height) + 4, COL_WHITE);
    DrawRectangleLines(static_cast<int>(satHandleX) - 3, static_cast<int>(satTrack.y) - 2, 6, static_cast<int>(satTrack.height) + 4, theme::TEXT_DIM);

    DrawTextArial("Bright", popupRec.x + 15.0f, brightTrack.y + 1.0f, 13.0f, theme::TEXT_MUTED);
    Color brightMin = ColorFromHSV(g_hsv.x, g_hsv.y, 0.0f);
    Color brightMax = ColorFromHSV(g_hsv.x, g_hsv.y, 1.0f);
    DrawRectangleGradientH(static_cast<int>(brightTrack.x), static_cast<int>(brightTrack.y), static_cast<int>(brightTrack.width), static_cast<int>(brightTrack.height), brightMin, brightMax);
    DrawRectangleLinesEx(brightTrack, 1.0f, theme::BORDER);
    float brightHandleX = brightTrack.x + g_hsv.z * brightTrack.width;
    DrawRectangle(static_cast<int>(brightHandleX) - 3, static_cast<int>(brightTrack.y) - 2, 6, static_cast<int>(brightTrack.height) + 4, COL_WHITE);
    DrawRectangleLines(static_cast<int>(brightHandleX) - 3, static_cast<int>(brightTrack.y) - 2, 6, static_cast<int>(brightTrack.height) + 4, theme::TEXT_DIM);

    auto swatchY = static_cast<float>(static_cast<int>(brightTrack.y) + 24);
    DrawTextArial("Presets:", popupRec.x + 15.0f, swatchY + 3.0f, 13.0f, theme::TEXT_MUTED);

    constexpr Color colors[] = { COL_RED, COL_ORANGE, COL_GOLD, COL_LIME, COL_GREEN, COL_BLUE, COL_PURPLE, COL_MAGENTA };
    for (int i = 0; i < 8; ++i) {
        Rectangle colorBtn;
        colorBtn.x = popupRec.x + 65.0f + (static_cast<float>(i) * 25.0f);
        colorBtn.y = swatchY;
        colorBtn.width = 20.0f;
        colorBtn.height = 20.0f;
        DrawRectangleRounded(colorBtn, 0.2f, 4, colors[i]);

        if (workColor->r == colors[i].r && workColor->g == colors[i].g && workColor->b == colors[i].b) {
            DrawRectangleLinesEx(colorBtn, 2.0f, COL_WHITE);
        } else {
            DrawRectangleLinesEx(colorBtn, 1.0f, theme::TEXT_DIM);
        }

        if (CheckCollisionPointRec(mouse, colorBtn)) MarkHand();
        if (CheckCollisionPointRec(mouse, colorBtn) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            *workColor = colors[i];
            g_hsv = ColorToHSV(*workColor);
            if (!g_color && g_selectedWater) g_selectedWater->SetBaseColor(*workColor);
        }
    }

    // Multi-select: while the picker is open the primary's color is mirrored to
    // every other selected object.
    if (g_color && g_selection.size() > 1) {
        for (auto* obj : g_selection) {
            if (obj && obj != g_selectedObject) *obj->GetColorPtr() = *g_color;
        }
    }
}

static void DrawCollisionPopup() {
    if (!g_collisionPopupOpen || !g_selectedObject) return;

    Vector2 mouse = GetMousePosition();
    constexpr float itemH = 26.0f;
    constexpr float popupW = 130.0f;
    constexpr int itemCount = 4;

    float x = g_collisionPopupAnchor.x + g_collisionPopupAnchor.width - popupW;
    float y = g_collisionPopupAnchor.y + g_collisionPopupAnchor.height + 2.0f;
    float popupH = static_cast<float>(itemCount) * itemH + 4.0f;
    if (y + popupH > static_cast<float>(GetScreenHeight())) {
        y = g_collisionPopupAnchor.y - popupH - 2.0f;
    }
    Rectangle popupRec = { x, y, popupW, popupH };

    if (IsKeyPressed(KEY_ESCAPE)) {
        g_collisionPopupOpen = false;
        return;
    }
    // The press that opened the popup fires this frame too, so give it a short
    // grace period before treating a click as an outside-click dismissal.
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mouse, popupRec)
        && (GetTime() - g_collisionPopupOpenTime) > 0.15) {
        g_collisionPopupOpen = false;
        return;
    }

    DrawRectangleRec({ popupRec.x + 3.0f, popupRec.y + 3.0f, popupRec.width, popupRec.height }, theme::SHADOW);
    DrawRectangleRounded(popupRec, 0.1f, 4, theme::BG_MENU);
    DrawRectangleLinesEx(popupRec, 1.0f, theme::BORDER_STRONG);

    for (int i = 0; i < itemCount; ++i) {
        auto acc = static_cast<pcoll::CollisionAccuracy>(i);
        Rectangle item = { popupRec.x + 3.0f, popupRec.y + 3.0f + static_cast<float>(i) * itemH,
                           popupRec.width - 6.0f, itemH - 4.0f };
        bool hovered = CheckCollisionPointRec(mouse, item);
        float t = HoverProgress(26U + static_cast<uint64_t>(i), hovered);
        if (t > 0.0f) {
            DrawRectangleRounded(item, 0.15f, 4, Mix(theme::ACCENT, theme::ACCENT_HOVER, t));
        }
        bool current = g_selectedObject->GetCollisionAccuracy() == acc;
        DrawTextArial(pcoll::CollisionAccuracyName(acc), item.x + 10.0f, item.y + 5.0f, 13.0f,
            current ? theme::ACCENT : (hovered ? theme::TEXT : theme::TEXT_MUTED));
        if (hovered) MarkHand();
        if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            g_collisionPopupOpen = false;
            g_selectedObject->SetCollisionAccuracy(acc);
            if (g_selection.size() > 1) {
                for (auto* obj : g_selection) {
                    if (obj && obj != g_selectedObject) obj->SetCollisionAccuracy(acc);
                }
            }
            Log("'%s' collision accuracy = %s", g_selectedObject->GetName().c_str(),
                pcoll::CollisionAccuracyName(acc));
            return;
        }
    }
}

static float DrawGroupHeader(const Rectangle& panelRec, float y, const char* label,
                             bool& open, uint64_t hoverKey) {
    Vector2 mouse = GetMousePosition();
    Rectangle header = { panelRec.x + 10.0f, y, panelRec.width - 20.0f, 24.0f };
    bool hovered = CheckCollisionPointRec(mouse, header);

    DrawLine(static_cast<int>(header.x), static_cast<int>(header.y), static_cast<int>(header.x + header.width), static_cast<int>(header.y), theme::DIVIDER);

    float t = HoverProgress(hoverKey, hovered);
    if (t > 0.0f) {
        DrawRectangleRec(header, Mix(theme::BG_WIDGET, theme::BG_WIDGET_HOVER, t));
    }

    float cx = header.x + 9.0f;
    float cy = header.y + header.height * 0.5f;
    if (open) {
        DrawLineEx({ cx - 4.0f, cy - 2.0f }, { cx, cy + 2.0f }, 1.5f, theme::ACCENT);
        DrawLineEx({ cx, cy + 2.0f }, { cx + 4.0f, cy - 2.0f }, 1.5f, theme::ACCENT);
    } else {
        DrawLineEx({ cx - 2.0f, cy - 4.0f }, { cx + 2.0f, cy }, 1.5f, theme::ACCENT);
        DrawLineEx({ cx + 2.0f, cy }, { cx - 2.0f, cy + 4.0f }, 1.5f, theme::ACCENT);
    }

    DrawTextArial(label, header.x + 20.0f, header.y + 4.0f, 13.0f,
        open ? theme::TEXT : theme::TEXT_MUTED);
    if (hovered) MarkHand();
    if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_clickConsumedThisFrame) open = !open;
    return y + 24.0f;
}

// ---------------------------------------------------------------------------
// 5. Right Panel (Properties Window)
// ---------------------------------------------------------------------------

// Total vertical extent of the properties content, mirroring the layout in
// DrawPropertiesPanel so the scroll offset can be clamped before drawing.
static float PropertiesContentHeight() {
    constexpr float rowHeight = 28.0f;
    constexpr float groupGap = 12.0f;
    auto group = [&](bool open, float rows) {
        return 24.0f + (open ? rows * rowHeight : 0.0f) + groupGap;
    };

    // Water body properties
    if (g_selectedWater) {
        float h = 50.0f;
        h += group(g_positionOpen, 3.0f);   // Pos X, Water H, Pos Z
        h += group(g_sizeOpen, 2.0f);        // Width, Depth
        h += group(g_generalOpen, 2.0f);     // Color swatch, Transparency
        h += group(g_waterNoiseOpen, 4.0f); // Noise: Amplitude, Frequency, Speed, Octaves
        h += group(g_waterFoamOpen, 3.0f);      // Foam: Intensity, Scale, Threshold
        h += 10.0f;
        return h;
    }

    float h = 50.0f; // below the title strip
    h += group(true, 6.0f);               // General (color + anchored + can collide + collision + transparency + mass)
    if (g_selectedObject && g_selectedObject->HasModel()) h += rowHeight; // mesh info line
    h += 2 * rowHeight;                                         // texture label + preset dropdown row
    h += group(g_positionOpen, 3.0f);     // Position
    h += group(g_sizeOpen, 3.0f);         // Size
    if (g_rotation) h += group(g_rotationOpen, 3.0f);
    if (g_origin)   h += group(g_originOpen, 3.0f);
    if (g_selectedObject) h += group(g_linearVelocityOpen, 3.0f);
    if (g_selectedObject) h += group(g_angularVelocityOpen, 3.0f);
    h += 10.0f; // bottom padding
    return h;
}

static void DrawPropertiesPanel() {
    Rectangle panelRec = GetPropertiesPanelBounds();

    DrawRectangleRec(panelRec, theme::BG_PANEL);
    DrawRectangleLinesEx(panelRec, 2.0f, theme::BORDER);

    DrawRectangleRec({ panelRec.x, panelRec.y, panelRec.width, 36.0f }, theme::BG_TITLE);
    DrawLine(static_cast<int>(panelRec.x), static_cast<int>(panelRec.y + 36.0f), static_cast<int>(panelRec.x + panelRec.width), static_cast<int>(panelRec.y + 36.0f), theme::DIVIDER);

    // Content area (below the title strip) used for scissor clipping + scrolling.
    Rectangle contentRec = { panelRec.x + 2.0f, panelRec.y + 38.0f, panelRec.width - 4.0f, panelRec.height - 40.0f };

if (!g_pos && !g_size && !g_color && !g_selectedWater) {
        DrawTextArial("[Properties]", panelRec.x + (panelRec.width / 2.0f) - 40.0f, panelRec.y + (panelRec.height / 2.0f) - 10.0f, 16.0f, theme::TEXT_DIM);
        return;
    }

    // --- Properties ---
    DrawTextArial("PROPERTIES", panelRec.x + 15.0f, panelRec.y + 10.0f, 14.0f, theme::ACCENT);
if (g_selection.size() > 1) {
        DrawTextArial(TextFormat("(%d selected)", static_cast<int>(g_selection.size())),
            panelRec.x + 15.0f, panelRec.y + 24.0f, 11.0f, theme::TEXT_DIM);
    }

    Vector2 mouse = GetMousePosition();

    float contentHeight = PropertiesContentHeight();
    float maxScroll = contentHeight - contentRec.height;
    if (maxScroll < 0.0f) maxScroll = 0.0f;

    if (CheckCollisionPointRec(mouse, panelRec)) {
        g_propertiesScrollY -= GetMouseWheelMove() * 24.0f;
        if (g_propertiesScrollY < 0.0f) g_propertiesScrollY = 0.0f;
    }
    if (g_propertiesScrollY > maxScroll) g_propertiesScrollY = maxScroll;

    constexpr float rowHeight = 28.0f;
    constexpr float inputWidth = 125.0f;
    constexpr float inputHeight = 22.0f;
    constexpr float groupGap = 12.0f;
    float inputX = panelRec.x + 95.0f;
    float labelX = panelRec.x + 15.0f;
    float generalControlX = inputX + inputWidth - 22.0f;

    float y = panelRec.y + 50.0f - g_propertiesScrollY;
    bool live = g_playActive && g_simulation;

    BeginScissorMode(static_cast<int>(contentRec.x), static_cast<int>(contentRec.y), static_cast<int>(contentRec.width), static_cast<int>(contentRec.height));

    // --- Water body properties ---
    if (g_selectedWater) {
        WaterBody* w = g_selectedWater;

        DrawTextArial("WATER BODY", panelRec.x + 15.0f, panelRec.y + 10.0f, 14.0f, theme::ACCENT);

        // Position
        y = DrawGroupHeader(panelRec, y, "Position", g_positionOpen, 100);
        if (g_positionOpen) {
            Vector3 wp = *w->GetPosPtr();
            float valX = wp.x;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_POS_X, valX, "Pos X:", labelX)) { wp.x = valX; *w->GetPosPtr() = wp; }
            y += rowHeight;
            float wh = w->GetWaterHeight();
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_WATER_H, wh, "Water H:", labelX)) w->SetWaterHeight(wh);
            y += rowHeight;
            float valZ = wp.z;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_POS_Z, valZ, "Pos Z:", labelX)) { wp.z = valZ; *w->GetPosPtr() = wp; }
            y += rowHeight;
        }
        y += groupGap;

        // Size
        y = DrawGroupHeader(panelRec, y, "Size", g_sizeOpen, 101);
        if (g_sizeOpen) {
            Vector3 ws = *w->GetSizePtr();
            float valW = ws.x;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_SIZE_W, valW, "Width:", labelX)) { ws.x = fmaxf(1.0f, valW); *w->GetSizePtr() = ws; w->MarkMeshDirty(); }
            y += rowHeight;
            float valD = ws.z;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_SIZE_D, valD, "Depth:", labelX)) { ws.z = fmaxf(1.0f, valD); *w->GetSizePtr() = ws; w->MarkMeshDirty(); }
            y += rowHeight;
        }
        y += groupGap;

        // Appearance (color + transparency)
        y = DrawGroupHeader(panelRec, y, "Appearance", g_generalOpen, 105);
        if (g_generalOpen) {
            Color baseCol = w->GetBaseColor();
            DrawTextArial("Color:", labelX, y + 3.0f, 13.0f, theme::TEXT_MUTED);
            Rectangle colorSwatchBtn = { generalControlX, y, 22.0f, 22.0f };
            bool swatchHovered = CheckCollisionPointRec(mouse, colorSwatchBtn);
            float swatchT = HoverProgress(20, swatchHovered);
            DrawRectangleRounded(colorSwatchBtn, 0.2f, 4, baseCol);
            DrawRectangleLinesEx(colorSwatchBtn, swatchHovered ? 2.0f : 1.0f, Mix(theme::BORDER_STRONG, theme::ACCENT_HOVER, swatchT));
            if (swatchHovered) MarkHand();
            if (swatchHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                g_hsv = ColorToHSV(baseCol);
                g_showColorPickerWindow = !g_showColorPickerWindow;
            }
            y += rowHeight;

            float trans = w->GetTransparency();
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_TRANSPARENCY, trans, "Transparency:", labelX)) w->SetTransparency(trans);
            y += rowHeight;
        }
        y += groupGap;

        // Noise
        y = DrawGroupHeader(panelRec, y, "Noise", g_waterNoiseOpen, 104);
        if (g_waterNoiseOpen) {
            WaterBody::NoiseParams n = w->GetNoiseParams();
            float valA = n.amplitude;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_AMPLITUDE, valA, "Amplitude:", labelX)) { n.amplitude = valA; w->SetNoiseParams(n); }
            y += rowHeight;
            float valF = n.frequency;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_FREQUENCY, valF, "Frequency:", labelX)) { n.frequency = valF; w->SetNoiseParams(n); }
            y += rowHeight;
            float valS = n.speed;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_SPEED, valS, "Speed:", labelX)) { n.speed = valS; w->SetNoiseParams(n); }
            y += rowHeight;
            float valO = (float)n.octaves;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_OCTAVES, valO, "Octaves:", labelX)) { n.octaves = (int)valO; w->SetNoiseParams(n); }
            y += rowHeight;
        }
        y += groupGap;

        // Foam
        y = DrawGroupHeader(panelRec, y, "Foam", g_waterFoamOpen, 106);
        if (g_waterFoamOpen) {
            WaterBody::FoamParams f = w->GetFoamParams();
            float valI = f.intensity;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_FOAM_INTENSITY, valI, "Intensity:", labelX)) { f.intensity = valI; w->SetFoamParams(f); }
            y += rowHeight;
            float valFS = f.scale;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_FOAM_SCALE, valFS, "Scale:", labelX)) { f.scale = valFS; w->SetFoamParams(f); }
            y += rowHeight;
            float valFT = f.threshold;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_FOAM_THRESHOLD, valFT, "Threshold:", labelX)) { f.threshold = valFT; w->SetFoamParams(f); }
            y += rowHeight;
        }
        y += groupGap;

        EndScissorMode();
        return;
    }

    // --- General (color + anchored) ---
    y = DrawGroupHeader(panelRec, y, "General", g_generalOpen, 105);
    if (g_generalOpen) {
        DrawTextArial("Color:", labelX, y + 3.0f, 13.0f, theme::TEXT_MUTED);

        Rectangle colorSwatchBtn = { generalControlX, y, 22.0f, 22.0f };

        bool swatchHovered = CheckCollisionPointRec(mouse, colorSwatchBtn);
        float swatchT = HoverProgress(20, swatchHovered);

        DrawRectangleRounded(colorSwatchBtn, 0.2f, 4, *g_color);
        DrawRectangleLinesEx(colorSwatchBtn, swatchHovered ? 2.0f : 1.0f, Mix(theme::BORDER_STRONG, theme::ACCENT_HOVER, swatchT));
        if (swatchHovered) MarkHand();

        if (!g_playActive && swatchHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            g_showColorPickerWindow = !g_showColorPickerWindow;
        }
        y += rowHeight;

        DrawTextArial("Anchored:", labelX, y + 3.0f, 13.0f, theme::TEXT_MUTED);

        Rectangle anchoredCheck = { generalControlX, y, 22.0f, 22.0f };

        bool anchored = g_selectedObject != nullptr && g_selectedObject->anchored;
        bool anchoredHovered = CheckCollisionPointRec(mouse, anchoredCheck);
        bool anchoredDisabled = g_selectedObject == nullptr;
        float anchorT = HoverProgress(21, anchoredHovered && !anchoredDisabled);

        Color checkBg = anchoredDisabled ? theme::BG_WIDGET
            : Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, anchorT);
        DrawRectangleRounded(anchoredCheck, 0.2f, 4, checkBg);
        DrawRectangleLinesEx(anchoredCheck, anchoredHovered ? 2.0f : 1.0f,
            anchoredHovered ? theme::ACCENT : theme::BORDER);

        if (anchored) {
            Color checkColor = anchoredDisabled ? theme::TEXT_DIM : theme::SUCCESS;
            Vector2 c1 = { anchoredCheck.x + 3.0f, anchoredCheck.y + anchoredCheck.height * 0.5f };
            Vector2 c2 = { anchoredCheck.x + anchoredCheck.width * 0.5f, anchoredCheck.y + anchoredCheck.height - 5.0f };
            Vector2 c3 = { anchoredCheck.x + anchoredCheck.width - 3.0f, anchoredCheck.y + 4.0f };
            DrawLineEx(c1, c2, 2.5f, checkColor);
            DrawLineEx(c2, c3, 2.5f, checkColor);
        }
        if (anchoredHovered && !anchoredDisabled) MarkHand();

        if (!anchoredDisabled && anchoredHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            bool newValue = !g_selectedObject->anchored;
            g_selectedObject->anchored = newValue;
            if (g_selection.size() > 1) {
                for (auto* obj : g_selection) {
                    if (obj && obj != g_selectedObject) obj->anchored = newValue;
                }
            }
            Log("'%s' anchored = %s", g_selectedObject->GetName().c_str(),
                g_selectedObject->anchored ? "true" : "false");
        }

        // --- Can Collide ---
        y += rowHeight;
        DrawTextArial("Can Collide:", labelX, y + 3.0f, 13.0f, theme::TEXT_MUTED);

        Rectangle canCollideCheck = { generalControlX, y, 22.0f, 22.0f };

        bool canCollide = g_selectedObject != nullptr && g_selectedObject->canCollide;
        bool canCollideHovered = CheckCollisionPointRec(mouse, canCollideCheck);
        bool canCollideDisabled = g_selectedObject == nullptr;
        float canCollideT = HoverProgress(24, canCollideHovered && !canCollideDisabled);

        Color ccBg = canCollideDisabled ? theme::BG_WIDGET
            : Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, canCollideT);
        DrawRectangleRounded(canCollideCheck, 0.2f, 4, ccBg);
        DrawRectangleLinesEx(canCollideCheck, canCollideHovered ? 2.0f : 1.0f,
            canCollideHovered ? theme::ACCENT : theme::BORDER);

        if (canCollide) {
            Color checkColor = canCollideDisabled ? theme::TEXT_DIM : theme::SUCCESS;
            Vector2 c1 = { canCollideCheck.x + 3.0f, canCollideCheck.y + canCollideCheck.height * 0.5f };
            Vector2 c2 = { canCollideCheck.x + canCollideCheck.width * 0.5f, canCollideCheck.y + canCollideCheck.height - 5.0f };
            Vector2 c3 = { canCollideCheck.x + canCollideCheck.width - 3.0f, canCollideCheck.y + 4.0f };
            DrawLineEx(c1, c2, 2.5f, checkColor);
            DrawLineEx(c2, c3, 2.5f, checkColor);
        }
        if (canCollideHovered && !canCollideDisabled) MarkHand();

        if (!canCollideDisabled && canCollideHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            bool newValue = !g_selectedObject->canCollide;
            g_selectedObject->canCollide = newValue;
            if (g_selection.size() > 1) {
                for (auto* obj : g_selection) {
                    if (obj && obj != g_selectedObject) obj->canCollide = newValue;
                }
            }
            Log("'%s' canCollide = %s", g_selectedObject->GetName().c_str(),
                g_selectedObject->canCollide ? "true" : "false");
        }

        // --- Collision accuracy (combo) ---
        y += rowHeight;
        DrawTextArial("Collision:", labelX, y + 3.0f, 13.0f, theme::TEXT_MUTED);

        bool accSupported = g_selectedObject && g_selectedObject->IsCollisionAccuracySupported();
        pcoll::CollisionAccuracy acc = g_selectedObject
            ? g_selectedObject->GetCollisionAccuracy() : pcoll::CollisionAccuracy::Default;

        Rectangle accCombo = { inputX, y, inputWidth, inputHeight };
        bool accHovered = CheckCollisionPointRec(mouse, accCombo);
        float accT = HoverProgress(25, accHovered && accSupported);
        Color accBg = accSupported ? Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, accT) : theme::BG_WIDGET;
        DrawRectangleRounded(accCombo, 0.2f, 4, accBg);
        DrawRectangleLinesEx(accCombo, (accHovered && accSupported) ? 2.0f : 1.0f,
            (accHovered && accSupported) ? theme::ACCENT : theme::BORDER);
        DrawTextArial(pcoll::CollisionAccuracyName(acc), inputX + 8.0f, y + 4.0f, 13.0f,
            accSupported ? theme::TEXT : theme::TEXT_MUTED);
        if (accSupported) {
            float ax = inputX + inputWidth - 14.0f;
            float ay = y + inputHeight * 0.5f;
            DrawLineEx({ ax - 3.0f, ay - 1.5f }, { ax, ay + 1.5f }, 1.5f, theme::TEXT_DIM);
            DrawLineEx({ ax, ay + 1.5f }, { ax + 3.0f, ay - 1.5f }, 1.5f, theme::TEXT_DIM);
            if (accHovered) MarkHand();
            if (accHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                g_collisionPopupOpen = !g_collisionPopupOpen;
                if (g_collisionPopupOpen) g_collisionPopupOpenTime = GetTime();
                g_collisionPopupAnchor = accCombo;
            }
        }

        // --- Transparency (0 = visible, 1 = invisible) ---
        if (g_selectedObject) {
            y += rowHeight;

            float transparencyValue = g_selectedObject->GetTransparency();
            float transparencyBefore = transparencyValue;
            bool transparencyEdited = DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_TRANSPARENCY, transparencyValue, "Transparency:", labelX);

            if (transparencyEdited && transparencyValue != transparencyBefore) {
                transparencyValue = transparencyValue < 0.0f ? 0.0f : (transparencyValue > 1.0f ? 1.0f : transparencyValue);
                g_selectedObject->SetTransparency(transparencyValue);
                if (g_selection.size() > 1) {
                    for (auto* obj : g_selection) {
                        if (obj && obj != g_selectedObject) obj->SetTransparency(transparencyValue);
                    }
                }
                Log("'%s' transparency = %.2f", g_selectedObject->GetName().c_str(), transparencyValue);
            }
        }

        // --- Mass (auto from size; manual override; reset to auto when scaled) ---
        if (g_selectedObject) {
            y += rowHeight;

            float massValue = g_selectedObject->GetMass();
            float massBefore = massValue;
            bool massAuto = g_selectedObject->IsMassAuto();
            bool massEdited = DrawNumberInput({ inputX, y, inputWidth - 30.0f, inputHeight }, FIELD_MASS, massValue, "Mass:", labelX);

            Rectangle massCheck = { generalControlX, y, 22.0f, 22.0f };
            bool massHovered = CheckCollisionPointRec(mouse, massCheck);
            float massT = HoverProgress(23, massHovered);
            Color massBg = massAuto
                ? Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, massT)
                : Mix(theme::BG_WIDGET, theme::BG_WIDGET, massT);
            DrawRectangleRounded(massCheck, 0.2f, 4, massBg);
            DrawRectangleLinesEx(massCheck, massHovered ? 2.0f : 1.0f,
                massHovered ? theme::ACCENT : theme::BORDER);
            if (massAuto) {
                Color checkColor = theme::SUCCESS;
                Vector2 c1 = { massCheck.x + 3.0f, massCheck.y + massCheck.height * 0.5f };
                Vector2 c2 = { massCheck.x + massCheck.width * 0.5f, massCheck.y + massCheck.height - 5.0f };
                Vector2 c3 = { massCheck.x + massCheck.width - 3.0f, massCheck.y + 4.0f };
                DrawLineEx(c1, c2, 2.5f, checkColor);
                DrawLineEx(c2, c3, 2.5f, checkColor);
            }
            if (massHovered) MarkHand();

            if (massHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (massAuto) g_selectedObject->SetMass(g_selectedObject->GetMass());
                else g_selectedObject->ResetMassAuto();
                if (g_selection.size() > 1) {
                    bool makeManual = !massAuto;
                    for (auto* obj : g_selection) {
                        if (!obj || obj == g_selectedObject) continue;
                        if (makeManual) obj->SetMass(obj->GetMass());
                        else obj->ResetMassAuto();
                    }
                }
                Log("'%s' mass = %s (%.3f)", g_selectedObject->GetName().c_str(),
                    g_selectedObject->IsMassAuto() ? "auto" : "manual", g_selectedObject->GetMass());
            }

            if (massValue != g_selectedObject->GetMass()) {
                g_selectedObject->SetMass(massValue);
            }
            if (massEdited && massValue != massBefore && g_selection.size() > 1) {
                for (auto* obj : g_selection) {
                    if (obj && obj != g_selectedObject) obj->SetMass(massValue);
                }
            }
        }

        // --- Mesh (read-only info for imported models) ---
        if (g_selectedObject && g_selectedObject->HasModel()) {
            y += rowHeight;
            std::string meshName = g_selectedObject->GetModelPath();
            size_t slash = meshName.find_last_of("/\\");
            if (slash != std::string::npos) meshName = meshName.substr(slash + 1);
            DrawTextArial("Mesh:", labelX, y + 6.0f, 14.0f, theme::TEXT_MUTED);
            float avail = inputX + inputWidth - labelX - 70.0f;
            const char* text = meshName.c_str();
            float textW = MeasureTextArial(text, 13.0f);
            if (textW > avail && avail > 10.0f) {
                float maxChars = avail / (textW / static_cast<float>(meshName.size()));
                std::string clipped = meshName.substr(0, static_cast<size_t>(maxChars) - 1) + "...";
                DrawTextArial(clipped.c_str(), inputX, y + 6.0f, 13.0f, theme::TEXT);
            } else {
                DrawTextArial(text, inputX, y + 6.0f, 13.0f, theme::TEXT);
            }
        }

        // --- Texture (for both primitives and imported meshes) ---
        y += rowHeight;
        DrawTextArial("Texture:", labelX, y + 3.0f, 13.0f, theme::TEXT_MUTED);

        std::string texName = "None";
        Color texColor = theme::TEXT_DIM;
        if (g_selectedObject && !g_selectedObject->GetTexturePath().empty()) {
            std::string tp = g_selectedObject->GetTexturePath();
            size_t tslash = tp.find_last_of("/\\");
            if (tslash != std::string::npos) tp = tp.substr(tslash + 1);
            texName = tp;
            texColor = theme::TEXT;
        }

        // Display texture name
        float texTextX = inputX;
        float texAvailW = inputX + inputWidth - texTextX - 50.0f;
        const char* texText = texName.c_str();
        float texTextW = MeasureTextArial(texText, 13.0f);
        if (texTextW > texAvailW && texAvailW > 10.0f) {
            float maxChars = texAvailW / (texTextW / static_cast<float>(texName.size()));
            std::string clipped = texName.substr(0, static_cast<size_t>(maxChars) - 1) + "...";
            DrawTextArial(clipped.c_str(), texTextX, y + 3.0f, 13.0f, texColor);
        } else {
            DrawTextArial(texText, texTextX, y + 3.0f, 13.0f, texColor);
        }

        // Preset Texture Dropdown (shown for primitives AND meshes if presets exist)
        const auto& currentProject = ::project::GetCurrentProject();
        bool hasPrimitive = false, hasMesh = false;
        for (auto* obj : g_selection) {
            if (obj && obj->HasModel()) hasMesh = true;
            else if (obj) hasPrimitive = true;
        }

        std::vector<std::string> presets;

        // Preset textures are global - always use the working directory (flyengine root)
        // for both scanning and resolving paths. This avoids path resolution issues
        // when a project is open in a different location.
        std::string presetBaseDir = std::filesystem::current_path().generic_string();

        if (!presetBaseDir.empty()) {
            // Auto-refresh cache when dropdown opens (if >2s since last refresh)
            double now = GetTime();
            if (!g_presetDropdownItemsCached || g_presetDropdownProjectDir != presetBaseDir ||
                (g_presetDropdownOpen && now - g_presetDropdownLastRefresh > 2.0)) {
                g_presetDropdownItems = textureManager::GetPresetTextures(presetBaseDir);
                g_presetDropdownProjectDir = presetBaseDir;
                g_presetDropdownItemsCached = true;
                g_presetDropdownLastRefresh = now;
            }
            presets = g_presetDropdownItems;
        }

        // Store the base dir for use in click handler
        std::string projectDirForPresets = presetBaseDir;

        bool showPresetDropdown = (hasPrimitive || hasMesh) && !presets.empty();
        bool showPresetError = (hasPrimitive || hasMesh) && presets.empty() && !projectDirForPresets.empty();

        // Save texture row y for folder/clear button placement
        float texRowY = y;

        if (showPresetDropdown) {
            // Advance y for dropdown row
            y += rowHeight;
            // Build dropdown items: "None" + preset filenames (stem only)
            std::vector<std::string> items = {"None"};
            for (const auto& p : presets) {
                std::string name = std::filesystem::path(p).stem().string();
                items.push_back(name);
            }

            // Dropdown rect (leave space for refresh button on right)
            const float refreshBtnSize = 24.0f;
            Rectangle dropdownRect = { inputX, y, inputWidth - refreshBtnSize - 2.0f, inputHeight };
            Rectangle refreshBtnRect = { dropdownRect.x + dropdownRect.width + 2.0f, y, refreshBtnSize, refreshBtnSize };
            bool dropdownHovered = CheckCollisionPointRec(mouse, dropdownRect);
            bool refreshBtnHovered = CheckCollisionPointRec(mouse, refreshBtnRect);

            // Draw dropdown background
            float dropdownT = HoverProgress(300, dropdownHovered);
            Color dropdownBg = Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, dropdownT);
            DrawRectangleRounded(dropdownRect, 0.2f, 4, dropdownBg);
            DrawRectangleLinesEx(dropdownRect, dropdownHovered ? 2.0f : 1.0f,
                dropdownHovered ? theme::ACCENT : theme::BORDER);
            if (dropdownHovered) MarkHand();

            // Auto-detect current texture among presets
            if (g_selectedObject) {
                std::string curTex = g_selectedObject->GetTexturePath();
                if (curTex.empty()) {
                    g_presetDropdownSelected = 0; // "None"
                } else {
                    g_presetDropdownSelected = -1;
                    std::string curStem = std::filesystem::path(curTex).stem().string();
                    for (int pi = 0; pi < (int)presets.size(); ++pi) {
                        std::string presetStem = std::filesystem::path(presets[pi]).stem().string();
                        if (curStem == presetStem) {
                            g_presetDropdownSelected = pi + 1;
                            break;
                        }
                    }
                }
            }

            // Draw current selection text
            std::string currentText = (g_presetDropdownSelected >= 0 && g_presetDropdownSelected < (int)items.size())
                ? items[g_presetDropdownSelected]
                : "Select preset texture...";
            float textX = dropdownRect.x + 8.0f;
            float textY = dropdownRect.y + (dropdownRect.height - 13.0f) * 0.5f;
            DrawTextArial(currentText.c_str(), textX, textY, 13.0f, theme::TEXT);

            // Draw dropdown arrow
            float arrowX = dropdownRect.x + dropdownRect.width - 20.0f;
            float arrowY = dropdownRect.y + dropdownRect.height * 0.5f;
            DrawTriangle(
                { arrowX - 5.0f, arrowY - 3.0f },
                { arrowX + 5.0f, arrowY - 3.0f },
                { arrowX, arrowY + 3.0f },
                theme::TEXT
            );

            // Draw refresh button [↻]
            float refreshT = HoverProgress(301, refreshBtnHovered);
            Color refreshBg = Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, refreshT);
            DrawRectangleRounded(refreshBtnRect, 0.2f, 4, refreshBg);
            DrawRectangleLinesEx(refreshBtnRect, refreshBtnHovered ? 2.0f : 1.0f,
                refreshBtnHovered ? theme::ACCENT : theme::BORDER);
            if (refreshBtnHovered) MarkHand();
            // Draw circular arrow (refresh symbol)
            float cx = refreshBtnRect.x + refreshBtnRect.width * 0.5f;
            float cy = refreshBtnRect.y + refreshBtnRect.height * 0.5f;
            float r = 6.0f;
            // Draw arc for refresh symbol
            for (int seg = 0; seg < 12; ++seg) {
                float a1 = seg * 30.0f * DEG2RAD;
                float a2 = (seg + 1) * 30.0f * DEG2RAD;
                if (seg >= 9) continue; // Leave gap for arrowhead
                DrawLineEx(
                    { cx + r * cosf(a1), cy + r * sinf(a1) },
                    { cx + r * cosf(a2), cy + r * sinf(a2) },
                    2.0f, theme::TEXT
                );
            }
            // Draw arrowhead
            float arrowA = 9 * 30.0f * DEG2RAD;
            float arrowLen = 8.0f;
            Vector2 arrowTip = { cx + arrowLen * cosf(arrowA), cy + arrowLen * sinf(arrowA) };
            Vector2 arrowBase1 = { cx + r * cosf(arrowA - 0.5f), cy + r * sinf(arrowA - 0.5f) };
            Vector2 arrowBase2 = { cx + r * cosf(arrowA + 0.5f), cy + r * sinf(arrowA + 0.5f) };
            DrawTriangle(arrowTip, arrowBase1, arrowBase2, theme::TEXT);

            // Handle refresh button click
            if (refreshBtnHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                // Force cache refresh
                g_presetDropdownItemsCached = false;
                g_presetDropdownLastRefresh = 0.0;
            }

            // Handle click to open/close
            if (dropdownHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                g_presetDropdownOpen = !g_presetDropdownOpen;
            }

            // Draw open dropdown list (store state for later rendering after scissor ends)
            if (g_presetDropdownOpen) {
                float itemHeight = 24.0f;
                int maxVisible = 8;
                int visibleCount = (int)std::min<size_t>(items.size(), maxVisible);
                Rectangle listRect = { dropdownRect.x, dropdownRect.y + dropdownRect.height,
                                     dropdownRect.width, itemHeight * visibleCount };

                // Keep the list fully on-screen: if it doesn't fit below the
                // dropdown header, flip it to open upward instead of letting
                // it run off (and get sliced at) the bottom of the window.
                float screenH = static_cast<float>(GetScreenHeight());
                if (listRect.y + listRect.height > screenH) {
                    float flippedY = dropdownRect.y - listRect.height;
                    listRect.y = (flippedY >= 0.0f) ? flippedY : (screenH - listRect.height);
                    if (listRect.y < 0.0f) listRect.y = 0.0f;
                }
                // Also clamp horizontally in case the header sits near a screen edge.
                float screenW = static_cast<float>(GetScreenWidth());
                if (listRect.x + listRect.width > screenW) listRect.x = screenW - listRect.width;
                if (listRect.x < 0.0f) listRect.x = 0.0f;

                // Store state for deferred rendering
                g_presetDropdownListPending = true;
                g_presetDropdownListRect = listRect;
                g_presetDropdownHeaderRect = dropdownRect;
                g_presetDropdownListItems = items;
                g_presetDropdownListPresets = presets;
                g_presetDropdownListProjectDir = projectDirForPresets;
            } else {
                g_presetDropdownListPending = false;
            }
        }
        else if (showPresetError) {
            // Advance y for error row and show error message
            y += rowHeight;
            DrawTextArial("Could not load preset textures", inputX, y + 3.0f, 13.0f, theme::TEXT_DIM);
        }

        // Folder + Clear buttons (only for imported meshes)
        if (hasMesh && !g_playActive) {
            // Folder button [📁]
            Rectangle folderBtn = { inputX + inputWidth - 44.0f, texRowY, 22.0f, 22.0f };
            bool folderHovered = CheckCollisionPointRec(mouse, folderBtn);
            float folderT = HoverProgress(200, folderHovered);
            Color folderBg = Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, folderT);
            DrawRectangleRounded(folderBtn, 0.2f, 4, folderBg);
            DrawRectangleLinesEx(folderBtn, folderHovered ? 2.0f : 1.0f,
                folderHovered ? theme::ACCENT : theme::BORDER);
            if (folderHovered) MarkHand();
            DrawTextArial("[F]", folderBtn.x + 4.0f, folderBtn.y + 3.0f, 12.0f, theme::TEXT);

            // Clear button [🗑️]
            Rectangle clearBtn = { inputX + inputWidth - 22.0f, texRowY, 22.0f, 22.0f };
            bool anySelectedHasTexture = false;
            for (auto* obj : g_selection) {
                if (obj && !obj->GetTexturePath().empty()) { anySelectedHasTexture = true; break; }
            }
            bool clearHovered = CheckCollisionPointRec(mouse, clearBtn) && anySelectedHasTexture;
            float clearT = HoverProgress(201, clearHovered);
            Color clearBg = clearHovered ? Mix(theme::BG_INPUT, theme::DANGER, clearT) : theme::BG_WIDGET;
            DrawRectangleRounded(clearBtn, 0.2f, 4, clearBg);
            DrawRectangleLinesEx(clearBtn, clearHovered ? 2.0f : 1.0f,
                clearHovered ? theme::DANGER : theme::BORDER);
            if (clearHovered) MarkHand();
            if (g_deleteIcon.id != 0) {
                DrawTexturePro(g_deleteIcon,
                    { 0, 0, (float)g_deleteIcon.width, (float)g_deleteIcon.height },
                    { clearBtn.x + 3.0f, clearBtn.y + 3.0f, clearBtn.width - 6.0f, clearBtn.height - 6.0f },
                    { 0, 0 }, 0.0f, theme::TEXT);
            } else {
                DrawTextArial("X", clearBtn.x + 6.0f, clearBtn.y + 3.0f, 12.0f, theme::TEXT);
            }

            // Handle folder/clear clicks (meshes only)
            if (folderHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                std::string texPath = ChooseTexturePath();
                if (!texPath.empty()) {
if (!currentProject.path.empty()) {
                    for (auto* obj : g_selection) {
                        if (obj && obj->HasModel()) {
                            obj->SetTexturePath(
                                textureManager::RegisterTexture(texPath, obj->GetModelPath()),
                                currentProject.path
                            );
                        }
                    }
                }
            }
if (clearHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (!currentProject.path.empty()) {
                    for (auto* obj : g_selection) {
                        if (obj && obj->HasModel()) {
                            obj->ClearTexture(currentProject.path);
                        }
                    }
}
        }
        // Advance past texture section: always 1 more row to get past the
        // dropdown/error row (or the label row if neither is shown).
        y += rowHeight;
    }
}
    y += groupGap;

    // --- Position ---
    y = DrawGroupHeader(panelRec, y, "Position", g_positionOpen, 100);
    if (g_positionOpen) {
        DrawMultiNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_POS_X, g_pos->x, "Pos X:", labelX,
            [](ScatteredObject* o, float v) { o->GetPosPtr()->x = v; });
        DrawMultiNumberInput({ inputX, y + rowHeight, inputWidth, inputHeight }, FIELD_POS_Y, g_pos->y, "Pos Y:", labelX,
            [](ScatteredObject* o, float v) { o->GetPosPtr()->y = v; });
        DrawMultiNumberInput({ inputX, y + rowHeight * 2.0f, inputWidth, inputHeight }, FIELD_POS_Z, g_pos->z, "Pos Z:", labelX,
            [](ScatteredObject* o, float v) { o->GetPosPtr()->z = v; });
        y += rowHeight * 3.0f;
    }
    y += groupGap;

    // --- Size ---
    y = DrawGroupHeader(panelRec, y, "Size", g_sizeOpen, 101);
    if (g_sizeOpen) {
        Vector3 sizeBefore = *g_size;
        DrawMultiNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_SIZE_W, g_size->x, "Width:", labelX,
            [](ScatteredObject* o, float v) { o->GetSizePtr()->x = v; o->ResetMassAuto(); });
        DrawMultiNumberInput({ inputX, y + rowHeight, inputWidth, inputHeight }, FIELD_SIZE_H, g_size->y, "Height:", labelX,
            [](ScatteredObject* o, float v) { o->GetSizePtr()->y = v; o->ResetMassAuto(); });
        DrawMultiNumberInput({ inputX, y + rowHeight * 2.0f, inputWidth, inputHeight }, FIELD_SIZE_L, g_size->z, "Length:", labelX,
            [](ScatteredObject* o, float v) { o->GetSizePtr()->z = v; o->ResetMassAuto(); });
        if (g_selectedObject &&
            (g_size->x != sizeBefore.x || g_size->y != sizeBefore.y || g_size->z != sizeBefore.z)) {
            g_selectedObject->ResetMassAuto();
        }
        y += rowHeight * 3.0f;
    }
    y += groupGap;

    // --- Rotation ---
    if (g_rotation) {
        y = DrawGroupHeader(panelRec, y, "Rotation", g_rotationOpen, 102);
        if (g_rotationOpen) {
            DrawMultiNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_ROT_X, g_rotation->x, "Rot X:", labelX,
                [](ScatteredObject* o, float v) { o->GetRotationPtr()->x = v; });
            DrawMultiNumberInput({ inputX, y + rowHeight, inputWidth, inputHeight }, FIELD_ROT_Y, g_rotation->y, "Rot Y:", labelX,
                [](ScatteredObject* o, float v) { o->GetRotationPtr()->y = v; });
            DrawMultiNumberInput({ inputX, y + rowHeight * 2.0f, inputWidth, inputHeight }, FIELD_ROT_Z, g_rotation->z, "Rot Z:", labelX,
                [](ScatteredObject* o, float v) { o->GetRotationPtr()->z = v; });
            y += rowHeight * 3.0f;
        }
        y += groupGap;
    }

    // --- Origin (pivot) ---
    if (g_origin) {
        y = DrawGroupHeader(panelRec, y, "Origin", g_originOpen, 106);
        if (g_originOpen) {
            DrawMultiNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_ORIGIN_X, g_origin->x, "Or X:", labelX,
                [](ScatteredObject* o, float v) { o->GetOriginPtr()->x = v; });
            DrawMultiNumberInput({ inputX, y + rowHeight, inputWidth, inputHeight }, FIELD_ORIGIN_Y, g_origin->y, "Or Y:", labelX,
                [](ScatteredObject* o, float v) { o->GetOriginPtr()->y = v; });
            DrawMultiNumberInput({ inputX, y + rowHeight * 2.0f, inputWidth, inputHeight }, FIELD_ORIGIN_Z, g_origin->z, "Or Z:", labelX,
                [](ScatteredObject* o, float v) { o->GetOriginPtr()->z = v; });
            y += rowHeight * 3.0f;
        }
        y += groupGap;
    }

    // --- Linear Velocity ---
    if (g_selectedObject) {
        y = DrawGroupHeader(panelRec, y, "Linear Velocity", g_linearVelocityOpen, 103);
        if (g_linearVelocityOpen) {
            Vector3 vel = live ? g_simulation->GetBodyVelocity(g_selectedObject) : g_selectedObject->GetVelocity();
            Vector3 velBefore = vel;
            bool velX = DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_VEL_X, vel.x, "Vel X:", labelX);
            bool velY = DrawNumberInput({ inputX, y + rowHeight, inputWidth, inputHeight }, FIELD_VEL_Y, vel.y, "Vel Y:", labelX);
            bool velZ = DrawNumberInput({ inputX, y + rowHeight * 2.0f, inputWidth, inputHeight }, FIELD_VEL_Z, vel.z, "Vel Z:", labelX);
            if (live) g_simulation->SetBodyVelocity(g_selectedObject, vel);
            else g_selectedObject->SetVelocity(vel);
            if ((velX || velY || velZ) && (vel.x != velBefore.x || vel.y != velBefore.y || vel.z != velBefore.z) && g_selection.size() > 1) {
                for (auto* obj : g_selection) {
                    if (!obj || obj == g_selectedObject) continue;
                    if (live) g_simulation->SetBodyVelocity(obj, vel);
                    else obj->SetVelocity(vel);
                }
            }
            y += rowHeight * 3.0f;
        }
        y += groupGap;
    }

    // --- Angular Velocity ---
    if (g_selectedObject) {
        y = DrawGroupHeader(panelRec, y, "Angular Velocity", g_angularVelocityOpen, 104);
        if (g_angularVelocityOpen) {
            Vector3 angVel = live ? g_simulation->GetBodyAngularVelocity(g_selectedObject) : g_selectedObject->GetAngularVelocity();
            Vector3 angVelBefore = angVel;
            bool angX = DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_ANG_X, angVel.x, "Ang X:", labelX);
            bool angY = DrawNumberInput({ inputX, y + rowHeight, inputWidth, inputHeight }, FIELD_ANG_Y, angVel.y, "Ang Y:", labelX);
            bool angZ = DrawNumberInput({ inputX, y + rowHeight * 2.0f, inputWidth, inputHeight }, FIELD_ANG_Z, angVel.z, "Ang Z:", labelX);
            if (live) g_simulation->SetBodyAngularVelocity(g_selectedObject, angVel);
            else g_selectedObject->SetAngularVelocity(angVel);
            if ((angX || angY || angZ) && (angVel.x != angVelBefore.x || angVel.y != angVelBefore.y || angVel.z != angVelBefore.z) && g_selection.size() > 1) {
                for (auto* obj : g_selection) {
                    if (!obj || obj == g_selectedObject) continue;
                    if (live) g_simulation->SetBodyAngularVelocity(obj, angVel);
                    else obj->SetAngularVelocity(angVel);
                }
            }
            y += rowHeight * 3.0f;
        }
        y += groupGap;
    }

    EndScissorMode();
}
}

static void DrawOutputPanel() {
    Rectangle panel = GetOutputPanelBounds();
    if (panel.width <= 0.0f) return;

    constexpr int shown = 6;

    DrawRectangleRec({ panel.x + 3.0f, panel.y + 3.0f, panel.width, panel.height }, theme::SHADOW);
    DrawRectangleRec(panel, Color{ 20, 21, 26, 235 });
    DrawRectangleLinesEx(panel, 1.0f, theme::BORDER_STRONG);
    DrawTextArial("OUTPUT", panel.x + 8.0f, panel.y + 5.0f, 13.0f, theme::ACCENT);
    DrawLine(static_cast<int>(panel.x + 6.0f), static_cast<int>(panel.y + 22.0f), static_cast<int>(panel.x + panel.width - 6.0f), static_cast<int>(panel.y + 22.0f), theme::DIVIDER);

    Rectangle clearBtn = { panel.x + panel.width - 24.0f, panel.y + 3.0f, 18.0f, 18.0f };
    Vector2 mouse = GetMousePosition();
    bool clearHover = CheckCollisionPointRec(mouse, clearBtn);
    float clearT = HoverProgress(22, clearHover);
    DrawRectangleRounded(clearBtn, 0.2f, 4, Mix(theme::BG_WIDGET, theme::DANGER, clearT));
    DrawRectangleLinesEx(clearBtn, 1.0f, Mix(theme::BORDER, theme::DANGER_HOVER, clearT));
    DrawCenteredTextArial("X", clearBtn, 12.0f, clearHover ? theme::TEXT : theme::TEXT_MUTED);
    if (clearHover) MarkHand();
    if (clearHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g_logCount = 0;
        g_logWrite = 0;
    }

    for (int i = 0; i < shown && i < g_logCount; ++i) {
        constexpr float rowH = 18.0f;
        int idx = (g_logWrite - 1 - i + kLogCapacity) % kLogCapacity;
        auto textY = static_cast<float>(panel.y + 28.0f + static_cast<float>(i) * rowH);
        if (textY + rowH > panel.y + panel.height - 4.0f) break;
        DrawTextArial(g_log[idx].text, panel.x + 10.0f, textY, 12.0f, theme::TEXT);
    }
}

// ---------------------------------------------------------------------------
// Terrain tool panel (Cities Skylines 2 / Unity style)
// Top half: terrain tools (raise/lower/smooth/flatten + brush sliders), or
// the procedural Generate tab (region box + seed + hills/mountains/textures)
// Bottom half: terrain properties
// ---------------------------------------------------------------------------
static bool g_terrainGenMode = false; // false = Sculpt tab, true = Generate tab
static float g_terrainPanelScrollY = 0.0f;
static float g_terrainPanelContentHeight = 0.0f; // measured at the end of the previous frame's draw
static bool g_terrainToolDropdownOpen = false;
static Rectangle g_terrainToolDropdownAnchor = { 0 };

static Rectangle GetTerrainPanelBounds() {
    // Bottom-right dock, sharing the right-hand column with the Properties
    // panel: Properties takes the top half, Terrain tools take the bottom
    // half, both at the same width.
    constexpr float topBarHeight = 120.0f;
    constexpr float panelWidth = 240.0f;
    float totalH = static_cast<float>(GetScreenHeight()) - topBarHeight;
    float halfH = totalH * 0.5f;
    float x = static_cast<float>(GetScreenWidth()) - panelWidth;
    float y = topBarHeight + halfH;
    return Rectangle{ x, y, panelWidth, totalH - halfH };
}

static void TerrainSlider(const char* label, float x, float y, float w,
                          float& value, float mn, float mx, const char* fmt, uint64_t key) {
    // Label
    DrawTextArial(label, x, y + 4.0f, 12.0f, theme::TEXT_MUTED);

    Rectangle track = { x + 86.0f, y + 6.0f, w - 86.0f - 56.0f, 12.0f };
    Vector2 mouse = GetMousePosition();

    float t = (mx > mn) ? (value - mn) / (mx - mn) : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    bool hovered = CheckCollisionPointRec(mouse, track);
    static std::unordered_map<uint64_t, bool> dragState;
    bool& dragging = dragState[key];

    if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) dragging = true;
    if (dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        float nt = (mouse.x - track.x) / track.width;
        value = mn + std::clamp(nt, 0.0f, 1.0f) * (mx - mn);
        if (hovered || CheckCollisionPointRec(mouse, Rectangle{ track.x - 6, track.y - 6, track.width + 12, track.height + 12 })) MarkHand();
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) dragging = false;

    // Track
    DrawRectangleRounded(track, 0.5f, 4, theme::BG_INPUT);
    Rectangle fill = { track.x, track.y, track.width * t, track.height };
    if (fill.width >= 2.0f) DrawRectangleRounded(fill, 0.5f, 4, Fade(theme::ACCENT, 0.55f));
    // Knob
    Vector2 knob = { track.x + track.width * t, track.y + track.height * 0.5f };
    DrawCircleV(knob, hovered ? 8.0f : 7.0f, hovered ? theme::ACCENT_HOVER : theme::ACCENT);

    char buf[48];
    snprintf(buf, sizeof(buf), fmt, value);
    float tw2 = MeasureTextArial(buf, 12.0f);
    DrawTextArial(buf, x + w - tw2 - 4.0f, y + 5.0f, 12.0f, theme::TEXT);
}

static bool TerrainCheckbox(float x, float y, const char* label, bool value, uint64_t key) {
    Vector2 mouse = GetMousePosition();
    Rectangle checkRect = { x, y, 18.0f, 18.0f };
    bool hovered = CheckCollisionPointRec(mouse, checkRect);
    float t = HoverProgress(key, hovered);
    Color bg = value ? theme::SUCCESS : theme::BG_WIDGET;
    DrawRectangleRounded(checkRect, 0.2f, 4, Mix(bg, theme::ACCENT_HOVER, t));
    DrawRectangleLinesEx(checkRect, hovered ? 2.0f : 1.0f, hovered ? theme::ACCENT : theme::BORDER);
    if (value) {
        Vector2 c1 = { checkRect.x + 4.0f, checkRect.y + checkRect.height * 0.5f };
        Vector2 c2 = { checkRect.x + checkRect.width * 0.5f, checkRect.y + checkRect.height - 4.0f };
        Vector2 c3 = { checkRect.x + checkRect.width - 4.0f, checkRect.y + 4.0f };
        DrawLineEx(c1, c2, 2.0f, COL_WHITE);
        DrawLineEx(c2, c3, 2.0f, COL_WHITE);
    }
    if (hovered) MarkHand();
    DrawTextArial(label, x + 26.0f, y + 3.0f, 13.0f, theme::TEXT_MUTED);
    return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static void DrawToolIcon(int kind, Rectangle ir, Color col) {
    Vector2 c = { ir.x + ir.width * 0.5f, ir.y + ir.height * 0.5f };
    switch (kind) {
        case 0: { // Raise: mountain with up arrow
            Vector2 a = { c.x, ir.y + 3.0f };
            Vector2 b = { ir.x + 4.0f, ir.y + ir.height - 3.0f };
            Vector2 d = { ir.x + ir.width - 4.0f, ir.y + ir.height - 3.0f };
            DrawTriangle(a, b, d, col);
            DrawLineEx({ c.x, ir.y + 1.0f }, { c.x, ir.y + 8.0f }, 2.0f, col);   // arrow shaft
            DrawLineEx({ c.x - 3.0f, ir.y + 4.0f }, { c.x, ir.y + 1.0f }, 2.0f, col);
            DrawLineEx({ c.x + 3.0f, ir.y + 4.0f }, { c.x, ir.y + 1.0f }, 2.0f, col);
            break;
        }
        case 1: { // Lower: inverted mountain
            Vector2 a = { c.x, ir.y + ir.height - 3.0f };
            Vector2 b = { ir.x + 4.0f, ir.y + 3.0f };
            Vector2 d = { ir.x + ir.width - 4.0f, ir.y + 3.0f };
            DrawTriangle(a, b, d, col);
            break;
        }
        case 2: { // Smooth: soft wave
            Vector2 p[7] = {
                { ir.x + 2.0f, c.y + 4.0f },
                { ir.x + ir.width * 0.22f, c.y - 3.0f },
                { ir.x + ir.width * 0.40f, c.y - 5.0f },
                { c.x, c.y },
                { ir.x + ir.width * 0.60f, c.y + 5.0f },
                { ir.x + ir.width * 0.78f, c.y + 3.0f },
                { ir.x + ir.width - 2.0f, c.y - 4.0f },
            };
            for (int i = 0; i < 6; i++) DrawLineEx(p[i], p[i + 1], 2.0f, col);
            break;
        }
        case 3: { // Flatten: plateau
            Vector2 p[6] = {
                { ir.x + 2.0f, ir.y + ir.height - 4.0f },
                { ir.x + ir.width * 0.25f, ir.y + 6.0f },
                { ir.x + ir.width * 0.45f, ir.y + 6.0f },
                { ir.x + ir.width * 0.55f, ir.y + 6.0f },
                { ir.x + ir.width * 0.75f, ir.y + 6.0f },
                { ir.x + ir.width - 2.0f, ir.y + ir.height - 4.0f },
            };
            for (int i = 0; i < 5; i++) DrawLineEx(p[i], p[i + 1], 2.0f, col);
            DrawLineEx({ ir.x + 2.0f, ir.y + ir.height - 4.0f }, { ir.x + ir.width - 2.0f, ir.y + ir.height - 4.0f }, 1.0f, Fade(col, 0.5f));
            break;
        }
        case 4: { // Paint: paintbrush
            // Brush handle
            DrawLineEx({ c.x - 7.0f, c.y + 8.0f }, { c.x + 1.0f, c.y - 1.0f }, 3.0f, col);
            // Brush ferrule
            DrawRectangleRec({ c.x - 2.0f, c.y - 5.0f, 8.0f, 6.0f }, col);
            // Brush tip
            DrawLineEx({ c.x + 2.0f, c.y - 5.0f }, { c.x + 2.0f, c.y - 10.0f }, 2.0f, col);
            DrawLineEx({ c.x + 5.0f, c.y - 5.0f }, { c.x + 5.0f, c.y - 8.0f }, 2.0f, col);
            break;
        }
        case 5: { // None: cursor/arrow
            DrawLineEx({ c.x - 8.0f, c.y + 8.0f }, { c.x, c.y - 8.0f }, 2.5f, col);
            DrawLineEx({ c.x, c.y - 8.0f }, { c.x + 8.0f, c.y + 8.0f }, 2.5f, col);
            DrawLineEx({ c.x - 4.0f, c.y }, { c.x + 4.0f, c.y }, 2.0f, col);
            break;
        }
    }
}

// Draws the real asset icon for a terrain sculpt tool (kind matches
// BasicTerrain::Tool / TerrainToolIconType ordering), tinted with col.
// Falls back to the hand-drawn DrawToolIcon() vector glyph if the asset
// failed to load (e.g. missing from assets/EditorIcons/).
static void DrawTerrainToolIcon(int kind, Rectangle ir, Color col) {
    if (kind < 0 || kind >= TTOOLICON_COUNT || g_terrainToolIcons[kind].id == 0) {
        DrawToolIcon(kind, ir, col);
        return;
    }
    Texture2D& tex = g_terrainToolIcons[kind];
    Rectangle src = { 0.0f, 0.0f, static_cast<float>(tex.width), static_cast<float>(tex.height) };
    DrawTexturePro(tex, src, ir, { 0.0f, 0.0f }, 0.0f, col);
}

static void DrawTerrainToolPanel() {
    Rectangle panelRec = GetTerrainPanelBounds();
    if (panelRec.width <= 0.0f) return;
    BasicTerrain* t = BasicTerrain::GetActive();
    // The panel is always visible; brush settings are shared by every terrain
    // so tools can be picked before any terrain exists.
    BasicTerrain::Brush& br = BasicTerrain::GetBrush();

    const float pad = 14.0f;

    // Shadow + body
    DrawRectangleRec({ panelRec.x + 4.0f, panelRec.y + 4.0f, panelRec.width, panelRec.height }, theme::SHADOW);
    DrawRectangleRounded(panelRec, 0.05f, 6, theme::BG_PANEL);
    DrawRectangleLinesEx(panelRec, 2.0f, theme::BORDER);

    // Title strip
    DrawRectangleRec({ panelRec.x + 1.0f, panelRec.y + 1.0f, panelRec.width - 2.0f, 33.0f }, theme::BG_TITLE);
    DrawLine(static_cast<int>(panelRec.x), static_cast<int>(panelRec.y + 34.0f),
             static_cast<int>(panelRec.x + panelRec.width), static_cast<int>(panelRec.y + 34.0f), theme::DIVIDER);
    DrawTextArial("TERRAIN", panelRec.x + pad, panelRec.y + 10.0f, 14.0f, theme::ACCENT);

    Vector2 mouse = GetMousePosition();

    // Content area (below the title strip) used for scissor clipping +
    // scrolling, same pattern as the Properties panel. Content height is
    // measured from the previous frame's pass (see the cleanup lambda
    // below) since the true height depends on which tab/tools are active.
    Rectangle contentRec = { panelRec.x + 2.0f, panelRec.y + 36.0f, panelRec.width - 4.0f, panelRec.height - 38.0f };
    if (CheckCollisionPointRec(mouse, panelRec)) {
        g_terrainPanelScrollY -= GetMouseWheelMove() * 24.0f;
    }
    float maxScroll = std::max(0.0f, g_terrainPanelContentHeight - contentRec.height);
    g_terrainPanelScrollY = std::clamp(g_terrainPanelScrollY, 0.0f, maxScroll);

    BeginScissorMode(static_cast<int>(contentRec.x), static_cast<int>(contentRec.y),
                      static_cast<int>(contentRec.width), static_cast<int>(contentRec.height));

    float innerW = panelRec.width - pad * 2.0f;
    float x = panelRec.x + pad;
    float contentTopY = panelRec.y + 44.0f;
    float y = contentTopY - g_terrainPanelScrollY;

    // Ends the scissor region, draws the tool dropdown's expanded list (if
    // open) unclipped on top of everything, and records this frame's real
    // content height for next frame's scroll clamping. Called before every
    // exit path (the early "no terrain" return and the natural end).
    auto finishPanel = [&]() {
        EndScissorMode();
        g_terrainPanelContentHeight = (y - contentTopY) + g_terrainPanelScrollY;

        if (g_terrainToolDropdownOpen) {
            constexpr int numTools = 6;
            const char* toolNames[numTools] = { "Raise", "Lower", "Smooth", "Flatten", "Paint", "None" };
            Rectangle anchor = g_terrainToolDropdownAnchor;
            Rectangle listRec = { anchor.x, anchor.y + anchor.height + 2.0f, anchor.width,
                                   static_cast<float>(numTools) * 26.0f + 8.0f };

            DrawRectangleRec({ listRec.x + 3.0f, listRec.y + 3.0f, listRec.width, listRec.height }, theme::SHADOW);
            DrawRectangleRounded(listRec, 0.08f, 4, theme::BG_MENU);
            DrawRectangleLinesEx(listRec, 1.0f, theme::BORDER_STRONG);

            int activeTool = static_cast<int>(br.tool);
            for (int i = 0; i < numTools; ++i) {
                Rectangle itemRec = { listRec.x + 4.0f, listRec.y + 4.0f + static_cast<float>(i) * 26.0f, listRec.width - 8.0f, 22.0f };
                bool hovered = CheckCollisionPointRec(mouse, itemRec);
                bool selected = (i == activeTool);
                if (hovered || selected) {
                    DrawRectangleRounded(itemRec, 0.15f, 4, hovered ? theme::ACCENT_HOVER : theme::ACCENT_SOFT);
                }
                Rectangle iconRec = { itemRec.x + 6.0f, itemRec.y + 3.0f, 16.0f, 16.0f };
                DrawTerrainToolIcon(i, iconRec, selected ? theme::ACCENT : theme::TEXT_MUTED);
                DrawTextArial(toolNames[i], itemRec.x + 28.0f, itemRec.y + 4.0f, 12.0f,
                              selected ? theme::TEXT : theme::TEXT_MUTED);
                if (hovered) MarkHand();
                if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    auto newTool = static_cast<BasicTerrain::Tool>(i);
                    if (newTool == BasicTerrain::Tool::Paint && br.tool != BasicTerrain::Tool::Paint) {
                        int lc = BasicTerrain::GetLayerCount();
                        if (lc > 1) br.paintLayer = 1;
                        else if (lc > 0) br.paintLayer = 0;
                    }
                    br.tool = newTool;
                    g_terrainToolDropdownOpen = false;
                }
            }

            if (IsKeyPressed(KEY_ESCAPE)) g_terrainToolDropdownOpen = false;
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)
                && !CheckCollisionPointRec(mouse, listRec) && !CheckCollisionPointRec(mouse, anchor)) {
                g_terrainToolDropdownOpen = false;
            }
        }
    };


    // ================= TOP HALF: TERRAIN TOOLS =================
    DrawTextArial("TERRAIN", x, y, 11.0f, theme::TEXT_DIM);
    DrawLine(static_cast<int>(x + 52.0f), static_cast<int>(y + 7.0f),
             static_cast<int>(x + innerW), static_cast<int>(y + 7.0f), theme::DIVIDER);
    y += 18.0f;

    // Sculpt / Generate tabs
    {
        const char* tabNames[2] = { "Sculpt", "Generate" };
        float tabGap = 6.0f;
        float tabW = (innerW - tabGap) * 0.5f;
        float tabH = 26.0f;
        for (int i = 0; i < 2; i++) {
            Rectangle tr = { x + i * (tabW + tabGap), y, tabW, tabH };
            bool sel = (g_terrainGenMode == (i == 1));
            bool hov = CheckCollisionPointRec(mouse, tr);
            float ht3 = HoverProgress(static_cast<uint64_t>(9600u + i), hov);
            Color bg = sel ? theme::ACCENT_SOFT : theme::BG_WIDGET;
            bg = Mix(bg, theme::BG_WIDGET_HOVER, ht3 * (sel ? 0.3f : 1.0f));
            DrawRectangleRounded(tr, 0.2f, 4, bg);
            DrawRectangleLinesEx(tr, sel ? 2.0f : 1.0f, sel ? theme::ACCENT : theme::BORDER);
            float tw3 = MeasureTextArial(tabNames[i], 12.0f);
            DrawTextArial(tabNames[i], tr.x + (tr.width - tw3) * 0.5f, tr.y + 6.0f, 12.0f,
                          sel ? theme::TEXT : theme::TEXT_MUTED);
            if (hov) MarkHand();
            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_terrainGenMode = (i == 1);
        }
        y += tabH + 10.0f;
    }

    // The region-box preview in the viewport only makes sense while the
    // Generate tab is actually open for this terrain.
    if (t) t->showGenBox = g_terrainGenMode;

    if (g_terrainGenMode) {
        // ================= GENERATE TAB =================
        if (!t) {
            DrawTextArial("Select or spawn a terrain first.", x, y + 4.0f, 12.0f, theme::TEXT_MUTED);
            y += 28.0f;
        } else {
            // Snap the box to the terrain footprint once, the first time this
            // tab is opened for this terrain; afterward the user's own edits
            // (via the sliders below) are preserved across frames/reopens.
            if (!t->genBoxInitialized) {
                t->genBoxPos = { t->position.x, t->position.y + t->GetMaxHeight() * 0.15f, t->position.z };
                float fw = t->GetWidth() * t->GetScale();
                float fd = t->GetDepth() * t->GetScale();
                t->genBoxSize = { fw * 0.5f, std::max(20.0f, (t->GetMaxHeight() - t->GetMinHeight()) * 0.4f), fd * 0.5f };
                t->genBoxInitialized = true;
            }

            DrawTextArial("SEED", x, y, 11.0f, theme::TEXT_DIM);
            y += 14.0f;
            float seedF = (float)t->genSeed;
            TerrainSlider("Seed", x, y, innerW - 78.0f, seedF, 0.0f, 999999.0f, "%.0f", 9610u);
            t->genSeed = (int)seedF;
            Rectangle randBtn = { x + innerW - 70.0f, y - 4.0f, 70.0f, 24.0f };
            bool rndHov = CheckCollisionPointRec(mouse, randBtn);
            DrawRectangleRounded(randBtn, 0.2f, 4, rndHov ? theme::BG_WIDGET_HOVER : theme::BG_WIDGET);
            DrawRectangleLinesEx(randBtn, rndHov ? 2.0f : 1.0f, rndHov ? theme::ACCENT : theme::BORDER);
            float rndW = MeasureTextArial("Random", 11.0f);
            DrawTextArial("Random", randBtn.x + (randBtn.width - rndW) * 0.5f, randBtn.y + 6.0f, 11.0f,
                          rndHov ? theme::TEXT : theme::TEXT_MUTED);
            if (rndHov) MarkHand();
            if (rndHov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) t->genSeed = GetRandomValue(0, 999999);
            y += 34.0f;

            DrawTextArial("INCLUDE", x, y, 11.0f, theme::TEXT_DIM);
            y += 14.0f;
            if (TerrainCheckbox(x, y, "Hills", t->genHills, 9611u)) t->genHills = !t->genHills;
            y += 24.0f;
            if (TerrainCheckbox(x, y, "Mountains", t->genMountains, 9612u)) t->genMountains = !t->genMountains;
            y += 24.0f;
            if (TerrainCheckbox(x, y, "Extra Textures", t->genTextures, 9613u)) t->genTextures = !t->genTextures;
            y += 30.0f;

            DrawTextArial("REGION BOX - POSITION", x, y, 11.0f, theme::TEXT_DIM);
            y += 14.0f;
            float posRangeX = t->GetWidth() * t->GetScale();
            float posRangeZ = t->GetDepth() * t->GetScale();
            TerrainSlider("X", x, y, innerW, t->genBoxPos.x, t->position.x - posRangeX, t->position.x + posRangeX, "%.0f m", 9620u); y += 30.0f;
            TerrainSlider("Y", x, y, innerW, t->genBoxPos.y, t->GetMinHeight(), t->GetMaxHeight(), "%.0f m", 9621u); y += 30.0f;
            TerrainSlider("Z", x, y, innerW, t->genBoxPos.z, t->position.z - posRangeZ, t->position.z + posRangeZ, "%.0f m", 9622u); y += 32.0f;

            DrawTextArial("REGION BOX - SIZE", x, y, 11.0f, theme::TEXT_DIM);
            y += 14.0f;
            float maxVSize = std::max(20.0f, t->GetMaxHeight() - t->GetMinHeight());
            TerrainSlider("Width",  x, y, innerW, t->genBoxSize.x, 4.0f, posRangeX * 2.0f, "%.0f m", 9623u); y += 30.0f;
            TerrainSlider("Height", x, y, innerW, t->genBoxSize.y, 4.0f, maxVSize, "%.0f m", 9624u); y += 30.0f;
            TerrainSlider("Depth",  x, y, innerW, t->genBoxSize.z, 4.0f, posRangeZ * 2.0f, "%.0f m", 9625u); y += 32.0f;

            // Generate button
            Rectangle genBtn = { x, y, innerW, 34.0f };
            bool gHov = CheckCollisionPointRec(mouse, genBtn);
            float gt = HoverProgress(9630u, gHov);
            DrawRectangleRounded(genBtn, 0.15f, 4, Mix(theme::ACCENT_SOFT, theme::ACCENT_HOVER, gt));
            DrawRectangleLinesEx(genBtn, gHov ? 2.0f : 1.0f, theme::ACCENT);
            const char* genLabel = "Generate";
            float glw = MeasureTextArial(genLabel, 13.0f);
            DrawTextArial(genLabel, genBtn.x + (genBtn.width - glw) * 0.5f, genBtn.y + 9.0f, 13.0f, theme::TEXT);
            if (gHov) MarkHand();
            if (gHov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                t->GenerateTerrainInBox();
            }
            y += 34.0f + 10.0f;
        }

        // Divider between halves
        DrawLine(static_cast<int>(panelRec.x + pad), static_cast<int>(y),
                 static_cast<int>(panelRec.x + panelRec.width - pad), static_cast<int>(y), theme::DIVIDER);
        y += 12.0f;
    } else {
    // Tool selector: a full-width dropdown (Raise/Lower/Smooth/Flatten/Paint/None)
    // instead of a 6-button icon grid, to save vertical space now that this
    // panel only gets half the column height.
    {
        constexpr const char* toolNames[6] = { "Raise", "Lower", "Smooth", "Flatten", "Paint", "None" };
        int activeTool = static_cast<int>(br.tool);
        Rectangle box = { x, y, innerW, 30.0f };
        bool hov = CheckCollisionPointRec(mouse, box);
        float ht = HoverProgress(9100u, hov);
        Color bg = Mix(theme::BG_WIDGET, theme::BG_WIDGET_HOVER, ht);
        DrawRectangleRounded(box, 0.15f, 4, bg);
        DrawRectangleLinesEx(box, g_terrainToolDropdownOpen ? 2.0f : 1.0f,
                              g_terrainToolDropdownOpen ? theme::ACCENT : theme::BORDER);

        Rectangle iconRec = { box.x + 8.0f, box.y + 7.0f, 16.0f, 16.0f };
        DrawTerrainToolIcon(activeTool, iconRec, theme::ACCENT);
        DrawTextArial(toolNames[activeTool], box.x + 32.0f, box.y + 8.0f, 13.0f, theme::TEXT);

        // Chevron
        float cx = box.x + box.width - 18.0f;
        float cy = box.y + box.height * 0.5f;
        Color chevCol = hov ? theme::TEXT : theme::TEXT_MUTED;
        if (g_terrainToolDropdownOpen) {
            DrawLineEx({ cx - 5.0f, cy + 2.0f }, { cx, cy - 3.0f }, 2.0f, chevCol);
            DrawLineEx({ cx, cy - 3.0f }, { cx + 5.0f, cy + 2.0f }, 2.0f, chevCol);
        } else {
            DrawLineEx({ cx - 5.0f, cy - 2.0f }, { cx, cy + 3.0f }, 2.0f, chevCol);
            DrawLineEx({ cx, cy + 3.0f }, { cx + 5.0f, cy - 2.0f }, 2.0f, chevCol);
        }

        if (hov) MarkHand();
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            g_terrainToolDropdownOpen = !g_terrainToolDropdownOpen;
        }
        // Anchor is captured in screen space (post-scroll), matching where
        // the closed box is actually drawn this frame; the expanded list is
        // rendered from this rect after the scissor region ends (see
        // finishPanel above).
        g_terrainToolDropdownAnchor = box;

        y += box.height + 10.0f;
    }

    // Brush shape: Circle | Square
    DrawTextArial("Shape", x, y + 7.0f, 12.0f, theme::TEXT_MUTED);
    for (int i = 0; i < 2; i++) {
        Rectangle sr = { x + 86.0f + i * 78.0f, y, 70.0f, 26.0f };
        bool sel = (static_cast<int>(br.shape) == i);
        bool hov = CheckCollisionPointRec(mouse, sr);
        float st2 = HoverProgress(static_cast<uint64_t>(9400u + i), hov);
        Color bg = sel ? theme::ACCENT_SOFT : theme::BG_WIDGET;
        bg = Mix(bg, theme::BG_WIDGET_HOVER, st2 * (sel ? 0.3f : 1.0f));
        DrawRectangleRounded(sr, 0.25f, 4, bg);
        DrawRectangleLinesEx(sr, sel ? 2.0f : 1.0f, sel ? theme::ACCENT : theme::BORDER);
        const char* nm = (i == 0) ? "Circle" : "Square";
        float nw = MeasureTextArial(nm, 12.0f);
        DrawTextArial(nm, sr.x + (sr.width - nw) * 0.5f, sr.y + 6.0f, 12.0f,
                      sel ? theme::TEXT : theme::TEXT_MUTED);
        if (hov) MarkHand();
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            br.shape = static_cast<BasicTerrain::Shape>(i);
        }
    }
    y += 34.0f;

    // Brush sliders
    TerrainSlider("Size",     x, y, innerW, br.radius,       4.0f, 200.0f, "%.0f m", 9201u); y += 32.0f;
    TerrainSlider("Strength", x, y, innerW, br.strength,     2.0f, 200.0f, "%.0f",   9202u); y += 32.0f;
    TerrainSlider("Hardness", x, y, innerW, br.hardness,     0.0f, 1.0f,   "%.2f",   9203u); y += 32.0f;

    // Target height only matters when flattening (row always reserved)
    if (t && br.tool == BasicTerrain::Tool::Flatten) {
        TerrainSlider("Target H", x, y, innerW, br.targetHeight, t->GetMinHeight(), t->GetMaxHeight(), "%.0f m", 9204u);
    }
    y += 32.0f;

    // Shortcut hint
    if (br.tool == BasicTerrain::Tool::Paint) {
        DrawTextArial("Click: stamp   Drag: paint   [ ] size   Ctrl erase", x, y + 2.0f, 10.0f, theme::TEXT_DIM);
    } else {
        DrawTextArial("Click: stamp   Drag: paint   [ ] size   Ctrl invert", x, y + 2.0f, 10.0f, theme::TEXT_DIM);
    }
    y += 24.0f;

    // ================= PAINT LAYERS (when Paint tool active) =================
    if (br.tool == BasicTerrain::Tool::Paint) {
        DrawLine(static_cast<int>(panelRec.x + pad), static_cast<int>(y),
                 static_cast<int>(panelRec.x + panelRec.width - pad), static_cast<int>(y), theme::DIVIDER);
        y += 12.0f;

        DrawTextArial("LAYERS", x, y, 11.0f, theme::TEXT_DIM);
        DrawLine(static_cast<int>(x + 52.0f), static_cast<int>(y + 7.0f),
                 static_cast<int>(x + innerW), static_cast<int>(y + 7.0f), theme::DIVIDER);
        y += 18.0f;

        int layerCount = BasicTerrain::GetLayerCount();
        Texture2D* layerTexs = BasicTerrain::GetLayerTextures();
        const std::string* layerNames = BasicTerrain::GetLayerNames();

        if (layerCount == 0) {
            DrawTextArial("No textures in TerrainTextures/", x, y, 11.0f, theme::TEXT_MUTED);
            y += 18.0f;
        } else {
            float thumbSize = 40.0f;
            float btnW = (innerW - 5.0f * 3.0f) / 4.0f;
            for (int i = 0; i < 4; i++) {
                Rectangle lr = { x + i * (btnW + 5.0f), y, btnW, thumbSize + 20.0f };
                bool active = (br.paintLayer == i);
                bool available = (i < layerCount);
                bool hov = CheckCollisionPointRec(mouse, lr) && available;
                float ht2 = HoverProgress(static_cast<uint64_t>(9500u + i), hov);

                Color bg = active ? theme::ACCENT_SOFT : theme::BG_WIDGET;
                bg = Mix(bg, theme::BG_WIDGET_HOVER, ht2 * (active ? 0.3f : 1.0f));
                DrawRectangleRounded(lr, 0.12f, 4, bg);
                DrawRectangleLinesEx(lr, active ? 2.0f : 1.0f, active ? theme::ACCENT : theme::BORDER);

                if (available && layerTexs[i].id > 0) {
                    Rectangle src = { 0, 0, (float)layerTexs[i].width, (float)layerTexs[i].height };
                    Rectangle dst = { lr.x + 2.0f, lr.y + 2.0f, thumbSize - 4.0f, thumbSize - 4.0f };
                    DrawTexturePro(layerTexs[i], src, dst, {0,0}, 0.0f, WHITE);
                } else if (available) {
                    DrawRectangleRec({lr.x + 2.0f, lr.y + 2.0f, thumbSize - 4.0f, thumbSize - 4.0f},
                        Fade(theme::ACCENT, 0.3f));
                } else {
                    DrawRectangleRec({lr.x + 2.0f, lr.y + 2.0f, thumbSize - 4.0f, thumbSize - 4.0f},
                        theme::BG_INPUT);
                }

                const char* label = available ? layerNames[i].c_str() : "Empty";
                float lw = MeasureTextArial(label, 9.0f);
                DrawTextArial(label, lr.x + (lr.width - lw) * 0.5f, lr.y + thumbSize - 2.0f, 9.0f,
                    available ? (active ? theme::TEXT : theme::TEXT_MUTED) : theme::TEXT_DIM);

                if (hov) MarkHand();
                if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    br.paintLayer = i;
                }
            }
            y += thumbSize + 22.0f;

            // Erase toggle
            Rectangle eraserRect = { x, y, innerW, 26.0f };
            bool eHov = CheckCollisionPointRec(mouse, eraserRect);
            float et = HoverProgress(9510u, eHov);
            Color eBg = br.paintErase ? theme::ACCENT_SOFT : theme::BG_WIDGET;
            eBg = Mix(eBg, theme::BG_WIDGET_HOVER, et * (br.paintErase ? 0.3f : 1.0f));
            DrawRectangleRounded(eraserRect, 0.18f, 4, eBg);
            DrawRectangleLinesEx(eraserRect, br.paintErase ? 2.0f : 1.0f,
                br.paintErase ? theme::ACCENT : theme::BORDER);
            const char* eLabel = br.paintErase ? "Erase Mode: ON" : "Erase Mode: OFF";
            float ew = MeasureTextArial(eLabel, 12.0f);
            DrawTextArial(eLabel, eraserRect.x + (eraserRect.width - ew) * 0.5f, eraserRect.y + 6.0f, 12.0f,
                br.paintErase ? theme::ACCENT : theme::TEXT_MUTED);
            if (eHov) MarkHand();
            if (eHov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                br.paintErase = !br.paintErase;
            }
            y += 32.0f;
        }
    }

    // Divider between halves
    DrawLine(static_cast<int>(panelRec.x + pad), static_cast<int>(y),
             static_cast<int>(panelRec.x + panelRec.width - pad), static_cast<int>(y), theme::DIVIDER);
    y += 12.0f;
    } // end Sculpt tab (g_terrainGenMode == false)

    // ================= BOTTOM HALF: PROPERTIES =================
    DrawTextArial("PROPERTIES", x, y, 11.0f, theme::TEXT_DIM);
    DrawLine(static_cast<int>(x + 74.0f), static_cast<int>(y + 7.0f),
             static_cast<int>(x + innerW), static_cast<int>(y + 7.0f), theme::DIVIDER);
    y += 20.0f;

    if (!t) {
        DrawTextArial("No terrain selected.", x, y + 14.0f, 13.0f, theme::TEXT_MUTED);
        DrawTextArial("Pick tools above, then click any terrain to edit.",
                      x, y + 36.0f, 11.0f, theme::TEXT_DIM);
        finishPanel();
        return;
    }

    auto infoRow = [&](const char* k, char* vbuf, size_t vsz) {
        DrawTextArial(k, x, y, 12.0f, theme::TEXT_MUTED);
        float vw = MeasureTextArial(vbuf, 12.0f);
        DrawTextArial(vbuf, x + innerW - vw, y, 12.0f, theme::TEXT);
        (void)vsz;
        y += 21.0f;
    };

    char buf[96];
    snprintf(buf, sizeof(buf), "%d x %d", t->GetWidth(), t->GetDepth());
    infoRow("Grid", buf, sizeof(buf));

    snprintf(buf, sizeof(buf), "%.0f x %.0f m", t->GetWidth() * t->GetScale(), t->GetDepth() * t->GetScale());
    infoRow("World Size", buf, sizeof(buf));

    snprintf(buf, sizeof(buf), "%.0f, 0, %.0f", t->position.x, t->position.z);
    infoRow("Position", buf, sizeof(buf));

    snprintf(buf, sizeof(buf), "%.0f m .. %.0f m", t->GetMinHeight(), t->GetMaxHeight());
    infoRow("Height Range", buf, sizeof(buf));
    y += 4.0f;

    // Wireframe toggle
    if (TerrainCheckbox(x, y, "Show Wireframe", t->showWireframe, 9301u)) {
        t->showWireframe = !t->showWireframe;
    }
    y += 28.0f;

    // Reset flat button
    Rectangle resetBtn = { x, y, 110.0f, 26.0f };
    bool rHov = CheckCollisionPointRec(mouse, resetBtn);
    float rt = HoverProgress(9302u, rHov);
    DrawRectangleRounded(resetBtn, 0.18f, 4, Mix(theme::BG_WIDGET, theme::BG_WIDGET_PRESSED, rt));
    DrawRectangleLinesEx(resetBtn, rHov ? 2.0f : 1.0f, rHov ? theme::DANGER : theme::BORDER);
    float rbw = MeasureTextArial("Reset Flat", 12.0f);
    DrawTextArial("Reset Flat", resetBtn.x + (resetBtn.width - rbw) * 0.5f, resetBtn.y + 6.0f, 12.0f,
                  rHov ? theme::DANGER_HOVER : theme::TEXT_MUTED);
    if (rHov) MarkHand();
    if (rHov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        t->GenerateFlat(0.0f);
    }

    // Hint about selection state
    DrawTextArial(t->editActive ? "Editing: LMB drag sculpt" : "Click terrain to edit",
                  x + 126.0f, y + 6.0f, 11.0f,
                  t->editActive ? theme::SUCCESS : theme::TEXT_DIM);

    finishPanel();
}


void Draw() {
    g_clickConsumedThisFrame = false;
    BeginCursorPass();

    if (g_playActive) {
        const char* msg = "PLAYING";
        float msgWidth = MeasureTextArial(msg, 20.0f);
        DrawTextArial(msg, (static_cast<float>(GetScreenWidth()) - msgWidth) * 0.5f, 15.0f, 20.0f, theme::DANGER);
    }
    // Top bar + explorer panel now render through Dear ImGui (see
    // DrawImGuiTopBar/DrawImGuiExplorer called from DrawImGuiFrame()).

    // Handle preset texture dropdown list click BEFORE properties panel
    // so the click is consumed and doesn't toggle Position/Size/etc. groups
    if (g_presetDropdownListPending && g_presetDropdownOpen) {
        float itemHeight = 24.0f;
        Rectangle listRect = g_presetDropdownListRect;
        const auto& items = g_presetDropdownListItems;
        const auto& presets = g_presetDropdownListPresets;
        const std::string& projectDir = g_presetDropdownListProjectDir;

        Vector2 mousePos = GetMousePosition();
        int visibleCount = (int)items.size();
        bool clickedItem = false;

        for (int i = 0; i < visibleCount; ++i) {
            Rectangle itemRect = { listRect.x, listRect.y + i * itemHeight,
                                 listRect.width, itemHeight };
            bool itemHovered = CheckCollisionPointRec(mousePos, itemRect);

            if (itemHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                g_presetDropdownSelected = i;
                g_presetDropdownOpen = false;
                g_presetDropdownListPending = false;
                clickedItem = true;
                g_clickConsumedThisFrame = true;

                // Apply texture to all selected objects
                if (i == 0) { // "None"
                    const auto& proj = ::project::GetCurrentProject();
                    for (auto* obj : g_selection) {
                        if (obj) {
                            obj->ClearTexture(proj.path);
                        }
                    }
                } else {
                    std::string presetPath = presets[i - 1];
                    // Ensure the preset texture is copied to the project's PresetTextures folder
                    // so the path is portable and saved correctly with the scene
                    const auto& proj = ::project::GetCurrentProject();
                    std::string projectRelativePath = textureManager::EnsurePresetTextureInProject(presetPath, proj.path);
                    for (auto* obj : g_selection) {
                        if (obj) {
                            obj->SetTexturePath(projectRelativePath, proj.path);
                        }
                    }
                }
            }
        }

        // Close if clicked outside list and dropdown
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            !CheckCollisionPointRec(mousePos, g_presetDropdownHeaderRect) &&
            !CheckCollisionPointRec(mousePos, listRect)) {
            g_presetDropdownOpen = false;
            g_presetDropdownListPending = false;
            g_clickConsumedThisFrame = true;
        }

        if (clickedItem) {
            // Click was consumed by dropdown list, skip properties panel click handling this frame
            // by setting a flag that DrawGroupHeader will check
        }
    }

    DrawPropertiesPanel();

    // Terrain tool panel (when a BasicTerrain is active)
    DrawTerrainToolPanel();

    // Docked console + output panel
    console::Draw();
    DrawOutputPanel();

    // Terrain editor UI
    DrawTerrainEditorUI();

    // Floating popups (topmost within the 2D pass, before the script editor)
    DrawContextMenu();
    DrawColorPickerPopupWindow();
    DrawCollisionPopup();

    DrawScriptWarningDialog();

    // Preset texture dropdown list (drawn after panels so it's on top)
    if (g_presetDropdownListPending && g_presetDropdownOpen) {
        // Defensive: make sure nothing upstream (e.g. console::Draw()) left a
        // scissor rect active, which would silently slice this list at its edge.
        EndScissorMode();

        float itemHeight = 24.0f;
        Rectangle listRect = g_presetDropdownListRect;
        const auto& items = g_presetDropdownListItems;
        const auto& presets = g_presetDropdownListPresets;
        const std::string& projectDir = g_presetDropdownListProjectDir;

        // Draw list background
        DrawRectangleRounded(listRect, 0.2f, 4, theme::BG_PANEL);
        DrawRectangleLinesEx(listRect, 1.0f, theme::BORDER);

        // Draw items
        Vector2 mousePos = GetMousePosition();
        int visibleCount = (int)items.size();
        for (int i = 0; i < visibleCount; ++i) {
            Rectangle itemRect = { listRect.x, listRect.y + i * itemHeight,
                                 listRect.width, itemHeight };
            bool itemHovered = CheckCollisionPointRec(mousePos, itemRect);

            if (itemHovered) {
                DrawRectangleRounded(itemRect, 0.2f, 4, theme::BG_INPUT_HOVER);
            }

            float itemTextY = itemRect.y + (itemRect.height - 13.0f) * 0.5f;
            DrawTextArial(items[i].c_str(), itemRect.x + 8.0f, itemTextY, 13.0f,
                itemHovered ? theme::ACCENT : theme::TEXT);
        }

        // Close if clicked outside list and dropdown (handled in early click processing)
    }

    EndCursorPass();
}

// ============================================================================
// Terrain Editor Functions
// ============================================================================

void InitTerrainEditor() {
    terrain::InitTerrainEditor();
}

void ShutdownTerrainEditor() {
    terrain::ShutdownTerrainEditor();
}

void UpdateTerrainEditor(Engine& engine, CameraController* cameraCtrl, phys::Simulation* physicsSim) {
    terrain::UpdateTerrainEditor(engine, cameraCtrl, physicsSim);
}

void DrawTerrainEditorUI() {
    terrain::DrawTerrainEditorUI();
}

bool IsTerrainEditorActive() {
    return terrain::IsTerrainEditorActive();
}

void SetTerrainEditorMode(TransformTool mode) {
    terrain::SetTerrainEditorMode(static_cast<int>(mode));
}

void HandleTerrainSelection(terrain::Terrain* terrainObj, bool selected) {
    terrain::HandleTerrainSelection(terrainObj, selected);
}

void DrawTerrainBrushPreview(const Camera3D& camera) {
    terrain::DrawTerrainBrushPreview(camera);
}

void RequestHeightmapImport() {
    terrain::RequestHeightmapImport();
}

// ---------------------------------------------------------------------------
// Dear ImGui integration
// ---------------------------------------------------------------------------

static void ApplyImGuiTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding     = 6.0f;
    s.PopupRounding      = 4.0f;
    s.FrameRounding      = 4.0f;
    s.ScrollbarRounding  = 4.0f;
    s.GrabRounding       = 4.0f;
    s.TabRounding        = 4.0f;
    s.WindowBorderSize   = 1.0f;
    s.WindowPadding      = ImVec2(8.0f, 8.0f);
    s.FramePadding       = ImVec2(6.0f, 4.0f);
    s.ItemSpacing        = ImVec2(6.0f, 5.0f);
    s.ItemInnerSpacing   = ImVec2(6.0f, 4.0f);
    s.ScrollbarSize      = 13.0f;

    // Mirrors the app's Dark Theme palette in theme:: (BG_*/TEXT_*/ACCENT...)
    auto rgba = [](int r, int g, int b, int a = 255) -> ImVec4 {
        return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
    };

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = rgba(232, 232, 238);   // theme::TEXT
    c[ImGuiCol_TextDisabled]         = rgba(122, 128, 140);   // theme::TEXT_DIM
    c[ImGuiCol_TextSelectedBg]       = rgba(0, 190, 200, 64); // theme::ACCENT_SOFT
    c[ImGuiCol_WindowBg]             = rgba(30, 32, 38);      // theme::BG_PANEL
    c[ImGuiCol_ChildBg]              = rgba(30, 32, 38);
    c[ImGuiCol_PopupBg]              = rgba(35, 37, 43, 250); // theme::BG_MENU
    c[ImGuiCol_Border]               = rgba(72, 77, 88);      // theme::BORDER
    c[ImGuiCol_BorderShadow]         = rgba(0, 0, 0, 60);
    c[ImGuiCol_FrameBg]              = rgba(21, 23, 28);      // theme::BG_INPUT
    c[ImGuiCol_FrameBgHovered]       = rgba(27, 29, 36);      // theme::BG_INPUT_HOVER
    c[ImGuiCol_FrameBgActive]        = rgba(40, 43, 51);      // theme::BG_WIDGET_PRESSED
    c[ImGuiCol_TitleBg]              = rgba(38, 41, 48);      // theme::BG_TITLE
    c[ImGuiCol_TitleBgActive]        = rgba(38, 41, 48);
    c[ImGuiCol_TitleBgCollapsed]     = rgba(25, 27, 32);      // theme::BG_TOP_BAR
    c[ImGuiCol_MenuBarBg]            = rgba(25, 27, 32);
    c[ImGuiCol_ScrollbarBg]          = rgba(21, 23, 28, 180);
    c[ImGuiCol_ScrollbarGrab]        = rgba(72, 77, 88);
    c[ImGuiCol_ScrollbarGrabHovered] = rgba(104, 110, 122);   // theme::BORDER_STRONG
    c[ImGuiCol_ScrollbarGrabActive]  = rgba(122, 128, 140);
    c[ImGuiCol_CheckMark]            = rgba(0, 190, 200);     // theme::ACCENT
    c[ImGuiCol_SliderGrab]           = rgba(0, 190, 200);
    c[ImGuiCol_SliderGrabActive]     = rgba(40, 210, 220);    // theme::ACCENT_HOVER
    c[ImGuiCol_Button]               = rgba(48, 51, 60);      // theme::BG_WIDGET
    c[ImGuiCol_ButtonHovered]        = rgba(63, 67, 78);      // theme::BG_WIDGET_HOVER
    c[ImGuiCol_ButtonActive]         = rgba(40, 43, 51);
    c[ImGuiCol_Header]               = rgba(48, 51, 60);
    c[ImGuiCol_HeaderHovered]        = rgba(63, 67, 78);
    c[ImGuiCol_HeaderActive]         = rgba(40, 43, 51);
    c[ImGuiCol_Separator]            = rgba(56, 60, 70);      // theme::DIVIDER
    c[ImGuiCol_SeparatorHovered]     = rgba(72, 77, 88);
    c[ImGuiCol_SeparatorActive]      = rgba(0, 190, 200);
    c[ImGuiCol_ResizeGrip]           = rgba(56, 60, 70);
    c[ImGuiCol_ResizeGripHovered]    = rgba(72, 77, 88);
    c[ImGuiCol_ResizeGripActive]     = rgba(104, 110, 122);
    c[ImGuiCol_Tab]                  = rgba(35, 37, 43);
    c[ImGuiCol_TabHovered]           = rgba(0, 190, 200, 100);
    c[ImGuiCol_TabActive]            = rgba(0, 190, 200, 55);
    c[ImGuiCol_TabUnfocused]         = rgba(35, 37, 43);
    c[ImGuiCol_TabUnfocusedActive]   = rgba(0, 190, 200, 35);
    c[ImGuiCol_TableHeaderBg]        = rgba(38, 41, 48);
    c[ImGuiCol_TableBorderStrong]    = rgba(72, 77, 88);
    c[ImGuiCol_TableBorderLight]     = rgba(56, 60, 70);
    c[ImGuiCol_TableRowBg]           = rgba(30, 32, 38);
    c[ImGuiCol_TableRowBgAlt]        = rgba(35, 37, 43);
    c[ImGuiCol_PlotLines]            = rgba(0, 190, 200);
    c[ImGuiCol_PlotHistogram]        = rgba(0, 190, 200);
    c[ImGuiCol_ModalWindowDimBg]     = rgba(0, 0, 0, 110);
}

void DrawImGuiFrame() {
    if (IsKeyPressed(KEY_F1)) g_showDemoWindow = !g_showDemoWindow;

    DrawImGuiTopBar();
    DrawImGuiExplorer();
    DrawImGuiExplorerMenu();

    ImGuiIO& io = ImGui::GetIO();

    if (g_showDemoWindow) {
        ImGui::ShowDemoWindow(&g_showDemoWindow);
    }

    // Only take over the cursor while the pointer is over an ImGui window;
    // elsewhere the raylib panels drive their own hand/ibeam behavior.
    if (io.WantCaptureMouse) {
        ImGui_ImplRaylib_UpdateMouseCursor();
    }

    ImGui::Render();
    ImGui_ImplRaylib_RenderDrawData(ImGui::GetDrawData());
}

} // namespace ui