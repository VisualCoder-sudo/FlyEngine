#pragma once

#include "raylib.h"
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward declarations for global types
class Engine;
class CameraController;

// Forward declare ui namespace types
namespace ui {
    enum class TransformTool;
}

// Forward declare terrain namespace
namespace terrain {
    class Terrain;
}

// Forward declare water namespace
class WaterBody;
class BasicTerrain;

class ScatteredObject;
class ModelGroup;
namespace phys { class Simulation; }

namespace ui {

enum class MenuAction {
    None,
    SpawnCube,
    SpawnSphere,
    SpawnCylinder,
    SpawnWedge,
    SpawnTerrain,
    SpawnBasicTerrain,
    SpawnWater,
    SpawnCity,
    SpawnCityOrganic,
    ImportTerrain,
    DeleteObject,
    DeleteWaterBody,
    ToggleTransformSpace,
    Save,
    SaveAs,
    OpenScene
};

enum class TransformTool {
    Select,
    Move,
    Scale,
    Rotate,
    Terrain
};

void Init();

// Scene edits made from the ImGui panels (Explorer) record an undo step through this hook, which the interaction
// manager installs. PushUndo() snapshots the scene as it is NOW, so call it before changing anything.
void SetUndoHook(void (*hook)(void*), void* user);
void PushUndo();
// Forgets the Explorer's standalone-script selection (after undo/redo swapped the list).
void ClearScriptSelection();
void Unload();

// Selected object & scene management
void SetSelectedObject(Vector3* pos, Vector3* size, Color* color);
void SetSelectedObject(Vector3* pos, Vector3* size, Color* color, Vector3* rotation, Vector3* origin = nullptr);
void SetSelectedObject(ScatteredObject* object);
void SetSceneObjects(const std::vector<ScatteredObject*>* objects);
// Models are mutable: Group/Ungroup add/remove containers through this pointer.
void SetSceneModels(std::vector<std::unique_ptr<ModelGroup>>* models);
void SetSimulation(phys::Simulation* sim);

// Multi-selection. ui owns the selection set; the 3D interaction manager reads
// it every frame and calls SetSelection() when the user picks in the viewport.
const std::vector<ScatteredObject*>& GetSelection();
ScatteredObject* GetPrimarySelection();
void SetSelection(const std::vector<ScatteredObject*>& selection, ScatteredObject* primary);
// Groups the current selection into a new model, or dissolves the model a given
// object belongs to. Used by the explorer's context menu.
void GroupSelectedObjects();
// Ctrl+G / Ctrl+U. Ungroup acts on the outermost groups that are fully selected, or else the group of the
// primary selection. Both return false when there was nothing to do.
bool CanGroupSelection();
bool UngroupSelection(bool dryRun = false);
void UngroupModelContaining(ScatteredObject* member);
// Ctrl+D: groups the duplicates (copies[i] came from sources[i]); fully duplicated groups are copied whole.
void PlaceDuplicates(const std::vector<ScatteredObject*>& sources, const std::vector<ScatteredObject*>& copies);
// Explorer actions that need to remove entities defer the actual deletion to
// the interaction manager (which owns the engine + object vector).
// `undoRecorded`: the caller's own undo step (taken before it changed the scene) already covers the delete.
void RequestDelete(const std::vector<ScatteredObject*>& targets, bool undoRecorded = false);
// `undoRecorded` (optional) is true when every pending request had its undo step recorded already.
std::vector<ScatteredObject*> ConsumePendingDelete(bool* undoRecorded = nullptr);
// Explorer "Duplicate": same as Ctrl+D on the current selection, run by the interaction manager.
void RequestDuplicate();
// The next selection change will not unfold the explorer to its row (duplicates stay as folded as the source).
void SuppressExplorerReveal();
bool ConsumeDuplicateRequest();

// Solid modelling on the current selection (see ScatteredObject::MakeUnion). The explorer menu and shortcuts only
// request it; the interaction manager, which owns the engine and the object list, does the work.
enum class UnionRequest { None, Union, Intersect, Separate };
void RequestUnion(UnionRequest request);
UnionRequest ConsumeUnionRequest();
// Union/Intersect: two or more plain primitives are selected. Separate: a union is selected.
bool CanUnionSelection();
bool CanSeparateSelection();

// Transform tools getter
TransformTool GetActiveTool();

// Transform space: true = Local, false = World
bool IsTransformLocalSpace();
void SetTransformLocalSpace(bool local);
void ToggleTransformSpace();

// Grid snapping for the transform gizmos. Holding Ctrl while dragging inverts `enabled`.
struct SnapSettings {
    bool enabled = true;
    float move = 0.5f;    // world units
    float rotate = 15.0f; // degrees
    float scale = 0.1f;   // size units (uniform scale: ratio)
    bool toObject = false; // Move: snap the selection's bounds flush against nearby objects
};
SnapSettings& GetSnapSettings();

// Dark readout next to the cursor while dragging a transform gizmo (distance, size or angle).
bool& GizmoValueLabel();

// Play mode: the top-bar Play/Stop button signals a toggle via ConsumePlayToggle(),
// which the physics simulation consumes once. SetPlayActive mirrors the active state.
bool ConsumePlayToggle();
bool IsPlayActive();
void SetPlayActive(bool active);

// For the scene test (Flyengine --testscene ... sky), which cannot click: selects a row of the
// Explorer's Lighting section (0 = none, 1 Time, 2 Sun, 3 Ambient, 4 Sky, 5 Fog, 6 Weather,
// 7 Clouds, 8 Picture), and opens or closes Preferences on one of its tabs (4 = Rendering).
void SelectLightingItem(int id);
// Switches the bottom panel to a tab (0 = Assets, 1 = Output) on the next frame; BottomTabShown() is the one showing.
void SelectBottomTab(int tab);
int BottomTabShown();
bool OutputHasUnread();     // lines were written that the Output tab has not shown (the red dot)
void ShowPreferences(bool show, int category);
// Called once before an AI reply's code block changes the scene, so the whole block is one undo step.
void SetAIBeforeApplyHook(std::function<void()> hook);

// The top-bar Import button requests a mesh import; the interaction manager
// consumes the request once per frame it was set (never while play is active).
bool ConsumeImportRequest();

// "Get started with Flyengine!" guided tour (ui_tutorial.cpp); DrawTutorial once per frame, last, from DrawImGuiFrame.
void OpenTutorial();
bool IsTutorialActive();
void DrawTutorial();

// Export (File > Export...), see ExportDialog.cpp.
void OpenExportDialog();
void DrawExportDialog();                           // once per frame, from DrawImGuiFrame
bool ConsumeExportRequest();                       // the dialog's Export button was pressed: save the project, then...
void StartExport(const std::string& projectDir);   // ...call this (the saved files are what gets packed)
bool IsExporting();
// Asset-browser drag released over the viewport: model path + ground position.
bool ConsumeModelSpawnRequest(std::string& outPath, Vector3& outPos);

// Commits/blurs any active text field (properties number input or rename box),
// used when the command console takes focus.
void BlurAllInput();

Font GetFont(); // shared UI font used by the command console

// Water body selection (single active selection for gizmo + properties)
void SetSelectedWater(WaterBody* w);
WaterBody* GetSelectedWater();

// BasicTerrain selection (single active selection for explorer + properties)
void SetSelectedTerrain(BasicTerrain* t);
BasicTerrain* GetSelectedTerrain();

// Area of the screen between the explorer (left) and properties (right)
// panels. Used by the command console to size its bar instead of spanning
// the full screen width.
Rectangle GetConsoleBarArea();

// On-screen output box: appends a printf-style message to a small persistent
// log panel drawn above the command console. Used by the script runtime and
// physics to show what's currently going on.
enum class LogSeverity : int { Neutral = 0, Success = 1, Error = 2 };
void Log(const char* fmt, ...);
void LogAlways(const char* fmt, ...);
void LogWithSeverity(LogSeverity severity, const char* fmt, ...);
void ClearLog();
// Copies every log line (script Print included) to `file` as it is written, flushed per line, so a run can be read
// without opening the in-engine console. Pass nullptr to stop. The caller owns the FILE.
void SetLogMirror(std::FILE* file);

// Re-skins the editor's accent colour (selection highlights, panel headings,
// buttons, sliders). Hover/pressed shades are derived from it. Exposed to
// plugins as CP_EngineAPI::ui_set_accent_color.
void SetAccentColor(Color accent);

// Script build status, shown as a panel over the top of the viewport while the
// project's Scripts/*.cpp compile and, when a build fails, listing each
// compiler error (click one to open the file at that line). Fed by
// NativeScriptHost; the panel stays hidden for Idle and Succeeded.
enum class ScriptBuildState : int { Idle = 0, Building, Succeeded, Failed };
struct ScriptDiagnostic {
    enum class Severity : int { Note = 0, Warning, Error };
    Severity severity = Severity::Error;
    std::string file;   // absolute path; empty when the message has no location
    int line = 0;       // 1-based, 0 = unknown
    int column = 0;
    std::string message;
};
void SetScriptBuildState(ScriptBuildState state, std::vector<ScriptDiagnostic> diagnostics = {});

// UI State Checks
bool IsEditingText();
bool IsMouseOverUI();
void UpdateInput();

// Context Menu Management
void OpenContextMenu(Vector2 mousePos, bool isObjectTarget, bool isWaterTarget);
void CloseContextMenu();
bool IsContextMenuOpen();
MenuAction ProcessContextMenu();
// Queues a spawn/menu action from an ImGui panel. `atViewportCenter` places spawned things in front of the
// camera instead of at the last viewport right-click.
void RequestMenuAction(MenuAction action, bool atViewportCenter);
bool ConsumeActionAtViewportCenter();

// True for the rest of the frame after a mouse press was consumed by a menu
// (item click or dismiss), so the same click never leaks into the world.
void MarkUIClickConsumed();
bool WasUIClickConsumed();

// Terrain editor functions
void InitTerrainEditor();
void ShutdownTerrainEditor();
void UpdateTerrainEditor(class ::Engine& engine, class ::CameraController* cameraCtrl, class phys::Simulation* physicsSim);
void DrawTerrainEditorUI();
bool IsTerrainEditorActive();
void SetTerrainEditorMode(TransformTool mode);
void HandleTerrainSelection(terrain::Terrain* terrainObj, bool selected);
void DrawTerrainBrushPreview(const Camera3D& camera);
void RequestHeightmapImport();

// Render UI
void Draw();

// Dear ImGui frame: builds the (currently debug) ImGui window set, then flushes
// raylib's render batch and draws the ImGui draw data on top. Call after the
// raylib 2D pass. Toggle the demo window with F1. `camera` is used by the
// asset browser to raycast a world position when a model is dragged out of
// it and dropped over the viewport.
void DrawImGuiFrame(const Camera3D& camera);

} // namespace ui