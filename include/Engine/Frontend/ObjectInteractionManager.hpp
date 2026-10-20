#pragma once
#include "MoveModule.hpp"
#include "../Backend/Entity.hpp"
#include "../include/Engine/Backend/ScatteredObject.hpp"
#include "ui.hpp"
#include "PluginHost.hpp"
#include "../Backend/UndoSystem.hpp"
#include "raylib.h"
#include <memory>
#include <string>
#include <vector>

class Engine;
class CameraController;

// Editor 3D interaction: selection (single + multi), model grouping, and the
// transform gizmos. The selection set itself lives in ui (ui::GetSelection /
// ui::GetPrimarySelection); this class syncs a local copy each frame and asks
// ui::SetSelection to apply viewport clicks.
class ObjectInteractionManager : public Entity {
public:
    ObjectInteractionManager(Engine& engine, Camera3D& camera, CameraController* cameraController,
                             std::vector<ScatteredObject*>& objects,
                             std::vector<std::unique_ptr<ModelGroup>>& models,
                             phys::Simulation* physicsSim = nullptr);
    ~ObjectInteractionManager() override;
    void Update(float dt) override;
    void DrawOverlay3D() override;

    bool IsEditorOnly() const override { return true; }

    void SetPhysicsSimulation(phys::Simulation* ps) { physicsSim = ps; }

private:
    struct DragStartState {
        Vector3 pos = { 0.0f, 0.0f, 0.0f };
        Vector3 size = { 1.0f, 1.0f, 1.0f };
        Vector3 rotation = { 0.0f, 0.0f, 0.0f };
        Vector3 prevEulerRad = { 0.0f, 0.0f, 0.0f }; // continuous (unwrapped) Euler during a rotate drag
    };

    Engine& engine;
    Camera3D& camera;
    CameraController* cameraController;
    pluginhost::SceneHost pluginScene;   // what plugins may do to the scene (spawn, remove)
    std::vector<ScatteredObject*>& objects;
    std::vector<std::unique_ptr<ModelGroup>>& models;
    phys::Simulation* physicsSim = nullptr;
    ScatteredObject* contextMenuTarget = nullptr;

    std::vector<ScatteredObject*> selectedObjects; // cached copy of ui::GetSelection()

    Vector2 rmbPressPos = { 0.0f, 0.0f };
    float rmbDragDistance = 0.0f;
    bool rmbPressInViewport = false;
    int activeHandle = -1;
    bool freeDrag = false; // Move tool body drag: objects follow cursor on the camera plane
    Vector2 dragStart = { 0.0f, 0.0f };
    Vector3 startPos = { 0.0f, 0.0f, 0.0f };
    Vector3 dragPlanePoint = { 0.0f, 0.0f, 0.0f }; // mouse-ray hit on the drag plane at press
    Vector3 dragPlaneNormal = { 0.0f, 1.0f, 0.0f }; // camera view direction at press
    MoveModule axisMove;                        // shared axis drag (objects + water)
    // Gizmo drag readout, set while a handle is dragged: distance (Move), size on the axis (Scale) or degrees (Rotate).
    float dragReadout = 0.0f;
    Vector3 rotateRefDir = { 1.0f, 0.0f, 0.0f };  // Rotate: ring-plane direction under the cursor at press (0 degrees)
    float rotateAccumRad = 0.0f;                  // Rotate: cursor sweep since press, unwrapped
    float rotateLastAngle = 0.0f;                 // Rotate: cursor angle on the ring plane last frame
    Vector3 rotateAxisDir = { 0.0f, 1.0f, 0.0f }; // Rotate: world axis being turned about
    float dragAxisWorldLen = 1.0f;              // grabbed axis handle offset (0.7 * size) in world units
    Vector3 startSize = { 1.0f, 1.0f, 1.0f };
    Vector3 startRotation = { 0.0f, 0.0f, 0.0f };
    Vector3 rotatePrevEuler = { 0.0f, 0.0f, 0.0f }; // continuous (unwrapped) Euler for single-object rotate drag
    Vector3 dragPivot = { 0.0f, 0.0f, 0.0f };   // world pivot (selection center) captured at drag start
    std::vector<DragStartState> dragStartStates; // per-object state at drag start
    BoundingBox dragStartBounds{};               // union of the selection's world bounds at drag start
    std::vector<BoundingBox> snapTargets;        // bounds of every unselected object at drag start (Snap to Object)

    void DrawDragReadout() const;
    void DrawGizmos() const; // one pass of the transform gizmos (DrawOverlay3D calls it twice)
    float SnapToObjectThreshold() const; // capture range in world units, grows with camera distance
    void SyncSelection();
    bool IsInSelection(ScatteredObject* object) const;
    void ToggleSelect(ScatteredObject* object);
    void ToggleModel(ModelGroup* model);
    void DeleteObject(ScatteredObject* target);
    void DeleteSelection();
    // Deletes whatever is selected (water, terrain, or objects). False if nothing was.
    bool DeleteSelectedAnything();
    void DeleteWaterBody(WaterBody* target);
    void RemoveObjectFromScene(ScatteredObject* target);
    void RemoveEmptyModels();

    // Undo/redo support.
    bool CaptureSnapshot(std::string& bytes, std::vector<int>& sel) const;
    void PushUndoNow();               // snapshot current scene and push as undo step
    void BeginDragUndoCapture();      // remember pre-drag scene to commit later
    void EndDragUndoCapture();        // push the pre-drag snapshot only if the scene changed
    void DoUndo();
    void DoRedo();
    void RestoreFromSnapshot(const std::string& bytes, const std::vector<int>& selIndices);
    void SelectObjectsByIndex(const std::vector<int>& indices);

    // Playtest isolation: whatever scripts, the console or physics do during
    // Play is temporary. The objects are snapshotted when Play starts and, if
    // the scene differs when it stops, put back exactly as they were (objects
    // created during play disappear). Cities are not part of it: nothing at
    // play time can change them, and rebuilding them is slow.
    void CapturePlaySnapshot();
    void RestorePlaySnapshot();
    std::string playSnapshotBytes;
    std::vector<std::pair<std::string, std::string>> playSnapshotRecords;   // per object, to report and verify the restore
    std::vector<int> playSnapshotSel;
    bool playSnapshotValid = false;
    bool wasPlaying = false;

    UndoSystem undo;
    std::string pendingUndoBytes;
    std::vector<int> pendingUndoSel;
    bool hasPendingUndo = false;

    // Top-bar Import button: file dialog -> copy into project -> spawn + select.
    // Starts the (non-blocking) mesh file picker. FinishMeshImport() is called
    // on a later frame with the chosen path, or an empty path if cancelled.
    void ImportMesh();
    void FinishMeshImport(const std::string& modelPath);
    // Spawns an already-in-project model file at a world position. The mesh is
    // parsed on a worker thread; the object appears a few frames later.
    void SpawnModelAt(const std::string& absModelPath, Vector3 pos);
    void SpawnPrefabAt(const std::string& absPrefabPath, Vector3 pos);
    void PumpPendingSpawns();
    void CompleteSpawn(const std::string& absModelPath, Vector3 pos);
    struct PendingSpawn { std::string path; Vector3 pos; };
    std::vector<PendingSpawn> pendingSpawns;

    // Delivers results from the non-blocking file pickers (scene open/save,
    // mesh import). Called first thing in Update() so a result is never
    // starved behind one of Update()'s early returns.
    void PumpDialogs();
    void CleanupPendingImports();
    int PickHandle(Vector2 mouse) const;
    ScatteredObject* PickObject(Ray ray) const;
    ModelGroup* PickModel(Ray ray) const;
    WaterBody* PickWaterBody(Ray ray) const;
    // What a viewport left-click selects. Without ALT, clicking a member of a
    // model (or its bounding box) resolves to the whole model; ALT forces
    // individual-object picking.
    struct ClickTarget {
        std::vector<ScatteredObject*> objects; // objects to select
        ScatteredObject* primary = nullptr;    // primary selection of the click
        ModelGroup* model = nullptr;           // non-null when the click is a whole-model pick
    };
    ClickTarget PickClickTargets(Ray ray, bool altHeld) const;
    bool IsExactSelection(const std::vector<ScatteredObject*>& targets) const;
    Vector3 SelectionCenter() const;
    void BeginSelectionDrag();

    // Directories created by ImportMesh that haven't been saved yet.
    // Cleaned up on OpenScene and editor exit; cleared on Save.
};
