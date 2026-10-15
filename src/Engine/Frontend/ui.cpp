// ui.cpp (fixed)
#include "ui_internal.hpp"
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
#include <utility>
#include <sstream>
#include <cctype>
#include <cmath>

namespace ui {

static Font g_arialFont = { 0 };
static Texture2D g_toolIcons[5] = {};
static Texture2D g_deleteIcon = { 0 };
Texture2D g_rowIcons[ROWICON_COUNT] = {};
// Asset browser: folder + search glyph (mesh/water/terrain/script entries
// reuse g_rowIcons above; the up-arrow reuses g_terrainToolIcons[TTOOLICON_LOWER]).
Texture2D g_folderIcon = { 0 };
Texture2D g_assetSearchIcon = { 0 };
Texture2D g_terrainToolIcons[TTOOLICON_COUNT] = {};
static Vector3* g_pos = nullptr;
static Vector3* g_size = nullptr;
static Color* g_color = nullptr;
static Vector3* g_rotation = nullptr;
static Vector3* g_origin = nullptr;
const std::vector<ScatteredObject*>* g_sceneObjects = nullptr;
std::vector<std::unique_ptr<ModelGroup>>* g_sceneModels = nullptr;

// A script is one thing: a C++ class and/or a node graph, found by the class name. Keys the graph by that name.
std::string ScriptKey(const ScriptRuntime::StandaloneScript& s) {
    return scriptLauncher::MakeClassName(s.typeName.empty() ? s.name : s.typeName);
}

static const std::vector<WaterBody*>& SceneWaters() {
    return WaterBody::GetInstances();
}
static ScatteredObject* g_selectedObject = nullptr;
static phys::Simulation* g_simulation = nullptr;
std::vector<ScatteredObject*> g_selection;
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
static int g_lightingSel = 0;   // the selected Lighting row of the explorer: 0 none, 1 Time, 2 Sun, 3 Ambient, 4 Sky, 5 Fog, 6 Weather, 7 Clouds, 8 Picture
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
bool g_clickConsumedThisFrame = false;

// Dear ImGui integration (debug overlay for the migration work)
static bool g_showDemoWindow = false;
static bool g_showDebugStats = false;

// Preferences panel: editor/workflow settings only, never game/scene data.
// Extremely basic for now - a category sidebar with one real setting
// (Language, General tab); more will be added to specific categories later.
static int  g_prefsLanguageIndex = 0;

// ImGui-driven rename state: signals the inline rename field to grab focus.
static bool g_imFocusNextRename = false;
static std::vector<ScatteredObject*> g_imDragPayload; // explorer drag payload buffer



// Points every accent-coloured ImGui style entry at theme::ACCENT. No-op until
// the ImGui context exists; ApplyImGuiTheme calls it again when it is created.
static void ApplyAccentToImGuiStyle() {
    if (!ImGui::GetCurrentContext()) return;
    ImVec4* c = ImGui::GetStyle().Colors;
    const ImVec4 hover(theme::ACCENT_HOVER.r / 255.0f, theme::ACCENT_HOVER.g / 255.0f,
                       theme::ACCENT_HOVER.b / 255.0f, 1.0f);
    c[ImGuiCol_TextSelectedBg]     = theme::AccentVec(64 / 255.0f);
    c[ImGuiCol_CheckMark]          = theme::AccentVec();
    c[ImGuiCol_SliderGrab]         = theme::AccentVec();
    c[ImGuiCol_SliderGrabActive]   = hover;
    c[ImGuiCol_SeparatorActive]    = theme::AccentVec();
    c[ImGuiCol_TabHovered]         = theme::AccentVec(100 / 255.0f);
    c[ImGuiCol_TabActive]          = theme::AccentVec(55 / 255.0f);
    c[ImGuiCol_TabUnfocusedActive] = theme::AccentVec(35 / 255.0f);
    c[ImGuiCol_PlotLines]          = theme::AccentVec();
    c[ImGuiCol_PlotHistogram]      = theme::AccentVec();
}

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
    FIELD_WATER_FOAM_INTENSITY, FIELD_WATER_FOAM_SCALE, FIELD_WATER_FOAM_THRESHOLD,
    FIELD_WATER_DETAIL_INTENSITY, FIELD_WATER_DETAIL_SCALE,
    FIELD_WATER_REFLECT_STRENGTH, FIELD_WATER_REFLECT_DISTORTION,
    FIELD_WATER_WAKE_STRENGTH, FIELD_WATER_SPLASH_STRENGTH, FIELD_WATER_FOAM_LIFE, FIELD_WATER_SPRAY_AMOUNT
};

std::unordered_map<uint64_t, float> g_hoverAlpha;

} // namespace
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
namespace {

float MenuAnim(double openTime) {
    auto anim = static_cast<float>((GetTime() - openTime) / 0.09);
    if (anim > 1.0f) anim = 1.0f;
    if (anim < 0.0f) anim = 0.0f;
    return anim;
}

bool g_cursorHand = false;
bool g_cursorIbeam = false;

void BeginCursorPass() { g_cursorHand = false; g_cursorIbeam = false; }
} // namespace
void MarkHand() { g_cursorHand = true; }
namespace {
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
static std::set<std::string> g_scriptSel;   // selected scripts in the Explorer, by ScriptKey (Ctrl/Shift-click)
static int g_scriptSelAnchor = -1;
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
static bool g_waterInteractOpen = true;
static bool g_positionOpen = true;
static bool g_sizeOpen = true;
static bool g_rotationOpen = true;
static bool g_originOpen = true;
static bool g_linearVelocityOpen = true;
static bool g_angularVelocityOpen = true;
static bool g_waterNoiseOpen = true;
static bool g_waterFoamOpen = true;
static bool g_waterDetailOpen = true;
static bool g_waterReflectOpen = true;
static bool g_waterWakeOpen = true;

// Screen rect of the currently active number field
static Rectangle g_activeFieldRect = { 0.0f, 0.0f, 0.0f, 0.0f };

// Forward declarations
static bool TextEditKeys(char* buf, int bufSize, int& cursor, int& sel, bool& commit);
static void TextEditMouse(const char* buf, int& cursor, int& sel, Rectangle rec,
                          float textStartX, float fontSize, bool active);
static void DrawTextSel(const char* buf, int cursor, int sel, float textStartX,
                        float y, float height, float fontSize, Color color);
static void ApplyImGuiTheme();

// Loads one of the engine's bundled icons, taking a name relative to
// assets/EditorIcons/ (or to assets/ when it starts with "../"). The returned
// Image is owned by the caller, who must UnloadImage() it after uploading.
//
// The icons are black silhouettes that the dark theme recolours to white. A
// missing file yields a zeroed Image rather than an error, so a partial
// install degrades to "no icon" instead of failing to start.
static Image LoadEditorIcon(const std::string& name) {
    Image img = {};
    const std::string path = platform::ResolveAsset("assets/EditorIcons/" + name);
    if (path.empty()) return img;
    img = LoadImage(path.c_str());
    if (img.data != nullptr) ImageColorBrightness(&img, 255);
    return img;
}

void Init() {
    // platform::ResolveFontPath() already applies the precedence: a font in
    // the working directory wins, then the system candidates. Empty means
    // nothing was found, and raylib's built-in bitmap font is the fallback.
    const std::string& fontPath = platform::ResolveFontPath();
    if (!fontPath.empty()) {
        g_arialFont = LoadFontEx(fontPath.c_str(), 32, nullptr, 0);
    }
    if (g_arialFont.texture.id == 0) g_arialFont = GetFontDefault();
    SetTextureFilter(g_arialFont.texture, TEXTURE_FILTER_BILINEAR);

    constexpr const char* toolIconNames[] = { "select.png", "move.png", "scale.png", "rotate.png", "import.png" };
    for (int i = 0; i < 5; ++i) {
        Image img = LoadEditorIcon(toolIconNames[i]);
        if (img.data != nullptr) {
            g_toolIcons[i] = LoadTextureFromImage(img);
            UnloadImage(img); // Free CPU RAM immediately after GPU upload
        }
    }

    // Load delete icon
    {
        Image delImg = LoadEditorIcon("../delete.png");
        if (delImg.data != nullptr) {
            g_deleteIcon = LoadTextureFromImage(delImg);
            UnloadImage(delImg);
        }
    }

    constexpr const char* rowIconNames[ROWICON_COUNT] = { "cube.png", "water.png", "terrain.png", "script.png" };
    for (int i = 0; i < ROWICON_COUNT; ++i) {
        Image img = LoadEditorIcon(rowIconNames[i]);
        if (img.data != nullptr) {
            g_rowIcons[i] = LoadTextureFromImage(img);
            UnloadImage(img);
        }
    }

    {
        Image img = LoadEditorIcon("Folder.png");
        if (img.data != nullptr) {
            g_folderIcon = LoadTextureFromImage(img);
            UnloadImage(img);
        }
    }
    {
        Image img = LoadEditorIcon("search.png");
        if (img.data != nullptr) {
            g_assetSearchIcon = LoadTextureFromImage(img);
            UnloadImage(img);
        }
    }

    constexpr const char* terrainToolIconNames[TTOOLICON_COUNT] = {
        "arrowup.png", "arrowdown.png", "smooth.png", "linehoriz.png", "paint.png",
        "erode.png", "noise.png", "ramp.png", "none.png"
    };
    for (int i = 0; i < TTOOLICON_COUNT; ++i) {
        Image img = LoadEditorIcon(terrainToolIconNames[i]);
        if (img.data != nullptr) {
            g_terrainToolIcons[i] = LoadTextureFromImage(img);
            UnloadImage(img);
        }
    }

    // Dear ImGui setup. Runs after InitWindow created the graphics device
    // (ScopedUI in main.cpp); rlImGuiSetup() creates the context.
    IMGUI_CHECKVERSION();
    rlImGuiSetup();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;               // no .ini config persistence for now
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Match the app's Arial look for ImGui text; include the symbol blocks used
    // by the explorer rows (▣ ▢ ⛰ arrows) so labels render correctly.
    ImFontConfig fontCfg;
    fontCfg.OversampleH = 1;
    fontCfg.OversampleV = 1;
    static const ImWchar glyphRanges[] = { 0x20, 0xFF, 0x2190, 0x21FF, 0x2200, 0x22FF, 0x2500, 0x25FF, 0x2600, 0x26FF, 0 };
    // Same resolved font the 3D text uses, so the ImGui labels and the scene
    // text are the same typeface at a consistent size.
    const std::string& imguiFontPath = platform::ResolveFontPath();
    if (!imguiFontPath.empty()) {
        io.Fonts->AddFontFromFileTTF(imguiFontPath.c_str(), 15.0f, &fontCfg, glyphRanges);
    } else {
        io.Fonts->AddFontDefault();
    }

    ApplyImGuiTheme();
}

void Unload() {
    // ImGui shutdown first: it still needs the graphics device, which is alive
    // until the Engine (and thus CloseWindow) is destroyed after ui::Unload().
    rlImGuiShutdown();

    for (int i = 0; i < 5; ++i) {
        if (g_toolIcons[i].id != 0) UnloadTexture(g_toolIcons[i]);
        g_toolIcons[i] = {};
    }
    if (g_deleteIcon.id != 0) UnloadTexture(g_deleteIcon);
    g_deleteIcon = {};
    for (int i = 0; i < ROWICON_COUNT; ++i) {
        if (g_rowIcons[i].id != 0) UnloadTexture(g_rowIcons[i]);
        g_rowIcons[i] = {};
    }
    if (g_folderIcon.id != 0) UnloadTexture(g_folderIcon);
    g_folderIcon = {};
    if (g_assetSearchIcon.id != 0) UnloadTexture(g_assetSearchIcon);
    g_assetSearchIcon = {};
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
LogEntry g_log[kLogCapacity];
int g_logWrite = 0;   // next slot to write
int g_logCount = 0;   // total entries currently held

// Lines ever written (clearing the log does not rewind it), and how many of them the Output tab has shown:
// the difference is what the red dot on the tab announces.
unsigned long long g_logTotal = 0;
unsigned long long g_logSeenTotal = 0;

static void LogImpl(LogSeverity severity, const char* fmt, va_list args) {
    char buf[kLogLineLen];
    vsnprintf(buf, sizeof(buf), fmt, args);

    loading::AddLogLine(buf);       // shown on the loading screen while a project loads
    snprintf(g_log[g_logWrite].text, sizeof(g_log[g_logWrite].text), "%s", buf);
    g_log[g_logWrite].severity = severity;
    g_log[g_logWrite].time = GetTime();
    g_logWrite = (g_logWrite + 1) % kLogCapacity;
    if (g_logCount < kLogCapacity) g_logCount++;
    g_logTotal++;
}

namespace {
// Script build status panel (see ui.hpp). Written by SetScriptBuildState,
// read by DrawScriptErrorPanel / IsMouseOverUI.
ScriptBuildState g_scriptBuildState = ScriptBuildState::Idle;
std::vector<ScriptDiagnostic> g_scriptDiagnostics;
double g_scriptBuildStarted = 0.0;
bool g_scriptPanelDismissed = false;
int g_scriptPanelScroll = 0; // first visible row
} // namespace

void SetScriptBuildState(ScriptBuildState state, std::vector<ScriptDiagnostic> diagnostics) {
    if (state == ScriptBuildState::Building && g_scriptBuildState != ScriptBuildState::Building)
        g_scriptBuildStarted = GetTime();
    // New results re-open a panel the user closed, and start at the top.
    if (state == ScriptBuildState::Failed) {
        g_scriptPanelDismissed = false;
        g_scriptPanelScroll = 0;
    }
    g_scriptBuildState = state;
    if (state != ScriptBuildState::Building) g_scriptDiagnostics = std::move(diagnostics);
}

void SetAccentColor(Color accent) {
    accent.a = 255;
    auto lighten = [](unsigned char v, float t) {
        return static_cast<unsigned char>(v + (255 - v) * t);
    };
    theme::ACCENT = accent;
    theme::ACCENT_HOVER = Color{ lighten(accent.r, 0.18f), lighten(accent.g, 0.18f), lighten(accent.b, 0.18f), 255 };
    theme::ACCENT_PRESSED = Color{ static_cast<unsigned char>(accent.r * 0.78f),
                                   static_cast<unsigned char>(accent.g * 0.78f),
                                   static_cast<unsigned char>(accent.b * 0.78f), 255 };
    theme::ACCENT_SOFT = Color{ accent.r, accent.g, accent.b, 42 };
    ApplyAccentToImGuiStyle();
}

void Log(const char* fmt, ...) {
    if (!g_playActive) return;

    va_list args;
    va_start(args, fmt);
    LogImpl(LogSeverity::Neutral, fmt, args);
    va_end(args);
}

void LogAlways(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogImpl(LogSeverity::Neutral, fmt, args);
    va_end(args);
}

void LogWithSeverity(LogSeverity severity, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogImpl(severity, fmt, args);
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

bool g_spawnRequested = false;
std::string g_spawnPath;
Vector3 g_spawnPos{0, 0, 0};
bool ConsumeModelSpawnRequest(std::string& outPath, Vector3& outPos) {
    if (!g_spawnRequested) return false;
    g_spawnRequested = false;
    outPath = g_spawnPath;
    outPos = g_spawnPos;
    return true;
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

bool IsEditingText() {
    return g_activeField != FIELD_NONE || g_renameObject != nullptr || g_renameModel != nullptr
        || g_scriptRenameIndex >= 0 || g_renameTerrain != nullptr || g_renameLegacyTerrain != nullptr
        || console::IsActive()
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
        if (ScriptRuntime* rt = GetActiveRuntime()) {
            auto& scripts = rt->StandaloneScripts();
            if (g_scriptRenameIndex >= 0 && g_scriptRenameIndex < static_cast<int>(scripts.size())) {
                auto& s = scripts[g_scriptRenameIndex];
                // Keep an auto-derived class name in sync with the display name so
                // the materialized .cpp (and the class it defines) still matches.
                std::string oldClass = scriptLauncher::MakeClassName(s.typeName.empty() ? s.name : s.typeName);
                s.name = g_scriptNameBuffer;
                if (s.typeName == oldClass)
                    s.typeName = scriptLauncher::MakeClassName(s.name);
                vscript::RenameKey(oldClass, ScriptKey(s));
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
    rlImGuiNewFrame(GetFrameTime());   // also calls ImGui::NewFrame()

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

// The bottom panel (Assets and Output tabs): docked directly on the command console bar, flush with it, spanning
// the same middle strip (between Explorer and Properties) as the console itself.
Rectangle GetAssetBrowserPanelBounds() {
    constexpr float h = 230.0f;
    Rectangle area = GetConsoleBarArea();
    Rectangle bar = console::GetBounds();
    return Rectangle{ area.x, bar.y - h, area.width, h };
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

// Script build panel: a strip over the top of the viewport. While compiling it
// is a one-line status (shown only once a build has taken a moment, so quick
// rebuilds don't flash it); after a failure it lists the compiler messages.
constexpr float kScriptPanelHeaderH = 26.0f;
constexpr float kScriptPanelRowH = 19.0f;
constexpr int kScriptPanelMaxRows = 7;

static bool ScriptPanelVisible() {
    if (g_scriptBuildState == ScriptBuildState::Failed) return !g_scriptPanelDismissed;
    if (g_scriptBuildState == ScriptBuildState::Building)
        return GetTime() - g_scriptBuildStarted > 0.5;
    return false;
}

static Rectangle GetScriptPanelBounds() {
    if (!ScriptPanelVisible()) return Rectangle{ 0.0f, 0.0f, 0.0f, 0.0f };
    Rectangle area = GetConsoleBarArea();
    const float top = GetTopBarBounds().height + 8.0f;
    float h = kScriptPanelHeaderH + 4.0f;
    if (g_scriptBuildState == ScriptBuildState::Failed) {
        const int rows = std::min<int>(kScriptPanelMaxRows, static_cast<int>(g_scriptDiagnostics.size()));
        h += static_cast<float>(rows) * kScriptPanelRowH;
    }
    return Rectangle{ area.x + 8.0f, top, area.width - 16.0f, h };
}

// Terrain tool panel (defined near ui::Draw; needs bounds checks here)


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
    if (CheckCollisionPointRec(mouse, GetAssetBrowserPanelBounds())) return true;

    Rectangle terrRec = GetTerrainPanelBounds();
    if (terrRec.width > 0.0f && CheckCollisionPointRec(mouse, terrRec)) return true;

    Rectangle scriptRec = GetScriptPanelBounds();
    if (scriptRec.width > 0.0f && CheckCollisionPointRec(mouse, scriptRec)) return true;

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

void DrawTextArial(const char* text, float posX, float posY, float fontSize, Color color) {
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

float MeasureTextArial(const char* text, float fontSize) {
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

// Deletes a script for real: its graph, its Explorer entry and its .cpp, and tells the host so the still compiled
// class doesn't bring it back. `index` is its StandaloneScripts slot, or -1 to look it up by key.
void DeleteScriptEverywhere(const std::string& key, int index) {
    vscript::RemoveByName(key);
    ScriptRuntime* rt = GetActiveRuntime();
    if (!rt) return;
    auto& scripts = rt->StandaloneScripts();
    if (index < 0)
        for (int i = 0; i < static_cast<int>(scripts.size()); ++i)
            if (ScriptKey(scripts[i]) == key) { index = i; break; }
    if (index >= 0 && index < static_cast<int>(scripts.size())) rt->RemoveStandalone(index);
    g_scriptSel.erase(key);
    if (scriptLauncher::DeleteScriptFile(key) && rt->GetHost()) rt->GetHost()->ForgetScriptClass(key);
}

// True when the script menu was opened on one of several selected scripts: the menu then acts on all of them.
static bool ScriptMenuIsMulti() {
    ScriptRuntime* rt = GetActiveRuntime();
    if (!rt || g_scriptSel.size() < 2) return false;
    auto& scripts = rt->StandaloneScripts();
    return g_explorerMenuScript >= 0 && g_explorerMenuScript < static_cast<int>(scripts.size()) &&
           g_scriptSel.count(ScriptKey(scripts[g_explorerMenuScript])) > 0;
}

// Graphs made in the Visual Script tab get a script entry of their own, so the Explorer lists them too.
static void SyncVisualScriptEntries() {
    ScriptRuntime* rt = GetActiveRuntime();
    if (!rt) return;
    auto& scripts = rt->StandaloneScripts();
    for (int i = 0; i < vscript::Count(); ++i) {
        const std::string key = vscript::NameAt(i);
        bool found = false;
        for (const auto& s : scripts) if (ScriptKey(s) == key) { found = true; break; }
        if (!found && g_sceneObjects)
            for (auto* o : *g_sceneObjects) if (o && o->script == key) { found = true; break; }
        if (found) continue;
        ScriptRuntime::StandaloneScript s;
        s.name = key;
        s.typeName = key;
        s.runOnPlay = true;
        scripts.push_back(std::move(s));
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
            if (singleSel && i == cursor++) return has ? "Edit Script" : "Add Script";
            if (has && i == cursor++) return "Remove Script";
            if (has && i == cursor++) return "Choose Editor...";
            if (has && i == cursor++) return "Open in Visual Script";
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
            return i == 0 ? "Insert Script" : nullptr;
        case ExplorerMenuMode::Script:
            if (ScriptMenuIsMulti()) return i == 0 ? "Open in Visual Script" : (i == 1 ? "Delete Selected" : nullptr);
            return i == 0 ? "Rename" : (i == 1 ? "Delete" : (i == 2 ? "Choose Editor..." : (i == 3 ? "Open in Visual Script" : nullptr)));
    }
    return nullptr;
}

static void PerformExplorerMenuAction(int i) {
    // Object/group/model actions work without a script runtime; only the
    // script-list actions need one, and they check below.
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
                scriptLauncher::EditObjectScript(g_explorerMenuObject);
                return;
            }
            if (has && i == cursor++) {
                if (ScriptRuntime* rt = GetActiveRuntime()) rt->StopObjectScript(g_explorerMenuObject);
                g_explorerMenuObject->script.clear();
                g_explorerMenuObject->runOnPlay = false;
                return;
            }
            if (has && i == cursor++) {
                scriptLauncher::ChooseEditorForObjectScript(g_explorerMenuObject);
                return;
            }
            if (has && i == cursor++) {
                vscript::OpenByName(scriptLauncher::MakeClassName(g_explorerMenuObject->script));
                SelectBottomTab(2);
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
            if (ScriptRuntime* rt = GetActiveRuntime()) {
                auto& scripts = rt->StandaloneScripts();
                ScriptRuntime::StandaloneScript s;
                s.name = "Script";
                s.typeName = "";
                s.runOnPlay = true;
                scripts.push_back(std::move(s));
                auto idx = static_cast<int>(scripts.size()) - 1;
                std::string className;
                scriptLauncher::EditStandaloneScript(scripts[idx].name, scripts[idx].typeName, &className);
                if (!className.empty() && scripts[idx].typeName.empty())
                    scripts[idx].typeName = className;
            }
            break;
        }
        case ExplorerMenuMode::Script: {
            if (ScriptRuntime* rt = GetActiveRuntime()) {
                auto& scripts = rt->StandaloneScripts();
                if (g_explorerMenuScript < 0 || g_explorerMenuScript >= static_cast<int>(scripts.size())) return;
                if (ScriptMenuIsMulti()) {
                    const std::set<std::string> keys = g_scriptSel;
                    if (i == 0) {
                        for (const auto& k : keys) vscript::OpenByName(k);
                        SelectBottomTab(2);
                    } else if (i == 1) {
                        for (int k = static_cast<int>(scripts.size()) - 1; k >= 0; --k)
                            if (keys.count(ScriptKey(scripts[k]))) {
                                DeleteScriptEverywhere(ScriptKey(scripts[k]), k);
                            }
                        g_scriptSel.clear();
                    }
                    return;
                }
                if (i == 0) {
                    g_scriptRenameIndex = g_explorerMenuScript;
                    snprintf(g_scriptNameBuffer, sizeof(g_scriptNameBuffer), "%s", scripts[g_explorerMenuScript].name.c_str());
                    g_scriptNameBuffer[sizeof(g_scriptNameBuffer) - 1] = '\0';
                    g_cursorPos = static_cast<int>(strlen(g_scriptNameBuffer));
                    g_selStart = -1;
                } else if (i == 1) {
                    DeleteScriptEverywhere(ScriptKey(scripts[g_explorerMenuScript]), g_explorerMenuScript);
                } else if (i == 2) {
                    auto& s = scripts[g_explorerMenuScript];
                    scriptLauncher::ChooseEditorForStandaloneScript(s.name, s.typeName);
                } else if (i == 3) {
                    vscript::OpenByName(ScriptKey(scripts[g_explorerMenuScript]));
                    SelectBottomTab(2);
                }
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
    // including retargeting an already-open menu to a new row - use it (not
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

// Shared look for every hand-drawn dropdown header/row: dark-teal fill inside a
// border that is grey at rest and bright cyan when active (hovered, selected, or open).
static std::string FitTextToWidth(const std::string& text, float fontSize, float maxWidth);

void DrawDropdownBox(const Rectangle& r, bool active, float alpha) {
    const Color rest   = Color{ 58, 58, 58, 255 };
    const Color lit    = theme::ACCENT_HOVER;
    const Color fill   = Mix(Color{ 14, 20, 24, 255 }, theme::ACCENT, 0.27f);
    DrawRectangleRounded(r, 0.2f, 4, Fade(active ? lit : rest, alpha));
    Rectangle inner = { r.x + 2.0f, r.y + 2.0f, r.width - 4.0f, r.height - 4.0f };
    DrawRectangleRounded(inner, 0.2f, 4, Fade(fill, alpha));
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

    if (hoverAdd || (showSubMenu && hoveringMain && !hoverSpace)) DrawDropdownBox(rowAdd, true, anim);
    if (hoverSpace) DrawDropdownBox(rowSpace, true, anim);
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

            if (itemHovered) DrawDropdownBox(itemRec, true, anim);
            DrawTextArial(items[i], itemRec.x + 10.0f, itemRec.y + 4.0f, 14.0f,
                          Fade(itemHovered ? theme::TEXT : theme::TEXT_MUTED, anim));
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
// 1. Top Bar (Dear ImGui) - File menu, Import, tool buttons, Play/Stop.
// Called once per frame from DrawImGuiFrame(), between ImGui::NewFrame()
// (UpdateInput()) and ImGui::Render().
// ---------------------------------------------------------------------------
static ImTextureID ToolTex(int i) {
    return g_toolIcons[i].id != 0
        ? static_cast<ImTextureID>(GetTextureImGuiId(g_toolIcons[i]))
        : static_cast<ImTextureID>(0);
}

static ImTextureID RowTex(RowIconType i) {
    return g_rowIcons[i].id != 0
        ? static_cast<ImTextureID>(GetTextureImGuiId(g_rowIcons[i]))
        : static_cast<ImTextureID>(0);
}

// Launch the standalone player with the current project
static void LaunchPlayer() {
    std::string playerExe = platform::ExecutablePath();
    std::filesystem::path exePath = std::filesystem::u8path(playerExe);
    
    // Try to find FlyPlayer in the same directory
    std::filesystem::path playerPath = exePath.parent_path() / "FlyPlayer";
    {
        // Knows the platform's file name too (FlyPlayer.exe on Windows).
        const std::string found = exporter::FindPlayerExecutable();
        if (!found.empty()) playerPath = std::filesystem::u8path(found);
    }
    if (!std::filesystem::exists(playerPath)) {
        // Try case-insensitive search
        for (auto& entry : std::filesystem::directory_iterator(exePath.parent_path())) {
            std::string name = entry.path().filename().string();
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower == "flyplayer") {
                playerPath = entry.path();
                break;
            }
        }
    }
    
    // Fallback to case variants
    if (!std::filesystem::exists(playerPath)) {
        playerPath = exePath.parent_path() / "flyplayer";
    }
    if (!std::filesystem::exists(playerPath)) {
        playerPath = exePath.parent_path() / "FLYPLAYER";
    }
    if (!std::filesystem::exists(playerPath)) {
        playerPath = exePath.parent_path() / "Flyplayer";
    }
    
    // Get current project path
    const auto& proj = project::GetCurrentProject();
    if (proj.path.empty()) {
        return;
    }
    
    // Save current scene to temp file (doesn't overwrite main .flyproj)
    std::string tempProjectFile = "";
    
    // Get legacy terrain from terrain editor state
    terrain::Terrain* currentTerrain = nullptr;
    auto& terrainEditorState = terrain::GetTerrainEditorState();
    currentTerrain = terrainEditorState.selectedTerrainLegacy;
    
    if (g_sceneObjects && g_sceneModels) {
        tempProjectFile = project::SaveProjectFileTemp(proj.path, *g_sceneObjects, *g_sceneModels, currentTerrain);
        if (!tempProjectFile.empty()) {
            std::printf("[Play in Player] Saved temp scene to: %s\n", tempProjectFile.c_str());
        }
    }
    
    // Final fallback to editor executable
    if (!std::filesystem::exists(playerPath)) {
        playerPath = std::filesystem::u8path(platform::ExecutablePath());
    }
    
    // Use temp file if created, otherwise use main project
    std::string launchPath = tempProjectFile.empty() ? proj.path : tempProjectFile;
    std::vector<std::string> args = {"-play", launchPath, "--noscripts"};
    platform::LaunchDetached(playerPath.string(), args);
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

    ImGui::SameLine();
    if (ImGui::Button("City", ImVec2(0.0f, 30.0f))) {
        ImGui::OpenPopup("##CityMenu");
    }
    ImVec2 cityBtnMin = ImGui::GetItemRectMin();

    ImGui::EndDisabled();

    // City menu popup, anchored under the City button
    ImGui::SetNextWindowPos(ImVec2(cityBtnMin.x, barRec.y + 38.0f));
    if (ImGui::BeginPopup("##CityMenu")) {
        if (ImGui::MenuItem("New regular city")) { g_pendingFileAction = MenuAction::SpawnCity; ImGui::CloseCurrentPopup(); }
        if (ImGui::MenuItem("New organic city")) { g_pendingFileAction = MenuAction::SpawnCityOrganic; ImGui::CloseCurrentPopup(); }
        ImGui::Separator();
        bool panelOpen = city::GetCityEditorState().panelOpen;
        if (ImGui::MenuItem("City panel", nullptr, &panelOpen)) {
            city::GetCityEditorState().panelOpen = panelOpen;
        }
        ImGui::EndPopup();
    }

    // File menu popup, anchored under the File button
    ImGui::SetNextWindowPos(ImVec2(barRec.x + 4.0f, barRec.y + 38.0f));
    if (ImGui::BeginPopup("##FileMenu")) {
        if (ImGui::MenuItem("Save")) { g_pendingFileAction = MenuAction::Save; ImGui::CloseCurrentPopup(); }
        if (ImGui::MenuItem("Save as")) { g_pendingFileAction = MenuAction::SaveAs; ImGui::CloseCurrentPopup(); }
        if (ImGui::MenuItem("Open scene")) { g_pendingFileAction = MenuAction::OpenScene; ImGui::CloseCurrentPopup(); }
        ImGui::Separator();
        if (ImGui::MenuItem("Export...")) { OpenExportDialog(); ImGui::CloseCurrentPopup(); }
        ImGui::Separator();
        if (ImGui::MenuItem("Preferences")) { g_showPreferences = true; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // Transform tool buttons (row 2)
    ImGui::SetCursorPos(ImVec2(75.0f, 45.0f));
    constexpr const char* toolNames[] = { "Select", "Move", "Scale", "Rotate" };
    constexpr TransformTool toolEnums[] = { TransformTool::Select, TransformTool::Move, TransformTool::Scale, TransformTool::Rotate };
    for (int i = 0; i < 4; ++i) {
        bool selected = (g_activeTool == toolEnums[i]);

        // One pill per tool: rounded icon tile on the left, label on the right.
        constexpr float pillH = 34.0f;
        constexpr float tile = 26.0f;
        constexpr float padX = 4.0f;
        constexpr float gap = 8.0f;
        const float textW = ImGui::CalcTextSize(toolNames[i]).x;
        const ImVec2 pillSize(padX + tile + gap + textW + 12.0f, pillH);

        ImGui::PushID(i);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##tool", pillSize)) g_activeTool = toolEnums[i];
        const bool hov = ImGui::IsItemHovered();
        if (hov) MarkHand();
        ImGui::PopID();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p1(p0.x + pillSize.x, p0.y + pillSize.y);
        ImU32 bg = selected ? theme::AccentU32(180)
                 : hov      ? IM_COL32(63, 67, 78, 255)
                            : IM_COL32(48, 51, 60, 255);
        dl->AddRectFilled(p0, p1, bg, 10.0f);

        const ImVec2 t0(p0.x + padX, p0.y + (pillH - tile) * 0.5f);
        const ImVec2 t1(t0.x + tile, t0.y + tile);
        dl->AddRectFilled(t0, t1, selected ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 22), 8.0f);
        if (ImTextureID tex = ToolTex(i)) {
            constexpr float icon = 16.0f;
            const float o = (tile - icon) * 0.5f;
            dl->AddImage(tex, ImVec2(t0.x + o, t0.y + o), ImVec2(t0.x + o + icon, t0.y + o + icon));
        }
        dl->AddText(ImVec2(t1.x + gap, p0.y + (pillH - ImGui::GetTextLineHeight()) * 0.5f),
                    IM_COL32(232, 232, 238, 255), toolNames[i]);

        if (i < 3) ImGui::SameLine(0.0f, 10.0f);
    }

    // Play in Player button (launches standalone player), right-aligned next to Play
    {
        ImGui::SetCursorPos(ImVec2(barRec.width - 210.0f, 15.0f));
        if (ImGui::Button("Play in Player", ImVec2(115.0f, 30.0f))) {
            LaunchPlayer();
        }
        ImGui::SameLine(0.0f, 12.0f);
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
// g_explorerDrag*, OpenExplorerMenu, PerformExplorerDrop, ...) - only the
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

    ImGui::TextColored(theme::AccentVec(), "EXPLORER");
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
                snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", g_selectedTerrain->GetName().c_str());
            } else if (terrain::GetTerrainEditorState().selectedTerrainLegacy) {
                terrain::Terrain* legacy = terrain::GetTerrainEditorState().selectedTerrainLegacy;
                g_renameLegacyTerrain = legacy;
                snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", legacy->GetName().c_str());
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
                    snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", pm->name.c_str());
                } else {
                    g_renameObject = g_primarySelection;
                    snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", g_primarySelection->GetName().c_str());
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
                                snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", model->name.c_str());
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
                            snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", terr->GetName().c_str());
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
                            snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", legacy->GetName().c_str());
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
                            snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", obj->GetName().c_str());
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

    // --- Lighting: scene-wide settings. Time is built in; the "+" menu inserts Sun, Ambient, Sky and Fog. Selecting a row
    // opens its settings in the properties area; right-click an inserted item to remove it (it goes back to its default). ---
    {
        gfx::LightingSettings& L = gfx::Lighting();
        if (!g_selection.empty() || g_selectedWater || g_selectedTerrain) g_lightingSel = 0;   // selecting anything else leaves it
        ImGui::Spacing();
        ImGui::TextDisabled("LIGHTING");
        ImGui::SameLine();
        if (ImGui::SmallButton("+##insertlighting")) ImGui::OpenPopup("##InsertLighting");
        if (ImGui::BeginPopup("##InsertLighting")) {
            ImGui::TextDisabled("Insert");
            ImGui::Separator();
            bool any = false;
            if (!L.hasSun && ImGui::MenuItem("Sun")) { L.hasSun = true; g_lightingSel = 2; any = true; }
            if (!L.hasAmbient && ImGui::MenuItem("Ambient")) { L.hasAmbient = true; g_lightingSel = 3; any = true; }
            // A new Sky is the atmosphere (it can be switched to one colour in its settings).
            if (!L.hasSky && ImGui::MenuItem("Sky")) { L.hasSky = true; L.skyMode = 1; g_lightingSel = 4; any = true; }
            // Under an atmosphere fog lies near the ground, so the sky stays visible above it.
            if (!L.hasFog && ImGui::MenuItem("Fog")) { L.hasFog = true; L.fogDensity = std::max(L.fogDensity, 0.004f); if (L.hasSky && L.skyMode == 1) L.fogHeight = 120.0f; g_lightingSel = 5; any = true; }
            if (!L.hasWeather && ImGui::MenuItem("Weather")) { L.hasWeather = true; L.overcast = 0.6f; L.rain = 0.5f; L.wetGround = 0.0f; g_lightingSel = 6; any = true; }
            // Clouds live in the atmosphere sky, so inserting them brings it along.
            if (!L.hasClouds && ImGui::MenuItem("Clouds")) { L.hasClouds = true; L.hasSky = true; L.skyMode = 1; g_lightingSel = 7; any = true; }
            if (!L.hasPicture && ImGui::MenuItem("Picture")) { L.hasPicture = true; g_lightingSel = 8; any = true; }
            if (any) {
                SetSelection({}, nullptr);
                if (g_selectedWater) { g_selectedWater->isSelected = false; g_selectedWater = nullptr; }
                if (g_selectedTerrain) { g_selectedTerrain->isSelected = false; g_selectedTerrain = nullptr; }
                BasicTerrain::SetActive(nullptr);
                terrain::GetTerrainEditorState().selectedTerrainLegacy = nullptr;
            }
            if (L.hasSun && L.hasAmbient && L.hasSky && L.hasFog && L.hasWeather && L.hasClouds && L.hasPicture) ImGui::TextDisabled("Everything is inserted.");
            ImGui::EndPopup();
        }
        struct Row { int id; const char* name; bool present; bool removable; };
        const Row rows[] = { { 1, "Time", true, false }, { 2, "Sun", L.hasSun, true }, { 3, "Ambient", L.hasAmbient, true },
                             { 4, "Sky", L.hasSky, true }, { 7, "Clouds", L.hasClouds, true }, { 5, "Fog", L.hasFog, true },
                             { 6, "Weather", L.hasWeather, true }, { 8, "Picture", L.hasPicture, true } };
        static int s_removeId = 0;
        for (const Row& r : rows) {
            if (!r.present) continue;
            ImGui::PushID(100 + r.id);
            bool rightClicked = false;
            const bool clicked = ExplorerRowIcon(ROWICON_OBJECT, r.name, g_lightingSel == r.id, &rightClicked);
            if (clicked && !ExplorerRenameActive() && !g_explorerMenuOpen) {
                SetSelection({}, nullptr);
                if (g_selectedWater) { g_selectedWater->isSelected = false; g_selectedWater = nullptr; }
                if (g_selectedTerrain) { g_selectedTerrain->isSelected = false; g_selectedTerrain = nullptr; }
                BasicTerrain::SetActive(nullptr);
                terrain::GetTerrainEditorState().selectedTerrainLegacy = nullptr;
                g_lightingSel = r.id;
            }
            if (rightClicked) {
                rightClickHandled = true;
                if (r.removable) { s_removeId = r.id; ImGui::OpenPopup("##LightingRowMenu"); }
            }
            ImGui::PopID();
        }
        if (ImGui::BeginPopup("##LightingRowMenu")) {
            if (ImGui::MenuItem("Remove (back to default)")) {
                const gfx::LightingSettings def;
                switch (s_removeId) {
                    case 2: L.hasSun = false; L.sunFollowsTime = def.sunFollowsTime; L.sunAzimuth = def.sunAzimuth; L.sunElevation = def.sunElevation; L.sunIntensity = def.sunIntensity;
                            for (int i = 0; i < 3; i++) L.sunColor[i] = def.sunColor[i]; break;
                    case 3: L.hasAmbient = false; L.ambient = def.ambient; break;
                    case 4: L.hasSky = false; L.skyMode = def.skyMode; for (int i = 0; i < 3; i++) { L.skyColor[i] = def.skyColor[i]; L.airColor[i] = def.airColor[i]; L.hazeColor[i] = def.hazeColor[i]; L.groundColor[i] = def.groundColor[i]; }
                            L.airDensity = def.airDensity; L.haze = def.haze; L.ozone = def.ozone; L.sunSize = def.sunSize; L.moonSize = def.moonSize;
                            L.moonPhase = def.moonPhase; L.moonLight = def.moonLight; L.stars = def.stars;
                            L.hasClouds = false; break;     // no atmosphere, no clouds in it
                    case 5: L.hasFog = false; L.fogDensity = 0.0f; L.fogHeight = def.fogHeight; break;
                    case 6: L.hasWeather = false; L.overcast = 0.0f; L.rain = 0.0f; L.wetGround = 0.0f; break;
                    case 7: L.hasClouds = false; L.cloudCoverage = def.cloudCoverage; L.cloudDensity = def.cloudDensity; L.cloudBase = def.cloudBase;
                            L.cloudThickness = def.cloudThickness; L.cloudScale = def.cloudScale; L.windSpeed = def.windSpeed; L.windDirection = def.windDirection; break;
                    case 8: L.hasPicture = false; L.toneCurve = def.toneCurve; L.exposure = def.exposure; L.autoExposure = def.autoExposure; L.bloom = def.bloom;
                            L.vignette = def.vignette; L.contrast = def.contrast; L.saturation = def.saturation; L.temperature = def.temperature; L.filmGrain = def.filmGrain; break;
                    default: break;
                }
                if (g_lightingSel == s_removeId) g_lightingSel = 1;
            }
            ImGui::EndPopup();
        }
    }

    SyncVisualScriptEntries();
    if (ScriptRuntime* rt = GetActiveRuntime()) {
        auto& scripts = rt->StandaloneScripts();
        ImGui::Spacing();
        ImGui::TextDisabled("SCRIPTS");

        for (int i = 0; i < static_cast<int>(scripts.size()); ++i) {
            ImGui::PushID(i);
            if (g_scriptRenameIndex == i) {
                static int s_lastRenameScriptIdx = -1;
                bool justOpened = (s_lastRenameScriptIdx != i);
                s_lastRenameScriptIdx = i;
                if (ExplorerRenameField("##scriptrename", g_scriptNameBuffer, sizeof(g_scriptNameBuffer), justOpened)) {
                    auto& s = scripts[i];
                    std::string oldClass = scriptLauncher::MakeClassName(s.typeName.empty() ? s.name : s.typeName);
                    s.name = g_scriptNameBuffer;
                    if (s.typeName == oldClass)
                        s.typeName = scriptLauncher::MakeClassName(s.name);
                    vscript::RenameKey(oldClass, ScriptKey(s));
                    g_scriptRenameIndex = -1;
                } else if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()) {
                    g_scriptRenameIndex = -1;
                }
            } else {
                char label[128];
                snprintf(label, sizeof(label), "%s", scripts[i].name.c_str());
                bool rightClicked = false;
                const std::string key = ScriptKey(scripts[i]);
                bool clicked = ExplorerRowIcon(ROWICON_SCRIPT, label, g_scriptSel.count(key) > 0, &rightClicked);
                if (rightClicked && !g_scriptSel.count(key)) { g_scriptSel = { key }; g_scriptSelAnchor = i; }
                if (clicked && !g_explorerMenuOpen) {
                    const ImGuiIO& io = ImGui::GetIO();
                    if (io.KeyCtrl) {                       // Ctrl: add or remove this one
                        if (!g_scriptSel.erase(key)) g_scriptSel.insert(key);
                        g_scriptSelAnchor = i;
                    } else if (io.KeyShift && g_scriptSelAnchor >= 0) {   // Shift: everything from the anchor to here
                        g_scriptSel.clear();
                        const int lo = std::min(g_scriptSelAnchor, i), hi = std::max(g_scriptSelAnchor, i);
                        for (int k = lo; k <= hi && k < static_cast<int>(scripts.size()); ++k) g_scriptSel.insert(ScriptKey(scripts[k]));
                    } else {                                // plain click: just this one, and edit it
                        g_scriptSel = { key };
                        g_scriptSelAnchor = i;
                        if (i >= 0 && i < static_cast<int>(scripts.size())) {
                            std::string className;
                            scriptLauncher::EditStandaloneScript(scripts[i].name, scripts[i].typeName, &className);
                            if (!className.empty()) {
                                scripts[i].typeName = className;
                                scripts[i].runOnPlay = true;
                            }
                        }
                    }
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
        memcpy(sub, text, static_cast<size_t>(i));
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
    // the same characters back out of ImGui's buffered queue instead - it's
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
        memcpy(sub, g_textBuffer, static_cast<size_t>(g_cursorPos));
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
        bool current = g_selectedObject->GetCollisionAccuracy() == acc;
        if (hovered || current) DrawDropdownBox(item, true);
        DrawTextArial(pcoll::CollisionAccuracyName(acc), item.x + 10.0f, item.y + 5.0f, 13.0f,
            (hovered || current) ? theme::TEXT : theme::TEXT_MUTED);
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
        h += group(g_generalOpen, 3.0f);     // Color swatch, Transparency, Player Collision
        h += group(g_waterNoiseOpen, 4.0f); // Noise: Amplitude, Frequency, Speed, Octaves
        h += group(g_waterFoamOpen, 3.0f);      // Foam: Intensity, Scale, Threshold
        h += group(g_waterDetailOpen, 2.0f);    // Surface detail: Intensity, Scale
        h += group(g_waterReflectOpen, 2.0f);   // Reflections: Strength, Distortion
        h += group(g_waterWakeOpen, 4.0f);      // Wake & spray: Wake, Splash, Foam life, Spray
        h += 10.0f;
        return h;
    }

    float h = 50.0f; // below the title strip
    h += group(true, 6.0f);               // General (color + anchored + can collide + collision + transparency + mass)
    if (g_selectedObject && g_selectedObject->HasModel()) h += rowHeight; // mesh info line
    h += 2 * rowHeight;                                         // texture label + preset dropdown row
    if (g_selectedObject && g_selectedObject->HasModel() && !g_playActive) h += rowHeight; // browse / clear row
    if (g_selectedObject) h += group(g_waterInteractOpen, 3.0f);  // Water Interactions
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

            // Player Collision: on = the player floats and swims; off = falls through.
            DrawTextArial("Player Collision:", labelX, y + 3.0f, 13.0f, theme::TEXT_MUTED);
            {
                Rectangle check = { generalControlX, y, 22.0f, 22.0f };
                bool on = w->GetPlayerCollision();
                bool hovered = CheckCollisionPointRec(mouse, check);
                float t = HoverProgress(29, hovered);
                DrawRectangleRounded(check, 0.2f, 4, Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, t));
                DrawRectangleLinesEx(check, hovered ? 2.0f : 1.0f, hovered ? theme::ACCENT : theme::BORDER);
                if (on) {
                    Vector2 c1 = { check.x + 3.0f, check.y + check.height * 0.5f };
                    Vector2 c2 = { check.x + check.width * 0.5f, check.y + check.height - 5.0f };
                    Vector2 c3 = { check.x + check.width - 3.0f, check.y + 4.0f };
                    DrawLineEx(c1, c2, 2.5f, theme::SUCCESS);
                    DrawLineEx(c2, c3, 2.5f, theme::SUCCESS);
                }
                if (hovered) MarkHand();
                if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    w->SetPlayerCollision(!on);
                    Log("'%s' player collision = %s", w->GetName().c_str(), !on ? "true" : "false");
                }
            }
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

        // Surface detail (capillary droplets/ripples)
        y = DrawGroupHeader(panelRec, y, "Surface detail", g_waterDetailOpen, 107);
        if (g_waterDetailOpen) {
            WaterBody::DetailParams d = w->GetDetailParams();
            float valDI = d.intensity;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_DETAIL_INTENSITY, valDI, "Intensity:", labelX)) { d.intensity = valDI; w->SetDetailParams(d); }
            y += rowHeight;
            float valDS = d.scale;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_DETAIL_SCALE, valDS, "Scale:", labelX)) { d.scale = valDS; w->SetDetailParams(d); }
            y += rowHeight;
        }
        y += groupGap;

        // Reflections (planar mirror of the world around this water)
        y = DrawGroupHeader(panelRec, y, "Reflections", g_waterReflectOpen, 108);
        if (g_waterReflectOpen) {
            WaterBody::ReflectionParams r = w->GetReflectionParams();
            float valRS = r.strength;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_REFLECT_STRENGTH, valRS, "Strength:", labelX)) { r.strength = valRS; w->SetReflectionParams(r); }
            y += rowHeight;
            float valRD = r.distortion;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_REFLECT_DISTORTION, valRD, "Distortion:", labelX)) { r.distortion = valRD; w->SetReflectionParams(r); }
            y += rowHeight;
        }
        y += groupGap;

        // Wake & spray (dynamic water: boat wakes, splashes, foam trails, droplets)
        y = DrawGroupHeader(panelRec, y, "Wake & spray", g_waterWakeOpen, 109);
        if (g_waterWakeOpen) {
            WaterBody::RippleParams rp = w->GetRippleParams();
            float valWK = rp.wakeStrength;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_WAKE_STRENGTH, valWK, "Wake:", labelX)) { rp.wakeStrength = std::max(0.0f, valWK); w->SetRippleParams(rp); }
            y += rowHeight;
            float valSP = rp.splashStrength;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_SPLASH_STRENGTH, valSP, "Splash:", labelX)) { rp.splashStrength = std::max(0.0f, valSP); w->SetRippleParams(rp); }
            y += rowHeight;
            float valFL = rp.foamLifetime;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_FOAM_LIFE, valFL, "Foam life:", labelX)) { rp.foamLifetime = std::clamp(valFL, 0.2f, 60.0f); w->SetRippleParams(rp); }
            y += rowHeight;
            // Spray amount: 0 turns droplets off.
            float valSA = rp.spray ? rp.sprayAmount : 0.0f;
            if (DrawNumberInput({ inputX, y, inputWidth, inputHeight }, FIELD_WATER_SPRAY_AMOUNT, valSA, "Spray:", labelX)) {
                rp.sprayAmount = std::clamp(valSA, 0.0f, 5.0f);
                rp.spray = rp.sprayAmount > 0.0f;
                w->SetRippleParams(rp);
            }
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
        (void)accT;
        if (accSupported) DrawDropdownBox(accCombo, (accHovered && accSupported) || g_collisionPopupOpen);
        else {
            DrawRectangleRounded(accCombo, 0.2f, 4, theme::BG_WIDGET);
            DrawRectangleLinesEx(accCombo, 1.0f, theme::BORDER);
        }
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
            DrawTextArial(FitTextToWidth(meshName, 13.0f, inputWidth).c_str(), inputX, y + 6.0f, 13.0f, theme::TEXT);
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

        // Texture file name, using the whole value column (the buttons have their own row).
        DrawTextArial(FitTextToWidth(texName, 13.0f, inputWidth).c_str(), inputX, y + 3.0f, 13.0f, texColor);

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

        // Browse / clear buttons get their own row under the file name.
        const bool showTexButtons = hasMesh && !g_playActive;
        float texBtnRowY = y;
        if (showTexButtons) y += rowHeight;

        if (showPresetDropdown) {
            // Advance y for dropdown row
            y += rowHeight;
            // Build dropdown items: "None" + preset filenames (stem only)
            std::vector<std::string> items = {"None"};
            for (const auto& p : presets) {
                std::string name = std::filesystem::path(p).stem().string();
                items.push_back(name);
            }

            // The list rescans itself every couple of seconds while open, so no
            // manual refresh button is needed.
            Rectangle dropdownRect = { inputX, y, inputWidth, inputHeight };
            bool dropdownHovered = CheckCollisionPointRec(mouse, dropdownRect);

            // Draw dropdown background
            DrawDropdownBox(dropdownRect, dropdownHovered || g_presetDropdownOpen);
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
            const bool presetActive = g_presetDropdownSelected >= 0 && g_presetDropdownSelected < (int)items.size();
            // A custom texture isn't one of the presets; say so instead of a stale prompt.
            std::string currentText = presetActive ? items[g_presetDropdownSelected]
                : (g_selectedObject && !g_selectedObject->GetTexturePath().empty() ? "Custom texture" : "Presets...");
            float textX = dropdownRect.x + 8.0f;
            float textY = dropdownRect.y + (dropdownRect.height - 13.0f) * 0.5f;
            DrawTextArial(FitTextToWidth(currentText, 13.0f, dropdownRect.width - 34.0f).c_str(), textX, textY, 13.0f,
                          presetActive ? theme::TEXT : theme::TEXT_DIM);

            // Draw dropdown arrow
            float arrowX = dropdownRect.x + dropdownRect.width - 20.0f;
            float arrowY = dropdownRect.y + dropdownRect.height * 0.5f;
            DrawTriangle(
                { arrowX - 5.0f, arrowY - 3.0f },
                { arrowX + 5.0f, arrowY - 3.0f },
                { arrowX, arrowY + 3.0f },
                theme::TEXT
            );

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

        // Advance past texture section: always 1 more row to get past the
        // dropdown/error row (or the label row if neither is shown). Must run
        // for primitives too, not just meshes, or the Position header overlaps
        // the dropdown.
        y += rowHeight;

        // Browse + clear buttons (only for imported meshes)
        if (showTexButtons) {
            const float clearW = 24.0f, gap = 4.0f;
            Rectangle browseBtn = { inputX, texBtnRowY, inputWidth - clearW - gap, 22.0f };
            Rectangle clearBtn = { inputX + inputWidth - clearW, texBtnRowY, clearW, 22.0f };

            bool browseHovered = CheckCollisionPointRec(mouse, browseBtn);
            float browseT = HoverProgress(200, browseHovered);
            DrawRectangleRounded(browseBtn, 0.2f, 4, Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, browseT));
            DrawRectangleLinesEx(browseBtn, browseHovered ? 2.0f : 1.0f, browseHovered ? theme::ACCENT : theme::BORDER);
            if (browseHovered) MarkHand();
            const char* browseLabel = "Browse...";
            const float browseLabelW = MeasureTextArial(browseLabel, 12.0f);
            DrawTextArial(browseLabel, browseBtn.x + (browseBtn.width - browseLabelW) * 0.5f, browseBtn.y + 4.0f, 12.0f, theme::TEXT);

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
            const Color clearTint = anySelectedHasTexture ? theme::TEXT : theme::TEXT_DIM;
            if (g_deleteIcon.id != 0) {
                DrawTexturePro(g_deleteIcon,
                    { 0, 0, (float)g_deleteIcon.width, (float)g_deleteIcon.height },
                    { clearBtn.x + 4.0f, clearBtn.y + 3.0f, clearBtn.width - 8.0f, clearBtn.height - 6.0f },
                    { 0, 0 }, 0.0f, clearTint);
            } else {
                DrawTextArial("X", clearBtn.x + 8.0f, clearBtn.y + 4.0f, 12.0f, clearTint);
            }

            if (browseHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                // Non-blocking: the result is delivered by PumpAssetBrowserDialogs()
                // on a later frame, not here.
                BeginChooseTexturePath(currentProject.path);
            }
            if (clearHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !currentProject.path.empty()) {
                for (auto* obj : g_selection) {
                    if (obj && obj->HasModel()) {
                        obj->ClearTexture(currentProject.path);
                    }
                }
            }
        }
    }
    y += groupGap;

    // --- Water Interactions ---
    if (g_selectedObject) {
        y = DrawGroupHeader(panelRec, y, "Water Interactions", g_waterInteractOpen, 110);
        if (g_waterInteractOpen) {
            struct WaterOpt { const char* label; bool ScatteredObject::WaterInteraction::* member; int hoverId; };
            const WaterOpt opts[3] = {
                { "Floats:", &ScatteredObject::WaterInteraction::floats, 26 },
                { "Foam:",   &ScatteredObject::WaterInteraction::foam,   27 },
                { "Splash:", &ScatteredObject::WaterInteraction::splash, 28 },
            };
            for (const WaterOpt& o : opts) {
                DrawTextArial(o.label, labelX, y + 3.0f, 13.0f, theme::TEXT_MUTED);
                Rectangle check = { generalControlX, y, 22.0f, 22.0f };
                bool on = g_selectedObject->waterInteraction.*(o.member);
                bool hovered = CheckCollisionPointRec(mouse, check);
                float t = HoverProgress(o.hoverId, hovered);
                DrawRectangleRounded(check, 0.2f, 4, Mix(theme::BG_INPUT, theme::BG_INPUT_HOVER, t));
                DrawRectangleLinesEx(check, hovered ? 2.0f : 1.0f, hovered ? theme::ACCENT : theme::BORDER);
                if (on) {
                    Vector2 c1 = { check.x + 3.0f, check.y + check.height * 0.5f };
                    Vector2 c2 = { check.x + check.width * 0.5f, check.y + check.height - 5.0f };
                    Vector2 c3 = { check.x + check.width - 3.0f, check.y + 4.0f };
                    DrawLineEx(c1, c2, 2.5f, theme::SUCCESS);
                    DrawLineEx(c2, c3, 2.5f, theme::SUCCESS);
                }
                if (hovered) MarkHand();
                if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    bool nv = !on;
                    g_selectedObject->waterInteraction.*(o.member) = nv;
                    for (auto* obj : g_selection) if (obj && obj != g_selectedObject) obj->waterInteraction.*(o.member) = nv;
                    Log("'%s' %s %s", g_selectedObject->GetName().c_str(), o.label, nv ? "true" : "false");
                }
                y += rowHeight;
            }
        }
        y += groupGap;
    }

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

// Cuts `text` to fit `maxWidth`, ending in "..." when it had to be shortened.
static std::string FitTextToWidth(const std::string& text, float fontSize, float maxWidth) {
    if (MeasureTextArial(text.c_str(), fontSize) <= maxWidth) return text;
    std::string out = text;
    while (!out.empty() && MeasureTextArial((out + "...").c_str(), fontSize) > maxWidth) out.pop_back();
    return out + "...";
}

static std::string BaseName(const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

static void DrawScriptErrorPanel() {
    Rectangle panel = GetScriptPanelBounds();
    if (panel.width <= 0.0f) return;

    constexpr float textSize = 12.0f;
    const Vector2 mouse = GetMousePosition();
    const bool failed = g_scriptBuildState == ScriptBuildState::Failed;

    int errors = 0, warnings = 0;
    for (const auto& d : g_scriptDiagnostics) {
        if (d.severity == ScriptDiagnostic::Severity::Error) ++errors;
        else if (d.severity == ScriptDiagnostic::Severity::Warning) ++warnings;
    }

    DrawRectangleRec({ panel.x + 3.0f, panel.y + 3.0f, panel.width, panel.height }, theme::SHADOW);
    DrawRectangleRec(panel, Color{ 20, 21, 26, 240 });
    DrawRectangleLinesEx(panel, 1.0f, failed ? theme::DANGER : theme::BORDER_STRONG);

    // --- header -----------------------------------------------------------
    std::string title;
    if (!failed) {
        title = "COMPILING SCRIPTS...";
    } else {
        title = "SCRIPT BUILD FAILED - " + std::to_string(errors) + (errors == 1 ? " error" : " errors");
        if (warnings > 0) title += ", " + std::to_string(warnings) + (warnings == 1 ? " warning" : " warnings");
    }
    DrawTextArial(title.c_str(), panel.x + 10.0f, panel.y + 6.0f, 13.0f, failed ? theme::DANGER : theme::ACCENT);

    if (failed) {
        // [Rebuild] and [X] on the right
        Rectangle closeBtn = { panel.x + panel.width - 24.0f, panel.y + 4.0f, 18.0f, 18.0f };
        Rectangle rebuildBtn = { closeBtn.x - 70.0f, panel.y + 4.0f, 64.0f, 18.0f };

        const bool rebuildHover = CheckCollisionPointRec(mouse, rebuildBtn);
        const float rebuildT = HoverProgress(9700, rebuildHover);
        DrawRectangleRounded(rebuildBtn, 0.2f, 4, Mix(theme::BG_WIDGET, theme::ACCENT_PRESSED, rebuildT));
        DrawRectangleLinesEx(rebuildBtn, 1.0f, Mix(theme::BORDER, theme::ACCENT, rebuildT));
        DrawCenteredTextArial("Rebuild", rebuildBtn, 12.0f, rebuildHover ? theme::TEXT : theme::TEXT_MUTED);
        if (rebuildHover) MarkHand();
        if (rebuildHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (ScriptRuntime* rt = GetActiveRuntime())
                if (auto* host = rt->GetHost()) host->RequestScriptRebuild();
        }

        const bool closeHover = CheckCollisionPointRec(mouse, closeBtn);
        const float closeT = HoverProgress(9701, closeHover);
        DrawRectangleRounded(closeBtn, 0.2f, 4, Mix(theme::BG_WIDGET, theme::DANGER, closeT));
        DrawRectangleLinesEx(closeBtn, 1.0f, Mix(theme::BORDER, theme::DANGER_HOVER, closeT));
        DrawCenteredTextArial("X", closeBtn, 12.0f, closeHover ? theme::TEXT : theme::TEXT_MUTED);
        if (closeHover) MarkHand();
        if (closeHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_scriptPanelDismissed = true;
    }
    DrawLine(static_cast<int>(panel.x + 6.0f), static_cast<int>(panel.y + kScriptPanelHeaderH),
             static_cast<int>(panel.x + panel.width - 6.0f), static_cast<int>(panel.y + kScriptPanelHeaderH), theme::DIVIDER);
    if (!failed) return;

    // --- rows -------------------------------------------------------------
    const int total = static_cast<int>(g_scriptDiagnostics.size());
    const int visible = std::min(kScriptPanelMaxRows, total);
    const int maxScroll = std::max(0, total - visible);
    const bool overPanel = CheckCollisionPointRec(mouse, panel);
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f && overPanel) g_scriptPanelScroll -= static_cast<int>(wheel * 2.0f);
    g_scriptPanelScroll = std::clamp(g_scriptPanelScroll, 0, maxScroll);

    const float rowsTop = panel.y + kScriptPanelHeaderH + 2.0f;
    const float textRight = panel.x + panel.width - (maxScroll > 0 ? 16.0f : 10.0f);

    for (int i = 0; i < visible; ++i) {
        const ScriptDiagnostic& d = g_scriptDiagnostics[static_cast<size_t>(g_scriptPanelScroll + i)];
        const Rectangle row = { panel.x + 4.0f, rowsTop + static_cast<float>(i) * kScriptPanelRowH,
                                panel.width - 8.0f, kScriptPanelRowH };
        const bool clickable = !d.file.empty();
        const bool hover = clickable && CheckCollisionPointRec(mouse, row);
        if (hover) {
            DrawRectangleRec(row, theme::BG_ROW_HOVER);
            MarkHand();
        }

        Color sevColor = theme::TEXT_MUTED;
        const char* sev = "note";
        if (d.severity == ScriptDiagnostic::Severity::Error) { sevColor = theme::DANGER; sev = "error"; }
        else if (d.severity == ScriptDiagnostic::Severity::Warning) { sevColor = Color{ 226, 178, 70, 255 }; sev = "warning"; }
        const bool isNote = d.severity == ScriptDiagnostic::Severity::Note;
        const float ty = row.y + (row.height - textSize) * 0.5f;

        float x = row.x + 6.0f + (isNote ? 14.0f : 0.0f);
        DrawTextArial(sev, x, ty, textSize, sevColor);
        x += 52.0f;

        if (clickable) {
            std::string where = BaseName(d.file);
            if (d.line > 0) where += ":" + std::to_string(d.line);
            DrawTextArial(where.c_str(), x, ty, textSize, theme::ACCENT);
            x += MeasureTextArial(where.c_str(), textSize) + 10.0f;
        }
        const std::string msg = FitTextToWidth(d.message, textSize, textRight - x);
        DrawTextArial(msg.c_str(), x, ty, textSize, isNote ? theme::TEXT_DIM : theme::TEXT);

        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            scriptLauncher::RequestOpenAt(d.file, d.line, d.column);
    }

    if (maxScroll > 0) {
        const float sbX = panel.x + panel.width - 10.0f;
        const float sbH = static_cast<float>(visible) * kScriptPanelRowH;
        DrawRectangle(static_cast<int>(sbX), static_cast<int>(rowsTop), 4, static_cast<int>(sbH), Color{ 255, 255, 255, 18 });
        float thumbH = std::max(12.0f, sbH * static_cast<float>(visible) / static_cast<float>(total));
        const float thumbY = rowsTop + (sbH - thumbH) * (static_cast<float>(g_scriptPanelScroll) / static_cast<float>(maxScroll));
        DrawRectangle(static_cast<int>(sbX), static_cast<int>(thumbY), 4, static_cast<int>(thumbH), theme::BORDER);
    }
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
    DrawScriptErrorPanel();

    // Terrain editor UI
    DrawTerrainEditorUI();

    // Floating popups (topmost within the 2D pass, before the script editor)
    DrawColorPickerPopupWindow();
    DrawCollisionPopup();

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

        // Draw items
        Vector2 mousePos = GetMousePosition();
        int visibleCount = (int)items.size();
        for (int i = 0; i < visibleCount; ++i) {
            Rectangle itemRect = { listRect.x, listRect.y + i * itemHeight,
                                 listRect.width, itemHeight };
            bool itemHovered = CheckCollisionPointRec(mousePos, itemRect);

            const bool itemActive = itemHovered || i == g_presetDropdownSelected;
            if (itemActive) DrawDropdownBox(itemRect, true);

            float itemTextY = itemRect.y + (itemRect.height - 13.0f) * 0.5f;
            DrawTextArial(items[i].c_str(), itemRect.x + 8.0f, itemTextY, 13.0f,
                itemActive ? theme::TEXT : theme::TEXT_MUTED);
        }

        // Close if clicked outside list and dropdown (handled in early click processing)
    }

    // Terrain tool dropdown list (drawn after panels so it's on top of everything)
    if (g_terrainToolDropdownListPending && g_terrainToolDropdownOpen) {
        // Defensive: make sure nothing upstream left a scissor rect active
        EndScissorMode();

        constexpr int numTools = 9;
        const char* toolNames[numTools] = { "Raise", "Lower", "Smooth", "Flatten", "Paint", "Erode", "Noise", "Ramp", "None" };
        Rectangle listRec = g_terrainToolDropdownListRect;
        int activeTool = g_terrainToolDropdownActiveTool;

        // Draw list background
        DrawRectangleRec({ listRec.x + 3.0f, listRec.y + 3.0f, listRec.width, listRec.height }, theme::SHADOW);
        DrawRectangleRounded(listRec, 0.08f, 4, theme::BG_MENU);
        DrawRectangleLinesEx(listRec, 1.0f, theme::BORDER_STRONG);

        Vector2 mouse = GetMousePosition();
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
            if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_clickConsumedThisFrame) {
                auto newTool = static_cast<BasicTerrain::Tool>(i);
                if (newTool == BasicTerrain::Tool::Paint) {
                    // Note: layer count logic would need the terrain reference here
                    // For now, just set the tool; the panel will update on next frame
                }
                // The actual tool change is handled in the panel's finishPanel lambda
                // This deferred rendering is purely for visual display
                // The click will be processed in the panel next frame
            }
        }
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

    ApplyAccentToImGuiStyle();
}

// Settings of the selected Lighting item, shown over the properties area.
static void DrawImGuiLightingPanel() {
    if (g_lightingSel == 0) return;
    Rectangle rec = GetPropertiesPanelBounds();
    ImGui::SetNextWindowPos(ImVec2(rec.x, rec.y));
    ImGui::SetNextWindowSize(ImVec2(rec.width, rec.height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##LightingProps", nullptr, flags);
    ImGui::TextColored(theme::AccentVec(), "LIGHTING");
    gfx::LightingSettings& L = gfx::Lighting();
    switch (g_lightingSel) {
    case 1: {
        ImGui::TextDisabled("Time");
        ImGui::Separator();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##hour", &L.timeOfDay, 0.0f, 24.0f, "");
        const int totalMin = static_cast<int>(L.timeOfDay * 60.0f + 0.5f) % (24 * 60);
        ImGui::Text("Game time  %02d:%02d", totalMin / 60, totalMin % 60);
        if (ImGui::Button("Dawn")) L.timeOfDay = 6.5f;
        ImGui::SameLine();
        if (ImGui::Button("Noon")) L.timeOfDay = 12.0f;
        ImGui::SameLine();
        if (ImGui::Button("Dusk")) L.timeOfDay = 18.0f;
        ImGui::SameLine();
        if (ImGui::Button("Night")) L.timeOfDay = 23.0f;
        ImGui::Spacing();
        ImGui::TextDisabled("Day length (real minutes per day)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat("##daylen", &L.dayLengthMinutes, 0.1f, 0.0f, 240.0f, L.dayLengthMinutes > 0.0f ? "%.1f min" : "off (time stays put)");
        ImGui::Spacing();
        ImGui::TextWrapped("The whole scene follows this time: sun and sky, window and street lights, car and bus lights. It is saved with the scene.");
        break;
    }
    case 2:
        ImGui::TextDisabled("Sun");
        ImGui::Separator();
        ImGui::Checkbox("Follow the time of day", &L.sunFollowsTime);
        ImGui::TextDisabled(L.sunFollowsTime ? "Direction at noon (compass, degrees)" : "Direction (compass, degrees)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##az", &L.sunAzimuth, 0.0f, 360.0f, "%.0f");
        ImGui::TextDisabled(L.sunFollowsTime ? "Height at noon" : "Height above the horizon");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##el", &L.sunElevation, 8.0f, 90.0f, "%.0f");
        ImGui::TextDisabled("Intensity");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##sint", &L.sunIntensity, 0.0f, 3.0f, "%.2f");
        ImGui::ColorEdit3("Colour", L.sunColor, ImGuiColorEditFlags_NoInputs);
        ImGui::Spacing();
        if (L.sunFollowsTime) ImGui::TextWrapped("The sun rises at 6:00, crosses the sky (15 degrees of compass an hour), peaks at the height above at noon and sets at 18:00, turning orange near the horizon. Shadows move with it. Untick to pin the sun in place.");
        else ImGui::TextWrapped("Pinned: the sun stays where you put it. A low sun gives long shadows. The time of day still dims it at night.");
        break;
    case 3:
        ImGui::TextDisabled("Ambient");
        ImGui::Separator();
        ImGui::TextDisabled("Strength (1 = default)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##amb", &L.ambient, 0.0f, 2.0f, "%.2f");
        ImGui::Spacing();
        ImGui::TextWrapped("The light that reaches everything, shadows included. Lower it for a moodier scene.");
        break;
    case 4: {
        ImGui::TextDisabled("Sky");
        ImGui::Separator();
        if (ImGui::RadioButton("Atmosphere", L.skyMode == 1)) L.skyMode = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("One colour", L.skyMode != 1)) L.skyMode = 0;
        ImGui::Spacing();
        if (L.skyMode != 1) {
            ImGui::ColorEdit3("Daytime sky", L.skyColor, ImGuiColorEditFlags_NoInputs);
            ImGui::Spacing();
            ImGui::TextWrapped("The background colour. It darkens towards night, and the fog uses it.");
            break;
        }
        // The same air with different numbers: each preset only sets the sliders below.
        struct SkyPreset { const char* name; float air[3]; float density, haze; float hazeColor[3]; float ozone; };
        static const SkyPreset presets[] = {
            { "Earth",       { 0.175f, 0.410f, 1.0f }, 1.0f, 2.0f, { 1.0f, 1.0f, 1.0f }, 1.0f },
            { "Clear",       { 0.175f, 0.410f, 1.0f }, 1.0f, 0.6f, { 1.0f, 1.0f, 1.0f }, 1.0f },
            { "Hazy",        { 0.175f, 0.410f, 1.0f }, 1.1f, 7.0f, { 1.0f, 0.97f, 0.92f }, 1.0f },
            { "Dusty",       { 1.0f, 0.55f, 0.25f },   0.7f, 6.0f, { 1.0f, 0.70f, 0.45f }, 0.0f },
            { "Green",       { 0.25f, 1.0f, 0.45f },   1.0f, 1.5f, { 1.0f, 1.0f, 1.0f }, 0.3f },
            { "Violet",      { 0.62f, 0.30f, 1.0f },   1.2f, 2.0f, { 1.0f, 0.9f, 1.0f }, 0.0f },
            { "Thin",        { 0.175f, 0.410f, 1.0f }, 0.2f, 0.3f, { 1.0f, 1.0f, 1.0f }, 1.0f },
        };
        int col = 0;
        for (const SkyPreset& p : presets) {
            if (col++ % 4 != 0) ImGui::SameLine();
            if (ImGui::Button(p.name)) {
                for (int i = 0; i < 3; i++) { L.airColor[i] = p.air[i]; L.hazeColor[i] = p.hazeColor[i]; }
                L.airDensity = p.density; L.haze = p.haze; L.ozone = p.ozone;
            }
        }
        ImGui::Spacing();
        ImGui::ColorEdit3("What the air scatters", L.airColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float);
        ImGui::TextDisabled("Air (thin .. thick)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##airdensity", &L.airDensity, 0.0f, 4.0f, "%.2f");
        ImGui::TextDisabled("Haze");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##haze", &L.haze, 0.0f, 12.0f, "%.2f");
        ImGui::ColorEdit3("Haze colour", L.hazeColor, ImGuiColorEditFlags_NoInputs);
        ImGui::TextDisabled("Ozone (blue at dusk)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##ozone", &L.ozone, 0.0f, 3.0f, "%.2f");
        ImGui::ColorEdit3("Far ground", L.groundColor, ImGuiColorEditFlags_NoInputs);
        ImGui::Spacing();
        ImGui::TextDisabled("Sun size");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##sunsize", &L.sunSize, 0.3f, 6.0f, "%.2f");
        ImGui::TextDisabled("Moon size");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##moonsize", &L.moonSize, 0.3f, 6.0f, "%.2f");
        ImGui::TextDisabled("Moon phase (new .. full .. new)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##moonphase", &L.moonPhase, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Moonlight");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##moonlight", &L.moonLight, 0.0f, 4.0f, "%.2f");
        ImGui::TextDisabled("Stars");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##stars", &L.stars, 0.0f, 4.0f, "%.2f");
        ImGui::Spacing();
        ImGui::TextWrapped("Air round a planet, lit by the sun: blue overhead, pale at the horizon, red at sunset, and the sunlight on the scene takes its colour from it. The Sun item sets where the sun is and how strong; the moon is opposite it.");
        break;
    }
    case 5:
        ImGui::TextDisabled("Fog");
        ImGui::Separator();
        ImGui::TextDisabled("Density");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##fog", &L.fogDensity, 0.0f, 0.03f, "%.4f");
        ImGui::TextDisabled("Height it thins over (m)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##fogheight", &L.fogHeight, 0.0f, 1500.0f, L.fogHeight > 0.0f ? "%.0f" : "the same at every height");
        ImGui::Spacing();
        ImGui::TextWrapped("Distant things fade into the fog. About 0.004 is a light haze, 0.02 is thick. With a height the fog lies on the ground and the sky shows above it; under an atmosphere sky the sun lights it, and shadows cut shafts through it.");
        break;
    case 7:
        ImGui::TextDisabled("Clouds");
        ImGui::Separator();
        if (ImGui::Button("Few")) { L.cloudCoverage = 0.22f; L.cloudDensity = 1.0f; }
        ImGui::SameLine();
        if (ImGui::Button("Scattered")) { L.cloudCoverage = 0.45f; L.cloudDensity = 1.0f; }
        ImGui::SameLine();
        if (ImGui::Button("Broken")) { L.cloudCoverage = 0.68f; L.cloudDensity = 1.2f; }
        ImGui::SameLine();
        if (ImGui::Button("Overcast")) { L.cloudCoverage = 1.0f; L.cloudDensity = 1.5f; }
        ImGui::Spacing();
        ImGui::TextDisabled("Cover");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##cloudcover", &L.cloudCoverage, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Thickness of the cloud itself");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##clouddensity", &L.cloudDensity, 0.2f, 3.0f, "%.2f");
        ImGui::TextDisabled("Base height (m)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##cloudbase", &L.cloudBase, 200.0f, 8000.0f, "%.0f");
        ImGui::TextDisabled("Tallest clouds (m)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##cloudthick", &L.cloudThickness, 300.0f, 5000.0f, "%.0f");
        ImGui::TextDisabled("Size");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##cloudscale", &L.cloudScale, 0.3f, 3.0f, "%.2f");
        ImGui::TextDisabled("Wind (m/s)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##windspeed", &L.windSpeed, 0.0f, 60.0f, "%.0f");
        ImGui::TextDisabled("Wind blows towards (compass, degrees)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##winddir", &L.windDirection, 0.0f, 360.0f, "%.0f");
        ImGui::Spacing();
        if (!(L.hasSky && L.skyMode == 1)) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Clouds need the Sky item set to Atmosphere.");
        ImGui::TextWrapped("A layer of cloud you can fly through and above. The sun and the sky light it, its shadow drifts over the ground, and the Weather item's cloud cover and rain add to it.");
        break;
    case 8: {
        ImGui::TextDisabled("Picture");
        ImGui::Separator();
        if (ImGui::Button("Neutral")) { L.toneCurve = 1; L.contrast = 1.0f; L.saturation = 1.0f; L.temperature = 0.0f; L.vignette = 0.0f; L.filmGrain = 0.0f; L.bloom = 0.4f; }
        ImGui::SameLine();
        if (ImGui::Button("Filmic")) { L.toneCurve = 2; L.contrast = 1.04f; L.saturation = 1.05f; L.temperature = 0.05f; L.vignette = 0.25f; L.filmGrain = 0.08f; L.bloom = 0.5f; }
        ImGui::SameLine();
        if (ImGui::Button("Soft")) { L.toneCurve = 3; L.contrast = 1.0f; L.saturation = 1.08f; L.temperature = 0.0f; L.vignette = 0.15f; L.filmGrain = 0.0f; L.bloom = 0.6f; }
        ImGui::SameLine();
        if (ImGui::Button("Vivid")) { L.toneCurve = 1; L.contrast = 1.1f; L.saturation = 1.25f; L.temperature = 0.0f; L.vignette = 0.1f; L.filmGrain = 0.0f; L.bloom = 0.5f; }
        ImGui::Spacing();
        ImGui::TextDisabled("Tone curve");
        ImGui::SetNextItemWidth(-1.0f);
        const char* curves[] = { "None (clips)", "Neutral (colours as given)", "ACES filmic", "AgX" };
        ImGui::Combo("##tonecurve", &L.toneCurve, curves, 4);
        ImGui::TextDisabled("Exposure (stops)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##exposure", &L.exposure, -4.0f, 4.0f, "%+.2f");
        ImGui::Checkbox("The eye adapts to the light", &L.autoExposure);
        ImGui::TextDisabled("Bloom");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##bloom", &L.bloom, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Contrast");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##contrast", &L.contrast, 0.5f, 1.5f, "%.2f");
        ImGui::TextDisabled("Saturation");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##saturation", &L.saturation, 0.0f, 2.0f, "%.2f");
        ImGui::TextDisabled("Temperature (cool .. warm)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##temperature", &L.temperature, -1.0f, 1.0f, "%+.2f");
        ImGui::TextDisabled("Vignette");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##vignette", &L.vignette, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Film grain");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##grain", &L.filmGrain, 0.0f, 1.0f, "%.2f");
        ImGui::Spacing();
        ImGui::TextWrapped("How the lit scene becomes the picture. The scene is lit in real light levels (the sun's disc and lamps are far brighter than white paper); the tone curve brings that onto the screen and bloom spreads what is very bright. The eye only adapts under an atmosphere sky. How much the machine spends on all this is in Preferences > Rendering.");
        break;
    }
    case 6:
        ImGui::TextDisabled("Weather");
        ImGui::Separator();
        if (ImGui::Button("Clear")) { L.overcast = 0.0f; L.rain = 0.0f; L.wetGround = 0.0f; }
        ImGui::SameLine();
        if (ImGui::Button("Overcast")) { L.overcast = 0.8f; L.rain = 0.0f; L.wetGround = 0.0f; }
        ImGui::SameLine();
        if (ImGui::Button("Rain")) { L.overcast = 0.6f; L.rain = 0.5f; }
        ImGui::SameLine();
        if (ImGui::Button("Storm")) { L.overcast = 1.0f; L.rain = 1.0f; }
        ImGui::Spacing();
        ImGui::TextDisabled("Cloud cover");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##cloud", &L.overcast, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Rain");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##rain", &L.rain, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Wet ground (rain wets it too)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##wet", &L.wetGround, 0.0f, 1.0f, "%.2f");
        ImGui::Spacing();
        ImGui::TextWrapped("Clouds dim and soften the sun and grey the sky. Rain falls around the camera, makes roads dark with puddles that mirror the sky, adds haze, and the city's pedestrians put up umbrellas.");
        break;
    default: break;
    }
    ImGui::End();
}

void SelectLightingItem(int id) { g_lightingSel = id; }
void SelectBottomTab(int tab) { g_bottomTabRequest = tab; }
int BottomTabShown() { return g_bottomTabActive; }
bool OutputHasUnread() { return g_logTotal != g_logSeenTotal; }

void DrawImGuiFrame(const Camera3D& camera) {
    if (IsKeyPressed(KEY_F1)) g_showDemoWindow = !g_showDemoWindow;
    if (IsKeyPressed(KEY_F2)) g_showDebugStats = !g_showDebugStats;

    DrawImGuiTopBar();
    DrawImGuiExplorer();
    DrawImGuiLightingPanel();
    DrawImGuiExplorerMenu();
    DrawImGuiAssetBrowser(camera);
    DrawImGuiAssetBrowserPopups();
    scriptLauncher::DrawImGuiModal();
    DrawImGuiPreferencesWindow();
    DrawExportDialog();
    city::DrawCityEditorPanel();
    PumpAssetBrowserDialogs();
    DrawPathPromptModal();

    // Debug stats window (Frustum culling, draw calls, etc.)
    if (g_showDebugStats) {
        ImGui::Begin("Debug Stats", &g_showDebugStats, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("RENDERING");
        ImGui::Separator();
        ImGui::Text("Draw Calls:  %d", gfx::GetDrawCallCount());
        ImGui::Text("Triangles:   %d (%.2f K)", gfx::GetTriangleCount(), gfx::GetTriangleCount() / 1000.0f);
        ImGui::Text("Meshes:      %d", gfx::GetMeshCount());
        ImGui::Text("Materials:   %d%s", gfx::GetMaterialCount(), gfx::GetStatsSaturated() ? "+" : "");
        ImGui::Text("Textures:    %d%s", gfx::GetTextureCount(), gfx::GetStatsSaturated() ? "+" : "");
        ImGui::Text("Rendered:    %d", gfx::GetRenderedEntityCount());
        ImGui::TextColored(ImVec4(1, 0.6f, 0, 1), "Culled:      %d", gfx::GetCulledEntityCount());
        int total = gfx::GetRenderedEntityCount() + gfx::GetCulledEntityCount();
        if (total > 0) {
            float pct = (float)gfx::GetCulledEntityCount() / total * 100.0f;
            ImGui::Text("Cull Rate:   %.1f%%", pct);
        }
        ImGui::Separator();
        ImGui::Text("Frustum Culling: %s", gfx::GetFrustumCullingEnabled() ? "ON" : "OFF");
        if (ImGui::Button(gfx::GetFrustumCullingEnabled() ? "Disable Culling" : "Enable Culling")) {
            gfx::SetFrustumCullingEnabled(!gfx::GetFrustumCullingEnabled());
        }
        // Per-pass wall time. Engine::Draw() has always computed these; they
        // were only readable from the stress harness, so there was no way to
        // see which pass a slow frame went into from inside the editor.
        ImGui::Separator();
        ImGui::Text("PASS TIMING (ms)");
        ImGui::Separator();
        {
            const gfx::FrameTimings t = gfx::GetFrameTimings();
            const double total = t.shadowMs + t.reflectionMs + t.skyMs + t.opaqueMs +
                                 t.transparentMs + t.postMs + t.twoDMs;
            // Show each pass as a share of the frame so the expensive one is
            // obvious without doing arithmetic against a frame budget.
            auto pass = [](const char* label, double ms, double total) {
                ImGui::Text("%-12s %7.3f  %5.1f%%", label, ms,
                            total > 0.0 ? (ms / total) * 100.0 : 0.0);
            };
            pass("Shadow", t.shadowMs, total);
            pass("Reflection", t.reflectionMs, total);
            pass("Sky", t.skyMs, total);
            pass("Opaque", t.opaqueMs, total);
            pass("Transparent", t.transparentMs, total);
            pass("Post", t.postMs, total);
            pass("2D/UI", t.twoDMs, total);
            ImGui::Text("%-12s %7.3f", "TOTAL", total);
        }
        ImGui::End();
    }

    ImGuiIO& io = ImGui::GetIO();

    if (g_showDemoWindow) {
        ImGui::ShowDemoWindow(&g_showDemoWindow);
    }

    // Only take over the cursor while the pointer is over an ImGui window;
    // elsewhere the raylib panels drive their own hand/ibeam behavior.
    if (io.WantCaptureMouse) {
        rlImGuiUpdateMouseCursor();
    }

    ImGui::Render();
    rlImGuiRender();

    // Viewport right-click menu is hand-drawn (2D), so paint it after the ImGui
    // pass or every ImGui window (asset browser, etc.) covers it.
    DrawContextMenu();
}

} // namespace ui
