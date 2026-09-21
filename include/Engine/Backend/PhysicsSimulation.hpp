#pragma once
#include "Entity.hpp"
#include "../PhysicsCollision.hpp"
#include "ScatteredObject.hpp"
#include "Box3DWrapper.hpp"
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
    void Update(float dt) override;

    bool IsPlaying() const { return playing; }

    void SetBodyPosition(ScatteredObject* object, Vector3 position);
    void SetBodyOrientation(ScatteredObject* object, Vector3 eulerDeg);
    Vector3 GetBodyVelocity(ScatteredObject* object) const;
    void SetBodyVelocity(ScatteredObject* object, Vector3 velocity);
    Vector3 GetBodyAngularVelocity(ScatteredObject* object) const;
    void SetBodyAngularVelocity(ScatteredObject* object, Vector3 velocity);

    void SpawnBodyForObject(ScatteredObject* obj);

    b3JointId CreateRevoluteJoint(ScatteredObject* a, ScatteredObject* b, Vector3 anchor);
    b3JointId CreateDistanceJoint(ScatteredObject* a, ScatteredObject* b, Vector3 anchorA, Vector3 anchorB);
    b3JointId CreateWeldJoint(ScatteredObject* a, ScatteredObject* b, Vector3 anchor);
    b3JointId CreateSphericalJoint(ScatteredObject* a, ScatteredObject* b, Vector3 anchor);
    void DestroyJoint(b3JointId jointId);

    RaycastHit RayCast(Vector3 origin, Vector3 end);

    const std::vector<ContactEvent>& GetContactBeginEvents() const { return contactBeginEvents; }
    const std::vector<ContactEvent>& GetContactHitEvents() const { return contactHitEvents; }

    void SetDebugDrawEnabled(bool enabled) { debugDrawEnabled = enabled; }
    bool IsDebugDrawEnabled() const { return debugDrawEnabled; }

    void SetGravity(float g) { gravity = g; }
    float GetGravity() const { return gravity; }
    void SetFriction(float f) { friction = f; }
    float GetFriction() const { return friction; }
    void SetRestitution(float r) { restitutionBase = r; }
    float GetRestitution() const { return restitutionBase; }

    b3WorldId GetWorldId() const;

private:
    void StartPlay();
    void StopPlay();
    void CreateShapeForObject(ScatteredObject* obj, b3BodyId bodyId);
    void WriteBack();
    void ApplyBuoyancy();
    void ProcessEvents();
    void DrawDebug();

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
    std::vector<b3JointId> joints;

    std::vector<ContactEvent> contactBeginEvents;
    std::vector<ContactEvent> contactHitEvents;

    bool debugDrawEnabled = false;

    float gravity = -9.81f;
    float friction = 0.4f;
    float restitutionBase = 0.7f;

    static constexpr float FIXED_DT = 1.0f / 120.0f;
    static constexpr int MAX_STEPS_PER_FRAME = 8;
    static constexpr int SUB_STEPS = 4;
};

} // namespace phys
