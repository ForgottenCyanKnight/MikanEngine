// SceneDebugRenderer.cpp - editor/debug visualization (AABB/OBB/BVH wireframes)
#include "Rendering/SceneDebugRenderer.h"
#include "Rendering/SceneRenderer.h"   // ModelInstanceGroup / VoxInstanceGroup definitions
#include "Rendering/ModelRenderer.h"
#include "Rendering/VoxRenderer.h"
#include "ECS/SceneECS.h"
#include "ECS/Components.h"
#include "AABB.h"

// Collect all camera entities under an entity subtree (helper for BVH wireframe visibility)
void SceneDebugRenderer::CollectCameraEntities(ECS::Entity entity, std::vector<ECS::Entity>& cameraEntities)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    if (coordinator.HasComponent<ECS::CameraComponent>(entity))
        cameraEntities.push_back(entity);

    auto children = sceneECS.GetChildren(entity);
    for (const auto& child : children)
        CollectCameraEntities(child, cameraEntities);
}

void SceneDebugRenderer::CollectAABBs(ECS::Entity entity,
                                      const std::unordered_map<std::string, ModelInstanceGroup>& modelGroups,
                                      const std::unordered_map<std::string, std::unique_ptr<ModelRenderer>>& modelRenderers,
                                      const std::unordered_map<std::string, std::unique_ptr<VoxRenderer>>& voxRenderers)
{
    (void)modelGroups; (void)voxRenderers;
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    if (coordinator.HasComponent<ECS::TransformComponent>(entity) &&
        coordinator.HasComponent<ECS::MeshComponent>(entity) &&
        coordinator.HasComponent<ECS::RenderComponent>(entity)) {

        auto& render = coordinator.GetComponent<ECS::RenderComponent>(entity);
        if (render.visible && (render.showAABB || render.showOBB)) {
            auto& mesh = coordinator.GetComponent<ECS::MeshComponent>(entity);
            auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);

            if ((mesh.type == ECS::MeshType::Model || mesh.type == ECS::MeshType::Plane) && !mesh.modelPath.empty()) {
                auto rendererIt = modelRenderers.find(mesh.modelPath);
                if (rendererIt != modelRenderers.end() && rendererIt->second && rendererIt->second->HasModelLoaded()) {
                    std::vector<AABB> subMeshAABBs = rendererIt->second->GetSubMeshAABBs();
                    glm::mat4 modelMatrix = transform.GetModelMatrix();

                    if (render.showOBB) {
                        for (const auto& localAABB : subMeshAABBs)
                            m_WireframeRenderer.AddOBBFromMatrix(localAABB, modelMatrix, glm::vec3(0.0f, 1.0f, 0.0f)); // green
                    } else if (render.showAABB) {
                        for (const auto& localAABB : subMeshAABBs) {
                            AABB worldAABB = localAABB.Transform(modelMatrix);
                            m_WireframeRenderer.AddAABB(worldAABB, glm::vec3(0.0f, 1.0f, 0.0f)); // green
                        }
                    }
                }
            }
        }
    }

    auto children = sceneECS.GetChildren(entity);
    for (const auto& child : children)
        CollectAABBs(child, modelGroups, modelRenderers, voxRenderers);
}

void SceneDebugRenderer::CollectVoxAABBs(ECS::Entity entity,
                                         const std::unordered_map<std::string, std::unique_ptr<VoxRenderer>>& voxRenderers)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    if (coordinator.HasComponent<ECS::TransformComponent>(entity) &&
        coordinator.HasComponent<ECS::VoxModelComponent>(entity) &&
        coordinator.HasComponent<ECS::RenderComponent>(entity)) {

        auto& render = coordinator.GetComponent<ECS::RenderComponent>(entity);
        if (render.visible && (render.showAABB || render.showOBB)) {
            auto& vox = coordinator.GetComponent<ECS::VoxModelComponent>(entity);
            auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);

            if (!vox.voxPath.empty()) {
                auto rendererIt = voxRenderers.find(vox.voxPath);
                if (rendererIt != voxRenderers.end() && rendererIt->second && rendererIt->second->HasLoaded()) {
                    auto& voxRenderer = rendererIt->second;
                    glm::vec3 minBounds = voxRenderer->GetMinBounds();
                    glm::vec3 maxBounds = voxRenderer->GetMaxBounds();

                    AABB localAABB;
                    localAABB.min = minBounds;
                    localAABB.max = maxBounds;

                    glm::mat4 modelMatrix = transform.GetModelMatrix();

                    if (render.showOBB) {
                        m_WireframeRenderer.AddOBBFromMatrix(localAABB, modelMatrix, glm::vec3(1.0f, 0.5f, 0.0f)); // orange
                    } else if (render.showAABB) {
                        AABB worldAABB = localAABB.Transform(modelMatrix);
                        m_WireframeRenderer.AddAABB(worldAABB, glm::vec3(1.0f, 0.5f, 0.0f)); // orange
                    }
                }
            }
        }
    }

    auto children = sceneECS.GetChildren(entity);
    for (const auto& child : children)
        CollectVoxAABBs(child, voxRenderers);
}

void SceneDebugRenderer::CollectBVH(ECS::Entity entity, const glm::vec3& cameraPos,
                                    const glm::mat4& effectiveCullView, const glm::mat4& effectiveCullProj,
                                    const std::unordered_map<std::string, std::unique_ptr<ModelRenderer>>& modelRenderers,
                                    const std::unordered_map<std::string, std::unique_ptr<VoxRenderer>>& voxRenderers)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    std::vector<ECS::Entity> cameraEntities;
    for (const auto& rootEntity : sceneECS.GetRootEntities())
        CollectCameraEntities(rootEntity, cameraEntities);

    bool showBVH = false;
    for (const auto& camEntity : cameraEntities) {
        auto& camera = coordinator.GetComponent<ECS::CameraComponent>(camEntity);
        if (camera.showBVHWireframe) { showBVH = true; break; }
    }
    if (!showBVH) return;

    if (coordinator.HasComponent<ECS::TransformComponent>(entity) &&
        coordinator.HasComponent<ECS::MeshComponent>(entity) &&
        coordinator.HasComponent<ECS::RenderComponent>(entity)) {

        auto& render = coordinator.GetComponent<ECS::RenderComponent>(entity);
        if (render.visible) {
            auto& mesh = coordinator.GetComponent<ECS::MeshComponent>(entity);
            auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);

            if ((mesh.type == ECS::MeshType::Model || mesh.type == ECS::MeshType::Plane) && !mesh.modelPath.empty()) {
                auto rendererIt = modelRenderers.find(mesh.modelPath);
                if (rendererIt != modelRenderers.end() && rendererIt->second && rendererIt->second->HasBVH()) {
                    glm::mat4 modelMatrix = transform.GetModelMatrix();

                    if (rendererIt->second->HasTopLevelBVH()) {
                        std::vector<AABB> topLevelBounds = rendererIt->second->GetTopLevelBVHNodeBounds();
                        for (const auto& localAABB : topLevelBounds)
                            m_WireframeRenderer.AddOBBFromMatrix(localAABB, modelMatrix, glm::vec3(1.0f, 0.8f, 0.0f)); // yellow
                    }

                    const auto& bvhData = rendererIt->second->GetBVHData();
                    size_t blasCount = bvhData.GetSubMeshCount();
                    for (size_t blasIdx = 0; blasIdx < blasCount; blasIdx++) {
                        std::vector<AABB> bvhBounds = rendererIt->second->GetBVHNodeBounds(blasIdx);
                        for (const auto& localAABB : bvhBounds)
                            m_WireframeRenderer.AddOBBFromMatrix(localAABB, modelMatrix, glm::vec3(0.0f, 0.5f, 1.0f)); // cyan
                    }
                }
            }
        }
    }

    // VoxRenderer BVH (green, depth-shaded)
    if (coordinator.HasComponent<ECS::TransformComponent>(entity) &&
        coordinator.HasComponent<ECS::VoxModelComponent>(entity) &&
        coordinator.HasComponent<ECS::RenderComponent>(entity)) {

        auto& render = coordinator.GetComponent<ECS::RenderComponent>(entity);
        if (render.visible) {
            auto& vox = coordinator.GetComponent<ECS::VoxModelComponent>(entity);
            auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);

            auto rendererIt = voxRenderers.find(vox.voxPath);
            if (rendererIt != voxRenderers.end() && rendererIt->second && rendererIt->second->HasBVH()) {
                glm::mat4 modelMatrix = transform.GetModelMatrix();
                const auto& bvh = rendererIt->second->GetBVH();
                const auto& nodes = bvh.getNodes();

                for (const auto& node : nodes) {
                    glm::vec3 center = (node.boundsMin + node.boundsMax) * 0.5f;
                    glm::vec3 halfSize = (node.boundsMax - node.boundsMin) * 0.5f;
                    center.z = -center.z; // BVH vs vox model Z convention flip

                    AABB localAABB(center - halfSize, center + halfSize);
                    float depthFactor = 1.0f - (node.depth / 30.0f);
                    glm::vec3 color = glm::vec3(0.0f, depthFactor * 0.8f, depthFactor * 0.3f);
                    m_WireframeRenderer.AddOBBFromMatrix(localAABB, modelMatrix, color);
                }
            }
        }
    }

    auto children = sceneECS.GetChildren(entity);
    for (const auto& child : children)
        CollectBVH(child, cameraPos, effectiveCullView, effectiveCullProj, modelRenderers, voxRenderers);
}