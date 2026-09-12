#pragma once

#include "Platform/Export.h"
#include "AABB.h"
#include "RendererBase.h"
#include "ECS/Components.h"
#include "ECS/Types.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

struct RenderWorld;

// 水面使用一个共享的规则网格和实例流：水体实体只提交 model/color/material，
// 不为每个水体重复创建顶点。第一阶段固定为 33x33，后续波浪位移可以直接在
// vertex shader 中增加，而不改变 ECS 或物理接口。
struct MIKAN_API WaterVertex {
    glm::vec2 position = glm::vec2(0.0f); // [-0.5, 0.5]，局部 X/Z
    glm::vec2 uv = glm::vec2(0.0f);       // [0, 1]
};

struct MIKAN_API WaterInstance {
    glm::mat4 model = glm::mat4(1.0f);
    glm::mat4 previousModel = glm::mat4(1.0f);
    glm::vec4 color = glm::vec4(0.035f, 0.22f, 0.32f, 1.0f);
    glm::vec4 material = glm::vec4(0.0f, 0.12f, 1.0f, 0.0f);
};

struct MIKAN_API WaterUniformData {
    glm::mat4 projView = glm::mat4(1.0f);
    glm::mat4 prevProjView = glm::mat4(1.0f);
    glm::vec4 cameraPosition = glm::vec4(0.0f);
    glm::vec4 taaJitter = glm::vec4(0.0f);
};

static_assert(sizeof(WaterVertex) == sizeof(float) * 4, "WaterVertex must stay 16 bytes");
static_assert(sizeof(WaterInstance) == sizeof(float) * 40, "WaterInstance must stay 160 bytes");
static_assert(sizeof(WaterUniformData) % 16 == 0, "WaterUniformData must be 16-byte aligned");

class MIKAN_API WaterRenderer {
public:
    WaterRenderer() = default;
    ~WaterRenderer();
    WaterRenderer(const WaterRenderer&) = delete;
    WaterRenderer& operator=(const WaterRenderer&) = delete;

    // renderPass 必须是当前 SceneRenderTarget 的 render pass；交换链重建时可重复调用。
    void Init(VkRenderPass renderPass);
    void Cleanup();

    // geometry pass 前收集水体并执行视锥裁剪。水体不依赖 MeshComponent，
    // 直接由 WaterComponent 生成自身的 GPU 网格。
    void Prepare(const std::vector<ECS::Entity>& rootEntities,
                const glm::vec3& cameraPosition,
                const std::array<Plane, 6>& frustumPlanes,
                bool useFrustumCulling);

    // Snapshot-driven preparation used by the frame renderer.
    void Prepare(const RenderWorld& world,
                 const glm::vec3& cameraPosition,
                 const std::array<Plane, 6>& frustumPlanes,
                 bool useFrustumCulling);

    // z-prepass 早于 SceneRenderer::PrepareFrame，因此提供独立的场景入口。
    void PrepareFromScene(const glm::vec3& cameraPosition,
                          const std::array<Plane, 6>& frustumPlanes,
                          bool useFrustumCulling);

    void Render(VkCommandBuffer commandBuffer, int width, int height,
                const glm::mat4& projView,
                const glm::mat4& prevProjView,
                const glm::vec3& cameraPosition);

    void RenderDepthPrepass(VkCommandBuffer commandBuffer, int width, int height,
                            const glm::mat4& projView,
                            const glm::vec3& cameraPosition);

    bool IsInitialized() const { return m_Pipeline.GetPipeline() != VK_NULL_HANDLE; }
    size_t GetVisibleWaterCount() const { return m_PreparedInstances.size(); }

private:
    static constexpr uint32_t kFramesInFlight = 3;
    static constexpr uint32_t kMeshResolution = 33;

    void CollectWaterEntities(ECS::Entity entity, std::vector<ECS::Entity>& entities) const;
    bool BuildMesh();
    bool CreateDescriptorResources();
    void DestroyDescriptorResources();
    bool CreateUniformBuffers();
    bool CreateDescriptorSets();
    bool CreatePipelines();
    bool EnsureInstanceCapacity(size_t instanceCount);
    void RenderInternal(VkCommandBuffer commandBuffer, int width, int height,
                        const glm::mat4& projView,
                        const glm::mat4& prevProjView,
                        const glm::vec3& cameraPosition,
                        bool depthOnly);

    VkRenderPass m_RenderPass = VK_NULL_HANDLE;
    VulkanPipeline m_Pipeline;
    VulkanPipeline m_DepthPipeline;

    VulkanBuffer m_VertexBuffer;
    VulkanBuffer m_IndexBuffer;
    uint32_t m_IndexCount = 0;
    bool m_PrimitiveRestartSupported = true;

    VkDescriptorSetLayout m_DescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
    std::array<std::unique_ptr<VulkanBuffer>, kFramesInFlight> m_UniformBuffers;
    std::array<VkDescriptorSet, kFramesInFlight> m_DescriptorSets{};

    std::array<VulkanBuffer, kFramesInFlight> m_InstanceBuffers;
    size_t m_InstanceCapacity = 0;

    std::vector<WaterInstance> m_PreparedInstances;
    std::vector<ECS::Entity> m_PreparedEntities;
    std::unordered_map<ECS::Entity, glm::mat4> m_PreviousModels;
};
