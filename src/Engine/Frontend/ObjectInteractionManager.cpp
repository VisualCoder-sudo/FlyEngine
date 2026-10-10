#include "../../../include/Engine/Frontend/ObjectInteractionManager.hpp"
#include "../../../include/Engine.hpp"
#include "../../../include/Engine/Backend/CameraController.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../include/Terrain/Terrain.hpp"
#include "../../../include/Terrain/TerrainRegistry.hpp"
#include "../../../include/Terrain/TerrainEditor.hpp"
#include "../../../include/Terrain/BasicTerrain.hpp"
#include "../../../include/Terrain/Water/WaterBody.hpp"
#include "../include/Terrain/TerrainIO.hpp"
#include "../../../include/Engine/Backend/ModelImport.hpp"
#include "../include/Engine/Frontend/ProjectManager.hpp"
#include "../../../include/Engine/Backend/ScenePersistence.hpp"
#include "../../../include/Engine/Backend/TextureManager.hpp"
#include "../../../include/CityGen/City.hpp"
#include "../../../include/CityGen/CityEditor.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "raylib.h"
#include "rlgl.h"
#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>

static const std::vector<platform::FileFilter>& MeshFilters() {
    static const std::vector<platform::FileFilter> filters = {
        {"Model Files", "*.obj;*.fbx;*.gltf;*.glb;*.iqm;*.vox;*.m3d"},
        {"All Files", "*"},
    };
    return filters;
}

static float WrapDeg(float a) {
    a = fmodf(a + 180.0f, 360.0f);
    if (a < 0.0f) a += 360.0f;
    return a - 180.0f;
}

// Extract Euler angles (radians) that reproduce the same orientation as "m"
// when fed back through the renderer's MatrixRotateXYZ. IMPORTANT: raylib's
// MatrixRotateXYZ is actually Rz(z)*Ry(y)*Rx(x), which differs from the
// Ra ymath QuaternionFromEuler/QuaternionToEuler order (Rx*Ry*Rz). Round-
// tripping through quaternions therefore corrupted the orientation of angled
// objects. This matrix inversion keeps the stored Euler guaranteed-consistent
// with how the body is actually rendered:
//   MatrixRotateXYZ(MatrixToEulerXYZ(m)) == m
static Vector3 MatrixToEulerXYZ(const Matrix& m) {
    float sb = m.m8;
    if (sb > 1.0f) sb = 1.0f;
    if (sb < -1.0f) sb = -1.0f;
    return { atan2f(-m.m9, m.m10), asinf(sb), atan2f(-m.m4, m.m0) };
}

// Same as MatrixToEulerXYZ but keeps the result continuous with "prev" (the
// previous frame's unwrapped Euler in radians), so the +-180 wrap and the
// pitch-singularity branch don't make the body spin wildly mid-drag. Both the
// primary and the (x+pi, pi-y, z+pi) equivalent branch represent the exact
// same orientation, so either choice keeps rendering correct.
static Vector3 MatrixToEulerContinuous(const Matrix& m, const Vector3& prev) {
    Vector3 e = MatrixToEulerXYZ(m);
    Vector3 alt = { e.x + PI, PI - e.y, e.z + PI };
    auto unwrap = [](float v, float target) {
        float d = v - target;
        d -= 2.0f * PI * roundf(d / (2.0f * PI));
        return target + d;
    };
    auto cost = [&](const Vector3& c) {
        Vector3 u = { unwrap(c.x, prev.x), unwrap(c.y, prev.y), unwrap(c.z, prev.z) };
        float dx = u.x - prev.x, dy = u.y - prev.y, dz = u.z - prev.z;
        return dx * dx + dy * dy + dz * dz;
    };
    if (cost(alt) < cost(e)) {
        return { unwrap(alt.x, prev.x), unwrap(alt.y, prev.y), unwrap(alt.z, prev.z) };
    }
    return { unwrap(e.x, prev.x), unwrap(e.y, prev.y), unwrap(e.z, prev.z) };
}

ObjectInteractionManager::ObjectInteractionManager(Engine& engine, Camera3D& camera, CameraController* cameraController,
                                                   std::vector<ScatteredObject*>& objects,
                                                   std::vector<std::unique_ptr<ModelGroup>>& models,
                                                   phys::Simulation* physicsSim)
    : engine(engine), camera(camera), cameraController(cameraController), objects(objects), models(models), physicsSim(physicsSim) {
    // City editor commits (node drags, regens, deletions) become undo steps.
    city::SetCityEditCallback([this]() { PushUndoNow(); });
}

ObjectInteractionManager::~ObjectInteractionManager() {
    CleanupPendingImports();
}

namespace {

constexpr int RING_SEGMENTS = 72;

Vector3 AxisDirection(int axis) {
    if (axis == 0) return { 1.0f, 0.0f, 0.0f };
    if (axis == 1) return { 0.0f, 1.0f, 0.0f };
    return { 0.0f, 0.0f, 1.0f };
}

// Scale handles follow the object's local geometry axes. This is important
// after rotation: scaling X/Y/Z should mean the object's local X/Y/Z, not
// world X/Y/Z.
Vector3 RotatedAxisDirection(Vector3 rotation, int axis) {
    Matrix rot = MatrixRotateXYZ({
        DEG2RAD * rotation.x,
        DEG2RAD * rotation.y,
        DEG2RAD * rotation.z
    });
    return Vector3Normalize(Vector3Transform(AxisDirection(axis), rot));
}

// Returns the axis direction for a transformation handle honoring the current
// transform-space toggle: local (rotated with the object) or world (axis-aligned).
Vector3 HandleAxisDirection(Vector3 rotation, int axis) {
    if (ui::IsTransformLocalSpace()) return RotatedAxisDirection(rotation, axis);
    return AxisDirection(axis);
}

// World-space scale-corner position honoring the current transform-space toggle.
Vector3 CornerSign(int corner); // fwd decl (defined below)
Vector3 HandleScaleCornerWorld(Vector3 center, Vector3 size, Vector3 rotation, int corner) {
    Vector3 s = CornerSign(corner);
    Vector3 result = center;
    result = Vector3Add(result, Vector3Scale(HandleAxisDirection(rotation, 0), s.x * size.x * 0.5f));
    result = Vector3Add(result, Vector3Scale(HandleAxisDirection(rotation, 1), s.y * size.y * 0.5f));
    result = Vector3Add(result, Vector3Scale(HandleAxisDirection(rotation, 2), s.z * size.z * 0.5f));
    return result;
}

Vector3 RingPoint(Vector3 center, float radius, int axis, float angle) {
    float c = cosf(angle) * radius;
    float s = sinf(angle) * radius;
    if (axis == 0) return { center.x, center.y + c, center.z + s }; // X: YZ plane
    if (axis == 1) return { center.x + c, center.y, center.z + s }; // Y: XZ plane
    return { center.x + c, center.y + s, center.z };                // Z: XY plane
}

// Ring point in object's local space (rotated by object's rotation)
Vector3 RotatedRingPoint(Vector3 center, float radius, Vector3 rotation, int axis, float angle) {
    float c = cosf(angle) * radius;
    float s = sinf(angle) * radius;
    Vector3 local;
    if (axis == 0) local = { 0.0f, c, s };        // Local X: YZ plane
    else if (axis == 1) local = { c, 0.0f, s };   // Local Y: XZ plane
    else local = { c, s, 0.0f };                  // Local Z: XY plane

    Matrix rot = MatrixRotateXYZ({ DEG2RAD * rotation.x, DEG2RAD * rotation.y, DEG2RAD * rotation.z });
    return Vector3Add(center, Vector3Transform(local, rot));
}

float DistanceToSegment(Vector2 point, Vector2 a, Vector2 b) {
    Vector2 ab = Vector2Subtract(b, a);
    float lengthSq = ab.x * ab.x + ab.y * ab.y;
    if (lengthSq <= 0.0001f) return Vector2Distance(point, a);
    Vector2 ap = Vector2Subtract(point, a);
    float t = Clamp((ap.x * ab.x + ap.y * ab.y) / lengthSq, 0.0f, 1.0f);
    return Vector2Distance(point, Vector2Add(a, Vector2Scale(ab, t)));
}

// Intersects a ray with an infinite plane defined by a point and a normal.
Vector3 RayPlaneIntersection(Ray ray, Vector3 planePoint, Vector3 planeNormal) {
    float denom = Vector3DotProduct(ray.direction, planeNormal);
    if (fabsf(denom) < 1e-6f) return ray.position; // Ray parallel to the plane
    float t = Vector3DotProduct(Vector3Subtract(planePoint, ray.position), planeNormal) / denom;
    return Vector3Add(ray.position, Vector3Scale(ray.direction, t));
}

float DistanceToScreenRing(Vector2 mouse, Vector3 center, float radius, int axis, const Camera3D& camera) {
    float closest = 1e9f;
    for (int i = 0; i < RING_SEGMENTS; ++i) {
        float a = (float)i / RING_SEGMENTS * 2.0f * PI;
        float b = (float)(i + 1) / RING_SEGMENTS * 2.0f * PI;
        Vector2 start = GetWorldToScreen(RingPoint(center, radius, axis, a), camera);
        Vector2 end = GetWorldToScreen(RingPoint(center, radius, axis, b), camera);
        closest = fminf(closest, DistanceToSegment(mouse, start, end));
    }
    return closest;
}

float DistanceToScreenRing(Vector2 mouse, Vector3 center, float radius, Vector3 rotation, int axis, const Camera3D& camera) {
    float closest = 1e9f;
    for (int i = 0; i < RING_SEGMENTS; ++i) {
        float a = (float)i / RING_SEGMENTS * 2.0f * PI;
        float b = (float)(i + 1) / RING_SEGMENTS * 2.0f * PI;
        Vector2 start = GetWorldToScreen(RotatedRingPoint(center, radius, rotation, axis, a), camera);
        Vector2 end = GetWorldToScreen(RotatedRingPoint(center, radius, rotation, axis, b), camera);
        closest = fminf(closest, DistanceToSegment(mouse, start, end));
    }
    return closest;
}

void DrawRotationRing(Vector3 center, float radius, int axis, Color color, bool hot = false) {
    const float thickness = fmaxf(0.035f, radius * (hot ? 0.038f : 0.025f));
    for (int i = 0; i < RING_SEGMENTS; ++i) {
        float a = (float)i / RING_SEGMENTS * 2.0f * PI;
        float b = (float)(i + 1) / RING_SEGMENTS * 2.0f * PI;
        DrawCylinderEx(RingPoint(center, radius, axis, a), RingPoint(center, radius, axis, b),
            thickness, thickness, 6, color);
    }
}

void DrawRotationRing(Vector3 center, float radius, Vector3 rotation, int axis, Color color, bool hot = false) {
    const float thickness = fmaxf(0.035f, radius * (hot ? 0.038f : 0.025f));
    for (int i = 0; i < RING_SEGMENTS; ++i) {
        float a = (float)i / RING_SEGMENTS * 2.0f * PI;
        float b = (float)(i + 1) / RING_SEGMENTS * 2.0f * PI;
        DrawCylinderEx(RotatedRingPoint(center, radius, rotation, axis, a), RotatedRingPoint(center, radius, rotation, axis, b),
            thickness, thickness, 6, color);
    }
}

constexpr float SCALE_MIN_SIZE = 0.15f;
constexpr float SCALE_AXIS_HANDLE_PIXELS = 14.0f;
constexpr float SCALE_CORNER_HANDLE_PIXELS = 12.0f;
constexpr float SCALE_PICK_RADIUS = 18.0f;
constexpr float SCALE_HANDLE_MIN_WORLD = 0.12f;
constexpr float SCALE_HANDLE_MAX_WORLD_FRACTION = 0.35f;

constexpr float ROTATION_RING_PIXELS = 90.0f;
constexpr float ROTATION_RING_MIN_WORLD = 0.3f;
constexpr float ROTATION_RING_MAX_WORLD = 5.0f;

// Move gizmo stays at least this many pixels from pivot to tip on screen.
constexpr float MOVE_HANDLE_PIXELS = 28.0f;

// Screen pixels covered by 1 world unit at the given point's depth.
float ScreenPixelsPerUnit(Vector3 pos, const Camera3D& camera) {
    Vector2 a = GetWorldToScreen(pos, camera);
    Vector2 b = GetWorldToScreen(Vector3Add(pos, Vector3{ 1.0f, 0.0f, 0.0f }), camera);
    return Vector2Distance(a, b);
}

// World-space size that renders as roughly `pixels` on screen at the point's depth.
float HandleWorldSize(Vector3 pos, float pixels, const Camera3D& camera) {
    float ppu = ScreenPixelsPerUnit(pos, camera);
    if (ppu < 1e-4f) ppu = 1.0f;
    return pixels / ppu;
}

// Sign multiplier for a bounding-box corner index 0..7 (one bit per axis).
Vector3 CornerSign(int corner) {
    return Vector3{
        (corner & 1) ? 1.0f : -1.0f,
        (corner & 2) ? 1.0f : -1.0f,
        (corner & 4) ? 1.0f : -1.0f
    };
}

float AxisSize(const Vector3& size, int axis) {
    if (axis == 0) return size.x;
    if (axis == 1) return size.y;
    return size.z;
}

// World-space distance from the pivot to the tip of a move-handle axis. Kept
// past the object's bounds so the gizmo reads clearly at any size.
float MoveHandleLen(const Vector3& size, int axis) {
    return fmaxf(AxisSize(size, axis) * 0.5f, 0.8f) + 1.6f;
}

// Move-handle world length that stays past the object's bounds but never
// shrinks below a fixed on-screen size, so the gizmo is always grabbable
// even when the camera is far away (mirrors the rotate ring's behavior).
float MoveHandleWorldLen(Vector3 pos, const Vector3& size, int axis, const Camera3D& camera) {
    float objectLen = MoveHandleLen(size, axis);
    float screenLen = HandleWorldSize(pos, MOVE_HANDLE_PIXELS, camera);
    return fmaxf(objectLen, screenLen);
}

// A scale axis handle whose direction projects to (nearly) a point on screen
// points at the camera and is ambiguous to grab - and dragging it felt like it
// "scaled the wrong way". Such handles are skipped by picking and drawn dimmed.
bool AxisScaleGrabbable(Vector3 pos, Vector3 size, Vector3 rotation, int axis, const Camera3D& camera) {
    Vector3 axisDir = HandleAxisDirection(rotation, axis);
    Vector3 handleWorld = Vector3Add(pos, Vector3Scale(axisDir, AxisSize(size, axis) * 0.5f));
    Vector2 handleScreen = GetWorldToScreen(handleWorld, camera);
    Vector3 probeWorld = Vector3Add(pos, Vector3Scale(axisDir, AxisSize(size, axis) * 0.5f + 1.0f));
    float screenAxisLen = Vector2Length(Vector2Subtract(GetWorldToScreen(probeWorld, camera), handleScreen));
    return screenAxisLen >= 6.0f;
}

} // namespace

// Water body move handle length - capped to keep gizmo proportional.
static float WaterMoveHandleLen(const Vector3& size, int axis) {
    float base = MoveHandleLen(size, axis);
    return fminf(base, 4.0f);
}

// Water body handle picking (Move: X/Y/Z axes at water surface; Scale: X/Z only)
static int PickWaterHandle(WaterBody* water, const Camera3D& cam, Vector2 mouse) {
    Vector3 wp = *water->GetPosPtr();
    Vector3 ws = *water->GetSizePtr();
    float wh = water->GetWaterHeight();
    Vector3 pivot = { wp.x, wh, wp.z };
    ui::TransformTool tool = ui::GetActiveTool();

    if (tool == ui::TransformTool::Move) {
        for (int i = 0; i < 3; ++i) {
            Vector2 tipScreen = GetWorldToScreen(Vector3Add(pivot, Vector3Scale(AxisDirection(i), WaterMoveHandleLen(ws, i))), cam);
            if (Vector2Distance(tipScreen, mouse) < 14.0f) return i;
        }
        for (int i = 0; i < 3; ++i) {
            Vector2 tipScreen = GetWorldToScreen(Vector3Add(pivot, Vector3Scale(AxisDirection(i), WaterMoveHandleLen(ws, i))), cam);
            if (DistanceToSegment(mouse, GetWorldToScreen(pivot, cam), tipScreen) < 8.0f) return i;
        }
    } else if (tool == ui::TransformTool::Scale) {
        for (int i = 0; i < 3; ++i) {
            if (i == 1) continue;
            if (!AxisScaleGrabbable(pivot, ws, {0,0,0}, i, cam)) continue;
            Vector3 handleWorld = Vector3Add(pivot, Vector3Scale(AxisDirection(i), AxisSize(ws, i) * 0.5f));
            if (Vector2Distance(GetWorldToScreen(handleWorld, cam), mouse) < SCALE_PICK_RADIUS) return i;
        }
        for (int cx = 0; cx < 2; ++cx) {
            for (int cz = 0; cz < 2; ++cz) {
                int ci = 3 + cx * 2 + cz;
                Vector3 corner = {
                    pivot.x + (cx == 0 ? -1.0f : 1.0f) * ws.x * 0.5f,
                    pivot.y,
                    pivot.z + (cz == 0 ? -1.0f : 1.0f) * ws.z * 0.5f
                };
                if (Vector2Distance(GetWorldToScreen(corner, cam), mouse) < SCALE_PICK_RADIUS) return ci;
            }
        }
    }
    return -1;
}

void ObjectInteractionManager::SyncSelection() {
    selectedObjects = ui::GetSelection();
}

bool ObjectInteractionManager::IsInSelection(ScatteredObject* object) const {
    return std::find(selectedObjects.begin(), selectedObjects.end(), object) != selectedObjects.end();
}

void ObjectInteractionManager::ToggleSelect(ScatteredObject* object) {
    if (!object) return;
    std::vector<ScatteredObject*> next = selectedObjects;
    auto it = std::find(next.begin(), next.end(), object);
    ScatteredObject* primary = nullptr;
    if (it != next.end()) {
        next.erase(it);
        primary = next.empty() ? nullptr : next.front();
    } else {
        next.push_back(object);
        primary = object;
    }
    ui::SetSelection(next, primary);
}

void ObjectInteractionManager::ToggleModel(ModelGroup* model) {
    if (!model || model->members.empty()) return;

    PushUndoNow();

    bool allSelected = true;
    for (auto* m : model->members) {
        if (!m || !IsInSelection(m)) { allSelected = false; break; }
    }

    std::vector<ScatteredObject*> next = selectedObjects;
    ScatteredObject* primary = nullptr;
    if (allSelected) {
        next.erase(std::remove_if(next.begin(), next.end(),
            [&](ScatteredObject* o) {
                return std::find(model->members.begin(), model->members.end(), o) != model->members.end();
            }), next.end());
        primary = next.empty() ? nullptr : next.front();
    } else {
        for (auto* m : model->members) {
            if (m && std::find(next.begin(), next.end(), m) == next.end()) next.push_back(m);
        }
        primary = model->members.front();
    }
    ui::SetSelection(next, primary);
}

Vector3 ObjectInteractionManager::SelectionCenter() const {
    bool first = true;
    BoundingBox b{};
    for (auto* obj : selectedObjects) {
        if (!obj) continue;
        BoundingBox m = obj->GetBoundingBox();
        if (first) {
            b = m;
            first = false;
        } else {
            b.min.x = fminf(b.min.x, m.min.x);
            b.min.y = fminf(b.min.y, m.min.y);
            b.min.z = fminf(b.min.z, m.min.z);
            b.max.x = fmaxf(b.max.x, m.max.x);
            b.max.y = fmaxf(b.max.y, m.max.y);
            b.max.z = fmaxf(b.max.z, m.max.z);
        }
    }
    if (first) return Vector3{ 0.0f, 0.0f, 0.0f };
    return Vector3{
        (b.min.x + b.max.x) * 0.5f,
        (b.min.y + b.max.y) * 0.5f,
        (b.min.z + b.max.z) * 0.5f
    };
}

void ObjectInteractionManager::RemoveEmptyModels() {
    models.erase(std::remove_if(models.begin(), models.end(),
        [](const std::unique_ptr<ModelGroup>& m) { return !m || m->members.empty(); }), models.end());
}

void ObjectInteractionManager::RemoveObjectFromScene(ScatteredObject* target) {
    if (!target) return;

    // Capture paths before the entity is freed below.
    const std::string modelPath = target->GetModelPath();
    const std::string texturePath = target->GetTexturePath();

    // Unlink from any model container and drop the container once empty.
    if (target->parentModel) {
        ModelGroup* model = target->parentModel;
        model->members.erase(std::remove(model->members.begin(), model->members.end(), target), model->members.end());
        target->parentModel = nullptr;
        RemoveEmptyModels();
    }

    // Remove raw pointer from the management vector
    auto it = std::find(objects.begin(), objects.end(), target);
    if (it != objects.end()) {
        objects.erase(it);
    }

    // Detach any live physics body before the entity is freed below; bodyMap
    // is keyed on the raw ScatteredObject*, so leaving it would dangle and
    // crash the next fixed step when objects are deleted mid-play.
    if (physicsSim) {
        physicsSim->RemoveObject(target);
    }

    // Remove entity from engine (this frees/deletes memory)
    engine.RemoveEntity(target);

    // Release the texture reference so shared/refcounted files can be cleaned up.
    if (!texturePath.empty()) {
        textureManager::UnregisterTexture(texturePath);
    }

    // Delete the model's folder when no remaining object still references it.
    if (!modelPath.empty()) {
        textureManager::RemoveModelDirectory(modelPath, objects);
    }
}

bool ObjectInteractionManager::CaptureSnapshot(std::string& bytes, std::vector<std::string>& sel) const {
    const project::Info& current = project::GetCurrentProject();
    if (!SnapshotSceneToMemory(objects, models, current.path, bytes)) return false;
    bytes += city::SerializeCitiesSnapshot();

    sel.clear();
    for (auto* o : ui::GetSelection()) {
        if (o) sel.push_back(o->GetName());
    }
    return true;
}

void ObjectInteractionManager::PushUndoNow() {
    std::string bytes;
    std::vector<std::string> sel;
    if (CaptureSnapshot(bytes, sel)) {
        undo.Push(std::move(bytes), std::move(sel));
    }
}

void ObjectInteractionManager::BeginDragUndoCapture() {
    pendingUndoBytes.clear();
    pendingUndoSel.clear();
    hasPendingUndo = CaptureSnapshot(pendingUndoBytes, pendingUndoSel);
}

void ObjectInteractionManager::EndDragUndoCapture() {
    if (!hasPendingUndo) return;
    hasPendingUndo = false;

    // Only record the step when the drag actually changed the scene.
    std::string bytes;
    std::vector<std::string> sel;
    if (CaptureSnapshot(bytes, sel) && bytes != pendingUndoBytes) {
        undo.Push(std::move(pendingUndoBytes), std::move(pendingUndoSel));
    }
}

void ObjectInteractionManager::CapturePlaySnapshot() {
    playSnapshotValid = false;
    playSnapshotSel.clear();
    if (!SnapshotSceneToMemory(objects, models, project::GetCurrentProject().path, playSnapshotBytes)) return;
    for (auto* o : ui::GetSelection()) {
        if (o) playSnapshotSel.push_back(o->GetName());
    }
    playSnapshotValid = true;
}

void ObjectInteractionManager::RestorePlaySnapshot() {
    if (!playSnapshotValid) return;
    playSnapshotValid = false;

    // The runtime's list of play-created objects is about to dangle (or already
    // does, once their entities are freed below).
    if (ScriptRuntime* rt = GetActiveRuntime()) rt->ForgetCreatedObjects();

    // Unchanged scene: leave it alone rather than rebuild every object.
    std::string now;
    if (SnapshotSceneToMemory(objects, models, project::GetCurrentProject().path, now) &&
        now == playSnapshotBytes) {
        return;
    }

    const size_t before = objects.size();
    RestoreFromSnapshot(playSnapshotBytes, playSnapshotSel);
    ui::LogAlways("[Play] Scene restored to how it was before Play (%zu object(s) in the scene during play).", before);
    playSnapshotBytes.clear();
}

void ObjectInteractionManager::RestoreFromSnapshot(const std::string& bytes, const std::vector<std::string>& selNames) {
    std::istringstream in(bytes);
    const size_t previousObjectCount = objects.size();

    if (!RestoreSceneFromMemory(in, engine, objects, models, project::GetCurrentProject().path)) {
        return;
    }

    ui::SetSelection({}, nullptr);
    contextMenuTarget = nullptr;

    // Remove the objects that existed before the restore (their replacemnents
    // were appended after them). Mirrors the scene-open cleanup.
    for (size_t i = 0; i < previousObjectCount; ++i) engine.RemoveEntity(objects[i]);
    objects.erase(objects.begin(), objects.begin() + previousObjectCount);

    // Trailing city payload ("\nCITY n ..."); a snapshot without one restores
    // the previous cities untouched.
    city::RestoreCitiesFromSnapshot(in, engine, [this]() -> city::City* {
        auto c = std::make_unique<city::City>();
        city::City* raw = c.get();
        engine.AddEntity(std::move(c));
        return raw;
    });

    // Cities were re-built from the snapshot above; the registry's raw City*
    // slots now point at freshly streamed objects. Re-bind the editor's
    // active-city pointer to the first live one (or clear it) so no frame ever
    // dereferences a city that was freed during the restore.
    {
        const auto& restored = city::GetCityRegistry().GetCities();
        city::City* next = nullptr;
        for (auto* c : restored) {
            if (c && c->alive) { next = c; break; }
        }
        city::SetActiveCity(next);
    }

    SyncSelection();
    SelectObjectsByName(selNames);
}

void ObjectInteractionManager::SelectObjectsByName(const std::vector<std::string>& names) {
    if (names.empty()) return;

    // Map the requested names to the freshly restored objects (first match each).
    std::vector<ScatteredObject*> matches;
    for (const auto& wanted : names) {
        for (auto* obj : objects) {
            if (obj && obj->GetName() == wanted &&
                std::find(matches.begin(), matches.end(), obj) == matches.end()) {
                matches.push_back(obj);
                break;
            }
        }
    }
    if (matches.empty()) return;

    ScatteredObject* primary = matches.front();
    ui::SetSelection(matches, primary);
}

void ObjectInteractionManager::DoUndo() {
    if (!undo.CanUndo()) return;

    // Save (but discard if unchanged) the current state onto the redo stack.
    std::string curBytes;
    std::vector<std::string> curSel;
    if (CaptureSnapshot(curBytes, curSel)) {
        undo.PushRedo(std::move(curBytes), std::move(curSel));
    }

    RestoreFromSnapshot(undo.PeekUndo(), undo.UndoSel());
    undo.PopUndo();
}

void ObjectInteractionManager::DoRedo() {
    if (!undo.CanRedo()) return;

    // Save the current state onto the undo stack so undo can return here. Must
    // NOT use Push() (which clears the redo stack we're about to read).
    std::string curBytes;
    std::vector<std::string> curSel;
    if (CaptureSnapshot(curBytes, curSel)) {
        undo.PushUndo(std::move(curBytes), std::move(curSel));
    }

    RestoreFromSnapshot(undo.PeekRedo(), undo.RedoSel());
    undo.PopRedo();
}

void ObjectInteractionManager::DeleteObject(ScatteredObject* target) {
    if (!target) return;
    RemoveObjectFromScene(target);

    // Drop the target from the live selection so no dangling pointer remains.
    std::vector<ScatteredObject*> next;
    for (auto* o : ui::GetSelection()) {
        if (o && o != target) next.push_back(o);
    }
    ScatteredObject* primary = ui::GetPrimarySelection();
    if (primary == target) primary = next.empty() ? nullptr : next.front();
    ui::SetSelection(next, primary);
}

void ObjectInteractionManager::DeleteSelection() {
    std::vector<ScatteredObject*> targets = ui::GetSelection();
    for (auto* target : targets) RemoveObjectFromScene(target);
    ui::SetSelection({}, nullptr);
}

bool ObjectInteractionManager::DeleteSelectedAnything() {
    if (WaterBody* water = ui::GetSelectedWater()) {
        PushUndoNow();
        DeleteWaterBody(water);
        return true;
    }
    if (BasicTerrain* terrain = ui::GetSelectedTerrain()) {
        ui::SetSelectedTerrain(nullptr);
        terrain->alive = false;
        return true;
    }
    // Full terrain::Terrain (chunked) deletion. Slot renumbering is
    // automatic: Unregister shifts all later terrains down, and since
    // nothing references the TERRAIN# slot, Explorer names stay valid.
    if (terrain::Terrain* legacy = terrain::GetTerrainEditorState().selectedTerrainLegacy) {
        terrain::HandleTerrainSelection(legacy, false);
        auto& registry = terrain::GetTerrainRegistry();
        registry.Unregister(legacy);
        legacy->alive = false;
        const project::Info& proj = project::GetCurrentProject();
        if (!proj.path.empty()) registry.WriteFile(proj.path + "/terrain.terrain");
        return true;
    }
    if (!selectedObjects.empty()) {
        PushUndoNow();
        DeleteSelection();
        return true;
    }
    return false;
}

void ObjectInteractionManager::DeleteWaterBody(WaterBody* target) {
    if (!target) return;
    engine.RemoveEntity(target);
    ui::SetSelectedWater(nullptr);
}

void ObjectInteractionManager::BeginSelectionDrag() {
    dragStartStates.clear();
    for (auto* obj : selectedObjects) {
        DragStartState s;
        if (obj) {
            s.pos = *obj->GetPosPtr();
            s.size = *obj->GetSizePtr();
            s.rotation = *obj->GetRotationPtr();
            s.prevEulerRad = { DEG2RAD * s.rotation.x, DEG2RAD * s.rotation.y, DEG2RAD * s.rotation.z };
        }
        dragStartStates.push_back(s);
    }
    dragPivot = SelectionCenter();
    ScatteredObject* primary = ui::GetPrimarySelection();
    if (primary) {
        startPos = *primary->GetPosPtr();
        startSize = *primary->GetSizePtr();
        startRotation = *primary->GetRotationPtr();
    }
}

ScatteredObject* ObjectInteractionManager::PickObject(Ray ray) const {
    float closestDistance = 1e9f;
    ScatteredObject* hitObject = nullptr;
    for (auto* obj : objects) {
        if (!obj) continue;
        BoundingBox box = obj->GetBoundingBox();
        // Check for valid bounding box
        if (box.min.x > box.max.x || box.min.y > box.max.y || box.min.z > box.max.z) continue;
        if (!std::isfinite(box.min.x) || !std::isfinite(box.min.y) || !std::isfinite(box.min.z)) continue;
        if (!std::isfinite(box.max.x) || !std::isfinite(box.max.y) || !std::isfinite(box.max.z)) continue;
        
        RayCollision collision = GetRayCollisionBox(ray, box);
        if (collision.hit && collision.distance < closestDistance) {
            closestDistance = collision.distance;
            hitObject = obj;
        }
    }
    return hitObject;
}

ModelGroup* ObjectInteractionManager::PickModel(Ray ray) const {
    float closestDistance = 1e9f;
    ModelGroup* hitModel = nullptr;
    for (const auto& model : models) {
        if (!model || model->members.empty()) continue;
        BoundingBox box = model->GetBounds();
        if (box.min.x > box.max.x || box.min.y > box.max.y || box.min.z > box.max.z) continue;
        if (!std::isfinite(box.min.x) || !std::isfinite(box.min.y) || !std::isfinite(box.min.z)) continue;
        if (!std::isfinite(box.max.x) || !std::isfinite(box.max.y) || !std::isfinite(box.max.z)) continue;
        
        RayCollision collision = GetRayCollisionBox(ray, box);
        if (collision.hit && collision.distance < closestDistance) {
            closestDistance = collision.distance;
            hitModel = model.get();
        }
    }
    return hitModel;
}

WaterBody* ObjectInteractionManager::PickWaterBody(Ray ray) const {
    float closestDistance = 1e9f;
    WaterBody* hit = nullptr;
    for (WaterBody* w : WaterBody::GetInstances()) {
        if (!w) continue;
        BoundingBox box = w->GetBoundingBox();
        if (box.min.x > box.max.x || box.min.y > box.max.y || box.min.z > box.max.z) continue;
        if (!std::isfinite(box.min.x) || !std::isfinite(box.min.y) || !std::isfinite(box.min.z)) continue;
        if (!std::isfinite(box.max.x) || !std::isfinite(box.max.y) || !std::isfinite(box.max.z)) continue;
        RayCollision collision = GetRayCollisionBox(ray, box);
        if (collision.hit && collision.distance < closestDistance) {
            closestDistance = collision.distance;
            hit = w;
        }
    }
    return hit;
}

ObjectInteractionManager::ClickTarget ObjectInteractionManager::PickClickTargets(Ray ray, bool altHeld) const {
    ClickTarget target;
    ScatteredObject* hitObject = PickObject(ray);

    if (hitObject && (altHeld || !hitObject->parentModel)) {
        // Individual object: ALT forces part picking; standalone objects always pick individually.
        target.objects.push_back(hitObject);
        target.primary = hitObject;
        return target;
    }

    if (hitObject && hitObject->parentModel) {
        // Clicking a member of a model selects the whole model.
        target.model = hitObject->parentModel;
        target.objects = target.model->members;
        target.primary = target.objects.empty() ? nullptr : target.objects.front();
        return target;
    }

    if (!altHeld) {
        ModelGroup* hitModel = PickModel(ray);
        if (hitModel != nullptr) {
            target.model = hitModel;
            target.objects = hitModel->members;
            target.primary = target.objects.empty() ? nullptr : target.objects.front();
        }
    }
    return target;
}

bool ObjectInteractionManager::IsExactSelection(const std::vector<ScatteredObject*>& targets) const {
    if (targets.size() != selectedObjects.size()) return false;
    for (auto* t : targets) {
        if (!t || !IsInSelection(t)) return false;
    }
    return true;
}

int ObjectInteractionManager::PickHandle(Vector2 mouse) const {
    ScatteredObject* primary = ui::GetPrimarySelection();
    if (!primary) return -1;
    Vector3 p = *primary->GetPosPtr();
    Vector3 s = *primary->GetSizePtr();
    Vector3 rotation = *primary->GetRotationPtr();
    Vector3 pivot = selectedObjects.size() > 1 ? SelectionCenter() : primary->GetOriginWorld();
    ui::TransformTool tool = ui::GetActiveTool();
    if (tool == ui::TransformTool::Move) {
        // Grab anywhere along an axis line (within 8px) or at its tip (within 14px),
        // so the whole gizmo is grabbable, not just the tiny tip spheres.
        for (int i = 0; i < 3; ++i) {
            Vector2 tipScreen = GetWorldToScreen(Vector3Add(pivot, Vector3Scale(HandleAxisDirection(rotation, i), MoveHandleWorldLen(pivot, s, i, camera))), camera);
            if (Vector2Distance(tipScreen, mouse) < 14.0f) return i;
        }
        for (int i = 0; i < 3; ++i) {
            Vector2 tipScreen = GetWorldToScreen(Vector3Add(pivot, Vector3Scale(HandleAxisDirection(rotation, i), MoveHandleWorldLen(pivot, s, i, camera))), camera);
            if (DistanceToSegment(mouse, GetWorldToScreen(pivot, camera), tipScreen) < 10.0f) return i;
        }
    } else if (tool == ui::TransformTool::Scale) {
        // Axis handles first (skipping end-on ones), then the 8 corners.
        for (int i = 0; i < 3; ++i) {
            if (!AxisScaleGrabbable(p, s, rotation, i, camera)) continue;
            Vector3 handleWorld = Vector3Add(p, Vector3Scale(HandleAxisDirection(rotation, i), AxisSize(s, i) * 0.5f));
            if (Vector2Distance(GetWorldToScreen(handleWorld, camera), mouse) < SCALE_PICK_RADIUS) return i;
        }
        for (int c = 0; c < 8; ++c) {
            if (Vector2Distance(GetWorldToScreen(HandleScaleCornerWorld(p, s, rotation, c), camera), mouse) < SCALE_PICK_RADIUS) return 3 + c;
        }
    } else if (tool == ui::TransformTool::Rotate) {
        float radius = Clamp(HandleWorldSize(pivot, ROTATION_RING_PIXELS, camera),
                             ROTATION_RING_MIN_WORLD, ROTATION_RING_MAX_WORLD);
        float closest = 14.0f;
        int pickedAxis = -1;
        bool localSpace = ui::IsTransformLocalSpace();
        for (int i = 0; i < 3; ++i) {
            float distance;
            if (localSpace) {
                distance = DistanceToScreenRing(mouse, pivot, radius, rotation, i, camera);
            } else {
                distance = DistanceToScreenRing(mouse, pivot, radius, i, camera);
            }
            if (distance < closest) {
                closest = distance;
                pickedAxis = i;
            }
        }
        return pickedAxis;
    }
    return -1;
}

// Handles results from the non-blocking file pickers.
//
// This must run unconditionally once per frame, before any of Update()'s
// early returns: the pickers are started from menu and button handlers, and
// their results can arrive many frames later. PollDialogResult() hands the
// result to whichever caller owns the pending dialog's purpose tag and to
// nobody else, so it is safe for ui.cpp to be polling at the same time.
void ObjectInteractionManager::PumpDialogs() {
    // Which purpose is in flight decides whether the terrain entity rides
    // along. Ctrl+S and menu "Save" (DialogPurpose::SaveScene) must keep it;
    // "Save As" (SaveSceneAs) has always dropped it.
    const platform::DialogPurpose inFlight = platform::PendingDialogPurpose();

    std::string picked;

    if (platform::PollDialogResult({platform::DialogPurpose::SaveScene,
                                    platform::DialogPurpose::SaveSceneAs}, picked)) {
        if (picked.empty()) return;   // cancelled
        terrain::Terrain* terrainPtr = nullptr;
        if (inFlight == platform::DialogPurpose::SaveScene) {
            for (auto& entity : engine.GetEntities()) {
                if (auto* t = dynamic_cast<terrain::Terrain*>(entity.get())) {
                    terrainPtr = t;
                    break;
                }
            }
        }
        ::SaveSceneToFile(objects, models, picked, terrainPtr);
        return;
    }

    if (platform::PollDialogResult({platform::DialogPurpose::OpenScene}, picked)) {
        if (picked.empty()) return;   // cancelled

        // Load first so a bad/cancelled file never destroys the current build.
        const size_t previousObjectCount = objects.size();
        const std::vector<city::City*> oldCities = city::GetCityRegistry().GetCities();
        if (!::LoadSceneFromFile(engine, objects, models, picked, physicsSim)) return;

        undo.Clear();
        ui::SetSelection({}, nullptr);
        contextMenuTarget = nullptr;
        CleanupPendingImports();
        for (size_t i = 0; i < previousObjectCount; ++i) engine.RemoveEntity(objects[i]);
        objects.erase(objects.begin(), objects.begin() + previousObjectCount);

        // The previous scene's cities are gone; the scene file registered the
        // fresh ones (loaded from the city.city sidecar).
        for (auto* c : oldCities) {
            if (!c) continue;
            city::GetCityRegistry().Unregister(c);
            c->alive = false;
        }
        city::SetActiveCity(nullptr);
        return;
    }

    if (platform::PollDialogResult({platform::DialogPurpose::ImportMesh}, picked)) {
        FinishMeshImport(picked);
    }
}

void ObjectInteractionManager::Update(float dt) {
    // Before anything that can return early: a dialog result that arrives
    // while the user is mid-click must not be starved for a frame.
    PumpDialogs();

    // Play/Stop edges. Must run before the early return below, and after
    // Simulation::Update (which toggles play) in the same frame.
    const bool playing = ui::IsPlayActive();
    if (playing != wasPlaying) {
        wasPlaying = playing;
        if (playing) CapturePlaySnapshot();
        else RestorePlaySnapshot();
    }

    if (playing) return; // editing disabled while physics runs

    // Update terrain editor
    ui::UpdateTerrainEditor(engine, cameraController, physicsSim);

    // City node editor: must run (and possibly consume the viewport click)
    // before any object picking below.
    const bool cityConsumedClick = city::UpdateCityEditor(engine, camera);

    if (ui::ConsumeImportRequest()) {
        ImportMesh();
        return;
    }

    SyncSelection();

    // Ctrl+S quick-save (Ctrl is swallowed by text fields/console/script editor).
    if (!ui::IsEditingText() &&
        (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && IsKeyPressed(KEY_S)) {
        const project::Info& current = project::GetCurrentProject();
        if (!current.path.empty()) {
            // Find terrain in scene
            terrain::Terrain* terrain = nullptr;
            for (auto& entity : engine.GetEntities()) {
                if (auto* t = dynamic_cast<terrain::Terrain*>(entity.get())) {
                    terrain = t;
                    break;
                }
            }
            project::SaveProjectFile(current.path, objects, models, terrain);
        } else {
            // Non-blocking picker; PumpDialogs() handles the result and
            // keeps the terrain entity, as this path always did.
            ::BeginChooseSceneSavePath(current.path, /*keepTerrain=*/true);
        }
        return;
    }

    // Explorer-initiated deletes (model "Delete" menu) are processed here.
    if (!ui::IsEditingText()) {
        std::vector<ScatteredObject*> pendingDelete = ui::ConsumePendingDelete();
        if (!pendingDelete.empty()) {
            for (auto* target : pendingDelete) {
                RemoveObjectFromScene(target);
            }
            // Refresh the live selection in case it referenced deleted objects.
            std::vector<ScatteredObject*> next;
            for (auto* o : ui::GetSelection()) {
                if (o && std::find(pendingDelete.begin(), pendingDelete.end(), o) == pendingDelete.end()) next.push_back(o);
            }
            ScatteredObject* primary = ui::GetPrimarySelection();
            if (primary && std::find(next.begin(), next.end(), primary) == next.end()) {
                primary = next.empty() ? nullptr : next.front();
            }
            ui::SetSelection(next, primary);
            return;
        }
    }

    // Ctrl+Z undo / Ctrl+Y redo (also Ctrl+Shift+Z as redo).
    if (!ui::IsEditingText() && !ui::IsContextMenuOpen() &&
        (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))) {
        const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        const bool zPressed = IsKeyPressed(KEY_Z);
        const bool yPressed = IsKeyPressed(KEY_Y);
        if (zPressed && shift) {
            DoRedo();
            return;
        }
        if (zPressed) {
            DoUndo();
            return;
        }
        if (yPressed) {
            DoRedo();
            return;
        }
    }

    // Ctrl+A selects every object in the scene.
    if (!ui::IsEditingText() && !ui::IsContextMenuOpen() &&
        (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && IsKeyPressed(KEY_A)) {
        std::vector<ScatteredObject*> all;
        all.reserve(objects.size());
        ScatteredObject* primary = nullptr;
        for (auto* obj : objects) {
            if (!obj) continue;
            all.push_back(obj);
            primary = obj;
        }
        ui::SetSelection(all, primary);
        return;
    }

    // Ctrl+L toggle transform space (Local/World)
    if (!ui::IsEditingText() && !ui::IsContextMenuOpen()) {
        if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && IsKeyPressed(KEY_L)) {
            ui::ToggleTransformSpace();
            ui::LogAlways("Transform space: %s", ui::IsTransformLocalSpace() ? "Local" : "World");
            return;
        }
    }

    // Delete/Backspace: one path for every deletable thing (objects, water, terrain).
    if (!ui::IsEditingText() && !ui::IsContextMenuOpen()) {
        if (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE)) {
            if (DeleteSelectedAnything()) return;
        }
    }

    // Ctrl+D duplicate / Delete-Backspace delete. Ctrl is swallowed by text
    // fields/console/script editor, so those are skipped via IsEditingText().
    const bool ctrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (!ui::IsEditingText() && !ui::IsContextMenuOpen() && !selectedObjects.empty()) {
        if (ctrlDown && IsKeyPressed(KEY_D)) {
            PushUndoNow();
            std::vector<ScatteredObject*> copies;
            std::vector<ModelGroup*> parentModels;
            ScatteredObject* newPrimary = nullptr;
            for (auto* obj : selectedObjects) {
                if (!obj) continue;
                auto copy = obj->Clone();
                ScatteredObject* np = copy.get();
                copies.push_back(np);
                parentModels.push_back(obj->parentModel);
                if (!newPrimary) newPrimary = np;
                objects.push_back(np);
                engine.AddEntity(std::move(copy));
            }
            // Duplicates inherit their source's grouping so a copy stays in the
            // same model, next to the original.
            for (size_t i = 0; i < copies.size(); ++i) {
                if (parentModels[i]) {
                    parentModels[i]->members.push_back(copies[i]);
                    copies[i]->parentModel = parentModels[i];
                }
            }
            ui::SetSelection(copies, newPrimary);
            return;
        }
        if (IsKeyPressed(KEY_F) && cameraController) {
            cameraController->FocusOn(SelectionCenter());
            return;
        }
    }

    // 1. Process Context Menu Actions FIRST
    ui::MenuAction action = ui::ProcessContextMenu();

    if (action == ui::MenuAction::Save) {
        const project::Info& current = project::GetCurrentProject();
        if (!current.path.empty()) {
            if (project::SaveProjectFile(current.path, objects, models)) {
                ClearPendingImports();
                ui::LogAlways("Saved project '%s'", current.name.c_str());
            } else {
                ui::LogAlways("Failed to save project: %s", current.path.c_str());
            }
        } else {
            ::BeginChooseSceneSavePath(project::GetCurrentProject().path,
                                       /*keepTerrain=*/true);
        }
        return;
    }

    if (action == ui::MenuAction::SaveAs) {
        // "Save As" has always written the scene without the terrain entity.
        ::BeginChooseSceneSavePath(project::GetCurrentProject().path,
                                   /*keepTerrain=*/false);
        return;
    }

    if (action == ui::MenuAction::OpenScene) {
        ::BeginChooseSceneOpenPath(project::GetCurrentProject().path);
        return;
    }

    // The mesh import picker is non-blocking too.
    {
        std::string picked;
        if (platform::PollDialogResult({platform::DialogPurpose::ImportMesh}, picked)) {
            FinishMeshImport(picked);
        }
    }

    if (action == ui::MenuAction::DeleteObject) {
        PushUndoNow();
        DeleteObject(contextMenuTarget);
        contextMenuTarget = nullptr;
        return;
    }
    if (action == ui::MenuAction::DeleteWaterBody) {
        DeleteSelectedAnything();
        return;
    }

    if (action == ui::MenuAction::ToggleTransformSpace) {
        ui::ToggleTransformSpace();
        ui::LogAlways("Transform space: %s", ui::IsTransformLocalSpace() ? "Local" : "World");
        return;
    }

    if (action == ui::MenuAction::SpawnCube ||
        action == ui::MenuAction::SpawnSphere ||
        action == ui::MenuAction::SpawnCylinder ||
        action == ui::MenuAction::SpawnWedge) {

        PushUndoNow();
        Ray ray = GetMouseRay(rmbPressPos, camera);
        Vector3 spawnPos;

        if (fabsf(ray.direction.y) > 0.001f) {
            float t = -ray.position.y / ray.direction.y;
            if (t > 0.1f && t < 150.0f) {
                spawnPos = {
                    ray.position.x + ray.direction.x * t,
                    0.75f,
                    ray.position.z + ray.direction.z * t
                };
            } else {
                spawnPos = Vector3Add(ray.position, Vector3Scale(ray.direction, 8.0f));
            }
        } else {
            spawnPos = Vector3Add(ray.position, Vector3Scale(ray.direction, 8.0f));
        }

        ShapeType type = ShapeType::Cube;
        if (action == ui::MenuAction::SpawnSphere)   type = ShapeType::Sphere;
        if (action == ui::MenuAction::SpawnCylinder) type = ShapeType::Cylinder;
        if (action == ui::MenuAction::SpawnWedge)    type = ShapeType::Wedge;

        auto newObj = std::make_unique<ScatteredObject>(spawnPos, Vector3{ 1.5f, 1.5f, 1.5f }, SKYBLUE, type);
        objects.push_back(newObj.get());
        engine.AddEntity(std::move(newObj));
        BasicTerrain::SetActive(nullptr);   // inserting props closes terrain tools
        return;
    }

    if (action == ui::MenuAction::SpawnWater) {
        Ray ray = GetMouseRay(rmbPressPos, camera);
        Vector3 spawnPos;

        if (fabsf(ray.direction.y) > 0.001f) {
            float t = -ray.position.y / ray.direction.y;
            if (t > 0.1f && t < 150.0f) {
                spawnPos = {
                    ray.position.x + ray.direction.x * t,
                    0.0f,
                    ray.position.z + ray.direction.z * t
                };
            } else {
                spawnPos = Vector3Add(ray.position, Vector3Scale(ray.direction, 8.0f));
            }
        } else {
            spawnPos = Vector3Add(ray.position, Vector3Scale(ray.direction, 8.0f));
        }

        ui::LogAlways("[water] Spawning water at (%.1f, %.1f, %.1f)", spawnPos.x, spawnPos.y, spawnPos.z);
        auto water = std::make_unique<WaterBody>(spawnPos, Vector3{ 100.0f, 1.0f, 100.0f }, 0.0f, Color{ 0, 100, 200, 180 });
        engine.AddEntity(std::move(water));
        ui::LogAlways("[water] Water body added to engine, total entities: %zu", engine.GetEntities().size());
        BasicTerrain::SetActive(nullptr);
        return;
    }

    if (action == ui::MenuAction::SpawnTerrain) {
        Ray ray = GetMouseRay(rmbPressPos, camera);
        Vector3 spawnPos;

        if (fabsf(ray.direction.y) > 0.001f) {
            float t = -ray.position.y / ray.direction.y;
            if (t > 0.1f && t < 150.0f) {
                spawnPos = {
                    ray.position.x + ray.direction.x * t,
                    0.0f,
                    ray.position.z + ray.direction.z * t
                };
            } else {
                spawnPos = Vector3Add(ray.position, Vector3Scale(ray.direction, 8.0f));
            }
        } else {
            spawnPos = Vector3Add(ray.position, Vector3Scale(ray.direction, 8.0f));
        }

        // Create default terrain
        auto terrain = std::make_unique<terrain::Terrain>(spawnPos, 1000.0f, 1000.0f, 256, 65);
        terrain->CreateBlank(0.0f);
        terrain->SetPhysicsMode(terrain::PhysicsMode::Heightfield);
        if (physicsSim) terrain->SetPhysicsSimulation(physicsSim);

        // Give it a unique Explorer name (duplicates are blocked), then take a
        // slot in the registry so terrain.terrain can be written immediately.
        auto& registry = terrain::GetTerrainRegistry();
        terrain->SetName(registry.MakeUniqueName("Landscape"));
        registry.Register(terrain.get());

        // Enable terrain editor mode
        ui::HandleTerrainSelection(terrain.get(), true);
        ui::SetTerrainEditorMode(ui::TransformTool::Terrain);

        engine.AddEntity(std::move(terrain));

        // Auto-create terrain.terrain next to the open project (if any).
        const project::Info& proj = project::GetCurrentProject();
        if (!proj.path.empty()) {
            std::string terrainFile = proj.path + "/terrain.terrain";
            if (registry.WriteFile(terrainFile)) {
                ui::LogAlways("[terrain] wrote %s", terrainFile.c_str());
            }
        }
        return;
    }

    if (action == ui::MenuAction::SpawnBasicTerrain) {
        Ray ray = GetMouseRay(rmbPressPos, camera);
        Vector3 spawnPos;

        if (fabsf(ray.direction.y) > 0.001f) {
            float t = -ray.position.y / ray.direction.y;
            if (t > 0.1f && t < 150.0f) {
                spawnPos = {
                    ray.position.x + ray.direction.x * t,
                    0.0f,
                    ray.position.z + ray.direction.z * t
                };
            } else {
                spawnPos = Vector3Add(ray.position, Vector3Scale(ray.direction, 8.0f));
            }
        } else {
            spawnPos = Vector3Add(ray.position, Vector3Scale(ray.direction, 8.0f));
        }

        // Create basic terrain (simple heightmap)
        auto basicTerrain = std::make_unique<BasicTerrain>(256, 256, 8.0f, 100.0f, 32.0f);
        basicTerrain->position = spawnPos;
        basicTerrain->GenerateFlat(0.0f);

        engine.AddEntity(std::move(basicTerrain));
        return;
    }

    if (action == ui::MenuAction::ImportTerrain) {
        // Request import dialog
        ui::RequestHeightmapImport();
        // The actual import will be handled in UpdateTerrainEditor
        return;
    }

    if (action == ui::MenuAction::SpawnCity || action == ui::MenuAction::SpawnCityOrganic) {
        // Place the city roughly 40 m in front of the camera, on the ground.
        Vector3 camDir = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
        float horiz = sqrtf(camDir.x * camDir.x + camDir.z * camDir.z);
        float dist = (horiz > 0.001f) ? 40.0f / horiz : 25.0f;
        const Vector2 origin = { camera.position.x + camDir.x * dist,
                                 camera.position.z + camDir.z * dist };

        auto city = std::make_unique<city::City>();
        city->SetName(city::GetCityRegistry().MakeUniqueName("City"));
        city->GetParams().organic = (action == ui::MenuAction::SpawnCityOrganic);
        city->GetParams().seed = ((int)(GetTime() * 997.0f)) % 90000 + 1;
        city->GenerateGrid(origin);

        city::City* raw = city.get();
        city::GetCityRegistry().Register(raw);
        engine.AddEntity(std::move(city));

        city::SetActiveCity(raw);
        ui::LogAlways("[city] Spawned %s at (%.1f, %.1f)",
                      raw->GetName().c_str(), origin.x, origin.y);
        BasicTerrain::SetActive(nullptr);
        return;
    }

    // T: quick-spawn a Basic Terrain ~40 units in front of the camera.
    if (!ui::IsEditingText() && !ui::IsContextMenuOpen() && IsKeyPressed(KEY_T)) {
        Ray ray = GetMouseRay(GetMousePosition(), camera);
        float horiz = sqrtf(ray.direction.x * ray.direction.x + ray.direction.z * ray.direction.z);
        float t = (horiz > 0.001f) ? 40.0f / horiz : 20.0f;
        Vector3 pos = { ray.position.x + ray.direction.x * t, 0.0f,
                        ray.position.z + ray.direction.z * t };
        auto bt = std::make_unique<BasicTerrain>(128, 128, 8.0f, 100.0f);
        bt->position = pos;
        engine.AddEntity(std::move(bt));
        ui::LogAlways("[terrain] spawned Basic Terrain at (%.0f, %.0f)", pos.x, pos.z);
        return;
    }

    // 2. RMB Mouse Tracking
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        bool overUI = ui::IsMouseOverUI();
        rmbPressInViewport = !overUI;
        if (rmbPressInViewport) {
            rmbPressPos = GetMousePosition();
            rmbDragDistance = 0.0f;
        }
    }

    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        Vector2 delta = GetMouseDelta();
        rmbDragDistance += fabsf(delta.x) + fabsf(delta.y);
    }

    // 3. Open Context Menu on RMB Release
    if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) {
        if (rmbPressInViewport && rmbDragDistance < 5.0f) {
            Ray ray = GetMouseRay(rmbPressPos, camera);
            ScatteredObject* hitObject = PickObject(ray);
            if (hitObject != nullptr) {
                contextMenuTarget = hitObject;
                ui::OpenContextMenu(rmbPressPos, true, false);
            } else {
                // Check for water body
                WaterBody* hitWater = PickWaterBody(ray);
                if (hitWater != nullptr) {
                    contextMenuTarget = nullptr;
                    ui::OpenContextMenu(rmbPressPos, false, true);
                } else {
                    contextMenuTarget = nullptr;
                    ui::OpenContextMenu(rmbPressPos, false, false);
                }
            }
        }
    }

    // 4. Block 3D Scene Interactions when hovering UI
    bool overUI = ui::IsMouseOverUI();
    if (overUI) {
        return;
    }

    const bool shiftDown = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    const bool ctrlHeld = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    const bool altHeld = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
    const bool additive = shiftDown || ctrlHeld;
    ScatteredObject* primary = ui::GetPrimarySelection();

    // Water body gizmo interaction (Move + Scale, no Rotate)
    WaterBody* waterSel = ui::GetSelectedWater();
    if (waterSel && ui::GetActiveTool() != ui::TransformTool::Select &&
        ui::GetActiveTool() != ui::TransformTool::Terrain &&
        ui::GetActiveTool() != ui::TransformTool::Rotate) {

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            dragStart = GetMousePosition();
            activeHandle = PickWaterHandle(waterSel, camera, GetMousePosition());
            if (activeHandle >= 0) {
                Vector3 wp = *waterSel->GetPosPtr();
                float wh = waterSel->GetWaterHeight();
                startPos = { wp.x, wh, wp.z };
                startSize = *waterSel->GetSizePtr();
                startRotation = { 0, 0, 0 };
                dragPivot = { wp.x, wh, wp.z };
                if (ui::GetActiveTool() == ui::TransformTool::Move) {
                    dragAxisWorldLen = WaterMoveHandleLen(startSize, activeHandle);
                }
                dragStartStates.clear();
                freeDrag = false;
                return;
            }
        }

        if (activeHandle >= 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector3 ws = *waterSel->GetSizePtr();

            if (ui::GetActiveTool() == ui::TransformTool::Move) {
                Vector2 mouseDelta = Vector2Subtract(GetMousePosition(), dragStart);
                float worldPerPixel = dragAxisWorldLen / fmaxf(40.0f, 1.0f);

                if (activeHandle == 0) {
                    float dx = Vector2DotProduct(mouseDelta, { 1.0f, 0.0f });
                    *waterSel->GetPosPtr() = { startPos.x + dx * worldPerPixel, startPos.y, startPos.z };
                } else if (activeHandle == 1) {
                    float dy = Vector2DotProduct(mouseDelta, { 0.0f, -1.0f });
                    waterSel->SetWaterHeight(startPos.y + dy * worldPerPixel);
                } else if (activeHandle == 2) {
                    float dz = Vector2DotProduct(mouseDelta, { 1.0f, 0.0f });
                    *waterSel->GetPosPtr() = { startPos.x, startPos.y, startPos.z + dz * worldPerPixel };
                }
            } else if (ui::GetActiveTool() == ui::TransformTool::Scale) {
                Vector2 mouseDelta = Vector2Subtract(GetMousePosition(), dragStart);
                Vector2 d = mouseDelta;
                float amount = (fabsf(d.x) > fabsf(d.y) ? d.x : -d.y) * 0.025f;

                if (activeHandle >= 3 && activeHandle < 7) {
                    int ci = activeHandle - 3;
                    int cx = ci / 2;
                    int cz = ci % 2;
                    Vector3 cornerStart = {
                        dragPivot.x + (cx == 0 ? -1.0f : 1.0f) * startSize.x * 0.5f,
                        dragPivot.y,
                        dragPivot.z + (cz == 0 ? -1.0f : 1.0f) * startSize.z * 0.5f
                    };
                    Vector2 centerScreen = GetWorldToScreen(dragPivot, camera);
                    Vector2 cornerScreen = GetWorldToScreen(cornerStart, camera);
                    float r0 = Vector2Distance(cornerScreen, centerScreen);
                    if (r0 > 2.0f) {
                        Vector2 dir = Vector2Scale(Vector2Subtract(cornerScreen, centerScreen), 1.0f / r0);
                        float deltaAlong = Vector2DotProduct(mouseDelta, dir);
                        float uniformFactor = Clamp(1.0f + deltaAlong / r0, 0.02f, 100.0f);
                        ws.x = fmaxf(SCALE_MIN_SIZE, startSize.x * uniformFactor);
                        ws.z = fmaxf(SCALE_MIN_SIZE, startSize.z * uniformFactor);
                    }
                } else if (activeHandle >= 0 && activeHandle < 3) {
                    Vector3 axisDir = AxisDirection(activeHandle);
                    float startAxis = AxisSize(startSize, activeHandle);
                    Vector3 handleWorld = Vector3Add(dragPivot, Vector3Scale(axisDir, startAxis * 0.5f));
                    Vector2 handleScreen = GetWorldToScreen(handleWorld, camera);
                    Vector3 probeWorld = Vector3Add(dragPivot, Vector3Scale(axisDir, startAxis * 0.5f + 1.0f));
                    Vector2 screenAxis = Vector2Subtract(GetWorldToScreen(probeWorld, camera), handleScreen);
                    float screenAxisLen = Vector2Length(screenAxis);

                    if (screenAxisLen > 2.0f) {
                        Vector2 centerScreen = GetWorldToScreen(dragPivot, camera);
                        float r0 = Vector2Distance(handleScreen, centerScreen);
                        if (r0 > 2.0f) {
                            Vector2 screenDir = Vector2Scale(screenAxis, 1.0f / screenAxisLen);
                            float deltaAlong = Vector2DotProduct(mouseDelta, screenDir);
                            float factor = Clamp(1.0f + deltaAlong / r0, 0.02f, 100.0f);
                            float newAxis = fmaxf(SCALE_MIN_SIZE, startAxis * factor);
                            if (activeHandle == 0) ws.x = newAxis;
                            if (activeHandle == 2) ws.z = newAxis;
                        } else {
                            if (activeHandle == 0) ws.x = fmaxf(SCALE_MIN_SIZE, ws.x + amount);
                            if (activeHandle == 2) ws.z = fmaxf(SCALE_MIN_SIZE, ws.z + amount);
                        }
                    } else {
                        if (activeHandle == 0) ws.x = fmaxf(SCALE_MIN_SIZE, ws.x + amount);
                        if (activeHandle == 2) ws.z = fmaxf(SCALE_MIN_SIZE, ws.z + amount);
                    }
                }
                *waterSel->GetSizePtr() = ws;
            }
            return;
        }

        // Release active water gizmo handle on mouse release
        if (activeHandle >= 0 && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            activeHandle = -1;
            freeDrag = false;
            return;
        }
    } else if (!primary || ui::GetActiveTool() == ui::TransformTool::Select ||
               ui::GetActiveTool() == ui::TransformTool::Terrain) {
        // No water selected or tool doesn't apply - fall through to ScatteredObject logic
    }

    if (primary && ui::GetActiveTool() != ui::TransformTool::Select) {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            dragStart = GetMousePosition();
            activeHandle = PickHandle(GetMousePosition());
            if (activeHandle >= 0) {
                BeginDragUndoCapture();
                Vector3 pivot = selectedObjects.size() > 1 ? SelectionCenter() : primary->GetOriginWorld();
                Vector2 centerOnScreen = GetWorldToScreen(pivot, camera);
                dragStartAngle = atan2f(dragStart.y - centerOnScreen.y, dragStart.x - centerOnScreen.x);
                BeginSelectionDrag();
                startPos = *primary->GetPosPtr();
                startSize = *primary->GetSizePtr();
                startRotation = *primary->GetRotationPtr();
                dragPivot = selectedObjects.size() > 1 ? SelectionCenter() : primary->GetOriginWorld();
                dragStartStates.clear();
                dragStartStates.reserve(selectedObjects.size());
                for (auto* obj : selectedObjects) {
                    if (obj) {
                        Vector3 r = *obj->GetRotationPtr();
                        dragStartStates.push_back({ *obj->GetPosPtr(), *obj->GetSizePtr(), r,
                                                    { DEG2RAD * r.x, DEG2RAD * r.y, DEG2RAD * r.z } });
                    }
                }

                if (ui::GetActiveTool() == ui::TransformTool::Rotate) {
                    bool localSpace = ui::IsTransformLocalSpace();
                    Vector3 axisDir = localSpace ? RotatedAxisDirection(startRotation, activeHandle) : AxisDirection(activeHandle);
                    Vector3 viewForward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
                    // Rotation sense so a counter-clockwise screen drag rotates the ring
                    // counter-clockwise for every axis orientation (verified against the
                    // projected ring geometry). Uses the "into scene" view axis, not the
                    // radial to-camera vector, so non-front-facing rings aren't inverted.
                    rotateSign = (Vector3DotProduct(axisDir, viewForward) > 0.0f) ? -1.0f : 1.0f;
                    rotatePrevEuler = { DEG2RAD * startRotation.x, DEG2RAD * startRotation.y, DEG2RAD * startRotation.z };
                } else if (ui::GetActiveTool() == ui::TransformTool::Move) {
                    dragAxisWorldLen = MoveHandleWorldLen(pivot, startSize, activeHandle, camera);
                }
                freeDrag = false;
                return;
            }
        }
        if ((activeHandle >= 0 || freeDrag) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector2 d = Vector2Subtract(GetMousePosition(), dragStart);
            float amount = (fabsf(d.x) > fabsf(d.y) ? d.x : -d.y) * 0.025f;
            const bool multi = selectedObjects.size() > 1;
            if (ui::GetActiveTool() == ui::TransformTool::Move) {
                Vector3 next = startPos;
                if (freeDrag) {
                    Ray ray = GetMouseRay(GetMousePosition(), camera);
                    Vector3 hit = RayPlaneIntersection(ray, startPos, dragPlaneNormal);
                    next = Vector3Add(startPos, Vector3Subtract(hit, dragPlanePoint));
                } else {
                    Vector3 axisDir = HandleAxisDirection(*primary->GetRotationPtr(), activeHandle);
                    Vector3 pivot = SelectionCenter();

                    Vector3 handleWorld = Vector3Add(pivot, Vector3Scale(axisDir, dragAxisWorldLen));
                    Vector2 handleScreen = GetWorldToScreen(handleWorld, camera);
                    Vector3 probeWorld = Vector3Add(pivot, Vector3Scale(axisDir, dragAxisWorldLen + 1.0f));
                    Vector2 screenAxis = Vector2Subtract(GetWorldToScreen(probeWorld, camera), handleScreen);
                    float screenAxisLen = Vector2Length(screenAxis);

                    if (screenAxisLen > 2.0f) {
                        Vector2 screenDir = Vector2Scale(screenAxis, 1.0f / screenAxisLen);
                        float cursorAlong = Vector2DotProduct(Vector2Subtract(GetMousePosition(), handleScreen), screenDir);
                        float worldPerPixel = 1.0f / screenAxisLen;
                        next = Vector3Add(*primary->GetPosPtr(), Vector3Scale(axisDir, cursorAlong * worldPerPixel));
                    } else {
                        if (activeHandle == 0) next.x += amount;
                        if (activeHandle == 1) next.y += amount;
                        if (activeHandle == 2) next.z += amount;
                    }
                }

                if (multi) {
                    Vector3 delta = Vector3Subtract(next, startPos);
                    for (size_t i = 0; i < selectedObjects.size(); ++i) {
                        ScatteredObject* obj = selectedObjects[i];
                        if (!obj) continue;
                        *obj->GetPosPtr() = Vector3Add(dragStartStates[i].pos, delta);
                    }
                } else {
                    *primary->GetPosPtr() = next;
                }
            } else if (ui::GetActiveTool() == ui::TransformTool::Scale) {
                Vector3 currentSize = *primary->GetSizePtr();
                Vector3 nextSize = currentSize;

                Vector2 mouseDelta = Vector2Subtract(GetMousePosition(), dragStart);

                float uniformFactor = 1.0f;
                bool uniformScale = false;
                if (activeHandle >= 3 && activeHandle < 11) {
                    int corner = activeHandle - 3;
                    Vector3 cornerStart = HandleScaleCornerWorld(startPos, startSize, startRotation, corner);
                    Vector2 centerScreen = GetWorldToScreen(startPos, camera);
                    Vector2 cornerScreen = GetWorldToScreen(cornerStart, camera);
                    float r0 = Vector2Distance(cornerScreen, centerScreen);
                    if (r0 > 2.0f) {
                        Vector2 dir = Vector2Scale(Vector2Subtract(cornerScreen, centerScreen), 1.0f / r0);
                        float deltaAlong = Vector2DotProduct(mouseDelta, dir);
                        uniformFactor = Clamp(1.0f + deltaAlong / r0, 0.02f, 100.0f);
                        uniformScale = true;
                        nextSize = Vector3Scale(startSize, uniformFactor);
                    }
                } else if (activeHandle >= 0 && activeHandle < 3) {
                    Vector3 axisDir = HandleAxisDirection(startRotation, activeHandle);
                    float startAxis = AxisSize(startSize, activeHandle);
                    Vector3 handleWorld = Vector3Add(startPos, Vector3Scale(axisDir, startAxis * 0.5f));
                    Vector2 handleScreen = GetWorldToScreen(handleWorld, camera);
                    Vector3 probeWorld = Vector3Add(startPos, Vector3Scale(axisDir, startAxis * 0.5f + 1.0f));
                    Vector2 screenAxis = Vector2Subtract(GetWorldToScreen(probeWorld, camera), handleScreen);
                    float screenAxisLen = Vector2Length(screenAxis);

                    if (screenAxisLen > 2.0f) {
                        Vector2 centerScreen = GetWorldToScreen(startPos, camera);
                        float r0 = Vector2Distance(handleScreen, centerScreen);
                        if (r0 > 2.0f) {
                            Vector2 screenDir = Vector2Scale(screenAxis, 1.0f / screenAxisLen);
                            float deltaAlong = Vector2DotProduct(mouseDelta, screenDir);
                            float factor = Clamp(1.0f + deltaAlong / r0, 0.02f, 100.0f);
                            float newAxis = fmaxf(startAxis * factor, SCALE_MIN_SIZE);
                            if (activeHandle == 0) nextSize.x = newAxis;
                            if (activeHandle == 1) nextSize.y = newAxis;
                            if (activeHandle == 2) nextSize.z = newAxis;
                        } else {
                            if (activeHandle == 0) nextSize.x = fmaxf(SCALE_MIN_SIZE, currentSize.x + amount);
                            if (activeHandle == 1) nextSize.y = fmaxf(SCALE_MIN_SIZE, currentSize.y + amount);
                            if (activeHandle == 2) nextSize.z = fmaxf(SCALE_MIN_SIZE, currentSize.z + amount);
                        }
                    } else {
                        if (activeHandle == 0) nextSize.x = fmaxf(SCALE_MIN_SIZE, currentSize.x + amount);
                        if (activeHandle == 1) nextSize.y = fmaxf(SCALE_MIN_SIZE, currentSize.y + amount);
                        if (activeHandle == 2) nextSize.z = fmaxf(SCALE_MIN_SIZE, currentSize.z + amount);
                    }
                }

                if (multi) {
                    const float pivotX = dragPivot.x, pivotY = dragPivot.y, pivotZ = dragPivot.z;
                    for (size_t i = 0; i < selectedObjects.size(); ++i) {
                        ScatteredObject* obj = selectedObjects[i];
                        if (!obj) continue;
                        DragStartState& st = dragStartStates[i];

                        if (uniformScale) {
                            *obj->GetPosPtr() = {
                                pivotX + (st.pos.x - pivotX) * uniformFactor,
                                pivotY + (st.pos.y - pivotY) * uniformFactor,
                                pivotZ + (st.pos.z - pivotZ) * uniformFactor
                            };
                            *obj->GetSizePtr() = Vector3Scale(st.size, uniformFactor);
                        } else if (activeHandle >= 0 && activeHandle < 3) {
                            float factor = (activeHandle == 0) ? nextSize.x / fmaxf(st.size.x, 1e-5f)
                                    : (activeHandle == 1) ? nextSize.y / fmaxf(st.size.y, 1e-5f)
                                    : nextSize.z / fmaxf(st.size.z, 1e-5f);
                            if (factor <= 0.0f) factor = 1.0f;

                            Vector3 np = st.pos;
                            if (activeHandle == 0) np.x = pivotX + (st.pos.x - pivotX) * factor;
                            if (activeHandle == 1) np.y = pivotY + (st.pos.y - pivotY) * factor;
                            if (activeHandle == 2) np.z = pivotZ + (st.pos.z - pivotZ) * factor;
                            *obj->GetPosPtr() = np;

                            Vector3 ns = st.size;
                            if (activeHandle == 0) ns.x = fmaxf(SCALE_MIN_SIZE, st.size.x * factor);
                            if (activeHandle == 1) ns.y = fmaxf(SCALE_MIN_SIZE, st.size.y * factor);
                            if (activeHandle == 2) ns.z = fmaxf(SCALE_MIN_SIZE, st.size.z * factor);
                            *obj->GetSizePtr() = ns;
                        }
                        obj->ResetMassAuto();
                    }
                } else {
                    *primary->GetSizePtr() = nextSize;
                    primary->ResetMassAuto();
                }
            } else if (ui::GetActiveTool() == ui::TransformTool::Rotate) {
                Vector2 currentMouse = GetMousePosition();
                Vector2 pivotScreen = GetWorldToScreen(dragPivot, camera);
                float currentAngle = atan2f(currentMouse.y - pivotScreen.y, currentMouse.x - pivotScreen.x);
                float totalDelta = currentAngle - dragStartAngle;
                if (totalDelta > PI) totalDelta -= 2.0f * PI;
                if (totalDelta < -PI) totalDelta += 2.0f * PI;
                // Total rotation from drag start is a pure function of the cursor
                // position, so it can't accumulate per-frame error or spiral out.
                float totalDegrees = -totalDelta * RAD2DEG * rotateSign;

                bool localSpace = ui::IsTransformLocalSpace();
                Vector3 axisDir = localSpace ? RotatedAxisDirection(startRotation, activeHandle) : AxisDirection(activeHandle);

                if (multi) {
                    Matrix groupRot = MatrixRotate(axisDir, DEG2RAD * totalDegrees);
                    for (size_t i = 0; i < selectedObjects.size(); ++i) {
                        ScatteredObject* obj = selectedObjects[i];
                        if (!obj) continue;
                        DragStartState& st = dragStartStates[i];

                        // Build the body rotation as the exact matrix the renderer
                        // uses (MatrixRotateXYZ), composed with the drag rotation
                        // about the fixed world/local axis. Extracting the Euler back
                        // through the same matrix convention keeps rendering and
                        // position exact even for angled objects (quaternion
                        // round-trips used a different axis order and were wrong).
                        Matrix R = MatrixMultiply(groupRot, MatrixRotateXYZ({ DEG2RAD * st.rotation.x, DEG2RAD * st.rotation.y, DEG2RAD * st.rotation.z }));
                        Vector3 e = MatrixToEulerContinuous(R, st.prevEulerRad);
                        st.prevEulerRad = e;
                        Vector3 nextEuler = { WrapDeg(e.x * RAD2DEG), WrapDeg(e.y * RAD2DEG), WrapDeg(e.z * RAD2DEG) };
                        *obj->GetRotationPtr() = nextEuler;

                        // Swing the object's world origin around the selection pivot.
                        // Built from the drag-start state so it's consistent every frame.
                        Matrix startR = MatrixRotateXYZ({ DEG2RAD * st.rotation.x, DEG2RAD * st.rotation.y, DEG2RAD * st.rotation.z });
                        Vector3 startOriginWorld = Vector3Add(st.pos, Vector3Transform(*obj->GetOriginPtr(), startR));
                        Vector3 toPivot = Vector3Subtract(startOriginWorld, dragPivot);
                        Vector3 newOriginWorld = Vector3Add(dragPivot, Vector3Transform(toPivot, groupRot));
                        *obj->GetPosPtr() = Vector3Subtract(newOriginWorld, Vector3Transform(*obj->GetOriginPtr(), R));
                    }
                } else {
                    Matrix R = MatrixMultiply(MatrixRotate(axisDir, DEG2RAD * totalDegrees), MatrixRotateXYZ({ DEG2RAD * startRotation.x, DEG2RAD * startRotation.y, DEG2RAD * startRotation.z }));
                    Vector3 e = MatrixToEulerContinuous(R, rotatePrevEuler);
                    rotatePrevEuler = e;
                    Vector3 nextEuler = { WrapDeg(e.x * RAD2DEG), WrapDeg(e.y * RAD2DEG), WrapDeg(e.z * RAD2DEG) };
                    *primary->GetRotationPtr() = nextEuler;

                    // Pivot == the object's world origin: keep it fixed by solving for
                    // the position from the exact same rotation used to render, so the
                    // body spins in place without drifting or jumping.
                    Vector3 origin = *primary->GetOriginPtr();
                    *primary->GetPosPtr() = Vector3Subtract(dragPivot, Vector3Transform(origin, R));
                }
            }
            return;
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            EndDragUndoCapture();
            activeHandle = -1;
            freeDrag = false;
        }
    }

    // 4b. Move tool: click an object to select it and drag it freely on the
    // camera plane so it follows the cursor exactly. Shift/Ctrl add to the
    // selection, ALT forces individual-object picking (skips model groups).
    if (ui::GetActiveTool() == ui::TransformTool::Move &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !ui::IsContextMenuOpen() && !cityConsumedClick) {

        Ray ray = GetMouseRay(GetMousePosition(), camera);
        ClickTarget target = PickClickTargets(ray, altHeld);

        if (!target.objects.empty() && target.primary) {
            if (additive) {
                if (target.model) {
                    ToggleModel(target.model);
                } else {
                    ToggleSelect(target.primary);
                }
            } else {
                if (!IsExactSelection(target.objects)) {
                    ui::SetSelection(target.objects, target.primary);
                }
                // Drag the whole (current) selection on the camera plane.
                SyncSelection();
                BeginDragUndoCapture();
                dragStart = GetMousePosition();
                BeginSelectionDrag();
                startPos = *target.primary->GetPosPtr();
                dragPlaneNormal = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
                dragPlanePoint = RayPlaneIntersection(ray, startPos, dragPlaneNormal);
                freeDrag = true;
            }
            return;
        }
    }

    // 5. Left Click Selection (Select tool)
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !ui::IsContextMenuOpen() && !ui::WasUIClickConsumed() && !cityConsumedClick) {
        Ray ray = GetMouseRay(GetMousePosition(), camera);
        ClickTarget target = PickClickTargets(ray, altHeld);

        if (!target.objects.empty() && target.primary) {
            if (additive) {
                if (target.model) {
                    ToggleModel(target.model);
                } else {
                    ToggleSelect(target.primary);
                }
            } else {
                ui::SetSelection(target.objects, target.primary);
            }
            return;
        }

        // Check water body picking (after objects, so objects take priority)
        WaterBody* hitWater = PickWaterBody(ray);
        if (hitWater) {
            ui::SetSelectedWater(hitWater);
            return;
        }

        // Check terrain picking. BasicTerrain handles its own click-to-select
        // and stamp/paint logic in BasicTerrain::Update() (run via the engine's
        // entity update). If we fall through to "empty space" here, we'd
        // deselect the terrain and MarkUIClickConsumed() on every single click
        // that lands on it (since terrain isn't a ScatteredObject or a
        // WaterBody), which starves BasicTerrain::Update()'s own
        // !ui::WasUIClickConsumed() check and makes every tool a no-op.
        for (auto* t : BasicTerrain::GetInstances()) {
            if (t && t->Raycast(ray, nullptr, nullptr, nullptr)) {
                return;
            }
        }

        // Clicked empty space (no object, no water, no terrain): deselect everything
        ui::SetSelectedWater(nullptr);
        ui::SetSelectedTerrain(nullptr);
        ui::SetSelection({}, nullptr);
        ui::MarkUIClickConsumed();
    }
}

void ObjectInteractionManager::DrawOverlay3D() {
    // Water body gizmo (no ScatteredObject primary needed)
    WaterBody* waterSel = ui::GetSelectedWater();
    if (waterSel && ui::GetActiveTool() != ui::TransformTool::Select &&
        ui::GetActiveTool() != ui::TransformTool::Terrain &&
        ui::GetActiveTool() != ui::TransformTool::Rotate) {
        Vector3 wp = *waterSel->GetPosPtr();
        Vector3 ws = *waterSel->GetSizePtr();
        float wh = waterSel->GetWaterHeight();
        Vector3 pivot = { wp.x, wh, wp.z };

        ui::TransformTool tool = ui::GetActiveTool();
        if (tool == ui::TransformTool::Move) {
            for (int i = 0; i < 3; ++i) {
                Vector3 axisDir = AxisDirection(i);
                float len = WaterMoveHandleLen(ws, i);
                Vector3 start = pivot;
                Vector3 end = Vector3Add(pivot, Vector3Scale(axisDir, len));
                Color color = (activeHandle == i) ? Color{ 255, 255, 80, 255 } : Color{ 200, 220, 255, 255 };
                DrawLine3D(start, end, color);
                DrawSphere(end, 0.08f, color);
            }
        } else if (tool == ui::TransformTool::Scale) {
            for (int i = 0; i < 3; ++i) {
                if (i == 1) continue; // skip Y axis for water scale
                if (!AxisScaleGrabbable(pivot, ws, {0,0,0}, i, camera)) continue;
                Vector3 axisDir = AxisDirection(i);
                float axisLen = AxisSize(ws, i);
                Vector3 start = Vector3Add(pivot, Vector3Scale(axisDir, -axisLen * 0.5f));
                Vector3 end = Vector3Add(pivot, Vector3Scale(axisDir, axisLen * 0.5f));
                Color color = (activeHandle == i) ? Color{ 255, 255, 80, 255 } : Color{ 200, 255, 200, 255 };
                DrawLine3D(start, end, color);
                DrawSphere(end, 0.06f, color);
            }
            // Draw XZ corner handles
            for (int cx = 0; cx < 2; ++cx) {
                for (int cz = 0; cz < 2; ++cz) {
                    Vector3 corner = {
                        pivot.x + (cx == 0 ? -1.0f : 1.0f) * ws.x * 0.5f,
                        pivot.y,
                        pivot.z + (cz == 0 ? -1.0f : 1.0f) * ws.z * 0.5f
                    };
                    Color color = (activeHandle == 3 + cx * 2 + cz) ? Color{ 255, 255, 80, 255 } : Color{ 200, 255, 200, 255 };
                    DrawSphere(corner, 0.05f, color);
                }
            }
        }
        return;
    }

    // Draw the move/scale/rotate gizmo for the primary selection.
    ScatteredObject* primary = ui::GetPrimarySelection();
    if (!primary) return;
    if (ui::GetActiveTool() == ui::TransformTool::Select) return;
    if (ui::GetActiveTool() == ui::TransformTool::Terrain) return;

    Vector3 p = *primary->GetPosPtr();
    Vector3 s = *primary->GetSizePtr();
    Vector3 rotation = *primary->GetRotationPtr();
    Vector3 pivot = selectedObjects.size() > 1 ? SelectionCenter() : primary->GetOriginWorld();

    ui::TransformTool tool = ui::GetActiveTool();
    if (tool == ui::TransformTool::Move) {
        for (int i = 0; i < 3; ++i) {
            Vector3 axisDir = HandleAxisDirection(rotation, i);
            float len = MoveHandleWorldLen(pivot, s, i, camera);
            Vector3 start = pivot;
            Vector3 end = Vector3Add(pivot, Vector3Scale(axisDir, len));
            Color color = (activeHandle == i) ? Color{ 255, 255, 80, 255 } : Color{ 200, 220, 255, 255 };
            DrawLine3D(start, end, color);
            DrawSphere(end, 0.08f, color);
        }
    } else if (tool == ui::TransformTool::Scale) {
        for (int i = 0; i < 3; ++i) {
            if (!AxisScaleGrabbable(p, s, rotation, i, camera)) continue;
            Vector3 axisDir = HandleAxisDirection(rotation, i);
            float axisLen = AxisSize(s, i);
            Vector3 start = Vector3Add(p, Vector3Scale(axisDir, -axisLen * 0.5f));
            Vector3 end = Vector3Add(p, Vector3Scale(axisDir, axisLen * 0.5f));
            Color color = (activeHandle == i) ? Color{ 255, 255, 80, 255 } : Color{ 200, 255, 200, 255 };
            DrawLine3D(start, end, color);
            DrawSphere(end, 0.06f, color);
        }
        // Draw corner handles
        for (int c = 0; c < 8; ++c) {
            Vector3 corner = HandleScaleCornerWorld(p, s, rotation, c);
            Color color = (activeHandle == 3 + c) ? Color{ 255, 255, 80, 255 } : Color{ 200, 255, 200, 255 };
            DrawSphere(corner, 0.05f, color);
        }
    } else if (tool == ui::TransformTool::Rotate) {
        float radius = Clamp(HandleWorldSize(pivot, ROTATION_RING_PIXELS, camera),
                             ROTATION_RING_MIN_WORLD, ROTATION_RING_MAX_WORLD);
        bool localSpace = ui::IsTransformLocalSpace();
        for (int i = 0; i < 3; ++i) {
            Color color = (activeHandle == i) ? Color{ 255, 255, 80, 255 } : Color{ 200, 220, 255, 255 };
            if (localSpace) {
                DrawRotationRing(pivot, radius, rotation, i, color, activeHandle == i);
            } else {
                // World space: slightly dimmer so local vs world is visually obvious
                Color worldColor = Color{ (unsigned char)(color.r / 2), (unsigned char)(color.g / 2),
                                          (unsigned char)(color.b / 2), 255 };
                DrawRotationRing(pivot, radius, i, worldColor, activeHandle == i);
            }
        }
    }
}

// The mesh picker is non-blocking (it may shell out to zenity/kdialog), so
// ImportMesh() only starts it; FinishMeshImport() runs on a later frame once
// PollDialogResult reports the chosen path. An empty path means cancelled.
void ObjectInteractionManager::ImportMesh() {
    const project::Info& current = project::GetCurrentProject();
    platform::BeginOpenFileDialog(platform::DialogPurpose::ImportMesh, "Import Mesh",
                                  current.path, MeshFilters());
}

void ObjectInteractionManager::FinishMeshImport(const std::string& modelPath) {
    if (modelPath.empty()) return;   // cancelled

    const project::Info& current = project::GetCurrentProject();
    const std::string projectDir = current.path.empty() ? "" : current.path;

    ImportResult result = ImportModel(modelPath, projectDir, current.name);
    if (!result.ok) {
        ui::LogAlways("Failed to import model: %s", result.error.c_str());
        return;
    }

    // Spawn object at camera center
    Vector3 spawnPos = camera.target;
    spawnPos.y = 0.5f;

    PushUndoNow();
    auto newObj = std::make_unique<ScatteredObject>(spawnPos, Vector3{ 1.5f, 1.5f, 1.5f }, WHITE, ShapeType::Cube);
    newObj->SetName(std::filesystem::path(result.storedPath).stem().string());
    // Resolve the project-relative stored path to an absolute file path so
    // LoadModel() can find it regardless of the process working directory.
    std::string absModel = ResolveStoredAssetPath(result.storedPath, projectDir);
    if (!newObj->SetModel(absModel)) {
        ui::LogAlways("Could not load imported mesh: %s", result.storedPath.c_str());
        return;
    }
    newObj->NormalizeModelToUnitBox();
    objects.push_back(newObj.get());
    engine.AddEntity(std::move(newObj));
    ui::SetSelection({ objects.back() }, objects.back());
    BasicTerrain::SetActive(nullptr);
    ClearPendingImports();
    pendingImportDirs.push_back(result.targetDir);
}

void ObjectInteractionManager::CleanupPendingImports() {
    // Called on project open/save/new - unregister any pending import directories
    for (const auto& dir : pendingImportDirs) {
        textureManager::RemoveModelDirectory(dir, objects);
    }
    pendingImportDirs.clear();
}

void ObjectInteractionManager::ClearPendingImports() {
    pendingImportDirs.clear();
}