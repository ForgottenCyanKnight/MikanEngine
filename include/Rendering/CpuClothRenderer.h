#pragma once

#include "Rendering/CpuClothSimulation.h"
#include "Rendering/RendererBase.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Forward-rendered CPU cloth surfaces plus the colored spring lattices and
// obstacle geometry emitted by the simulation. The renderer owns only Vulkan
// upload/pipeline resources; all simulation state can remain frozen on the
// first frame.
class MIKAN_API CpuClothRenderer {
public:
    CpuClothRenderer() = default;
    ~CpuClothRenderer();

    CpuClothRenderer(const CpuClothRenderer&) = delete;
    CpuClothRenderer& operator=(const CpuClothRenderer&) = delete;

    void Render(VkCommandBuffer commandBuffer, uint32_t width, uint32_t height,
                VkRenderPass renderPass, uint32_t subpass,
                const glm::mat4& view, const glm::mat4& proj,
                const CpuClothSimulation& simulation);
    void Cleanup();

private:
    static constexpr uint32_t kFramesInFlight = 3;
    static constexpr size_t kInitialSurfaceCapacity = 8192;
    static constexpr size_t kInitialSpringCapacity = 16384;

    struct PushConstants {
        glm::mat4 viewProj = glm::mat4(1.0f);
        glm::mat4 model = glm::mat4(1.0f);
    };
    static_assert(sizeof(PushConstants) == sizeof(float) * 32,
                  "CPU cloth push constants must remain two mat4 values");

    struct PipelineSet {
        VkRenderPass renderPass = VK_NULL_HANDLE;
        uint32_t subpass = 0;
        VulkanPipeline surface;
        VulkanPipeline springs;
    };

    bool EnsureInitialized(VkRenderPass renderPass, uint32_t subpass);
    bool CreatePipelines(PipelineSet& pipelineSet, uint32_t subpass);
    bool EnsureBufferCapacity(VulkanBuffer& buffer, size_t& capacity,
                              size_t required, size_t initialCapacity);
    bool UploadFrame(const CpuClothSimulation& simulation,
                     uint32_t frameSlot, uint64_t frameSerial);

    std::vector<std::unique_ptr<PipelineSet>> m_PipelineSets;
    PipelineSet* m_ActivePipelineSet = nullptr;

    std::array<VulkanBuffer, kFramesInFlight> m_SurfaceBuffers;
    std::array<VulkanBuffer, kFramesInFlight> m_SpringBuffers;
    std::array<size_t, kFramesInFlight> m_SurfaceCapacities{};
    std::array<size_t, kFramesInFlight> m_SpringCapacities{};
    std::array<uint32_t, kFramesInFlight> m_SurfaceCounts{};
    std::array<uint32_t, kFramesInFlight> m_SpringCounts{};

    std::vector<ClothRenderVertex> m_SurfaceUpload;
    std::vector<ClothRenderVertex> m_SpringUpload;
    uint64_t m_LastUploadFrameSerial = UINT64_MAX;
};
