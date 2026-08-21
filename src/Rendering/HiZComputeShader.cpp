#include "HiZComputeShader.h"
#include "Core/EngineGlobal.h"
#include "Core/EngineConfig.h"
#include "Rendering/RendererBase.h"
#include "Rendering/RenderTarget.h"
#include <iostream>
#include <fstream>
#include <filesystem>

extern RenderTarget g_GameRenderTarget;

static uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &memProperties);
    
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

HiZComputeShader::~HiZComputeShader() {
    Cleanup();
}

bool HiZComputeShader::Init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t width, uint32_t height) {
    m_Device = device;
    m_PhysicalDevice = physicalDevice;
    m_Width = width;
    m_Height = height;

    m_MipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
    std::cout << "[HiZComputeShader] Initializing (" << width << "x" << height << ", " << m_MipLevels << " mips)..." << std::endl;

    if (!CreateDescriptorSetLayout()) {
        std::cerr << "[HiZComputeShader] Failed to create descriptor set layout" << std::endl;
        return false;
    }

    std::string shaderPath = EngineConfig::GetShaderPath("voxel_hiz.comp.spv");
    if (!CreateShaderModule(shaderPath)) {
        std::cerr << "[HiZComputeShader] Failed to create shader module" << std::endl;
        return false;
    }

    if (!CreatePipeline()) {
        std::cerr << "[HiZComputeShader] Failed to create pipeline" << std::endl;
        return false;
    }

    if (!CreateDescriptorPool()) {
        std::cerr << "[HiZComputeShader] Failed to create descriptor pool" << std::endl;
        return false;
    }

    if (!CreateDescriptorSets(m_MipLevels)) {
        std::cerr << "[HiZComputeShader] Failed to create descriptor sets" << std::endl;
        return false;
    }

    return true;
}

VkImageView HiZComputeShader::GetHiZTextureView() const {
    // 返回第一个 mip 层级的纹理视图（最高分辨率）
    if (m_MipImageViews.empty()) {
        return VK_NULL_HANDLE;
    }
    return m_MipImageViews[0];
}

VkImageView HiZComputeShader::GetHiZTextureViewForCulling() const {
    // 返回用于剔除的 Hi-Z 纹理视图（上一帧的数据）
    if (m_CullingMipImageViews.empty()) {
        return VK_NULL_HANDLE;
    }
    // 使用上一帧的缓冲区（当前写入的是这帧，所以读上一帧就是 1 - m_WriteBufferIndex）
    int readIndex = 1 - m_WriteBufferIndex;
    return m_CullingMipImageViews[readIndex];
}

void HiZComputeShader::SwapBuffers() {
    // 交换读写缓冲区
    m_WriteBufferIndex = 1 - m_WriteBufferIndex;
}

VkImage HiZComputeShader::GetHiZTextureImage() const {
    // 返回第一个 mip 层级的纹理
    if (m_MipImages.empty()) {
        return VK_NULL_HANDLE;
    }
    return m_MipImages[0];
}

void HiZComputeShader::Cleanup() {
    // 检查设备是否有效
    extern VkDevice g_Device;
    extern VkAllocationCallbacks* g_Allocator;
    bool deviceValid = (m_Device != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE);
    
    for (size_t i = 0; i < m_MipImages.size(); ++i) {
        if (m_MipImageViews[i] != VK_NULL_HANDLE && deviceValid) {
            vkDestroyImageView(m_Device, m_MipImageViews[i], g_Allocator);
            m_MipImageViews[i] = VK_NULL_HANDLE;
        }
        if (m_MipImages[i] != VK_NULL_HANDLE && deviceValid) {
            vkDestroyImage(m_Device, m_MipImages[i], g_Allocator);
            m_MipImages[i] = VK_NULL_HANDLE;
        }
        if (m_MipImageMemories[i] != VK_NULL_HANDLE && deviceValid) {
            vkFreeMemory(m_Device, m_MipImageMemories[i], g_Allocator);
            m_MipImageMemories[i] = VK_NULL_HANDLE;
        }
    }

    m_MipImageViews.clear();
    m_MipImages.clear();
    m_MipImageMemories.clear();
    
    // 清理双缓冲资源
    for (size_t i = 0; i < m_CullingMipImageViews.size(); ++i) {
        if (m_CullingMipImageViews[i] != VK_NULL_HANDLE && deviceValid) {
            vkDestroyImageView(m_Device, m_CullingMipImageViews[i], g_Allocator);
            m_CullingMipImageViews[i] = VK_NULL_HANDLE;
        }
        if (m_CullingMipImages[i] != VK_NULL_HANDLE && deviceValid) {
            vkDestroyImage(m_Device, m_CullingMipImages[i], g_Allocator);
            m_CullingMipImages[i] = VK_NULL_HANDLE;
        }
        if (m_CullingMipImageMemories[i] != VK_NULL_HANDLE && deviceValid) {
            vkFreeMemory(m_Device, m_CullingMipImageMemories[i], g_Allocator);
            m_CullingMipImageMemories[i] = VK_NULL_HANDLE;
        }
    }
    m_CullingMipImageViews.clear();
    m_CullingMipImages.clear();
    m_CullingMipImageMemories.clear();

    if (m_DescriptorPool != VK_NULL_HANDLE && deviceValid) {
        vkDestroyDescriptorPool(m_Device, m_DescriptorPool, g_Allocator);
        m_DescriptorPool = VK_NULL_HANDLE;
    }

    if (m_Pipeline != VK_NULL_HANDLE && deviceValid) {
        vkDestroyPipeline(m_Device, m_Pipeline, g_Allocator);
        m_Pipeline = VK_NULL_HANDLE;
    }

    if (m_PipelineLayout != VK_NULL_HANDLE && deviceValid) {
        vkDestroyPipelineLayout(m_Device, m_PipelineLayout, g_Allocator);
        m_PipelineLayout = VK_NULL_HANDLE;
    }

    if (m_ShaderModule != VK_NULL_HANDLE && deviceValid) {
        vkDestroyShaderModule(m_Device, m_ShaderModule, g_Allocator);
        m_ShaderModule = VK_NULL_HANDLE;
    }

    if (m_DescriptorSetLayout != VK_NULL_HANDLE && deviceValid) {
        vkDestroyDescriptorSetLayout(m_Device, m_DescriptorSetLayout, g_Allocator);
        m_DescriptorSetLayout = VK_NULL_HANDLE;
    }

    m_DescriptorSets.clear();
    m_Device = VK_NULL_HANDLE;
    m_PhysicalDevice = VK_NULL_HANDLE;
}

void HiZComputeShader::GenerateMipLevels(VkCommandBuffer commandBuffer, VkImage depthImage, uint32_t mipLevels) {
    uint32_t levelsToGenerate = std::min(mipLevels, m_MipLevels) - 1;

    if (levelsToGenerate == 0) return;

    TransitionDepthForRead(commandBuffer, depthImage, 0);

    bool hasDispatched1x1 = false;  // 使用非静态变量，每帧都会重置
    
    // 修复：循环条件应该是 <= levelsToGenerate，确保所有mip层级都被生成
    for (uint32_t level = 1; level <= levelsToGenerate; ++level) {
        // std::cout << "[HiZComputeShader] Generating mip level " << level << std::endl;
        
        uint32_t srcWidth = std::max(m_Width >> (level - 1), 1u);
        uint32_t srcHeight = std::max(m_Height >> (level - 1), 1u);
        
        uint32_t groupCountX = (srcWidth + 15) / 16;
        uint32_t groupCountY = (srcHeight + 15) / 16;
        
        // 当groupCount已经是(1,1,1)时，停止生成（避免重复的1x1 dispatch）
        if (groupCountX == 1 && groupCountY == 1) {
            if (hasDispatched1x1) {
                break;
            }
            hasDispatched1x1 = true;
        }

        // 转换目标 mip 层级为写入布局
        TransitionMipLevelForWrite(commandBuffer, level);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);

        // std::cout << "[HiZComputeShader] Dispatching level " << level << " with " 
        //           << groupCountX << "x" << groupCountY << " groups (src: " 
        //           << srcWidth << "x" << srcHeight << ")" << std::endl;

        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, 
                                &m_DescriptorSets[level - 1], 0, nullptr);

        // 传递 push constants: SourceMipLevel 和 IsSourceMipImage
        int32_t pushConstants[2] = { 
            static_cast<int32_t>(level - 1),  // SourceMipLevel
            (level == 1) ? 0 : 1              // IsSourceMipImage: 第一级使用深度纹理 (0)，后续使用 m_MipImages (1)
        };
        vkCmdPushConstants(commandBuffer, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 
                          sizeof(pushConstants), pushConstants);

        vkCmdDispatch(commandBuffer, groupCountX, groupCountY, 1);

        // 将刚写入的层级转换为读取布局，供下一级使用
        TransitionMipLevelForRead(commandBuffer, level);

        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(commandBuffer, 
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    
    // 将生成的 Hi-Z 数据复制到双缓冲用于下一帧的剔除
    CopyToCullingBuffer(commandBuffer);
}

void HiZComputeShader::CopyToCullingBuffer(VkCommandBuffer commandBuffer) {
    // 确保双缓冲资源已创建
    if (m_CullingMipImages.empty() && !m_MipImages.empty()) {
        // 双缓冲：每个缓冲区有 2 个图像（每个缓冲区包含所有 mip 层级）
        m_CullingMipImages.resize(2);
        m_CullingMipImageMemories.resize(2);
        m_CullingMipImageViews.resize(2);
        
        VkFormat depthFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
        
        for (int buf = 0; buf < 2; ++buf) {
            // 每个缓冲区创建一个包含所有 mip 层级的图像
            VkImageCreateInfo imageInfo{};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.extent.width = std::max(m_Width >> 1, 1u); // mip 1 的尺寸
            imageInfo.extent.height = std::max(m_Height >> 1, 1u);
            imageInfo.extent.depth = 1;
            imageInfo.mipLevels = m_MipLevels - 1; // 包含所有 mip 层级
            imageInfo.arrayLayers = 1;
            imageInfo.format = depthFormat;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateImage(m_Device, &imageInfo, g_Allocator, &m_CullingMipImages[buf]) != VK_SUCCESS) {
                std::cerr << "[HiZComputeShader] Failed to create culling mip image " << buf << std::endl;
                return;
            }

            VkMemoryRequirements memRequirements;
            vkGetImageMemoryRequirements(m_Device, m_CullingMipImages[buf], &memRequirements);

            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memRequirements.size;
            allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

            if (vkAllocateMemory(m_Device, &allocInfo, g_Allocator, &m_CullingMipImageMemories[buf]) != VK_SUCCESS) {
                std::cerr << "[HiZComputeShader] Failed to allocate memory for culling mip image " << buf << std::endl;
                return;
            }

            vkBindImageMemory(m_Device, m_CullingMipImages[buf], m_CullingMipImageMemories[buf], 0);

            // 创建视图，包含所有 mip 层级
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = m_CullingMipImages[buf];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = depthFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1; // [RENDERDOC] single mip view (multi-mip view crashes RenderDoc dispatch inspection)
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;

            if (vkCreateImageView(m_Device, &viewInfo, g_Allocator, &m_CullingMipImageViews[buf]) != VK_SUCCESS) {
                std::cerr << "[HiZComputeShader] Failed to create culling image view for buffer " << buf << std::endl;
                return;
            }
        }
    }
    
    // 写入目标缓冲区的索引（当前帧写入，下帧读取）
    int writeIndex = m_WriteBufferIndex;
    
    // 复制每个 mip 层级到目标图像的对应层级
    for (uint32_t i = 1; i < m_MipLevels; ++i) {
        int srcMipIndex = i; // m_MipImages[i] 是源 mip 图像
        uint32_t mipWidth = std::max(m_Width >> i, 1u);
        uint32_t mipHeight = std::max(m_Height >> i, 1u);
        
        // 转换目标 mip 层级为传输dst布局
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_CullingMipImages[writeIndex];
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = i - 1; // 目标 mip 层级（从0开始）
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        
        // 转换源 mip 层级为传输src布局
        VkImageMemoryBarrier srcBarrier{};
        srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        srcBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        srcBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.image = m_MipImages[srcMipIndex];
        srcBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        srcBarrier.subresourceRange.baseMipLevel = 0;
        srcBarrier.subresourceRange.levelCount = 1;
        srcBarrier.subresourceRange.baseArrayLayer = 0;
        srcBarrier.subresourceRange.layerCount = 1;
        
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &srcBarrier);
        
        // 复制图像
        VkImageCopy copyRegion{};
        copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.srcSubresource.mipLevel = 0;
        copyRegion.srcSubresource.baseArrayLayer = 0;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.dstSubresource.mipLevel = i - 1; // 目标 mip 层级
        copyRegion.dstSubresource.baseArrayLayer = 0;
        copyRegion.dstSubresource.layerCount = 1;
        copyRegion.extent.width = mipWidth;
        copyRegion.extent.height = mipHeight;
        copyRegion.extent.depth = 1;
        
        vkCmdCopyImage(commandBuffer, m_MipImages[srcMipIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       m_CullingMipImages[writeIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
        
        // 恢复源图像布局
        VkImageMemoryBarrier restoreBarrier{};
        restoreBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        restoreBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        restoreBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        restoreBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        restoreBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        restoreBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        restoreBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        restoreBarrier.image = m_MipImages[srcMipIndex];
        restoreBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        restoreBarrier.subresourceRange.baseMipLevel = 0;
        restoreBarrier.subresourceRange.levelCount = 1;
        restoreBarrier.subresourceRange.baseArrayLayer = 0;
        restoreBarrier.subresourceRange.layerCount = 1;
        
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &restoreBarrier);
    }
    
    // 将 culling mip 图像从 TRANSFER_DST_OPTIMAL 转换到 SHADER_READ_ONLY_OPTIMAL
    // 这样下一帧的剔除计算着色器才能正确采样
    VkImageMemoryBarrier cullingImageBarrier{};
    cullingImageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    cullingImageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    cullingImageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    cullingImageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    cullingImageBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    cullingImageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cullingImageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cullingImageBarrier.image = m_CullingMipImages[writeIndex];
    cullingImageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cullingImageBarrier.subresourceRange.baseMipLevel = 0;
    cullingImageBarrier.subresourceRange.levelCount = m_MipLevels - 1;
    cullingImageBarrier.subresourceRange.baseArrayLayer = 0;
    cullingImageBarrier.subresourceRange.layerCount = 1;
    
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &cullingImageBarrier);
    
    // 注意：SwapBuffers() 不在这里调用，而是在每帧渲染结束时调用一次
    // 避免场景视图和游戏视图各自调用导致双缓冲索引错乱
}

bool HiZComputeShader::CreateShaderModule(const std::string& shaderPath) {
    std::ifstream file(shaderPath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[HiZComputeShader] Failed to open shader file: " << shaderPath << std::endl;
        
        // 尝试使用绝对路径
        std::string absolutePath = std::filesystem::absolute(shaderPath).string();
        std::cerr << "[HiZComputeShader] Trying absolute path: " << absolutePath << std::endl;
        
        file.open(absolutePath, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "[HiZComputeShader] Failed to open shader file with absolute path: " << absolutePath << std::endl;
            return false;
        }
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);

    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = buffer.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(buffer.data());

    if (vkCreateShaderModule(m_Device, &createInfo, g_Allocator, &m_ShaderModule) != VK_SUCCESS) {
        std::cerr << "[HiZComputeShader] Failed to create shader module from file: " << shaderPath << std::endl;
        return false;
    }

    return true;
}

bool HiZComputeShader::CreateDescriptorSetLayout() {
    std::vector<VkDescriptorSetLayoutBinding> bindings(3);

    // Binding 0: 深度纹理采样器（用于第一级mip生成）
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[0].pImmutableSamplers = nullptr;

    // Binding 1: 输出存储图像
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].pImmutableSamplers = nullptr;

    // Binding 2: HiZ mip图像采样器（用于后续级mip生成）
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_Device, &layoutInfo, g_Allocator, &m_DescriptorSetLayout) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool HiZComputeShader::CreatePipeline() {
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(int32_t) * 2;  // SourceMipLevel 和 IsSourceMipImage

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_DescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(m_Device, &pipelineLayoutInfo, g_Allocator, &m_PipelineLayout) != VK_SUCCESS) {
        return false;
    }

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = m_ShaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = m_PipelineLayout;

    if (vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1, &pipelineInfo, g_Allocator, &m_Pipeline) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool HiZComputeShader::CreateDescriptorPool() {
    std::vector<VkDescriptorPoolSize> poolSizes(2);
    
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = m_MipLevels * 2;  // 每个descriptor set有2个sampler绑定
    
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = m_MipLevels;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = m_MipLevels;

    if (vkCreateDescriptorPool(m_Device, &poolInfo, g_Allocator, &m_DescriptorPool) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool HiZComputeShader::CreateDescriptorSets(uint32_t mipLevels) {
    
    m_MipImages.resize(mipLevels);
    m_MipImageMemories.resize(mipLevels);
    m_MipImageViews.resize(mipLevels);
    m_DescriptorSets.resize(mipLevels - 1);

    VkFormat depthFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

    for (uint32_t i = 0; i < mipLevels; ++i) {
        uint32_t width = std::max(m_Width >> i, 1u);
        uint32_t height = std::max(m_Height >> i, 1u);

        if (i > 0) {
            VkImageCreateInfo imageInfo{};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.extent.width = width;
            imageInfo.extent.height = height;
            imageInfo.extent.depth = 1;
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.format = depthFormat;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateImage(m_Device, &imageInfo, g_Allocator, &m_MipImages[i]) != VK_SUCCESS) {
                std::cerr << "[HiZComputeShader] Failed to create mip image " << i << std::endl;
                return false;
            }

            VkMemoryRequirements memRequirements;
            vkGetImageMemoryRequirements(m_Device, m_MipImages[i], &memRequirements);

            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memRequirements.size;
            allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

            if (vkAllocateMemory(m_Device, &allocInfo, g_Allocator, &m_MipImageMemories[i]) != VK_SUCCESS) {
                std::cerr << "[HiZComputeShader] Failed to allocate memory for mip image " << i << std::endl;
                return false;
            }

            vkBindImageMemory(m_Device, m_MipImages[i], m_MipImageMemories[i], 0);

            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = m_MipImages[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = depthFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;

            if (vkCreateImageView(m_Device, &viewInfo, g_Allocator, &m_MipImageViews[i]) != VK_SUCCESS) {
                std::cerr << "[HiZComputeShader] Failed to create image view for mip " << i << std::endl;
                return false;
            }
        }
    }

    std::vector<VkDescriptorSetLayout> layouts(mipLevels - 1, m_DescriptorSetLayout);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_DescriptorPool;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(mipLevels - 1);
    allocInfo.pSetLayouts = layouts.data();

    if (vkAllocateDescriptorSets(m_Device, &allocInfo, m_DescriptorSets.data()) != VK_SUCCESS) {
        std::cerr << "[HiZComputeShader] Failed to allocate descriptor sets" << std::endl;
        return false;
    }

    for (uint32_t i = 0; i < m_MipLevels - 1; ++i) {
        // Binding 0: 深度纹理（只在第一级使用）
        VkDescriptorImageInfo depthInfo{};
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        depthInfo.imageView = g_GameRenderTarget.GetDepthImageView();
        depthInfo.sampler = g_GameRenderTarget.GetSampler();

        // Binding 1: 输出存储图像
        VkDescriptorImageInfo dstInfo{};
        dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        dstInfo.imageView = m_MipImageViews[i + 1];

        // Binding 2: HiZ mip图像（从第二级开始使用）
        VkDescriptorImageInfo hizInfo{};
        hizInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        hizInfo.imageView = m_MipImageViews[i];
        hizInfo.sampler = g_GameRenderTarget.GetSampler();

        VkWriteDescriptorSet writes[3]{};
        
        // Binding 0: 深度纹理
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_DescriptorSets[i];
        writes[0].dstBinding = 0;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &depthInfo;

        // Binding 1: 输出存储图像
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_DescriptorSets[i];
        writes[1].dstBinding = 1;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &dstInfo;

        // Binding 2: HiZ mip图像
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = m_DescriptorSets[i];
        writes[2].dstBinding = 2;
        writes[2].dstArrayElement = 0;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].descriptorCount = 1;
        writes[2].pImageInfo = &hizInfo;

        vkUpdateDescriptorSets(m_Device, 3, writes, 0, nullptr);
    }

    return true;
}

void HiZComputeShader::TransitionDepthForRead(VkCommandBuffer commandBuffer, VkImage depthImage, uint32_t baseMipLevel) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = depthImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.baseMipLevel = baseMipLevel;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(commandBuffer,
                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void HiZComputeShader::TransitionMipLevelForWrite(VkCommandBuffer commandBuffer, uint32_t mipLevel) {
    if (mipLevel == 0 || mipLevel >= m_MipImages.size()) return;

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_MipImages[mipLevel];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

    vkCmdPipelineBarrier(commandBuffer,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void HiZComputeShader::TransitionMipLevelForRead(VkCommandBuffer commandBuffer, uint32_t mipLevel) {
    if (mipLevel == 0 || mipLevel >= m_MipImages.size()) return;

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_MipImages[mipLevel];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(commandBuffer,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
}
