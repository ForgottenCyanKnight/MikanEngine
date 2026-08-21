#pragma once
#include "Platform/Export.h"
#ifndef VOXEL_TEXTURE3D_MANAGER_H
#define VOXEL_TEXTURE3D_MANAGER_H

#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>

// 体素 Texture3D 描述符管理器
// 用于管理多个体素模型的 Texture3D 数组，支持计算着色器访问

struct MIKAN_API VoxelTexture3D {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    uint32_t sizeX = 0;
    uint32_t sizeY = 0;
    uint32_t sizeZ = 0;
    uint32_t voxelCount = 0;
    std::string name;
    
    void Cleanup(VkDevice device, VkAllocationCallbacks* allocator);
};

struct MIKAN_API VoxelTexture3DManagerConfig {
    uint32_t maxTextures = 64;          // 最大 Texture3D 数量
    VkFormat format = VK_FORMAT_R8_UINT; // 体素格式 (R8)
    bool enableMipmaps = false;          // 是否启用 Mipmaps
    VkFilter filter = VK_FILTER_NEAREST; // 采样过滤器类型
};

class MIKAN_API VoxelTexture3DManager {
public:
    VoxelTexture3DManager();
    ~VoxelTexture3DManager();
    
    // 初始化管理器
    bool Initialize(const VoxelTexture3DManagerConfig& config = {});
    
    // 清理资源
    void Cleanup();
    
    // 从体素数据创建 Texture3D
    uint32_t CreateTexture3D(
        const std::string& name,
        const std::vector<uint8_t>& voxelData,
        uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ,
        const std::vector<VkSamplerAddressMode>& addressModes = {}
    );
    
    // 删除 Texture3D
    void DestroyTexture3D(uint32_t textureIndex);
    
    // 获取 Texture3D 索引
    int32_t GetTextureIndex(const std::string& name) const;
    
    // 获取 Texture3D 对象
    const VoxelTexture3D* GetTexture3D(uint32_t index) const;
    const VoxelTexture3D* GetTexture3D(const std::string& name) const;
    
    // 获取描述符集
    VkDescriptorSet GetDescriptorSet() const { return m_DescriptorSet; }
    VkDescriptorSetLayout GetDescriptorSetLayout() const { return m_DescriptorSetLayout; }
    VkPipelineLayout GetPipelineLayout() const { return m_PipelineLayout; }
    VkSampler GetSampler() const { return m_Sampler; }
    
    // 获取 Texture3D 数组信息
    uint32_t GetTextureCount() const { return m_Textures.size(); }
    uint32_t GetMaxTextures() const { return m_Config.maxTextures; }
    
    // 更新描述符集（添加新纹理后调用）
    void UpdateDescriptorSet();
    
    // 创建计算着色器管线布局
    void CreateComputePipelineLayout();
    
private:
    // 创建描述符集布局
    bool CreateDescriptorSetLayout();
    
    // 创建描述符池
    bool CreateDescriptorPool();
    
    // 分配描述符集
    bool AllocateDescriptorSet();
    
    // 创建采样器
    bool CreateSampler();
    
    // 创建 Image 和 ImageView
    bool CreateImage(const std::string& name,
                    const std::vector<uint8_t>& voxelData,
                    uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ,
                    VoxelTexture3D& outTexture);
    
    // 上传体素数据到 GPU
    bool UploadVoxelData(VoxelTexture3D& texture, const std::vector<uint8_t>& voxelData);
    
    // 转换图像布局
    void TransitionImageLayout(VkImage image, 
                              VkImageLayout oldLayout, 
                              VkImageLayout newLayout,
                              uint32_t mipLevels,
                              VkCommandBuffer cmd);
    
    VoxelTexture3DManagerConfig m_Config;
    
    std::vector<VoxelTexture3D> m_Textures;
    std::unordered_map<std::string, uint32_t> m_TextureNameToIndex;
    
    VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
    VkSampler m_Sampler = VK_NULL_HANDLE;
    
    bool m_Initialized = false;
};

#endif // VOXEL_TEXTURE3D_MANAGER_H
