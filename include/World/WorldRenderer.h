#pragma once
// Vulkan voxel terrain renderer. Geometry is resident by chunk; each view draws visible ranges.
#include "Platform/Export.h"
#include "RendererBase.h"
#include "World/World.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <memory>

struct WorldPushConstants {
    glm::mat4 projView;
};

class MIKAN_API WorldRenderer : public BaseRenderer {
public:
    WorldRenderer();
    virtual ~WorldRenderer();

    virtual void Init(VkRenderPass renderPass) override;
    virtual void Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj) override;
    virtual void Cleanup() override;

    virtual VkPipeline GetPipeline() const override { return m_Pipeline.GetPipeline(); }
    virtual VkPipelineLayout GetPipelineLayout() const override { return m_Pipeline.GetLayout(); }

    // 关联世界（World::Update 由外部每帧驱动）
    void SetWorld(World* world) { m_World = world; }
    // Call after the preceding render submission's fence has completed.
    void ReleaseDetachedMeshes();
    VkImageView GetAtlasView() const { return m_AtlasImageView; }
    VkSampler GetAtlasSampler() const { return m_AtlasSampler; }
    // Binding 0 is the same fragment sampler used by Renderer2D's texture layout.
    VkDescriptorSet GetAtlasDescriptor() const { return m_DescriptorSet; }
    glm::uvec2 GetAtlasSize() const { return {m_AtlasWidth, m_AtlasHeight}; }
    World* GetWorld() const { return m_World; }
    bool IsInitialized() const { return m_Initialized; }

    // 渲染世界（带视锥剔除）；frustumPlanes 用于剔除 chunk
    void RenderWorld(VkCommandBuffer commandBuffer, int width, int height,
                     const glm::mat4& view, const glm::mat4& proj,
                     const glm::vec3& cameraPos, const std::array<Plane, 6>& frustumPlanes,
                     int viewSlot = 0, int probeFace = 0);
    bool EnsureWaterTargetPipeline(VkRenderPass renderPass);
    bool HasPreparedWater(VkCommandBuffer commandBuffer, int viewSlot) const;
    void DrawWaterToTarget(VkCommandBuffer commandBuffer, const glm::mat4& projView,
                           const glm::vec3& cameraPosition, int viewSlot);

private:
    struct ResidentMeshes;
    bool CreatePipeline(VkRenderPass renderPass);
    bool CreateQuadBuffer();
    bool LoadAtlasTexture();
    bool CreateDescriptorSet();

    World* m_World = nullptr;
    bool m_Initialized = false;

    // 缓冲
    VkBuffer m_QuadVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_QuadVertexBufferMemory = VK_NULL_HANDLE;

    std::unique_ptr<ResidentMeshes> m_ResidentMeshes;

    // Solid and foliage G-buffer pipelines, plus the separate water mask.
    VulkanPipeline m_Pipeline;
    VulkanPipeline m_AlphaPipeline;
    VulkanPipeline m_WaterTargetPipeline;

    // 图集纹理描述符
    VulkanDescriptor m_Descriptor;
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSet m_MipAtlasDescriptorSet = VK_NULL_HANDLE;
    VkImage m_AtlasImage = VK_NULL_HANDLE;
    VkDeviceMemory m_AtlasImageMemory = VK_NULL_HANDLE;
    VkImageView m_AtlasImageView = VK_NULL_HANDLE;
    VkSampler m_AtlasSampler = VK_NULL_HANDLE;
    VkSampler m_MipAtlasSampler = VK_NULL_HANDLE;
    uint32_t m_AtlasWidth = 0, m_AtlasHeight = 0;

};
