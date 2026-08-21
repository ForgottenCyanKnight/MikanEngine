#pragma once

#include "ECS/System.h"
#include "ECS/Components.h"
#include <vulkan/vulkan.h>

namespace ECS {

// 渲染系统 - 处理所有需要渲染的实体
class RenderSystem : public System {
public:
    void Update(float deltaTime) override;
    void Render(VkCommandBuffer commandBuffer);

    // 设置相机实体（必须有 TransformComponent 和 CameraComponent）
    void SetCameraEntity(Entity cameraEntity) { m_CameraEntity = cameraEntity; }
    Entity GetCameraEntity() const { return m_CameraEntity; }

private:
    Entity m_CameraEntity = INVALID_ENTITY;
};

} // namespace ECS
