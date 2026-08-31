#include "PhysicsSimulation.hpp"
#include "WaterBody.hpp"
#include "Flyscript.hpp"
#include "Graphics.hpp"
#include "Box3DWrapper.hpp"
#include "ui.hpp"
#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace phys {

namespace {

void ExtractMeshVerts(ScatteredObject* obj, std::vector<Vector3>& out) {
    auto extractFromMesh = [&](const Mesh& mesh) {
        if (mesh.vertices == nullptr || mesh.triangleCount <= 0) return;
        if (mesh.indices != nullptr) {
            for (int i = 0; i < mesh.triangleCount; ++i) {
                const unsigned short* idx = &mesh.indices[i * 3];
                if ((int)idx[0] >= mesh.vertexCount ||
                    (int)idx[1] >= mesh.vertexCount ||
                    (int)idx[2] >= mesh.vertexCount) continue;
                const float* a = &mesh.vertices[idx[0] * 3];
                const float* b = &mesh.vertices[idx[1] * 3];
                const float* c = &mesh.vertices[idx[2] * 3];
                out.push_back({ a[0], a[1], a[2] });
                out.push_back({ b[0], b[1], b[2] });
                out.push_back({ c[0], c[1], c[2] });
            }
        } else {
            int triCount = mesh.triangleCount;
            int maxTri = mesh.vertexCount / 3;
            if (triCount > maxTri) triCount = maxTri;
            for (int i = 0; i < triCount; ++i) {
                const float* a = &mesh.vertices[(i * 3 + 0) * 3];
                const float* b = &mesh.vertices[(i * 3 + 1) * 3];
                const float* c = &mesh.vertices[(i * 3 + 2) * 3];
                out.push_back({ a[0], a[1], a[2] });
                out.push_back({ b[0], b[1], b[2] });
                out.push_back({ c[0], c[1], c[2] });
            }
        }
    };
    // Use the object's own imported model when present (the precise/hull
    // colliders must be built from the actual mesh geometry), otherwise fall
    // back to the shared primitive shape model.
    if (obj->HasModel() && obj->GetModel().meshes != nullptr) {
        const Model& cm = obj->GetModel();
        for (int i = 0; i < cm.meshCount; ++i) extractFromMesh(cm.meshes[i]);
        return;
    }
    Model& m = gfx::GetShapeModel(obj->GetShapeType());
    for (int i = 0; i < m.meshCount; ++i) extractFromMesh(m.meshes[i]);
}

std::vector<b3Vec3> DedupeVertsToB3(const std::vector<Vector3>& raw) {
    if (raw.empty()) {
        return { b3Vec3{ -0.5f,-0.5f,-0.5f }, b3Vec3{ 0.5f,-0.5f,-0.5f },
                 b3Vec3{ 0.5f, 0.5f,-0.5f }, b3Vec3{ -0.5f, 0.5f,-0.5f },
                 b3Vec3{ -0.5f,-0.5f, 0.5f }, b3Vec3{ 0.5f,-0.5f, 0.5f },
                 b3Vec3{ 0.5f, 0.5f, 0.5f }, b3Vec3{ -0.5f, 0.5f, 0.5f } };
    }
    Vector3 mn{ FLT_MAX, FLT_MAX, FLT_MAX };
    Vector3 mx{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (auto& v : raw) {
        mn.x = fminf(mn.x, v.x); mn.y = fminf(mn.y, v.y); mn.z = fminf(mn.z, v.z);
        mx.x = fmaxf(mx.x, v.x); mx.y = fmaxf(mx.y, v.y); mx.z = fmaxf(mx.z, v.z);
    }
    Vector3 ext = Vector3Subtract(mx, mn);
    float maxExt = fmaxf(ext.x, fmaxf(ext.y, ext.z));
    if (maxExt < 1e-7f) {
        std::vector<b3Vec3> result;
        for (auto& v : raw) {
            if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z))
                result.push_back({ v.x, v.y, v.z });
        }
        return result;
    }
    float cell = fmaxf(maxExt / 65536.0f, 1e-6f);
    struct GridKey { int x, y, z; };
    struct GridLess {
        bool operator()(const GridKey& a, const GridKey& b) const {
            if (a.x != b.x) return a.x < b.x;
            if (a.y != b.y) return a.y < b.y;
            return a.z < b.z;
        }
    };
    std::map<GridKey, int, GridLess> cells;
    std::vector<b3Vec3> result;
    for (auto& p : raw) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
        int ix = std::clamp((int)((p.x - mn.x) / cell), 0, 65535);
        int iy = std::clamp((int)((p.y - mn.y) / cell), 0, 65535);
        int iz = std::clamp((int)((p.z - mn.z) / cell), 0, 65535);
        GridKey k{ ix, iy, iz };
        if (cells.insert(std::make_pair(k, (int)result.size())).second) {
            result.push_back({ p.x, p.y, p.z });
        }
    }
    return result;
}

struct IndexedTriMesh {
    std::vector<b3Vec3> vertices;
    std::vector<int32_t> indices;
};

IndexedTriMesh BuildIndexedTriMesh(const std::vector<Vector3>& raw) {
    IndexedTriMesh result;
    std::map<std::tuple<int, int, int>, int32_t> vertMap;
    auto getOrAdd = [&](Vector3 v) -> int32_t {
        int ix = (int)std::round(v.x * 10000.0f);
        int iy = (int)std::round(v.y * 10000.0f);
        int iz = (int)std::round(v.z * 10000.0f);
        auto key = std::make_tuple(ix, iy, iz);
        auto it = vertMap.find(key);
        if (it != vertMap.end()) return it->second;
        int32_t idx = (int32_t)result.vertices.size();
        result.vertices.push_back({ v.x, v.y, v.z });
        vertMap[key] = idx;
        return idx;
    };
    for (size_t i = 0; i + 2 < raw.size(); i += 3) {
        result.indices.push_back(getOrAdd(raw[i]));
        result.indices.push_back(getOrAdd(raw[i + 1]));
        result.indices.push_back(getOrAdd(raw[i + 2]));
    }
    return result;
}

} // namespace

static ScatteredObject* FindObjectByBody(const std::vector<std::pair<b3BodyId, ScatteredObject*>>& map, b3BodyId id) {
    for (auto& [bid, obj] : map) {
        if (bid.index1 == id.index1 && bid.world0 == id.world0 && bid.generation == id.generation)
            return obj;
    }
    return nullptr;
}

Simulation::Simulation(std::vector<ScatteredObject*>& objects) : objects(objects) {}

b3WorldId Simulation::GetWorldId() const {
    return world && world->IsValid() ? world->GetId() : b3WorldId{};
}

void Simulation::SetBodyPosition(ScatteredObject* object, Vector3 position) {
    auto it = bodyMap.find(object);
    if (it != bodyMap.end() && b3Body_IsValid(it->second.bodyId)) {
        Quaternion ori = b3wrap::GetBodyRotation(it->second.bodyId);
        b3wrap::SetBodyTransform(it->second.bodyId, position, ori);
        b3wrap::SetBodyAwake(it->second.bodyId, true);
    }
}

void Simulation::SetBodyOrientation(ScatteredObject* object, Vector3 eulerDeg) {
    auto it = bodyMap.find(object);
    if (it != bodyMap.end() && b3Body_IsValid(it->second.bodyId)) {
        Quaternion ori = QuaternionFromMatrix(
            MatrixRotateXYZ({ DEG2RAD * eulerDeg.x, DEG2RAD * eulerDeg.y, DEG2RAD * eulerDeg.z }));
        Vector3 pos = b3wrap::GetBodyPosition(it->second.bodyId);
        b3wrap::SetBodyTransform(it->second.bodyId, pos, ori);
        b3wrap::SetBodyAwake(it->second.bodyId, true);
    }
}

Vector3 Simulation::GetBodyVelocity(ScatteredObject* object) const {
    auto it = bodyMap.find(object);
    if (it != bodyMap.end() && b3Body_IsValid(it->second.bodyId))
        return b3wrap::GetBodyLinearVelocity(it->second.bodyId);
    return Vector3Zero();
}

void Simulation::SetBodyVelocity(ScatteredObject* object, Vector3 velocity) {
    auto it = bodyMap.find(object);
    if (it != bodyMap.end() && b3Body_IsValid(it->second.bodyId)) {
        if (b3Body_GetType(it->second.bodyId) == b3_staticBody) return;
        b3wrap::SetBodyLinearVelocity(it->second.bodyId, velocity);
        b3wrap::SetBodyAwake(it->second.bodyId, true);
    }
}

Vector3 Simulation::GetBodyAngularVelocity(ScatteredObject* object) const {
    auto it = bodyMap.find(object);
    if (it != bodyMap.end() && b3Body_IsValid(it->second.bodyId))
        return b3wrap::GetBodyAngularVelocity(it->second.bodyId);
    return Vector3Zero();
}

void Simulation::SetBodyAngularVelocity(ScatteredObject* object, Vector3 velocity) {
    auto it = bodyMap.find(object);
    if (it != bodyMap.end() && b3Body_IsValid(it->second.bodyId)) {
        if (b3Body_GetType(it->second.bodyId) == b3_staticBody) return;
        b3wrap::SetBodyAngularVelocity(it->second.bodyId, velocity);
        b3wrap::SetBodyAwake(it->second.bodyId, true);
    }
}

void Simulation::Update(float dt) {
    if (ui::ConsumePlayToggle()) {
        if (!playing && !ui::IsScriptWarningActive()) {
            std::vector<flyscript::ScriptWarning> allWarnings;
            if (flyscript::IsRuntimeReady()) {
                auto& rt = flyscript::GetRuntime();
                for (size_t i = 0; i < rt.scripts.size(); ++i) {
                    if (!rt.scripts[i].runOnPlay || rt.scripts[i].source.empty()) continue;
                    auto warns = flyscript::CheckScriptSafety(rt.scripts[i].name, rt.scripts[i].source);
                    allWarnings.insert(allWarnings.end(), warns.begin(), warns.end());
                }
                for (auto* obj : rt.GetObjects()) {
                    if (!obj || !obj->runOnPlay || obj->script.empty()) continue;
                    auto warns = flyscript::CheckScriptSafety(obj->GetName(), obj->script);
                    allWarnings.insert(allWarnings.end(), warns.begin(), warns.end());
                }
            }
            if (!allWarnings.empty()) {
                ui::ShowScriptWarning(allWarnings);
                return;
            }
        }
        playing = !playing;
        ui::SetPlayActive(playing);
        if (playing) {
            StartPlay();
        } else {
            StopPlay();
        }
    }
    if (!playing && ui::ConsumeScriptWarningApproved()) {
        playing = true;
        ui::SetPlayActive(true);
        StartPlay();
    }
    if (!playing) return;

    accumulator += dt;
    int steps = 0;
    while (accumulator >= FIXED_DT && steps < MAX_STEPS_PER_FRAME) {
        contactBeginEvents.clear();
        contactHitEvents.clear();
        ApplyBuoyancy();
        world->Step(FIXED_DT, SUB_STEPS);
        ProcessEvents();
        accumulator -= FIXED_DT;
        ++steps;
    }
    if (steps == MAX_STEPS_PER_FRAME) accumulator = 0.0f;

    WriteBack();
    if (debugDrawEnabled) DrawDebug();
}

void Simulation::StartPlay() {
    bodyMap.clear();
    bodyToObject.clear();
    hulls.clear();
    meshes.clear();
    joints.clear();
    contactBeginEvents.clear();
    contactHitEvents.clear();
    ui::ClearLog();
    playStartTime = GetTime();

    b3WorldDef def = b3DefaultWorldDef();
    def.gravity = b3Vec3{ 0.0f, gravity, 0.0f };
    def.enableSleep = true;
    def.enableContinuous = true;
    def.restitutionThreshold = 2.0f;
    def.contactHertz = 30.0f;
    def.contactDampingRatio = 1.0f;
    def.contactSpeed = 4.0f;
    def.maximumLinearSpeed = 60.0f;
    world = std::make_unique<b3wrap::World>(def);

    b3BodyId groundBody = b3wrap::CreateBody(world->GetId(), Vector3{ 0.0f, -1.0f, 0.0f }, QuaternionIdentity(), b3_staticBody);
    b3wrap::AddBoxShape(groundBody, Vector3{ 1000.0f, 1.0f, 1000.0f }, 0.0f, friction, restitutionBase);

    for (ScatteredObject* obj : objects) {
        if (!obj) continue;

        Vector3 pos = *obj->GetPosPtr();
        Vector3 rot = *obj->GetRotationPtr();
        Quaternion ori = QuaternionFromMatrix(
            MatrixRotateXYZ({ DEG2RAD * rot.x, DEG2RAD * rot.y, DEG2RAD * rot.z }));

        bool anchored = obj->anchored;
        b3BodyType type = anchored ? b3_staticBody : b3_dynamicBody;

        b3BodyId bodyId = b3wrap::CreateBody(world->GetId(), pos, ori, type);
        b3Body_SetName(bodyId, obj->GetName().c_str());

        CreateShapeForObject(obj, bodyId);

        if (!anchored) {
            b3wrap::SetBodyLinearVelocity(bodyId, obj->GetVelocity());
            b3wrap::SetBodyAngularVelocity(bodyId, obj->GetAngularVelocity());
        }

        BodyRecord rec;
        rec.bodyId = bodyId;
        rec.startPos = pos;
        rec.startLinVel = obj->GetVelocity();
        rec.startAngVel = obj->GetAngularVelocity();
        rec.wasAnchored = anchored;
        bodyMap[obj] = rec;
        bodyToObject.push_back({ bodyId, obj });
    }

    ui::Log("Playtest Session Started(%d bodies)", (int)bodyMap.size());
    ui::Log("Attempting to start Playtest");
}

void Simulation::CreateShapeForObject(ScatteredObject* obj, b3BodyId bodyId) {
    Vector3 size = *obj->GetSizePtr();
    Vector3 halfExtents = Vector3Scale(size, 0.5f);
    pcoll::CollisionAccuracy accuracy = obj->GetCollisionAccuracy();

    float mass = fmaxf(obj->GetMass(), 1e-3f);
    float volume;
    if (obj->GetShapeType() == ShapeType::Sphere) {
        float radius = (size.x + size.y + size.z) / 6.0f;
        volume = (4.0f / 3.0f) * PI * radius * radius * radius;
    } else if (obj->GetShapeType() == ShapeType::Cylinder) {
        float radius = (size.x + size.z) / 4.0f;
        volume = PI * radius * radius * size.y;
    } else {
        volume = size.x * size.y * size.z;
    }
    float density = mass / fmaxf(volume, 1e-6f);
    float restBase = fmaxf(0.0f, fminf(1.0f, 1.0f - 0.12f * mass)) * restitutionBase;

    auto shapeDef = b3DefaultShapeDef();
    shapeDef.density = density;
    shapeDef.baseMaterial.friction = friction;
    shapeDef.baseMaterial.restitution = restBase;

    switch (accuracy) {
        case pcoll::CollisionAccuracy::Box: {
            if (obj->GetShapeType() == ShapeType::Sphere) {
                b3Sphere sphere{};
                sphere.radius = (size.x + size.y + size.z) / 6.0f;
                b3CreateSphereShape(bodyId, &shapeDef, &sphere);
            } else {
                b3BoxHull box = b3MakeBoxHull(halfExtents.x, halfExtents.y, halfExtents.z);
                b3CreateHullShape(bodyId, &shapeDef, &box.base);
            }
            break;
        }

        case pcoll::CollisionAccuracy::Hull:
        case pcoll::CollisionAccuracy::Default: {
            if (obj->GetShapeType() == ShapeType::Sphere) {
                b3Sphere sphere{};
                sphere.radius = (size.x + size.y + size.z) / 6.0f;
                b3CreateSphereShape(bodyId, &shapeDef, &sphere);
            } else if (obj->GetShapeType() == ShapeType::Cylinder) {
                b3HullData* cyl = b3CreateCylinder(size.y, (size.x + size.z) / 4.0f, 0.0f, 16);
                if (cyl) {
                    b3CreateHullShape(bodyId, &shapeDef, cyl);
                    hulls.push_back(cyl);
                }
            } else {
                std::vector<Vector3> rawVerts;
                ExtractMeshVerts(obj, rawVerts);
                std::vector<b3Vec3> verts = DedupeVertsToB3(rawVerts);
                if (verts.size() >= 4) {
                    for (auto& v : verts) { v.x *= size.x; v.y *= size.y; v.z *= size.z; }
                    b3HullData* hull = b3CreateHull(verts.data(), (int)verts.size(), 64);
                    if (hull) {
                        b3CreateHullShape(bodyId, &shapeDef, hull);
                        hulls.push_back(hull);
                    }
                } else {
                    b3BoxHull box = b3MakeBoxHull(halfExtents.x, halfExtents.y, halfExtents.z);
                    b3CreateHullShape(bodyId, &shapeDef, &box.base);
                }
            }
            break;
        }

        case pcoll::CollisionAccuracy::Precise: {
            if (obj->GetShapeType() == ShapeType::Sphere) {
                b3Sphere sphere{};
                sphere.radius = (size.x + size.y + size.z) / 6.0f;
                b3CreateSphereShape(bodyId, &shapeDef, &sphere);
            } else if (obj->GetShapeType() == ShapeType::Cylinder) {
                b3HullData* cyl = b3CreateCylinder(size.y, (size.x + size.z) / 4.0f, 0.0f, 24);
                if (cyl) {
                    b3CreateHullShape(bodyId, &shapeDef, cyl);
                    hulls.push_back(cyl);
                }
            } else {
                // Triangle-mesh shapes only generate contacts on static bodies in
                // Box3D, so a precise mesh collider would silently stop a dynamic
                // body (it falls straight through everything). Keep precise
                // triangle-mesh collision for anchored/static objects, and fall
                // back to a convex hull for dynamic ones so their physics still
                // works.
                if (!obj->anchored) {
                    std::vector<Vector3> rawVerts;
                    ExtractMeshVerts(obj, rawVerts);
                    std::vector<b3Vec3> verts = DedupeVertsToB3(rawVerts);
                    if (verts.size() >= 4) {
                        for (auto& v : verts) { v.x *= size.x; v.y *= size.y; v.z *= size.z; }
                        b3HullData* hull = b3CreateHull(verts.data(), (int)verts.size(), 64);
                        if (hull) {
                            b3CreateHullShape(bodyId, &shapeDef, hull);
                            hulls.push_back(hull);
                            break;
                        }
                    }
                    b3BoxHull box = b3MakeBoxHull(halfExtents.x, halfExtents.y, halfExtents.z);
                    b3CreateHullShape(bodyId, &shapeDef, &box.base);
                    break;
                }

                std::vector<Vector3> rawVerts;
                ExtractMeshVerts(obj, rawVerts);
                if (!rawVerts.empty()) {
                    IndexedTriMesh imesh = BuildIndexedTriMesh(rawVerts);
                    for (auto& v : imesh.vertices) { v.x *= size.x; v.y *= size.y; v.z *= size.z; }
                    b3MeshDef md{};
                    md.vertices = imesh.vertices.data();
                    md.indices = imesh.indices.data();
                    md.vertexCount = (int)imesh.vertices.size();
                    md.triangleCount = (int)imesh.indices.size() / 3;
                    md.weldTolerance = 0.001f;
                    md.weldVertices = true;
                    md.identifyEdges = true;
                    b3MeshData* mesh = b3CreateMesh(&md, nullptr, 0);
                    if (mesh) {
                        b3CreateMeshShape(bodyId, &shapeDef, mesh, b3Vec3_one);
                        meshes.push_back(mesh);
                    }
                } else {
                    b3BoxHull box = b3MakeBoxHull(halfExtents.x, halfExtents.y, halfExtents.z);
                    b3CreateHullShape(bodyId, &shapeDef, &box.base);
                }
            }
            break;
        }
    }
}

void Simulation::SpawnBodyForObject(ScatteredObject* obj) {
    if (!obj || !playing || !world) return;
    if (bodyMap.find(obj) != bodyMap.end()) return;

    Vector3 pos = *obj->GetPosPtr();
    Vector3 rot = *obj->GetRotationPtr();
    Quaternion ori = QuaternionFromMatrix(
        MatrixRotateXYZ({ DEG2RAD * rot.x, DEG2RAD * rot.y, DEG2RAD * rot.z }));

    bool anchored = obj->anchored;
    b3BodyType type = anchored ? b3_staticBody : b3_dynamicBody;

    b3BodyId bodyId = b3wrap::CreateBody(world->GetId(), pos, ori, type);
    b3Body_SetName(bodyId, obj->GetName().c_str());

    CreateShapeForObject(obj, bodyId);

    BodyRecord rec;
    rec.bodyId = bodyId;
    rec.startPos = pos;
    rec.startLinVel = Vector3Zero();
    rec.startAngVel = Vector3Zero();
    rec.wasAnchored = anchored;
    bodyMap[obj] = rec;
}

void Simulation::StopPlay() {
    for (auto& [obj, rec] : bodyMap) {
        if (!obj || !b3Body_IsValid(rec.bodyId)) continue;
        obj->ClearRenderRotation();
        *obj->GetPosPtr() = rec.startPos;
        obj->SetVelocity(rec.startLinVel);
        obj->SetAngularVelocity(rec.startAngVel);
    }

    for (auto j : joints) {
        if (b3Joint_IsValid(j)) b3DestroyJoint(j, false);
    }
    joints.clear();
    bodyMap.clear();
    bodyToObject.clear();
    for (auto* h : hulls) b3DestroyHull(h);
    hulls.clear();
    for (auto* m : meshes) b3DestroyMesh(m);
    meshes.clear();
    contactBeginEvents.clear();
    contactHitEvents.clear();
    world.reset();

    ui::LogAlways("Playtest Session ended, %.2f s", GetTime() - playStartTime);
}

void Simulation::ApplyBuoyancy() {
    const float GRAVITY = 9.81f;
    const float WATER_DENSITY = 1000.0f;
    const float OBJECT_DENSITY = 500.0f;
    const float DRAG_COEFF = 1.5f;
    const float WAVE_PUSH = 4.0f;
    const float TORQUE_STRENGTH = 5.0f;
    const float SAMPLE_EPS = 0.1f;

    auto& waterBodies = WaterBody::GetInstances();
    if (waterBodies.empty()) return;

    for (auto& [obj, rec] : bodyMap) {
        if (!obj || rec.wasAnchored) continue;
        if (!b3Body_IsValid(rec.bodyId)) continue;
        if (b3Body_GetType(rec.bodyId) != b3_dynamicBody) continue;

        Vector3 pos = b3wrap::GetBodyPosition(rec.bodyId);
        Vector3 vel = b3wrap::GetBodyLinearVelocity(rec.bodyId);
        Vector3 halfExt = Vector3Scale(*obj->GetSizePtr(), 0.5f);

        for (WaterBody* water : waterBodies) {
            if (!water->IntersectsXZ({ pos.x - halfExt.x, pos.y - halfExt.y, pos.z - halfExt.z,
                                       pos.x + halfExt.x, pos.y + halfExt.y, pos.z + halfExt.z })) continue;

            float waterSurface = water->GetHeightAt(pos.x, pos.z);
            float bottomY = pos.y - halfExt.y;
            float objectHeight = halfExt.y * 2.0f;
            float submergedFraction = Clamp((waterSurface - bottomY) / objectHeight, 0.0f, 1.0f);

            if (submergedFraction <= 0.0f) {
                rec.wasSubmerged = false;
                continue;
            }

            float mass = fmaxf(b3Body_GetMass(rec.bodyId), 0.001f);

            // Archimedes buoyancy
            float densityRatio = WATER_DENSITY / OBJECT_DENSITY;
            float buoyancyAccel = GRAVITY * densityRatio * submergedFraction;
            b3Body_ApplyForceToCenter(rec.bodyId, { 0.0f, mass * buoyancyAccel, 0.0f }, true);

            // Water surface gradient — drives horizontal wave push
            float hL = water->GetHeightAt(pos.x - SAMPLE_EPS, pos.z);
            float hR = water->GetHeightAt(pos.x + SAMPLE_EPS, pos.z);
            float hD = water->GetHeightAt(pos.x, pos.z - SAMPLE_EPS);
            float hU = water->GetHeightAt(pos.x, pos.z + SAMPLE_EPS);

            float gradX = (hR - hL) / (2.0f * SAMPLE_EPS);
            float gradZ = (hU - hD) / (2.0f * SAMPLE_EPS);

            // Push object downhill along the wave slope
            float pushForce = WAVE_PUSH * submergedFraction * mass;
            b3Body_ApplyForceToCenter(rec.bodyId, { -gradX * pushForce, 0.0f, -gradZ * pushForce }, true);

            // Quadratic drag: F = C * submerged * mass * |v|² (realistic fluid resistance)
            float speed = Vector3Length(vel);
            if (speed > 0.001f) {
                float dragForce = DRAG_COEFF * submergedFraction * mass * speed;
                Vector3 drag = Vector3Scale(vel, -dragForce / speed);
                b3Body_ApplyForceToCenter(rec.bodyId, { drag.x, drag.y, drag.z }, true);
            }

            // Rotation: align body up with water surface normal
            Vector3 waterNormal = Vector3Normalize({ -gradX, 1.0f, -gradZ });

            Quaternion ori = b3wrap::GetBodyRotation(rec.bodyId);
            Vector3 bodyUp = Vector3RotateByQuaternion({ 0.0f, 1.0f, 0.0f }, ori);

            Vector3 cross = Vector3CrossProduct(bodyUp, waterNormal);
            float dot = Vector3DotProduct(bodyUp, waterNormal);
            float torqueMag = TORQUE_STRENGTH * submergedFraction * fmaxf(0.0f, 1.0f - dot);
            Vector3 torque = Vector3Scale(cross, torqueMag);
            b3Body_ApplyTorque(rec.bodyId, { torque.x, torque.y, torque.z }, true);

            // Angular damping
            Vector3 angVel = b3wrap::GetBodyAngularVelocity(rec.bodyId);
            float angSpeed = Vector3Length(angVel);
            if (angSpeed > 0.01f) {
                float angDrag = 1.5f * submergedFraction * angSpeed;
                Vector3 angDragVec = Vector3Scale(angVel, -angDrag / angSpeed);
                b3Body_ApplyTorque(rec.bodyId, { angDragVec.x, angDragVec.y, angDragVec.z }, true);
            }

            rec.wasSubmerged = true;
            break;
        }
    }
}

void Simulation::WriteBack() {
    for (auto& [obj, rec] : bodyMap) {
        if (!obj || !b3Body_IsValid(rec.bodyId)) continue;

        bool anchored = obj->anchored;
        b3BodyType targetType = anchored ? b3_staticBody : b3_dynamicBody;
        b3BodyType currentType = b3Body_GetType(rec.bodyId);
        if (currentType != targetType) {
            b3Body_SetType(rec.bodyId, targetType);
            if (!anchored && rec.wasAnchored) b3wrap::SetBodyAwake(rec.bodyId, true);
        }
        rec.wasAnchored = anchored;

        Vector3 pos = b3wrap::GetBodyPosition(rec.bodyId);
        Quaternion ori = b3wrap::GetBodyRotation(rec.bodyId);

        *obj->GetPosPtr() = pos;
        obj->SetRenderRotation(QuaternionToMatrix(ori));
        obj->SetVelocity(b3wrap::GetBodyLinearVelocity(rec.bodyId));
        obj->SetAngularVelocity(b3wrap::GetBodyAngularVelocity(rec.bodyId));
    }
}

b3JointId Simulation::CreateRevoluteJoint(ScatteredObject* a, ScatteredObject* b, Vector3 anchor) {
    if (!world || !a || !b) return b3JointId{};
    auto itA = bodyMap.find(a);
    auto itB = bodyMap.find(b);
    if (itA == bodyMap.end() || itB == bodyMap.end()) return b3JointId{};
    b3JointId j = b3wrap::CreateRevoluteJoint(world->GetId(), itA->second.bodyId, itB->second.bodyId, anchor);
    if (b3Joint_IsValid(j)) joints.push_back(j);
    return j;
}

b3JointId Simulation::CreateDistanceJoint(ScatteredObject* a, ScatteredObject* b, Vector3 anchorA, Vector3 anchorB) {
    if (!world || !a || !b) return b3JointId{};
    auto itA = bodyMap.find(a);
    auto itB = bodyMap.find(b);
    if (itA == bodyMap.end() || itB == bodyMap.end()) return b3JointId{};
    b3JointId j = b3wrap::CreateDistanceJoint(world->GetId(), itA->second.bodyId, itB->second.bodyId, anchorA, anchorB);
    if (b3Joint_IsValid(j)) joints.push_back(j);
    return j;
}

b3JointId Simulation::CreateWeldJoint(ScatteredObject* a, ScatteredObject* b, Vector3 anchor) {
    if (!world || !a || !b) return b3JointId{};
    auto itA = bodyMap.find(a);
    auto itB = bodyMap.find(b);
    if (itA == bodyMap.end() || itB == bodyMap.end()) return b3JointId{};
    b3JointId j = b3wrap::CreateWeldJoint(world->GetId(), itA->second.bodyId, itB->second.bodyId, anchor);
    if (b3Joint_IsValid(j)) joints.push_back(j);
    return j;
}

b3JointId Simulation::CreateSphericalJoint(ScatteredObject* a, ScatteredObject* b, Vector3 anchor) {
    if (!world || !a || !b) return b3JointId{};
    auto itA = bodyMap.find(a);
    auto itB = bodyMap.find(b);
    if (itA == bodyMap.end() || itB == bodyMap.end()) return b3JointId{};
    b3JointId j = b3wrap::CreateSphericalJoint(world->GetId(), itA->second.bodyId, itB->second.bodyId, anchor);
    if (b3Joint_IsValid(j)) joints.push_back(j);
    return j;
}

void Simulation::DestroyJoint(b3JointId jointId) {
    if (!b3Joint_IsValid(jointId)) return;
    b3DestroyJoint(jointId, true);
    for (auto it = joints.begin(); it != joints.end(); ) {
        if (!b3Joint_IsValid(*it)) it = joints.erase(it);
        else ++it;
    }
}

RaycastHit Simulation::RayCast(Vector3 origin, Vector3 end) {
    RaycastHit result;
    if (!world || !world->IsValid()) return result;
    b3wrap::RaycastResult r = b3wrap::RayCastClosest(world->GetId(), origin, end);
    result.hit = r.hit;
    result.point = r.point;
    result.normal = r.normal;
    result.fraction = r.fraction;
    if (r.hit && b3Shape_IsValid(r.shapeId)) {
        b3BodyId bodyId = b3Shape_GetBody(r.shapeId);
        result.object = FindObjectByBody(bodyToObject, bodyId);
    }
    return result;
}

void Simulation::ProcessEvents() {
    if (!world || !world->IsValid()) return;

    b3ContactEvents ce = world->GetContactEvents();

    for (int i = 0; i < ce.beginCount; ++i) {
        b3ContactBeginTouchEvent& e = ce.beginEvents[i];
        ContactEvent ev;
        ev.objectA = FindObjectByBody(bodyToObject, b3Shape_GetBody(e.shapeIdA));
        ev.objectB = FindObjectByBody(bodyToObject, b3Shape_GetBody(e.shapeIdB));
        contactBeginEvents.push_back(ev);
    }

    for (int i = 0; i < ce.hitCount; ++i) {
        b3ContactHitEvent& e = ce.hitEvents[i];
        ContactEvent ev;
        ev.objectA = FindObjectByBody(bodyToObject, b3Shape_GetBody(e.shapeIdA));
        ev.objectB = FindObjectByBody(bodyToObject, b3Shape_GetBody(e.shapeIdB));
        ev.point = b3wrap::ToRL(e.point);
        ev.normal = b3wrap::ToRL(e.normal);
        ev.approachSpeed = e.approachSpeed;
        contactHitEvents.push_back(ev);
    }
}

void Simulation::DrawDebug() {
    if (!world || !world->IsValid()) return;

    for (auto& [obj, rec] : bodyMap) {
        if (!obj || !b3Body_IsValid(rec.bodyId)) continue;
        if (b3Body_GetType(rec.bodyId) == b3_staticBody) continue;

        Vector3 pos = b3wrap::GetBodyPosition(rec.bodyId);
        Quaternion ori = b3wrap::GetBodyRotation(rec.bodyId);
        Vector3 size = *obj->GetSizePtr();
        Vector3 halfExtents = Vector3Scale(size, 0.5f);

        Color wireColor = { 0, 200, 255, 180 };
        BoundingBox bb;
        bb.min = Vector3Subtract(pos, halfExtents);
        bb.max = Vector3Add(pos, halfExtents);
        DrawBoundingBox(bb, wireColor);

        Vector3 forward = Vector3RotateByQuaternion(Vector3{ 0, 0, 1 }, ori);
        Vector3 up = Vector3RotateByQuaternion(Vector3{ 0, 1, 0 }, ori);
        DrawLine3D(pos, Vector3Add(pos, Vector3Scale(forward, halfExtents.z * 1.5f)), BLUE);
        DrawLine3D(pos, Vector3Add(pos, Vector3Scale(up, halfExtents.y * 1.5f)), GREEN);
    }

    for (auto j : joints) {
        if (!b3Joint_IsValid(j)) continue;
        b3BodyId bA = b3Joint_GetBodyA(j);
        b3BodyId bB = b3Joint_GetBodyB(j);
        Vector3 pA = b3wrap::GetBodyPosition(bA);
        Vector3 pB = b3wrap::GetBodyPosition(bB);
        DrawLine3D(pA, pB, YELLOW);
    }

    for (auto& ev : contactHitEvents) {
        DrawSphere(ev.point, 0.02f, RED);
        DrawLine3D(ev.point, Vector3Add(ev.point, Vector3Scale(ev.normal, 0.1f)), ORANGE);
    }
}

} // namespace phys