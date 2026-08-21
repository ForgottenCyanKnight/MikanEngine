#include "Rendering/VoxelMeshMultiDrawIndirect.h"
#include "Core/EngineGlobal.h"
#include "Core/EngineConfig.h"
#include "VulkanManager.h"
#include <SDL3/SDL_iostream.h>
#include <iostream>
#include <fstream>

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
    
    std::cout << "[VoxelMeshMultiDrawIndirect] GPU culling buffers created successfully" << std::endl;
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
    // Android: 使用 SDL IO 从 assets 目录读取（GetShaderPath 在 Android 下返回 shaders/spv/ 前缀，与 SDL assets 约定一致）
    std::string fullPath = EngineConfig::GetShaderPath("voxel_culling.comp.spv");
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
    
    std::cout << "[VoxelMeshMultiDrawIndirect] Loaded compute shader: " << shaderPath << " (" << fileSize << " bytes)" << std::endl;
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
    
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = shaderStageInfo;
    pipelineInfo.layout = m_cullingPipelineLayout;
    
    if (vkCreateComputePipelines(g_Device, VK_NULL_HANDLE, 1, &pipelineInfo, allocator, &m_cullingPipeline) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create culling pipeline!" << std::endl;
        vkDestroyPipelineLayout(g_Device, m_cullingPipelineLayout, allocator);
        vkDestroyShaderModule(g_Device, computeShaderModule, allocator);
        return false;
    }
    
    vkDestroyShaderModule(g_Device, computeShaderModule, allocator);
    
    std::cout << "[VoxelMeshMultiDrawIndirect] GPU culling pipeline created successfully" << std::endl;
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
    
    // 1. 创建描述符集布局（简化版 - 只保留必要的绑点）
    VkDescriptorSetLayoutBinding bindings[3] = {};
    
    // Binding 0: 相机数据（Uniform Buffer）
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    
    // Binding 1: 绘制命令（Storage Buffer）
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    
    // Binding 2: 可见绘制命令（Storage Buffer）
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    
    if (vkCreateDescriptorSetLayout(g_Device, &layoutInfo, allocator, &m_cullingDescriptorSetLayout) != VK_SUCCESS) {
        std::cerr << "[VoxelMeshMultiDrawIndirect] Failed to create culling descriptor set layout!" << std::endl;
        return false;
    }
    
    // 2. 创建描述符池（简化版）
    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 2;  // 只需要 2 个 storage buffer
    
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
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
    
    VkWriteDescriptorSet descriptorWrites[3] = {};
    
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
    
    vkUpdateDescriptorSets(g_Device, 3, descriptorWrites, 0, nullptr);
    
    std::cout << "[VoxelMeshMultiDrawIndirect] GPU culling descriptor set created successfully" << std::endl;
    return true;
}
