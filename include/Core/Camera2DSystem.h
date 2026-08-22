#pragma once
// Camera2DSystem.h - Cinemachine 风格 2D 智能相机系统(序5)
// 每帧(Canvas2D::SyncCameraFromScene 之前)遍历场景, 对带 Camera2DComponent 且 enabled 的实体:
//   - 解析 followTargetName → followTarget(实体名懒解析, 支持运行中改目标)
//   - 目标位置 = 目标 Transform.position + followOffset + lookahead × 目标速度(速度前视)
//   - 帧率无关指数阻尼(dampX/dampY)平滑到目标 → 写回 component.center
//   - confiner 边界限制(useBounds + boundMin/boundMax, 视野边缘按视口/zoom 收窄)
//   - 屏幕震动(ShakeCamera 触发, 伪噪声 × 时长比例衰减, 叠加在 center 之上)
// Canvas2D::SyncCameraFromScene 随后读取最终 center/zoom 应用(本系统不渲染)。
#include "Platform/Export.h"
#include "ECS/Types.h"
#include <glm/glm.hpp>
#include <string>
#include <unordered_set>

namespace ECS { struct Camera2DComponent; }

class MIKAN_API Camera2DSystem {
public:
    static Camera2DSystem& GetInstance();

    // 视口(世界层像素尺寸; confiner 视野边缘收窄用); 引擎每帧/窗口尺寸变化时更新
    void SetViewport(uint32_t width, uint32_t height);

    // 每帧在 Canvas2D::SyncCameraFromScene 之前调用
    void Update(float dt);

    // 场景树中的 active 相机契约：按 root/child 顺序选择第一个 enabled 相机。
    // Canvas2D 与玩法层均通过此接口消费，避免各自实现不同的选择规则。
    ECS::Entity GetActiveCamera();

    // 场景加载/播放边界：清除旧场景的运行时跟随、震动和阻尼状态，随后重新绑定目标。
    void Reset();
    void Rebind();
    void SetSceneContext(const std::string& scenePath);

    // 屏幕震动: 作用于指定相机实体(INVALID = 第一个 enabled 相机); strength=像素振幅, duration=秒
    void ShakeCamera(ECS::Entity e, float strength, float duration);
    void ShakeCamera(float strength, float duration); // 作用于第一个 enabled 相机

private:
    Camera2DSystem() = default;
    ~Camera2DSystem() = default;
    Camera2DSystem(const Camera2DSystem&) = delete;
    Camera2DSystem& operator=(const Camera2DSystem&) = delete;

    void VisitEntity(ECS::Entity e, float dt);
    void UpdateCamera(ECS::Entity e, ECS::Camera2DComponent& c, float dt);
    void ResolveTarget(ECS::Entity e, ECS::Camera2DComponent& c);
    ECS::Entity FindFirstEnabledCamera();
    void ResetCameraRuntime(ECS::Camera2DComponent& c);
    void DiagnoseMissingTarget(ECS::Entity e, const ECS::Camera2DComponent& c);

    uint32_t m_ViewportWidth = 1920;
    uint32_t m_ViewportHeight = 1080;
    ECS::Entity m_ActiveCamera = ECS::INVALID_ENTITY;
    std::string m_SceneContext = "<unknown>";
    std::unordered_set<std::string> m_MissingTargetDiagnostics;
};
