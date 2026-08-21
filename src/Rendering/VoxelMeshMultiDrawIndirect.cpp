#include "Rendering/VoxelMeshMultiDrawIndirect.h"
#include "Core/EngineGlobal.h"
#include "Core/EngineConfig.h"
#include "VulkanManager.h"
#include "Rendering/RendererBase.h"
#include "Rendering/SceneRenderer.h"
#include "Rendering/RenderTarget.h"
#include "AABB.h"
#include <iostream>
#include <unordered_set>
#include <unordered_map>
#include <array>
#include <thread>
#include <future>
#include <execution>
#include <mutex>

extern SceneRenderer g_SceneRenderer;
extern RenderTarget g_GameRenderTarget;

namespace {
    constexpr glm::vec3 FACE_NORMALS[6] = {
        glm::vec3(0.0f, 0.0f, 1.0f),
        glm::vec3(0.0f, 0.0f, -1.0f),
        glm::vec3(-1.0f, 0.0f, 0.0f),
        glm::vec3(1.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec3(0.0f, -1.0f, 0.0f)
    };
}

VoxelMeshMultiDrawIndirect::VoxelMeshMultiDrawIndirect()
    : m_supportsMDI(false),
      m_supportsComputeShader(false),
      m_vertexBuffer(VK_NULL_HANDLE),
      m_vertexBufferMemory(VK_NULL_HANDLE),
      m_indexBuffer(VK_NULL_HANDLE),
      m_indexBufferMemory(VK_NULL_HANDLE),
      m_visibleDrawCommandBuffer(VK_NULL_HANDLE),
      m_visibleDrawCommandBufferMemory(VK_NULL_HANDLE),
      m_counterBuffer(VK_NULL_HANDLE),
      m_counterBufferMemory(VK_NULL_HANDLE),
      m_mappedCounterPtr(nullptr),
      m_cullingCameraBuffer(VK_NULL_HANDLE),
      m_cullingCameraBufferMemory(VK_NULL_HANDLE),
      m_mappedCameraPtr(nullptr),
      m_cullingPipeline(VK_NULL_HANDLE),
      m_cullingPipelineLayout(VK_NULL_HANDLE),
      m_cullingDescriptorSetLayout(VK_NULL_HANDLE),
      m_cullingDescriptorPool(VK_NULL_HANDLE),
      m_cullingDescriptorSet(VK_NULL_HANDLE),
      m_dummyHiZImage(VK_NULL_HANDLE),
      m_dummyHiZImageMemory(VK_NULL_HANDLE),
      m_dummyHiZImageView(VK_NULL_HANDLE),
      m_maxVoxelModels(0),
      m_maxTotalVertices(0),
      m_maxTotalIndices(0),
      m_totalVertices(0),
      m_totalIndices(0),
      m_totalInstances(0),
      m_currentFaceCommandCount(0),
      m_geometryDataDirty(true),
      m_currentCommandBufferIndex(0),
      m_currentFenceIndex(0),
      m_currentInstanceBufferIndex(0),
      m_currentDrawCommandBufferIndex(0)
{
    // 初始化命令缓冲区池
    for (size_t i = 0; i < 4; i++) {
        m_copyCommandBufferPool[i] = VK_NULL_HANDLE;
        m_copyFences[i] = VK_NULL_HANDLE;
    }
    
    // 初始化双缓冲
    for (size_t i = 0; i < 2; i++) {
        m_instanceBuffers[i] = VK_NULL_HANDLE;
        m_instanceBufferMemories[i] = VK_NULL_HANDLE;
        m_mappedInstancePtrs[i] = nullptr;
        m_drawCommandBuffers[i] = VK_NULL_HANDLE;
        m_drawCommandBufferMemories[i] = VK_NULL_HANDLE;
        m_mappedDrawCommandPtrs[i] = nullptr;
    }
}

VoxelMeshMultiDrawIndirect::~VoxelMeshMultiDrawIndirect()
{
    extern VkDevice g_Device;
    extern VkCommandPool g_CommandPool;
    bool deviceValid = (g_Device != VK_NULL_HANDLE);
    bool commandPoolValid = (g_CommandPool != VK_NULL_HANDLE);
    
    // g_Allocator 可以为 nullptr，这是正常的，使用默认分配器
    VkAllocationCallbacks* allocator = g_Allocator;
    
    // 释放命令缓冲区池
    if (m_copyCommandBufferPool[0] != VK_NULL_HANDLE && deviceValid && commandPoolValid) {
        vkFreeCommandBuffers(g_Device, g_CommandPool, 4, m_copyCommandBufferPool);
    }
    
    // 释放 fence
    for (size_t i = 0; i < 4; i++) {
        if (m_copyFences[i] != VK_NULL_HANDLE && deviceValid) {
            vkDestroyFence(g_Device, m_copyFences[i], g_Allocator);
        }
    }
    
    // 清理 GPU 剔除资源
    if (m_cullingPipeline != VK_NULL_HANDLE && deviceValid) {
        vkDestroyPipeline(g_Device, m_cullingPipeline, allocator);
    }
    if (m_cullingPipelineLayout != VK_NULL_HANDLE && deviceValid) {
        vkDestroyPipelineLayout(g_Device, m_cullingPipelineLayout, allocator);
    }
    if (m_cullingDescriptorSetLayout != VK_NULL_HANDLE && deviceValid) {
        vkDestroyDescriptorSetLayout(g_Device, m_cullingDescriptorSetLayout, allocator);
    }
    if (m_cullingDescriptorPool != VK_NULL_HANDLE && deviceValid) {
        vkDestroyDescriptorPool(g_Device, m_cullingDescriptorPool, allocator);
    }
    
    // 清理 GPU 剔除缓冲区
    if (m_mappedCounterPtr && deviceValid) {
        vkUnmapMemory(g_Device, m_counterBufferMemory);
    }
    if (m_mappedCameraPtr && deviceValid) {
        vkUnmapMemory(g_Device, m_cullingCameraBufferMemory);
    }
    if (m_visibleDrawCommandBuffer != VK_NULL_HANDLE && deviceValid) {
        vkDestroyBuffer(g_Device, m_visibleDrawCommandBuffer, allocator);
        vkFreeMemory(g_Device, m_visibleDrawCommandBufferMemory, allocator);
    }
    if (m_counterBuffer != VK_NULL_HANDLE && deviceValid) {
        vkDestroyBuffer(g_Device, m_counterBuffer, allocator);
        vkFreeMemory(g_Device, m_counterBufferMemory, allocator);
    }
    if (m_cullingCameraBuffer != VK_NULL_HANDLE && deviceValid) {
        vkDestroyBuffer(g_Device, m_cullingCameraBuffer, allocator);
        vkFreeMemory(g_Device, m_cullingCameraBufferMemory, allocator);
    }
    
    // 清理 dummy Hi-Z 图像资源
    if (m_dummyHiZImageView != VK_NULL_HANDLE && deviceValid) {
        vkDestroyImageView(g_Device, m_dummyHiZImageView, allocator);
    }
    if (m_dummyHiZImage != VK_NULL_HANDLE && deviceValid) {
        vkDestroyImage(g_Device, m_dummyHiZImage, allocator);
    }
    if (m_dummyHiZImageMemory != VK_NULL_HANDLE && deviceValid) {
        vkFreeMemory(g_Device, m_dummyHiZImageMemory, allocator);
    }
    
    // 释放双缓冲
    for (size_t i = 0; i < 2; i++) {
        if (m_mappedInstancePtrs[i] && deviceValid) {
            vkUnmapMemory(g_Device, m_instanceBufferMemories[i]);
        }
        if (m_mappedDrawCommandPtrs[i] && deviceValid) {
            vkUnmapMemory(g_Device, m_drawCommandBufferMemories[i]);
        }
        if (m_instanceBuffers[i] != VK_NULL_HANDLE && deviceValid) {
            vkDestroyBuffer(g_Device, m_instanceBuffers[i], allocator);
        }
        if (m_instanceBufferMemories[i] != VK_NULL_HANDLE && deviceValid) {
            vkFreeMemory(g_Device, m_instanceBufferMemories[i], allocator);
        }
        if (m_drawCommandBuffers[i] != VK_NULL_HANDLE && deviceValid) {
            vkDestroyBuffer(g_Device, m_drawCommandBuffers[i], allocator);
        }
        if (m_drawCommandBufferMemories[i] != VK_NULL_HANDLE && deviceValid) {
            vkFreeMemory(g_Device, m_drawCommandBufferMemories[i], allocator);
        }
    }
    
    if (m_vertexBuffer && deviceValid) {
        vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
    }
    if (m_vertexBufferMemory && deviceValid) {
        vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
    }
    if (m_indexBuffer && deviceValid) {
        vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
    }
    if (m_indexBufferMemory && deviceValid) {
        vkFreeMemory(g_Device, m_indexBufferMemory, allocator);
    }
}

bool VoxelMeshMultiDrawIndirect::Initialize(size_t maxVoxelModels, size_t maxTotalVertices, size_t maxTotalIndices)
{
    std::cout << "[VoxelMeshMultiDrawIndirect] Initializing (max " << maxVoxelModels << " models, " << maxTotalVertices << " verts, " << maxTotalIndices << " idx)..." << std::endl;
    
    m_maxVoxelModels = maxVoxelModels;
    m_maxTotalVertices = maxTotalVertices;
    m_maxTotalIndices = maxTotalIndices;
    m_supportsMDI = CheckMultiDrawIndirectSupport();
    
    // 输出 MDI 支持状态
    if (m_supportsMDI) {
        std::cout << "[VoxelMeshMultiDrawIndirect] MDI is supported" << std::endl;
    } else {
        if (g_PhysicalDevice == VK_NULL_HANDLE) {
            std::cout << "[VoxelMeshMultiDrawIndirect] MDI is not supported: g_PhysicalDevice is null" << std::endl;
        } else {
            std::cout << "[VoxelMeshMultiDrawIndirect] MDI is not supported: device does not support multiDrawIndirect feature" << std::endl;
        }
    }
    
    // 检查计算着色器支持
    m_supportsComputeShader = CheckComputeShaderSupport();
    
    if (!CreateBuffers(maxVoxelModels, maxTotalVertices, maxTotalIndices)) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create buffers!" << std::endl;
        return false;
    }
    
    // 创建 GPU 剔除资源（如果支持计算着色器）
    if (m_supportsComputeShader) {
        if (!CreateGPUCullingResources()) {
            std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create GPU culling resources!" << std::endl;
            return false;
        }
        if (!CreateCullingDescriptorSet()) {
            std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create culling descriptor set!" << std::endl;
            return false;
        }
        if (!CreateCullingPipeline()) {
            std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create culling pipeline!" << std::endl;
            return false;
        }
    } else {
        std::cout << "[VoxelMeshMultiDrawIndirect] Compute shader not supported, using CPU culling" << std::endl;
    }
    
    // 使用 CPU 方案生成绘制命令
    if (m_supportsMDI) {
        std::cout << "[VoxelMeshMultiDrawIndirect] Using GPU-based Multi Draw Indirect rendering" << std::endl;
        if (!m_supportsComputeShader) {
            std::cout << "[VoxelMeshMultiDrawIndirect] GPU frustum culling not available (compute shader not supported)" << std::endl;
        }
    } else {
        std::cout << "[VoxelMeshMultiDrawIndirect] Using CPU-based draw command generation (fallback)" << std::endl;
    }
    
    // 创建命令缓冲区池
    VkCommandBufferAllocateInfo cmdBufferAllocInfo{};
    cmdBufferAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdBufferAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdBufferAllocInfo.commandPool = g_CommandPool;
    cmdBufferAllocInfo.commandBufferCount = 4;
    
    if (vkAllocateCommandBuffers(g_Device, &cmdBufferAllocInfo, m_copyCommandBufferPool) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to allocate command buffers!" << std::endl;
        return false;
    }
    
    // 创建 fence
    for (size_t i = 0; i < 4; i++) {
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = 0;
        
        if (vkCreateFence(g_Device, &fenceInfo, g_Allocator, &m_copyFences[i]) != VK_SUCCESS) {
            std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create fence!" << std::endl;
            return false;
        }
    }
    
    std::cout << "[VoxelMeshMultiDrawIndirect] Initialization completed" << std::endl;
    return true;
}

bool VoxelMeshMultiDrawIndirect::CheckMultiDrawIndirectSupport()
{
    if (g_PhysicalDevice == VK_NULL_HANDLE) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] g_PhysicalDevice is null, cannot check MDI support!" << std::endl;
        return false;
    }
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(g_PhysicalDevice, &features);
    return features.multiDrawIndirect;
}

bool VoxelMeshMultiDrawIndirect::CheckComputeShaderSupport()
{
    if (g_PhysicalDevice == VK_NULL_HANDLE) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] g_PhysicalDevice is null, cannot check compute shader support!" << std::endl;
        return false;
    }
    
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(g_PhysicalDevice, &properties);
    
    // 检查是否支持计算着色器
    bool supportsCompute = (properties.limits.maxComputeWorkGroupCount[0] > 0);
    
    return supportsCompute;
}

bool VoxelMeshMultiDrawIndirect::CreateBuffers(size_t maxVoxelModels, size_t maxTotalVertices, size_t maxTotalIndices)
{
    if (g_Device == VK_NULL_HANDLE) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] g_Device is null!" << std::endl;
        return false;
    }
    // g_Allocator 可以为 nullptr，这是正常的，使用默认分配器
    VkAllocationCallbacks* allocator = g_Allocator;
    
    // 创建顶点缓冲区
    VkDeviceSize vertexBufferSize = sizeof(VoxelMeshVertex) * maxTotalVertices;
    VkBufferCreateInfo vertexBufferInfo{};
    vertexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vertexBufferInfo.size = vertexBufferSize;
    vertexBufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vertexBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(g_Device, &vertexBufferInfo, allocator, &m_vertexBuffer) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create vertex buffer!" << std::endl;
        return false;
    }

    VkMemoryRequirements vertexMemRequirements;
    vkGetBufferMemoryRequirements(g_Device, m_vertexBuffer, &vertexMemRequirements);

    VkMemoryAllocateInfo vertexAllocInfo{};
    vertexAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    vertexAllocInfo.allocationSize = vertexMemRequirements.size;
    vertexAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(vertexMemRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(g_Device, &vertexAllocInfo, allocator, &m_vertexBufferMemory) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to allocate vertex buffer memory!" << std::endl;
        vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
        m_vertexBuffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(g_Device, m_vertexBuffer, m_vertexBufferMemory, 0);

    // 创建索引缓冲区
    VkDeviceSize indexBufferSize = sizeof(uint32_t) * maxTotalIndices;
    VkBufferCreateInfo indexBufferInfo{};
    indexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    indexBufferInfo.size = indexBufferSize;
    indexBufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    indexBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(g_Device, &indexBufferInfo, allocator, &m_indexBuffer) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create index buffer!" << std::endl;
        vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
        vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
        m_vertexBuffer = VK_NULL_HANDLE;
        m_vertexBufferMemory = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements indexMemRequirements;
    vkGetBufferMemoryRequirements(g_Device, m_indexBuffer, &indexMemRequirements);

    VkMemoryAllocateInfo indexAllocInfo{};
    indexAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    indexAllocInfo.allocationSize = indexMemRequirements.size;
    indexAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(indexMemRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(g_Device, &indexAllocInfo, allocator, &m_indexBufferMemory) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to allocate index buffer memory!" << std::endl;
        vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
        vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
        m_vertexBuffer = VK_NULL_HANDLE;
        m_vertexBufferMemory = VK_NULL_HANDLE;
        m_indexBuffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(g_Device, m_indexBuffer, m_indexBufferMemory, 0);

    // 创建实例数据缓冲区（双缓冲）
    VkDeviceSize instanceBufferSize = sizeof(InstanceData) * maxVoxelModels;
    VkBufferCreateInfo instanceBufferInfo{};
    instanceBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    instanceBufferInfo.size = instanceBufferSize;
    instanceBufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    instanceBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    for (size_t i = 0; i < 2; i++) {
        if (vkCreateBuffer(g_Device, &instanceBufferInfo, allocator, &m_instanceBuffers[i]) != VK_SUCCESS) {
            std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create instance buffer!" << std::endl;
            // 清理已创建的缓冲区
            for (size_t j = 0; j < i; j++) {
                if (m_mappedInstancePtrs[j]) {
                    vkUnmapMemory(g_Device, m_instanceBufferMemories[j]);
                }
                if (m_instanceBuffers[j] != VK_NULL_HANDLE) {
                    vkDestroyBuffer(g_Device, m_instanceBuffers[j], allocator);
                }
                if (m_instanceBufferMemories[j] != VK_NULL_HANDLE) {
                    vkFreeMemory(g_Device, m_instanceBufferMemories[j], allocator);
                }
            }
            vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
            vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
            vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
            vkFreeMemory(g_Device, m_indexBufferMemory, allocator);
            m_vertexBuffer = VK_NULL_HANDLE;
            m_vertexBufferMemory = VK_NULL_HANDLE;
            m_indexBuffer = VK_NULL_HANDLE;
            m_indexBufferMemory = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryRequirements instanceMemRequirements;
        vkGetBufferMemoryRequirements(g_Device, m_instanceBuffers[i], &instanceMemRequirements);

        VkMemoryAllocateInfo instanceAllocInfo{};
        instanceAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        instanceAllocInfo.allocationSize = instanceMemRequirements.size;
        instanceAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(instanceMemRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(g_Device, &instanceAllocInfo, allocator, &m_instanceBufferMemories[i]) != VK_SUCCESS) {
            std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to allocate instance buffer memory!" << std::endl;
            // 清理已创建的缓冲区
            for (size_t j = 0; j < i; j++) {
                if (m_mappedInstancePtrs[j]) {
                    vkUnmapMemory(g_Device, m_instanceBufferMemories[j]);
                }
                if (m_instanceBuffers[j] != VK_NULL_HANDLE) {
                    vkDestroyBuffer(g_Device, m_instanceBuffers[j], allocator);
                }
                if (m_instanceBufferMemories[j] != VK_NULL_HANDLE) {
                    vkFreeMemory(g_Device, m_instanceBufferMemories[j], allocator);
                }
            }
            vkDestroyBuffer(g_Device, m_instanceBuffers[i], allocator);
            vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
            vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
            vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
            vkFreeMemory(g_Device, m_indexBufferMemory, allocator);
            m_vertexBuffer = VK_NULL_HANDLE;
            m_vertexBufferMemory = VK_NULL_HANDLE;
            m_indexBuffer = VK_NULL_HANDLE;
            m_indexBufferMemory = VK_NULL_HANDLE;
            return false;
        }

        vkBindBufferMemory(g_Device, m_instanceBuffers[i], m_instanceBufferMemories[i], 0);
        if (vkMapMemory(g_Device, m_instanceBufferMemories[i], 0, instanceBufferSize, 0, &m_mappedInstancePtrs[i]) != VK_SUCCESS) {
            std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to map instance buffer memory!" << std::endl;
            // 清理已创建的缓冲区
            for (size_t j = 0; j < i; j++) {
                if (m_mappedInstancePtrs[j]) {
                    vkUnmapMemory(g_Device, m_instanceBufferMemories[j]);
                }
                if (m_instanceBuffers[j] != VK_NULL_HANDLE) {
                    vkDestroyBuffer(g_Device, m_instanceBuffers[j], allocator);
                }
                if (m_instanceBufferMemories[j] != VK_NULL_HANDLE) {
                    vkFreeMemory(g_Device, m_instanceBufferMemories[j], allocator);
                }
            }
            vkUnmapMemory(g_Device, m_instanceBufferMemories[i]);
            vkDestroyBuffer(g_Device, m_instanceBuffers[i], allocator);
            vkFreeMemory(g_Device, m_instanceBufferMemories[i], allocator);
            vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
            vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
            vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
            vkFreeMemory(g_Device, m_indexBufferMemory, allocator);
            m_vertexBuffer = VK_NULL_HANDLE;
            m_vertexBufferMemory = VK_NULL_HANDLE;
            m_indexBuffer = VK_NULL_HANDLE;
            m_indexBufferMemory = VK_NULL_HANDLE;
            return false;
        }
    }

    // 创建绘制命令缓冲区（双缓冲，用于 CPU 生成绘制命令）
    // 使用 FaceDrawCommand 结构体，每个模型 6 个面命令
    VkDeviceSize drawCommandBufferSize = sizeof(FaceDrawCommand) * maxVoxelModels * 6;
    VkBufferCreateInfo drawCommandBufferInfo{};
    drawCommandBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    drawCommandBufferInfo.size = drawCommandBufferSize;
    drawCommandBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;  // storage buffer 供 shader 读取
    drawCommandBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    for (size_t i = 0; i < 2; i++) {
        if (vkCreateBuffer(g_Device, &drawCommandBufferInfo, allocator, &m_drawCommandBuffers[i]) != VK_SUCCESS) {
            std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create draw command buffer!" << std::endl;
            // 清理已创建的缓冲区
            for (size_t j = 0; j < 2; j++) {
                if (m_mappedInstancePtrs[j]) {
                    vkUnmapMemory(g_Device, m_instanceBufferMemories[j]);
                }
                if (m_instanceBuffers[j] != VK_NULL_HANDLE) {
                    vkDestroyBuffer(g_Device, m_instanceBuffers[j], allocator);
                }
                if (m_instanceBufferMemories[j] != VK_NULL_HANDLE) {
                    vkFreeMemory(g_Device, m_instanceBufferMemories[j], allocator);
                }
            }
            for (size_t j = 0; j < i; j++) {
                if (m_mappedDrawCommandPtrs[j]) {
                    vkUnmapMemory(g_Device, m_drawCommandBufferMemories[j]);
                }
                if (m_drawCommandBuffers[j] != VK_NULL_HANDLE) {
                    vkDestroyBuffer(g_Device, m_drawCommandBuffers[j], allocator);
                }
                if (m_drawCommandBufferMemories[j] != VK_NULL_HANDLE) {
                    vkFreeMemory(g_Device, m_drawCommandBufferMemories[j], allocator);
                }
            }
            vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
            vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
            vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
            vkFreeMemory(g_Device, m_indexBufferMemory, allocator);
            m_vertexBuffer = VK_NULL_HANDLE;
            m_vertexBufferMemory = VK_NULL_HANDLE;
            m_indexBuffer = VK_NULL_HANDLE;
            m_indexBufferMemory = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryRequirements drawCommandMemRequirements;
        vkGetBufferMemoryRequirements(g_Device, m_drawCommandBuffers[i], &drawCommandMemRequirements);

        VkMemoryAllocateInfo drawCommandAllocInfo{};
        drawCommandAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        drawCommandAllocInfo.allocationSize = drawCommandMemRequirements.size;
        drawCommandAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(drawCommandMemRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(g_Device, &drawCommandAllocInfo, allocator, &m_drawCommandBufferMemories[i]) != VK_SUCCESS) {
            std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to allocate draw command buffer memory!" << std::endl;
            // 清理已创建的缓冲区
            for (size_t j = 0; j < 2; j++) {
                if (m_mappedInstancePtrs[j]) {
                    vkUnmapMemory(g_Device, m_instanceBufferMemories[j]);
                }
                if (m_instanceBuffers[j] != VK_NULL_HANDLE) {
                    vkDestroyBuffer(g_Device, m_instanceBuffers[j], allocator);
                }
                if (m_instanceBufferMemories[j] != VK_NULL_HANDLE) {
                    vkFreeMemory(g_Device, m_instanceBufferMemories[j], allocator);
                }
            }
            for (size_t j = 0; j < i; j++) {
                if (m_mappedDrawCommandPtrs[j]) {
                    vkUnmapMemory(g_Device, m_drawCommandBufferMemories[j]);
                }
                if (m_drawCommandBuffers[j] != VK_NULL_HANDLE) {
                    vkDestroyBuffer(g_Device, m_drawCommandBuffers[j], allocator);
                }
                if (m_drawCommandBufferMemories[j] != VK_NULL_HANDLE) {
                    vkFreeMemory(g_Device, m_drawCommandBufferMemories[j], allocator);
                }
            }
            vkDestroyBuffer(g_Device, m_drawCommandBuffers[i], allocator);
            vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
            vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
            vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
            vkFreeMemory(g_Device, m_indexBufferMemory, allocator);
            m_vertexBuffer = VK_NULL_HANDLE;
            m_vertexBufferMemory = VK_NULL_HANDLE;
            m_indexBuffer = VK_NULL_HANDLE;
            m_indexBufferMemory = VK_NULL_HANDLE;
            return false;
        }

        vkBindBufferMemory(g_Device, m_drawCommandBuffers[i], m_drawCommandBufferMemories[i], 0);
        if (vkMapMemory(g_Device, m_drawCommandBufferMemories[i], 0, drawCommandBufferSize, 0, &m_mappedDrawCommandPtrs[i]) != VK_SUCCESS) {
            std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to map draw command buffer memory!" << std::endl;
            // 清理已创建的缓冲区
            for (size_t j = 0; j < 2; j++) {
                if (m_mappedInstancePtrs[j]) {
                    vkUnmapMemory(g_Device, m_instanceBufferMemories[j]);
                }
                if (m_instanceBuffers[j] != VK_NULL_HANDLE) {
                    vkDestroyBuffer(g_Device, m_instanceBuffers[j], allocator);
                }
                if (m_instanceBufferMemories[j] != VK_NULL_HANDLE) {
                    vkFreeMemory(g_Device, m_instanceBufferMemories[j], allocator);
                }
            }
            for (size_t j = 0; j < i; j++) {
                if (m_mappedDrawCommandPtrs[j]) {
                    vkUnmapMemory(g_Device, m_drawCommandBufferMemories[j]);
                }
                if (m_drawCommandBuffers[j] != VK_NULL_HANDLE) {
                    vkDestroyBuffer(g_Device, m_drawCommandBuffers[j], allocator);
                }
                if (m_drawCommandBufferMemories[j] != VK_NULL_HANDLE) {
                    vkFreeMemory(g_Device, m_drawCommandBufferMemories[j], allocator);
                }
            }
            vkUnmapMemory(g_Device, m_drawCommandBufferMemories[i]);
            vkDestroyBuffer(g_Device, m_drawCommandBuffers[i], allocator);
            vkFreeMemory(g_Device, m_drawCommandBufferMemories[i], allocator);
            vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
            vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
            vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
            vkFreeMemory(g_Device, m_indexBufferMemory, allocator);
            m_vertexBuffer = VK_NULL_HANDLE;
            m_vertexBufferMemory = VK_NULL_HANDLE;
            m_indexBuffer = VK_NULL_HANDLE;
            m_indexBufferMemory = VK_NULL_HANDLE;
            return false;
        }
    }

    // 创建可见绘制命令缓冲区（GPU 可写，用于计算着色器输出）
    // 修改：使用 FaceDrawCommand 结构体，大小为 maxVoxelModels * 6
    VkDeviceSize visibleDrawCommandBufferSize = sizeof(FaceDrawCommand) * maxVoxelModels * 6;
    VkBufferCreateInfo visibleDrawCommandBufferInfo{};
    visibleDrawCommandBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    visibleDrawCommandBufferInfo.size = visibleDrawCommandBufferSize;
    visibleDrawCommandBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    visibleDrawCommandBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(g_Device, &visibleDrawCommandBufferInfo, allocator, &m_visibleDrawCommandBuffer) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create visible draw command buffer!" << std::endl;
        vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
        vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
        vkFreeMemory(g_Device, m_indexBufferMemory, allocator);
        
        // 清理双缓冲
        for (size_t i = 0; i < 2; i++) {
            if (m_mappedInstancePtrs[i]) {
                vkUnmapMemory(g_Device, m_instanceBufferMemories[i]);
            }
            if (m_instanceBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_instanceBuffers[i], allocator);
            }
            if (m_instanceBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_instanceBufferMemories[i], allocator);
            }
            if (m_mappedDrawCommandPtrs[i]) {
                vkUnmapMemory(g_Device, m_drawCommandBufferMemories[i]);
            }
            if (m_drawCommandBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_drawCommandBuffers[i], allocator);
            }
            if (m_drawCommandBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_drawCommandBufferMemories[i], allocator);
            }
        }
        
        m_vertexBuffer = VK_NULL_HANDLE;
        m_vertexBufferMemory = VK_NULL_HANDLE;
        m_indexBuffer = VK_NULL_HANDLE;
        m_indexBufferMemory = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements visibleDrawCommandMemRequirements;
    vkGetBufferMemoryRequirements(g_Device, m_visibleDrawCommandBuffer, &visibleDrawCommandMemRequirements);

    VkMemoryAllocateInfo visibleDrawCommandAllocInfo{};
    visibleDrawCommandAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    visibleDrawCommandAllocInfo.allocationSize = visibleDrawCommandMemRequirements.size;
    visibleDrawCommandAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(visibleDrawCommandMemRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(g_Device, &visibleDrawCommandAllocInfo, allocator, &m_visibleDrawCommandBufferMemory) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to allocate visible draw command buffer memory!" << std::endl;
        vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
        vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
        vkFreeMemory(g_Device, m_indexBufferMemory, allocator);
        
        // 清理双缓冲
        for (size_t i = 0; i < 2; i++) {
            if (m_mappedInstancePtrs[i]) {
                vkUnmapMemory(g_Device, m_instanceBufferMemories[i]);
            }
            if (m_instanceBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_instanceBuffers[i], allocator);
            }
            if (m_instanceBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_instanceBufferMemories[i], allocator);
            }
            if (m_mappedDrawCommandPtrs[i]) {
                vkUnmapMemory(g_Device, m_drawCommandBufferMemories[i]);
            }
            if (m_drawCommandBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_drawCommandBuffers[i], allocator);
            }
            if (m_drawCommandBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_drawCommandBufferMemories[i], allocator);
            }
        }
        
        vkDestroyBuffer(g_Device, m_visibleDrawCommandBuffer, allocator);
        m_vertexBuffer = VK_NULL_HANDLE;
        m_vertexBufferMemory = VK_NULL_HANDLE;
        m_indexBuffer = VK_NULL_HANDLE;
        m_indexBufferMemory = VK_NULL_HANDLE;
        m_visibleDrawCommandBuffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(g_Device, m_visibleDrawCommandBuffer, m_visibleDrawCommandBufferMemory, 0);

    // 创建计数器缓冲区（用于计算着色器）
    VkDeviceSize counterBufferSize = sizeof(uint32_t);
    VkBufferCreateInfo counterBufferInfo{};
    counterBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    counterBufferInfo.size = counterBufferSize;
    counterBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    counterBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(g_Device, &counterBufferInfo, allocator, &m_counterBuffer) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create counter buffer!" << std::endl;
        vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
        vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
        vkFreeMemory(g_Device, m_indexBufferMemory, allocator);
        
        // 清理双缓冲
        for (size_t i = 0; i < 2; i++) {
            if (m_mappedInstancePtrs[i]) {
                vkUnmapMemory(g_Device, m_instanceBufferMemories[i]);
            }
            if (m_instanceBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_instanceBuffers[i], allocator);
            }
            if (m_instanceBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_instanceBufferMemories[i], allocator);
            }
            if (m_mappedDrawCommandPtrs[i]) {
                vkUnmapMemory(g_Device, m_drawCommandBufferMemories[i]);
            }
            if (m_drawCommandBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_drawCommandBuffers[i], allocator);
            }
            if (m_drawCommandBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_drawCommandBufferMemories[i], allocator);
            }
        }
        
        vkDestroyBuffer(g_Device, m_visibleDrawCommandBuffer, allocator);
        vkFreeMemory(g_Device, m_visibleDrawCommandBufferMemory, allocator);
        m_vertexBuffer = VK_NULL_HANDLE;
        m_vertexBufferMemory = VK_NULL_HANDLE;
        m_indexBuffer = VK_NULL_HANDLE;
        m_indexBufferMemory = VK_NULL_HANDLE;
        m_visibleDrawCommandBuffer = VK_NULL_HANDLE;
        m_visibleDrawCommandBufferMemory = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements counterMemRequirements;
    vkGetBufferMemoryRequirements(g_Device, m_counterBuffer, &counterMemRequirements);

    VkMemoryAllocateInfo counterAllocInfo{};
    counterAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    counterAllocInfo.allocationSize = counterMemRequirements.size;
    counterAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(counterMemRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(g_Device, &counterAllocInfo, allocator, &m_counterBufferMemory) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to allocate counter buffer memory!" << std::endl;
        vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
        vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
        vkFreeMemory(g_Device, m_indexBufferMemory, allocator);
        
        // 清理双缓冲
        for (size_t i = 0; i < 2; i++) {
            if (m_mappedInstancePtrs[i]) {
                vkUnmapMemory(g_Device, m_instanceBufferMemories[i]);
            }
            if (m_instanceBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_instanceBuffers[i], allocator);
            }
            if (m_instanceBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_instanceBufferMemories[i], allocator);
            }
            if (m_mappedDrawCommandPtrs[i]) {
                vkUnmapMemory(g_Device, m_drawCommandBufferMemories[i]);
            }
            if (m_drawCommandBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_drawCommandBuffers[i], allocator);
            }
            if (m_drawCommandBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_drawCommandBufferMemories[i], allocator);
            }
        }
        
        vkDestroyBuffer(g_Device, m_visibleDrawCommandBuffer, allocator);
        vkFreeMemory(g_Device, m_visibleDrawCommandBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_counterBuffer, allocator);
        m_vertexBuffer = VK_NULL_HANDLE;
        m_vertexBufferMemory = VK_NULL_HANDLE;
        m_indexBuffer = VK_NULL_HANDLE;
        m_indexBufferMemory = VK_NULL_HANDLE;
        m_visibleDrawCommandBuffer = VK_NULL_HANDLE;
        m_visibleDrawCommandBufferMemory = VK_NULL_HANDLE;
        m_counterBuffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(g_Device, m_counterBuffer, m_counterBufferMemory, 0);
    if (vkMapMemory(g_Device, m_counterBufferMemory, 0, counterBufferSize, 0, reinterpret_cast<void**>(&m_mappedCounterPtr)) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to map counter buffer memory!" << std::endl;
        vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
        vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
        vkFreeMemory(g_Device, m_indexBufferMemory, allocator);
        
        // 清理双缓冲
        for (size_t i = 0; i < 2; i++) {
            if (m_mappedInstancePtrs[i]) {
                vkUnmapMemory(g_Device, m_instanceBufferMemories[i]);
            }
            if (m_instanceBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_instanceBuffers[i], allocator);
            }
            if (m_instanceBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_instanceBufferMemories[i], allocator);
            }
            if (m_mappedDrawCommandPtrs[i]) {
                vkUnmapMemory(g_Device, m_drawCommandBufferMemories[i]);
            }
            if (m_drawCommandBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_drawCommandBuffers[i], allocator);
            }
            if (m_drawCommandBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_drawCommandBufferMemories[i], allocator);
            }
        }
        
        vkDestroyBuffer(g_Device, m_visibleDrawCommandBuffer, allocator);
        vkFreeMemory(g_Device, m_visibleDrawCommandBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_counterBuffer, allocator);
        vkFreeMemory(g_Device, m_counterBufferMemory, allocator);
        m_vertexBuffer = VK_NULL_HANDLE;
        m_vertexBufferMemory = VK_NULL_HANDLE;
        m_indexBuffer = VK_NULL_HANDLE;
        m_indexBufferMemory = VK_NULL_HANDLE;
        m_visibleDrawCommandBuffer = VK_NULL_HANDLE;
        m_visibleDrawCommandBufferMemory = VK_NULL_HANDLE;
        m_counterBuffer = VK_NULL_HANDLE;
        m_counterBufferMemory = VK_NULL_HANDLE;
        m_mappedCounterPtr = nullptr;
        return false;
    }

    // 创建相机数据缓冲区
    VkDeviceSize cameraBufferSize = sizeof(CullingCameraData);
    VkBufferCreateInfo cameraBufferInfo{};
    cameraBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    cameraBufferInfo.size = cameraBufferSize;
    cameraBufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    cameraBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(g_Device, &cameraBufferInfo, allocator, &m_cullingCameraBuffer) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create camera buffer!" << std::endl;
        vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
        vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
        vkFreeMemory(g_Device, m_indexBufferMemory, allocator);
        
        // 清理双缓冲
        for (size_t i = 0; i < 2; i++) {
            if (m_mappedInstancePtrs[i]) {
                vkUnmapMemory(g_Device, m_instanceBufferMemories[i]);
            }
            if (m_instanceBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_instanceBuffers[i], allocator);
            }
            if (m_instanceBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_instanceBufferMemories[i], allocator);
            }
            if (m_mappedDrawCommandPtrs[i]) {
                vkUnmapMemory(g_Device, m_drawCommandBufferMemories[i]);
            }
            if (m_drawCommandBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_drawCommandBuffers[i], allocator);
            }
            if (m_drawCommandBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_drawCommandBufferMemories[i], allocator);
            }
        }
        
        vkDestroyBuffer(g_Device, m_visibleDrawCommandBuffer, allocator);
        vkFreeMemory(g_Device, m_visibleDrawCommandBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_counterBuffer, allocator);
        vkFreeMemory(g_Device, m_counterBufferMemory, allocator);
        m_vertexBuffer = VK_NULL_HANDLE;
        m_vertexBufferMemory = VK_NULL_HANDLE;
        m_indexBuffer = VK_NULL_HANDLE;
        m_indexBufferMemory = VK_NULL_HANDLE;
        m_visibleDrawCommandBuffer = VK_NULL_HANDLE;
        m_visibleDrawCommandBufferMemory = VK_NULL_HANDLE;
        m_counterBuffer = VK_NULL_HANDLE;
        m_counterBufferMemory = VK_NULL_HANDLE;
        m_cullingCameraBuffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements cameraMemRequirements;
    vkGetBufferMemoryRequirements(g_Device, m_cullingCameraBuffer, &cameraMemRequirements);

    VkMemoryAllocateInfo cameraAllocInfo{};
    cameraAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    cameraAllocInfo.allocationSize = cameraMemRequirements.size;
    cameraAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(cameraMemRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(g_Device, &cameraAllocInfo, allocator, &m_cullingCameraBufferMemory) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to allocate camera buffer memory!" << std::endl;
        vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
        vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
        vkFreeMemory(g_Device, m_indexBufferMemory, allocator);
        
        // 清理双缓冲
        for (size_t i = 0; i < 2; i++) {
            if (m_mappedInstancePtrs[i]) {
                vkUnmapMemory(g_Device, m_instanceBufferMemories[i]);
            }
            if (m_instanceBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_instanceBuffers[i], allocator);
            }
            if (m_instanceBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_instanceBufferMemories[i], allocator);
            }
            if (m_mappedDrawCommandPtrs[i]) {
                vkUnmapMemory(g_Device, m_drawCommandBufferMemories[i]);
            }
            if (m_drawCommandBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_drawCommandBuffers[i], allocator);
            }
            if (m_drawCommandBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_drawCommandBufferMemories[i], allocator);
            }
        }
        
        vkDestroyBuffer(g_Device, m_visibleDrawCommandBuffer, allocator);
        vkFreeMemory(g_Device, m_visibleDrawCommandBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_counterBuffer, allocator);
        vkFreeMemory(g_Device, m_counterBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_cullingCameraBuffer, allocator);
        m_vertexBuffer = VK_NULL_HANDLE;
        m_vertexBufferMemory = VK_NULL_HANDLE;
        m_indexBuffer = VK_NULL_HANDLE;
        m_indexBufferMemory = VK_NULL_HANDLE;
        m_visibleDrawCommandBuffer = VK_NULL_HANDLE;
        m_visibleDrawCommandBufferMemory = VK_NULL_HANDLE;
        m_counterBuffer = VK_NULL_HANDLE;
        m_counterBufferMemory = VK_NULL_HANDLE;
        m_cullingCameraBuffer = VK_NULL_HANDLE;
        m_cullingCameraBufferMemory = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(g_Device, m_cullingCameraBuffer, m_cullingCameraBufferMemory, 0);
    if (vkMapMemory(g_Device, m_cullingCameraBufferMemory, 0, cameraBufferSize, 0, reinterpret_cast<void**>(&m_mappedCameraPtr)) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to map camera buffer memory!" << std::endl;
        vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
        vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
        vkFreeMemory(g_Device, m_indexBufferMemory, allocator);
        
        // 清理双缓冲
        for (size_t i = 0; i < 2; i++) {
            if (m_mappedInstancePtrs[i]) {
                vkUnmapMemory(g_Device, m_instanceBufferMemories[i]);
            }
            if (m_instanceBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_instanceBuffers[i], allocator);
            }
            if (m_instanceBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_instanceBufferMemories[i], allocator);
            }
            if (m_mappedDrawCommandPtrs[i]) {
                vkUnmapMemory(g_Device, m_drawCommandBufferMemories[i]);
            }
            if (m_drawCommandBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_drawCommandBuffers[i], allocator);
            }
            if (m_drawCommandBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_drawCommandBufferMemories[i], allocator);
            }
        }
        
        vkDestroyBuffer(g_Device, m_visibleDrawCommandBuffer, allocator);
        vkFreeMemory(g_Device, m_visibleDrawCommandBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_counterBuffer, allocator);
        vkFreeMemory(g_Device, m_counterBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_cullingCameraBuffer, allocator);
        vkFreeMemory(g_Device, m_cullingCameraBufferMemory, allocator);
        m_vertexBuffer = VK_NULL_HANDLE;
        m_vertexBufferMemory = VK_NULL_HANDLE;
        m_indexBuffer = VK_NULL_HANDLE;
        m_indexBufferMemory = VK_NULL_HANDLE;
        m_visibleDrawCommandBuffer = VK_NULL_HANDLE;
        m_visibleDrawCommandBufferMemory = VK_NULL_HANDLE;
        m_counterBuffer = VK_NULL_HANDLE;
        m_counterBufferMemory = VK_NULL_HANDLE;
        m_cullingCameraBuffer = VK_NULL_HANDLE;
        m_cullingCameraBufferMemory = VK_NULL_HANDLE;
        m_mappedCameraPtr = nullptr;
        return false;
    }

    return true;
}

void VoxelMeshMultiDrawIndirect::AddVoxelModel(void* entityId, const std::string& voxPath, const VoxRenderer* renderer, const glm::mat4& transform, const glm::vec4& color)
{
    // 检查是否需要扩容实例和绘制命令缓冲区
    if (m_totalInstances >= m_maxVoxelModels) {
        // 双倍扩容
        size_t newMaxVoxelModels = m_maxVoxelModels * 2;
        std::cout << "[VoxelMeshMultiDrawIndirect] Resizing instance and draw command buffers: " << m_maxVoxelModels << " -> " << newMaxVoxelModels << std::endl;
        
        // 确保GPU已经完成了对旧缓冲区的使用
        vkQueueWaitIdle(g_Queue);
        
        // 销毁旧的双缓冲
        for (size_t i = 0; i < 2; i++) {
            if (m_mappedInstancePtrs[i]) {
                vkUnmapMemory(g_Device, m_instanceBufferMemories[i]);
            }
            if (m_instanceBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_instanceBuffers[i], g_Allocator);
            }
            if (m_instanceBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_instanceBufferMemories[i], g_Allocator);
            }
            if (m_mappedDrawCommandPtrs[i]) {
                vkUnmapMemory(g_Device, m_drawCommandBufferMemories[i]);
            }
            if (m_drawCommandBuffers[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(g_Device, m_drawCommandBuffers[i], g_Allocator);
            }
            if (m_drawCommandBufferMemories[i] != VK_NULL_HANDLE) {
                vkFreeMemory(g_Device, m_drawCommandBufferMemories[i], g_Allocator);
            }
        }
        
        // 创建新的实例数据缓冲区（双缓冲）
        VkDeviceSize instanceBufferSize = sizeof(InstanceData) * newMaxVoxelModels;
        VkBufferCreateInfo instanceBufferInfo{};
        instanceBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        instanceBufferInfo.size = instanceBufferSize;
        instanceBufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        instanceBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        for (size_t i = 0; i < 2; i++) {
            if (vkCreateBuffer(g_Device, &instanceBufferInfo, g_Allocator, &m_instanceBuffers[i]) != VK_SUCCESS) {
                std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to resize instance buffer!" << std::endl;
                // 清理已创建的缓冲区
                for (size_t j = 0; j < i; j++) {
                    if (m_mappedInstancePtrs[j]) {
                        vkUnmapMemory(g_Device, m_instanceBufferMemories[j]);
                    }
                    if (m_instanceBuffers[j] != VK_NULL_HANDLE) {
                        vkDestroyBuffer(g_Device, m_instanceBuffers[j], g_Allocator);
                    }
                    if (m_instanceBufferMemories[j] != VK_NULL_HANDLE) {
                        vkFreeMemory(g_Device, m_instanceBufferMemories[j], g_Allocator);
                    }
                }
                return;
            }
            
            VkMemoryRequirements instanceMemRequirements;
            vkGetBufferMemoryRequirements(g_Device, m_instanceBuffers[i], &instanceMemRequirements);
            
            VkMemoryAllocateInfo instanceAllocInfo{};
            instanceAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            instanceAllocInfo.allocationSize = instanceMemRequirements.size;
            instanceAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(instanceMemRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            
            if (vkAllocateMemory(g_Device, &instanceAllocInfo, g_Allocator, &m_instanceBufferMemories[i]) != VK_SUCCESS) {
                std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to allocate resized instance buffer memory!" << std::endl;
                // 清理已创建的缓冲区
                for (size_t j = 0; j < i; j++) {
                    if (m_mappedInstancePtrs[j]) {
                        vkUnmapMemory(g_Device, m_instanceBufferMemories[j]);
                    }
                    if (m_instanceBuffers[j] != VK_NULL_HANDLE) {
                        vkDestroyBuffer(g_Device, m_instanceBuffers[j], g_Allocator);
                    }
                    if (m_instanceBufferMemories[j] != VK_NULL_HANDLE) {
                        vkFreeMemory(g_Device, m_instanceBufferMemories[j], g_Allocator);
                    }
                }
                vkDestroyBuffer(g_Device, m_instanceBuffers[i], g_Allocator);
                return;
            }
            
            vkBindBufferMemory(g_Device, m_instanceBuffers[i], m_instanceBufferMemories[i], 0);
            if (vkMapMemory(g_Device, m_instanceBufferMemories[i], 0, instanceBufferSize, 0, &m_mappedInstancePtrs[i]) != VK_SUCCESS) {
                std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to map resized instance buffer memory!" << std::endl;
                // 清理已创建的缓冲区
                for (size_t j = 0; j < i; j++) {
                    if (m_mappedInstancePtrs[j]) {
                        vkUnmapMemory(g_Device, m_instanceBufferMemories[j]);
                    }
                    if (m_instanceBuffers[j] != VK_NULL_HANDLE) {
                        vkDestroyBuffer(g_Device, m_instanceBuffers[j], g_Allocator);
                    }
                    if (m_instanceBufferMemories[j] != VK_NULL_HANDLE) {
                        vkFreeMemory(g_Device, m_instanceBufferMemories[j], g_Allocator);
                    }
                }
                vkUnmapMemory(g_Device, m_instanceBufferMemories[i]);
                vkDestroyBuffer(g_Device, m_instanceBuffers[i], g_Allocator);
                vkFreeMemory(g_Device, m_instanceBufferMemories[i], g_Allocator);
                return;
            }
        }
        
        // 创建新的绘制命令缓冲区（双缓冲）
        // 使用FaceDrawCommand结构体，每个模型6个面命令
        VkDeviceSize drawCommandBufferSize = sizeof(FaceDrawCommand) * newMaxVoxelModels * 6;
        VkBufferCreateInfo drawCommandBufferInfo{};
        drawCommandBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        drawCommandBufferInfo.size = drawCommandBufferSize;
        drawCommandBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        drawCommandBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        for (size_t i = 0; i < 2; i++) {
            if (vkCreateBuffer(g_Device, &drawCommandBufferInfo, g_Allocator, &m_drawCommandBuffers[i]) != VK_SUCCESS) {
                std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to resize draw command buffer!" << std::endl;
                // 清理已创建的缓冲区
                for (size_t j = 0; j < 2; j++) {
                    if (m_mappedInstancePtrs[j]) {
                        vkUnmapMemory(g_Device, m_instanceBufferMemories[j]);
                    }
                    if (m_instanceBuffers[j] != VK_NULL_HANDLE) {
                        vkDestroyBuffer(g_Device, m_instanceBuffers[j], g_Allocator);
                    }
                    if (m_instanceBufferMemories[j] != VK_NULL_HANDLE) {
                        vkFreeMemory(g_Device, m_instanceBufferMemories[j], g_Allocator);
                    }
                }
                return;
            }
            
            VkMemoryRequirements drawCommandMemRequirements;
            vkGetBufferMemoryRequirements(g_Device, m_drawCommandBuffers[i], &drawCommandMemRequirements);
            
            VkMemoryAllocateInfo drawCommandAllocInfo{};
            drawCommandAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            drawCommandAllocInfo.allocationSize = drawCommandMemRequirements.size;
            drawCommandAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(drawCommandMemRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            
            if (vkAllocateMemory(g_Device, &drawCommandAllocInfo, g_Allocator, &m_drawCommandBufferMemories[i]) != VK_SUCCESS) {
                std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to allocate resized draw command buffer memory!" << std::endl;
                // 清理已创建的缓冲区
                for (size_t j = 0; j < 2; j++) {
                    if (m_mappedInstancePtrs[j]) {
                        vkUnmapMemory(g_Device, m_instanceBufferMemories[j]);
                    }
                    if (m_instanceBuffers[j] != VK_NULL_HANDLE) {
                        vkDestroyBuffer(g_Device, m_instanceBuffers[j], g_Allocator);
                    }
                    if (m_instanceBufferMemories[j] != VK_NULL_HANDLE) {
                        vkFreeMemory(g_Device, m_instanceBufferMemories[j], g_Allocator);
                    }
                }
                vkDestroyBuffer(g_Device, m_drawCommandBuffers[i], g_Allocator);
                return;
            }
            
            vkBindBufferMemory(g_Device, m_drawCommandBuffers[i], m_drawCommandBufferMemories[i], 0);
            if (vkMapMemory(g_Device, m_drawCommandBufferMemories[i], 0, drawCommandBufferSize, 0, &m_mappedDrawCommandPtrs[i]) != VK_SUCCESS) {
                std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to map resized draw command buffer memory!" << std::endl;
                // 清理已创建的缓冲区
                for (size_t j = 0; j < 2; j++) {
                    if (m_mappedInstancePtrs[j]) {
                        vkUnmapMemory(g_Device, m_instanceBufferMemories[j]);
                    }
                    if (m_instanceBuffers[j] != VK_NULL_HANDLE) {
                        vkDestroyBuffer(g_Device, m_instanceBuffers[j], g_Allocator);
                    }
                    if (m_instanceBufferMemories[j] != VK_NULL_HANDLE) {
                        vkFreeMemory(g_Device, m_instanceBufferMemories[j], g_Allocator);
                    }
                }
                vkUnmapMemory(g_Device, m_drawCommandBufferMemories[i]);
                vkDestroyBuffer(g_Device, m_drawCommandBuffers[i], g_Allocator);
                vkFreeMemory(g_Device, m_drawCommandBufferMemories[i], g_Allocator);
                return;
            }
        }
        
        // 更新最大模型数量
        m_maxVoxelModels = newMaxVoxelModels;
    }

    VoxelModelData modelData;
    modelData.entityId = entityId;
    modelData.voxPath = voxPath;
    modelData.transform = transform;
    modelData.color = color;
    modelData.firstInstance = m_totalInstances;
    modelData.instanceCount = 1;
    modelData.visible = true;  // 新添加的模型默认可见
    
    // 初始化脏标记
    modelData.transformDirty = true;
    modelData.colorDirty = true;
    modelData.staticDirty = true;
    
    // 暂时设置为 0，在 MergeGeometryData 中会更新
    modelData.firstVertex = 0;
    modelData.vertexCount = 0;
    modelData.firstIndex = 0;
    modelData.indexCount = 0;
    
    // 查找或创建对应 voxPath 的分组
    bool found = false;
    for (auto& group : m_rendererGroups) {
        if (group.voxPath == voxPath) {
            group.models.push_back(modelData);
            group.totalInstances++;
            found = true;
            break;
        }
    }
    
    if (!found) {
        RendererGroup newGroup;
        newGroup.voxPath = voxPath;
        newGroup.renderer = renderer;
        newGroup.models.push_back(modelData);
        newGroup.totalInstances = 1;
        m_rendererGroups.push_back(newGroup);
    }
    
    // 更新总实例数量
    m_totalInstances += modelData.instanceCount;
    
    m_geometryDataDirty = true;
}

void VoxelMeshMultiDrawIndirect::UpdateVoxelModel(void* entityId, const std::string& voxPath, const VoxRenderer* renderer, const glm::mat4& transform, const glm::vec4& color, bool visible)
{
    // 查找对应的分组和模型（使用 entityId 作为唯一标识）
    for (auto& group : m_rendererGroups) {
        for (auto& modelData : group.models) {
            if (modelData.entityId == entityId) {
                // 更新可见性标记
                modelData.visible = visible;
                
                // 如果是可见模型，添加到可见列表（在 UpdateDrawCommandsAndInstanceData 中使用）
                // 注意：不在这里直接处理，因为需要等待所有 UpdateVoxelModel 调用完成
                
                // 检查数据是否变化
                if (modelData.transform != transform) {
                    modelData.transform = transform;
                    modelData.transformDirty = true;
                    m_geometryDataDirty = true;  // 标记几何数据需要更新
        // [cleaned per-frame test output] std::cout << "[MDI] UpdateVoxelModel: transform changed!" << std::endl;
                }
                if (modelData.color != color) {
                    modelData.color = color;
                    modelData.colorDirty = true;
                    m_geometryDataDirty = true;  // 标记几何数据需要更新
        // [cleaned per-frame test output] std::cout << "[MDI] UpdateVoxelModel: color changed!" << std::endl;
                }
                return;  // 找到并更新后直接返回
            }
        }
    }
    
    // 如果没找到，说明还没有添加这个模型，调用 AddVoxelModel
        // [cleaned per-frame test output] std::cout << "[MDI] UpdateVoxelModel: model not found, calling AddVoxelModel!" << std::endl;
    AddVoxelModel(entityId, voxPath, renderer, transform, color);
}



// 执行 GPU 剔除
void VoxelMeshMultiDrawIndirect::ExecuteGPUCulling(VkCommandBuffer commandBuffer,
                                                    const glm::mat4& projView,
                                                    const glm::mat4& prevProjView,
                                                    const glm::mat4& cullProjView,
                                                    const glm::vec3& cameraPosition,
                                                    bool useDualFrustumCulling,
                                                    bool enableBackfaceCulling,
                                                    bool enableHiZCulling)
{
    // 添加额外的安全检查
    if (!m_supportsComputeShader) {
        // [cleaned per-frame test output] std::cout << "[VoxelMeshMultiDrawIndirect] GPU culling skipped: compute shader not supported" << std::endl;
        return;
    }
    if (!m_mappedCameraPtr) {
        // [cleaned per-frame test output] std::cout << "[VoxelMeshMultiDrawIndirect] GPU culling skipped: camera buffer not mapped" << std::endl;
        return;
    }
    if (m_totalInstances == 0) {
        // [cleaned per-frame test output] std::cout << "[VoxelMeshMultiDrawIndirect] GPU culling skipped: no instances to cull" << std::endl;
        return;
    }
    
    // 检查关键资源是否有效
    if (m_cullingPipeline == VK_NULL_HANDLE) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] GPU culling skipped: culling pipeline not initialized!" << std::endl;
        return;
    }
    if (m_cullingDescriptorSet == VK_NULL_HANDLE) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] GPU culling skipped: descriptor set not initialized!" << std::endl;
        return;
    }
    
    if (m_visibleDrawCommandBuffer == VK_NULL_HANDLE) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] GPU culling skipped: visible draw command buffer not initialized!" << std::endl;
        return;
    }
    
    // std::cout << "[VoxelMeshMultiDrawIndirect] Executing GPU culling with " << m_totalInstances << " instances (Hi-Z: " << enableHiZCulling << ")" << std::endl;
    
    // 1. 更新相机数据（完整版本）
    m_mappedCameraPtr->totalFaceCommands = static_cast<uint32_t>(m_totalInstances * 6);  // 每个模型 6 个面
    m_mappedCameraPtr->enableHiZCulling = enableHiZCulling ? 1 : 0;
    m_mappedCameraPtr->screenWidth = EngineConfig::WINDOW_WIDTH;
    m_mappedCameraPtr->screenHeight = EngineConfig::WINDOW_HEIGHT;
    
    // 设置相机位置
    m_mappedCameraPtr->viewPos = glm::vec4(cameraPosition, 1.0f);
    
    // 设置视图投影矩阵
    m_mappedCameraPtr->viewProj = cullProjView;
    
    // 设置上一帧的视图投影矩阵（用于Hi-Z遮挡测试）
    // 注意：这里使用prevProjView，即上一帧相机的viewProj
    // 这样HiZ测试时使用的是上一帧的视角来测试当前帧的voxels
    m_mappedCameraPtr->prevViewProj = prevProjView;
    
    // 提取视锥体平面
    std::array<Plane, 6> frustumPlanes = AABBUtils::ExtractFrustumPlanes(cullProjView);
    for (int i = 0; i < 6; i++) {
        m_mappedCameraPtr->frustumPlanes[i] = glm::vec4(frustumPlanes[i].normal, frustumPlanes[i].distance);
    }
    
    // 2. 每帧更新描述符集（确保 Hi-Z 视图是最新的）
    VkDescriptorBufferInfo cameraBufferInfo{};
    cameraBufferInfo.buffer = m_cullingCameraBuffer;
    cameraBufferInfo.offset = 0;
    cameraBufferInfo.range = sizeof(CullingCameraData);
    
    VkDescriptorBufferInfo drawCommandBufferInfo{};
    drawCommandBufferInfo.buffer = m_drawCommandBuffers[m_currentInstanceBufferIndex];
    drawCommandBufferInfo.offset = 0;
    drawCommandBufferInfo.range = VK_WHOLE_SIZE;
    
    VkDescriptorBufferInfo visibleDrawBufferInfo{};
    visibleDrawBufferInfo.buffer = m_visibleDrawCommandBuffer;
    visibleDrawBufferInfo.offset = 0;
    visibleDrawBufferInfo.range = VK_WHOLE_SIZE;
    
    VkDescriptorBufferInfo instanceDataBufferInfo{};
    instanceDataBufferInfo.buffer = m_instanceBuffers[m_currentInstanceBufferIndex];
    instanceDataBufferInfo.offset = 0;
    instanceDataBufferInfo.range = VK_WHOLE_SIZE;
    
    VkDescriptorImageInfo hizImageInfo{};
    hizImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    
    // 当 Hi-Z 可用时使用真实的 Hi-Z 纹理,否则使用 dummy 图像
    VkImageView hizView = g_SceneRenderer.GetHiZShader().GetHiZTextureViewForCulling();
    if (hizView != VK_NULL_HANDLE) {
        hizImageInfo.imageView = hizView;
    } else {
        hizImageInfo.imageView = m_dummyHiZImageView;
    }
    hizImageInfo.sampler = g_GameRenderTarget.GetHiZSampler();
    
    // 验证所有资源有效性
    if (cameraBufferInfo.buffer == VK_NULL_HANDLE) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] ERROR: camera buffer is null!" << std::endl;
        return;
    }
    if (drawCommandBufferInfo.buffer == VK_NULL_HANDLE) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] ERROR: draw command buffer is null!" << std::endl;
        return;
    }
    if (visibleDrawBufferInfo.buffer == VK_NULL_HANDLE) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] ERROR: visible draw buffer is null!" << std::endl;
        return;
    }
    if (instanceDataBufferInfo.buffer == VK_NULL_HANDLE) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] ERROR: instance data buffer is null!" << std::endl;
        return;
    }
    if (hizImageInfo.imageView == VK_NULL_HANDLE) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] ERROR: Hi-Z image view is null!" << std::endl;
        return;
    }
    if (hizImageInfo.sampler == VK_NULL_HANDLE) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] ERROR: Hi-Z sampler is null!" << std::endl;
        return;
    }
    
    VkWriteDescriptorSet descriptorWrites[5] = {};
    
    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].pNext = nullptr;
    descriptorWrites[0].dstSet = m_cullingDescriptorSet;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptorWrites[0].pBufferInfo = &cameraBufferInfo;
    descriptorWrites[0].pImageInfo = nullptr;
    descriptorWrites[0].pTexelBufferView = nullptr;
    
    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].pNext = nullptr;
    descriptorWrites[1].dstSet = m_cullingDescriptorSet;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrites[1].pBufferInfo = &drawCommandBufferInfo;
    descriptorWrites[1].pImageInfo = nullptr;
    descriptorWrites[1].pTexelBufferView = nullptr;
    
    descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[2].pNext = nullptr;
    descriptorWrites[2].dstSet = m_cullingDescriptorSet;
    descriptorWrites[2].dstBinding = 2;
    descriptorWrites[2].dstArrayElement = 0;
    descriptorWrites[2].descriptorCount = 1;
    descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrites[2].pBufferInfo = &visibleDrawBufferInfo;
    descriptorWrites[2].pImageInfo = nullptr;
    descriptorWrites[2].pTexelBufferView = nullptr;
    
    descriptorWrites[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[3].pNext = nullptr;
    descriptorWrites[3].dstSet = m_cullingDescriptorSet;
    descriptorWrites[3].dstBinding = 3;
    descriptorWrites[3].dstArrayElement = 0;
    descriptorWrites[3].descriptorCount = 1;
    descriptorWrites[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrites[3].pBufferInfo = &instanceDataBufferInfo;
    descriptorWrites[3].pImageInfo = nullptr;
    descriptorWrites[3].pTexelBufferView = nullptr;
    
    // 始终更新所有 5 个 binding，确保描述符集完整初始化
    descriptorWrites[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[4].pNext = nullptr;
    descriptorWrites[4].dstSet = m_cullingDescriptorSet;
    descriptorWrites[4].dstBinding = 4;
    descriptorWrites[4].dstArrayElement = 0;
    descriptorWrites[4].descriptorCount = 1;
    descriptorWrites[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[4].pImageInfo = &hizImageInfo;
    descriptorWrites[4].pBufferInfo = nullptr;
    descriptorWrites[4].pTexelBufferView = nullptr;
    
    vkUpdateDescriptorSets(g_Device, 5, descriptorWrites, 0, nullptr);
    
    // 3. 绑定计算管线和描述符集
        // [cleaned per-frame test output] std::cout << "[VoxelMeshMultiDrawIndirect] Binding pipeline and descriptor set..." << std::endl;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_cullingPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, 
                           m_cullingPipelineLayout, 0, 1, &m_cullingDescriptorSet, 0, nullptr);
        // [cleaned per-frame test output] std::cout << "[VoxelMeshMultiDrawIndirect] Pipeline and descriptor set bound successfully" << std::endl;
    
    // 4. 计算需要的工作组数量
    // 修改：基于 totalFaceCommands 而不是 totalInstances
    uint32_t totalFaceCommands = m_mappedCameraPtr ? m_mappedCameraPtr->totalFaceCommands : 0;
    uint32_t workGroupCount = static_cast<uint32_t>((totalFaceCommands + 63) / 64); // local_size_x = 64
    
    // 添加调试日志（已清理：每帧测试输出）
    // std::cout << "[VoxelMeshMultiDrawIndirect] Dispatching compute shader: workGroupCount=" 
    //       << workGroupCount << ", totalFaceCommands=" << totalFaceCommands << std::endl;
    
    // 添加缓冲区大小检查（已清理：每帧测试输出）
    // std::cout << "[VoxelMeshMultiDrawIndirect] Buffer sizes - drawCommand: " 
    //       << (drawCommandBufferInfo.range == VK_WHOLE_SIZE ? "WHOLE_SIZE" : std::to_string(drawCommandBufferInfo.range))
    //       << ", visibleDraw: " 
    //       << (visibleDrawBufferInfo.range == VK_WHOLE_SIZE ? "WHOLE_SIZE" : std::to_string(visibleDrawBufferInfo.range))
    //       << ", instanceData: " 
    //       << (instanceDataBufferInfo.range == VK_WHOLE_SIZE ? "WHOLE_SIZE" : std::to_string(instanceDataBufferInfo.range))
    //       << std::endl;
    
    // 5. 分派计算任务
        // [cleaned per-frame test output] std::cout << "[VoxelMeshMultiDrawIndirect] Calling vkCmdDispatch..." << std::endl;
    vkCmdDispatch(commandBuffer, workGroupCount, 1, 1);
        // [cleaned per-frame test output] std::cout << "[VoxelMeshMultiDrawIndirect] vkCmdDispatch completed" << std::endl;
    
    // 添加内存屏障确保计算结果对后续操作可见
    VkMemoryBarrier computeBarrier{};
    computeBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    computeBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    computeBarrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        0,
        1, &computeBarrier,
        0, nullptr,
        0, nullptr
    );
    
    // 6. 添加缓冲区内存屏障：确保计算着色器的写入对后续的绘制操作可见
    VkBufferMemoryBarrier cullingBarrier{};
    cullingBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    cullingBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    cullingBarrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    cullingBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cullingBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cullingBarrier.buffer = m_visibleDrawCommandBuffer;
    cullingBarrier.offset = 0;
    cullingBarrier.size = VK_WHOLE_SIZE;
    
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        0,
        0, nullptr,
        1, &cullingBarrier,
        0, nullptr
    );
    
    // 为计数器缓冲区添加内存屏障
    VkBufferMemoryBarrier counterReadBarrier{};
    counterReadBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    counterReadBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    counterReadBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    counterReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    counterReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    counterReadBarrier.buffer = m_counterBuffer;
    counterReadBarrier.offset = 0;
    counterReadBarrier.size = sizeof(uint32_t);
    
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0,
        0, nullptr,
        1, &counterReadBarrier,
        0, nullptr
    );
}

void VoxelMeshMultiDrawIndirect::MergeGeometryData()
{
    // 如果几何数据不需要更新，直接返回
    if (!m_geometryDataDirty) {
        return;
    }
    
    if (m_rendererGroups.empty()) {
        m_geometryDataDirty = false;
        return;
    }
    
    // g_Allocator 可以为 nullptr，这是正常的，使用默认分配器
    VkAllocationCallbacks* allocator = g_Allocator;
    
    // 计算总顶点和索引数量（考虑网格缓存，避免重复计算）
    m_totalVertices = 0;
    m_totalIndices = 0;
    m_totalInstances = 0;
    
    // 收集需要复制的唯一网格数据
    std::vector<const VoxRenderer*> uniqueRenderers;
    std::unordered_set<const VoxRenderer*> rendererSet;
    size_t totalVertexDataSize = 0;
    size_t totalIndexDataSize = 0;
    
    bool hasNewRenderers = false;
    
    for (const auto& group : m_rendererGroups) {
        const VoxRenderer* renderer = group.renderer;
        if (rendererSet.find(renderer) == rendererSet.end()) {
            rendererSet.insert(renderer);
            uniqueRenderers.push_back(renderer);
            // 检查是否是新的渲染器（不在缓存中）
            if (m_meshCache.find(renderer) == m_meshCache.end()) {
                hasNewRenderers = true;
            }
            const VoxelMeshData& meshData = renderer->GetMeshData();
            m_totalVertices += meshData.vertexCount;
            m_totalIndices += meshData.indexCount;
            totalVertexDataSize += sizeof(VoxelMeshVertex) * meshData.vertexCount;
            totalIndexDataSize += sizeof(uint32_t) * meshData.indexCount;
        }
        m_totalInstances += group.totalInstances;
    }
    
    // 如果没有新的渲染器，且缓冲区大小足够，直接更新模型偏移信息
    if (!hasNewRenderers && m_totalVertices <= m_maxTotalVertices && m_totalIndices <= m_maxTotalIndices) {
        // 更新所有模型的偏移信息
        // 注意：必须按照 m_rendererGroups 的顺序来分配偏移量，即使 renderer 已存在于缓存中
        size_t currentVertexOffset = 0;
        size_t currentIndexOffset = 0;
        size_t currentInstanceOffset = 0;  // 新增：实例偏移量
        
        for (auto& group : m_rendererGroups) {
            const VoxRenderer* renderer = group.renderer;
            
            // 从缓存中获取网格数据的偏移信息
            auto it = m_meshCache.find(renderer);
            if (it != m_meshCache.end()) {
                const MeshCacheEntry& entry = it->second;
                // 使用当前累计的偏移量，而不是缓存中的绝对偏移量
                for (auto& modelData : group.models) {
                    modelData.firstVertex = currentVertexOffset;
                    modelData.vertexCount = entry.vertexCount;
                    modelData.firstIndex = currentIndexOffset;
                    modelData.indexCount = entry.indexCount;
                    modelData.firstInstance = currentInstanceOffset;  // 新增：更新实例偏移量
                    modelData.instanceCount = 1;
                    
                    currentInstanceOffset++;  // 每个模型累加
                }
                
                // 累加偏移量（每个 group 只累加一次，因为同组共享网格数据）
                currentVertexOffset += entry.vertexCount;
                currentIndexOffset += entry.indexCount;
            }
        }
        m_geometryDataDirty = false;
        return;
    }
    
    if (m_totalVertices == 0 || m_totalIndices == 0) {
        m_geometryDataDirty = false;
        return;
    }
    
    // 检查是否需要扩容缓冲区
    bool needResize = false;
    size_t newMaxVertices = m_maxTotalVertices;
    size_t newMaxIndices = m_maxTotalIndices;
    
    if (m_totalVertices > m_maxTotalVertices) {
        newMaxVertices = m_totalVertices * 2; // 双倍扩容
        needResize = true;
    }
    
    if (m_totalIndices > m_maxTotalIndices) {
        newMaxIndices = m_totalIndices * 2; // 双倍扩容
        needResize = true;
    }
    
    if (needResize) {
        std::cout << "[VoxelMeshMultiDrawIndirect] Resizing buffers: Vertices " << m_maxTotalVertices 
                  << " -> " << newMaxVertices << ", Indices " << m_maxTotalIndices << " -> " << newMaxIndices << std::endl;
        
        // 确保GPU已经完成了对旧缓冲区的使用
        vkQueueWaitIdle(g_Queue);
        
        // 销毁旧的缓冲区
        if (m_vertexBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
            vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
        }
        if (m_indexBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
            vkFreeMemory(g_Device, m_indexBufferMemory, allocator);
        }
        
        // 重新创建缓冲区
        VkDeviceSize vertexBufferSize = sizeof(VoxelMeshVertex) * newMaxVertices;
        VkBufferCreateInfo vertexBufferInfo{};
        vertexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        vertexBufferInfo.size = vertexBufferSize;
        vertexBufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vertexBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        if (vkCreateBuffer(g_Device, &vertexBufferInfo, allocator, &m_vertexBuffer) != VK_SUCCESS) {
            std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to resize vertex buffer!" << std::endl;
            m_geometryDataDirty = false;
            return;
        }
        
        VkMemoryRequirements vertexMemRequirements;
        vkGetBufferMemoryRequirements(g_Device, m_vertexBuffer, &vertexMemRequirements);
        
        VkMemoryAllocateInfo vertexAllocInfo{};
        vertexAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        vertexAllocInfo.allocationSize = vertexMemRequirements.size;
        vertexAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(vertexMemRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        
        if (vkAllocateMemory(g_Device, &vertexAllocInfo, allocator, &m_vertexBufferMemory) != VK_SUCCESS) {
            std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to allocate resized vertex buffer memory!" << std::endl;
            vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
            m_vertexBuffer = VK_NULL_HANDLE;
            m_geometryDataDirty = false;
            return;
        }
        
        vkBindBufferMemory(g_Device, m_vertexBuffer, m_vertexBufferMemory, 0);
        
        // 创建索引缓冲区
        VkDeviceSize indexBufferSize = sizeof(uint32_t) * newMaxIndices;
        VkBufferCreateInfo indexBufferInfo{};
        indexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        indexBufferInfo.size = indexBufferSize;
        indexBufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        indexBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        if (vkCreateBuffer(g_Device, &indexBufferInfo, allocator, &m_indexBuffer) != VK_SUCCESS) {
            std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to resize index buffer!" << std::endl;
            vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
            vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
            m_vertexBuffer = VK_NULL_HANDLE;
            m_vertexBufferMemory = VK_NULL_HANDLE;
            m_geometryDataDirty = false;
            return;
        }
        
        VkMemoryRequirements indexMemRequirements;
        vkGetBufferMemoryRequirements(g_Device, m_indexBuffer, &indexMemRequirements);
        
        VkMemoryAllocateInfo indexAllocInfo{};
        indexAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        indexAllocInfo.allocationSize = indexMemRequirements.size;
        indexAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(indexMemRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        
        if (vkAllocateMemory(g_Device, &indexAllocInfo, allocator, &m_indexBufferMemory) != VK_SUCCESS) {
            std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to allocate resized index buffer memory!" << std::endl;
            vkDestroyBuffer(g_Device, m_vertexBuffer, allocator);
            vkFreeMemory(g_Device, m_vertexBufferMemory, allocator);
            vkDestroyBuffer(g_Device, m_indexBuffer, allocator);
            m_vertexBuffer = VK_NULL_HANDLE;
            m_vertexBufferMemory = VK_NULL_HANDLE;
            m_indexBuffer = VK_NULL_HANDLE;
            m_geometryDataDirty = false;
            return;
        }
        
        vkBindBufferMemory(g_Device, m_indexBuffer, m_indexBufferMemory, 0);
        
        // 更新最大大小
        m_maxTotalVertices = newMaxVertices;
        m_maxTotalIndices = newMaxIndices;
    }
    
    // 清空网格缓存
    m_meshCache.clear();
    
    // 合并所有模型的顶点和索引数据
    size_t currentVertexOffset = 0;
    size_t currentIndexOffset = 0;
    
    // 使用预分配的临时缓冲区
    m_tempVertices.resize(m_totalVertices);
    m_tempIndices.resize(m_totalIndices);
    std::vector<VoxelMeshVertex>& vertices = m_tempVertices;
    std::vector<uint32_t>& indices = m_tempIndices;
    
    // 一次性创建大的暂存缓冲区来读取所有唯一网格数据
    if (totalVertexDataSize > 0 || totalIndexDataSize > 0) {
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingBufferMemory = VK_NULL_HANDLE;
        
        VkDeviceSize stagingBufferSize = totalVertexDataSize + totalIndexDataSize;
        VkBufferCreateInfo stagingBufferInfo{};
        stagingBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingBufferInfo.size = stagingBufferSize;
        stagingBufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        stagingBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        if (vkCreateBuffer(g_Device, &stagingBufferInfo, allocator, &stagingBuffer) == VK_SUCCESS) {
            VkMemoryRequirements stagingMemRequirements;
            vkGetBufferMemoryRequirements(g_Device, stagingBuffer, &stagingMemRequirements);
            
            VkMemoryAllocateInfo stagingAllocInfo{};
            stagingAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            stagingAllocInfo.allocationSize = stagingMemRequirements.size;
            stagingAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(stagingMemRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            
            if (vkAllocateMemory(g_Device, &stagingAllocInfo, allocator, &stagingBufferMemory) == VK_SUCCESS) {
                vkBindBufferMemory(g_Device, stagingBuffer, stagingBufferMemory, 0);
                
                // 使用命令缓冲区池中的命令缓冲区
                VkCommandBuffer copyCommandBuffer = m_copyCommandBufferPool[m_currentCommandBufferIndex];
                m_currentCommandBufferIndex = (m_currentCommandBufferIndex + 1) % 4;
                
                // 重置命令缓冲区
                vkResetCommandBuffer(copyCommandBuffer, 0);
                
                VkCommandBufferBeginInfo beginInfo{};
                beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                
                if (vkBeginCommandBuffer(copyCommandBuffer, &beginInfo) == VK_SUCCESS) {
                    // 批量复制所有唯一网格的数据到暂存缓冲区
                    size_t stagingOffset = 0;
                    
                    for (const VoxRenderer* renderer : uniqueRenderers) {
                        const VoxelMeshData& meshData = renderer->GetMeshData();
                        
                        // 复制顶点数据
                        if (meshData.vertexCount > 0) {
                            VkBufferCopy copyRegion{};
                            copyRegion.srcOffset = 0;
                            copyRegion.dstOffset = stagingOffset;
                            copyRegion.size = sizeof(VoxelMeshVertex) * meshData.vertexCount;
                            vkCmdCopyBuffer(copyCommandBuffer, meshData.vertexBuffer, stagingBuffer, 1, &copyRegion);
                            stagingOffset += copyRegion.size;
                        }
                        
                        // 复制索引数据
                        if (meshData.indexCount > 0) {
                            VkBufferCopy copyRegion{};
                            copyRegion.srcOffset = 0;
                            copyRegion.dstOffset = stagingOffset;
                            copyRegion.size = sizeof(uint32_t) * meshData.indexCount;
                            vkCmdCopyBuffer(copyCommandBuffer, meshData.indexBuffer, stagingBuffer, 1, &copyRegion);
                            stagingOffset += copyRegion.size;
                        }
                    }
                    
                    if (vkEndCommandBuffer(copyCommandBuffer) == VK_SUCCESS) {
                        VkSubmitInfo submitInfo{};
                        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                        submitInfo.commandBufferCount = 1;
                        submitInfo.pCommandBuffers = &copyCommandBuffer;
                        
                        // 使用当前 fence
                        VkFence currentFence = m_copyFences[m_currentFenceIndex];
                        m_currentFenceIndex = (m_currentFenceIndex + 1) % 4;
                        
                        // 重置 fence
                        vkResetFences(g_Device, 1, &currentFence);
                        
                        if (vkQueueSubmit(g_Queue, 1, &submitInfo, currentFence) == VK_SUCCESS) {
                            // 等待 fence 信号，而不是等待整个队列
                            vkWaitForFences(g_Device, 1, &currentFence, VK_TRUE, UINT64_MAX);
                            
                            // 映射暂存缓冲区并读取所有数据
                            void* stagingData = nullptr;
                            if (vkMapMemory(g_Device, stagingBufferMemory, 0, stagingMemRequirements.size, 0, &stagingData) == VK_SUCCESS) {
                                size_t dataOffset = 0;
                                
                                // 处理唯一网格数据并更新缓存
                                for (const VoxRenderer* renderer : uniqueRenderers) {
                                    const VoxelMeshData& meshData = renderer->GetMeshData();
                                    
                                    // 复制顶点数据
                                    if (meshData.vertexCount > 0) {
                                        size_t vertexSize = sizeof(VoxelMeshVertex) * meshData.vertexCount;
                                        memcpy(&vertices[currentVertexOffset], (char*)stagingData + dataOffset, vertexSize);
                                        dataOffset += vertexSize;
                                    }
                                    
                                    // 复制索引数据
                                    if (meshData.indexCount > 0) {
                                        size_t indexSize = sizeof(uint32_t) * meshData.indexCount;
                                        memcpy(&indices[currentIndexOffset], (char*)stagingData + dataOffset, indexSize);
                                        dataOffset += indexSize;
                                    }
                                    
                                    // 更新网格缓存
                                    MeshCacheEntry entry;
                                    entry.firstVertex = currentVertexOffset;
                                    entry.vertexCount = meshData.vertexCount;
                                    entry.firstIndex = currentIndexOffset;
                                    entry.indexCount = meshData.indexCount;
                                    m_meshCache[renderer] = entry;
                                    
                                    // 更新偏移量
                                    currentVertexOffset += meshData.vertexCount;
                                    currentIndexOffset += meshData.indexCount;
                                }
                                
                                vkUnmapMemory(g_Device, stagingBufferMemory);
                            }
                            
                            vkFreeMemory(g_Device, stagingBufferMemory, allocator);
                        }
                    }
                }
            }
            
            vkDestroyBuffer(g_Device, stagingBuffer, allocator);
        }
    }
    
    // 更新所有模型的偏移信息
    // 注意：必须同时更新 firstInstance，确保实例索引正确
    size_t currentInstanceOffset = 0;
    for (auto& group : m_rendererGroups) {
        const VoxRenderer* renderer = group.renderer;
        
        // 从缓存中获取网格数据的偏移信息
        auto it = m_meshCache.find(renderer);
        if (it != m_meshCache.end()) {
            const MeshCacheEntry& entry = it->second;
            for (auto& modelData : group.models) {
                modelData.firstVertex = entry.firstVertex;
                modelData.vertexCount = entry.vertexCount;
                modelData.firstIndex = entry.firstIndex;
                modelData.indexCount = entry.indexCount;
                modelData.firstInstance = currentInstanceOffset;  // 新增：更新实例偏移量
                modelData.instanceCount = 1;
                
                currentInstanceOffset++;  // 每个模型累加
            }
        }
    }
    
    // 使用命令缓冲区池中的命令缓冲区
    VkCommandBuffer copyCommandBuffer = m_copyCommandBufferPool[m_currentCommandBufferIndex];
    m_currentCommandBufferIndex = (m_currentCommandBufferIndex + 1) % 4;
    
    // 重置命令缓冲区
    vkResetCommandBuffer(copyCommandBuffer, 0);
    
    // 开始命令缓冲区
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    if (vkBeginCommandBuffer(copyCommandBuffer, &beginInfo) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to begin command buffer!" << std::endl;
        return;
    }
    
    // 复制顶点和索引数据到全局缓冲区
    if (m_totalVertices > 0) {
        // 直接从临时缓冲区复制到全局缓冲区
        VkDeviceSize vertexBufferSize = sizeof(VoxelMeshVertex) * m_totalVertices;
        VkDeviceSize indexBufferSize = sizeof(uint32_t) * m_totalIndices;
        
        // 创建一个临时的主机可见缓冲区来传输数据
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingBufferMemory = VK_NULL_HANDLE;
        
        VkBufferCreateInfo stagingBufferInfo{};
        stagingBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingBufferInfo.size = vertexBufferSize + indexBufferSize;
        stagingBufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        stagingBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        if (vkCreateBuffer(g_Device, &stagingBufferInfo, allocator, &stagingBuffer) == VK_SUCCESS) {
            VkMemoryRequirements stagingMemRequirements;
            vkGetBufferMemoryRequirements(g_Device, stagingBuffer, &stagingMemRequirements);
            
            VkMemoryAllocateInfo stagingAllocInfo{};
            stagingAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            stagingAllocInfo.allocationSize = stagingMemRequirements.size;
            stagingAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(stagingMemRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            
            if (vkAllocateMemory(g_Device, &stagingAllocInfo, allocator, &stagingBufferMemory) == VK_SUCCESS) {
                vkBindBufferMemory(g_Device, stagingBuffer, stagingBufferMemory, 0);
                
                // 映射并复制数据
                void* stagingData = nullptr;
                if (vkMapMemory(g_Device, stagingBufferMemory, 0, stagingBufferInfo.size, 0, &stagingData) == VK_SUCCESS) {
                    // 复制顶点数据
                    memcpy(stagingData, vertices.data(), vertexBufferSize);
                    
                    // 复制索引数据
                    if (m_totalIndices > 0) {
                        memcpy((char*)stagingData + vertexBufferSize, indices.data(), indexBufferSize);
                    }
                    
                    vkUnmapMemory(g_Device, stagingBufferMemory);
                }
                
                // 复制顶点数据到全局顶点缓冲区
                VkBufferCopy vertexCopyRegion{};
                vertexCopyRegion.srcOffset = 0;
                vertexCopyRegion.dstOffset = 0;
                vertexCopyRegion.size = vertexBufferSize;
                vkCmdCopyBuffer(copyCommandBuffer, stagingBuffer, m_vertexBuffer, 1, &vertexCopyRegion);
                
                // 复制索引数据到全局索引缓冲区
                if (m_totalIndices > 0) {
                    VkBufferCopy indexCopyRegion{};
                    indexCopyRegion.srcOffset = vertexBufferSize;
                    indexCopyRegion.dstOffset = 0;
                    indexCopyRegion.size = indexBufferSize;
                    vkCmdCopyBuffer(copyCommandBuffer, stagingBuffer, m_indexBuffer, 1, &indexCopyRegion);
                }
                
                // 结束命令缓冲区
                if (vkEndCommandBuffer(copyCommandBuffer) != VK_SUCCESS) {
                    std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to end command buffer!" << std::endl;
                    vkFreeMemory(g_Device, stagingBufferMemory, allocator);
                    vkDestroyBuffer(g_Device, stagingBuffer, allocator);
                    return;
                }
                
                // 提交命令缓冲区
                VkSubmitInfo submitInfo{};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &copyCommandBuffer;
                
                if (vkQueueSubmit(g_Queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
                    std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to submit command buffer!" << std::endl;
                    vkFreeMemory(g_Device, stagingBufferMemory, allocator);
                    vkDestroyBuffer(g_Device, stagingBuffer, allocator);
                    return;
                }
                
                // 等待命令执行完成
                vkQueueWaitIdle(g_Queue);
                
                vkFreeMemory(g_Device, stagingBufferMemory, allocator);
            }
            
            vkDestroyBuffer(g_Device, stagingBuffer, allocator);
        }
    }
    
    // 标记几何数据已更新
    m_geometryDataDirty = false;
    
    // 初始化双缓冲区数据（确保两个缓冲区初始数据一致）
    // 注意：只在首次初始化或有新网格时执行
    if (m_supportsMDI && m_totalInstances > 0) {
        // 生成初始实例数据
        std::vector<InstanceData> initialInstances(m_totalInstances);
        size_t instanceIndex = 0;
        for (const auto& group : m_rendererGroups) {
            const VoxRenderer* renderer = group.renderer;
            for (const auto& modelData : group.models) {
                InstanceData instance;
                instance.model = modelData.transform;
                instance.prevModel = modelData.transform;
                instance.albedoColor = modelData.color;
                instance.materialData = glm::vec4(0.5f, 0.5f, 1.0f, 0.0f);
                instance.worldMinBounds = renderer->GetMinBounds();
                instance.voxelSize = renderer->GetVoxelSize();
                initialInstances[instanceIndex++] = instance;
            }
        }
        
        // 复制到两个缓冲区
        memcpy(m_mappedInstancePtrs[0], initialInstances.data(), initialInstances.size() * sizeof(InstanceData));
        memcpy(m_mappedInstancePtrs[1], initialInstances.data(), initialInstances.size() * sizeof(InstanceData));
        
        std::cout << "[MDI] Initialized dual instance buffers with " << m_totalInstances << " instances" << std::endl;
    }
}

void VoxelMeshMultiDrawIndirect::UpdateDrawCommandsAndInstanceData(const glm::vec3& cameraPosition, bool enableBackfaceCulling)
{
    if (m_rendererGroups.empty() || m_mappedInstancePtrs[0] == nullptr || m_mappedDrawCommandPtrs[0] == nullptr) {
        return;
    }

    // 步骤 1：收集所有可见模型（visible=true 的模型）
    m_visibleModels.clear();
    m_visibleModels.reserve(m_totalInstances);
    
    for (auto& group : m_rendererGroups) {
        for (auto& modelData : group.models) {
            if (modelData.visible) {
                m_visibleModels.push_back(&modelData);
            }
        }
    }
    
    // 如果没有可见模型，直接返回
    if (m_visibleModels.empty()) {
        m_currentFaceCommandCount = 0;
        return;
    }

    struct FaceGroupKey {
        const VoxRenderer* renderer;
        size_t firstIndex;
        size_t firstVertex;
        size_t indexCount;
        
        bool operator==(const FaceGroupKey& other) const {
            return renderer == other.renderer &&
                   firstIndex == other.firstIndex && 
                   firstVertex == other.firstVertex && 
                   indexCount == other.indexCount;
        }
    };
    
    struct FaceGroupKeyHash {
        size_t operator()(const FaceGroupKey& k) const {
            return std::hash<const void*>()(k.renderer) ^ 
                   (std::hash<size_t>()(k.firstIndex) << 1) ^ 
                   (std::hash<size_t>()(k.firstVertex) << 2) ^ 
                   (std::hash<size_t>()(k.indexCount) << 3);
        }
    };
    
    struct FaceInstanceGroup {
        size_t firstIndex;
        size_t indexCount;
        size_t firstVertex;
        const VoxRenderer* renderer;
        std::vector<size_t> instanceIndices;
    };
    
    struct ThreadLocalData {
        std::vector<FaceDrawCommand> faceCommands;
        std::vector<InstanceData> instanceData;
        std::array<std::unordered_map<FaceGroupKey, FaceInstanceGroup, FaceGroupKeyHash>, 6> faceGroupMaps;
        size_t instanceStartIndex;
        size_t instanceCount;
        
        ThreadLocalData() : instanceStartIndex(0), instanceCount(0) {
            faceCommands.reserve(100);
            for (int i = 0; i < 6; i++) {
                faceGroupMaps[i].reserve(100);
            }
            instanceData.reserve(100);
        }
        
        ThreadLocalData(size_t startIndex) : instanceStartIndex(startIndex), instanceCount(0) {
            faceCommands.reserve(100);
            for (int i = 0; i < 6; i++) {
                faceGroupMaps[i].reserve(100);
            }
            instanceData.reserve(100);
        }
    };

    // 计算实例索引偏移
    size_t totalInstances = 0;
    for (const auto& group : m_rendererGroups) {
        totalInstances += group.models.size();
    }

    // 预计算每个渲染组的实例索引偏移
    std::vector<size_t> instanceOffsets;
    instanceOffsets.reserve(m_rendererGroups.size());
    
    size_t currentOffset = 0;
    for (const auto& group : m_rendererGroups) {
        instanceOffsets.push_back(currentOffset);
        currentOffset += group.models.size();
    }
    
    // 为每个可见模型预计算全局实例索引
    std::vector<size_t> visibleModelGlobalIndices;
    visibleModelGlobalIndices.reserve(m_visibleModels.size());
    
    for (const auto* modelDataPtr : m_visibleModels) {
        // 查找模型在全局 m_rendererGroups 中的索引
        size_t globalIndex = 0;
        bool found = false;
        for (size_t g = 0; g < m_rendererGroups.size() && !found; g++) {
            const auto& group = m_rendererGroups[g];
            for (size_t j = 0; j < group.models.size(); j++) {
                if (&group.models[j] == modelDataPtr) {
                    globalIndex = instanceOffsets[g] + j;
                    found = true;
                    break;
                }
            }
        }
        visibleModelGlobalIndices.push_back(found ? globalIndex : 0);
    }
    
    // 多线程处理（仅 Windows 平台支持 std::execution）
    std::vector<ThreadLocalData> threadData;
    threadData.resize(m_visibleModels.size());
    
#if defined(_WIN32) || defined(_WIN64)
    // Windows 平台：使用并行算法处理可见模型
    std::for_each(std::execution::par, m_visibleModels.begin(), m_visibleModels.end(),
        [&](VoxelModelData* modelDataPtr) {
            VoxelModelData& modelData = *modelDataPtr;
            
            // 查找模型在可见列表中的索引（用于访问 threadData）
            size_t threadIndex = 0;
            for (size_t i = 0; i < m_visibleModels.size(); i++) {
                if (m_visibleModels[i] == &modelData) {
                    threadIndex = i;
                    break;
                }
            }
            
            // 使用预计算的全局实例索引
            size_t globalInstanceIndex = visibleModelGlobalIndices[threadIndex];
            
            // 查找模型所属的组
            auto* groupPtr = static_cast<RendererGroup*>(nullptr);
            for (size_t g = 0; g < m_rendererGroups.size(); g++) {
                auto& group = m_rendererGroups[g];
                for (size_t j = 0; j < group.models.size(); j++) {
                    if (&group.models[j] == &modelData) {
                        groupPtr = &group;
                        break;
                    }
                }
                if (groupPtr) break;
            }
            
            if (!groupPtr) return;
            auto& group = *groupPtr;
            
            const VoxRenderer* renderer = group.renderer;
            const VoxelMeshData& meshData = renderer->GetMeshData();
            
            ThreadLocalData localData(globalInstanceIndex);
            
            glm::mat3 normalMatrix = glm::mat3(modelData.transform);
            normalMatrix[0] = glm::normalize(normalMatrix[0]);
            normalMatrix[1] = glm::normalize(normalMatrix[1]);
            normalMatrix[2] = glm::normalize(normalMatrix[2]);
            
            // PC 端：使用脏标记检查，只重新计算变化的实例数据
            bool needsUpdate = modelData.transformDirty || modelData.colorDirty || modelData.staticDirty;
            
            // 填充实例数据
            InstanceData instance;
            if (needsUpdate) {
                // 重新计算实例数据
                instance.model = modelData.transform;
                instance.prevModel = modelData.transform;
                instance.albedoColor = modelData.color;
                instance.materialData = glm::vec4(0.5f, 0.5f, 1.0f, 0.0f);
                instance.worldMinBounds = renderer->GetMinBounds();
                instance.voxelSize = renderer->GetVoxelSize();
                
                // 更新缓存数据
                modelData.cachedInstanceData = instance;
            } else {
                // 使用缓存的数据
                instance = modelData.cachedInstanceData;
            }
            localData.instanceData.push_back(instance);
            
            // 执行背面剔除和绘制命令生成（每帧都执行，不管数据是否变化）
            for (int faceDir = 0; faceDir < 6; faceDir++) {
                const auto& faceGroup = meshData.faceGroups[faceDir];
                
                if (faceGroup.indexCount == 0) continue;
                
                bool isVisible = true;
                if (enableBackfaceCulling) {
                    bool cameraInsideModel = false;
                    
                    glm::vec3 localMin = renderer->GetMinBounds();
                    glm::vec3 localMax = renderer->GetMaxBounds();
                    
                    glm::vec3 modelPos = glm::vec3(modelData.transform[3]);
                    
                    float scaleX = glm::length(glm::vec3(modelData.transform[0]));
                    float scaleY = glm::length(glm::vec3(modelData.transform[1]));
                    float scaleZ = glm::length(glm::vec3(modelData.transform[2]));
                    
                    glm::vec3 scaledLocalMin = localMin * glm::vec3(scaleX, scaleY, scaleZ);
                    glm::vec3 scaledLocalMax = localMax * glm::vec3(scaleX, scaleY, scaleZ);
                    
                    glm::vec3 worldMin = modelPos + scaledLocalMin;
                    glm::vec3 worldMax = modelPos + scaledLocalMax;
                    
                    cameraInsideModel = (cameraPosition.x >= worldMin.x && cameraPosition.x <= worldMax.x &&
                                       cameraPosition.y >= worldMin.y && cameraPosition.y <= worldMax.y &&
                                       cameraPosition.z >= worldMin.z && cameraPosition.z <= worldMax.z);
                    
                    glm::vec3 modelCenter = (worldMin + worldMax) * 0.5f;
                    float distanceToCamera = glm::length(cameraPosition - modelCenter);
                    
                    glm::vec3 extent = worldMax - worldMin;
                    float boundingRadius = glm::length(extent) * 0.5f;
                    
                    bool shouldCull = !cameraInsideModel && (distanceToCamera >= 1.0f * boundingRadius);
                    
                    if (shouldCull) {
                        glm::vec3 faceNormal = FACE_NORMALS[faceDir];
                        faceNormal = normalMatrix * faceNormal;
                        faceNormal = glm::normalize(faceNormal);
                        
                        glm::vec3 faceLocalPos = faceGroup.faceCenter;
                        glm::vec3 faceWorldPos = glm::vec3(modelData.transform * glm::vec4(faceLocalPos, 1.0f));
                        
                        glm::vec3 toCamera = cameraPosition - faceWorldPos;
                        float toCameraLength = glm::length(toCamera);
                        if (toCameraLength > 0.0f) {
                            toCamera = toCamera / toCameraLength;
                        }
                        
                        float dotProduct = glm::dot(faceNormal, toCamera);
                        isVisible = (dotProduct > -0.3f);
                    }
                }
                
                if (isVisible) {
                    FaceGroupKey key{
                        renderer,
                        modelData.firstIndex + faceGroup.firstIndex,
                        modelData.firstVertex,
                        faceGroup.indexCount
                    };
                    
                    auto& groupMap = localData.faceGroupMaps[faceDir];
                    auto it = groupMap.find(key);
                    
                    if (it != groupMap.end()) {
                        it->second.instanceIndices.push_back(localData.instanceStartIndex);
                    } else {
                        FaceInstanceGroup newGroup;
                        newGroup.firstIndex = key.firstIndex;
                        newGroup.indexCount = key.indexCount;
                        newGroup.firstVertex = key.firstVertex;
                        newGroup.renderer = renderer;
                        newGroup.instanceIndices.push_back(localData.instanceStartIndex);
                        groupMap[key] = std::move(newGroup);
                    }
                }
            }
            
            localData.instanceCount++;
            threadData[threadIndex] = std::move(localData);
        });
#else
    // Android/ 其他平台：使用串行算法处理可见模型
    for (size_t i = 0; i < m_visibleModels.size(); i++) {
        VoxelModelData& modelData = *m_visibleModels[i];
        
        // 查找模型所属的组
        auto* groupPtr = static_cast<RendererGroup*>(nullptr);
        for (size_t g = 0; g < m_rendererGroups.size(); g++) {
            auto& group = m_rendererGroups[g];
            for (size_t j = 0; j < group.models.size(); j++) {
                if (&group.models[j] == &modelData) {
                    groupPtr = &group;
                    break;
                }
            }
            if (groupPtr) break;
        }
        
        if (!groupPtr) continue;
        auto& group = *groupPtr;
        
        const VoxRenderer* renderer = group.renderer;
        const VoxelMeshData& meshData = renderer->GetMeshData();
        
        // 使用预计算的全局实例索引
        ThreadLocalData localData(visibleModelGlobalIndices[i]);
        
        glm::mat3 normalMatrix = glm::mat3(modelData.transform);
        normalMatrix[0] = glm::normalize(normalMatrix[0]);
        normalMatrix[1] = glm::normalize(normalMatrix[1]);
        normalMatrix[2] = glm::normalize(normalMatrix[2]);
        
        // 填充实例数据
        InstanceData instance;
        instance.model = modelData.transform;
        instance.prevModel = modelData.transform;
        instance.albedoColor = modelData.color;
        instance.materialData = glm::vec4(0.5f, 0.5f, 1.0f, 0.0f);
        instance.worldMinBounds = renderer->GetMinBounds();
        instance.voxelSize = renderer->GetVoxelSize();
        localData.instanceData.push_back(instance);
        
        // 执行背面剔除和绘制命令生成
        for (int faceDir = 0; faceDir < 6; faceDir++) {
            const auto& faceGroup = meshData.faceGroups[faceDir];
            
            if (faceGroup.indexCount == 0) continue;
            
            bool isVisible = true;
            if (enableBackfaceCulling) {
                bool cameraInsideModel = false;
                
                glm::vec3 localMin = renderer->GetMinBounds();
                glm::vec3 localMax = renderer->GetMaxBounds();
                
                glm::vec3 modelPos = glm::vec3(modelData.transform[3]);
                
                float scaleX = glm::length(glm::vec3(modelData.transform[0]));
                float scaleY = glm::length(glm::vec3(modelData.transform[1]));
                float scaleZ = glm::length(glm::vec3(modelData.transform[2]));
                
                glm::vec3 scaledLocalMin = localMin * glm::vec3(scaleX, scaleY, scaleZ);
                glm::vec3 scaledLocalMax = localMax * glm::vec3(scaleX, scaleY, scaleZ);
                
                glm::vec3 worldMin = modelPos + scaledLocalMin;
                glm::vec3 worldMax = modelPos + scaledLocalMax;
                
                cameraInsideModel = (cameraPosition.x >= worldMin.x && cameraPosition.x <= worldMax.x &&
                                   cameraPosition.y >= worldMin.y && cameraPosition.y <= worldMax.y &&
                                   cameraPosition.z >= worldMin.z && cameraPosition.z <= worldMax.z);
                
                glm::vec3 modelCenter = (worldMin + worldMax) * 0.5f;
                float distanceToCamera = glm::length(cameraPosition - modelCenter);
                
                glm::vec3 extent = worldMax - worldMin;
                float boundingRadius = glm::length(extent) * 0.5f;
                
                bool shouldCull = !cameraInsideModel && (distanceToCamera >= 1.0f * boundingRadius);
                
                if (shouldCull) {
                    glm::vec3 faceNormal = FACE_NORMALS[faceDir];
                    faceNormal = normalMatrix * faceNormal;
                    faceNormal = glm::normalize(faceNormal);
                    
                    glm::vec3 faceLocalPos = faceGroup.faceCenter;
                    glm::vec3 faceWorldPos = glm::vec3(modelData.transform * glm::vec4(faceLocalPos, 1.0f));
                    
                    glm::vec3 toCamera = cameraPosition - faceWorldPos;
                    float toCameraLength = glm::length(toCamera);
                    if (toCameraLength > 0.0f) {
                        toCamera = toCamera / toCameraLength;
                    }
                    
                    float dotProduct = glm::dot(faceNormal, toCamera);
                    isVisible = (dotProduct > -0.3f);
                }
            }
            
            if (isVisible) {
                FaceGroupKey key{
                    renderer,
                    modelData.firstIndex + faceGroup.firstIndex,
                    modelData.firstVertex,
                    faceGroup.indexCount
                };
                
                auto& groupMap = localData.faceGroupMaps[faceDir];
                auto it = groupMap.find(key);
                
                if (it != groupMap.end()) {
                    it->second.instanceIndices.push_back(localData.instanceStartIndex);
                } else {
                    FaceInstanceGroup newGroup;
                    newGroup.firstIndex = key.firstIndex;
                    newGroup.indexCount = key.indexCount;
                    newGroup.firstVertex = key.firstVertex;
                    newGroup.instanceIndices.push_back(localData.instanceStartIndex);
                    groupMap[key] = std::move(newGroup);
                }
            }
        }
        
        localData.instanceCount++;
        threadData[i] = std::move(localData);
    }
#endif
    
    // 生成绘制命令（直接使用已存储的 renderer 信息）
    for (auto& data : threadData) {
        for (int faceDir = 0; faceDir < 6; faceDir++) {
            for (const auto& [key, faceGroupItem] : data.faceGroupMaps[faceDir]) {
                if (!faceGroupItem.instanceIndices.empty()) {
                    FaceDrawCommand cmd;
                    cmd.indexCount = static_cast<uint32_t>(faceGroupItem.indexCount);
                    cmd.instanceCount = static_cast<uint32_t>(faceGroupItem.instanceIndices.size());
                    cmd.firstIndex = static_cast<uint32_t>(faceGroupItem.firstIndex);
                    cmd.vertexOffset = static_cast<int32_t>(faceGroupItem.firstVertex);
                    cmd.firstInstance = static_cast<uint32_t>(faceGroupItem.instanceIndices[0]);
                    cmd.faceDirection = faceDir;
                    cmd.enabled = 1;
                    data.faceCommands.push_back(cmd);
                }
            }
        }
    }
    
    // 合并所有线程的结果（全局合并相同网格相同方向的面）
    std::vector<FaceDrawCommand> tempFaceCommands;
    std::vector<InstanceData> tempInstanceData;
    
    tempFaceCommands.reserve(m_maxVoxelModels * 6);
    tempInstanceData.reserve(m_maxVoxelModels);
    
    // 收集需要更新的实例索引（仅 PC 端使用）
    std::vector<size_t> updatedInstanceIndices;
    updatedInstanceIndices.reserve(m_maxVoxelModels);
    
    // 全局合并：将不同线程的相同 renderer、相同网格、相同方向的面组合并
    // 由于线程内部已经按 renderer 分组，这里只需要合并相同 (renderer, firstIndex, firstVertex, indexCount, faceDirection) 的面组
    struct GlobalFaceGroupKey {
        const VoxRenderer* renderer;
        size_t firstIndex;
        size_t firstVertex;
        size_t indexCount;
        int faceDirection;
        
        bool operator==(const GlobalFaceGroupKey& other) const {
            return renderer == other.renderer &&
                   firstIndex == other.firstIndex && 
                   firstVertex == other.firstVertex && 
                   indexCount == other.indexCount &&
                   faceDirection == other.faceDirection;
        }
    };
    
    struct GlobalFaceGroupKeyHash {
        size_t operator()(const GlobalFaceGroupKey& k) const {
            return std::hash<const void*>()(k.renderer) ^ 
                   (std::hash<size_t>()(k.firstIndex) << 1) ^ 
                   (std::hash<size_t>()(k.firstVertex) << 2) ^ 
                   (std::hash<size_t>()(k.indexCount) << 3) ^ 
                   (std::hash<int>()(k.faceDirection) << 4);
        }
    };
    
    struct GlobalFaceInstanceGroup {
        size_t firstIndex;
        size_t indexCount;
        size_t firstVertex;
        int faceDirection;
        const VoxRenderer* renderer;
        std::vector<size_t> instanceIndices;
    };
    
    std::unordered_map<GlobalFaceGroupKey, GlobalFaceInstanceGroup, GlobalFaceGroupKeyHash> globalFaceGroups;
    
    // 合并所有线程的面组数据
    for (auto& data : threadData) {
        for (int faceDir = 0; faceDir < 6; faceDir++) {
            for (const auto& [key, faceGroupItem] : data.faceGroupMaps[faceDir]) {
                if (faceGroupItem.instanceIndices.empty()) continue;
                
                GlobalFaceGroupKey groupKey{
                    faceGroupItem.renderer,
                    faceGroupItem.firstIndex,
                    faceGroupItem.firstVertex,
                    faceGroupItem.indexCount,
                    faceDir
                };
                
                auto it = globalFaceGroups.find(groupKey);
                if (it != globalFaceGroups.end()) {
                    for (size_t idx : faceGroupItem.instanceIndices) {
                        it->second.instanceIndices.push_back(idx);
                    }
                } else {
                    GlobalFaceInstanceGroup newGroup;
                    newGroup.firstIndex = groupKey.firstIndex;
                    newGroup.indexCount = groupKey.indexCount;
                    newGroup.firstVertex = groupKey.firstVertex;
                    newGroup.faceDirection = groupKey.faceDirection;
                    newGroup.renderer = groupKey.renderer;
                    newGroup.instanceIndices = faceGroupItem.instanceIndices;
                    globalFaceGroups[groupKey] = std::move(newGroup);
                }
            }
        }
        
#if defined(_WIN32) || defined(_WIN64)
        // PC 端：只添加有变化的实例数据（使用脏标记）
        for (size_t i = 0; i < data.instanceData.size(); i++) {
            size_t globalInstanceIndex = data.instanceStartIndex + i;
            size_t modelIndex = globalInstanceIndex;
            
            const VoxelModelData* modelData = nullptr;
            size_t currentIndex = 0;
            for (const auto& group : m_rendererGroups) {
                if (modelIndex >= currentIndex && modelIndex < currentIndex + group.models.size()) {
                    modelData = &group.models[modelIndex - currentIndex];
                    break;
                }
                currentIndex += group.models.size();
            }
            
            if (modelData && (modelData->transformDirty || modelData->colorDirty || modelData->staticDirty)) {
                updatedInstanceIndices.push_back(globalInstanceIndex);
                tempInstanceData.push_back(data.instanceData[i]);
            }
        }
#else
        // 移动端：添加所有实例数据（每帧全量更新）
        for (size_t i = 0; i < data.instanceData.size(); i++) {
            tempInstanceData.push_back(data.instanceData[i]);
        }
#endif
    }
    
    // 第二步：将合并后的面组转换为绘制命令（按 renderer 和面方向排序）
    // 使用 vector 存储后手动排序，保证缓存友好的顺序
    std::vector<std::pair<GlobalFaceGroupKey, GlobalFaceInstanceGroup>> sortedFaceGroups;
    sortedFaceGroups.reserve(globalFaceGroups.size());
    
    for (auto& [key, group] : globalFaceGroups) {
        if (!group.instanceIndices.empty()) {
            sortedFaceGroups.emplace_back(key, group);
        }
    }
    
    // 排序：先按 renderer 指针地址排序，再按面方向排序
    std::sort(sortedFaceGroups.begin(), sortedFaceGroups.end(),
        [](const auto& a, const auto& b) {
            // 首先按 renderer 排序
            if (a.first.renderer != b.first.renderer) {
                return a.first.renderer < b.first.renderer;
            }
            // 相同 renderer 按面方向排序
            return a.first.faceDirection < b.first.faceDirection;
        });
    
    // 生成有序的绘制命令
    for (const auto& [key, group] : sortedFaceGroups) {
        FaceDrawCommand cmd;
        cmd.indexCount = static_cast<uint32_t>(group.indexCount);
        cmd.instanceCount = static_cast<uint32_t>(group.instanceIndices.size());
        cmd.firstIndex = static_cast<uint32_t>(group.firstIndex);
        cmd.vertexOffset = static_cast<int32_t>(group.firstVertex);
        cmd.firstInstance = static_cast<uint32_t>(group.instanceIndices[0]);
        cmd.faceDirection = group.faceDirection;
        cmd.enabled = 1;
        tempFaceCommands.push_back(cmd);
    }
    
    // 调试信息
#if defined(_WIN32) || defined(_WIN64)
    // 统计脏标记情况
    size_t dirtyCount = 0;
    size_t totalCount = 0;
    for (const auto& group : m_rendererGroups) {
        for (const auto& modelData : group.models) {
            totalCount++;
            if (modelData.transformDirty || modelData.colorDirty || modelData.staticDirty) {
                dirtyCount++;
            }
        }
    }
    // 调试日志已移除
#else
    // 移动端日志已移除
#endif
    
    // 使用当前缓冲区索引
    size_t currentBufferIndex = m_currentInstanceBufferIndex;
    
    // 更新所有绘制命令
    if (!tempFaceCommands.empty()) {
        memcpy(m_mappedDrawCommandPtrs[currentBufferIndex], tempFaceCommands.data(), tempFaceCommands.size() * sizeof(FaceDrawCommand));
    }
    
#if defined(_WIN32) || defined(_WIN64)
    // PC 端：双缓冲增量更新（修复抖动问题）
    // 核心思想：每帧复制完整数据到写缓冲区，然后增量更新变化的部分
    // 这样可以确保两个缓冲区都有最新的完整数据
    size_t writeBufferIndex = (m_currentInstanceBufferIndex + 1) % 2;
    
    // 步骤 1：复制当前缓冲区的完整数据到写缓冲区
    // 这确保写缓冲区有最新的完整数据
    if (m_totalInstances > 0) {
        memcpy(m_mappedInstancePtrs[writeBufferIndex], 
               m_mappedInstancePtrs[m_currentInstanceBufferIndex], 
               sizeof(InstanceData) * m_totalInstances);
    }
    
    // 步骤 2：增量更新变化的实例
    if (!updatedInstanceIndices.empty()) {
        for (size_t i = 0; i < updatedInstanceIndices.size(); i++) {
            size_t instanceIndex = updatedInstanceIndices[i];
            if (instanceIndex < m_totalInstances) {
                memcpy(&((InstanceData*)m_mappedInstancePtrs[writeBufferIndex])[instanceIndex], 
                       &tempInstanceData[i], 
                       sizeof(InstanceData));
            }
        }
    }
    // 注意：即使没有变化的实例，步骤 1 已经确保了数据完整性
    
    // 清除脏标记
    for (auto& group : m_rendererGroups) {
        for (auto& modelData : group.models) {
            modelData.transformDirty = false;
            modelData.colorDirty = false;
            modelData.staticDirty = false;
        }
    }
#else
    // 移动端：更新所有实例数据（使用双缓冲）
    if (!tempInstanceData.empty()) {
        memcpy(m_mappedInstancePtrs[currentBufferIndex], tempInstanceData.data(), tempInstanceData.size() * sizeof(InstanceData));
    }
#endif
    
    // 更新总绘制命令数
    m_currentFaceCommandCount = static_cast<uint32_t>(tempFaceCommands.size());
}

void VoxelMeshMultiDrawIndirect::Render(VkCommandBuffer commandBuffer, int width, int height,
                                       const glm::mat4& projView, const glm::mat4& prevProjView,
                                       const glm::mat4& cullProjView,
                                       const glm::vec3& cameraPosition,
                                       bool useDualFrustumCulling,
                                       bool enableBackfaceCulling,
                                       bool depthOnly)
{
    // std::cout << "[VoxelMeshMultiDrawIndirect::Render] Entered Render function" << std::endl;
    if (m_rendererGroups.empty()) {
        return;
    }
    
    if (g_Device == VK_NULL_HANDLE) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] g_Device is null!" << std::endl;
        return;
    }

    // 合并几何数据（只有在数据有变化时才执行）
    MergeGeometryData();
    
    // 更新实例数据和绘制命令（CPU 端背面剔除）
    UpdateDrawCommandsAndInstanceData(cameraPosition, enableBackfaceCulling);
    
    // 重置所有模型的可见性标记为 false（为下一帧做准备）
    // 注意：在下一帧 SceneRenderer 调用 UpdateVoxelModel 时会重新设置 visible=true
    for (auto& group : m_rendererGroups) {
        for (auto& modelData : group.models) {
            modelData.visible = false;
        }
    }
    
    // 添加内存屏障：确保 CPU 更新的绘制命令和实例数据对 GPU 可见
    VkMemoryBarrier preCullingBarrier{};
    preCullingBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    preCullingBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    preCullingBarrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
        0,
        1, &preCullingBarrier,
        0, nullptr,
        0, nullptr
    );

    // 执行 GPU 剔除（视锥剔除 + 背面剔除）
    bool useGPUCulling = true;
    // 检查是否有有效的 Hi-Z 数据用于剔除（第一帧可能没有）
    bool hasValidHiZData = g_SceneRenderer.GetHiZShader().HasValidCullingData();
    bool enableHiZCulling = hasValidHiZData; // 启用 Hi-Z 遮挡剔除
    
    if (useGPUCulling && m_supportsComputeShader) {
        ExecuteGPUCulling(commandBuffer, projView, prevProjView, cullProjView, cameraPosition, 
                          useDualFrustumCulling, enableBackfaceCulling, enableHiZCulling);
    } else {
        std::cout << "[VoxelMeshMultiDrawIndirect::Render] GPU culling not executed" << std::endl;
    }

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    // 使用当前缓冲区索引
    size_t currentBufferIndex = m_currentInstanceBufferIndex;
    
    // 绑定顶点缓冲区和实例缓冲区（只绑定一次）
    VkBuffer vertexBuffers[] = {m_vertexBuffer, m_instanceBuffers[currentBufferIndex]};
    VkDeviceSize offsets[] = {0, 0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    // 使用第一个模型的管线（z-prepass depthOnly 时用 depth 管线，只写深度）
    if (!m_rendererGroups.empty()) {
        const VoxRenderer* firstRenderer = m_rendererGroups[0].renderer;
        VkPipeline pipeline = depthOnly ? firstRenderer->GetMeshDepthPipeline() : firstRenderer->GetMeshPipeline();
        VkPipelineLayout pipelineLayout = depthOnly ? firstRenderer->GetMeshDepthPipelineLayout() : firstRenderer->GetMeshPipelineLayout();
        
        if (pipeline != VK_NULL_HANDLE && pipelineLayout != VK_NULL_HANDLE) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

            // 推送 push constants
            struct PushConstants {
                glm::mat4 projView;
                glm::mat4 prevProjView;
                glm::vec3 cameraPosition;
                float padding;
            } pushConstants;

            pushConstants.projView = projView;
            pushConstants.prevProjView = prevProjView;
            pushConstants.cameraPosition = cameraPosition;
            pushConstants.padding = 0.0f;

            vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(PushConstants), &pushConstants);

            if (m_supportsMDI) {
                // 使用真正的多重间接绘制 - 单个调用绘制所有面
                if (useGPUCulling && m_supportsComputeShader) {
                    // 使用 GPU 剔除后的可见绘制命令
                    // 使用实际的绘制命令数量，而不是最大数量
                    uint32_t actualDrawCount = m_currentFaceCommandCount > 0 ? m_currentFaceCommandCount : 1;
                    vkCmdDrawIndexedIndirect(commandBuffer, m_visibleDrawCommandBuffer,
                                            0,  // offset = 0
                                            actualDrawCount,  // drawCount = 实际绘制数量
                                            sizeof(FaceDrawCommand));  // stride
                } else {
                    // 使用 CPU 剔除后的绘制命令（只包含可见面）
                    vkCmdDrawIndexedIndirect(commandBuffer, m_drawCommandBuffers[currentBufferIndex],
                                            0,  // offset = 0
                                            m_currentFaceCommandCount,  // drawCount = CPU 剔除后的可见面数量
                                            sizeof(FaceDrawCommand));  // stride
                }
            } else {
                // 使用多个 DrawCall 直接在 CPU 上提交绘制命令（回退方案）
                std::cout << "[VoxelMeshMultiDrawIndirect] Rendering with CPU fallback (groups=" 
                          << m_rendererGroups.size() << ")" << std::endl;
                size_t instanceOffset = 0;
                for (size_t i = 0; i < m_rendererGroups.size(); i++) {
                    const auto& group = m_rendererGroups[i];
                    const VoxelModelData& modelData = group.models[0];

                    // 执行绘制
                    vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(modelData.indexCount),
                                    static_cast<uint32_t>(group.totalInstances),
                                    static_cast<uint32_t>(modelData.firstIndex),
                                    static_cast<int32_t>(modelData.firstVertex),
                                    static_cast<uint32_t>(instanceOffset));

                    instanceOffset += group.totalInstances;
                }
            }
        }
    }
    
    // 切换缓冲区索引，为下一帧做准备
    // 注意：PC 端使用双缓冲增量更新策略
    // GPU 读取当前缓冲区，CPU 写入下一个缓冲区
    m_currentInstanceBufferIndex = (m_currentInstanceBufferIndex + 1) % 2;
    m_currentDrawCommandBufferIndex = (m_currentDrawCommandBufferIndex + 1) % 2;
}

void VoxelMeshMultiDrawIndirect::Clear()
{
    m_rendererGroups.clear();
    m_totalVertices = 0;
    m_totalIndices = 0;
    m_totalInstances = 0;
    m_currentFaceCommandCount = 0;
    m_geometryDataDirty = true;
    
    // 清空网格缓存，确保下次 MergeGeometryData 时重新上传所有数据
    m_meshCache.clear();
    
    // 清空可见模型列表
    m_visibleModels.clear();
    
    // 清空绘制命令缓冲区和实例缓冲区，确保下次重建时从干净的状态开始
    if (m_mappedDrawCommandPtrs[0]) {
        memset(m_mappedDrawCommandPtrs[0], 0, sizeof(FaceDrawCommand) * m_maxVoxelModels * 6);
    }
    if (m_mappedDrawCommandPtrs[1]) {
        memset(m_mappedDrawCommandPtrs[1], 0, sizeof(FaceDrawCommand) * m_maxVoxelModels * 6);
    }
    if (m_mappedInstancePtrs[0]) {
        memset(m_mappedInstancePtrs[0], 0, sizeof(InstanceData) * m_maxVoxelModels);
    }
    if (m_mappedInstancePtrs[1]) {
        memset(m_mappedInstancePtrs[1], 0, sizeof(InstanceData) * m_maxVoxelModels);
    }
}

// ================================================================================
// GPU Culling Implementation
// ================================================================================

// 创建 GPU 剔除资源
bool VoxelMeshMultiDrawIndirect::CreateGPUCullingResources()
{
    if (g_Device == VK_NULL_HANDLE) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] g_Device is null!" << std::endl;
        return false;
    }
    
    VkAllocationCallbacks* allocator = g_Allocator;
    
    // 1. 创建可见绘制命令缓冲区
    VkDeviceSize visibleDrawBufferSize = sizeof(VoxelMeshMultiDrawIndirect::FaceDrawCommand) * m_maxVoxelModels * 6 + sizeof(uint32_t);
    VkBufferCreateInfo visibleDrawBufferInfo{};
    visibleDrawBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    visibleDrawBufferInfo.size = visibleDrawBufferSize;
    visibleDrawBufferInfo.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    visibleDrawBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(g_Device, &visibleDrawBufferInfo, allocator, &m_visibleDrawCommandBuffer) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create visible draw command buffer!" << std::endl;
        return false;
    }
    
    VkMemoryRequirements visibleDrawMemReqs;
    vkGetBufferMemoryRequirements(g_Device, m_visibleDrawCommandBuffer, &visibleDrawMemReqs);
    
    VkMemoryAllocateInfo visibleDrawAllocInfo{};
    visibleDrawAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    visibleDrawAllocInfo.allocationSize = visibleDrawMemReqs.size;
    visibleDrawAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(visibleDrawMemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    if (vkAllocateMemory(g_Device, &visibleDrawAllocInfo, allocator, &m_visibleDrawCommandBufferMemory) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to allocate visible draw command buffer memory!" << std::endl;
        vkDestroyBuffer(g_Device, m_visibleDrawCommandBuffer, allocator);
        return false;
    }
    
    vkBindBufferMemory(g_Device, m_visibleDrawCommandBuffer, m_visibleDrawCommandBufferMemory, 0);
    
    // 2. 创建计数器缓冲区
    VkDeviceSize counterBufferSize = sizeof(uint32_t);
    VkBufferCreateInfo counterBufferInfo{};
    counterBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    counterBufferInfo.size = counterBufferSize;
    counterBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    counterBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(g_Device, &counterBufferInfo, allocator, &m_counterBuffer) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create counter buffer!" << std::endl;
        vkDestroyBuffer(g_Device, m_visibleDrawCommandBuffer, allocator);
        vkFreeMemory(g_Device, m_visibleDrawCommandBufferMemory, allocator);
        return false;
    }
    
    VkMemoryRequirements counterMemReqs;
    vkGetBufferMemoryRequirements(g_Device, m_counterBuffer, &counterMemReqs);
    
    VkMemoryAllocateInfo counterAllocInfo{};
    counterAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    counterAllocInfo.allocationSize = counterMemReqs.size;
    counterAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(counterMemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    if (vkAllocateMemory(g_Device, &counterAllocInfo, allocator, &m_counterBufferMemory) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to allocate counter buffer memory!" << std::endl;
        vkDestroyBuffer(g_Device, m_counterBuffer, allocator);
        vkDestroyBuffer(g_Device, m_visibleDrawCommandBuffer, allocator);
        vkFreeMemory(g_Device, m_visibleDrawCommandBufferMemory, allocator);
        return false;
    }
    
    vkBindBufferMemory(g_Device, m_counterBuffer, m_counterBufferMemory, 0);
    vkMapMemory(g_Device, m_counterBufferMemory, 0, counterBufferSize, 0, reinterpret_cast<void**>(&m_mappedCounterPtr));
    
    // 3. 创建相机数据缓冲区
    VkDeviceSize cameraBufferSize = sizeof(VoxelMeshMultiDrawIndirect::CullingCameraData);
    VkBufferCreateInfo cameraBufferInfo{};
    cameraBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    cameraBufferInfo.size = cameraBufferSize;
    cameraBufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    cameraBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(g_Device, &cameraBufferInfo, allocator, &m_cullingCameraBuffer) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create camera buffer!" << std::endl;
        vkDestroyBuffer(g_Device, m_counterBuffer, allocator);
        vkFreeMemory(g_Device, m_counterBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_visibleDrawCommandBuffer, allocator);
        vkFreeMemory(g_Device, m_visibleDrawCommandBufferMemory, allocator);
        return false;
    }
    
    VkMemoryRequirements cameraMemReqs;
    vkGetBufferMemoryRequirements(g_Device, m_cullingCameraBuffer, &cameraMemReqs);
    
    VkMemoryAllocateInfo cameraAllocInfo{};
    cameraAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    cameraAllocInfo.allocationSize = cameraMemReqs.size;
    cameraAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(cameraMemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    if (vkAllocateMemory(g_Device, &cameraAllocInfo, allocator, &m_cullingCameraBufferMemory) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to allocate camera buffer memory!" << std::endl;
        vkDestroyBuffer(g_Device, m_cullingCameraBuffer, allocator);
        vkDestroyBuffer(g_Device, m_counterBuffer, allocator);
        vkFreeMemory(g_Device, m_counterBufferMemory, allocator);
        vkDestroyBuffer(g_Device, m_visibleDrawCommandBuffer, allocator);
        vkFreeMemory(g_Device, m_visibleDrawCommandBufferMemory, allocator);
        return false;
    }
    
    vkBindBufferMemory(g_Device, m_cullingCameraBuffer, m_cullingCameraBufferMemory, 0);
    vkMapMemory(g_Device, m_cullingCameraBufferMemory, 0, cameraBufferSize, 0, reinterpret_cast<void**>(&m_mappedCameraPtr));
    
    // 4. 创建 dummy Hi-Z 图像（1x1 R8 格式，用于 Hi-Z 不可用时的描述符集更新）
    VkImageCreateInfo dummyImageInfo{};
    dummyImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    dummyImageInfo.imageType = VK_IMAGE_TYPE_2D;
    dummyImageInfo.format = VK_FORMAT_R8_UNORM;
    dummyImageInfo.extent = { 1, 1, 1 };
    dummyImageInfo.mipLevels = 1;
    dummyImageInfo.arrayLayers = 1;
    dummyImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    dummyImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    dummyImageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    dummyImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    dummyImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    if (vkCreateImage(g_Device, &dummyImageInfo, allocator, &m_dummyHiZImage) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create dummy Hi-Z image!" << std::endl;
        return false;
    }
    
    VkMemoryRequirements dummyImageMemReqs;
    vkGetImageMemoryRequirements(g_Device, m_dummyHiZImage, &dummyImageMemReqs);
    
    VkMemoryAllocateInfo dummyImageAllocInfo{};
    dummyImageAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    dummyImageAllocInfo.allocationSize = dummyImageMemReqs.size;
    dummyImageAllocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(dummyImageMemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    if (vkAllocateMemory(g_Device, &dummyImageAllocInfo, allocator, &m_dummyHiZImageMemory) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to allocate dummy Hi-Z image memory!" << std::endl;
        vkDestroyImage(g_Device, m_dummyHiZImage, allocator);
        return false;
    }
    
    vkBindImageMemory(g_Device, m_dummyHiZImage, m_dummyHiZImageMemory, 0);
    
    // 转换 dummy 图像到 SHADER_READ_ONLY_OPTIMAL 布局
    VkCommandBuffer commandBuffer;
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = g_CommandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    
    vkAllocateCommandBuffers(g_Device, &allocInfo, &commandBuffer);
    
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    
    VkImageMemoryBarrier imageBarrier{};
    imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.image = m_dummyHiZImage;
    imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBarrier.subresourceRange.baseMipLevel = 0;
    imageBarrier.subresourceRange.levelCount = 1;
    imageBarrier.subresourceRange.baseArrayLayer = 0;
    imageBarrier.subresourceRange.layerCount = 1;
    imageBarrier.srcAccessMask = 0;
    imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &imageBarrier);
    
    vkEndCommandBuffer(commandBuffer);
    
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    
    vkQueueSubmit(g_Queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_Queue);
    
    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
    
    // 创建 dummy 图像视图
    VkImageViewCreateInfo dummyImageViewInfo{};
    dummyImageViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    dummyImageViewInfo.image = m_dummyHiZImage;
    dummyImageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dummyImageViewInfo.format = VK_FORMAT_R8_UNORM;
    dummyImageViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dummyImageViewInfo.subresourceRange.baseMipLevel = 0;
    dummyImageViewInfo.subresourceRange.levelCount = 1;
    dummyImageViewInfo.subresourceRange.baseArrayLayer = 0;
    dummyImageViewInfo.subresourceRange.layerCount = 1;
    
    if (vkCreateImageView(g_Device, &dummyImageViewInfo, allocator, &m_dummyHiZImageView) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create dummy Hi-Z image view!" << std::endl;
        vkFreeMemory(g_Device, m_dummyHiZImageMemory, allocator);
        vkDestroyImage(g_Device, m_dummyHiZImage, allocator);
        return false;
    }
    
    return true;
}

// 创建计算着色器管线
bool VoxelMeshMultiDrawIndirect::CreateCullingPipeline()
{
    if (g_Device == VK_NULL_HANDLE) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] g_Device is null!" << std::endl;
        return false;
    }
    
    VkAllocationCallbacks* allocator = g_Allocator;
    
    // 1. 读取计算着色器代码
    std::vector<char> computeShaderCode;
    
#ifdef __ANDROID__
    // Android: 使用 SDL IO 从 assets 目录读取（engine/ = 引擎系统资产，sync_assets.ps1 同步进 APK）
    std::string fullPath = "engine/shaders/spv/voxel_culling.comp.spv";
    SDL_IOStream* io = SDL_IOFromFile(fullPath.c_str(), "rb");
    if (io == nullptr) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to open compute shader: " << fullPath << " (SDL Error: " << SDL_GetError() << ")" << std::endl;
        std::cerr << "[VoxelMeshMultiDrawIndirect] GPU culling will be disabled, falling back to CPU culling" << std::endl;
        m_supportsComputeShader = false;
        return true;
    }
    
    Sint64 fileSize = SDL_GetIOSize(io);
    if (fileSize <= 0) {
        SDL_CloseIO(io);
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to get compute shader size: " << fullPath << std::endl;
        m_supportsComputeShader = false;
        return true;
    }
    
    computeShaderCode.resize((size_t)fileSize);
    if (SDL_ReadIO(io, computeShaderCode.data(), (size_t)fileSize) != (size_t)fileSize) {
        SDL_CloseIO(io);
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to read compute shader: " << fullPath << std::endl;
        m_supportsComputeShader = false;
        return true;
    }
    SDL_CloseIO(io);
    
#else
    // 桌面端：通过 EngineConfig 集中解析 shader 路径（优先简化版 shader）
    std::vector<std::string> possiblePaths = {
        EngineConfig::GetShaderPath("voxel_culling_simple.comp.spv"),
        EngineConfig::GetShaderPath("voxel_culling.comp.spv")
    };
    
    std::string shaderPath;
    std::ifstream file;
    bool found = false;
    
    for (const auto& path : possiblePaths) {
        file.open(path, std::ios::ate | std::ios::binary);
        if (file.is_open()) {
            shaderPath = path;
            found = true;
            break;
        }
    }
    
    if (!found) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to open compute shader: engine/shaders/spv/voxel_culling.comp.spv" << std::endl;
        std::cerr << "[VoxelMeshMultiDrawIndirect] GPU culling will be disabled, falling back to CPU culling" << std::endl;
        m_supportsComputeShader = false;
        return true; // 返回 true 允许继续初始化（使用 CPU 回退）
    }
    
    size_t fileSize = static_cast<size_t>(file.tellg());
    computeShaderCode.resize(fileSize);
    file.seekg(0);
    file.read(computeShaderCode.data(), fileSize);
    file.close();
    
#endif
    
    // 2. 创建 shader module
    VkShaderModuleCreateInfo shaderModuleInfo{};
    shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderModuleInfo.codeSize = computeShaderCode.size();
    shaderModuleInfo.pCode = reinterpret_cast<const uint32_t*>(computeShaderCode.data());
    
    VkShaderModule computeShaderModule;
    if (vkCreateShaderModule(g_Device, &shaderModuleInfo, allocator, &computeShaderModule) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create compute shader module!" << std::endl;
        return false;
    }
    
    // 3. 设置管线布局
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_cullingDescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 0;
    pipelineLayoutInfo.pPushConstantRanges = nullptr;
    
    if (vkCreatePipelineLayout(g_Device, &pipelineLayoutInfo, allocator, &m_cullingPipelineLayout) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create culling pipeline layout!" << std::endl;
        vkDestroyShaderModule(g_Device, computeShaderModule, allocator);
        return false;
    }
    
    // 4. 创建计算管线
    VkPipelineShaderStageCreateInfo shaderStageInfo{};
    shaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStageInfo.module = computeShaderModule;
    shaderStageInfo.pName = "main";
    shaderStageInfo.pSpecializationInfo = nullptr;
    
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = nullptr;
    pipelineInfo.flags = 0;
    pipelineInfo.stage = shaderStageInfo;
    pipelineInfo.layout = m_cullingPipelineLayout;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;
    
    if (vkCreateComputePipelines(g_Device, VK_NULL_HANDLE, 1, &pipelineInfo, allocator, &m_cullingPipeline) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create culling pipeline!" << std::endl;
        vkDestroyPipelineLayout(g_Device, m_cullingPipelineLayout, allocator);
        vkDestroyShaderModule(g_Device, computeShaderModule, allocator);
        return false;
    }
    
    vkDestroyShaderModule(g_Device, computeShaderModule, allocator);
    
    return true;
}

// 创建描述符集
bool VoxelMeshMultiDrawIndirect::CreateCullingDescriptorSet()
{
    if (g_Device == VK_NULL_HANDLE) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] g_Device is null!" << std::endl;
        return false;
    }
    
    VkAllocationCallbacks* allocator = g_Allocator;
    
    // 1. 创建描述符集布局（完整版本，支持 Hi-Z 遮挡剔除）
    VkDescriptorSetLayoutBinding bindings[5] = {};
    
    // Binding 0: 相机数据（Uniform Buffer）
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[0].pImmutableSamplers = nullptr;
    
    // Binding 1: 绘制命令（Storage Buffer）
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].pImmutableSamplers = nullptr;
    
    // Binding 2: 可见绘制命令（Storage Buffer）
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].pImmutableSamplers = nullptr;
    
    // Binding 3: 实例数据（Storage Buffer）- 包含 AABB 信息
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[3].pImmutableSamplers = nullptr;
    
    // Binding 4: Hi-Z 深度金字塔纹理（Combined Image Sampler）
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[4].pImmutableSamplers = nullptr;
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext = nullptr;
    layoutInfo.flags = 0;
    layoutInfo.bindingCount = 5;
    layoutInfo.pBindings = bindings;
    
    if (vkCreateDescriptorSetLayout(g_Device, &layoutInfo, allocator, &m_cullingDescriptorSetLayout) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create culling descriptor set layout!" << std::endl;
        return false;
    }
    
    // 2. 创建描述符池（完整版本）
    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 3;  // 3 个 storage buffer
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[2].descriptorCount = 1;  // Hi-Z 纹理
    
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.pNext = nullptr;
    poolInfo.flags = 0;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = 1;
    
    if (vkCreateDescriptorPool(g_Device, &poolInfo, allocator, &m_cullingDescriptorPool) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create culling descriptor pool!" << std::endl;
        vkDestroyDescriptorSetLayout(g_Device, m_cullingDescriptorSetLayout, allocator);
        return false;
    }
    
    // 3. 分配描述符集
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.pNext = nullptr;
    allocInfo.descriptorPool = m_cullingDescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_cullingDescriptorSetLayout;
    
    if (vkAllocateDescriptorSets(g_Device, &allocInfo, &m_cullingDescriptorSet) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to allocate culling descriptor set!" << std::endl;
        vkDestroyDescriptorPool(g_Device, m_cullingDescriptorPool, allocator);
        vkDestroyDescriptorSetLayout(g_Device, m_cullingDescriptorSetLayout, allocator);
        return false;
    }
    
    // 4. 更新描述符集
    VkDescriptorBufferInfo cameraBufferInfo{};
    cameraBufferInfo.buffer = m_cullingCameraBuffer;
    cameraBufferInfo.offset = 0;
    cameraBufferInfo.range = sizeof(VoxelMeshMultiDrawIndirect::CullingCameraData);
    
    // 使用当前缓冲区索引
    size_t currentBufferIndex = m_currentInstanceBufferIndex;
    
    VkDescriptorBufferInfo drawCommandBufferInfo{};
    drawCommandBufferInfo.buffer = m_drawCommandBuffers[currentBufferIndex];
    drawCommandBufferInfo.offset = 0;
    drawCommandBufferInfo.range = VK_WHOLE_SIZE;
    
    VkDescriptorBufferInfo visibleDrawBufferInfo{};
    visibleDrawBufferInfo.buffer = m_visibleDrawCommandBuffer;
    visibleDrawBufferInfo.offset = 0;
    visibleDrawBufferInfo.range = VK_WHOLE_SIZE;
    
    // 实例数据缓冲区（包含 AABB 信息）
    VkDescriptorBufferInfo instanceDataBufferInfo{};
    instanceDataBufferInfo.buffer = m_instanceBuffers[currentBufferIndex];
    instanceDataBufferInfo.offset = 0;
    instanceDataBufferInfo.range = VK_WHOLE_SIZE;
    
    // Hi-Z 纹理 - 使用已定义的全局变量（使用上一帧的 Hi-Z 数据用于剔除）
    VkDescriptorImageInfo hizImageInfo{};
    hizImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    
    // 当 Hi-Z 可用时使用真实的 Hi-Z 纹理,否则使用 dummy 图像
    VkImageView hizView = g_SceneRenderer.GetHiZShader().GetHiZTextureViewForCulling();
    if (hizView != VK_NULL_HANDLE) {
        hizImageInfo.imageView = hizView;
    } else {
        hizImageInfo.imageView = m_dummyHiZImageView;
    }
    hizImageInfo.sampler = g_GameRenderTarget.GetHiZSampler(); // 使用支持多层 mip 的采样器
    
    VkWriteDescriptorSet descriptorWrites[5] = {};
    
    // Writing binding 0: 相机数据
    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = m_cullingDescriptorSet;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pBufferInfo = &cameraBufferInfo;
    
    // Writing binding 1: 绘制命令
    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstSet = m_cullingDescriptorSet;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pBufferInfo = &drawCommandBufferInfo;
    
    // Writing binding 2: 可见绘制命令
    descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[2].dstSet = m_cullingDescriptorSet;
    descriptorWrites[2].dstBinding = 2;
    descriptorWrites[2].dstArrayElement = 0;
    descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrites[2].descriptorCount = 1;
    descriptorWrites[2].pBufferInfo = &visibleDrawBufferInfo;
    
    // Writing binding 3: 实例数据
    descriptorWrites[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[3].dstSet = m_cullingDescriptorSet;
    descriptorWrites[3].dstBinding = 3;
    descriptorWrites[3].dstArrayElement = 0;
    descriptorWrites[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrites[3].descriptorCount = 1;
    descriptorWrites[3].pBufferInfo = &instanceDataBufferInfo;
    
    // Writing binding 4: Hi-Z 纹理（始终更新，确保描述符集完整初始化）
    // 当 Hi-Z 不可用时，使用空视图 + 有效采样器，着色器通过 enableHiZCulling 标志跳过 Hi-Z 测试
    descriptorWrites[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[4].dstSet = m_cullingDescriptorSet;
    descriptorWrites[4].dstBinding = 4;
    descriptorWrites[4].dstArrayElement = 0;
    descriptorWrites[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[4].descriptorCount = 1;
    descriptorWrites[4].pImageInfo = &hizImageInfo;
    
    vkUpdateDescriptorSets(g_Device, 5, descriptorWrites, 0, nullptr);
    
    return true;
}
