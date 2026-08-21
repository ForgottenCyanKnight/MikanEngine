#pragma once
#include "Platform/Export.h"

#include <vulkan/vulkan.h>
#include <vector>
#include <string>

class MIKAN_API HiZComputeShader {
public:
    HiZComputeShader() = default;
    ~HiZComputeShader();

    bool Init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t width, uint32_t height);
    void Cleanup();

    void GenerateMipLevels(VkCommandBuffer commandBuffer, VkImage depthImage, uint32_t mipLevels);
    
    // 获取 Hi-Z 纹理（用于遮挡剔除）
    VkImageView GetHiZTextureView() const;
    VkImageView GetHiZTextureViewForCulling() const; // 获取用于剔除的 Hi-Z 纹理（上一帧）
    VkImage GetHiZTextureImage() const;
    
    // 获取 Mip 层级数量
    uint32_t GetMipLevels() const { return m_MipLevels; }
    
    // 检查是否初始化成功
    bool IsInitialized() const { return m_Pipeline != VK_NULL_HANDLE && !m_DescriptorSets.empty(); }
    
    // 检查是否有可用的 Hi-Z 数据用于剔除（第一帧可能没有）
    bool HasValidCullingData() const { return !m_CullingMipImageViews.empty(); }
    
    // 交换缓冲区（每帧调用）
    void SwapBuffers();

private:
    bool CreateShaderModule(const std::string& shaderPath);
    bool CreatePipeline();
    bool CreateDescriptorSetLayout();
    bool CreateDescriptorPool();
    bool CreateDescriptorSets(uint32_t mipLevels);

    void TransitionDepthForRead(VkCommandBuffer commandBuffer, VkImage depthImage, uint32_t baseMipLevel);
    void TransitionMipLevelForWrite(VkCommandBuffer commandBuffer, uint32_t mipLevel);
    void TransitionMipLevelForRead(VkCommandBuffer commandBuffer, uint32_t mipLevel);
    void CopyToCullingBuffer(VkCommandBuffer commandBuffer); // 复制到剔除用缓冲区

    VkDevice m_Device = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;

    VkShaderModule m_ShaderModule = VK_NULL_HANDLE;
    VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_Pipeline = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_DescriptorSets;

    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    uint32_t m_MipLevels = 0;

    std::vector<VkImage> m_MipImages;
    std::vector<VkDeviceMemory> m_MipImageMemories;
    std::vector<VkImageView> m_MipImageViews;
    
    // 双缓冲：用于剔除的 Hi-Z 纹理（上一帧）
    std::vector<VkImage> m_CullingMipImages;
    std::vector<VkDeviceMemory> m_CullingMipImageMemories;
    std::vector<VkImageView> m_CullingMipImageViews;
    int m_WriteBufferIndex = 0; // 当前写入缓冲区索引 (0 或 1)

    VkImageLayout m_CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};
