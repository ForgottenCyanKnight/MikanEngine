#pragma once
// SceneDebugRenderer.h - editor/debug visualization (AABB/OBB/BVH wireframes)
// Split out of SceneRenderer so the main renderer stays focused on rendering.
#include "Platform/Export.h"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <unordered_map>
#include <memory>
#include <vector>
#include "WireframeRenderer.h"
#include "ECS/Types.h"

class ModelRenderer;
class VoxRenderer;
struct RenderWorld;
struct ModelInstanceGroup;
struct VoxInstanceGroup;

class MIKAN_API SceneDebugRenderer {
public:
    void Init(VkRenderPass renderPass) { (void)renderPass; }
    void Cleanup() { m_WireframeRenderer.Cleanup(); }

    void ClearInstances() { m_WireframeRenderer.ClearInstances(); }
    void ClearFrustums()  { m_WireframeRenderer.ClearFrustums(); }
    bool HasInstances() const { return m_WireframeRenderer.HasInstances(); }
    void Render(VkCommandBuffer cmd, const glm::mat4& view, const glm::mat4& proj) { m_WireframeRenderer.Render(cmd, view, proj); }
    WireframeRenderer& GetWireframeRenderer() { return m_WireframeRenderer; }

    // Collect AABB / OBB wireframes for entities flagged showAABB/showOBB
    void CollectAABBs(ECS::Entity entity, const std::unordered_map<std::string, ModelInstanceGroup>& modelGroups,
                      const std::unordered_map<std::string, std::unique_ptr<ModelRenderer>>& modelRenderers,
                      const std::unordered_map<std::string, std::unique_ptr<VoxRenderer>>& voxRenderers);
    void CollectAABBs(const RenderWorld& world,
                      const std::unordered_map<std::string, std::unique_ptr<ModelRenderer>>& modelRenderers);
    void CollectVoxAABBs(ECS::Entity entity,
                         const std::unordered_map<std::string, std::unique_ptr<VoxRenderer>>& voxRenderers);
    void CollectVoxAABBs(const RenderWorld& world,
                         const std::unordered_map<std::string, std::unique_ptr<VoxRenderer>>& voxRenderers);
    // Collect BVH wireframes (top-level yellow, BLAS cyan) when a camera has showBVHWireframe
    void CollectBVH(ECS::Entity entity, const glm::vec3& cameraPos,
                    const glm::mat4& effectiveCullView, const glm::mat4& effectiveCullProj,
                    const std::unordered_map<std::string, std::unique_ptr<ModelRenderer>>& modelRenderers,
                    const std::unordered_map<std::string, std::unique_ptr<VoxRenderer>>& voxRenderers);
    void CollectBVH(const RenderWorld& world, const glm::vec3& cameraPos,
                    const glm::mat4& effectiveCullView, const glm::mat4& effectiveCullProj,
                    const std::unordered_map<std::string, std::unique_ptr<ModelRenderer>>& modelRenderers,
                    const std::unordered_map<std::string, std::unique_ptr<VoxRenderer>>& voxRenderers);
    // Collect all 3D collision shapes when any camera enables the global inspector flag.
    // The camera property is intentionally global, matching showBVHWireframe semantics.
    void CollectCollisionWireframes(
        const std::vector<ECS::Entity>& cameraEntities,
        const std::unordered_map<std::string, std::unique_ptr<ModelRenderer>>& modelRenderers);
    void CollectCollisionWireframes(
        const RenderWorld& world,
        const std::unordered_map<std::string, std::unique_ptr<ModelRenderer>>& modelRenderers);

private:
    void CollectCameraEntities(ECS::Entity entity, std::vector<ECS::Entity>& cameraEntities);
    WireframeRenderer m_WireframeRenderer;
};
