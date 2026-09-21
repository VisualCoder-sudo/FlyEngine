#pragma once

#include "box3d/box3d.h"
#include "raylib.h"
#include "raymath.h"

namespace b3wrap {

// ---- Raylib <-> Box3D type conversions ----

inline b3Vec3 ToB3(Vector3 v) {
    return b3Vec3{ v.x, v.y, v.z };
}

inline Vector3 ToRL(b3Vec3 v) {
    return Vector3{ v.x, v.y, v.z };
}

inline b3Quat ToB3Quat(Quaternion q) {
    return b3Quat{ { q.x, q.y, q.z }, q.w };
}

inline Quaternion ToRL(b3Quat q) {
    return Quaternion{ q.v.x, q.v.y, q.v.z, q.s };
}

inline b3Transform ToB3Transform(Vector3 pos, Quaternion ori) {
    return b3Transform{ ToB3(pos), ToB3Quat(ori) };
}

inline b3WorldTransform ToB3WorldTransform(Vector3 pos, Quaternion ori) {
    return b3WorldTransform{ { pos.x, pos.y, pos.z }, ToB3Quat(ori) };
}

// ---- RAII world wrapper ----

class World {
public:
    World() {
        b3WorldDef def = b3DefaultWorldDef();
        def.gravity = b3Vec3{ 0.0f, -9.81f, 0.0f };
        def.enableSleep = true;
        def.enableContinuous = true;
        m_id = b3CreateWorld(&def);
    }

    explicit World(const b3WorldDef& def) : m_id(b3CreateWorld(&def)) {}

    ~World() {
        if (b3World_IsValid(m_id)) {
            b3DestroyWorld(m_id);
        }
    }

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    World(World&& other) noexcept : m_id(other.m_id) {
        other.m_id = b3WorldId{};
    }
    World& operator=(World&& other) noexcept {
        if (this != &other) {
            if (b3World_IsValid(m_id)) b3DestroyWorld(m_id);
            m_id = other.m_id;
            other.m_id = b3WorldId{};
        }
        return *this;
    }

    b3WorldId GetId() const { return m_id; }
    bool IsValid() const { return b3World_IsValid(m_id); }

    void Step(float timeStep, int subStepCount = 4) {
        b3World_Step(m_id, timeStep, subStepCount);
    }

    void SetGravity(Vector3 g) {
        b3World_SetGravity(m_id, ToB3(g));
    }
    Vector3 GetGravity() const {
        return ToRL(b3World_GetGravity(m_id));
    }

    void EnableSleeping(bool flag) { b3World_EnableSleeping(m_id, flag); }
    bool IsSleepingEnabled() const { return b3World_IsSleepingEnabled(m_id); }

    void EnableContinuous(bool flag) { b3World_EnableContinuous(m_id, flag); }

    void SetRestitutionThreshold(float value) { b3World_SetRestitutionThreshold(m_id, value); }
    float GetRestitutionThreshold() const { return b3World_GetRestitutionThreshold(m_id); }

    void SetMaximumLinearSpeed(float speed) { b3World_SetMaximumLinearSpeed(m_id, speed); }
    float GetMaximumLinearSpeed() const { return b3World_GetMaximumLinearSpeed(m_id); }

    void SetContactTuning(float hertz, float dampingRatio, float contactSpeed) {
        b3World_SetContactTuning(m_id, hertz, dampingRatio, contactSpeed);
    }

    void SetUserData(void* userData) { b3World_SetUserData(m_id, userData); }
    void* GetUserData() const { return b3World_GetUserData(m_id); }

    b3BodyEvents GetBodyEvents() const { return b3World_GetBodyEvents(m_id); }
    b3ContactEvents GetContactEvents() const { return b3World_GetContactEvents(m_id); }
    b3SensorEvents GetSensorEvents() const { return b3World_GetSensorEvents(m_id); }
    b3JointEvents GetJointEvents() const { return b3World_GetJointEvents(m_id); }

    int GetAwakeBodyCount() const { return b3World_GetAwakeBodyCount(m_id); }

private:
    b3WorldId m_id{};
};

// ---- Body creation helpers ----

inline b3BodyId CreateBody(b3WorldId worldId, Vector3 pos, Quaternion ori,
                           b3BodyType type = b3_dynamicBody,
                           void* userData = nullptr) {
    b3BodyDef def = b3DefaultBodyDef();
    def.type = type;
    def.position = { pos.x, pos.y, pos.z };
    def.rotation = ToB3Quat(ori);
    def.userData = userData;
    return b3CreateBody(worldId, &def);
}

inline b3ShapeId AddBoxShape(b3BodyId bodyId, Vector3 halfExtents,
                             float density = 1.0f, float friction = 0.4f, float restitution = 0.0f) {
    b3ShapeDef def = b3DefaultShapeDef();
    def.density = density;
    def.baseMaterial.friction = friction;
    def.baseMaterial.restitution = restitution;
    b3BoxHull box = b3MakeBoxHull(halfExtents.x, halfExtents.y, halfExtents.z);
    return b3CreateHullShape(bodyId, &def, &box.base);
}

inline b3ShapeId AddSphereShape(b3BodyId bodyId, float radius,
                                float density = 1.0f, float friction = 0.4f, float restitution = 0.0f) {
    b3ShapeDef def = b3DefaultShapeDef();
    def.density = density;
    def.baseMaterial.friction = friction;
    def.baseMaterial.restitution = restitution;
    b3Sphere sphere;
    sphere.center = b3Vec3_zero;
    sphere.radius = radius;
    return b3CreateSphereShape(bodyId, &def, &sphere);
}

inline b3ShapeId AddCapsuleShape(b3BodyId bodyId, float radius, float halfHeight,
                                 float density = 1.0f, float friction = 0.4f, float restitution = 0.0f) {
    b3ShapeDef def = b3DefaultShapeDef();
    def.density = density;
    def.baseMaterial.friction = friction;
    def.baseMaterial.restitution = restitution;
    b3Capsule capsule;
    capsule.center1 = b3Vec3{ -halfHeight, 0.0f, 0.0f };
    capsule.center2 = b3Vec3{ halfHeight, 0.0f, 0.0f };
    capsule.radius = radius;
    return b3CreateCapsuleShape(bodyId, &def, &capsule);
}

inline b3ShapeId AddHullShape(b3BodyId bodyId, const b3HullData* hull,
                              float density = 1.0f, float friction = 0.4f, float restitution = 0.0f) {
    b3ShapeDef def = b3DefaultShapeDef();
    def.density = density;
    def.baseMaterial.friction = friction;
    def.baseMaterial.restitution = restitution;
    return b3CreateHullShape(bodyId, &def, hull);
}

inline b3ShapeId AddMeshShape(b3BodyId bodyId, const b3MeshData* mesh, b3Vec3 scale,
                              float density = 1.0f, float friction = 0.4f, float restitution = 0.0f) {
    b3ShapeDef def = b3DefaultShapeDef();
    def.density = density;
    def.baseMaterial.friction = friction;
    def.baseMaterial.restitution = restitution;
    return b3CreateMeshShape(bodyId, &def, mesh, scale);
}

// ---- Body state helpers ----

inline void SetBodyTransform(b3BodyId bodyId, Vector3 pos, Quaternion ori) {
    b3Body_SetTransform(bodyId, { pos.x, pos.y, pos.z }, ToB3Quat(ori));
}

inline Vector3 GetBodyPosition(b3BodyId bodyId) {
    return ToRL(b3Body_GetPosition(bodyId));
}

inline Quaternion GetBodyRotation(b3BodyId bodyId) {
    return ToRL(b3Body_GetRotation(bodyId));
}

inline void SetBodyLinearVelocity(b3BodyId bodyId, Vector3 vel) {
    b3Body_SetLinearVelocity(bodyId, ToB3(vel));
}

inline Vector3 GetBodyLinearVelocity(b3BodyId bodyId) {
    return ToRL(b3Body_GetLinearVelocity(bodyId));
}

inline void SetBodyAngularVelocity(b3BodyId bodyId, Vector3 vel) {
    b3Body_SetAngularVelocity(bodyId, ToB3(vel));
}

inline Vector3 GetBodyAngularVelocity(b3BodyId bodyId) {
    return ToRL(b3Body_GetAngularVelocity(bodyId));
}

inline void SetBodyType(b3BodyId bodyId, b3BodyType type) {
    b3Body_SetType(bodyId, type);
}

inline b3BodyType GetBodyType(b3BodyId bodyId) {
    return b3Body_GetType(bodyId);
}

inline void SetBodyAwake(b3BodyId bodyId, bool awake) {
    b3Body_SetAwake(bodyId, awake);
}

inline bool IsBodyAwake(b3BodyId bodyId) {
    return b3Body_IsAwake(bodyId);
}

// ---- Force / torque helpers ----

inline void ApplyForceToCenter(b3BodyId bodyId, Vector3 force, bool wake = true) {
    b3Body_ApplyForceToCenter(bodyId, ToB3(force), wake);
}

inline void ApplyForce(b3BodyId bodyId, Vector3 force, Vector3 worldPoint, bool wake = true) {
    b3Body_ApplyForce(bodyId, ToB3(force), { worldPoint.x, worldPoint.y, worldPoint.z }, wake);
}

inline void ApplyTorque(b3BodyId bodyId, Vector3 torque, bool wake = true) {
    b3Body_ApplyTorque(bodyId, ToB3(torque), wake);
}

inline float GetBodyMass(b3BodyId bodyId) {
    return b3Body_GetMass(bodyId);
}

// ---- Joint creation helpers ----

inline b3JointId CreateRevoluteJoint(b3WorldId worldId, b3BodyId bodyA, b3BodyId bodyB,
                                     Vector3 anchorWorldPos,
                                     bool enableLimit = false, float lowerAngle = 0.0f, float upperAngle = 0.0f,
                                     bool enableMotor = false, float maxTorque = 0.0f, float motorSpeed = 0.0f) {
    b3RevoluteJointDef def = b3DefaultRevoluteJointDef();
    def.base.bodyIdA = bodyA;
    def.base.bodyIdB = bodyB;
    b3WorldTransform wA = b3Body_GetTransform(bodyA);
    b3WorldTransform wB = b3Body_GetTransform(bodyB);
    def.base.localFrameA = b3InvMulWorldTransforms(wA, b3Transform{ ToB3(anchorWorldPos), b3Quat_identity });
    def.base.localFrameB = b3InvMulWorldTransforms(wB, b3Transform{ ToB3(anchorWorldPos), b3Quat_identity });
    def.enableLimit = enableLimit;
    def.lowerAngle = lowerAngle;
    def.upperAngle = upperAngle;
    def.enableMotor = enableMotor;
    def.maxMotorTorque = maxTorque;
    def.motorSpeed = motorSpeed;
    return b3CreateRevoluteJoint(worldId, &def);
}

inline b3JointId CreateDistanceJoint(b3WorldId worldId, b3BodyId bodyA, b3BodyId bodyB,
                                     Vector3 anchorA, Vector3 anchorB,
                                     float length = -1.0f,
                                     bool enableSpring = false, float hertz = 0.0f, float dampingRatio = 0.0f) {
    b3DistanceJointDef def = b3DefaultDistanceJointDef();
    def.base.bodyIdA = bodyA;
    def.base.bodyIdB = bodyB;
    def.base.localFrameA = b3Transform{ ToB3(anchorA), b3Quat_identity };
    def.base.localFrameB = b3Transform{ ToB3(anchorB), b3Quat_identity };
    def.enableSpring = enableSpring;
    def.hertz = hertz;
    def.dampingRatio = dampingRatio;
    if (length >= 0.0f) def.length = length;
    return b3CreateDistanceJoint(worldId, &def);
}

inline b3JointId CreateWeldJoint(b3WorldId worldId, b3BodyId bodyA, b3BodyId bodyB,
                                  Vector3 anchorWorldPos,
                                  float linearHertz = 0.0f, float angularHertz = 0.0f,
                                  float linearDampingRatio = 1.0f, float angularDampingRatio = 1.0f) {
    b3WeldJointDef def = b3DefaultWeldJointDef();
    def.base.bodyIdA = bodyA;
    def.base.bodyIdB = bodyB;
    b3WorldTransform wA = b3Body_GetTransform(bodyA);
    b3WorldTransform wB = b3Body_GetTransform(bodyB);
    def.base.localFrameA = b3InvMulWorldTransforms(wA, b3Transform{ ToB3(anchorWorldPos), b3Quat_identity });
    def.base.localFrameB = b3InvMulWorldTransforms(wB, b3Transform{ ToB3(anchorWorldPos), b3Quat_identity });
    def.linearHertz = linearHertz;
    def.angularHertz = angularHertz;
    def.linearDampingRatio = linearDampingRatio;
    def.angularDampingRatio = angularDampingRatio;
    return b3CreateWeldJoint(worldId, &def);
}

inline b3JointId CreateSphericalJoint(b3WorldId worldId, b3BodyId bodyA, b3BodyId bodyB,
                                       Vector3 anchorWorldPos,
                                       bool enableConeLimit = false, float coneAngle = 0.0f,
                                       bool enableMotor = false, float maxTorque = 0.0f) {
    b3SphericalJointDef def = b3DefaultSphericalJointDef();
    def.base.bodyIdA = bodyA;
    def.base.bodyIdB = bodyB;
    b3WorldTransform wA = b3Body_GetTransform(bodyA);
    b3WorldTransform wB = b3Body_GetTransform(bodyB);
    def.base.localFrameA = b3InvMulWorldTransforms(wA, b3Transform{ ToB3(anchorWorldPos), b3Quat_identity });
    def.base.localFrameB = b3InvMulWorldTransforms(wB, b3Transform{ ToB3(anchorWorldPos), b3Quat_identity });
    def.enableConeLimit = enableConeLimit;
    def.coneAngle = coneAngle;
    def.enableMotor = enableMotor;
    def.maxMotorTorque = maxTorque;
    return b3CreateSphericalJoint(worldId, &def);
}

inline b3JointId CreatePrismaticJoint(b3WorldId worldId, b3BodyId bodyA, b3BodyId bodyB,
                                       Vector3 anchorWorldPos, Vector3 axisWorld,
                                       bool enableLimit = false, float lower = 0.0f, float upper = 0.0f,
                                       bool enableMotor = false, float maxForce = 0.0f, float speed = 0.0f) {
    b3PrismaticJointDef def = b3DefaultPrismaticJointDef();
    def.base.bodyIdA = bodyA;
    def.base.bodyIdB = bodyB;
    b3WorldTransform wA = b3Body_GetTransform(bodyA);
    b3WorldTransform wB = b3Body_GetTransform(bodyB);
    def.base.localFrameA = b3InvMulWorldTransforms(wA, b3Transform{ ToB3(anchorWorldPos), b3Quat_identity });
    def.base.localFrameB = b3InvMulWorldTransforms(wB, b3Transform{ ToB3(anchorWorldPos), b3Quat_identity });
    def.enableLimit = enableLimit;
    def.lowerTranslation = lower;
    def.upperTranslation = upper;
    def.enableMotor = enableMotor;
    def.maxMotorForce = maxForce;
    def.motorSpeed = speed;
    return b3CreatePrismaticJoint(worldId, &def);
}

// ---- Query helpers ----

struct RaycastResult {
    bool hit = false;
    Vector3 point{};
    Vector3 normal{};
    float fraction = 1.0f;
    b3ShapeId shapeId{};
};

inline RaycastResult RayCastClosest(b3WorldId worldId, Vector3 origin, Vector3 end) {
    b3RayResult r = b3World_CastRayClosest(worldId, ToB3(origin), ToB3(Vector3Subtract(end, origin)), b3DefaultQueryFilter());
    RaycastResult res;
    res.hit = r.hit;
    res.point = ToRL(r.point);
    res.normal = ToRL(r.normal);
    res.fraction = r.fraction;
    res.shapeId = r.shapeId;
    return res;
}

// ---- Sensor shape helpers ----

inline b3ShapeId AddSensorBox(b3BodyId bodyId, Vector3 halfExtents) {
    b3ShapeDef def = b3DefaultShapeDef();
    def.isSensor = true;
    def.density = 0.0f;
    b3BoxHull box = b3MakeBoxHull(halfExtents.x, halfExtents.y, halfExtents.z);
    return b3CreateHullShape(bodyId, &def, &box.base);
}

inline b3ShapeId AddSensorSphere(b3BodyId bodyId, float radius) {
    b3ShapeDef def = b3DefaultShapeDef();
    def.isSensor = true;
    def.density = 0.0f;
    b3Sphere sphere{};
    sphere.radius = radius;
    return b3CreateSphereShape(bodyId, &def, &sphere);
}

} // namespace b3wrap
