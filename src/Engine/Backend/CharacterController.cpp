// CharacterController.cpp - Player-controlled character with kinematic/dynamic modes.

#include "../../../include/Engine/Backend/CharacterController.hpp"
#include "../../../include/Engine.hpp"
#include "../../../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine/TechnicalTools.hpp"
#include "raylib.h"
#include "raymath.h"
#include <algorithm>
#include <iostream>

namespace {
    constexpr float CAPSULE_RADIUS = 0.4f;
    constexpr float CAPSULE_HEIGHT = 1.8f;
    constexpr float CAMERA_HEIGHT = 1.6f;
    constexpr float GROUND_CHECK_DIST = 0.1f;

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
        camera->position = {spawnPos.x, spawnPos.y + CAMERA_HEIGHT, spawnPos.z};
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
        Vector3 spawnPos = {0, 3, 0};
        sim.SetBodyPosition(body, spawnPos);
        *body->GetPosPtr() = spawnPos;
        camera->position = {spawnPos.x, spawnPos.y + CAMERA_HEIGHT, spawnPos.z};
        
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
    pos.y += CAMERA_HEIGHT;
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
        camera->position = {pos.x, pos.y + CAMERA_HEIGHT, pos.z};
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

    Vector3 pos = *body->GetPosPtr();
    Vector3 vel = {0, 0, 0};

    // Ground check via raycast from capsule CENTER downward
    // Capsule half-height = 0.9. When standing on ground (y=0), center at y=0.9, bottom at 0.0.
    // Raycast distance = half-height + TINY epsilon (0.01) so it ONLY hits when capsule actually touches ground.
    // With D=0.91: hits when center ≤ 0.91 (bottom at 0.01), grounded when center ≈ 0.9 (bottom ≈ 0.0).
    const float CAPSULE_HALF_HEIGHT = CAPSULE_HEIGHT * 0.5f;  // 0.9f
    const float GROUND_CHECK_EPSILON = 0.01f;
    // The body is teleported, so a fast fall can end a frame inside the surface (or past it). Reach
    // down by this frame's fall distance so the landing is caught, then snap to the surface below.
    const float fallStep = verticalVelocity < 0.0f ? -verticalVelocity * dt : 0.0f;
    float groundCheckDist = CAPSULE_HALF_HEIGHT + GROUND_CHECK_EPSILON + fallStep;  // 0.91 units from center (+ fall)
    auto rayHit = sim.RayCast(pos, {0, -1, 0}, groundCheckDist, body);
    ReportGroundCheck(rayHit, pos, groundCheckDist);
    bool wasGrounded = grounded;
    // Rising means we just jumped: the ray still reaches the surface for the first few frames of the
    // jump, and counting that as ground would cancel the jump velocity (a tiny hop, then stuck).
    grounded = rayHit.hit && verticalVelocity <= 0.0f;

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

    vel.x = moveDir.x * speed;
    vel.z = moveDir.z * speed;

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

    // The kinematic body is teleported, so physics never stops it against static geometry
    // (city buildings, walls). Sweep short horizontal rays at foot/body/head height and
    // cancel the velocity component pushing into a near-vertical surface, so the player
    // slides along walls. Slopes (normal.y >= 0.5) and the ground are left to the
    // ground-check ray.
    {
        const float radius = CAPSULE_RADIUS;
        const float heights[3] = { -CAPSULE_HALF_HEIGHT + 0.4f, 0.0f, CAPSULE_HALF_HEIGHT - 0.1f };
        for (int pass = 0; pass < 2; ++pass) {
            const float hspeed = sqrtf(vel.x * vel.x + vel.z * vel.z);
            if (hspeed < 1e-4f) break;
            const Vector3 hdir = { vel.x / hspeed, 0.0f, vel.z / hspeed };
            const float reach = radius + hspeed * dt;
            bool blocked = false;
            for (float h : heights) {
                const Vector3 origin = { pos.x, pos.y + h, pos.z };
                const auto hit = sim.RayCast(origin, hdir, reach, body);
                if (!hit.hit || fabsf(hit.normal.y) >= 0.5f) continue;
                Vector3 n = { hit.normal.x, 0.0f, hit.normal.z };
                const float nl = sqrtf(n.x * n.x + n.z * n.z);
                if (nl < 1e-4f) continue;
                n.x /= nl; n.z /= nl;
                const float into = vel.x * n.x + vel.z * n.z;
                if (into >= 0.0f) continue;
                vel.x -= n.x * into;
                vel.z -= n.z * into;
                blocked = true;
            }
            if (!blocked) break;
        }
    }

    // For Box3D kinematic body: manually integrate position from velocity
    // Use SetBodyPosition - it sets physics body position and wakes it
    Vector3 newPos = Vector3Add(pos, Vector3Scale(vel, dt));

    // Standing: sit exactly on the surface (undoes any penetration left by a hard landing; also follows slopes).
    if (grounded && rayHit.hit) newPos.y = rayHit.point.y + CAPSULE_HALF_HEIGHT;
    
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