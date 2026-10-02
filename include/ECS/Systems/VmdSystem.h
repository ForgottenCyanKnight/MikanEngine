#pragma once

#include "Platform/Export.h"
#include "Animation/VmdMotion.h"
#include "Animation/MmdRuntime.h"
#include "ECS/Types.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace ECS {

struct VmdPlayerComponent;

// VMD 运行时适配层：负责把独立的 Animation::VmdMotion 绑定到 ECS 实体。
// 组件挂在 PMX/PMD 上时驱动 ModelRenderer，挂在相机上时驱动 Transform/Camera。
class MIKAN_API VmdSystem {
public:
    static VmdSystem& GetInstance();

    void Update(float deltaTime);
    void Clear();

private:
    struct RuntimeState {
        std::shared_ptr<Animation::VmdMotion> motion;
        std::string resolvedPath;
        std::string sourceCameraPath;
        std::string loadError;
        float lastStartFrame = 0.0f;
        bool loadAttempted = false;
        bool initialized = false;
        std::shared_ptr<Animation::MmdRuntime> mmd;
        std::string modelPath, mmdMotionPath, mmdFaceMotionPath;
        std::string sourceModelPath, sourceMotionPath, sourceFaceMotionPath;
        uint64_t lastVisitTick = 0;
        bool mmdLoadAttempted = false;
        float lastMmdFrame = -1.0f;
        std::vector<glm::vec3> mmdPositions, mmdNormals;
        std::vector<glm::vec2> mmdUvs;
        const void* mmdRenderer = nullptr;
        uint64_t appliedPoseRevision = 0;
        glm::vec3 cameraStageOffset{0.0f};
        bool cameraFollowTimelineSource = false;
    };

    VmdSystem() = default;
    ~VmdSystem() = default;
    VmdSystem(const VmdSystem&) = delete;
    VmdSystem& operator=(const VmdSystem&) = delete;

    void VisitEntity(Entity entity, float deltaTime);
    bool EnsureMotion(Entity entity, const std::string& path, RuntimeState& state);
    void Advance(VmdPlayerComponent& player, RuntimeState& state, float deltaTime);
    void ApplyToModel(Entity entity, const VmdPlayerComponent& player,
                      const Animation::VmdMotion& motion);
    void ApplyToCamera(Entity entity, const VmdPlayerComponent& player,
                       const Animation::VmdMotion& motion);
    bool UpdateMmd(Entity entity, VmdPlayerComponent* player, RuntimeState& state, float deltaTime);

    std::unordered_map<Entity, RuntimeState> m_runtime;
    uint64_t m_visitTick = 0;
    std::string m_projectRoot, m_resourceRoot, m_engineRoot;
};

} // namespace ECS
