#include "Rendering/VoxelTexture3DManager.h"
#include "EngineGlobal.h"
#include "VulkanManager.h"
#include <vulkan/vulkan_core.h>
#include <iostream>
#include <cstring>

// 启用 Vulkan 扩展支持存储纹理
#define VK_ENABLE_BETA_EXTENSIONS

// VoxelTexture3D 清理
void VoxelTexture3D::Cleanup(VkDevice device, VkAllocationCallbacks* allocator) {
    if (device == VK_NULL_HANDLE) {
        // 设备无效时只清空句柄
        imageView = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        imageMemory = VK_NULL_HANDLE;
        return;
    }
    
    if (imageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, imageView, allocator);
        imageView = VK_NULL_HANDLE;
    }
    if (image != VK_NULL_HANDLE) {
        vkDestroyImage(device, image, allocator);
        image = VK_NULL_HANDLE;
    }
    if (imageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, imageMemory, allocator);
        imageMemory = VK_NULL_HANDLE;
    }
}

// VoxelTexture3DManager 实现
VoxelTexture3DManager::VoxelTexture3DManager() {
}

VoxelTexture3DManager::~VoxelTexture3DManager() {
    Cleanup();
}

bool VoxelTexture3DManager::Initialize(const VoxelTexture3DManagerConfig& config) {
    if (m_Initialized) {
        return false;
    }
    
    m_Config = config;
    
    // 1. 创建描述符集布局
    if (!CreateDescriptorSetLayout()) {
        return false;
    }
    
    // 2. 创建描述符池
    if (!CreateDescriptorPool()) {
        return false;
    }
    
    // 3. 分配描述符集
    if (!AllocateDescriptorSet()) {
        return false;
    }
    
    // 4. 创建采样器
    if (!CreateSampler()) {
        return false;
    }
    
    // 5. 创建计算着色器管线布局
    CreateComputePipelineLayout();
    
    m_Initialized = true;
    
    return true;
}

void VoxelTexture3DManager::Cleanup() {
    if (!m_Initialized) {
        return;
    }
    
    // 检查设备是否有效
    extern VkDevice g_Device;
    extern VkAllocationCallbacks* g_Allocator;
    
    bool deviceValid = (g_Device != VK_NULL_HANDLE);
    
    // 清理所有 Texture3D
    for (size_t i = 0; i < m_Textures.size(); ++i) {
        auto& texture = m_Textures[i];
        if (deviceValid) {
            texture.Cleanup(g_Device, g_Allocator);
        } else {
            // 设备无效时只清空句柄
            texture.imageView = VK_NULL_HANDLE;
            texture.image = VK_NULL_HANDLE;
            texture.imageMemory = VK_NULL_HANDLE;
        }
    }
    m_Textures.clear();
    m_TextureNameToIndex.clear();
    
    // 清理管线布局
    if (m_PipelineLayout != VK_NULL_HANDLE && deviceValid) {
        vkDestroyPipelineLayout(g_Device, m_PipelineLayout, g_Allocator);
        m_PipelineLayout = VK_NULL_HANDLE;
    }
    
    // 清理描述符集
    if (m_DescriptorPool != VK_NULL_HANDLE && deviceValid) {
        vkDestroyDescriptorPool(g_Device, m_DescriptorPool, g_Allocator);
        m_DescriptorPool = VK_NULL_HANDLE;
    }
    
    // 清理描述符集布局
    if (m_DescriptorSetLayout != VK_NULL_HANDLE && deviceValid) {
        vkDestroyDescriptorSetLayout(g_Device, m_DescriptorSetLayout, g_Allocator);
        m_DescriptorSetLayout = VK_NULL_HANDLE;
    }
    
    // 清理采样器
    if (m_Sampler != VK_NULL_HANDLE && deviceValid) {
        vkDestroySampler(g_Device, m_Sampler, g_Allocator);
        m_Sampler = VK_NULL_HANDLE;
    }
    
    m_Initialized = false;
    std::cout << "[VoxelTexture3DManager] Cleanup completed!" << std::endl;
}

bool VoxelTexture3DManager::CreateDescriptorSetLayout() {
    // Binding 0: Texture3D 数组（存储纹理）
    VkDescriptorSetLayoutBinding textureBinding = {};
    textureBinding.binding = 0;
    textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    textureBinding.descriptorCount = m_Config.maxTextures;  // 数组大小
    textureBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    textureBinding.pImmutableSamplers = nullptr;
    
    // Binding 1: Sampler（所有纹理共享）
    VkDescriptorSetLayoutBinding samplerBinding = {};
    samplerBinding.binding = 1;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    
    std::vector<VkDescriptorSetLayoutBinding> bindings = {textureBinding, samplerBinding};
    
    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    
    VkResult result = vkCreateDescriptorSetLayout(g_Device, &layoutInfo, g_Allocator, &m_DescriptorSetLayout);
    if (result != VK_SUCCESS) {
        return false;
    }
    
    return true;
}

bool VoxelTexture3DManager::CreateDescriptorPool() {
    // 计算需要的描述符数量
    uint32_t textureCount = m_Config.maxTextures;
    
    VkDescriptorPoolSize poolSizes[2];
    
    // Storage Image 池
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = textureCount;
    
    // Sampler 池
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    poolSizes[1].descriptorCount = 1;
    
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = 1;  // 只需要一个描述符集
    
    VkResult result = vkCreateDescriptorPool(g_Device, &poolInfo, g_Allocator, &m_DescriptorPool);
    if (result != VK_SUCCESS) {
        return false;
    }
    
    return true;
}

bool VoxelTexture3DManager::AllocateDescriptorSet() {
    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_DescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_DescriptorSetLayout;
    
    VkResult result = vkAllocateDescriptorSets(g_Device, &allocInfo, &m_DescriptorSet);
    if (result != VK_SUCCESS) {
        return false;
    }
    
    return true;
}

bool VoxelTexture3DManager::CreateSampler() {
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = m_Config.filter;
    samplerInfo.minFilter = m_Config.filter;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    
    VkResult result = vkCreateSampler(g_Device, &samplerInfo, g_Allocator, &m_Sampler);
    if (result != VK_SUCCESS) {
        return false;
    }
    
    return true;
}

void VoxelTexture3DManager::CreateComputePipelineLayout() {
    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_DescriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 0;
    layoutInfo.pPushConstantRanges = nullptr;
    
    VkResult result = vkCreatePipelineLayout(g_Device, &layoutInfo, g_Allocator, &m_PipelineLayout);
    if (result != VK_SUCCESS) {
        return;
    }
}

uint32_t VoxelTexture3DManager::CreateTexture3D(
    const std::string& name,
    const std::vector<uint8_t>& voxelData,
    uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ,
    const std::vector<VkSamplerAddressMode>& addressModes)
{
    if (!m_Initialized) {
        return UINT32_MAX;
    }
    
    if (m_Textures.size() >= m_Config.maxTextures) {
        return UINT32_MAX;
    }
    
    // 检查是否已存在同名纹理
    auto it = m_TextureNameToIndex.find(name);
    if (it != m_TextureNameToIndex.end()) {
        DestroyTexture3D(it->second);
    }
    
    // 创建 Texture3D
    VoxelTexture3D texture;
    texture.name = name;
    texture.sizeX = sizeX;
    texture.sizeY = sizeY;
    texture.sizeZ = sizeZ;
    texture.voxelCount = static_cast<uint32_t>(voxelData.size());
    
    if (!CreateImage(name, voxelData, sizeX, sizeY, sizeZ, texture)) {
        return UINT32_MAX;
    }
    
    // 添加到管理器
    uint32_t index = static_cast<uint32_t>(m_Textures.size());
    m_Textures.push_back(std::move(texture));
    m_TextureNameToIndex[name] = index;
    
    // 更新描述符集
    UpdateDescriptorSet();
    
    return index;
}

void VoxelTexture3DManager::DestroyTexture3D(uint32_t textureIndex) {
    if (textureIndex >= m_Textures.size()) {
        std::cerr << "[VoxelTexture3DManager] Invalid texture index: " << textureIndex << std::endl;
        return;
    }
    
    std::cout << "[VoxelTexture3DManager] Destroying Texture3D at index " << textureIndex << std::endl;
    
    // 清理资源
    m_Textures[textureIndex].Cleanup(g_Device, g_Allocator);
    
    // 从映射中移除
    for (auto it = m_TextureNameToIndex.begin(); it != m_TextureNameToIndex.end(); ) {
        if (it->second == textureIndex) {
            it = m_TextureNameToIndex.erase(it);
        } else {
            ++it;
        }
    }
    
    // 从数组中移除（这里简单处理，实际可能需要更复杂的索引管理）
    m_Textures[textureIndex] = VoxelTexture3D();
    
    // 更新描述符集
    UpdateDescriptorSet();
}

int32_t VoxelTexture3DManager::GetTextureIndex(const std::string& name) const {
    auto it = m_TextureNameToIndex.find(name);
    if (it != m_TextureNameToIndex.end()) {
        return static_cast<int32_t>(it->second);
    }
    return -1;
}

const VoxelTexture3D* VoxelTexture3DManager::GetTexture3D(uint32_t index) const {
    if (index >= m_Textures.size()) {
        return nullptr;
    }
    return &m_Textures[index];
}

const VoxelTexture3D* VoxelTexture3DManager::GetTexture3D(const std::string& name) const {
    int32_t index = GetTextureIndex(name);
    if (index < 0) {
        return nullptr;
    }
    return GetTexture3D(static_cast<uint32_t>(index));
}

void VoxelTexture3DManager::UpdateDescriptorSet() {
    if (m_DescriptorSet == VK_NULL_HANDLE) {
        return;
    }
    
    // 准备 Texture3D 数组的描述符
    std::vector<VkDescriptorImageInfo> textureInfos(m_Textures.size());
    for (size_t i = 0; i < m_Textures.size(); i++) {
        textureInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        textureInfos[i].imageView = m_Textures[i].imageView;
        textureInfos[i].sampler = VK_NULL_HANDLE;  // Sampler 在 binding 1
    }
    
    // 更新 Binding 0 (Texture3D 数组)
    VkWriteDescriptorSet writeDescriptorSets[2] = {};
    
    writeDescriptorSets[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeDescriptorSets[0].dstSet = m_DescriptorSet;
    writeDescriptorSets[0].dstBinding = 0;
    writeDescriptorSets[0].dstArrayElement = 0;
    writeDescriptorSets[0].descriptorCount = static_cast<uint32_t>(m_Textures.size());
    writeDescriptorSets[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writeDescriptorSets[0].pImageInfo = textureInfos.data();
    
    // 更新 Binding 1 (Sampler)
    VkDescriptorImageInfo samplerInfo = {};
    samplerInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    samplerInfo.imageView = VK_NULL_HANDLE;
    samplerInfo.sampler = m_Sampler;
    
    writeDescriptorSets[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeDescriptorSets[1].dstSet = m_DescriptorSet;
    writeDescriptorSets[1].dstBinding = 1;
    writeDescriptorSets[1].dstArrayElement = 0;
    writeDescriptorSets[1].descriptorCount = 1;
    writeDescriptorSets[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writeDescriptorSets[1].pImageInfo = &samplerInfo;
    
    vkUpdateDescriptorSets(g_Device, 2, writeDescriptorSets, 0, nullptr);
}

bool VoxelTexture3DManager::CreateImage(const std::string& name,
                                       const std::vector<uint8_t>& voxelData,
                                       uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ,
                                       VoxelTexture3D& outTexture) {
    // 1. 创建 VkImage
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_3D;
    imageInfo.format = m_Config.format;
    imageInfo.extent = {sizeX, sizeY, sizeZ};
    imageInfo.mipLevels = m_Config.enableMipmaps ? static_cast<uint32_t>(std::floor(std::log2(std::max({sizeX, sizeY, sizeZ}))) + 1) : 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (m_Config.enableMipmaps) {
        imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VkResult result = vkCreateImage(g_Device, &imageInfo, g_Allocator, &outTexture.image);
    if (result != VK_SUCCESS) {
        std::cerr << "[VoxelTexture3DManager] Failed to create image: " << result << std::endl;
        return false;
    }
    
    // 2. 分配内存
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(g_Device, outTexture.image, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    
    // 查找合适的内存类型
    uint32_t memoryTypeIndex = 0;
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &memProperties);
    
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memRequirements.memoryTypeBits & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memoryTypeIndex = i;
            break;
        }
    }
    
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    
    result = vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &outTexture.imageMemory);
    if (result != VK_SUCCESS) {
        std::cerr << "[VoxelTexture3DManager] Failed to allocate image memory: " << result << std::endl;
        return false;
    }
    
    vkBindImageMemory(g_Device, outTexture.image, outTexture.imageMemory, 0);
    
    // 3. 创建 ImageView
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outTexture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format = m_Config.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = imageInfo.mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    
    result = vkCreateImageView(g_Device, &viewInfo, g_Allocator, &outTexture.imageView);
    if (result != VK_SUCCESS) {
        std::cerr << "[VoxelTexture3DManager] Failed to create image view: " << result << std::endl;
        return false;
    }
    
    // 4. 上传体素数据
    if (!UploadVoxelData(outTexture, voxelData)) {
        std::cerr << "[VoxelTexture3DManager] Failed to upload voxel data!" << std::endl;
        return false;
    }
    
    return true;
}

bool VoxelTexture3DManager::UploadVoxelData(VoxelTexture3D& texture, const std::vector<uint8_t>& voxelData) {
    // 创建暂存缓冲区
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = voxelData.size();
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    
    VkResult result = vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &stagingBuffer);
    if (result != VK_SUCCESS) {
        std::cerr << "[VoxelTexture3DManager] Failed to create staging buffer: " << result << std::endl;
        return false;
    }
    
    // 分配暂存缓冲区内存
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(g_Device, stagingBuffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    
    // 查找主机可见内存
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &memProperties);
    
    uint32_t memoryTypeIndex = 0;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memRequirements.memoryTypeBits & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memoryTypeIndex = i;
            break;
        }
    }
    
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    
    result = vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &stagingBufferMemory);
    if (result != VK_SUCCESS) {
        std::cerr << "[VoxelTexture3DManager] Failed to allocate staging buffer memory: " << result << std::endl;
        vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
        return false;
    }
    
    vkBindBufferMemory(g_Device, stagingBuffer, stagingBufferMemory, 0);
    
    // 映射并复制数据
    void* data;
    vkMapMemory(g_Device, stagingBufferMemory, 0, voxelData.size(), 0, &data);
    memcpy(data, voxelData.data(), voxelData.size());
    vkUnmapMemory(g_Device, stagingBufferMemory);
    
    // 创建命令缓冲区
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = g_QueueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    
    VkCommandPool commandPool;
    vkCreateCommandPool(g_Device, &poolInfo, g_Allocator, &commandPool);
    
    VkCommandBufferAllocateInfo cmdAllocInfo = {};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = commandPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(g_Device, &cmdAllocInfo, &cmd);
    
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);
    
    // 转换图像布局到 TRANSFER_DST_OPTIMAL
    TransitionImageLayout(texture.image, 
                         VK_IMAGE_LAYOUT_UNDEFINED, 
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 
                         1, cmd);
    
    // 复制缓冲区到图像
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {texture.sizeX, texture.sizeY, texture.sizeZ};
    
    vkCmdCopyBufferToImage(cmd, stagingBuffer, texture.image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    
    // 转换图像布局到 GENERAL（用于计算着色器访问）
    TransitionImageLayout(texture.image, 
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 
                         VK_IMAGE_LAYOUT_GENERAL, 
                         1, cmd);
    
    vkEndCommandBuffer(cmd);
    
    // 提交命令
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    vkCreateFence(g_Device, &fenceInfo, g_Allocator, &fence);
    
    vkQueueSubmit(g_Queue, 1, &submitInfo, fence);
    vkWaitForFences(g_Device, 1, &fence, VK_TRUE, UINT64_MAX);
    
    // 清理
    vkDestroyFence(g_Device, fence, g_Allocator);
    vkFreeCommandBuffers(g_Device, commandPool, 1, &cmd);
    vkDestroyCommandPool(g_Device, commandPool, g_Allocator);
    vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
    vkFreeMemory(g_Device, stagingBufferMemory, g_Allocator);
    
    std::cout << "[VoxelTexture3DManager] Voxel data uploaded to GPU" << std::endl;
    return true;
}

void VoxelTexture3DManager::TransitionImageLayout(VkImage image, 
                                                 VkImageLayout oldLayout, 
                                                 VkImageLayout newLayout,
                                                 uint32_t mipLevels,
                                                 VkCommandBuffer cmd) {
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    
    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;
    
    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else {
        std::cerr << "[VoxelTexture3DManager] Unsupported layout transition!" << std::endl;
        return;
    }
    
    vkCmdPipelineBarrier(
        cmd,
        sourceStage, destinationStage,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );
}
