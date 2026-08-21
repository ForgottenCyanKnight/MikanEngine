#include "Platform/Export.h"
// TextureDescriptorCache.h
// 纹理描述符集缓存管理器
// 优化 ImGui 大量纹理绘制时的性能，减少描述符集创建和销毁开销

#ifndef TEXTURE_DESCRIPTOR_CACHE_H
#define TEXTURE_DESCRIPTOR_CACHE_H

#include <vulkan/vulkan.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <cstdint>

// 缓存的纹理描述符信息
struct MIKAN_API CachedTextureDescriptor {
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint64_t lastUsedFrame = 0;  // 最后使用的帧号
    uint32_t useCount = 0;       // 使用次数
    bool inUse = false;          // 是否正在使用
};

// 纹理缓存配置
struct MIKAN_API TextureCacheConfig {
    uint32_t maxCachedDescriptors = 512;  // 最大缓存描述符数量
    uint32_t cleanupThreshold = 256;       // 清理阈值
    uint32_t framesBeforeCleanup = 120;    // 多少帧后清理未使用的描述符
};

class MIKAN_API TextureDescriptorCache {
public:
    TextureDescriptorCache(VkDevice device, VkPhysicalDevice physicalDevice,
                          VkCommandPool commandPool, VkQueue queue,
                          VkAllocationCallbacks* allocator);
    ~TextureDescriptorCache();

    // 初始化
    bool Initialize(const TextureCacheConfig& config = TextureCacheConfig());
    
    // 获取或创建描述符集
    VkDescriptorSet GetOrCreateDescriptor(const std::string& textureName,
                                         VkImageView imageView,
                                         VkSampler sampler);
    
    // 标记描述符为正在使用（每帧调用）
    void MarkDescriptorUsed(const std::string& textureName);
    
    // 移除描述符
    void RemoveDescriptor(const std::string& textureName);
    
    // 清理未使用的描述符
    void CleanupUnusedDescriptors(uint64_t currentFrame);
    
    // 获取缓存统计信息
    uint32_t GetCachedCount() const { return static_cast<uint32_t>(m_CachedDescriptors.size()); }
    uint32_t GetActiveCount() const { return m_ActiveDescriptorCount; }
    
    // 清理所有资源
    void Cleanup();

private:
    // 创建描述符集
    VkDescriptorSet CreateDescriptor(VkImageView imageView, VkSampler sampler);
    
    // 销毁描述符集
    void DestroyDescriptor(CachedTextureDescriptor& cached);
    
    // 设备句柄
    VkDevice m_Device = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    VkCommandPool m_CommandPool = VK_NULL_HANDLE;
    VkQueue m_Queue = VK_NULL_HANDLE;
    VkAllocationCallbacks* m_Allocator = nullptr;
    
    // 缓存管理
    std::unordered_map<std::string, CachedTextureDescriptor> m_CachedDescriptors;
    std::unordered_map<uint64_t, std::string> m_DescriptorToName;  // 描述符集 -> 名称的反向映射
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
    
    // 配置和统计
    TextureCacheConfig m_Config;
    uint64_t m_CurrentFrame = 0;
    uint32_t m_ActiveDescriptorCount = 0;
};

#endif // TEXTURE_DESCRIPTOR_CACHE_H
