#include "Core/PlayerControllerSystem.h"

#include "Core/InputGlobals.h"
#include "Core/PhysicsGlobals.h"
#include "ECS/Coordinator.h"
#include "ECS/PhysicsSystem.h"
#include "ECS/SceneECS.h"

#include <SDL3/SDL.h>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <functional>

namespace ECS {

namespace {

Entity FindCameraEntity(const PlayerControllerComponent& controller) {
    SceneECS& scene = SceneECS::GetInstance();
    Coordinator& coordinator = Coordinator::GetInstance();

    if (!controller.cameraName.empty()) {
        const Entity named = scene.FindByName(controller.cameraName);
        if (named != INVALID_ENTITY &&
            coordinator.HasComponent<CameraComponent>(named) &&
            coordinator.HasComponent<TransformComponent>(named)) {
            return named;
        }
    }

    std::function<Entity(Entity)> visit = [&](Entity entity) -> Entity {
        if (coordinator.HasComponent<CameraComponent>(entity) &&
            coordinator.HasComponent<TransformComponent>(entity) &&
            coordinator.GetComponent<CameraComponent>(entity).isMainCamera) {
            return entity;
        }
        for (Entity child : scene.GetChildren(entity)) {
            const Entity result = visit(child);
            if (result != INVALID_ENTITY) return result;
        }
        return INVALID_ENTITY;
    };

    for (Entity root : scene.GetRootEntities()) {
        const Entity result = visit(root);
        if (result != INVALID_ENTITY) return result;
    }
    return INVALID_ENTITY;
}

glm::vec3 CameraForwardXZ(Entity cameraEntity) {
    if (cameraEntity == INVALID_ENTITY) return glm::vec3(0.0f, 0.0f, -1.0f);

    const glm::mat4 world = SceneECS::GetInstance().GetWorldMatrix(cameraEntity);
    glm::vec3 forward = glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
    forward.y = 0.0f;
    const float length = glm::length(forward);
    return length > 0.0001f ? forward / length : glm::vec3(0.0f, 0.0f, -1.0f);
}

} // namespace

void PlayerControllerSystem::SetSyntheticInput(const glm::vec2& move, bool jump) {
    m_SyntheticMove = glm::clamp(move, glm::vec2(-1.0f), glm::vec2(1.0f));
    m_SyntheticJump = jump;
    m_UseSyntheticInput = true;
}

void PlayerControllerSystem::ClearSyntheticInput() {
    m_SyntheticMove = glm::vec2(0.0f);
    m_SyntheticJump = false;
    m_UseSyntheticInput = false;
}

void PlayerControllerSystem::Update(float deltaTime) {
    if (!g_PhysicsSystemPtr || m_Entities.empty()) return;

    const float dt = std::clamp(deltaTime, 0.0f, 0.1f);
    glm::vec2 touchMove = g_InputController.GetTouchMoveDirection();
    bool keyW = g_InputController.IsKeyDown(SDL_SCANCODE_W);
    bool keyS = g_InputController.IsKeyDown(SDL_SCANCODE_S);
    bool keyA = g_InputController.IsKeyDown(SDL_SCANCODE_A);
    bool keyD = g_InputController.IsKeyDown(SDL_SCANCODE_D);
    bool keyJump = g_InputController.IsKeyDown(SDL_SCANCODE_SPACE);
    bool touchJump = g_InputController.IsTouchButtonDown(InputController::TouchAction::Jump);

    if (m_UseSyntheticInput) {
        // Match the virtual joystick convention: x = right, y = forward.
        touchMove = m_SyntheticMove;
        keyW = keyS = keyA = keyD = keyJump = false;
        touchJump = m_SyntheticJump;
    }

    for (Entity entity : m_Entities) {
        Coordinator& coordinator = Coordinator::GetInstance();
        if (!coordinator.HasComponent<PlayerControllerComponent>(entity) ||
            !coordinator.HasComponent<TransformComponent>(entity) ||
            !coordinator.HasComponent<RigidBodyComponent>(entity)) {
            continue;
        }

        const PlayerControllerComponent& controller =
            coordinator.GetComponent<PlayerControllerComponent>(entity);
        RuntimeState& runtime = m_Runtime[entity];
        const bool jumpDown = keyJump || touchJump;

        if (!controller.enabled) {
            runtime.jumpWasDown = jumpDown;
            continue;
        }

        const Entity cameraEntity = FindCameraEntity(controller);
        const glm::vec3 forward = CameraForwardXZ(cameraEntity);
        const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));

        glm::vec3 moveDirection(0.0f);
        if (keyW) moveDirection += forward;
        if (keyS) moveDirection -= forward;
        if (keyA) moveDirection -= right;
        if (keyD) moveDirection += right;
        if (glm::length(touchMove) > 0.001f) {
            moveDirection += right * touchMove.x - forward * touchMove.y;
        }
        if (glm::length(moveDirection) > 1.0f) {
            moveDirection = glm::normalize(moveDirection);
        }

        glm::vec3 velocity = g_PhysicsSystemPtr->GetLinearVelocity(entity);
        const bool nearRestingVerticalSpeed = std::abs(velocity.y) < 0.35f;
        const float control = nearRestingVerticalSpeed
            ? 1.0f
            : std::clamp(controller.airControl, 0.0f, 1.0f);
        const glm::vec3 desiredHorizontal =
            glm::vec3(moveDirection.x, 0.0f, moveDirection.z) * controller.moveSpeed;
        const float response = 1.0f - std::exp(-std::max(0.0f, controller.acceleration) *
                                                control * dt);
        velocity.x += (desiredHorizontal.x - velocity.x) * response;
        velocity.z += (desiredHorizontal.z - velocity.z) * response;

        // 这是原型阶段的保守跳跃判定：只有刚体垂直速度接近零时允许起跳，
        // 起跳后等到进入下落阶段才重新 armed，避免按住空格连续起跳。
        if (runtime.jumpArmed && velocity.y < -1.0f) {
            runtime.jumpArmed = false;
        }
        if (jumpDown && !runtime.jumpWasDown && !runtime.jumpArmed &&
            nearRestingVerticalSpeed) {
            velocity.y = std::max(0.0f, controller.jumpSpeed);
            runtime.jumpArmed = true;
        }
        g_PhysicsSystemPtr->SetLinearVelocity(entity, velocity);

        if (controller.faceMoveDirection && glm::length(moveDirection) > 0.001f) {
            const float yaw = std::atan2(moveDirection.x, -moveDirection.z);
            auto& transform = coordinator.GetComponent<TransformComponent>(entity);
            transform.rotation = glm::quat(glm::vec3(0.0f, yaw, 0.0f));
            transform.MarkDirty();
        }

        runtime.jumpWasDown = jumpDown;
    }
}

void PlayerControllerSystem::OnEntityRemoved(Entity entity) {
    m_Runtime.erase(entity);
}

} // namespace ECS
