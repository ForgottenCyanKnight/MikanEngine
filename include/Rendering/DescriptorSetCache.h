#pragma once
#include "Platform/Export.h"
#ifndef DESCRIPTOR_SET_CACHE_H
#define DESCRIPTOR_SET_CACHE_H

#include <vulkan/vulkan.h>
#include <unordered_map>
#include <string>
#include <functional>
#include "EngineGlobal.h"

struct MIKAN_API MaterialHash {
    std::string diffuseTexturePath;
    std::string normalTexturePath;
    std::string roughnessTexturePath;
    std::string metallicTexturePath;
    std::string emissiveTexturePath;   // 2026-08-09

    bool operator==(const MaterialHash& other) const {
        return diffuseTexturePath == other.diffuseTexturePath &&
               normalTexturePath == other.normalTexturePath &&
               roughnessTexturePath == other.roughnessTexturePath &&
               metallicTexturePath == other.metallicTexturePath &&
               emissiveTexturePath == other.emissiveTexturePath;
    }
};

namespace std {
    template<> struct hash<MaterialHash> {
        size_t operator()(const MaterialHash& key) const {
            size_t hashVal = 0;
            std::hash<std::string> hasher;
            hashVal ^= hasher(key.diffuseTexturePath) + 0x9e3779b9 + (hashVal << 6) + (hashVal >> 2);
            hashVal ^= hasher(key.normalTexturePath) + 0x9e3779b9 + (hashVal << 6) + (hashVal >> 2);
            hashVal ^= hasher(key.roughnessTexturePath) + 0x9e3779b9 + (hashVal << 6) + (hashVal >> 2);
            hashVal ^= hasher(key.metallicTexturePath) + 0x9e3779b9 + (hashVal << 6) + (hashVal >> 2);
            hashVal ^= hasher(key.emissiveTexturePath) + 0x9e3779b9 + (hashVal << 6) + (hashVal >> 2);
            return hashVal;
        }
    };
}

class MIKAN_API DescriptorSetCache {
public:
    static DescriptorSetCache& GetInstance();

    void Init() {
        m_descriptorPool = VK_NULL_HANDLE;
        m_descriptorLayout = VK_NULL_HANDLE;
    }

    void Cleanup() {
        for (auto& [hash, set] : m_cache) {
            if (set != VK_NULL_HANDLE) {
                vkFreeDescriptorSets(g_Device, m_descriptorPool, 1, &set);
            }
        }
        m_cache.clear();

        if (m_descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(g_Device, m_descriptorPool, g_Allocator);
            m_descriptorPool = VK_NULL_HANDLE;
        }
        if (m_descriptorLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(g_Device, m_descriptorLayout, g_Allocator);
            m_descriptorLayout = VK_NULL_HANDLE;
        }
    }

    void CreateDescriptorSetLayout() {
        // binding 0-3: 贴图（diffuse/normal/roughness/metallic）
        // binding 4: 骨骼蒙皮矩阵 UBO（顶点着色器；无骨骼模型共享布局，weight=0 时 shader 跳过）
        // binding 5: 自发光贴图（emissive，2026-08-09；Bistro 发光体材质）
        std::array<VkDescriptorSetLayoutBinding, 6> bindings = {};
        
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[0].pImmutableSamplers = nullptr;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].pImmutableSamplers = nullptr;

        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].pImmutableSamplers = nullptr;

        bindings[3].binding = 3;
        bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[3].pImmutableSamplers = nullptr;

        bindings[4].binding = 4;
        bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;   // 骨骼蒙皮矩阵 UBO（固定 64，vertex 动态索引；2026-08-17 DYNAMIC——3 帧槽轮换）
        bindings[4].descriptorCount = 1;
        bindings[4].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        bindings[4].pImmutableSamplers = nullptr;

        bindings[5].binding = 5;
        bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[5].descriptorCount = 1;
        bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[5].pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();

        VkResult err = vkCreateDescriptorSetLayout(g_Device, &layoutInfo, g_Allocator, &m_descriptorLayout);
        check_vk_result(err);
    }

    VkDescriptorSetLayout GetLayout() const { return m_descriptorLayout; }

    void CreateDescriptorPool(uint32_t maxSets = 1000) {
        std::array<VkDescriptorPoolSize, 2> poolSizes = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        // bindings 0-3 and 5 are combined image samplers (five per set).
        poolSizes[0].descriptorCount = maxSets * 5;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;   // 骨骼蒙皮矩阵
        poolSizes[1].descriptorCount = maxSets;

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        poolInfo.maxSets = maxSets;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

        VkResult err = vkCreateDescriptorPool(g_Device, &poolInfo, g_Allocator, &m_descriptorPool);
        check_vk_result(err);
    }

    // 清空材质→descriptor 缓存（不销毁 pool/layout；旧 set 留池复用）。
    // 调用场景：descriptor 布局/类型变化（如蒙皮 binding 4 从 texel→UBO→SSBO）后必须清，
    // 否则长驻进程缓存命中旧 set → 回调不执行 → shader 引用空 binding → 不渲染。
    void ClearCache() { m_cache.clear(); }

    VkDescriptorSet GetOrCreate(const MaterialHash& materialHash, 
                                  std::function<void(VkDescriptorSet)> updateCallback) {
        auto it = m_cache.find(materialHash);
        if (it != m_cache.end()) {
            return it->second;
        }

        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_descriptorLayout;

        VkResult err = vkAllocateDescriptorSets(g_Device, &allocInfo, &descriptorSet);
        if (err != VK_SUCCESS) {
            check_vk_result(err);
            return VK_NULL_HANDLE;
        }

        if (updateCallback) {
            updateCallback(descriptorSet);
        }

        m_cache[materialHash] = descriptorSet;
        return descriptorSet;
    }

private:
    DescriptorSetCache() = default;
    ~DescriptorSetCache() = default;
    DescriptorSetCache(const DescriptorSetCache&) = delete;
    DescriptorSetCache& operator=(const DescriptorSetCache&) = delete;

    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorLayout = VK_NULL_HANDLE;
    std::unordered_map<MaterialHash, VkDescriptorSet> m_cache;
};

#endif
