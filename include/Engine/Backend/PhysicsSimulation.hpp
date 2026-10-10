#pragma once
#include "Entity.hpp"
#include "../PhysicsCollision.hpp"
#include "ScatteredObject.hpp"
#include "Box3DWrapper.hpp"
#include "../../Terrain/TerrainCollider.hpp"
#include "raylib.h"
#include "raymath.h"
#include <unordered_map>
#include <vector>

namespace phys {

class Simulation;

struct RaycastHit {
    bool hit = false;
    Vector3 point{};
    Vector3 normal{};
    float fraction = 1.0f;
    ScatteredObject* object = nullptr;
};

struct ContactEvent {
    ScatteredObject* objectA = nullptr;
    ScatteredObject* objectB = nullptr;
    Vector3 point{};
    Vector3 normal{};
    float approachSpeed = 0.0f;
};

class Simulation : public Entity {
public:
    explicit Simulation(std::vector<ScatteredObject*>& objects);
    ~Simulation() override;
    void Update(float dt) override;

    bool IsPlaying() const { return playing; }

    void SetBodyPosition(ScatteredObject* object, Vector3 position);
    void SetBodyOrientation(ScatteredObject* object, Vector3 eulerDeg);
    Vector3 GetBodyVelocity(ScatteredObject* object) const;
    void SetBodyVelocity(ScatteredObject* object, Vector3 velocity);
    Vector3 GetBodyAngularVelocity(ScatteredObject* object) const;
    void SetBodyAngularVelocity(ScatteredObject* object, Vector3 velocity);

    void ApplyForceToBody(ScatteredObject* object, Vector3 force, bool wake = true);
    void ApplyImpulseToBody(ScatteredObject* object, Vector3 impulse, bool wake = true);

    void StartPlay();  // Made public for player auto-start
    void SpawnBodyForObject(ScatteredObject* obj, b3BodyType type = b3_dynamicBody);
    // Removes the physical body for an object that is being destroyed. Must be
    // called before the ScatteredObject is freed so bodyMap/bodyToObject never
    // hold dangling pointers (the editor can delete objects mid-play).
    void RemoveObject(ScatteredObject* obj);

    // Change body type (for player controller kinematic movement)
    void SetBodyType(ScatteredObject* obj, b3BodyType type);

    b3JointId CreateRevoluteJoint(ScatteredObject* a, ScatteredObject* b, Vector3 anchor);
    b3JointId CreateDistanceJoint(ScatteredObject* a, ScatteredObject* b, Vector3 anchorA, Vector3 anchorB);
    b3JointId CreateWeldJoint(ScatteredObject* a, ScatteredObject* b, Vector3 anchor);
    b3JointId CreateSphericalJoint(ScatteredObject* a, ScatteredObject* b, Vector3 anchor);
    void DestroyJoint(b3JointId jointId);

    RaycastHit RayCast(Vector3 origin, Vector3 end);
    RaycastHit RayCast(Vector3 origin, Vector3 direction, float maxDistance, ScatteredObject* ignore = nullptr);

    // Move a kinematic body by delta (for CharacterController)
    void MoveKinematic(ScatteredObject* obj, Vector3 delta);

    // Kinematic character queries. The capsule is upright and centred on
    // `position`: `radius` plus a straight section running halfSegment above
    // and below the centre, so its total height is 2 * (halfSegment + radius).
    // Everything solid is collided against (terrain, city, static and dynamic
    // bodies) except `self`'s own body and sensors.
    struct MoverResult {
        Vector3 position{};   // where the capsule ended up
        Vector3 moved{};      // position - start; less than the request when something blocked it
    };
    // Slides the capsule by `delta`: it stops at obstacles, slides along walls
    // and slopes, and is pushed out of anything it already overlaps. When
    // flattenBelowNormalY > 0, surfaces whose normal.y is in [0, flattenBelowNormalY)
    // (walls and slopes too steep to stand on) act as vertical walls, so
    // pushing into them cannot shove the capsule up the slope.
    MoverResult MoveCapsule(ScatteredObject* self, Vector3 position, Vector3 delta, float radius, float halfSegment,
                            float flattenBelowNormalY = 0.0f);
    // Fraction (0..1) of `delta` the capsule can travel before touching something.
    float CastCapsule(ScatteredObject* self, Vector3 position, Vector3 delta, float radius, float halfSegment);
    // Finds the most upward-facing surface touching the bottom of the capsule.
    // True when its normal.y >= minNormalY (i.e. walkable); outNormal is that surface's normal.
    bool GetCapsuleGround(ScatteredObject* self, Vector3 position, float radius, float halfSegment,
                          float minNormalY, Vector3& outNormal);

    const std::vector<ContactEvent>& GetContactBeginEvents() const { return contactBeginEvents; }
    const std::vector<ContactEvent>& GetContactHitEvents() const { return contactHitEvents; }

    // Debug draw. This class collects the data (contact events, body transforms,
    // joint endpoints) and the PhysicsDebugVisualizer in TechnicalTools.cpp
    // draws it, because that is where the F2 toggles live. This used to also
    // carry a second, self-contained renderer behind its own "debug draw
    // enabled" flag that nothing ever set, so it never ran and its drawing logic
    // rotted. Having one renderer means the toggles and the drawing cannot
    // disagree about what is being shown.
    //
    // Authoritative transform of an object's physics body. The object's own
    // position is synced by WriteBack(), but its euler rotation is not (that
    // goes to the render matrix), so a debug overlay reading *GetRotationPtr()
    // would show a stale orientation during play.
    bool GetBodyTransform(ScatteredObject* obj, Vector3& outPos, Quaternion& outRot) const;

    // World-space endpoints of every joint, for the debug overlay.
    void GetJointSegments(std::vector<std::pair<Vector3, Vector3>>& out) const;

    void SetGravity(float g) { gravity = g; }
    float GetGravity() const { return gravity; }
    void SetFriction(float f) { friction = f; }
    float GetFriction() const { return friction; }
    void SetRestitution(float r) { restitutionBase = r; }
    float GetRestitution() const { return restitutionBase; }

    b3WorldId GetWorldId() const;
    
    // Debug stats
    int GetBodyCount() const { return (int)bodyMap.size(); }
    int GetContactCount() const { return (int)(contactBeginEvents.size() + contactHitEvents.size()); }
    double GetLastStepTimeMs() const { return lastStepTimeMs; }
    
    // Check if object has a physics body
    bool HasBody(ScatteredObject* obj) const { return bodyMap.find(obj) != bodyMap.end(); }

private:
    bool FindBodyId(ScatteredObject* obj, b3BodyId& out) const;
    void StopPlay();
    void CreateShapeForObject(ScatteredObject* obj, b3BodyId bodyId);
    void WriteBack();
    void ApplyBuoyancy();
    void ProcessEvents();

    std::vector<ScatteredObject*>& objects;
    bool playing = false;
    float accumulator = 0.0f;
    double playStartTime = 0.0;

    std::unique_ptr<b3wrap::World> world;

    struct BodyRecord {
        b3BodyId bodyId{};
        Vector3 startPos{};
        Vector3 startLinVel{};
        Vector3 startAngVel{};
        bool wasAnchored = false;
        bool wasSubmerged = false;
        bool wasSurfaceContact = false;
        float prevBodyBottom = 0.0f;
        float prevWaterHeight = 0.0f;
        Vector3 lastWaterForce{};
    };
    std::unordered_map<ScatteredObject*, BodyRecord> bodyMap;
    std::vector<std::pair<b3BodyId, ScatteredObject*>> bodyToObject;

    std::vector<b3HullData*> hulls;
    std::vector<b3MeshData*> meshes;
    std::vector<terrain::TerrainCollider> terrainColliders;
    std::vector<b3JointId> joints;

    std::vector<ContactEvent> contactBeginEvents;
    std::vector<ContactEvent> contactHitEvents;

    // Overwritten from the "physics_gravity" cvar in StartPlay(), where the
    // box3d world is built. Kept at -9.81 (Earth), which is what the engine
    // has always simulated: Box3DWrapper.hpp and CharacterController.hpp both
    // hardcode the same value, so this is the real baseline. The cvar used to
    // advertise -19.62 while nothing read it, which made the documented default
    // and the actual one disagree; the cvar is now the side that changed.
    float gravity = -9.81f;
    float friction = 0.4f;
    float restitutionBase = 0.7f;
    double lastStepTimeMs = 0.0;

    // The fixed timestep and the per-frame sub-step cap are the two knobs that
    // decide how the simulation trades accuracy for CPU. They used to be
    // `static constexpr` and unreadable at runtime, with matching cvars
    // ("physics.fixed_dt", "physics.max_substeps") registered alongside them
    // that nothing ever consulted. Sampling the cvar in Step() is a map lookup
    // per frame, which is nothing next to the box3d solve it governs.
    static constexpr float kDefaultFixedDt = 1.0f / 120.0f;
    static constexpr int kDefaultMaxStepsPerFrame = 8;
    static constexpr int SUB_STEPS = 4;

    float fixedDt = kDefaultFixedDt;
    int maxStepsPerFrame = kDefaultMaxStepsPerFrame;
};

} // namespace phys
