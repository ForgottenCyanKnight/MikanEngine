// TextureDescriptorCache.cpp
// 纹理描述符集缓存管理器实现

#include "Rendering/TextureDescriptorCache.h"
#include "EngineGlobal.h"


#include <cstring>

TextureDescriptorCache::TextureDescriptorCache(VkDevice device, VkPhysicalDevice physicalDevice,
                                              VkCommandPool commandPool, VkQueue queue,
                                              VkAllocationCallbacks* allocator)
    : m_Device(device), m_PhysicalDevice(physicalDevice),
      m_CommandPool(commandPool), m_Queue(queue), m_Allocator(allocator)
{
}

TextureDescriptorCache::~TextureDescriptorCache() {
    Cleanup();
}

bool TextureDescriptorCache::Initialize(const TextureCacheConfig& config) {
    if (m_DescriptorPool != VK_NULL_HANDLE) {
        return false;  // 已经初始化过了
    }

    m_Config = config;

    // 创建描述符池
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = m_Config.maxCachedDescriptors;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = m_Config.maxCachedDescriptors;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;  // 允许单独释放描述符集

    VkResult err = vkCreateDescriptorPool(m_Device, &poolInfo, m_Allocator, &m_DescriptorPool);
    if (err != VK_SUCCESS) {
        printf("[TextureDescriptorCache] Failed to create descriptor pool!");
        return false;
    }

    printf("[TextureDescriptorCache] Initialized with max %d cached descriptors",
                                   m_Config.maxCachedDescriptors);
    return true;
}

VkDescriptorSet TextureDescriptorCache::GetOrCreateDescriptor(const std::string& textureName,
                                                             VkImageView imageView,
                                                             VkSampler sampler) {
    // 检查缓存中是否已存在
    auto it = m_CachedDescriptors.find(textureName);
    if (it != m_CachedDescriptors.end()) {
        // 更新使用信息
        it->second.lastUsedFrame = m_CurrentFrame;
        it->second.useCount++;
        it->second.inUse = true;
        return it->second.descriptorSet;
    }

    // 检查是否需要清理
    if (m_CachedDescriptors.size() >= m_Config.cleanupThreshold) {
        CleanupUnusedDescriptors(m_CurrentFrame);
    }

    // 创建新的描述符集
    VkDescriptorSet newDescriptor = CreateDescriptor(imageView, sampler);
    if (newDescriptor == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }

    // 添加到缓存
    CachedTextureDescriptor cached;
    cached.descriptorSet = newDescriptor;
    cached.imageView = imageView;
    cached.sampler = sampler;
    cached.lastUsedFrame = m_CurrentFrame;
    cached.useCount = 1;
    cached.inUse = true;

    m_CachedDescriptors[textureName] = cached;
    m_DescriptorToName[(uint64_t)newDescriptor] = textureName;
    m_ActiveDescriptorCount++;

    printf("[TextureDescriptorCache] Created new descriptor for '%s' (cached: %d/%d)",
                                   textureName.c_str(), m_ActiveDescriptorCount, m_Config.maxCachedDescriptors);

    return newDescriptor;
}

void TextureDescriptorCache::MarkDescriptorUsed(const std::string& textureName) {
    auto it = m_CachedDescriptors.find(textureName);
    if (it != m_CachedDescriptors.end()) {
        it->second.lastUsedFrame = m_CurrentFrame;
        it->second.inUse = true;
    }
}

void TextureDescriptorCache::RemoveDescriptor(const std::string& textureName) {
    auto it = m_CachedDescriptors.find(textureName);
    if (it != m_CachedDescriptors.end()) {
        DestroyDescriptor(it->second);
        m_DescriptorToName.erase((uint64_t)it->second.descriptorSet);
        m_CachedDescriptors.erase(it);
        if (m_ActiveDescriptorCount > 0) {
            m_ActiveDescriptorCount--;
        }
    }
}

void TextureDescriptorCache::CleanupUnusedDescriptors(uint64_t currentFrame) {
    std::vector<std::string> toRemove;

    for (auto& pair : m_CachedDescriptors) {
        CachedTextureDescriptor& cached = pair.second;

        // 如果超过指定帧数未使用，且当前不在使用中，则标记为待删除
        if (!cached.inUse &&
            (currentFrame - cached.lastUsedFrame) > m_Config.framesBeforeCleanup) {
            toRemove.push_back(pair.first);
        }

        // 重置 inUse 标志
        cached.inUse = false;
    }

    // 删除未使用的描述符
    for (const auto& name : toRemove) {
        printf("[TextureDescriptorCache] Cleaning up unused descriptor: %s", name.c_str());
        RemoveDescriptor(name);
    }

    if (!toRemove.empty()) {
        printf("[TextureDescriptorCache] Cleaned up %d unused descriptors", toRemove.size());
    }
}

VkDescriptorSet TextureDescriptorCache::CreateDescriptor(VkImageView imageView, VkSampler sampler) {
    if (m_DescriptorPool == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }

    // 分配描述符集
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_DescriptorPool;
    allocInfo.descriptorSetCount = 1;

    // 创建临时的描述符集布局
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    VkResult err = vkCreateDescriptorSetLayout(m_Device, &layoutInfo, m_Allocator, &descriptorSetLayout);
    if (err != VK_SUCCESS) {
        printf("[TextureDescriptorCache] Failed to create descriptor set layout!");
        return VK_NULL_HANDLE;
    }

    allocInfo.pSetLayouts = &descriptorSetLayout;

    err = vkAllocateDescriptorSets(m_Device, &allocInfo, &descriptorSet);
    if (err != VK_SUCCESS) {
        printf("[TextureDescriptorCache] Failed to allocate descriptor set!");
        vkDestroyDescriptorSetLayout(m_Device, descriptorSetLayout, m_Allocator);
        return VK_NULL_HANDLE;
    }

    // 更新描述符集
    VkDescriptorImageInfo imageInfo = {};
    imageInfo.sampler = sampler;
    imageInfo.imageView = imageView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet descriptorWrite = {};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = descriptorSet;
    descriptorWrite.dstBinding = 0;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(m_Device, 1, &descriptorWrite, 0, nullptr);

    // 存储布局以便后续清理
    // 注意：这里简化处理，实际应该在 CachedTextureDescriptor 中保存 layout

    return descriptorSet;
}

void TextureDescriptorCache::DestroyDescriptor(CachedTextureDescriptor& cached) {
    if (cached.descriptorSet != VK_NULL_HANDLE && m_DescriptorPool != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(m_Device, m_DescriptorPool, 1, &cached.descriptorSet);
        cached.descriptorSet = VK_NULL_HANDLE;
    }

    if (cached.descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_Device, cached.descriptorSetLayout, m_Allocator);
        cached.descriptorSetLayout = VK_NULL_HANDLE;
    }
}

void TextureDescriptorCache::Cleanup() {
    for (auto& pair : m_CachedDescriptors) {
        DestroyDescriptor(pair.second);
    }

    m_CachedDescriptors.clear();
    m_DescriptorToName.clear();
    m_ActiveDescriptorCount = 0;

    if (m_DescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_Device, m_DescriptorPool, m_Allocator);
        m_DescriptorPool = VK_NULL_HANDLE;
    }
}
