#define GLM_ENABLE_EXPERIMENTAL
#include "Core/ThirdPersonCameraSystem.h"
#include "Core/InputGlobals.h"
#include "ECS/Coordinator.h"
#include "ECS/Components.h"
#include "ECS/SceneECS.h"
#include "Core/RenderGlobals.h"
#include "Rendering/AABB.h"
#include "Rendering/SceneRenderer.h"
#include "World/World.h"
#include "World/WorldGlobals.h"
#include <SDL3/SDL.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <vector>

namespace {
constexpr float kPoseEpsilon = 0.000001f;

bool IsFiniteVec3(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z);
}

bool IsFiniteQuat(const glm::quat& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z) && std::isfinite(value.w);
}

glm::vec3 SafeNormalize(const glm::vec3& value, const glm::vec3& fallback) {
    if (!IsFiniteVec3(value)) return fallback;
    const float lengthSquared = glm::dot(value, value);
    if (!std::isfinite(lengthSquared) || lengthSquared <= kPoseEpsilon) {
        return fallback;
    }
    return value / std::sqrt(lengthSquared);
}

glm::quat SafeNormalize(const glm::quat& value, const glm::quat& fallback) {
    if (!IsFiniteQuat(value)) return fallback;
    const float lengthSquared = value.x * value.x + value.y * value.y +
        value.z * value.z + value.w * value.w;
    if (!std::isfinite(lengthSquared) || lengthSquared <= kPoseEpsilon) {
        return fallback;
    }
    return value / std::sqrt(lengthSquared);
}

glm::quat SafeLookAt(const glm::vec3& direction, const glm::vec3& up,
                     const glm::quat& fallbackRotation) {
    const glm::vec3 safeDirection = SafeNormalize(direction, glm::vec3(0.0f, 0.0f, -1.0f));
    glm::vec3 safeUp = SafeNormalize(up, glm::vec3(0.0f, 1.0f, 0.0f));
    // quatLookAt 的 up 与 forward 不能平行；锁定目标在玩家正上/正下方时，
    // 使用水平轴作为备用 up，避免叉积退化后产生 NaN 四元数。
    if (std::abs(glm::dot(safeDirection, safeUp)) > 0.999f) {
        safeUp = std::abs(safeDirection.y) < 0.999f
            ? glm::vec3(0.0f, 1.0f, 0.0f)
            : glm::vec3(1.0f, 0.0f, 0.0f);
    }
    return SafeNormalize(glm::quatLookAt(safeDirection, safeUp), fallbackRotation);
}

float DampingAlpha(float damping, float dt) {
    if (damping <= 0.0f) return 1.0f;
    return 1.0f - std::exp(-damping * std::max(dt, 0.0f));
}

// 编辑器模式下只允许游戏视图接收第三人称相机输入；层级、属性、场景视图、
// 控制面板等 ImGui 窗口上的鼠标/键盘操作不得改变游戏相机。纯游戏模式没有
// Editor.dll，直接放行输入。
bool IsThirdPersonInputAllowed() {
    if (!g_EditorActive || g_RunMode == RunMode::Game) return true;

    ImGuiContext* context = ImGui::GetCurrentContext();
    if (!context) return false;

    ImGuiWindow* gameView = ImGui::FindWindowByName("游戏视图");
    if (!gameView || gameView->Collapsed) return false;
    if (gameView->DockNode && gameView->DockNode->VisibleWindow != gameView) return false;
    return ImGui::IsMouseHoveringRect(gameView->Rect().Min, gameView->Rect().Max, true);
}

// 返回射线进入 AABB 的距离。把相机安全半径直接扩展到 AABB 后，
// 相机在接触碰撞体前就会停下，不会因为中心点尚未进入而穿过墙角。
bool RayAABBEntryDistance(const glm::vec3& start, const glm::vec3& direction,
                          const AABB& box, float maxDistance, float& outEntry,
                          glm::vec3* outNormal = nullptr) {
    float entry = 0.0f;
    float exit = maxDistance;
    glm::vec3 entryNormal(0.0f);

    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) < 0.000001f) {
            if (start[axis] < box.min[axis] || start[axis] > box.max[axis]) {
                return false;
            }
            continue;
        }

        float nearDistance = (box.min[axis] - start[axis]) / direction[axis];
        float farDistance = (box.max[axis] - start[axis]) / direction[axis];
        const float nearNormalSign = direction[axis] > 0.0f ? -1.0f : 1.0f;
        if (nearDistance > farDistance) std::swap(nearDistance, farDistance);
        if (nearDistance > entry) {
            entry = nearDistance;
            entryNormal = glm::vec3(0.0f);
            entryNormal[axis] = nearNormalSign;
        }
        exit = std::min(exit, farDistance);
        if (entry > exit) return false;
    }

    if (exit < 0.0f || entry > maxDistance) return false;
    outEntry = glm::clamp(entry, 0.0f, maxDistance);
    if (outNormal) *outNormal = entryNormal;
    return true;
}

bool IsEntityInSubtree(ECS::SceneECS& scene, ECS::Entity entity,
                       ECS::Entity ancestor) {
    ECS::Entity current = entity;
    for (int depth = 0; current != ECS::INVALID_ENTITY && depth < 1024; ++depth) {
        if (current == ancestor) return true;
        current = scene.GetParent(current);
    }
    return false;
}
}

ThirdPersonCameraSystem& ThirdPersonCameraSystem::GetInstance() {
    static ThirdPersonCameraSystem instance;
    return instance;
}

void ThirdPersonCameraSystem::SetSceneContext(const std::string& scenePath) {
    m_SceneContext = scenePath.empty() ? "<unknown>" : scenePath;
    m_MissingTargetDiagnostics.clear();
}

ECS::Entity ThirdPersonCameraSystem::FindMainCamera() const {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& scene = ECS::SceneECS::GetInstance();
    ECS::Entity result = ECS::INVALID_ENTITY;

    std::function<void(ECS::Entity)> visit = [&](ECS::Entity entity) {
        if (result != ECS::INVALID_ENTITY) return;
        if (coordinator.HasComponent<ECS::CameraComponent>(entity) &&
            coordinator.HasComponent<ECS::TransformComponent>(entity) &&
            coordinator.GetComponent<ECS::CameraComponent>(entity).isMainCamera) {
            result = entity;
            return;
        }
        for (const ECS::Entity child : scene.GetChildren(entity)) visit(child);
    };

    for (const ECS::Entity root : scene.GetRootEntities()) visit(root);
    return result;
}

ECS::Entity ThirdPersonCameraSystem::ResolveTarget(const ECS::CameraComponent& camera) const {
    if (camera.thirdPersonTargetName.empty()) return ECS::INVALID_ENTITY;
    return ECS::SceneECS::GetInstance().FindByName(camera.thirdPersonTargetName);
}

bool ThirdPersonCameraSystem::IsValidLockTarget(
    ECS::Entity entity, ECS::Entity followTarget, const ECS::CameraComponent& camera) const {
    if (entity == ECS::INVALID_ENTITY || entity == followTarget) return false;

    auto& coordinator = ECS::Coordinator::GetInstance();
    if (!coordinator.HasComponent<ECS::TransformComponent>(entity)) return false;

    auto& scene = ECS::SceneECS::GetInstance();
    const glm::vec3 followPosition = scene.GetWorldPosition(followTarget);
    const glm::vec3 targetPosition = scene.GetWorldPosition(entity);
    if (glm::distance(followPosition, targetPosition) >
        std::max(0.1f, camera.thirdPersonLockOnMaxDistance)) {
        return false;
    }

    if (!coordinator.HasComponent<ECS::LockOnTargetComponent>(entity)) {
        return !camera.thirdPersonLockTargetName.empty() &&
            scene.GetName(entity) == camera.thirdPersonLockTargetName;
    }
    return coordinator.GetComponent<ECS::LockOnTargetComponent>(entity).enabled;
}

ECS::Entity ThirdPersonCameraSystem::FindLockTarget(
    ECS::Entity followTarget, ECS::Entity cameraEntity,
    const ECS::CameraComponent& camera) const {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& scene = ECS::SceneECS::GetInstance();

    // 配置了名字时优先锁定指定目标，适合 Boss/剧情镜头。
    if (!camera.thirdPersonLockTargetName.empty()) {
        const ECS::Entity named = scene.FindByName(camera.thirdPersonLockTargetName);
        if (IsValidLockTarget(named, followTarget, camera)) return named;
    }

    if (!coordinator.HasComponent<ECS::TransformComponent>(cameraEntity)) {
        return ECS::INVALID_ENTITY;
    }

    const glm::vec3 followPosition = scene.GetWorldPosition(followTarget);
    const auto& cameraTransform = coordinator.GetComponent<ECS::TransformComponent>(cameraEntity);
    const glm::vec3 cameraPosition = scene.GetWorldPosition(cameraEntity);
    const glm::quat cameraRotation = SafeNormalize(
        cameraTransform.rotation, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    const glm::vec3 cameraForward = SafeNormalize(
        cameraRotation * glm::vec3(0.0f, 0.0f, -1.0f),
        glm::vec3(0.0f, 0.0f, -1.0f));
    const float maxDistance = std::max(0.1f, camera.thirdPersonLockOnMaxDistance);

    ECS::Entity best = ECS::INVALID_ENTITY;
    float bestScore = std::numeric_limits<float>::max();
    std::function<void(ECS::Entity)> visit = [&](ECS::Entity entity) {
        if (entity != followTarget && entity != cameraEntity &&
            coordinator.HasComponent<ECS::LockOnTargetComponent>(entity) &&
            coordinator.HasComponent<ECS::TransformComponent>(entity)) {
            const auto& targetable = coordinator.GetComponent<ECS::LockOnTargetComponent>(entity);
            if (targetable.enabled) {
                const glm::vec3 targetPosition = scene.GetWorldPosition(entity) + targetable.aimOffset;
                const glm::vec3 fromPlayer = targetPosition - followPosition;
                const float distance = glm::length(fromPlayer);
                if (distance <= maxDistance && distance > 0.001f) {
                    const glm::vec3 viewDirection = glm::normalize(targetPosition - cameraPosition);
                    const float facing = glm::dot(cameraForward, viewDirection);
                    // 允许目标略在侧后方，但过滤掉完全背后的对象。
                    if (facing >= -0.75f) {
                        const float anglePenalty = (1.0f - facing) * 4.0f;
                        const float score = distance + anglePenalty - targetable.priority * 5.0f;
                        if (score < bestScore) {
                            bestScore = score;
                            best = entity;
                        }
                    }
                }
            }
        }
        for (const ECS::Entity child : scene.GetChildren(entity)) visit(child);
    };

    for (const ECS::Entity root : scene.GetRootEntities()) visit(root);
    return best;
}

glm::vec3 ThirdPersonCameraSystem::ResolveCameraCollisionPosition(
    const glm::vec3& pivot, const glm::vec3& desiredPosition,
    ECS::Entity followTarget, ECS::Entity cameraEntity,
    const ECS::CameraComponent& camera, float* outSafeDistance) const {
    if (outSafeDistance) *outSafeDistance = 0.0f;
    if (!IsFiniteVec3(pivot)) return glm::vec3(0.0f);
    if (!IsFiniteVec3(desiredPosition)) return pivot;

    // 遮挡与相机碰撞是两条独立路径：当前原型保持完整轨道距离，
    // 让后续的遮挡物淡化/虚化系统处理玩家与相机之间的可见性。
    if (!camera.thirdPersonCollisionEnabled ||
        camera.thirdPersonPreserveDistanceWhenOccluded) {
        if (outSafeDistance) *outSafeDistance = glm::distance(pivot, desiredPosition);
        return desiredPosition;
    }

    const glm::vec3 segment = desiredPosition - pivot;
    const float distance = glm::length(segment);
    if (outSafeDistance) *outSafeDistance = distance;
    if (distance <= 0.0001f) return desiredPosition;

    const glm::vec3 direction = segment / distance;
    const float clearance = std::max(0.0f, camera.thirdPersonCollisionRadius) +
        std::max(0.0f, camera.thirdPersonCollisionBuffer);
    struct CollisionObstacle {
        AABB world;
    };
    struct CollisionHit {
        AABB world;
        AABB expanded;
        glm::vec3 normal = glm::vec3(0.0f);
        float distance = 0.0f;
    };

    std::vector<CollisionObstacle> obstacles;
    obstacles.reserve(32);
    auto addObstacle = [&](const AABB& worldAABB) {
        if (worldAABB.IsValid()) obstacles.push_back({worldAABB});
    };

    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& scene = ECS::SceneECS::GetInstance();
    std::function<void(ECS::Entity)> visit = [&](ECS::Entity entity) {
        // 玩家自身及其子节点不能作为相机障碍物，否则相机一开始就会被
        // 玩家模型/碰撞体截停在角色内部。
        if (IsEntityInSubtree(scene, entity, followTarget) ||
            IsEntityInSubtree(scene, entity, cameraEntity)) {
            return;
        }

        if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
            const glm::mat4 worldMatrix = scene.GetWorldMatrix(entity);
            const bool hasCollider = coordinator.HasComponent<ECS::ColliderComponent>(entity);
            if (hasCollider) {
                const auto& collider = coordinator.GetComponent<ECS::ColliderComponent>(entity);
                if (!collider.isTrigger) {
                    const glm::vec3 halfSize = glm::max(
                        glm::abs(collider.size) * 0.5f, glm::vec3(0.001f));
                    addObstacle(AABB(
                        collider.offset - halfSize,
                        collider.offset + halfSize).Transform(worldMatrix));
                }
            } else if (coordinator.HasComponent<ECS::MeshComponent>(entity)) {
                // 没有显式 Collider 时，模型 subMesh AABB 作为原型期的碰撞兜底。
                const auto& mesh = coordinator.GetComponent<ECS::MeshComponent>(entity);
                if ((mesh.type == ECS::MeshType::Model || mesh.type == ECS::MeshType::Plane) &&
                    !mesh.modelPath.empty()) {
                    if (ModelRenderer* renderer = g_SceneRenderer.GetModelRenderer(mesh.modelPath);
                        renderer != nullptr && renderer->HasModelLoaded()) {
                        for (const AABB& localAABB : renderer->GetSubMeshAABBs()) {
                            addObstacle(localAABB.Transform(worldMatrix));
                        }
                    }
                }
            }
        }

        for (const ECS::Entity child : scene.GetChildren(entity)) visit(child);
    };
    for (const ECS::Entity root : scene.GetRootEntities()) visit(root);

    // 体素世界只作为可选兜底；模型 Collider/AABB 是本路径的主要碰撞来源。
    if (g_EnableVoxelWorld && g_World != nullptr) {
        const World::HitResult hit = g_World->RayCast(pivot, direction, distance);
        if (hit.hit) {
            const AABB voxelBox(glm::vec3(hit.position),
                                glm::vec3(hit.position) + glm::vec3(1.0f));
            addObstacle(voxelBox);
        }
    }

    auto findNearestHit = [&](const glm::vec3& start, const glm::vec3& end,
                              CollisionHit& outHit) {
        const glm::vec3 path = end - start;
        const float pathLength = glm::length(path);
        if (!std::isfinite(pathLength) || pathLength <= 0.0001f) return false;

        const glm::vec3 pathDirection = path / pathLength;
        bool found = false;
        float nearestDistance = pathLength;
        for (const CollisionObstacle& obstacle : obstacles) {
            const AABB expanded(
                obstacle.world.min - glm::vec3(clearance),
                obstacle.world.max + glm::vec3(clearance));

            // 如果起点只在安全扩展区内、但不在实际碰撞体内，允许沿着
            // 实际碰撞体的外侧向外移动。这个判断避免贴墙时把入口永久
            // 判成 0；真正进入墙体的路径仍会被拦截。
            if (expanded.Contains(start) && !obstacle.world.Contains(start)) {
                float actualEntry = pathLength;
                const bool entersActual = RayAABBEntryDistance(
                    start, pathDirection, obstacle.world, pathLength, actualEntry);
                if (!entersActual && !expanded.Contains(end)) continue;
            }

            float entry = pathLength;
            glm::vec3 normal(0.0f);
            if (!RayAABBEntryDistance(
                    start, pathDirection, expanded, pathLength, entry, &normal)) {
                continue;
            }
            if (entry >= nearestDistance - 0.0001f) continue;

            // 起点在扩展区内时，扩展 AABB 的入口距离是 0，无法直接给出
            // 墙面法线。用实际 AABB 再求一次法线，只用于保持结果稳定，
            // 不改变安全截停距离。
            if (glm::dot(normal, normal) <= 0.000001f) {
                float actualEntry = pathLength;
                glm::vec3 actualNormal(0.0f);
                if (RayAABBEntryDistance(
                        start, pathDirection, obstacle.world, pathLength,
                        actualEntry, &actualNormal) &&
                    glm::dot(actualNormal, actualNormal) > 0.000001f) {
                    normal = actualNormal;
                }
            }
            if (glm::dot(normal, normal) <= 0.000001f) {
                const int axis = std::abs(pathDirection.x) > std::abs(pathDirection.z)
                    ? (std::abs(pathDirection.x) > std::abs(pathDirection.y) ? 0 : 1)
                    : (std::abs(pathDirection.z) > std::abs(pathDirection.y) ? 2 : 1);
                normal[axis] = pathDirection[axis] > 0.0f ? -1.0f : 1.0f;
            }

            nearestDistance = entry;
            outHit.world = obstacle.world;
            outHit.expanded = expanded;
            outHit.normal = normal;
            outHit.distance = entry;
            found = true;
        }
        return found;
    };

    CollisionHit hit;
    if (!findNearestHit(pivot, desiredPosition, hit)) return desiredPosition;

    // AABB 已经按球体半径+缓冲扩展；在边界前留一点精度余量，避免
    // 相机因浮点误差贴进碰撞体。即使入口为 0，也至少离 pivot 一个
    // 相机安全半径，绝不把相机放到观察点中心。
    const float boundaryEpsilon = 0.005f;
    float safeDistance = std::min(
        distance,
        std::max(0.05f, hit.distance > clearance
            ? hit.distance - boundaryEpsilon
            : clearance));

    // 当玩家已经贴在墙面上时，沿当前视线维持最小距离可能没有合法
    // 的“既不穿墙又保持无遮挡”的点。Cinemachine 的 PreserveCameraDistance
    // 会尝试沿障碍物边界保留距离；这里采用同样的折中：只在相机安全距离
    // 已经塌缩到配置下限时，把相机中心移到扩展 AABB 的侧面，保持用户
    // 轨道状态不变。若相邻障碍物封死侧面，则退回到前面的安全截停点。
    const float preferredMinimumDistance = std::max(
        clearance + boundaryEpsilon * 2.0f,
        std::max(0.05f, camera.thirdPersonCollisionMinDistance));
    if (safeDistance + boundaryEpsilon < preferredMinimumDistance) {
        const glm::vec3 desiredOffset = desiredPosition - pivot;
        const glm::vec3 desiredDirection = SafeNormalize(
            desiredOffset, direction);
        const int normalAxis =
            (std::abs(hit.normal.x) >= std::abs(hit.normal.y) &&
             std::abs(hit.normal.x) >= std::abs(hit.normal.z)) ? 0 :
            ((std::abs(hit.normal.y) >= std::abs(hit.normal.z)) ? 1 : 2);
        const glm::vec3 previousOffset = IsFiniteVec3(m_Runtime.position)
            ? m_Runtime.position - pivot : desiredOffset;
        const glm::vec3 cameraRight = SafeNormalize(
            glm::cross(direction, glm::vec3(0.0f, 1.0f, 0.0f)),
            glm::vec3(1.0f, 0.0f, 0.0f));
        const float previousSide = glm::dot(previousOffset, cameraRight);

        float bestScore = std::numeric_limits<float>::max();
        glm::vec3 bestCandidate(0.0f);
        bool foundCandidate = false;
        const int axisCount = normalAxis == 1 ? 2 : 1;
        for (int axisChoice = 0; axisChoice < axisCount; ++axisChoice) {
            const int slideAxis = normalAxis == 0 ? 2
                : (normalAxis == 2 ? 0 : (axisChoice == 0 ? 0 : 2));
            glm::vec3 remaining = desiredOffset;
            remaining[slideAxis] = 0.0f;
            const float remainingLength = glm::length(remaining);
            if (remainingLength <= 0.0001f) continue;

            for (const float side : {-1.0f, 1.0f}) {
                const float boundary = side < 0.0f
                    ? hit.expanded.min[slideAxis] - boundaryEpsilon
                    : hit.expanded.max[slideAxis] + boundaryEpsilon;
                const float axisOffset = boundary - pivot[slideAxis];
                const float restSquared = distance * distance - axisOffset * axisOffset;
                if (restSquared <= 0.0001f) continue;

                const float scale = std::sqrt(restSquared) / remainingLength;
                glm::vec3 candidateOffset = remaining * scale;
                candidateOffset[slideAxis] = axisOffset;
                const glm::vec3 candidate = pivot + candidateOffset;
                if (!IsFiniteVec3(candidate)) continue;

                bool insideObstacle = false;
                for (const CollisionObstacle& obstacle : obstacles) {
                    const AABB expanded(
                        obstacle.world.min - glm::vec3(clearance),
                        obstacle.world.max + glm::vec3(clearance));
                    if (expanded.Contains(candidate)) {
                        insideObstacle = true;
                        break;
                    }
                }
                if (insideObstacle || glm::length(candidateOffset) + 0.001f <
                    preferredMinimumDistance) continue;

                float score = 1.0f - glm::dot(
                    SafeNormalize(candidateOffset, desiredDirection), desiredDirection);
                const float candidateSide = glm::dot(candidateOffset, cameraRight);
                if (std::abs(previousSide) > 0.05f &&
                    candidateSide * previousSide < 0.0f) {
                    score += 0.02f; // 保持上一帧的绕墙侧，避免左右翻转
                }
                if (!foundCandidate || score < bestScore) {
                    foundCandidate = true;
                    bestScore = score;
                    bestCandidate = candidate;
                }
            }
        }

        if (foundCandidate) {
            if (outSafeDistance) *outSafeDistance = glm::length(bestCandidate - pivot);
            return bestCandidate;
        }
    }

    if (outSafeDistance) *outSafeDistance = safeDistance;

    // 当前碰撞解析保持 Cinemachine ThirdPersonFollow 的主规则：只沿
    // 当前相机-目标方向拉近，不改写用户 yaw/pitch。拐角处是否能绕墙
    // 由玩家主动环绕决定，系统不再自动搜索另一条轨道造成吸附。
    return pivot + direction * safeDistance;
}

void ThirdPersonCameraSystem::DiagnoseMissingTarget(
    ECS::Entity camera, const ECS::CameraComponent& component) {
    const std::string key = std::to_string(camera) + ":" + component.thirdPersonTargetName;
    if (m_MissingTargetDiagnostics.insert(key).second) {
        std::cerr << "[ThirdPersonCameraSystem] target not found: camera=" << camera
                  << " target=\"" << component.thirdPersonTargetName
                  << "\" scene=\"" << m_SceneContext << "\"\n";
    }
}

void ThirdPersonCameraSystem::ReleaseOwnedMouseCapture() {
    if (m_MouseCaptureOwned) {
        if (g_InputController.IsMouseCaptured()) g_InputController.SetMouseCapture(false);
        m_MouseCaptureOwned = false;
    }
}

void ThirdPersonCameraSystem::Reset() {
    ReleaseOwnedMouseCapture();
    m_Runtime = RuntimeState{};
    m_MissingTargetDiagnostics.clear();
    m_LockButtonWasDown = false;
}

void ThirdPersonCameraSystem::ClearLockOn() {
    m_Runtime.lockOn = false;
    m_Runtime.lockTarget = ECS::INVALID_ENTITY;
}

void ThirdPersonCameraSystem::Update(float dt) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& scene = ECS::SceneECS::GetInstance();

    const ECS::Entity cameraEntity = FindMainCamera();
    if (cameraEntity == ECS::INVALID_ENTITY ||
        !coordinator.HasComponent<ECS::CameraComponent>(cameraEntity) ||
        !coordinator.HasComponent<ECS::TransformComponent>(cameraEntity)) {
        g_InputController.ConsumeMouseDelta();
        g_InputController.ConsumeTouchLookDelta();
        g_InputController.ConsumeTouchZoomDelta();
        g_InputController.ConsumeMouseWheel();
        ReleaseOwnedMouseCapture();
        m_Runtime = RuntimeState{};
        m_LockButtonWasDown = false;
        return;
    }

    auto& camera = coordinator.GetComponent<ECS::CameraComponent>(cameraEntity);
    auto& cameraTransform = coordinator.GetComponent<ECS::TransformComponent>(cameraEntity);
    if (!camera.thirdPersonEnabled) {
        if (m_Runtime.initialized && m_Runtime.camera == cameraEntity &&
            m_Runtime.sceneVersion == scene.GetEntitySetVersion()) {
            camera.fov = m_Runtime.baseFov;
        }
        g_InputController.ConsumeMouseDelta();
        g_InputController.ConsumeTouchLookDelta();
        g_InputController.ConsumeTouchZoomDelta();
        g_InputController.ConsumeMouseWheel();
        ReleaseOwnedMouseCapture();
        m_Runtime = RuntimeState{};
        m_LockButtonWasDown = false;
        return;
    }

    const ECS::Entity targetEntity = ResolveTarget(camera);
    if (targetEntity == ECS::INVALID_ENTITY ||
        !coordinator.HasComponent<ECS::TransformComponent>(targetEntity)) {
        DiagnoseMissingTarget(cameraEntity, camera);
        g_InputController.ConsumeMouseDelta();
        g_InputController.ConsumeTouchLookDelta();
        g_InputController.ConsumeTouchZoomDelta();
        g_InputController.ConsumeMouseWheel();
        ReleaseOwnedMouseCapture();
        m_Runtime = RuntimeState{};
        m_LockButtonWasDown = false;
        return;
    }

    const bool cameraInputAllowed = IsThirdPersonInputAllowed();
    if (camera.thirdPersonCaptureMouse && cameraInputAllowed) {
        if (!g_InputController.IsMouseCaptured()) {
            g_InputController.SetMouseCapture(true);
            m_MouseCaptureOwned = true;
        }
    } else {
        ReleaseOwnedMouseCapture();
    }

    const uint32_t sceneVersion = scene.GetEntitySetVersion();
    const bool needsInitialization = !m_Runtime.initialized ||
        m_Runtime.camera != cameraEntity || m_Runtime.target != targetEntity ||
        m_Runtime.sceneVersion != sceneVersion;
    if (needsInitialization) {
        const float minDistance = std::max(0.01f, camera.thirdPersonMinDistance);
        const float maxDistance = std::max(minDistance, camera.thirdPersonMaxDistance);
        m_Runtime.camera = cameraEntity;
        m_Runtime.target = targetEntity;
        m_Runtime.sceneVersion = sceneVersion;
        m_Runtime.yaw = camera.thirdPersonYaw;
        m_Runtime.pitch = glm::clamp(camera.thirdPersonPitch, camera.thirdPersonMinPitch, camera.thirdPersonMaxPitch);
        m_Runtime.distance = glm::clamp(camera.thirdPersonDistance, minDistance, maxDistance);
        m_Runtime.collisionLastSafePosition = glm::vec3(0.0f);
        m_Runtime.collisionClearTime = 0.0f;
        m_Runtime.collisionSettleTime = 0.0f;
        m_Runtime.collisionActive = false;
        m_Runtime.collisionStateValid = false;
        m_Runtime.baseFov = camera.fov;
        if (!std::isfinite(m_Runtime.baseFov)) m_Runtime.baseFov = 60.0f;
        m_Runtime.aiming = false;
        m_Runtime.lockOn = false;
        m_Runtime.lockTarget = ECS::INVALID_ENTITY;
        m_Runtime.position = IsFiniteVec3(cameraTransform.position)
            ? cameraTransform.position
            : scene.GetWorldPosition(cameraEntity);
        if (!IsFiniteVec3(m_Runtime.position)) m_Runtime.position = glm::vec3(0.0f);
        m_Runtime.rotation = SafeNormalize(
            cameraTransform.rotation, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        m_Runtime.initialized = true;
    }

    const bool lockButtonDown = cameraInputAllowed &&
        g_InputController.IsKeyDown(SDL_SCANCODE_Q);
    const bool lockButtonPressed = lockButtonDown && !m_LockButtonWasDown;
    m_LockButtonWasDown = lockButtonDown;
    if (!camera.thirdPersonLockOnEnabled) {
        ClearLockOn();
    } else if (lockButtonPressed) {
        if (m_Runtime.lockOn) {
            ClearLockOn();
        } else {
            const ECS::Entity lockTarget = FindLockTarget(
                targetEntity, cameraEntity, camera);
            if (lockTarget != ECS::INVALID_ENTITY) {
                m_Runtime.lockOn = true;
                m_Runtime.lockTarget = lockTarget;
            } else {
                std::cerr << "[ThirdPersonCameraSystem] no lock target within "
                          << camera.thirdPersonLockOnMaxDistance << " units\n";
            }
        }
    }

    if (m_Runtime.lockOn && !IsValidLockTarget(
            m_Runtime.lockTarget, targetEntity, camera)) {
        const ECS::Entity replacement = FindLockTarget(
            targetEntity, cameraEntity, camera);
        if (replacement != ECS::INVALID_ENTITY) {
            m_Runtime.lockTarget = replacement;
        } else {
            ClearLockOn();
        }
    }

    const bool aiming = cameraInputAllowed && camera.thirdPersonAimEnabled &&
        (g_InputController.IsKeyDown(SDL_SCANCODE_LSHIFT) ||
         g_InputController.IsKeyDown(SDL_SCANCODE_RSHIFT));
    m_Runtime.aiming = aiming;

    const glm::vec2 mouseDelta = g_InputController.ConsumeMouseDelta();
    const glm::vec2 touchLookDelta = g_InputController.ConsumeTouchLookDelta();
    const float touchZoomDelta = g_InputController.ConsumeTouchZoomDelta();
    // 非持续捕获模式必须按住右键才允许自由视角；编辑器窗口上的输入已被
    // cameraInputAllowed 屏蔽，避免操作属性/层级面板时改变游戏相机。
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    const Uint32 mouseState = SDL_GetMouseState(&mouseX, &mouseY);
    const bool rightMouseDown =
        g_InputController.IsMouseButtonDown(SDL_BUTTON_RIGHT) ||
        (mouseState & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) != 0;
    const bool orbiting = cameraInputAllowed && (camera.thirdPersonCaptureMouse ||
        rightMouseDown || aiming);
    float requestedYaw = m_Runtime.yaw;
    float requestedPitch = m_Runtime.pitch;
    if (orbiting) {
        const float sensitivity = aiming
            ? camera.thirdPersonAimSensitivity
            : camera.thirdPersonOrbitSensitivity;
        // 轨道相机约定：鼠标向右拖动时，相机从玩家后方绕向玩家右侧。
        requestedYaw -= mouseDelta.x * sensitivity;
        requestedPitch -= mouseDelta.y * sensitivity;
        requestedPitch = glm::clamp(
            requestedPitch, camera.thirdPersonMinPitch, camera.thirdPersonMaxPitch);
    }
    if (cameraInputAllowed && g_InputController.IsTouchEnabled() &&
        glm::dot(touchLookDelta, touchLookDelta) > 0.000001f) {
        // 触摸屏坐标 y 向下；手指上滑时 deltaY 为负，让 pitch 减小，镜头抬头。
        requestedYaw -= touchLookDelta.x * camera.thirdPersonOrbitSensitivity;
        requestedPitch += touchLookDelta.y * camera.thirdPersonOrbitSensitivity;
        requestedPitch = glm::clamp(
            requestedPitch, camera.thirdPersonMinPitch, camera.thirdPersonMaxPitch);
    }

    const float wheel = g_InputController.ConsumeMouseWheel();
    if (wheel != 0.0f || std::abs(touchZoomDelta) > 0.0001f) {
        const float minDistance = std::max(0.01f, camera.thirdPersonMinDistance);
        const float maxDistance = std::max(minDistance, camera.thirdPersonMaxDistance);
        // 双指张开为正，与滚轮上滚一致地拉近相机；100 像素约调整 2 个世界单位，
        // 再乘场景已有的 thirdPersonZoomSensitivity 以保持配置语义一致。
        constexpr float kTouchZoomPixelsToWorld = 0.02f;
        m_Runtime.distance = glm::clamp(
            m_Runtime.distance - camera.thirdPersonZoomSensitivity *
                (wheel + touchZoomDelta * kTouchZoomPixelsToWorld),
            minDistance, maxDistance);
    }

    const glm::vec3 pivot = scene.GetWorldPosition(targetEntity) + camera.thirdPersonTargetOffset;
    glm::vec3 lookPivot = pivot;
    float effectiveDistance = m_Runtime.distance;
    if (m_Runtime.lockOn) {
        glm::vec3 lockPosition = scene.GetWorldPosition(m_Runtime.lockTarget);
        if (coordinator.HasComponent<ECS::LockOnTargetComponent>(m_Runtime.lockTarget)) {
            lockPosition += coordinator.GetComponent<ECS::LockOnTargetComponent>(m_Runtime.lockTarget).aimOffset;
        }
        const float blend = glm::clamp(camera.thirdPersonLockOnLookAtBlend, 0.0f, 1.0f);
        lookPivot = glm::mix(pivot, lockPosition, blend);

        // 目标距离较远时适当拉远相机，尽量同时保留玩家和敌人。
        const float playerTargetDistance = glm::distance(pivot, lockPosition);
        const float lockDistance = playerTargetDistance * 0.7f;
        const float minDistance = std::max(0.01f, camera.thirdPersonMinDistance);
        const float maxDistance = std::max(minDistance, camera.thirdPersonMaxDistance);
        effectiveDistance = glm::clamp(std::max(effectiveDistance, lockDistance), minDistance, maxDistance);
    }
    const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
    const float shoulderOffset = aiming ? camera.thirdPersonAimShoulderOffset : 0.0f;

    auto buildDesiredPosition = [&](float yaw, float pitch) {
        const float yawRadians = glm::radians(yaw);
        const float pitchRadians = glm::radians(pitch);
        const float cosPitch = std::cos(pitchRadians);
        const glm::vec3 orbitDirection(
            std::sin(yawRadians) * cosPitch,
            std::sin(pitchRadians),
            std::cos(yawRadians) * cosPitch);
        const glm::vec3 orbitPosition = pivot + orbitDirection * effectiveDistance;
        const glm::vec3 orbitLookDirection = SafeNormalize(
            lookPivot - orbitPosition, SafeNormalize(
                -orbitDirection, glm::vec3(0.0f, 0.0f, -1.0f)));
        glm::vec3 cameraRight = glm::cross(orbitLookDirection, worldUp);
        cameraRight = SafeNormalize(cameraRight, glm::vec3(1.0f, 0.0f, 0.0f));
        return orbitPosition + cameraRight * shoulderOffset;
    };

    auto resolveOrbit = [&](float yaw, float pitch,
                            glm::vec3& outDesired,
                            glm::vec3& outAdjusted,
                            float* outSafeDistance = nullptr) {
        outDesired = buildDesiredPosition(yaw, pitch);
        if (!IsFiniteVec3(outDesired)) {
            outDesired = IsFiniteVec3(m_Runtime.position)
                ? m_Runtime.position
                : pivot + glm::vec3(0.0f, 0.0f, std::max(0.05f, effectiveDistance));
            outAdjusted = outDesired;
            return false;
        }
        outAdjusted = ResolveCameraCollisionPosition(
            pivot, outDesired, targetEntity, cameraEntity, camera,
            outSafeDistance);
        if (!IsFiniteVec3(outAdjusted)) outAdjusted = outDesired;
        return true;
    };

    glm::vec3 desiredPosition;
    glm::vec3 collisionAdjustedPosition;
    float rawSafeDistance = 0.0f;
    if (!resolveOrbit(requestedYaw, requestedPitch,
                      desiredPosition, collisionAdjustedPosition,
                      &rawSafeDistance)) {
        desiredPosition = IsFiniteVec3(m_Runtime.position)
            ? m_Runtime.position
            : pivot + glm::vec3(0.0f, 0.0f, std::max(0.05f, effectiveDistance));
        collisionAdjustedPosition = desiredPosition;
        rawSafeDistance = glm::distance(pivot, desiredPosition);
    }

    // 可选的 Cinemachine ThirdPersonFollow 风格碰撞路径只修正相机位置，
    // 不改变用户输入的 yaw/pitch。保持距离模式下这里直接使用完整轨道，
    // 不进入碰撞缩距与恢复状态机。
    const float desiredDistance = glm::distance(pivot, desiredPosition);
    const bool collisionResolutionEnabled = camera.thirdPersonCollisionEnabled &&
        !camera.thirdPersonPreserveDistanceWhenOccluded;
    const bool collisionPositionCorrected = collisionResolutionEnabled &&
        glm::dot(collisionAdjustedPosition - desiredPosition,
                 collisionAdjustedPosition - desiredPosition) > 0.000001f;
    const bool rawBlocked = collisionResolutionEnabled &&
        ((std::isfinite(rawSafeDistance) &&
          rawSafeDistance + 0.001f < desiredDistance) ||
         collisionPositionCorrected);
    const float smoothingTime = std::max(
        0.0f, camera.thirdPersonCollisionSmoothingTime);
    const float safeDt = std::max(0.0f, dt);
    // 配置从缩距模式切换到保持距离模式时，清掉旧的安全点，避免旧状态
    // 在 smoothingTime 内再次把相机留在玩家身边。
    if (camera.thirdPersonPreserveDistanceWhenOccluded) {
        m_Runtime.collisionLastSafePosition = desiredPosition;
        m_Runtime.collisionClearTime = 0.0f;
        m_Runtime.collisionSettleTime = 0.0f;
        m_Runtime.collisionActive = false;
        m_Runtime.collisionStateValid = true;
    }
    const bool wasCollisionActive = m_Runtime.collisionActive;

    if (!m_Runtime.collisionStateValid) {
        m_Runtime.collisionLastSafePosition = collisionAdjustedPosition;
        m_Runtime.collisionClearTime = 0.0f;
        m_Runtime.collisionSettleTime = 0.0f;
        m_Runtime.collisionActive = rawBlocked;
        m_Runtime.collisionStateValid = true;
    }

    glm::vec3 collisionTarget = collisionAdjustedPosition;
    if (rawBlocked) {
        const float rawDistance = glm::distance(pivot, collisionAdjustedPosition);
        const float heldDistance = glm::distance(
            pivot, m_Runtime.collisionLastSafePosition);

        m_Runtime.collisionClearTime = 0.0f;
        if (!m_Runtime.collisionActive ||
            !std::isfinite(heldDistance) || rawDistance < heldDistance ||
            smoothingTime <= 0.0f) {
            m_Runtime.collisionLastSafePosition = collisionAdjustedPosition;
            m_Runtime.collisionSettleTime = 0.0f;
        } else {
            m_Runtime.collisionSettleTime += safeDt;
        }
        m_Runtime.collisionActive = true;

        // 遮挡路径改变时先保持最近安全点一小段时间，避免方块角点的
        // AABB 命中面切换直接把镜头拉远/拉近；遮挡持续后再接受新距离。
        const float settleTime = m_Runtime.collisionSettleTime;
        collisionTarget = (settleTime < smoothingTime)
            ? m_Runtime.collisionLastSafePosition
            : collisionAdjustedPosition;
        if (settleTime >= smoothingTime)
            m_Runtime.collisionLastSafePosition = collisionAdjustedPosition;
    } else if (m_Runtime.collisionActive) {
        m_Runtime.collisionClearTime += safeDt;
        if (m_Runtime.collisionClearTime < smoothingTime) {
            collisionTarget = m_Runtime.collisionLastSafePosition;
        } else {
            m_Runtime.collisionActive = false;
            collisionTarget = desiredPosition;
        }
    } else {
        collisionTarget = desiredPosition;
    }

    m_Runtime.yaw = requestedYaw;
    m_Runtime.pitch = requestedPitch;
    const glm::quat safeRuntimeRotation = SafeNormalize(
        m_Runtime.rotation, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    const glm::vec3 fallbackLookDirection = SafeNormalize(
        safeRuntimeRotation * glm::vec3(0.0f, 0.0f, -1.0f),
        glm::vec3(0.0f, 0.0f, -1.0f));
    const glm::vec3 lookDirection = SafeNormalize(
        lookPivot - collisionTarget, fallbackLookDirection);
    const glm::quat desiredRotation = SafeLookAt(
        lookDirection, worldUp, safeRuntimeRotation);

    const float positionDamping = aiming
        ? camera.thirdPersonAimPositionDamping
        : camera.thirdPersonPositionDamping;
    const float rotationDamping = aiming
        ? camera.thirdPersonAimRotationDamping
        : camera.thirdPersonRotationDamping;

    const glm::vec3 safeRuntimePosition = IsFiniteVec3(m_Runtime.position)
        ? m_Runtime.position
        : collisionTarget;
    float targetPositionDamping = positionDamping;
    if (rawBlocked) {
        // 进入遮挡时快速收回，避免相机在墙体内部停留；离开时使用
        // 更慢的恢复阻尼，符合 Cinemachine 的 Into/FromCollision 分离。
        targetPositionDamping = std::max(
            positionDamping, std::max(0.0f, camera.thirdPersonCollisionDampingIn));
    } else if (wasCollisionActive) {
        targetPositionDamping = std::min(
            positionDamping, std::max(0.0f, camera.thirdPersonCollisionDampingOut));
    }
    m_Runtime.position = glm::mix(
        safeRuntimePosition, collisionTarget,
        DampingAlpha(targetPositionDamping, dt));
    if (!IsFiniteVec3(m_Runtime.position)) m_Runtime.position = collisionTarget;
    // 阻尼插值的旧位置也可能已经落在墙内；写回前再复检一次，避免平滑过程穿模。
    m_Runtime.position = ResolveCameraCollisionPosition(
        pivot, m_Runtime.position, targetEntity, cameraEntity, camera);
    if (!IsFiniteVec3(m_Runtime.position)) m_Runtime.position = collisionTarget;
    m_Runtime.rotation = SafeNormalize(glm::slerp(
        safeRuntimeRotation, desiredRotation,
        DampingAlpha(rotationDamping, dt)), safeRuntimeRotation);
    if (!IsFiniteQuat(m_Runtime.rotation)) m_Runtime.rotation = desiredRotation;

    const float targetFov = aiming
        ? glm::clamp(camera.thirdPersonAimFov, 1.0f, 179.0f)
        : m_Runtime.baseFov;
    if (!std::isfinite(camera.fov)) camera.fov = m_Runtime.baseFov;
    camera.fov = glm::mix(camera.fov, targetFov, DampingAlpha(rotationDamping, dt));
    if (!std::isfinite(camera.fov)) camera.fov = targetFov;

    scene.SetPosition(cameraEntity, m_Runtime.position);
    scene.SetRotation(cameraEntity, m_Runtime.rotation);
}
