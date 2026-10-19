// ui.cpp (fixed)
#include "ui_internal.hpp"
#include "../../../include/Engine/Backend/VisualScript.hpp"
#include "../../../include/Terrain/Water/WaterBody.hpp"
#include "../../../include/Engine/Scripts/CommandConsole.hpp"
#include "../../../include/Engine/Graphics.hpp"
#include "../../../include/Engine/LoadingScreen.hpp"
#include "../include/Engine/Scripts/ScriptLauncher.hpp"
#include "../../../include/Engine/Backend/TextureManager.hpp"
#include "../include/Engine/Frontend/ProjectManager.hpp"
#include "../../../include/Terrain/TerrainEditor.hpp"
#include "../../../include/Terrain/TerrainRegistry.hpp"
#include "../../../include/CityGen/CityEditor.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Terrain/BasicTerrain.hpp"
#include "../include/Engine/Backend/PhysicsSimulation.hpp"
#include "rlimgui.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>

namespace ui {

static Font g_arialFont = { 0 };
Texture2D g_toolIcons[5] = {};
Texture2D g_deleteIcon = { 0 };
Texture2D g_rowIcons[ROWICON_COUNT] = {};
// Asset browser: folder + search glyph (mesh/water/terrain/script entries
// reuse g_rowIcons above; the up-arrow reuses g_terrainToolIcons[TTOOLICON_LOWER]).
Texture2D g_folderIcon = { 0 };
Texture2D g_assetSearchIcon = { 0 };
Texture2D g_terrainToolIcons[TTOOLICON_COUNT] = {};
Vector3* g_pos = nullptr;
Vector3* g_size = nullptr;
Color* g_color = nullptr;
Vector3* g_rotation = nullptr;
Vector3* g_origin = nullptr;
const std::vector<ScatteredObject*>* g_sceneObjects = nullptr;
std::vector<std::unique_ptr<ModelGroup>>* g_sceneModels = nullptr;

// A script is one thing: a C++ class and/or a node graph, found by the class name. Keys the graph by that name.
std::string ScriptKey(const ScriptRuntime::StandaloneScript& s) {
    return scriptLauncher::MakeClassName(s.typeName.empty() ? s.name : s.typeName);
}

ScatteredObject* g_selectedObject = nullptr;
phys::Simulation* g_simulation = nullptr;
std::vector<ScatteredObject*> g_selection;
ScatteredObject* g_primarySelection = nullptr;
int g_explorerAnchorIndex = -1; // index into the flattened explorer list for shift-range
ScatteredObject* g_renameObject = nullptr;
ModelGroup* g_renameModel = nullptr;
BasicTerrain* g_renameTerrain = nullptr;
terrain::Terrain* g_renameLegacyTerrain = nullptr;
ScatteredObject* g_lastExplorerClick = nullptr;
ModelGroup* g_lastExplorerModelClick = nullptr;
double g_lastExplorerClickTime = 0.0;
WaterBody* g_selectedWater = nullptr;
int g_lightingSel = 0;   // the selected Lighting row of the explorer: 0 none, 1 Time, 2 Sun, 3 Ambient, 4 Sky, 5 Fog, 6 Weather, 7 Clouds, 8 Picture
BasicTerrain* g_selectedTerrain = nullptr;
char g_nameBuffer[64] = "";
float g_colorPickerAnchorY = -1.0f;


// Preset texture dropdown state
bool g_presetDropdownOpen = false;
int g_presetDropdownSelected = -1;  // -1 = none chosen, 0 = "None", 1+ = preset index
std::vector<std::string> g_presetDropdownItems;  // project-relative paths
bool g_presetDropdownItemsCached = false;
std::string g_presetDropdownProjectDir;  // to detect project change
double g_presetDropdownLastRefresh = 0.0;  // for auto-refresh

// Preset dropdown list rendering state (for drawing after scissor ends)
bool g_presetDropdownListPending = false;
Rectangle g_presetDropdownListRect = { 0 };
Rectangle g_presetDropdownHeaderRect = { 0 };
std::vector<std::string> g_presetDropdownListItems;
std::vector<std::string> g_presetDropdownListPresets;
std::string g_presetDropdownListProjectDir;

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

// ---------------------------------------------------------------------------
// Anonymous namespace: internal linkage definitions
// ---------------------------------------------------------------------------
namespace {



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
} // namespace
void MarkIbeam() { g_cursorIbeam = true; }
namespace {

void EndCursorPass() {
    SetMouseCursor(g_cursorIbeam ? MOUSE_CURSOR_IBEAM
        : g_cursorHand ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_DEFAULT);
}

} // namespace

// Active Toolbar State
TransformTool g_activeTool = TransformTool::Select;
static bool g_transformLocalSpace = true; // true = Local, false = World
SnapSettings g_snap;

// HSV Color Picker & Popup State
Vector3 g_hsv = { 0.0f, 0.0f, 1.0f };
int g_activeSlider = 0; // 0 = None, 1 = Hue, 2 = Saturation, 3 = Brightness
bool g_showColorPickerWindow = false;
static Color g_vec3PickColor = BLACK;

// Collision accuracy combo popup state (properties panel)
bool g_collisionPopupOpen = false;
double g_collisionPopupOpenTime = 0.0;
Rectangle g_collisionPopupAnchor = { 0.0f, 0.0f, 0.0f, 0.0f };

// Context Menu State
static bool g_menuOpen = false;
static Vector2 g_menuPos = { 0.0f, 0.0f };
static bool g_isObjectTarget = false;
static bool g_isWaterTarget = false;
static bool g_showSubMenu = false;
static double g_menuOpenTime = 0.0;
static bool g_fileMenuOpen = false;
static double g_fileMenuOpenTime = 0.0;
MenuAction g_pendingFileAction = MenuAction::None; // set by the ImGui File menu, consumed by ProcessContextMenu()

// Play Mode State
bool g_playActive = false;
bool g_playTogglePending = false;
static double g_playEndedAt = 0.0;
bool g_importRequested = false;

// Explorer right-click context menu (separate from the 3D-view menu)
bool g_explorerMenuOpen = false;
Vector2 g_explorerMenuPos = { 0.0f, 0.0f };
ExplorerMenuMode g_explorerMenuMode = ExplorerMenuMode::Empty;
ScatteredObject* g_explorerMenuObject = nullptr;
int g_explorerMenuScript = -1;
std::set<std::string> g_scriptSel;   // selected scripts in the Explorer, by ScriptKey (Ctrl/Shift-click)
int g_scriptSelAnchor = -1;
double g_explorerMenuOpenTime = 0.0;

// Vertical scroll offset (pixels) for the explorer panel content
static float g_explorerScrollY = 0.0f;

// Vertical scroll offset (pixels) for the properties panel content
float g_propertiesScrollY = 0.0f;

// Standalone script rename state
int g_scriptRenameIndex = -1;
char g_scriptNameBuffer[64] = "";

FieldID g_activeField = FIELD_NONE;
char g_textBuffer[32] = "";
int g_cursorPos = 0;
int g_selStart = -1;  // selection anchor index, -1 = no selection
bool g_mouseDrag = false;

// Collapsible property-group state (all open by default).
bool g_generalOpen = true;
bool g_waterInteractOpen = true;
bool g_positionOpen = true;
bool g_sizeOpen = true;
bool g_rotationOpen = true;
bool g_originOpen = true;
bool g_linearVelocityOpen = true;
bool g_angularVelocityOpen = true;
bool g_waterNoiseOpen = true;
bool g_waterFoamOpen = true;
bool g_waterDetailOpen = true;
bool g_waterReflectOpen = true;
bool g_waterWakeOpen = true;
bool g_waterUnderOpen = true;

// Screen rect of the currently active number field
Rectangle g_activeFieldRect = { 0.0f, 0.0f, 0.0f, 0.0f };

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
    if (img.data != nullptr) {
        // Row icons are drawn at 14 px; oversized exports are scaled to the 96 px size of the rest of the set.
        if (img.width > 128 || img.height > 128) ImageResize(&img, 96, 96);
        // Icons in explorericons/ keep their own colours; the older black silhouettes are recoloured to white.
        if (name.rfind("explorericons/", 0) != 0) ImageColorBrightness(&img, 255);
    }
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

    constexpr const char* rowIconNames[ROWICON_COUNT] = { "cube.png", "water.png", "terrain.png", "script.png", "explorericons/Group.png" };
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

// Script build status panel (see ui.hpp). Written by SetScriptBuildState,
// read by DrawScriptErrorPanel / IsMouseOverUI.
ScriptBuildState g_scriptBuildState = ScriptBuildState::Idle;
std::vector<ScriptDiagnostic> g_scriptDiagnostics;
double g_scriptBuildStarted = 0.0;
bool g_scriptPanelDismissed = false;
int g_scriptPanelScroll = 0; // first visible row

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

static bool g_duplicateRequested = false;
void RequestDuplicate() { g_duplicateRequested = true; }
bool ConsumeDuplicateRequest() { bool r = g_duplicateRequested; g_duplicateRequested = false; return r; }

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

SnapSettings& GetSnapSettings() {
    return g_snap;
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

// Keeps `candidate` as the primary when it is still selected, otherwise falls
// back to the first remaining selection.
ScatteredObject* KeepPrimary(const std::vector<ScatteredObject*>& selection, ScatteredObject* candidate) {
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

Rectangle GetTopBarBounds() {
    return Rectangle{ 0.0f, 0.0f, static_cast<float>(GetScreenWidth()), 120.0f };
}

Rectangle GetExplorerPanelBounds() {
    constexpr float topBarHeight = 120.0f;
    return Rectangle{ 0.0f, topBarHeight, 220.0f, static_cast<float>(GetScreenHeight()) - topBarHeight };
}

Rectangle GetPropertiesPanelBounds() {
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

void GetColorPickerWindowBounds(Rectangle& popupRec) {
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


// Script build panel: a strip over the top of the viewport. While compiling it
// is a one-line status (shown only once a build has taken a moment, so quick
// rebuilds don't flash it); after a failure it lists the compiler messages.

static bool ScriptPanelVisible() {
    if (g_scriptBuildState == ScriptBuildState::Failed) return !g_scriptPanelDismissed;
    if (g_scriptBuildState == ScriptBuildState::Building)
        return GetTime() - g_scriptBuildStarted > 0.5;
    return false;
}

Rectangle GetScriptPanelBounds() {
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

void DrawCenteredTextArial(const char* text, const Rectangle& rec, float fontSize, Color color) {
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

static bool g_uiClickConsumed = false;

void MarkUIClickConsumed() { g_uiClickConsumed = true; }
bool WasUIClickConsumed() { return g_uiClickConsumed; }

static void (*g_undoHook)(void*) = nullptr;
static void* g_undoHookUser = nullptr;

void SetUndoHook(void (*hook)(void*), void* user) {
    g_undoHook = hook;
    g_undoHookUser = user;
}

void ClearScriptSelection() {
    g_scriptSel.clear();
    g_scriptSelAnchor = -1;
    g_scriptRenameIndex = -1;
}

void PushUndo() {
    if (g_undoHook) g_undoHook(g_undoHookUser);
}

static bool g_pendingAtCenter = false;

void RequestMenuAction(MenuAction action, bool atViewportCenter) {
    g_pendingFileAction = action;
    g_pendingAtCenter = atViewportCenter;
}

bool ConsumeActionAtViewportCenter() {
    bool v = g_pendingAtCenter;
    g_pendingAtCenter = false;
    return v;
}

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


ImTextureID PluginIconTex(const std::string& path) {
    static std::map<std::string, Texture2D> cache;
    if (path.empty()) return static_cast<ImTextureID>(0);
    auto it = cache.find(path);
    if (it == cache.end()) {
        Texture2D tex = {};
        Image img = LoadImage(path.c_str());
        if (img.data != nullptr) {
            if (img.width > 128 || img.height > 128) ImageResize(&img, 96, 96);
            tex = LoadTextureFromImage(img);
            UnloadImage(img);
        }
        it = cache.emplace(path, tex).first;
    }
    return it->second.id != 0 ? static_cast<ImTextureID>(GetTextureImGuiId(it->second)) : static_cast<ImTextureID>(0);
}

ImTextureID RowTex(RowIconType i) {
    return g_rowIcons[i].id != 0
        ? static_cast<ImTextureID>(GetTextureImGuiId(g_rowIcons[i]))
        : static_cast<ImTextureID>(0);
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
        int visibleCount = static_cast<int>(items.size());
        bool clickedItem = false;

        for (int i = 0; i < visibleCount; ++i) {
            Rectangle itemRect = { listRect.x, listRect.y + static_cast<float>(i) * itemHeight,
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
        int visibleCount = static_cast<int>(items.size());
        for (int i = 0; i < visibleCount; ++i) {
            Rectangle itemRect = { listRect.x, listRect.y + static_cast<float>(i) * itemHeight,
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
    DrawTutorial();

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
        int entityTotal = gfx::GetRenderedEntityCount() + gfx::GetCulledEntityCount();
        if (entityTotal > 0) {
            float pct = static_cast<float>(gfx::GetCulledEntityCount()) / static_cast<float>(entityTotal) * 100.0f;
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
            auto pass = [](const char* label, double ms, double frameMs) {
                ImGui::Text("%-12s %7.3f  %5.1f%%", label, ms,
                            frameMs > 0.0 ? (ms / frameMs) * 100.0 : 0.0);
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
