#pragma once
#include "../Backend/Entity.hpp"
#include "../include/Engine/Backend/ScatteredObject.hpp"
#include "ui.hpp"
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
    float dragStartAngle = 0.0f;
    float rotateSign = 1.0f; // flips rotation direction depending on which side of the axis the camera is on
    Vector3 startPos = { 0.0f, 0.0f, 0.0f };
    Vector3 dragPlanePoint = { 0.0f, 0.0f, 0.0f }; // mouse-ray hit on the drag plane at press
    Vector3 dragPlaneNormal = { 0.0f, 1.0f, 0.0f }; // camera view direction at press
    float dragAxisWorldLen = 1.0f;              // grabbed axis handle offset (0.7 * size) in world units
    Vector3 startSize = { 1.0f, 1.0f, 1.0f };
    Vector3 startRotation = { 0.0f, 0.0f, 0.0f };
    Vector3 rotatePrevEuler = { 0.0f, 0.0f, 0.0f }; // continuous (unwrapped) Euler for single-object rotate drag
    Vector3 dragPivot = { 0.0f, 0.0f, 0.0f };   // world pivot (selection center) captured at drag start
    std::vector<DragStartState> dragStartStates; // per-object state at drag start

    void SyncSelection();
    bool IsInSelection(ScatteredObject* object) const;
    void ToggleSelect(ScatteredObject* object);
    void ToggleModel(ModelGroup* model);
    void DeleteObject(ScatteredObject* target);
    void DeleteSelection();
    void DeleteWaterBody(WaterBody* target);
    void RemoveObjectFromScene(ScatteredObject* target);
    void RemoveEmptyModels();

    // Undo/redo support.
    bool CaptureSnapshot(std::string& bytes, std::vector<std::string>& sel) const;
    void PushUndoNow();               // snapshot current scene and push as undo step
    void BeginDragUndoCapture();      // remember pre-drag scene to commit later
    void EndDragUndoCapture();        // push the pre-drag snapshot only if the scene changed
    void DoUndo();
    void DoRedo();
    void RestoreFromSnapshot(const std::string& bytes, const std::vector<std::string>& selNames);
    void SelectObjectsByName(const std::vector<std::string>& names);

    UndoSystem undo;
    std::string pendingUndoBytes;
    std::vector<std::string> pendingUndoSel;
    bool hasPendingUndo = false;

    // Top-bar Import button: file dialog -> copy into project -> spawn + select.
    // Starts the (non-blocking) mesh file picker. FinishMeshImport() is called
    // on a later frame with the chosen path, or an empty path if cancelled.
    void ImportMesh();
    void FinishMeshImport(const std::string& modelPath);

    // Delivers results from the non-blocking file pickers (scene open/save,
    // mesh import). Called first thing in Update() so a result is never
    // starved behind one of Update()'s early returns.
    void PumpDialogs();
    void CleanupPendingImports();
    void ClearPendingImports();
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
    std::vector<std::string> pendingImportDirs;
};
