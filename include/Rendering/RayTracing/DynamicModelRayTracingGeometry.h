#pragma once
#include "Rendering/RayTracing/ModelRayTracingGeometry.h"
#include "Rendering/ModelLoader.h"

// Dynamic geometry is per pose renderer AND submission slot. Never compact,
// share with static assets, or release scratch needed for subsequent refits.
class DynamicModelRayTracingGeometry final : public ModelRayTracingGeometry {
public:
    bool IsDynamic() const override { return true; }
    VkDeviceAddress PreviousVertexAddress() const override { return previousVertices.GetDeviceAddress(); }
    std::span<const Vertex> DeformedVertices() const override { return currentCpu?std::span<const Vertex>(*currentCpu):std::span<const Vertex>{}; }
    bool GetDeformedBounds(glm::vec3& lo,glm::vec3& hi) const override { lo=boundsMin;hi=boundsMax;return currentCpu!=nullptr; }
    void ReleaseBuildInputs() override {} // persistent upload buffers + update scratch
    bool MatchesDynamic(ModelRenderer& renderer,uint32_t subMesh) const;
    bool RecordPose(VkCommandBuffer cmd,ModelRenderer& renderer,uint32_t subMesh,
        const std::shared_ptr<const std::vector<Vertex>>& current,
        const std::shared_ptr<const std::vector<Vertex>>& previous,bool& boundsChanged);
private:
    VulkanBuffer previousVertices,previousStaging;
    std::shared_ptr<const std::vector<Vertex>> currentCpu,previousCpu;
    glm::vec3 boundsMin{0},boundsMax{0};
};
