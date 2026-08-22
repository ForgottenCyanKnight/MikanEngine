#pragma once

#include "Platform/Export.h"
#include "ECS/Types.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <unordered_set>

namespace ECS {
struct CameraComponent;
struct TransformComponent;
}

// 基础 3D 第三人称相机。
// CameraComponent 保存可序列化配置，本系统保存帧间运行时状态：
//   - 按目标实体名平滑跟随
//   - 围绕目标水平旋转/上下俯仰
//   - 鼠标滚轮调整距离
//   - 可选持续捕获鼠标，否则按住鼠标右键环绕
//   - 按住 Shift 进入瞄准：右肩偏移、瞄准点锁定、FOV 平滑收窄
//   - 按 Q 锁定带 LockOnTargetComponent 的敌人，同时保持玩家跟随
class MIKAN_API ThirdPersonCameraSystem {
public:
    static ThirdPersonCameraSystem& GetInstance();

    // 在玩法/脚本更新之后调用，写回主相机实体 Transform。
    void Update(float dt);

    // 场景切换或退出时清除上一场景的相机状态。
    void Reset();
    void SetSceneContext(const std::string& scenePath);

    // 供 HUD/玩法层读取当前瞄准状态(例如显示准星或切换武器姿态)。
    bool IsAiming() const { return m_Runtime.aiming; }
    bool IsLockOnActive() const { return m_Runtime.lockOn; }
    ECS::Entity GetLockTarget() const { return m_Runtime.lockTarget; }
    void ClearLockOn();

private:
    ThirdPersonCameraSystem() = default;
    ~ThirdPersonCameraSystem() = default;
    ThirdPersonCameraSystem(const ThirdPersonCameraSystem&) = delete;
    ThirdPersonCameraSystem& operator=(const ThirdPersonCameraSystem&) = delete;

    struct RuntimeState {
        ECS::Entity camera = ECS::INVALID_ENTITY;
        ECS::Entity target = ECS::INVALID_ENTITY;
        uint32_t sceneVersion = 0;
        glm::vec3 position = glm::vec3(0.0f);
        glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        float yaw = 180.0f;
        float pitch = 15.0f;
        float distance = 5.0f;
        // 碰撞状态独立于用户轨道角度：相机只调整位置，不反写 yaw，
        // 并在障碍物短暂消失时保持最近安全位置，避免角点吸附/跳变。
        glm::vec3 collisionLastSafePosition = glm::vec3(0.0f);
        float collisionClearTime = 0.0f;
        float collisionSettleTime = 0.0f;
        bool collisionActive = false;
        bool collisionStateValid = false;
        float baseFov = 60.0f;
        bool aiming = false;
        bool lockOn = false;
        ECS::Entity lockTarget = ECS::INVALID_ENTITY;
        bool initialized = false;
    };

    ECS::Entity FindMainCamera() const;
    ECS::Entity ResolveTarget(const ECS::CameraComponent& camera) const;
    ECS::Entity FindLockTarget(ECS::Entity followTarget, ECS::Entity cameraEntity,
                               const ECS::CameraComponent& camera) const;
    bool IsValidLockTarget(ECS::Entity entity, ECS::Entity followTarget,
                           const ECS::CameraComponent& camera) const;
    glm::vec3 ResolveCameraCollisionPosition(const glm::vec3& pivot,
                                             const glm::vec3& desiredPosition,
                                             ECS::Entity followTarget,
                                             ECS::Entity cameraEntity,
                                             const ECS::CameraComponent& camera,
                                             float* outSafeDistance = nullptr) const;
    void DiagnoseMissingTarget(ECS::Entity camera, const ECS::CameraComponent& component);
    void ReleaseOwnedMouseCapture();

    RuntimeState m_Runtime;
    std::string m_SceneContext = "<unknown>";
    std::unordered_set<std::string> m_MissingTargetDiagnostics;
    bool m_MouseCaptureOwned = false;
    bool m_LockButtonWasDown = false;
};
