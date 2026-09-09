#include "PreviewGenerator.h"
#include "PreviewModelRenderer.h"
#include "PreviewVoxRenderer.h"
#include "EngineGlobal.h"
#include "EngineConfig.h"
#include "ModelRenderer.h"
#include "TexturePool.h"
#include "EditorManager.h"
#include "ModelLoader.h"
#include "VoxLoader.h"
#include "ECS/Coordinator.h"
#include "ECS/Components.h"
#include "ECS/SceneECS.h"
#include "Core/ProjectManager.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <cstring>
#include <filesystem>
#include <SDL3/SDL_iostream.h>
#include <SDL3_image/SDL_image.h>

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

PreviewGenerator& PreviewGenerator::GetInstance() {
    static PreviewGenerator instance;
    return instance;
}

void PreviewGenerator::Init() {
    if (m_Initialized) return;
    
    m_TexturePool = EditorManager::GetInstance().GetTexturePool();
    if (m_TexturePool == nullptr) {
        // 纹理池不可用:不标记已初始化,调用方检测后跳过,避免使用未初始化的渲染器
        fprintf(stderr, "[Preview] TexturePool unavailable, preview generation disabled\n");
        return;
    }
    
    CreateRenderTarget();
    if (m_RenderPass == VK_NULL_HANDLE) {
        fprintf(stderr, "[Preview] RenderTarget creation failed, preview generation disabled\n");
        return;
    }
    
    m_PreviewRenderer = std::make_unique<PreviewModelRenderer>();
    m_PreviewRenderer->Init(m_RenderPass);
    
    m_Initialized = true;
}

void PreviewGenerator::Cleanup() {
    if (!m_Initialized) return;
    
    if (m_PreviewRenderer) {
        m_PreviewRenderer->Cleanup();
        m_PreviewRenderer.reset();
    }
    
    if (m_VoxRenderer) {
        m_VoxRenderer->Cleanup();
        m_VoxRenderer.reset();
    }
    
    DestroyRenderTarget();
    
    m_Initialized = false;
}

void PreviewGenerator::CreateRenderTarget() {
    VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    
    VkAttachmentDescription attachments[2] = {};
    
    attachments[0].format = colorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    
    attachments[1].format = depthFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    
    VkAttachmentReference colorRef = {};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    
    VkAttachmentReference depthRef = {};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;
    
    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 2;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    
    VkResult err = vkCreateRenderPass(g_Device, &renderPassInfo, g_Allocator, &m_RenderPass);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to create render pass\n");
        return;
    }
    
    VkImageCreateInfo colorImageInfo = {};
    colorImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    colorImageInfo.imageType = VK_IMAGE_TYPE_2D;
    colorImageInfo.format = colorFormat;
    colorImageInfo.extent.width = PREVIEW_SIZE;
    colorImageInfo.extent.height = PREVIEW_SIZE;
    colorImageInfo.extent.depth = 1;
    colorImageInfo.mipLevels = 1;
    colorImageInfo.arrayLayers = 1;
    colorImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    colorImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    colorImageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    colorImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    colorImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    err = vkCreateImage(g_Device, &colorImageInfo, g_Allocator, &m_ColorImage);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to create color image\n");
        return;
    }
    
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(g_Device, m_ColorImage, &memReqs);
    
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    err = vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_ColorImageMemory);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to allocate color image memory\n");
        return;
    }
    
    vkBindImageMemory(g_Device, m_ColorImage, m_ColorImageMemory, 0);
    
    VkImageViewCreateInfo colorViewInfo = {};
    colorViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    colorViewInfo.image = m_ColorImage;
    colorViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    colorViewInfo.format = colorFormat;
    colorViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    colorViewInfo.subresourceRange.baseMipLevel = 0;
    colorViewInfo.subresourceRange.levelCount = 1;
    colorViewInfo.subresourceRange.baseArrayLayer = 0;
    colorViewInfo.subresourceRange.layerCount = 1;
    
    err = vkCreateImageView(g_Device, &colorViewInfo, g_Allocator, &m_ColorImageView);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to create color image view\n");
        return;
    }
    
    VkImageCreateInfo depthImageInfo = {};
    depthImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthImageInfo.imageType = VK_IMAGE_TYPE_2D;
    depthImageInfo.format = depthFormat;
    depthImageInfo.extent.width = PREVIEW_SIZE;
    depthImageInfo.extent.height = PREVIEW_SIZE;
    depthImageInfo.extent.depth = 1;
    depthImageInfo.mipLevels = 1;
    depthImageInfo.arrayLayers = 1;
    depthImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    depthImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthImageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    depthImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    err = vkCreateImage(g_Device, &depthImageInfo, g_Allocator, &m_DepthImage);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to create depth image\n");
        return;
    }
    
    vkGetImageMemoryRequirements(g_Device, m_DepthImage, &memReqs);
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    err = vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_DepthImageMemory);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to allocate depth image memory\n");
        return;
    }
    
    vkBindImageMemory(g_Device, m_DepthImage, m_DepthImageMemory, 0);
    
    VkImageViewCreateInfo depthViewInfo = {};
    depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthViewInfo.image = m_DepthImage;
    depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthViewInfo.format = depthFormat;
    depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthViewInfo.subresourceRange.baseMipLevel = 0;
    depthViewInfo.subresourceRange.levelCount = 1;
    depthViewInfo.subresourceRange.baseArrayLayer = 0;
    depthViewInfo.subresourceRange.layerCount = 1;
    
    err = vkCreateImageView(g_Device, &depthViewInfo, g_Allocator, &m_DepthImageView);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to create depth image view\n");
        return;
    }
    
    VkImageView framebufferAttachments[2] = { m_ColorImageView, m_DepthImageView };
    
    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_RenderPass;
    framebufferInfo.attachmentCount = 2;
    framebufferInfo.pAttachments = framebufferAttachments;
    framebufferInfo.width = PREVIEW_SIZE;
    framebufferInfo.height = PREVIEW_SIZE;
    framebufferInfo.layers = 1;
    
    err = vkCreateFramebuffer(g_Device, &framebufferInfo, g_Allocator, &m_Framebuffer);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to create framebuffer\n");
        return;
    }
}

void PreviewGenerator::DestroyRenderTarget() {
    if (m_Framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(g_Device, m_Framebuffer, g_Allocator);
        m_Framebuffer = VK_NULL_HANDLE;
    }
    if (m_DepthImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(g_Device, m_DepthImageView, g_Allocator);
        m_DepthImageView = VK_NULL_HANDLE;
    }
    if (m_DepthImage != VK_NULL_HANDLE) {
        vkDestroyImage(g_Device, m_DepthImage, g_Allocator);
        m_DepthImage = VK_NULL_HANDLE;
    }
    if (m_DepthImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_DepthImageMemory, g_Allocator);
        m_DepthImageMemory = VK_NULL_HANDLE;
    }
    if (m_ColorImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(g_Device, m_ColorImageView, g_Allocator);
        m_ColorImageView = VK_NULL_HANDLE;
    }
    if (m_ColorImage != VK_NULL_HANDLE) {
        vkDestroyImage(g_Device, m_ColorImage, g_Allocator);
        m_ColorImage = VK_NULL_HANDLE;
    }
    if (m_ColorImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_ColorImageMemory, g_Allocator);
        m_ColorImageMemory = VK_NULL_HANDLE;
    }
    if (m_RenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(g_Device, m_RenderPass, g_Allocator);
        m_RenderPass = VK_NULL_HANDLE;
    }
}

bool PreviewGenerator::GeneratePreview(const std::string& modelPath, const std::string& outputPath) {
#ifdef __ANDROID__
    // 安卓平台不支持文件系统写入，跳过预览生成
    printf("Preview generation skipped on Android\n");
    return false;
#endif

    if (!m_Initialized) {
        Init();
        if (!m_Initialized) {
            return false; // 预览初始化失败(纹理池/渲染目标不可用),跳过本次生成
        }
    }
    
    vkDeviceWaitIdle(g_Device);
    
    MeshData meshData = ModelLoader::LoadModel(modelPath);
    if (meshData.subMeshes.empty()) {
        fprintf(stderr, "PreviewGenerator: Failed to load model: %s\n", modelPath.c_str());
        return false;
    }
    
    glm::vec3 minBounds(FLT_MAX, FLT_MAX, FLT_MAX);
    glm::vec3 maxBounds(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    
    for (const auto& subMesh : meshData.subMeshes) {
        for (const auto& vertex : subMesh.vertices) {
            minBounds = glm::min(minBounds, vertex.Position);
            maxBounds = glm::max(maxBounds, vertex.Position);
        }
    }
    
    glm::vec3 center = (minBounds + maxBounds) * 0.5f;
    glm::vec3 size = maxBounds - minBounds;
    float maxDim = glm::max(size.x, glm::max(size.y, size.z));
    float cameraDistance = maxDim * 2.0f;
    
    glm::vec3 cameraPos = center + glm::vec3(cameraDistance * 0.707f, maxDim * 0.5f, cameraDistance * 0.707f);
    glm::vec3 cameraTarget = center;
    
    glm::mat4 view = glm::lookAt(cameraPos, cameraTarget, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, cameraDistance * 10.0f);
    proj[1][1] *= -1;
    
    m_PreviewRenderer->Cleanup();
    m_PreviewRenderer->Init(m_RenderPass);
    m_PreviewRenderer->LoadModel(modelPath);
    
    if (!m_PreviewRenderer->HasModelLoaded()) {
        fprintf(stderr, "PreviewGenerator: Failed to load model: %s\n", modelPath.c_str());
        return false;
    }
    
    bool hasAlbedoTexture = m_PreviewRenderer->HasAlbedoTexture();
    
    std::vector<ModelInstanceData> instanceData(1);
    instanceData[0].model = glm::mat4(1.0f);
    instanceData[0].albedoColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
    instanceData[0].materialData = glm::vec4(0.0f, 0.75f, 1.0f, hasAlbedoTexture ? 1.0f : 0.0f);
    instanceData[0].textureFlags = glm::vec4(m_PreviewRenderer->HasNormalTexture() ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = g_CommandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    
    VkCommandBuffer commandBuffer;
    VkResult err = vkAllocateCommandBuffers(g_Device, &allocInfo, &commandBuffer);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to allocate command buffer\n");
        return false;
    }
    
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    
    VkRenderPassBeginInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_RenderPass;
    renderPassInfo.framebuffer = m_Framebuffer;
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = { PREVIEW_SIZE, PREVIEW_SIZE };
    
    VkClearValue clearValues[2] = {};
    clearValues[0].color = { { 0.1f, 0.1f, 0.12f, 1.0f } };
    clearValues[1].depthStencil = { 1.0f, 0 };
    
    renderPassInfo.clearValueCount = 2;
    renderPassInfo.pClearValues = clearValues;
    
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    
    m_PreviewRenderer->RenderInstanced(commandBuffer, PREVIEW_SIZE, PREVIEW_SIZE, view, proj, instanceData, nullptr);
    
    vkCmdEndRenderPass(commandBuffer);
    
    vkEndCommandBuffer(commandBuffer);
    
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    
    err = vkQueueSubmit(g_Queue, 1, &submitInfo, VK_NULL_HANDLE);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to submit command buffer: %d\n", err);
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        return false;
    }
    
    vkQueueWaitIdle(g_Queue);
    
    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
    
    VkImageCreateInfo dstImageInfo = {};
    dstImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    dstImageInfo.imageType = VK_IMAGE_TYPE_2D;
    dstImageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    dstImageInfo.extent.width = PREVIEW_SIZE;
    dstImageInfo.extent.height = PREVIEW_SIZE;
    dstImageInfo.extent.depth = 1;
    dstImageInfo.mipLevels = 1;
    dstImageInfo.arrayLayers = 1;
    dstImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    dstImageInfo.tiling = VK_IMAGE_TILING_LINEAR;
    dstImageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    dstImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    dstImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    VkImage dstImage;
    VkDeviceMemory dstImageMemory;
    
    err = vkCreateImage(g_Device, &dstImageInfo, g_Allocator, &dstImage);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to create destination image\n");
        return false;
    }
    
    VkMemoryRequirements dstMemReqs;
    vkGetImageMemoryRequirements(g_Device, dstImage, &dstMemReqs);
    
    VkMemoryAllocateInfo dstAllocInfo = {};
    dstAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    dstAllocInfo.allocationSize = dstMemReqs.size;
    dstAllocInfo.memoryTypeIndex = FindMemoryType(dstMemReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    err = vkAllocateMemory(g_Device, &dstAllocInfo, g_Allocator, &dstImageMemory);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to allocate destination image memory\n");
        vkDestroyImage(g_Device, dstImage, g_Allocator);
        return false;
    }
    
    vkBindImageMemory(g_Device, dstImage, dstImageMemory, 0);
    
    allocInfo.commandBufferCount = 1;
    err = vkAllocateCommandBuffers(g_Device, &allocInfo, &commandBuffer);
    if (err != VK_SUCCESS) {
        vkDestroyImage(g_Device, dstImage, g_Allocator);
        vkFreeMemory(g_Device, dstImageMemory, g_Allocator);
        return false;
    }
    
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    
    VkImageMemoryBarrier srcBarrier = {};
    srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    srcBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.image = m_ColorImage;
    srcBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    srcBarrier.subresourceRange.baseMipLevel = 0;
    srcBarrier.subresourceRange.levelCount = 1;
    srcBarrier.subresourceRange.baseArrayLayer = 0;
    srcBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &srcBarrier);
    
    VkImageMemoryBarrier dstBarrier = {};
    dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dstBarrier.srcAccessMask = 0;
    dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.image = dstImage;
    dstBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dstBarrier.subresourceRange.baseMipLevel = 0;
    dstBarrier.subresourceRange.levelCount = 1;
    dstBarrier.subresourceRange.baseArrayLayer = 0;
    dstBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &dstBarrier);
    
    VkImageCopy copyRegion = {};
    copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.srcSubresource.mipLevel = 0;
    copyRegion.srcSubresource.baseArrayLayer = 0;
    copyRegion.srcSubresource.layerCount = 1;
    copyRegion.srcOffset = { 0, 0, 0 };
    copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.dstSubresource.mipLevel = 0;
    copyRegion.dstSubresource.baseArrayLayer = 0;
    copyRegion.dstSubresource.layerCount = 1;
    copyRegion.dstOffset = { 0, 0, 0 };
    copyRegion.extent.width = PREVIEW_SIZE;
    copyRegion.extent.height = PREVIEW_SIZE;
    copyRegion.extent.depth = 1;
    
    vkCmdCopyImage(commandBuffer, m_ColorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
    
    VkImageMemoryBarrier readBarrier = {};
    readBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    readBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    readBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    readBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    readBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    readBarrier.image = dstImage;
    readBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    readBarrier.subresourceRange.baseMipLevel = 0;
    readBarrier.subresourceRange.levelCount = 1;
    readBarrier.subresourceRange.baseArrayLayer = 0;
    readBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 0, nullptr, 1, &readBarrier);
    
    vkEndCommandBuffer(commandBuffer);
    
    submitInfo.pCommandBuffers = &commandBuffer;
    err = vkQueueSubmit(g_Queue, 1, &submitInfo, VK_NULL_HANDLE);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to submit copy command buffer: %d\n", err);
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        vkDestroyImage(g_Device, dstImage, g_Allocator);
        vkFreeMemory(g_Device, dstImageMemory, g_Allocator);
        return false;
    }
    
    vkQueueWaitIdle(g_Queue);
    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
    
    void* data;
    vkMapMemory(g_Device, dstImageMemory, 0, dstMemReqs.size, 0, &data);
    
    SDL_Surface* surface = SDL_CreateSurface(PREVIEW_SIZE, PREVIEW_SIZE, SDL_PIXELFORMAT_ABGR8888);
    if (surface) {
        uint8_t* pixels = (uint8_t*)surface->pixels;
        uint8_t* srcData = (uint8_t*)data;
        
        VkImageSubresource subresource = {};
        subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        VkSubresourceLayout layout;
        vkGetImageSubresourceLayout(g_Device, dstImage, &subresource, &layout);
        
        for (uint32_t y = 0; y < PREVIEW_SIZE; y++) {
            uint32_t* srcRow = (uint32_t*)(srcData + layout.rowPitch * y);
            uint32_t* dstRow = (uint32_t*)(pixels + surface->pitch * y);
            memcpy(dstRow, srcRow, PREVIEW_SIZE * 4);
        }
        
        std::string outputPathPng = outputPath;
        if (outputPathPng.size() > 4 && outputPathPng.substr(outputPathPng.size() - 4) == ".png") {
            IMG_SavePNG(surface, outputPathPng.c_str());
        } else {
            SDL_SaveBMP(surface, outputPath.c_str());
        }
        SDL_DestroySurface(surface);
    }
    
    vkUnmapMemory(g_Device, dstImageMemory);
    vkDestroyImage(g_Device, dstImage, g_Allocator);
    vkFreeMemory(g_Device, dstImageMemory, g_Allocator);
    
    vkDeviceWaitIdle(g_Device);
    
    printf("Preview saved successfully: %s\n", outputPath.c_str());
    return true;
}

bool PreviewGenerator::GenerateMaterialPreview(const ECS::MaterialComponent& material, const std::string& outputPath) {
#ifdef __ANDROID__
    // 安卓平台不支持文件系统写入，跳过预览生成
    printf("Material preview generation skipped on Android\n");
    return false;
#endif
    
    if (!m_Initialized) {
        Init();
        if (!m_Initialized) {
            return false; // 预览初始化失败,跳过本次生成
        }
    }
    
    vkDeviceWaitIdle(g_Device);
    
    // 确保.meta目录存在
    std::filesystem::path previewPath(outputPath);
    std::filesystem::path metaDir = previewPath.parent_path();
    if (!std::filesystem::exists(metaDir)) {
        std::filesystem::create_directories(metaDir);
    }
    
    // 基础预览模型属于引擎内置资源，不跟随当前项目资产根变化。
    const std::string spherePath =
        ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/sphere.glb");
    
    m_PreviewRenderer->Cleanup();
    m_PreviewRenderer->Init(m_RenderPass);
    m_PreviewRenderer->LoadModel(spherePath);
    
    if (!m_PreviewRenderer->HasModelLoaded()) {
        fprintf(stderr, "PreviewGenerator: Failed to load sphere model\n");
        return false;
    }
    
    // 加载材质纹理并更新描述符集
    auto& subMeshes = m_PreviewRenderer->GetSubMeshes();
    for (auto& subMesh : subMeshes) {
        // 加载 albedo 纹理
        if (material.useAlbedoTexture && !material.albedoPath.empty()) {
            SamplerType samplerType = SamplerType::Linear;
            switch (material.albedoSamplerType) {
            case 0: samplerType = SamplerType::Linear; break;
            case 1: samplerType = SamplerType::Nearest; break;
            case 2: samplerType = SamplerType::LinearClamp; break;
            case 3: samplerType = SamplerType::NearestClamp; break;
            }
            m_TexturePool->LoadTexture2D(material.albedoPath, material.albedoPath, samplerType);
            const TextureInfo* tex = m_TexturePool->GetTexture(material.albedoPath);
            if (tex && tex->imageView != VK_NULL_HANDLE) {
                VkDescriptorImageInfo imageInfo = {};
                imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imageInfo.imageView = tex->imageView;
                imageInfo.sampler = m_TexturePool->GetSampler(material.albedoPath);
                
                VkWriteDescriptorSet descriptorWrite = {};
                descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorWrite.dstSet = subMesh.descriptorSet;
                descriptorWrite.dstBinding = 0;
                descriptorWrite.dstArrayElement = 0;
                descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                descriptorWrite.descriptorCount = 1;
                descriptorWrite.pImageInfo = &imageInfo;
                
                vkUpdateDescriptorSets(g_Device, 1, &descriptorWrite, 0, nullptr);
            }
        }
        
        // 加载 normal 纹理
        if (material.useNormalTexture && !material.normalPath.empty()) {
            SamplerType samplerType = SamplerType::Linear;
            switch (material.normalSamplerType) {
            case 0: samplerType = SamplerType::Linear; break;
            case 1: samplerType = SamplerType::Nearest; break;
            case 2: samplerType = SamplerType::LinearClamp; break;
            case 3: samplerType = SamplerType::NearestClamp; break;
            }
            m_TexturePool->LoadTexture2D(material.normalPath, material.normalPath, samplerType);
            const TextureInfo* tex = m_TexturePool->GetTexture(material.normalPath);
            if (tex && tex->imageView != VK_NULL_HANDLE) {
                VkDescriptorImageInfo imageInfo = {};
                imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imageInfo.imageView = tex->imageView;
                imageInfo.sampler = m_TexturePool->GetSampler(material.normalPath);
                
                VkWriteDescriptorSet descriptorWrite = {};
                descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorWrite.dstSet = subMesh.descriptorSet;
                descriptorWrite.dstBinding = 1;
                descriptorWrite.dstArrayElement = 0;
                descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                descriptorWrite.descriptorCount = 1;
                descriptorWrite.pImageInfo = &imageInfo;
                
                vkUpdateDescriptorSets(g_Device, 1, &descriptorWrite, 0, nullptr);
            }
        }
        
        // 加载 roughness 纹理
        if (material.useRoughnessTexture && !material.roughnessPath.empty()) {
            SamplerType samplerType = SamplerType::Linear;
            switch (material.roughnessSamplerType) {
            case 0: samplerType = SamplerType::Linear; break;
            case 1: samplerType = SamplerType::Nearest; break;
            case 2: samplerType = SamplerType::LinearClamp; break;
            case 3: samplerType = SamplerType::NearestClamp; break;
            }
            m_TexturePool->LoadTexture2D(material.roughnessPath, material.roughnessPath, samplerType);
            const TextureInfo* tex = m_TexturePool->GetTexture(material.roughnessPath);
            if (tex && tex->imageView != VK_NULL_HANDLE) {
                VkDescriptorImageInfo imageInfo = {};
                imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imageInfo.imageView = tex->imageView;
                imageInfo.sampler = m_TexturePool->GetSampler(material.roughnessPath);
                
                VkWriteDescriptorSet descriptorWrite = {};
                descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorWrite.dstSet = subMesh.descriptorSet;
                descriptorWrite.dstBinding = 2;
                descriptorWrite.dstArrayElement = 0;
                descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                descriptorWrite.descriptorCount = 1;
                descriptorWrite.pImageInfo = &imageInfo;
                
                vkUpdateDescriptorSets(g_Device, 1, &descriptorWrite, 0, nullptr);
            }
        }
        
        // 加载 metallic 纹理
        if (material.useMetallicTexture && !material.metallicPath.empty()) {
            SamplerType samplerType = SamplerType::Linear;
            switch (material.metallicSamplerType) {
            case 0: samplerType = SamplerType::Linear; break;
            case 1: samplerType = SamplerType::Nearest; break;
            case 2: samplerType = SamplerType::LinearClamp; break;
            case 3: samplerType = SamplerType::NearestClamp; break;
            }
            m_TexturePool->LoadTexture2D(material.metallicPath, material.metallicPath, samplerType);
            const TextureInfo* tex = m_TexturePool->GetTexture(material.metallicPath);
            if (tex && tex->imageView != VK_NULL_HANDLE) {
                VkDescriptorImageInfo imageInfo = {};
                imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imageInfo.imageView = tex->imageView;
                imageInfo.sampler = m_TexturePool->GetSampler(material.metallicPath);
                
                VkWriteDescriptorSet descriptorWrite = {};
                descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorWrite.dstSet = subMesh.descriptorSet;
                descriptorWrite.dstBinding = 3;
                descriptorWrite.dstArrayElement = 0;
                descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                descriptorWrite.descriptorCount = 1;
                descriptorWrite.pImageInfo = &imageInfo;
                
                vkUpdateDescriptorSets(g_Device, 1, &descriptorWrite, 0, nullptr);
            }
        }
    }
    
    glm::vec3 cameraPos = glm::vec3(2.0f, 1.5f, 2.0f);
    glm::vec3 cameraTarget = glm::vec3(0.0f, 0.0f, 0.0f);
    
    glm::mat4 view = glm::lookAt(cameraPos, cameraTarget, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, 10.0f);
    proj[1][1] *= -1;
    
    std::vector<ModelInstanceData> instanceData(1);
    instanceData[0].model = glm::mat4(1.0f);
    instanceData[0].albedoColor = glm::vec4(material.albedoColor, 1.0f);
    instanceData[0].materialData = glm::vec4(material.metallic, material.roughness, material.ao, material.useAlbedoTexture ? 1.0f : 0.0f);
    instanceData[0].textureFlags = glm::vec4(
        material.useNormalTexture ? 1.0f : 0.0f,
        material.useRoughnessTexture ? 1.0f : 0.0f,
        material.useMetallicTexture ? 1.0f : 0.0f,
        0.0f
    );
    
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = g_CommandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    
    VkCommandBuffer commandBuffer;
    VkResult err = vkAllocateCommandBuffers(g_Device, &allocInfo, &commandBuffer);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to allocate command buffer\n");
        return false;
    }
    
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    
    VkRenderPassBeginInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_RenderPass;
    renderPassInfo.framebuffer = m_Framebuffer;
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = { PREVIEW_SIZE, PREVIEW_SIZE };
    
    VkClearValue clearValues[2] = {};
    clearValues[0].color = { { 0.1f, 0.1f, 0.12f, 1.0f } };
    clearValues[1].depthStencil = { 1.0f, 0 };
    
    renderPassInfo.clearValueCount = 2;
    renderPassInfo.pClearValues = clearValues;
    
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    
    m_PreviewRenderer->RenderInstanced(commandBuffer, PREVIEW_SIZE, PREVIEW_SIZE, view, proj, instanceData, &material);
    
    vkCmdEndRenderPass(commandBuffer);
    
    vkEndCommandBuffer(commandBuffer);
    
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    
    err = vkQueueSubmit(g_Queue, 1, &submitInfo, VK_NULL_HANDLE);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to submit command buffer: %d\n", err);
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        return false;
    }
    
    vkQueueWaitIdle(g_Queue);
    
    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
    
    VkImageCreateInfo dstImageInfo = {};
    dstImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    dstImageInfo.imageType = VK_IMAGE_TYPE_2D;
    dstImageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    dstImageInfo.extent.width = PREVIEW_SIZE;
    dstImageInfo.extent.height = PREVIEW_SIZE;
    dstImageInfo.extent.depth = 1;
    dstImageInfo.mipLevels = 1;
    dstImageInfo.arrayLayers = 1;
    dstImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    dstImageInfo.tiling = VK_IMAGE_TILING_LINEAR;
    dstImageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    dstImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    dstImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    VkImage dstImage;
    VkDeviceMemory dstImageMemory;
    
    err = vkCreateImage(g_Device, &dstImageInfo, g_Allocator, &dstImage);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to create destination image\n");
        return false;
    }
    
    VkMemoryRequirements dstMemReqs;
    vkGetImageMemoryRequirements(g_Device, dstImage, &dstMemReqs);
    
    VkMemoryAllocateInfo dstAllocInfo = {};
    dstAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    dstAllocInfo.allocationSize = dstMemReqs.size;
    dstAllocInfo.memoryTypeIndex = FindMemoryType(dstMemReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    err = vkAllocateMemory(g_Device, &dstAllocInfo, g_Allocator, &dstImageMemory);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to allocate destination image memory\n");
        vkDestroyImage(g_Device, dstImage, g_Allocator);
        return false;
    }
    
    vkBindImageMemory(g_Device, dstImage, dstImageMemory, 0);
    
    allocInfo.commandBufferCount = 1;
    err = vkAllocateCommandBuffers(g_Device, &allocInfo, &commandBuffer);
    if (err != VK_SUCCESS) {
        vkDestroyImage(g_Device, dstImage, g_Allocator);
        vkFreeMemory(g_Device, dstImageMemory, g_Allocator);
        return false;
    }
    
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    
    VkImageMemoryBarrier srcBarrier = {};
    srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    srcBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.image = m_ColorImage;
    srcBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    srcBarrier.subresourceRange.baseMipLevel = 0;
    srcBarrier.subresourceRange.levelCount = 1;
    srcBarrier.subresourceRange.baseArrayLayer = 0;
    srcBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &srcBarrier);
    
    VkImageMemoryBarrier dstBarrier = {};
    dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dstBarrier.srcAccessMask = 0;
    dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.image = dstImage;
    dstBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dstBarrier.subresourceRange.baseMipLevel = 0;
    dstBarrier.subresourceRange.levelCount = 1;
    dstBarrier.subresourceRange.baseArrayLayer = 0;
    dstBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &dstBarrier);
    
    VkImageCopy copyRegion = {};
    copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.srcSubresource.mipLevel = 0;
    copyRegion.srcSubresource.baseArrayLayer = 0;
    copyRegion.srcSubresource.layerCount = 1;
    copyRegion.srcOffset = { 0, 0, 0 };
    copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.dstSubresource.mipLevel = 0;
    copyRegion.dstSubresource.baseArrayLayer = 0;
    copyRegion.dstSubresource.layerCount = 1;
    copyRegion.dstOffset = { 0, 0, 0 };
    copyRegion.extent.width = PREVIEW_SIZE;
    copyRegion.extent.height = PREVIEW_SIZE;
    copyRegion.extent.depth = 1;
    
    vkCmdCopyImage(commandBuffer, m_ColorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
    
    VkImageMemoryBarrier readBarrier = {};
    readBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    readBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    readBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    readBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    readBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    readBarrier.image = dstImage;
    readBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    readBarrier.subresourceRange.baseMipLevel = 0;
    readBarrier.subresourceRange.levelCount = 1;
    readBarrier.subresourceRange.baseArrayLayer = 0;
    readBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 0, nullptr, 1, &readBarrier);
    
    vkEndCommandBuffer(commandBuffer);
    
    submitInfo.pCommandBuffers = &commandBuffer;
    err = vkQueueSubmit(g_Queue, 1, &submitInfo, VK_NULL_HANDLE);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to submit copy command buffer: %d\n", err);
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        vkDestroyImage(g_Device, dstImage, g_Allocator);
        vkFreeMemory(g_Device, dstImageMemory, g_Allocator);
        return false;
    }
    
    vkQueueWaitIdle(g_Queue);
    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
    
    void* data;
    vkMapMemory(g_Device, dstImageMemory, 0, dstMemReqs.size, 0, &data);
    
    SDL_Surface* surface = SDL_CreateSurface(PREVIEW_SIZE, PREVIEW_SIZE, SDL_PIXELFORMAT_ABGR8888);
    if (surface) {
        uint8_t* pixels = (uint8_t*)surface->pixels;
        uint8_t* srcData = (uint8_t*)data;
        
        VkImageSubresource subresource = {};
        subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        VkSubresourceLayout layout;
        vkGetImageSubresourceLayout(g_Device, dstImage, &subresource, &layout);
        
        for (uint32_t y = 0; y < PREVIEW_SIZE; y++) {
            uint32_t* srcRow = (uint32_t*)(srcData + layout.rowPitch * y);
            uint32_t* dstRow = (uint32_t*)(pixels + surface->pitch * y);
            memcpy(dstRow, srcRow, PREVIEW_SIZE * 4);
        }
        
        std::string outputPathPng = outputPath;
        if (outputPathPng.size() > 4 && outputPathPng.substr(outputPathPng.size() - 4) == ".png") {
            IMG_SavePNG(surface, outputPathPng.c_str());
        } else {
            SDL_SaveBMP(surface, outputPath.c_str());
        }
        SDL_DestroySurface(surface);
    }
    
    vkUnmapMemory(g_Device, dstImageMemory);
    vkDestroyImage(g_Device, dstImage, g_Allocator);
    vkFreeMemory(g_Device, dstImageMemory, g_Allocator);
    
    vkDeviceWaitIdle(g_Device);
    
    printf("Material preview saved successfully: %s\n", outputPath.c_str());
    return true;
}

bool PreviewGenerator::GenerateVoxPreview(const std::string& voxPath, const std::string& outputPath) {
#ifdef __ANDROID__
    // 安卓平台不支持文件系统写入，跳过预览生成
    printf("Vox preview generation skipped on Android\n");
    return false;
#endif
    
    if (!m_Initialized) {
        Init();
        if (!m_Initialized) {
            return false; // 预览初始化失败,跳过本次生成
        }
    }
    
    vkDeviceWaitIdle(g_Device);
    
    // 初始化 VoxRenderer
    if (!m_VoxRenderer) {
        m_VoxRenderer = std::make_unique<PreviewVoxRenderer>();
    }
    
    // 加载 VOX 文件
    VoxFormat::VoxData voxData;
    if (!VoxFormat::LoadVoxFile(voxPath, voxData)) {
        fprintf(stderr, "PreviewGenerator: Failed to load VOX file: %s\n", voxPath.c_str());
        return false;
    }
    
    m_VoxRenderer->Cleanup();
    m_VoxRenderer->Init(m_RenderPass);
    
    if (!m_VoxRenderer->LoadFromVoxData(voxData, 1.0f)) {
        fprintf(stderr, "PreviewGenerator: Failed to load VOX data\n");
        return false;
    }
    
    // 获取模型边界
    glm::vec3 minBounds = m_VoxRenderer->GetMinBounds();
    glm::vec3 maxBounds = m_VoxRenderer->GetMaxBounds();
    glm::vec3 center = (minBounds + maxBounds) * 0.5f;
    glm::vec3 size = maxBounds - minBounds;
    float maxDim = glm::max(size.x, glm::max(size.y, size.z));
    float cameraDistance = maxDim * 2.5f;
    
    // 设置相机位置
    glm::vec3 cameraPos = center + glm::vec3(-cameraDistance * 0.707f, maxDim * 0.5f, -cameraDistance * 0.707f);
    glm::vec3 cameraTarget = center;
    
    glm::mat4 view = glm::lookAt(cameraPos, cameraTarget, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, cameraDistance * 10.0f);
    proj[1][1] *= -1;
    
    glm::mat4 projView = proj * view;
    
    // 创建实例数据
    std::vector<VoxelInstanceData> instanceData(1);
    instanceData[0].model = glm::mat4(1.0f);
    instanceData[0].prevModel = glm::mat4(1.0f);
    instanceData[0].albedoColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    instanceData[0].materialData = glm::vec4(0.0f, 0.75f, 1.0f, 0.0f);
    instanceData[0].worldMinBounds = minBounds;
    instanceData[0].voxelSize = 1.0f;
    
    // 分配命令缓冲区
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = g_CommandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    
    VkCommandBuffer commandBuffer;
    VkResult err = vkAllocateCommandBuffers(g_Device, &allocInfo, &commandBuffer);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to allocate command buffer\n");
        return false;
    }
    
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    
    // 开始渲染
    VkRenderPassBeginInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_RenderPass;
    renderPassInfo.framebuffer = m_Framebuffer;
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = { PREVIEW_SIZE, PREVIEW_SIZE };
    
    VkClearValue clearValues[2] = {};
    clearValues[0].color = { { 0.1f, 0.1f, 0.12f, 1.0f } };
    clearValues[1].depthStencil = { 1.0f, 0 };
    
    renderPassInfo.clearValueCount = 2;
    renderPassInfo.pClearValues = clearValues;
    
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    
    // 渲染 VOX 模型
    m_VoxRenderer->RenderMesh(commandBuffer, PREVIEW_SIZE, PREVIEW_SIZE, projView, projView, cameraPos, instanceData);
    
    vkCmdEndRenderPass(commandBuffer);
    vkEndCommandBuffer(commandBuffer);
    
    // 提交命令
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    
    err = vkQueueSubmit(g_Queue, 1, &submitInfo, VK_NULL_HANDLE);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to submit command buffer: %d\n", err);
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        return false;
    }
    
    vkQueueWaitIdle(g_Queue);
    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
    
    // 创建目标图像用于读取
    VkImageCreateInfo dstImageInfo = {};
    dstImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    dstImageInfo.imageType = VK_IMAGE_TYPE_2D;
    dstImageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    dstImageInfo.extent.width = PREVIEW_SIZE;
    dstImageInfo.extent.height = PREVIEW_SIZE;
    dstImageInfo.extent.depth = 1;
    dstImageInfo.mipLevels = 1;
    dstImageInfo.arrayLayers = 1;
    dstImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    dstImageInfo.tiling = VK_IMAGE_TILING_LINEAR;
    dstImageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    dstImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    dstImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    VkImage dstImage;
    VkDeviceMemory dstImageMemory;
    
    err = vkCreateImage(g_Device, &dstImageInfo, g_Allocator, &dstImage);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to create destination image\n");
        return false;
    }
    
    VkMemoryRequirements dstMemReqs;
    vkGetImageMemoryRequirements(g_Device, dstImage, &dstMemReqs);
    
    VkMemoryAllocateInfo dstAllocInfo = {};
    dstAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    dstAllocInfo.allocationSize = dstMemReqs.size;
    dstAllocInfo.memoryTypeIndex = FindMemoryType(dstMemReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    err = vkAllocateMemory(g_Device, &dstAllocInfo, g_Allocator, &dstImageMemory);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to allocate destination image memory\n");
        vkDestroyImage(g_Device, dstImage, g_Allocator);
        return false;
    }
    
    vkBindImageMemory(g_Device, dstImage, dstImageMemory, 0);
    
    // 复制图像到主机可见内存
    allocInfo.commandBufferCount = 1;
    err = vkAllocateCommandBuffers(g_Device, &allocInfo, &commandBuffer);
    if (err != VK_SUCCESS) {
        vkDestroyImage(g_Device, dstImage, g_Allocator);
        vkFreeMemory(g_Device, dstImageMemory, g_Allocator);
        return false;
    }
    
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    
    VkImageMemoryBarrier srcBarrier = {};
    srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    srcBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.image = m_ColorImage;
    srcBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    srcBarrier.subresourceRange.baseMipLevel = 0;
    srcBarrier.subresourceRange.levelCount = 1;
    srcBarrier.subresourceRange.baseArrayLayer = 0;
    srcBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &srcBarrier);
    
    VkImageMemoryBarrier dstBarrier = {};
    dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dstBarrier.srcAccessMask = 0;
    dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.image = dstImage;
    dstBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dstBarrier.subresourceRange.baseMipLevel = 0;
    dstBarrier.subresourceRange.levelCount = 1;
    dstBarrier.subresourceRange.baseArrayLayer = 0;
    dstBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &dstBarrier);
    
    VkImageCopy copyRegion = {};
    copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.srcSubresource.mipLevel = 0;
    copyRegion.srcSubresource.baseArrayLayer = 0;
    copyRegion.srcSubresource.layerCount = 1;
    copyRegion.srcOffset = { 0, 0, 0 };
    copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.dstSubresource.mipLevel = 0;
    copyRegion.dstSubresource.baseArrayLayer = 0;
    copyRegion.dstSubresource.layerCount = 1;
    copyRegion.dstOffset = { 0, 0, 0 };
    copyRegion.extent.width = PREVIEW_SIZE;
    copyRegion.extent.height = PREVIEW_SIZE;
    copyRegion.extent.depth = 1;
    
    vkCmdCopyImage(commandBuffer, m_ColorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
    
    VkImageMemoryBarrier readBarrier = {};
    readBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    readBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    readBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    readBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    readBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    readBarrier.image = dstImage;
    readBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    readBarrier.subresourceRange.baseMipLevel = 0;
    readBarrier.subresourceRange.levelCount = 1;
    readBarrier.subresourceRange.baseArrayLayer = 0;
    readBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 0, nullptr, 1, &readBarrier);
    
    vkEndCommandBuffer(commandBuffer);
    
    submitInfo.pCommandBuffers = &commandBuffer;
    err = vkQueueSubmit(g_Queue, 1, &submitInfo, VK_NULL_HANDLE);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "PreviewGenerator: Failed to submit copy command buffer: %d\n", err);
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        vkDestroyImage(g_Device, dstImage, g_Allocator);
        vkFreeMemory(g_Device, dstImageMemory, g_Allocator);
        return false;
    }
    
    vkQueueWaitIdle(g_Queue);
    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
    
    // 保存图像到文件
    void* data;
    vkMapMemory(g_Device, dstImageMemory, 0, dstMemReqs.size, 0, &data);
    
    SDL_Surface* surface = SDL_CreateSurface(PREVIEW_SIZE, PREVIEW_SIZE, SDL_PIXELFORMAT_ABGR8888);
    if (surface) {
        uint8_t* pixels = (uint8_t*)surface->pixels;
        uint8_t* srcData = (uint8_t*)data;
        
        VkImageSubresource subresource = {};
        subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        VkSubresourceLayout layout;
        vkGetImageSubresourceLayout(g_Device, dstImage, &subresource, &layout);
        
        for (uint32_t y = 0; y < PREVIEW_SIZE; y++) {
            uint32_t* srcRow = (uint32_t*)(srcData + layout.rowPitch * y);
            uint32_t* dstRow = (uint32_t*)(pixels + surface->pitch * y);
            memcpy(dstRow, srcRow, PREVIEW_SIZE * 4);
        }
        
        std::string outputPathPng = outputPath;
        if (outputPathPng.size() > 4 && outputPathPng.substr(outputPathPng.size() - 4) == ".png") {
            IMG_SavePNG(surface, outputPathPng.c_str());
        } else {
            SDL_SaveBMP(surface, outputPath.c_str());
        }
        SDL_DestroySurface(surface);
    }
    
    vkUnmapMemory(g_Device, dstImageMemory);
    vkDestroyImage(g_Device, dstImage, g_Allocator);
    vkFreeMemory(g_Device, dstImageMemory, g_Allocator);
    
    vkDeviceWaitIdle(g_Device);
    
    printf("Vox preview saved successfully: %s\n", outputPath.c_str());
    return true;
}
