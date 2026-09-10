#pragma once

#include "Platform/Export.h"
#include "Rendering/RendererBase.h"

#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <vector>

// Transparent closed-vessel renderer for the GPU SPH sample. The geometry is
// a wide chamber with stepped platforms and a narrow observation channel. The
// simulation runs in container-local coordinates; this renderer applies the
// matching world transform so the particles remain inspectable through it.
class MIKAN_API GpuSphContainerRenderer {
public:
    GpuSphContainerRenderer() = default;
    ~GpuSphContainerRenderer();

    GpuSphContainerRenderer(const GpuSphContainerRenderer&) = delete;
    GpuSphContainerRenderer& operator=(const GpuSphContainerRenderer&) = delete;

    void Render(VkCommandBuffer commandBuffer, uint32_t width, uint32_t height,
                VkRenderPass renderPass, uint32_t subpass,
                const glm::mat4& view, const glm::mat4& proj,
                const glm::vec3& center, const glm::vec3& halfExtents,
                const glm::quat& rotation);
    void Cleanup();

private:
    struct PushConstants {
        glm::mat4 viewProj = glm::mat4(1.0f);
        glm::mat4 model = glm::mat4(1.0f);
    };
    static_assert(sizeof(PushConstants) == 128,
                  "Container push constants must fit Vulkan's guaranteed minimum");

    struct PipelineSet {
        VkRenderPass renderPass = VK_NULL_HANDLE;
        uint32_t subpass = 0;
        VulkanPipeline faces;
        VulkanPipeline edges;
    };

    bool EnsureInitialized(VkRenderPass renderPass, uint32_t subpass);
    bool CreateVertexBuffer();
    bool CreatePipelines(PipelineSet& pipelineSet, uint32_t subpass);

    VulkanBuffer m_VertexBuffer;
    uint32_t m_VertexCount = 0;
    VulkanBuffer m_EdgeVertexBuffer;
    uint32_t m_EdgeVertexCount = 0;
    std::vector<std::unique_ptr<PipelineSet>> m_PipelineSets;
    PipelineSet* m_ActivePipelineSet = nullptr;
};
