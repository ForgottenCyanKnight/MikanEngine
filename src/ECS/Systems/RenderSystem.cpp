#include "ECS/Systems/RenderSystem.h"
#include "ECS/ECS.h"

namespace ECS {

void RenderSystem::Update(float deltaTime) {
    // 可以在这里做视锥体剔除等预处理
}

void RenderSystem::Render(VkCommandBuffer commandBuffer) {
    auto& coordinator = Coordinator::GetInstance();

    // 获取相机信息
    glm::mat4 viewMatrix = glm::mat4(1.0f);
    glm::mat4 projMatrix = glm::mat4(1.0f);

    if (m_CameraEntity != INVALID_ENTITY &&
        coordinator.HasComponent<TransformComponent>(m_CameraEntity) &&
        coordinator.HasComponent<CameraComponent>(m_CameraEntity)) {

        auto& cameraTransform = coordinator.GetComponent<TransformComponent>(m_CameraEntity);
        auto& camera = coordinator.GetComponent<CameraComponent>(m_CameraEntity);

        // TODO: 获取实际的宽高比
        float aspectRatio = 16.0f / 9.0f;
        projMatrix = camera.GetProjectionMatrix(aspectRatio);
        viewMatrix = camera.GetViewMatrix(cameraTransform.position, cameraTransform.rotation);
    }

    // 渲染所有有 Transform + Mesh + Render 组件的实体
    for (auto entity : m_Entities) {
        // 双重检查确保实体有效
        if (entity == INVALID_ENTITY) {
            continue;
        }
        
        if (!coordinator.HasComponent<TransformComponent>(entity) ||
            !coordinator.HasComponent<MeshComponent>(entity) ||
            !coordinator.HasComponent<RenderComponent>(entity)) {
            continue;
        }

        auto& render = coordinator.GetComponent<RenderComponent>(entity);
        if (!render.visible) continue;

        auto& transform = coordinator.GetComponent<TransformComponent>(entity);
        auto& mesh = coordinator.GetComponent<MeshComponent>(entity);

        // 检查 mesh 是否有效
        if (mesh.modelPath.empty()) {
            continue;
        }

        glm::mat4 modelMatrix = transform.GetModelMatrix();

        // TODO: 实际的渲染调用
        // 这里应该根据 mesh.type 调用对应的渲染逻辑
        // 例如：g_SceneRenderer.RenderMesh(commandBuffer, mesh.type, modelMatrix, viewMatrix, projMatrix);
    }
}

} // namespace ECS
