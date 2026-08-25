#pragma once

#include "ParticleSystem.h"
#include "RendererBase.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Vulkan 粒子后端：每个粒子由一个四边形实例表示，顶点阶段根据
// cameraRight/cameraUp 生成始终朝向相机的 Billboard。调用方可把它放在
// 合成后的透明前向 subpass 中，使其参与深度测试并进入后处理链。
class MIKAN_API ParticleRenderer {
public:
    ParticleRenderer() = default;
    ~ParticleRenderer();

    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    // renderPass/subpass 是当前透明粒子阶段。不同视口或 render pass 变化时
    // 会惰性重建兼容的管线变体。
    void Render(VkCommandBuffer commandBuffer, uint32_t width, uint32_t height,
                VkRenderPass renderPass, uint32_t subpass,
                const glm::mat4& view, const glm::mat4& proj,
                const glm::vec3& cameraPosition, const glm::vec2& taaJitter);
    void Cleanup();

    bool IsInitialized() const;

private:
    static constexpr uint32_t kFramesInFlight = 3;
    static constexpr uint32_t kMaxOverlayDrawsPerFrame = 4;
    static constexpr size_t kInitialInstanceCapacity = 1024;

    struct UniformData {
        glm::mat4 viewProj = glm::mat4(1.0f);
        glm::vec4 cameraRight = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec4 cameraUp = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
        glm::vec4 taaJitter = glm::vec4(0.0f);
    };
    static_assert(sizeof(UniformData) % 16 == 0,
                  "ParticleRenderer UBO must stay std140 aligned");

    struct PipelineSet {
        VkRenderPass renderPass = VK_NULL_HANDLE;
        uint32_t subpass = 0;
        VulkanPipeline alpha;
        VulkanPipeline additive;
    };

    bool EnsureInitialized(VkRenderPass renderPass, uint32_t subpass);
    bool CreateDescriptorResources();
    void DestroyDescriptorResources();
    bool CreateUniformBuffers();
    bool CreateDescriptorSets();
    bool CreateInstanceBuffers();
    bool CreatePipelines(PipelineSet& pipelineSet, uint32_t subpass);
    bool EnsureInstanceCapacity(uint32_t frameSlot, uint32_t drawSlot,
                                 size_t instanceCount);

    void RenderBatch(VkCommandBuffer commandBuffer, uint32_t width, uint32_t height,
                     uint32_t frameSlot, uint32_t drawSlot, size_t instanceOffset,
                     size_t instanceCount, VulkanPipeline& pipeline);

    // SceneView/GameView/swapchain can use different UI render-pass handles in
    // the same command buffer. Keep all compatible pipeline variants alive
    // until the next GPU-idle swapchain rebuild instead of destroying the first
    // one while its command is still recorded.
    std::vector<std::unique_ptr<PipelineSet>> m_PipelineSets;
    PipelineSet* m_ActivePipelineSet = nullptr;

    VkDescriptorSetLayout m_DescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
    std::array<std::array<std::unique_ptr<VulkanBuffer>, kMaxOverlayDrawsPerFrame>,
               kFramesInFlight> m_UniformBuffers;
    std::array<std::array<VkDescriptorSet, kMaxOverlayDrawsPerFrame>,
               kFramesInFlight> m_DescriptorSets{};
    std::array<std::array<VulkanBuffer, kMaxOverlayDrawsPerFrame>, kFramesInFlight>
        m_InstanceBuffers;
    std::array<std::array<size_t, kMaxOverlayDrawsPerFrame>, kFramesInFlight>
        m_InstanceCapacities{};

    uint32_t m_LastFrameSlot = UINT32_MAX;
    uint32_t m_DrawIndex = 0;
    std::vector<ParticleInstance> m_AlphaInstances;
    std::vector<ParticleInstance> m_AdditiveInstances;
    std::vector<ParticleInstance> m_UploadInstances;
};
