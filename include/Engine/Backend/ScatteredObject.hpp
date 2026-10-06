#pragma once
#include "Entity.hpp"
#include "Frustum.hpp"
#include "../PhysicsCollision.hpp"
#include "raylib.h"
#include "raymath.h"
#include <memory>
#include <string>
#include <vector>

enum class ShapeType {
    Cube,
    Sphere,
    Cylinder,
    Wedge
};

// Editor grouping container. A Model groups a set of ScatteredObjects so they
// can be selected and transformed together. Members keep living in the flat
// scene object list (physics/scripts iterate that); the model only adds an
// editor-level grouping + persistence layer.
class ScatteredObject;

class ModelGroup {
public:
    std::string name = "Model";
    std::vector<ScatteredObject*> members;

    // Axis-aligned union of all member bounds. Zero box if empty.
    BoundingBox GetBounds() const;
};

class ScatteredObject : public Entity {
public:
    ScatteredObject(Vector3 pos, Vector3 size, Color color, ShapeType shape = ShapeType::Cube);
    ~ScatteredObject() override;

    // Owns a GPU texture handle when a custom one is loaded, so copying
    // would double-free it - keep this non-copyable.
    ScatteredObject(const ScatteredObject&) = delete;
    ScatteredObject& operator=(const ScatteredObject&) = delete;

    void Update(float dt) override;
    void Draw() override;

    BoundingBox GetBoundingBox() const;
    Vector3* GetPosPtr();
    Vector3* GetSizePtr();
    Color* GetColorPtr();
    float* GetRotationYPtr();
    Vector3* GetRotationPtr();
    ShapeType GetShapeType() const;

    // Editor pivot: local offset from the object's center the move/rotate
    // gizmos attach to. World position of the pivot is GetOriginWorld().
    Vector3* GetOriginPtr();
    Vector3 GetOriginWorld() const;

    // Pre-set physics velocities. Applied to the body when play starts;
    // editable from the properties panel before pressing Play.
    Vector3 GetVelocity() const;
    void SetVelocity(Vector3 v);
    Vector3 GetAngularVelocity() const;
    void SetAngularVelocity(Vector3 v);

    // Physics mass. 0 means "auto": recomputed from the object's current size
    // (by shape) whenever read. SetMass() switches to a manual override, which
    // ResetMassAuto() clears again (the properties panel does this whenever the
    // object is scaled).
    float GetMass() const;
    void SetMass(float m);
    void ResetMassAuto();
    bool IsMassAuto() const;
    // The stored field (0 = auto), as persisted to disk.
    float GetStoredMass() const { return mass; }
    const std::string& GetName() const;
    void SetName(const std::string& newName);

    // Loads a texture from disk and applies it to this object. Silently
    // keeps the previous (or default) texture if the file can't be loaded.
    void SetTexture(const std::string& path);
    Texture2D GetTexture() const;

    // Loads a 3D mesh (obj/gltf/glb/iqm/vox/m3d) that replaces the primitive
    // shape. raylib loads the file's textures automatically. Returns false
    // (keeping any previous geometry) when the file can't be loaded.
    bool SetModel(const std::string& path);
    const std::string& GetModelPath() const;
    bool HasModel() const;
    // The loaded mesh model (empty/zeroed when there is no own model). Read-only
    // access for building colliders, bounds, etc. from the actual geometry.
    const Model& GetModel() const { return model; }
    // Local-space bounds of the loaded mesh geometry (empty box when there is
    // no model). Bounds are the un-normalized mesh as loaded.
    BoundingBox GetModelBounds() const;
    // Fits the loaded mesh so it spans exactly [-0.5, 0.5] on every axis and
    // sets this object's size to the mesh's original extent. Call after
    // SetModel() at import time so the pick/physics box encloses the visual.
    void SetSizeFromModel();
    // Same normalization as SetSizeFromModel(), but leaves `size` untouched.
    // Call after SetModel() when reloading a scene, where `size` was already
    // read from the save file and must be preserved rather than re-derived.
    void NormalizeModelToUnitBox();

    // Deep copy of this object (same transform, physics, script, texture). The
    // clone shares the source's texture handle without owning it, so it is safe
    // for either object to be destroyed independently.
    std::unique_ptr<ScatteredObject> Clone() const;

    // Physics-driven orientation override. While set, the object renders with
    // this rotation matrix instead of the Euler angles. Cleared with ClearRenderRotation().
    void SetRenderRotation(const Matrix& rot);
    void ClearRenderRotation();

    bool isSelected = false;

    // Editor grouping: the model this object belongs to, or nullptr when it is
    // a standalone top-level object. Owned by the editor, never freed here.
    ModelGroup* parentModel = nullptr;

    // Flyscript attached to this object. Edited from the explorer's
    // right-click menu and executed when Run or Play starts (see runOnPlay).
    std::string script;
    bool runOnPlay = true;

    // When true, the physics simulation treats this object as static:
    // gravity, impulses and forces are ignored and it cannot be pushed around.
    // New objects start anchored; uncheck it in the properties panel
    // (or set Anchored = false in a script) to let it fall.
    bool anchored = true;

    // When false the object ignores all collisions (including the ground).
    bool canCollide = true;

    // Collision fidelity. Cubes (plain primitive, no model) are always Box:
    // IsCollisionAccuracySupported() is false and Get/Set clamp to Box.
    pcoll::CollisionAccuracy GetCollisionAccuracy() const;
    void SetCollisionAccuracy(pcoll::CollisionAccuracy accuracy);
    bool IsCollisionAccuracySupported() const;
    // Unit-space collider built from the current mesh/primitive geometry
    // (cached; invalidated when the accuracy or the mesh geometry changes).
    const pcoll::Collider& GetCollider() const;

    // Frustum culling support
    bool IsVisible(const Frustum& frustum) const override;
    BoundingBox GetCullBounds() const override { return GetBoundingBox(); }

    // Transparency: 0 = fully visible/opaque, 1 = fully invisible/transparent
    float GetTransparency() const;
    void SetTransparency(float t);
    float* GetTransparencyPtr();

    // Texture: project-relative path to custom texture file
    std::string GetTexturePath() const;
    void SetTexturePath(const std::string& projectRelativePath, const std::string& projectDir);
    void ClearTexture(const std::string& projectDir);

private:
    Vector3 pos;
    Vector3 size;
    Color color;
    ShapeType shape;
    Vector3 rotation = { 0.0f, 0.0f, 0.0f };
    Vector3 origin = { 0.0f, 0.0f, 0.0f };
    std::string name = "Object";

    Vector3 velocity = { 0.0f, 0.0f, 0.0f };
    Vector3 angularVelocity = { 0.0f, 0.0f, 0.0f };
    float mass = 0.0f; // 0 = auto (derived from size)

    Matrix renderRotation = MatrixIdentity();
    bool useRenderRotation = false;

    Model model{};
    std::string modelPath;
    bool hasOwnModel = false;

    pcoll::CollisionAccuracy collisionAccuracy = pcoll::CollisionAccuracy::Default;
    mutable pcoll::Collider colliderCache;
    mutable int colliderCacheKey = -1;
    // Bumped whenever the mesh geometry changes so the collider cache reloads.
    unsigned int geometryVersion = 0;

    // Transparency: 0 = fully visible/opaque, 1 = fully invisible/transparent
    float transparency = 0.0f;

    // Texture: project-relative path to custom texture file
    std::string texturePath;
    bool textureNeedsBind = false;  // Set by SetTexturePath, cleared on first draw

    // LOD (Level of Detail) state for preset textures.
    // -1 = uncomputed, 0 = full-res, 1 = medium, 2 = low.
    int currentLODLevel = -1;

    void UpdateLOD();
};

// Called by Engine before the draw loop so objects can compute LOD distance.
void SetLODCameraPos(Vector3 pos);