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
#include <utility>

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

void PlayerControllerSystem::EnableGroundRelativeMotion(Entity entity) {
    m_Runtime[entity].groundRelativeMotion = true;
}

void PlayerControllerSystem::RequestJump(Entity entity) {
    m_Runtime[entity].jumpRequested = true;
}

bool PlayerControllerSystem::QueryGroundSupport(Entity entity, glm::vec3& supportVelocity) const {
    supportVelocity = glm::vec3(0.0f);
    if (!g_PhysicsSystemPtr || entity == INVALID_ENTITY) return false;
    const glm::vec3 feet = SceneECS::GetInstance().GetWorldPosition(entity);
    constexpr glm::vec3 offsets[] = {{0, 0, 0}, {.19f, 0, 0}, {-.19f, 0, 0}, {0, 0, .19f}, {0, 0, -.19f}};
    bool supportingBodyFound = false;
    for (const glm::vec3& offset : offsets) {
        const glm::vec3 sample = feet + offset;
        if (g_PhysicsSystemPtr->CastSolidRay(sample + glm::vec3(0, .08f, 0),
                                             sample - glm::vec3(0, .16f, 0), entity) < 1.0f) {
            supportingBodyFound = true;
            break;
        }
    }
    if (!supportingBodyFound) return false;

    std::vector<Entity> contacts;
    g_PhysicsSystemPtr->QueryOrientedBox(feet - glm::vec3(0, .06f, 0), glm::quat(1, 0, 0, 0),
                                         {.22f, .12f, .22f}, entity, contacts);
    auto& coordinator = Coordinator::GetInstance();
    for (const Entity contact : contacts) {
        if (!coordinator.HasComponent<RigidBodyComponent>(contact)) continue;
        const auto& collider = coordinator.GetComponent<RigidBodyComponent>(contact);
        if (collider.type == RigidBodyComponent::Type::Kinematic) {
            supportVelocity = g_PhysicsSystemPtr->GetLinearVelocity(contact);
            break;
        }
    }
    return std::abs(g_PhysicsSystemPtr->GetLinearVelocity(entity).y - supportVelocity.y) < .55f;
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
        const bool jumpRequested = std::exchange(runtime.jumpRequested, false);

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
        glm::vec3 supportVelocity(0.0f);
        const bool grounded = runtime.groundRelativeMotion
            ? QueryGroundSupport(entity, supportVelocity)
            : std::abs(velocity.y) < 0.35f;
        const float control = grounded
            ? 1.0f
            : std::clamp(controller.airControl, 0.0f, 1.0f);
        const glm::vec3 desiredHorizontal =
            glm::vec3(moveDirection.x, 0.0f, moveDirection.z) * controller.moveSpeed +
            (grounded ? supportVelocity : glm::vec3(0.0f));
        const float response = 1.0f - std::exp(-std::max(0.0f, controller.acceleration) *
                                                control * dt);
        velocity.x += (desiredHorizontal.x - velocity.x) * response;
        velocity.z += (desiredHorizontal.z - velocity.z) * response;

        // 这是原型阶段的保守跳跃判定：只有刚体垂直速度接近零时允许起跳，
        // 起跳后等到进入下落阶段才重新 armed，避免按住空格连续起跳。
        if (runtime.jumpArmed && velocity.y < -1.0f) {
            runtime.jumpArmed = false;
        }
        if (((jumpDown && !runtime.jumpWasDown) || jumpRequested) && !runtime.jumpArmed &&
            grounded) {
            velocity.y = std::max(0.0f, controller.jumpSpeed) + supportVelocity.y;
            runtime.jumpArmed = true;
        }
        g_PhysicsSystemPtr->SetLinearVelocity(entity, velocity);

        if (controller.faceMoveDirection && glm::length(moveDirection) > 0.001f) {
            const float yaw = std::atan2(moveDirection.x, -moveDirection.z);
            // 朝向写刚体而非 Transform:直接写 Transform.rotation 会触发
            // PhysicsSystem 的"外部修改"判定,每帧把刚体位置重置回旧
            // Transform,清空本帧位移(角色原地踏步)。刚体旋转经正常
            // 物理→Transform 同步回流,不产生版本冲突。
            g_PhysicsSystemPtr->SetRigidBodyOrientation(
                entity, glm::quat(glm::vec3(0.0f, yaw, 0.0f)));
        }

        runtime.jumpWasDown = jumpDown;
    }
}

void PlayerControllerSystem::OnEntityRemoved(Entity entity) {
    m_Runtime.erase(entity);
}

} // namespace ECS
