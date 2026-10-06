#pragma once
#include "Entity.hpp"
#include "ScatteredObject.hpp"
#include "raylib.h"

class Engine;
namespace phys { class Simulation; }

enum class CharacterControllerMode {
    Kinematic,  // Direct position/velocity control (platformer-style)
    Dynamic     // Physics-body driven (force-based, physics-heavy games)
};

class CharacterController : public Entity {
public:
    CharacterController(Engine& engine, Camera3D* camera, phys::Simulation& sim,
                        CharacterControllerMode mode = CharacterControllerMode::Kinematic);
    ~CharacterController() override;
    void Update(float dt) override;

    // Script API
    void SetMoveInput(Vector2 input);        // WASD / stick
    void SetLookInput(Vector2 delta);        // mouse / stick
    void Jump();
    void SetSprint(bool sprint);
    void SetCrouch(bool crouch);
    void SetMode(CharacterControllerMode mode);

    // State queries
    bool IsGrounded() const;
    Vector3 GetVelocity() const;
    Vector3 GetPosition() const;
    Vector3 GetForward() const;
    Vector3 GetRight() const;

    // Access to physics body
    ScatteredObject* GetPlayerBody() const { return body; }

    // Call after sim.StartPlay() to spawn physics body
    void EnsurePhysicsBody();

    // Configurable
    float walkSpeed = 5.0f;
    float sprintSpeed = 10.0f;
    float jumpForce = 8.0f;
    // Gravity is now synced from PhysicsSimulation (default -9.81f).
    // Use SetGravity() on the Simulation to change both.
    float gravity = -9.81f;
    float airControl = 0.3f;
    float cameraSensitivity = 0.002f;
    float cameraPitchMin = -89.0f;
    float cameraPitchMax = 89.0f;

private:
    Engine& engine;
    Camera3D* camera;
    phys::Simulation& sim;
    CharacterControllerMode mode;

    ScatteredObject* body = nullptr;  // physics capsule
    float yaw = 0.0f, pitch = 0.0f;
    Vector2 moveInput = {0, 0};
    Vector2 lookInput = {0, 0};
    bool wantJump = false;
    bool sprint = false;
    bool crouch = false;
    bool grounded = false;
    float verticalVelocity = 0.0f;  // persists across frames so gravity actually accumulates
    float coyoteTimer = 0.0f;
    float jumpBufferTimer = 0.0f;

    void CreatePhysicsBody();
    void DestroyPhysicsBody();
    void UpdateKinematic(float dt);
    void UpdateDynamic(float dt);
    void UpdateCamera();
    void ProcessInput();
};