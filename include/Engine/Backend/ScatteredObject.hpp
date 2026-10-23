#pragma once
#include "Entity.hpp"
#include "Frustum.hpp"
#include "ModelAnimator.hpp"
#include "ObjectNode.hpp"
#include "../PhysicsCollision.hpp"
#include "../../Graphics/SurfaceMaterial.hpp"
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

// How a part takes part in a Union: Add merges it in, Negate carves it out of the result, Intersect keeps only
// the volume it shares with the result.
enum class UnionMode {
    Add,
    Negate,
    Intersect
};

// One source part of a Union. Its position is relative to the union's centre (axis-aligned with the union
// at the time it was made), so the union can be moved and scaled and the parts still follow.
struct UnionPart {
    std::unique_ptr<ScatteredObject> obj;
    UnionMode mode = UnionMode::Add;
};

class ModelGroup {
public:
    std::string name = "Model";
    std::vector<ScatteredObject*> members;

    // Nesting: a model can sit inside another model. Both links are editor-owned
    // and kept in sync by the explorer; the model vector still owns the storage.
    ModelGroup* parent = nullptr;
    std::vector<ModelGroup*> children;

    // Explorer toggles. Flags on a group apply to everything below it.
    bool hidden = false;
    bool locked = false;

    // Plugin ownership. A plugin that generates this group's contents (e.g. a
    // building sketch tool) tags it and keeps its editable source data here.
    // Both strings are saved with the scene and with undo/play snapshots, so the
    // data survives save/load, undo and export. Empty tag = ordinary group.
    std::string pluginTag;
    std::string pluginData;

    // Axis-aligned union of all member and child bounds. Zero box if empty.
    BoundingBox GetBounds() const;

    bool IsDescendantOf(const ModelGroup* other) const;
    ModelGroup* Root();
    // Every object in this group and all nested groups.
    void CollectObjects(std::vector<ScatteredObject*>& out) const;
    bool IsHiddenEffective() const;
    bool IsLockedEffective() const;
    void AddChild(ModelGroup* child);
    void RemoveChild(ModelGroup* child);
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
    // A plain opaque cube big enough to hide things behind it: fills `out` with its 8 world-space corners
    // and returns true. Used by occlusion culling.
    bool GetOccluderCorners(Vector3 out[8], float& volume) const;
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
    // Starts parsing an FBX on a worker thread so a later SetModel() on the same
    // path doesn't stall the frame. No-op for other formats / already-loaded files.
    static void PrefetchModel(const std::string& path);
    // True once SetModel(path) will not block (prefetch finished, or none pending).
    static bool IsModelReady(const std::string& path);
    const std::string& GetModelPath() const;

    // Skeletal animation: imported glTF/IQM/M3D models that carry clips play the
    // first one in a loop. FBX models are baked to their bind pose and have none.
    int GetAnimationCount() const { return animator ? animator->ClipCount() : 0; }
    const char* GetAnimationName(int clip) const { return animator ? animator->ClipName(clip) : ""; }
    int GetCurrentAnimation() const { return animator ? animator->CurrentClip() : -1; }
    bool IsAnimationPlaying() const { return animator && animator->IsPlaying(); }
    bool PlayAnimation(int clip, bool loop = true, float speed = 1.0f);
    bool PlayAnimation(const std::string& name, bool loop = true, float speed = 1.0f);
    void StopAnimation();
    // Chosen clip, saved with the scene and applied whenever the model attaches
    // (also before it has loaded). Empty = the model's first clip.
    std::string animClip;
    bool animLoop = true;
    bool animPlay = true;      // false = held on the clip's first frame
    float animSpeed = 1.0f;
    void ApplyAnimation();     // pushes the four settings above into the animator
    // Absolute path of the texture that came with the model file (empty if none).
    const std::string& GetModelDiffuseFile() const { return modelDiffuseFile; }
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
    // World-space heading of the object's local X axis (physics orientation if set, else Euler).
    float GetYaw() const;
    // The rotation the object renders with: the physics orientation if set, else the Euler angles.
    Matrix GetRotationMatrix() const;

    bool isSelected = false;
    bool isHovered = false;   // editor only: the cursor is over it, drawn with the hover outline

    // Editor grouping: the model this object belongs to, or nullptr when it is
    // a standalone top-level object. Owned by the editor, never freed here.
    ModelGroup* parentModel = nullptr;

    // Explorer eye / lock toggles. Hidden objects are not drawn, locked ones
    // cannot be picked in the viewport. Use the Effective helpers to include
    // the parent chain.
    bool hidden = false;
    bool locked = false;
    bool IsHiddenEffective() const;
    bool IsLockedEffective() const;

    // Flyscript attached to this object. Edited from the explorer's
    // right-click menu and executed when Run or Play starts (see runOnPlay).
    // `script` is the first script; `extraScripts` are the others (an object can carry several).
    std::string script;
    std::vector<std::string> extraScripts;
    bool runOnPlay = true;

    // Child nodes (lights, decals, textures) inserted from the explorer. They move with the object.
    std::vector<ObjectNode> nodes;
    // Queues this object's enabled Decal / Texture nodes for gfx::DrawNodeSurfaces (main pass only).
    void QueueNodeSurfaces() const;
    // Where a light node sits in the world: its position, the unit axis it points along (zero for a point light) and,
    // for a surface light, the half-extents of its emitting rectangle.
    void GetNodeEmitter(const ObjectNode& node, Vector3& pos, Vector3& dir, Vector3& halfU, Vector3& halfV) const;
    // Editor only: outlines of the object's light nodes (range, cone, emitter), drawn while it is selected.
    void DrawNodeGizmos() const;

    // Every attached script class, primary first.
    std::vector<std::string> AllScripts() const;
    bool HasScript(const std::string& name) const;
    void AddScript(const std::string& name);
    // Removes one script; removing the first promotes the next one.
    void RemoveScript(const std::string& name);

    // When true, the physics simulation treats this object as static:
    // gravity, impulses and forces are ignored and it cannot be pushed around.
    // New objects start anchored; uncheck it in the properties panel
    // (or set Anchored = false in a script) to let it fall.
    bool anchored = true;

    // When false the object ignores all collisions (including the ground).
    bool canCollide = true;

    // How the object reacts to water bodies (applied by the physics simulation
    // while the object is in water).
    struct WaterInteraction {
        bool floats = true;  // buoyancy; off = sinks
        bool foam   = true;  // leaves a wake / foam trail
        bool splash = true;  // splash and spray on entering the water
    };
    WaterInteraction waterInteraction;

    // Collision fidelity. Cubes (plain primitive, no model) are always Box:
    // IsCollisionAccuracySupported() is false and Get/Set clamp to Box.
    pcoll::CollisionAccuracy GetCollisionAccuracy() const;
    void SetCollisionAccuracy(pcoll::CollisionAccuracy accuracy);
    bool IsCollisionAccuracySupported() const;
    // Unit-space collider built from the current mesh/primitive geometry
    // (cached; invalidated when the accuracy or the mesh geometry changes).
    const pcoll::Collider& GetCollider() const;

    // ---- Unions (solid modelling) -------------------------------------------------------------------------------
    // A union is one object whose mesh is the CSG result of its source parts. Faces keep the colour of the part
    // they came from, unless useGlobalColor is on: then the whole union is drawn in `color`. The parts are kept
    // so Separate can hand them back. Only primitive parts (no imported meshes) can be unioned.
    //
    // Builds a union from `parts` placed in the world (positions/sizes/rotations as in the scene) and the mode of
    // each. The union is centred on the result and the parts are re-expressed relative to it. Null when the
    // result is empty (e.g. nothing is left after subtracting) or fewer than one positive part is given.
    static std::unique_ptr<ScatteredObject> MakeUnion(std::vector<std::unique_ptr<ScatteredObject>> parts,
                                                      const std::vector<UnionMode>& modes);
    bool IsUnion() const { return !unionParts.empty(); }
    // The union's parts, positions relative to its centre. Empty for an ordinary object.
    std::vector<UnionPart>& UnionParts() { return unionParts; }
    const std::vector<UnionPart>& UnionParts() const { return unionParts; }
    // Size of the union as built: the object's size divided by this is the scale the user applied since.
    Vector3 unionBaseSize{ 1.0f, 1.0f, 1.0f };
    // Off: every face keeps its source part's colour. On: the whole union is `color`.
    bool useGlobalColor = false;
    void SetUseGlobalColor(bool on);
    // Re-runs the CSG from the parts and replaces the mesh. Called after load and whenever the parts change.
    bool RebuildUnionMesh(Vector3* outCenter = nullptr);
    // Hands the parts back as free-standing objects, placed where the union (with its move/rotate/scale) puts
    // them. The union is left empty.
    std::vector<std::unique_ptr<ScatteredObject>> TakeUnionParts();

    // A part marked Negate is a cutter: shown as a translucent red ghost in the editor, never collides, and
    // is carved out of a Union it is made part of.
    bool negate = false;

    // Frustum culling support
    bool IsVisible(const Frustum& frustum) const override;
    BoundingBox GetCullBounds() const override { return GetBoundingBox(); }

    // Physically based material (metallic, roughness, emission). A primitive shape always uses it. An imported
    // model uses its file's own materials (and their maps) while useModelMaterial is on; off, `surface` replaces
    // their values on every part, with the maps still modulating them. A non-zero emission always applies.
    gfx::SurfaceMaterial surface;
    bool useModelMaterial = true;

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
    std::string sharedModelKey;     // key into the shared mesh cache
    std::unique_ptr<ModelAnimator> animator;   // private skinned meshes; null for static models
    unsigned long long animFrame = ~0ull;      // render frame the pose was last skinned on
    Vector3 modelExtent{ 1.0f, 1.0f, 1.0f };
    std::string modelDiffuseFile;   // base-colour texture shipped with the model (abs path)
    void ReleaseModel();

    // Union state (see IsUnion). The mesh is built here, not loaded from a file, so it is owned by this object.
    std::vector<UnionPart> unionParts;
    bool ownsProceduralMesh = false;
    std::vector<std::vector<unsigned char>> unionVertexColors;   // per mesh: each face's source-part colour
    void ApplyUnionColors();

    pcoll::CollisionAccuracy collisionAccuracy = pcoll::CollisionAccuracy::Default;
    // Colliders live in unit space, so every object using the same mesh (or
    // primitive) and accuracy shares one instance.
    mutable std::shared_ptr<const pcoll::Collider> colliderCache;
    mutable int colliderCacheKey = -1;
    // Bumped whenever the mesh geometry changes so the collider cache reloads.
    unsigned int geometryVersion = 0;

    // Last GetBoundingBox() answer and the pos/size/rotation it was computed from.
    mutable Vector3 boundsCacheKey[3]{};
    mutable BoundingBox boundsCache{};
    mutable bool boundsCacheValid = false;

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