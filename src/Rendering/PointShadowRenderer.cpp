#include "Rendering/PointShadowRenderer.h"
#include "Core/EngineGlobal.h"
#include "Core/VulkanContext.h"
#include "Core/Log.h"

#include <glm/gtc/matrix_transform.hpp>

// 点光源阴影实现：cubemap 数组 + 6 面深度渲染（线性深度 dist/range）
// 面朝向约定（与 samplerCubeArray 采样方向无关——cube 覆盖全方向）：
//   face 0-5 = +X -X +Y -Y +Z -Z（标准 cubemap 面序）

namespace {
uint32_t FindShadowMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return VK_MAX_MEMORY_TYPES;
}
}

void PointShadowRenderer::Cleanup()
{
    if (!m_Initialized && m_Image == VK_NULL_HANDLE) return;
    for (auto& fb : m_FrameBuffers) {
        if (fb != VK_NULL_HANDLE && g_Device) vkDestroyFramebuffer(g_Device, fb, g_Allocator);
        fb = VK_NULL_HANDLE;
    }
    for (auto& v : m_FaceViews) {
        if (v != VK_NULL_HANDLE && g_Device) vkDestroyImageView(g_Device, v, g_Allocator);
        v = VK_NULL_HANDLE;
    }
    if (m_CubeArrayView != VK_NULL_HANDLE && g_Device) vkDestroyImageView(g_Device, m_CubeArrayView, g_Allocator);
    m_CubeArrayView = VK_NULL_HANDLE;
    if (m_RenderPass != VK_NULL_HANDLE && g_Device) vkDestroyRenderPass(g_Device, m_RenderPass, g_Allocator);
    m_RenderPass = VK_NULL_HANDLE;
    if (m_Image != VK_NULL_HANDLE && g_Device) vkDestroyImage(g_Device, m_Image, g_Allocator);
    m_Image = VK_NULL_HANDLE;
    if (m_Memory != VK_NULL_HANDLE && g_Device) vkFreeMemory(g_Device, m_Memory, g_Allocator);
    m_Memory = VK_NULL_HANDLE;
    m_Initialized = false;
}

bool PointShadowRenderer::Init()
{
    if (m_Initialized) return true;
    const uint32_t layerCount = MAX_SHADOW_LIGHTS * 6;

    // ---- cube array image（D16_UNORM，CUBE_COMPATIBLE）----
    VkImageCreateInfo ii = {};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_D16_UNORM;
    ii.extent = { SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = layerCount;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (vkCreateImage(g_Device, &ii, g_Allocator, &m_Image) != VK_SUCCESS) {
        LOGE("[PointShadow] cube array image create failed");
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_Device, m_Image, &req);
    const uint32_t mt = FindShadowMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == VK_MAX_MEMORY_TYPES) { Cleanup(); return false; }
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mt;
    if (vkAllocateMemory(g_Device, &ai, g_Allocator, &m_Memory) != VK_SUCCESS) { Cleanup(); return false; }
    vkBindImageMemory(g_Device, m_Image, m_Memory, 0);

    // ---- cube array view（采样）----
    VkImageViewCreateInfo cvi = {};
    cvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    cvi.image = m_Image;
    cvi.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    cvi.format = VK_FORMAT_D16_UNORM;
    cvi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    cvi.subresourceRange.baseMipLevel = 0;
    cvi.subresourceRange.levelCount = 1;
    cvi.subresourceRange.baseArrayLayer = 0;
    cvi.subresourceRange.layerCount = layerCount;
    if (vkCreateImageView(g_Device, &cvi, g_Allocator, &m_CubeArrayView) != VK_SUCCESS) { Cleanup(); return false; }

    // ---- face 2D views + framebuffers ----
    VkAttachmentDescription att = {};
    att.format = VK_FORMAT_D16_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref = {};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 0;
    sub.pDepthStencilAttachment = &ref;
    VkRenderPassCreateInfo rpi = {};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    if (vkCreateRenderPass(g_Device, &rpi, g_Allocator, &m_RenderPass) != VK_SUCCESS) { Cleanup(); return false; }

    for (uint32_t i = 0; i < layerCount; i++) {
        VkImageViewCreateInfo fvi = {};
        fvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        fvi.image = m_Image;
        fvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        fvi.format = VK_FORMAT_D16_UNORM;
        fvi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        fvi.subresourceRange.baseMipLevel = 0;
        fvi.subresourceRange.levelCount = 1;
        fvi.subresourceRange.baseArrayLayer = i;
        fvi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(g_Device, &fvi, g_Allocator, &m_FaceViews[i]) != VK_SUCCESS) { Cleanup(); return false; }
        VkFramebufferCreateInfo fbi = {};
        fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbi.renderPass = m_RenderPass;
        fbi.attachmentCount = 1;
        fbi.pAttachments = &m_FaceViews[i];
        fbi.width = SHADOW_MAP_SIZE;
        fbi.height = SHADOW_MAP_SIZE;
        fbi.layers = 1;
        if (vkCreateFramebuffer(g_Device, &fbi, g_Allocator, &m_FrameBuffers[i]) != VK_SUCCESS) { Cleanup(); return false; }
    }
    m_Initialized = true;
    LOGI("[PointShadow] cube array ready: %d lights × 6 faces × %d² D16", MAX_SHADOW_LIGHTS, SHADOW_MAP_SIZE);
    return true;
}

void PointShadowRenderer::UpdateMatrices(const glm::vec3* lightPositions, const float* lightRanges, int lightCount)
{
    m_LightCount = lightCount > MAX_SHADOW_LIGHTS ? MAX_SHADOW_LIGHTS : lightCount;
    const glm::vec3 faces[6][2] = {
        { glm::vec3(1, 0, 0), glm::vec3(0, -1, 0) },
        { glm::vec3(-1, 0, 0), glm::vec3(0, -1, 0) },
        { glm::vec3(0, 1, 0), glm::vec3(0, 0, 1) },
        { glm::vec3(0, -1, 0), glm::vec3(0, 0, -1) },
        { glm::vec3(0, 0, 1), glm::vec3(0, -1, 0) },
        { glm::vec3(0, 0, -1), glm::vec3(0, -1, 0) },
    };
    for (int l = 0; l < m_LightCount; l++) {
        const float farP = lightRanges[l] > 0.1f ? lightRanges[l] : 10.0f;
        const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, NEAR_PLANE, farP);
        for (int f = 0; f < 6; f++) {
            const glm::mat4 view = glm::lookAt(lightPositions[l], lightPositions[l] + faces[f][0], faces[f][1]);
            m_FaceProjView[l * 6 + f] = proj * view;
        }
    }
}

void PointShadowRenderer::BeginFace(VkCommandBuffer cmd, int lightIndex, int face, int width, int height)
{
    if (lightIndex >= m_LightCount) return;
    const int idx = lightIndex * 6 + face;
    VkClearValue clear = {};
    clear.depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo rpbi = {};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = m_RenderPass;
    rpbi.framebuffer = m_FrameBuffers[idx];
    rpbi.renderArea = { 0, 0, (uint32_t)width, (uint32_t)height };
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = {};
    vp.width = (float)width;
    vp.height = (float)height;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc = {};
    sc.extent = { (uint32_t)width, (uint32_t)height };
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void PointShadowRenderer::EndFace(VkCommandBuffer cmd)
{
    vkCmdEndRenderPass(cmd);
}

void PointShadowRenderer::Finalize(VkCommandBuffer cmd)
{
    // cube array：depth attachment → shader read（深度采样）
    VkImageMemoryBarrier imb = {};
    imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imb.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    imb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imb.image = m_Image;
    imb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    imb.subresourceRange.baseMipLevel = 0;
    imb.subresourceRange.levelCount = 1;
    imb.subresourceRange.baseArrayLayer = 0;
    imb.subresourceRange.layerCount = MAX_SHADOW_LIGHTS * 6;
    imb.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,   // 旧 Vulkan 头无 LATE/EARLY_FRAGMENT 位（1.3）
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &imb);
}
