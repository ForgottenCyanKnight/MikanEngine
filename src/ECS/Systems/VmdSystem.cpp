#define GLM_ENABLE_EXPERIMENTAL
#include "ECS/Systems/VmdSystem.h"

#include "Core/ProjectManager.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"
#include "Rendering/ModelRenderer.h"
#include "Rendering/SceneCollector.h"
#include "Rendering/SceneRenderer.h"
#include "Core/Log.h"
#include "Core/RuntimeCapabilities.h"
#include "Core/Utf8Path.h"
#include "Animation/MmdPlaybackClock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
#include <fstream>
#include "json.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

// SceneRenderer 的实际实例由 EngineGlobals.cpp 提供；不要调用
// SceneRenderer::GetInstance()，否则会产生一个未初始化的第二个渲染器实例。
extern SceneRenderer g_SceneRenderer;

namespace ECS {
namespace {

constexpr float kVmdFramesPerSecond = 30.0f;
Animation::MmdPlaybackClock s_PlaybackClock;

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
    s_PlaybackClock.Reset();
}

bool VmdSystem::EnsureMotion(Entity entity, const std::string& path, RuntimeState& state) {
    const std::string resolvedPath = ProjectManager::GetInstance().ResolveAssetPath(path);
    if (state.resolvedPath != resolvedPath) {
        state.motion.reset();
        state.loadAttempted = false;
        state.initialized = false;
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
        LOGE("[VmdSystem] entity=%u failed to load '%s': %s",
                     static_cast<unsigned>(entity), resolvedPath.c_str(), error.c_str());
        return false;
    }
    if (motion->Empty()) {
        state.loadError = "motion contains no bone or camera frames";
        LOGE("[VmdSystem] entity=%u motion '%s' is empty",
                     static_cast<unsigned>(entity), resolvedPath.c_str());
        return false;
    }

    state.motion = std::move(motion);
    state.cameraStageOffset=glm::vec3(0);
    state.cameraFollowTimelineSource=false;
    if(state.motion->HasCameraTrack()) {
        std::ifstream input(Utf8Path(resolvedPath+".import.json"));
        if(input) {
            try {
                const auto settings=nlohmann::json::parse(input);
                const auto offset=settings.value("cameraStageOffset",std::vector<float>{0,0,0});
                if(offset.size()!=3 || !IsFinite(offset[0]) || !IsFinite(offset[1]) || !IsFinite(offset[2]))
                    throw std::runtime_error("cameraStageOffset must contain three finite coordinates");
                state.cameraStageOffset=glm::vec3(offset[0],offset[1],offset[2]);
                state.cameraFollowTimelineSource=settings.value("cameraFollowTimelineSource",false);
            } catch(const std::exception& error) {
                state.cameraStageOffset=glm::vec3(0);state.cameraFollowTimelineSource=false;
                LOGW("[VmdSystem] invalid camera import settings: %s",error.what());
            }
        }
    }
    LOGI("[VmdSystem] entity=%u loaded '%s' (bones=%zu cameras=%zu lastFrame=%.0f)",
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

    ModelRenderer* renderer = g_SceneRenderer.GetModelRendererForKey(SceneCollector::GetModelRendererKey(entity));
    if (renderer == nullptr || !renderer->HasSkinning()) return;

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

    // MMD cameras orbit the interest point in Y / negative-Z / X order.
    const auto matrix=glm::rotate(glm::mat4(1),keyframe.rotation.y,glm::vec3(0,1,0)) *
        glm::rotate(glm::mat4(1),keyframe.rotation.z,glm::vec3(0,0,-1)) *
        glm::rotate(glm::mat4(1),keyframe.rotation.x,glm::vec3(1,0,0));
    glm::quat rotation = glm::normalize(glm::quat_cast(matrix));
    if (!std::isfinite(rotation.w) || !std::isfinite(rotation.x) ||
        !std::isfinite(rotation.y) || !std::isfinite(rotation.z)) {
        rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }

    auto& transform = coordinator.GetComponent<TransformComponent>(entity);
    const float distance = IsFinite(keyframe.distance) ? keyframe.distance : 0.0f;
    glm::vec3 interest(keyframe.position.x,keyframe.position.y,-keyframe.position.z);
    const auto& state=m_runtime.at(entity);
    interest+=state.cameraStageOffset;
    if(state.cameraFollowTimelineSource && !player.timelineSource.empty()) {
        const Entity source=SceneECS::GetInstance().FindByName(player.timelineSource);
        if(source!=INVALID_ENTITY && coordinator.HasComponent<TransformComponent>(source)) {
            interest+=glm::vec3(SceneECS::GetInstance().GetWorldMatrix(source)[3]);
        }
    }
    transform.position = interest + rotation * glm::vec3(0.0f, 0.0f, std::fabs(distance));
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

bool VmdSystem::UpdateMmd(Entity entity, VmdPlayerComponent* player, RuntimeState& state, float deltaTime) {
    auto& coordinator = Coordinator::GetInstance();
    if (!coordinator.HasComponent<MeshComponent>(entity)) return false;
    const auto modelPath = ProjectManager::GetInstance().ResolveAssetPath(
        coordinator.GetComponent<MeshComponent>(entity).modelPath);
    auto extension = Utf8String(Utf8Path(modelPath).extension());
    std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if (extension != ".pmx") return false;
    const auto motionPath = player && !player->motionPath.empty()
        ? ProjectManager::GetInstance().ResolveAssetPath(player->motionPath) : std::string();
    const auto faceMotionPath = player && !player->faceMotionPath.empty()
        ? ProjectManager::GetInstance().ResolveAssetPath(player->faceMotionPath) : std::string();
    if (state.modelPath != modelPath || state.mmdMotionPath != motionPath || state.mmdFaceMotionPath != faceMotionPath) {
        state.mmd.reset(); state.mmdLoadAttempted=false; state.initialized=false;
        state.modelPath=modelPath; state.mmdMotionPath=motionPath; state.mmdFaceMotionPath=faceMotionPath;state.lastMmdFrame=-1;
    }
    if (!state.mmdLoadAttempted) {
        state.mmdLoadAttempted=true;
        auto model=std::make_shared<Animation::MmdRuntime>();
        if (!model->Load(modelPath,motionPath,faceMotionPath)) {
            LOGE("[MMD/Jolt] entity=%u failed to load '%s' motion='%s'",entity,modelPath.c_str(),motionPath.c_str());
            return true;
        }
        state.mmd=std::move(model);
        LOGI("[MMD/Jolt] entity=%u bones=%zu morphs=%zu lastFrame=%.0f",entity,state.mmd->GetBoneCount(),state.mmd->GetMorphCount(),state.mmd->GetLastFrame());
    }
    if (!state.mmd) return true;
    float frame=0;
    if (player) {
        const float end=state.mmd->GetLastFrame();
        const float start=SafeFrame(player->startFrame,end);
        if (!state.initialized || state.lastStartFrame!=start) {
            player->currentFrame=start; state.initialized=true; state.lastStartFrame=start;
        }
        const float dt=std::isfinite(deltaTime)?std::max(0.0f,deltaTime):0;
        const float speed=std::isfinite(player->speed)?player->speed:0;
        frame=SafeFrame(player->currentFrame,end);
        if (player->playing) frame+=dt*30*speed;
        if (player->loop && end>start && (frame>end || frame<start)) {
            frame=start+std::fmod(frame-start,end-start); if(frame<start) frame+=end-start;
        } else if(frame>end || frame<start) { frame=std::clamp(frame,start,end);player->playing=false; }
        player->currentFrame=frame;
    }
    // A seek, reverse playback or loop must not retain the old simulation state.
    const bool seek=state.lastMmdFrame>=0 && (frame<state.lastMmdFrame || std::fabs(frame-state.lastMmdFrame)>15);
    const bool simulate=(!player || player->physicsEnabled);
    state.mmd->Update(frame,(seek || (player && !player->playing))?0:deltaTime,simulate && !seek);
    state.lastMmdFrame=frame;
    if (Core::GetRuntimeCapabilities().rendering) {
        auto* renderer=g_SceneRenderer.GetModelRendererForKey(SceneCollector::GetModelRendererKey(entity));
        if (renderer) {
            std::vector<glm::vec3> positions,normals;std::vector<glm::vec2> uvs;
            state.mmd->GetVertices(positions,normals,uvs);
            renderer->ApplyMmdMaterials(state.mmd->GetMaterials());
            if(!renderer->ApplyMmdVertices(positions,normals,uvs)) {
                static bool reported=false;
                if(!reported) { LOGE("[MMD/Jolt] vertex upload failed: isMmd=%d vertices=%zu",renderer->GetMeshData().isMmd?1:0,positions.size());reported=true; }
            }
        }
    }
    return true;
}

void VmdSystem::VisitEntity(Entity entity, float deltaTime, std::unordered_set<Entity>& visited) {
    auto& scene = SceneECS::GetInstance();
    auto& coordinator = Coordinator::GetInstance();

    if (coordinator.HasComponent<VmdPlayerComponent>(entity)) {
        visited.insert(entity);
        auto& player = coordinator.GetComponent<VmdPlayerComponent>(entity);
        if (player.enabled && !player.motionPath.empty()) {
            RuntimeState& state = m_runtime[entity];
            const bool modelTarget=player.target==VmdTarget::Model ||
                (player.target==VmdTarget::Auto && !coordinator.HasComponent<CameraComponent>(entity));
            if (modelTarget && UpdateMmd(entity,&player,state,deltaTime)) {
                // PMX uses the complete MMD evaluation pipeline.
            } else if (EnsureMotion(entity, player.motionPath, state) && state.motion) {
                if(player.timelineSource.empty()) Advance(player, state, deltaTime);

                const bool hasCamera = coordinator.HasComponent<CameraComponent>(entity);
                const bool useCamera = player.target == VmdTarget::Camera ||
                                       (player.target == VmdTarget::Auto && hasCamera);
                const bool useModel = player.target == VmdTarget::Model ||
                                      (player.target == VmdTarget::Auto && !hasCamera);
                if (useCamera && player.timelineSource.empty()) ApplyToCamera(entity, player, *state.motion);
                else if (useModel) ApplyToModel(entity, player, *state.motion);
            }
        }
        else if (player.enabled && player.motionPath.empty()) {
            UpdateMmd(entity,&player,m_runtime[entity],deltaTime);
        }
    } else if (coordinator.HasComponent<MeshComponent>(entity)) {
        const auto& path=coordinator.GetComponent<MeshComponent>(entity).modelPath;
        auto ext=Utf8String(Utf8Path(path).extension());
        std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        if(ext==".pmx") { visited.insert(entity); UpdateMmd(entity,nullptr,m_runtime[entity],deltaTime); }
    }

    for (const Entity child : scene.GetChildren(entity)) {
        VisitEntity(child, deltaTime, visited);
    }
}

void VmdSystem::Update(float deltaTime) {
    const float playbackTime = s_PlaybackClock.Advance(deltaTime);
    // Still initialize/refresh paused poses, but never advance per render frame.
    if (deltaTime > 0.0f && playbackTime == 0.0f && !m_runtime.empty()) return;
    deltaTime = playbackTime;
    auto& scene = SceneECS::GetInstance();
    std::unordered_set<Entity> visited;
    for (const Entity root : scene.GetRootEntities()) {
        VisitEntity(root, deltaTime, visited);
    }
    // Sample synchronized cameras only after every model advanced this frame.
    auto& coordinator=Coordinator::GetInstance();
    for(auto& [entity,state]:m_runtime) {
        if(!state.motion || !state.motion->HasCameraTrack() ||
            !coordinator.HasComponent<CameraComponent>(entity) ||
            !coordinator.HasComponent<VmdPlayerComponent>(entity)) continue;
        auto& player=coordinator.GetComponent<VmdPlayerComponent>(entity);
        if(!player.enabled || player.timelineSource.empty()) continue;
        const auto source=scene.FindByName(player.timelineSource);
        if(source==INVALID_ENTITY || !coordinator.HasComponent<VmdPlayerComponent>(source)) continue;
        if(player.playing) player.currentFrame=coordinator.GetComponent<VmdPlayerComponent>(source).currentFrame;
        ApplyToCamera(entity,player,*state.motion);
    }
    for (auto it = m_runtime.begin(); it != m_runtime.end();) {
        if (visited.find(it->first) == visited.end()) it = m_runtime.erase(it);
        else ++it;
    }
}

} // namespace ECS
