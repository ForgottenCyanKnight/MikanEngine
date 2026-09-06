#define GLM_ENABLE_EXPERIMENTAL
#include "ECS/Systems/VmdSystem.h"

#include "Core/ProjectManager.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"
#include "Rendering/ModelRenderer.h"
#include "Rendering/SceneCollector.h"
#include "Rendering/SceneRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

// SceneRenderer 的实际实例由 EngineGlobals.cpp 提供；不要调用
// SceneRenderer::GetInstance()，否则会产生一个未初始化的第二个渲染器实例。
extern SceneRenderer g_SceneRenderer;

namespace ECS {
namespace {

constexpr float kVmdFramesPerSecond = 30.0f;

bool IsFinite(float value) {
    return std::isfinite(value);
}

float SafeFrame(float value, float lastFrame) {
    if (!IsFinite(value)) return 0.0f;
    return std::clamp(value, 0.0f, std::max(0.0f, lastFrame));
}

} // namespace

VmdSystem& VmdSystem::GetInstance() {
    static VmdSystem instance;
    return instance;
}

void VmdSystem::Clear() {
    m_runtime.clear();
}

bool VmdSystem::EnsureMotion(Entity entity, const std::string& path, RuntimeState& state) {
    const std::string resolvedPath = ProjectManager::GetInstance().ResolveAssetPath(path);
    if (state.resolvedPath != resolvedPath) {
        state = RuntimeState{};
        state.resolvedPath = resolvedPath;
    }
    if (resolvedPath.empty()) return false;
    if (state.motion) return true;
    if (state.loadAttempted) return false;

    state.loadAttempted = true;
    auto motion = std::make_shared<Animation::VmdMotion>();
    std::string error;
    if (!Animation::VmdMotion::LoadFromFile(resolvedPath, *motion, error)) {
        state.loadError = error;
        std::fprintf(stderr, "[VmdSystem] entity=%u failed to load '%s': %s\n",
                     static_cast<unsigned>(entity), resolvedPath.c_str(), error.c_str());
        return false;
    }
    if (motion->Empty()) {
        state.loadError = "motion contains no bone or camera frames";
        std::fprintf(stderr, "[VmdSystem] entity=%u motion '%s' is empty\n",
                     static_cast<unsigned>(entity), resolvedPath.c_str());
        return false;
    }

    state.motion = std::move(motion);
    std::printf("[VmdSystem] entity=%u loaded '%s' (bones=%zu cameras=%zu lastFrame=%.0f)\n",
                static_cast<unsigned>(entity), resolvedPath.c_str(),
                state.motion->GetBoneTracks().size(),
                state.motion->GetCameraKeyframes().size(),
                state.motion->GetLastFrame());
    return true;
}

void VmdSystem::Advance(VmdPlayerComponent& player, RuntimeState& state, float deltaTime) {
    const float lastFrame = std::max(0.0f, state.motion ? state.motion->GetLastFrame() : 0.0f);
    const float requestedStart = IsFinite(player.startFrame) ? player.startFrame : 0.0f;
    const float startFrame = SafeFrame(requestedStart, lastFrame);

    if (!state.initialized || state.lastStartFrame != startFrame || !IsFinite(player.currentFrame)) {
        player.currentFrame = startFrame;
        state.lastStartFrame = startFrame;
        state.initialized = true;
    }
    player.currentFrame = SafeFrame(player.currentFrame, lastFrame);

    const float speed = IsFinite(player.speed) ? player.speed : 0.0f;
    const float dt = IsFinite(deltaTime) && deltaTime > 0.0f ? deltaTime : 0.0f;
    if (player.playing && speed != 0.0f) {
        player.currentFrame += dt * kVmdFramesPerSecond * speed;
    }

    const float span = lastFrame - startFrame;
    if (span <= std::numeric_limits<float>::epsilon()) {
        player.currentFrame = startFrame;
        return;
    }

    if (player.loop) {
        float offset = std::fmod(player.currentFrame - startFrame, span);
        if (offset < 0.0f) offset += span;
        player.currentFrame = startFrame + offset;
    } else {
        if (speed >= 0.0f && player.currentFrame >= lastFrame) {
            player.currentFrame = lastFrame;
            player.playing = false;
        } else if (speed < 0.0f && player.currentFrame <= startFrame) {
            player.currentFrame = startFrame;
            player.playing = false;
        } else {
            player.currentFrame = std::clamp(player.currentFrame, startFrame, lastFrame);
        }
    }
}

void VmdSystem::ApplyToModel(Entity entity, const VmdPlayerComponent& player,
                             const Animation::VmdMotion& motion) {
    auto& coordinator = Coordinator::GetInstance();
    if (!coordinator.HasComponent<MeshComponent>(entity)) return;
    const auto& mesh = coordinator.GetComponent<MeshComponent>(entity);
    if (mesh.modelPath.empty()) return;

    ModelRenderer* renderer = g_SceneRenderer.GetModelRenderer(SceneCollector::GetModelRendererKey(entity));
    if (renderer == nullptr || !renderer->HasModelLoaded() || !renderer->HasSkinning()) return;

    const auto& bones = renderer->GetMeshData().bones;
    std::vector<glm::mat4> localTransforms;
    localTransforms.reserve(bones.size());
    for (const auto& bone : bones) {
        glm::mat4 local = bone.bindLocalTransform;
        glm::vec3 translation(0.0f);
        glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        if (motion.SampleBone(bone.name, player.currentFrame, translation, rotation)) {
            const glm::mat4 delta = glm::translate(glm::mat4(1.0f), translation) *
                                     glm::mat4_cast(rotation);
            // VMD 的位移/旋转是相对于 PMX 绑定姿态的增量。
            local = bone.bindLocalTransform * delta;
        }
        localTransforms.push_back(local);
    }
    renderer->ApplyBoneLocalPose(localTransforms);
}

void VmdSystem::ApplyToCamera(Entity entity, const VmdPlayerComponent& player,
                              const Animation::VmdMotion& motion) {
    auto& coordinator = Coordinator::GetInstance();
    if (!coordinator.HasComponent<CameraComponent>(entity) ||
        !coordinator.HasComponent<TransformComponent>(entity)) return;

    Animation::VmdCameraKeyframe keyframe;
    if (!motion.SampleCamera(player.currentFrame, keyframe)) return;

    glm::quat rotation = glm::normalize(glm::quat(keyframe.rotation));
    if (!std::isfinite(rotation.w) || !std::isfinite(rotation.x) ||
        !std::isfinite(rotation.y) || !std::isfinite(rotation.z)) {
        rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }

    auto& transform = coordinator.GetComponent<TransformComponent>(entity);
    const float distance = IsFinite(keyframe.distance) ? keyframe.distance : 0.0f;
    transform.position = keyframe.position + rotation * glm::vec3(0.0f, 0.0f, distance);
    transform.rotation = rotation;
    transform.MarkDirty();

    auto& camera = coordinator.GetComponent<CameraComponent>(entity);
    if (IsFinite(keyframe.viewAngle) && keyframe.viewAngle > 0.1f) {
        camera.fov = std::clamp(keyframe.viewAngle, 1.0f, 179.0f);
    }
    camera.isOrthographic = !keyframe.perspective;
    if (camera.isOrthographic) {
        const float halfFov = glm::radians(std::clamp(keyframe.viewAngle, 1.0f, 179.0f)) * 0.5f;
        camera.orthographicSize = std::max(0.001f, std::fabs(distance) * std::tan(halfFov));
    }
}

void VmdSystem::VisitEntity(Entity entity, float deltaTime, std::unordered_set<Entity>& visited) {
    auto& scene = SceneECS::GetInstance();
    auto& coordinator = Coordinator::GetInstance();

    if (coordinator.HasComponent<VmdPlayerComponent>(entity)) {
        visited.insert(entity);
        auto& player = coordinator.GetComponent<VmdPlayerComponent>(entity);
        if (player.enabled && !player.motionPath.empty()) {
            RuntimeState& state = m_runtime[entity];
            if (EnsureMotion(entity, player.motionPath, state) && state.motion) {
                Advance(player, state, deltaTime);

                const bool hasCamera = coordinator.HasComponent<CameraComponent>(entity);
                const bool useCamera = player.target == VmdTarget::Camera ||
                                       (player.target == VmdTarget::Auto && hasCamera);
                const bool useModel = player.target == VmdTarget::Model ||
                                      (player.target == VmdTarget::Auto && !hasCamera);
                if (useCamera) ApplyToCamera(entity, player, *state.motion);
                else if (useModel) ApplyToModel(entity, player, *state.motion);
            }
        }
    }

    for (const Entity child : scene.GetChildren(entity)) {
        VisitEntity(child, deltaTime, visited);
    }
}

void VmdSystem::Update(float deltaTime) {
    auto& scene = SceneECS::GetInstance();
    std::unordered_set<Entity> visited;
    for (const Entity root : scene.GetRootEntities()) {
        VisitEntity(root, deltaTime, visited);
    }
    for (auto it = m_runtime.begin(); it != m_runtime.end();) {
        if (visited.find(it->first) == visited.end()) it = m_runtime.erase(it);
        else ++it;
    }
}

} // namespace ECS
