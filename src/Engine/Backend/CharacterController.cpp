// CharacterController.cpp - Player-controlled character with kinematic/dynamic modes.

#include "../../../include/Engine/Backend/CharacterController.hpp"
#include "../../../include/Engine.hpp"
#include "../../../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine/TechnicalTools.hpp"
#include "../../../include/Terrain/BasicTerrain.hpp"
#include "raylib.h"
#include "raymath.h"
#include <algorithm>
#include <iostream>

namespace {
    constexpr float CAPSULE_RADIUS = 0.4f;
    constexpr float CAPSULE_HEIGHT = 1.8f;
    constexpr float CAMERA_HEIGHT = 1.6f;   // eye height above the FEET
    // The body position is the capsule's centre, so the eye sits this far above it (not CAMERA_HEIGHT:
    // that put the eyes 2.5 m up, far above a 1.72 m pedestrian).
    constexpr float CAMERA_OFFSET = CAMERA_HEIGHT - CAPSULE_HEIGHT * 0.5f;
    constexpr float GROUND_CHECK_DIST = 0.1f;
    constexpr float CAPSULE_HALF_HEIGHT = CAPSULE_HEIGHT * 0.5f;
    // Steepest slope the player can stand and walk on (50 degrees); anything steeper is slid down.
    // This is cos(50 deg), the smallest upward component of the surface normal that still counts as ground.
    constexpr float WALKABLE_NORMAL_Y = 0.6428f;
    // Tallest ledge the player steps onto without jumping, and the longest drop it stays glued to.
    constexpr float STEP_HEIGHT = 0.4f;
    // How far forward a step-up carries the player in the one frame it happens (see UpdateKinematic).
    constexpr float STEP_FORWARD = CAPSULE_RADIUS * 0.75f;

    // Where the player starts: above the origin, lifted clear of any terrain that covers it so a hill
    // at the origin does not swallow the capsule (a capsule that starts inside the ground has no surface
    // to be pushed out of).
    Vector3 SpawnPosition() {
        float y = 3.0f;
        for (const BasicTerrain* t : BasicTerrain::GetInstances()) {
            if (!t) continue;
            const float hx = t->GetWidth() * t->GetScale() * 0.5f;
            const float hz = t->GetDepth() * t->GetScale() * 0.5f;
            if (fabsf(t->position.x) > hx || fabsf(t->position.z) > hz) continue;
            y = std::max(y, t->position.y + t->GetHeightAt(0.0f, 0.0f) + CAPSULE_HALF_HEIGHT + 0.5f);
        }
        return { 0.0f, y, 0.0f };
    }

    // Report a ground-check raycast to the physics debug overlay (F2). The
    // ground check is a one-frame boolean that decides whether the player can
    // jump, so when it is wrong the symptom is "the player cannot jump" with
    // nothing on screen to explain it. The overlay's raycast layer is the only
    // place the probe becomes visible.
    void ReportGroundCheck(const phys::RaycastHit& hit, const Vector3& origin, float maxDist) {
        TechTools::PhysicsDebugVisualizer::DebugRaycast rc;
        rc.origin = origin;
        rc.direction = Vector3{ 0.0f, -1.0f, 0.0f };
        rc.maxDist = maxDist;
        rc.hit = hit.hit;
        rc.hitPoint = hit.point;
        rc.hitNormal = hit.normal;
        // Green when it found ground, amber when the probe fell through: the
        // two failure modes look identical from the outside otherwise.
        rc.color = hit.hit ? GREEN : Color{ 255, 160, 0, 255 };
        TechTools::PhysicsDebugVisualizer::Instance().AddDebugRaycast(rc);
    }
}

CharacterController::CharacterController(Engine& engine, Camera3D* camera, phys::Simulation& sim,
                                         CharacterControllerMode mode)
    : engine(engine), camera(camera), sim(sim), mode(mode) {
    // Default yaw/pitch (looking along -Z)
    yaw = 0.0f;
    pitch = 0.0f;
    CreatePhysicsBody();
    
    // Position body at reasonable spawn height (above ground)
    if (body) {
        Vector3 spawnPos = {0, 5, 0};  // Spawn 5 units up
        *body->GetPosPtr() = spawnPos;
        camera->position = {spawnPos.x, spawnPos.y + CAMERA_OFFSET, spawnPos.z};
    }
    
    // Capture mouse for first-person control (sokol_app's mouse lock already
    // delivers raw, unaccelerated motion).
    DisableCursor();
}

CharacterController::~CharacterController() {
    DestroyPhysicsBody();
}

void CharacterController::CreatePhysicsBody() {
    // Create a capsule body for collision at spawn position
    // Spawn at y=3 - capsule center, bottom at y=2.1, ground at y=0
    // Raycast distance 1.0 from center will detect ground when center reaches y=1.0
    Vector3 spawnPos = {0, 3, 0};
    body = new ScatteredObject(spawnPos, {CAPSULE_RADIUS * 2, CAPSULE_HEIGHT, CAPSULE_RADIUS * 2}, WHITE, ShapeType::Cylinder);
    body->SetName("PlayerCharacter");
    body->SetCollisionAccuracy(pcoll::CollisionAccuracy::Box);
    body->SetMass(1.0f);
    // Don't spawn yet - wait for physics world to exist
    
    // Sync gravity from physics simulation
    gravity = sim.GetGravity();
    
    // Add to engine so it renders
    engine.AddEntity(std::unique_ptr<ScatteredObject>(body));
}

// Call AFTER simulation.StartPlay() to ensure body exists in physics world
void CharacterController::EnsurePhysicsBody() {
    if (body) {
        sim.SpawnBodyForObject(body, b3_kinematicBody);
        // Position at spawn height
        Vector3 spawnPos = SpawnPosition();
        sim.SetBodyPosition(body, spawnPos);
        *body->GetPosPtr() = spawnPos;
        camera->position = {spawnPos.x, spawnPos.y + CAMERA_OFFSET, spawnPos.z};
        
        // Sync gravity from physics simulation (in case it was changed)
        gravity = sim.GetGravity();
    }
}

void CharacterController::DestroyPhysicsBody() {
    if (body) {
        sim.RemoveObject(body);
        // Body is owned by engine now, don't delete
        body = nullptr;
    }
}

void CharacterController::SetMoveInput(Vector2 input) {
    moveInput = input;
}

void CharacterController::SetLookInput(Vector2 delta) {
    lookInput = delta;
}

void CharacterController::Jump() {
    if (grounded) wantJump = true;
}

void CharacterController::SetSprint(bool s) {
    sprint = s;
}

void CharacterController::SetCrouch(bool c) {
    crouch = c;
}

void CharacterController::SetMode(CharacterControllerMode m) {
    mode = m;
}

bool CharacterController::IsGrounded() const {
    return grounded;
}

Vector3 CharacterController::GetVelocity() const {
    if (!body) return {0, 0, 0};
    return sim.GetBodyVelocity(body);
}

Vector3 CharacterController::GetPosition() const {
    if (!body) return camera->position;
    Vector3 pos = *body->GetPosPtr();
    pos.y += CAMERA_OFFSET;
    return pos;
}

Vector3 CharacterController::GetForward() const {
    return Vector3Normalize({camera->target.x - camera->position.x, 0, camera->target.z - camera->position.z});
}

Vector3 CharacterController::GetRight() const {
    Vector3 f = GetForward();
    // Cross product: up × forward = right (in right-handed coords)
    // up = (0,1,0), forward = f
    // right = up × forward = (f.z, 0, -f.x) is WRONG
    // right = forward × up = (-f.z, 0, f.x) for right-handed coords
    return Vector3Normalize({-f.z, 0, f.x});
}

void CharacterController::Update(float dt) {
    ProcessInput();
    
    if (mode == CharacterControllerMode::Kinematic) {
        UpdateKinematic(dt);
    } else {
        UpdateDynamic(dt);
    }
    
    // Sync camera position to body FIRST
    if (body) {
        Vector3 pos = *body->GetPosPtr();
        camera->position = {pos.x, pos.y + CAMERA_OFFSET, pos.z};
    }
    
    // THEN update camera target from new position
    UpdateCamera();
}

void CharacterController::ProcessInput() {
    // Keyboard input (only if no script has set input)
    if (moveInput.x == 0 && moveInput.y == 0) {
        Vector2 input = {0, 0};
        if (IsKeyDown(KEY_W)) input.y -= 1;
        if (IsKeyDown(KEY_S)) input.y += 1;
        if (IsKeyDown(KEY_A)) input.x -= 1;
        if (IsKeyDown(KEY_D)) input.x += 1;
        moveInput = input;
    }

    // Mouse look - GetMouseDelta gives relative movement (works best with disabled cursor)
    if (lookInput.x == 0 && lookInput.y == 0) {
        Vector2 delta = GetMouseDelta();
        lookInput = delta;
    }

    // Don't re-center - use relative movement directly with disabled cursor
    // This avoids fighting with the OS cursor position

    if (IsKeyPressed(KEY_SPACE)) wantJump = true;
    sprint = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    crouch = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}

void CharacterController::UpdateCamera() {
    // Lower sensitivity: pixels -> radians (0.001f = ~1° per 50px)
    yaw -= lookInput.x * 0.001f;
    pitch -= lookInput.y * 0.001f;
    pitch = std::clamp(pitch, cameraPitchMin * DEG2RAD, cameraPitchMax * DEG2RAD);

    Vector3 forward = {
        cosf(pitch) * sinf(yaw),
        sinf(pitch),
        cosf(pitch) * cosf(yaw)
    };
    // Ensure forward is normalized
    forward = Vector3Normalize(forward);
    camera->target = Vector3Add(camera->position, forward);

    // Reset input for next frame
    moveInput = {0, 0};
    lookInput = {0, 0};
}

void CharacterController::UpdateKinematic(float dt) {
    if (!body) return;

    const Vector3 pos = *body->GetPosPtr();
    const float halfSegment = CAPSULE_HALF_HEIGHT - CAPSULE_RADIUS;   // straight part of the capsule, each side of the centre

    // Ground check: is something walkable touching the bottom of the capsule? Rising means we just
    // jumped: the surface is still within reach for the first frames of the jump, and counting that
    // as ground would cancel the jump velocity (a tiny hop, then stuck).
    Vector3 groundNormal = { 0.0f, 1.0f, 0.0f };
    const bool wasGrounded = grounded;
    grounded = verticalVelocity <= 0.0f &&
               sim.GetCapsuleGround(body, pos, CAPSULE_RADIUS, halfSegment, WALKABLE_NORMAL_Y, groundNormal);
    {
        phys::RaycastHit probe;
        probe.hit = grounded;
        probe.point = { pos.x, pos.y - CAPSULE_HALF_HEIGHT, pos.z };
        probe.normal = groundNormal;
        ReportGroundCheck(probe, pos, CAPSULE_HALF_HEIGHT + GROUND_CHECK_DIST);
    }

    // Coyote time: allow jump for a short time after leaving ground
    const float COYOTE_TIME = 0.1f;
    if (wasGrounded && !grounded) {
        coyoteTimer = COYOTE_TIME;  // Start coyote timer when leaving ground
    } else if (!grounded) {
        coyoteTimer = std::max(0.0f, coyoteTimer - dt);
    } else {
        coyoteTimer = 0.0f;  // Reset when grounded
    }

    // Jump buffering: queue jump input for a short time before landing
    const float JUMP_BUFFER_TIME = 0.1f;
    if (wantJump) {
        jumpBufferTimer = JUMP_BUFFER_TIME;
    } else {
        jumpBufferTimer = std::max(0.0f, jumpBufferTimer - dt);
    }

    // Calculate move direction
    Vector3 forward = GetForward();
    Vector3 right = GetRight();
    Vector3 moveDir = Vector3Scale(forward, -moveInput.y);
    moveDir = Vector3Add(moveDir, Vector3Scale(right, moveInput.x));
    if (Vector3Length(moveDir) > 0) moveDir = Vector3Normalize(moveDir);

    float speed = sprint ? sprintSpeed : walkSpeed;
    if (crouch) speed *= 0.5f;

    Vector3 vel = { moveDir.x * speed, 0.0f, moveDir.z * speed };

    // Vertical velocity - handle jump BEFORE applying gravity.
    // verticalVelocity is a class member, so it carries over frame to frame
    // instead of being recomputed from 0 every Update() call.
    bool canJump = grounded || coyoteTimer > 0.0f;
    bool shouldJump = canJump && jumpBufferTimer > 0.0f;

    if (shouldJump) {
        verticalVelocity = jumpForce;
        wantJump = false;
        jumpBufferTimer = 0.0f;
        coyoteTimer = 0.0f;
        grounded = false;  // Immediately unground to prevent re-grounding this frame
    } else if (grounded) {
        verticalVelocity = 0.0f;
    } else {
        // Accumulate gravity onto the existing vertical velocity (real acceleration)
        verticalVelocity += gravity * dt;
        // Air control
        vel.x += moveDir.x * speed * airControl * dt;
        vel.z += moveDir.z * speed * airControl * dt;
    }
    vel.y = verticalVelocity;

    // Stop tiny velocities to prevent jitter when stopping
    const float VEL_THRESHOLD = 0.01f;
    if (fabsf(vel.x) < VEL_THRESHOLD) vel.x = 0;
    if (fabsf(vel.z) < VEL_THRESHOLD) vel.z = 0;

    // Walking: follow the ground. Tilt the velocity into the surface plane at the same horizontal
    // speed, so going uphill climbs and going downhill stays planted instead of skipping off it.
    if (grounded) {
        vel.y = -(groundNormal.x * vel.x + groundNormal.z * vel.z) / groundNormal.y;
    }

    // The capsule slides: it stops against walls, runs along slopes and terrain, and is pushed out of
    // anything it overlaps. The body itself is teleported to the result below. Slopes steeper than
    // WALKABLE_NORMAL_Y count as walls for the walking part of the move (otherwise walking into one
    // would carry the player up it); airborne, that part is separate from the fall so a steep face
    // blocks the walk but still lets gravity slide the player down it.
    const Vector3 delta = Vector3Scale(vel, dt);
    Vector3 newPos;
    Vector3 movedTotal;
    float movedVertical;   // the fall/jump part of the move, which is what vertical velocity must match
    if (grounded) {
        const auto m = sim.MoveCapsule(body, pos, delta, CAPSULE_RADIUS, halfSegment, WALKABLE_NORMAL_Y);
        newPos = m.position;
        movedTotal = m.moved;
        movedVertical = m.moved.y;
    } else {
        const auto walk = sim.MoveCapsule(body, pos, { delta.x, 0.0f, delta.z }, CAPSULE_RADIUS, halfSegment, WALKABLE_NORMAL_Y);
        const auto fall = sim.MoveCapsule(body, walk.position, { 0.0f, delta.y, 0.0f }, CAPSULE_RADIUS, halfSegment);
        newPos = fall.position;
        movedTotal = Vector3Add(walk.moved, fall.moved);
        movedVertical = fall.moved.y;
    }

    const float wantedH = sqrtf(delta.x * delta.x + delta.z * delta.z);
    const float gotH = sqrtf(movedTotal.x * movedTotal.x + movedTotal.z * movedTotal.z);

    // Step up: walking into a ledge lower than STEP_HEIGHT (a curb, a stair, a lip of terrain) lifts the
    // capsule over it instead of stopping dead.
    if (grounded && wantedH > 1e-5f && gotH < wantedH * 0.5f) {
        const auto lifted = sim.MoveCapsule(body, pos, { 0.0f, STEP_HEIGHT, 0.0f }, CAPSULE_RADIUS, halfSegment);
        // Go forward at least STEP_FORWARD: a round capsule bottom perched on the ledge's edge sits on a
        // surface too steep to stand on, so a single frame's stride would often fall back off it.
        const float stride = std::max(wantedH, STEP_FORWARD) / wantedH;
        const auto across = sim.MoveCapsule(body, lifted.position, { delta.x * stride, 0.0f, delta.z * stride }, CAPSULE_RADIUS, halfSegment, WALKABLE_NORMAL_Y);
        const float acrossH = sqrtf(across.moved.x * across.moved.x + across.moved.z * across.moved.z);
        if (acrossH > gotH + 0.01f) {
            const float lift = lifted.position.y - pos.y;
            const float reach = lift + GROUND_CHECK_DIST;
            const float fraction = sim.CastCapsule(body, across.position, { 0.0f, -reach, 0.0f }, CAPSULE_RADIUS, halfSegment);
            if (fraction < 1.0f) {
                Vector3 landed = across.position;
                landed.y -= reach * fraction;
                Vector3 landedNormal;
                if (landed.y > pos.y + 0.01f &&
                    sim.GetCapsuleGround(body, landed, CAPSULE_RADIUS, halfSegment, WALKABLE_NORMAL_Y, landedNormal)) {
                    newPos = landed;
                }
            }
        }
    }

    // Stay on the ground over bumps and down slopes: if we were standing and are not rising, drop to
    // the surface below (up to STEP_HEIGHT) rather than going airborne for a frame.
    if (grounded && verticalVelocity <= 0.0f) {
        const float fraction = sim.CastCapsule(body, newPos, { 0.0f, -STEP_HEIGHT, 0.0f }, CAPSULE_RADIUS, halfSegment);
        if (fraction < 1.0f) {
            Vector3 dropped = newPos;
            dropped.y -= STEP_HEIGHT * fraction;
            Vector3 droppedNormal;
            if (sim.GetCapsuleGround(body, dropped, CAPSULE_RADIUS, halfSegment, WALKABLE_NORMAL_Y, droppedNormal)) {
                newPos = dropped;
            }
        }
    }

    // Blocked vertically (landed, bumped a ceiling, resting on a slope too steep to stand on): the
    // velocity cannot exceed what the capsule actually travelled.
    if (!grounded && fabsf(movedVertical - delta.y) > 1e-4f) {
        const float actual = movedVertical / dt;
        verticalVelocity = verticalVelocity < 0.0f ? std::clamp(actual, verticalVelocity, 0.0f)
                                                   : std::clamp(actual, 0.0f, verticalVelocity);
    }

    // Update physics body position directly
    sim.SetBodyPosition(body, newPos);

    // Also update the ScatteredObject position for camera sync
    *body->GetPosPtr() = newPos;
}

void CharacterController::UpdateDynamic(float dt) {
    if (!body) return;

    // Dynamic mode: apply forces to physics body
    // Ground check via raycast from capsule center with proper distance
    // Capsule half-height = 0.9. Raycast distance = half-height + tiny epsilon (0.01).
    const float CAPSULE_HALF_HEIGHT = CAPSULE_HEIGHT * 0.5f;
    const float GROUND_CHECK_EPSILON = 0.01f;
    Vector3 rayOrigin = *body->GetPosPtr();
    float groundCheckDist = CAPSULE_HALF_HEIGHT + GROUND_CHECK_EPSILON;
    const phys::RaycastHit groundHit = sim.RayCast(rayOrigin, {0, -1, 0}, groundCheckDist, body);
    grounded = groundHit.hit;
    ReportGroundCheck(groundHit, rayOrigin, groundCheckDist);

    Vector3 forward = GetForward();
    Vector3 right = GetRight();
    Vector3 moveDir = Vector3Scale(forward, -moveInput.y);
    moveDir = Vector3Add(moveDir, Vector3Scale(right, moveInput.x));
    if (Vector3Length(moveDir) > 0) moveDir = Vector3Normalize(moveDir);

    float speed = sprint ? sprintSpeed : walkSpeed;
    if (crouch) speed *= 0.5f;

    Vector3 targetVel = {moveDir.x * speed, 0, moveDir.z * speed};
    Vector3 currentVel = sim.GetBodyVelocity(body);
    currentVel.y = 0; // ignore vertical for horizontal control

    Vector3 velDiff = Vector3Subtract(targetVel, currentVel);
    float force = 50.0f; // tune as needed
    sim.ApplyForceToBody(body, Vector3Scale(velDiff, force));

    if (grounded && wantJump) {
        sim.ApplyImpulseToBody(body, {0, jumpForce * body->GetMass(), 0});
        wantJump = false;
        grounded = false;
    }
}