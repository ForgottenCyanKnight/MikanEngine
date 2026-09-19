#include "Rendering/WaterTargetRT.h"

#include "Core/VulkanContext.h"
#include "Core/VulkanManager.h"
#include "Core/Log.h"

#include <array>

// ============================================================================
// WaterTargetRT 实现 —— 见头文件注释（deferred water compositing 数据源）。
// 图像/内存创建采用与 VulkanPostProcessHistory 相同的裸 vkCreateImage 路线。
// ============================================================================

WaterTargetRT::~WaterTargetRT() {
    Cleanup();
}

bool WaterTargetRT::EnsureRenderPass() {
    if (m_RenderPass != VK_NULL_HANDLE) return true;
    if (g_Device == VK_NULL_HANDLE) return false;

    // 颜色附件：RGBA16F，finalLayout=SHADER_READ_ONLY（后处理直接采样）。
    // initialLayout 同样声明 SHADER_READ_ONLY——首帧前的 UNDEFINED→SHADER_READ_ONLY
    // 由 BeginPass 里的 warmup 丢弃屏障完成（内容未定义无所谓，每帧 CLEAR）。
    VkAttachmentDescription color{};
    color.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // 深度附件：只用于水面自遮挡排序，每帧 CLEAR，不跨帧保持。
    VkAttachmentDescription depth{};
    depth.format = VK_FORMAT_D32_SFLOAT;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    // 外部依赖：上一帧对该 RT 的采样读 → 本帧附件写（initialLayout 语义要求）。
    VkSubpassDependency depIn{};
    depIn.srcSubpass = VK_SUBPASS_EXTERNAL;
    depIn.dstSubpass = 0;
    depIn.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    depIn.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    depIn.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    depIn.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency depOut{};
    depOut.srcSubpass = 0;
    depOut.dstSubpass = VK_SUBPASS_EXTERNAL;
    depOut.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    depOut.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depOut.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    depOut.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkAttachmentDescription attachments[2] = { color, depth };
    VkSubpassDependency dependencies[2] = { depIn, depOut };
    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 2;
    rpInfo.pAttachments = attachments;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 2;
    rpInfo.pDependencies = dependencies;
    if (vkCreateRenderPass(g_Device, &rpInfo, g_Allocator, &m_RenderPass) != VK_SUCCESS) {
        m_RenderPass = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool WaterTargetRT::EnsureSize(uint32_t width, uint32_t height) {
    if (g_Device == VK_NULL_HANDLE || width == 0 || height == 0) return false;
    if (m_Framebuffer != VK_NULL_HANDLE && m_Width == width && m_Height == height) {
        return true;
    }
    if (!EnsureRenderPass()) return false;
    DestroyImages();
    if (!CreateImages(width, height)) {
        DestroyImages();
        return false;
    }
    m_Width = width;
    m_Height = height;

    std::array<VkImageView, 2> fbViews = { m_View, m_DepthView };
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = m_RenderPass;
    fbInfo.attachmentCount = static_cast<uint32_t>(fbViews.size());
    fbInfo.pAttachments = fbViews.data();
    fbInfo.width = width;
    fbInfo.height = height;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(g_Device, &fbInfo, g_Allocator, &m_Framebuffer) != VK_SUCCESS) {
        m_Framebuffer = VK_NULL_HANDLE;
        DestroyImages();
        return false;
    }

    if (m_Sampler == VK_NULL_HANDLE) {
        // Nearest：mask 是逐像素覆盖判定，不做插值模糊（岸边柔边留给合成端）。
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_NEAREST;
        si.minFilter = VK_FILTER_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(g_Device, &si, g_Allocator, &m_Sampler) != VK_SUCCESS) {
            m_Sampler = VK_NULL_HANDLE;
        }
    }
    return true;
}

bool WaterTargetRT::CreateImages(uint32_t width, uint32_t height) {
    auto createImage = [&](VkFormat format, VkImageUsageFlags usage,
                           VkImage& image, VkDeviceMemory& memory) -> bool {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format;
        ii.extent = { width, height, 1 };
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(g_Device, &ii, g_Allocator, &image) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(g_Device, image, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mp);
        for (uint32_t m = 0; m < mp.memoryTypeCount; ++m) {
            if ((mr.memoryTypeBits & (1u << m)) &&
                (mp.memoryTypes[m].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                ai.memoryTypeIndex = m;
                break;
            }
        }
        if (vkAllocateMemory(g_Device, &ai, g_Allocator, &memory) != VK_SUCCESS) {
            return false;
        }
        return vkBindImageMemory(g_Device, image, memory, 0) == VK_SUCCESS;
    };

    if (!createImage(VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     m_Image, m_Memory)) {
        return false;
    }
    if (!createImage(VK_FORMAT_D32_SFLOAT,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     m_DepthImage, m_DepthMemory)) {
        return false;
    }

    auto createView = [&](VkImage image, VkFormat format, VkImageAspectFlags aspect) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = { aspect, 0, 1, 0, 1 };
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(g_Device, &vi, g_Allocator, &view);
        return view;
    };
    m_View = createView(m_Image, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    m_DepthView = createView(m_DepthImage, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (m_View == VK_NULL_HANDLE || m_DepthView == VK_NULL_HANDLE) {
        return false;
    }

    // 新建的 color image 当前在 UNDEFINED——下一帧渲染前先丢弃屏障到
    // SHADER_READ_ONLY（render pass 的 initialLayout），内容无所谓（每帧 CLEAR）。
    m_NeedsLayoutWarmup = true;
    LOGI("[WaterTargetRT] created %ux%u (RGBA16F mask/viewZ/normal + D32)", width, height);
    return true;
}

void WaterTargetRT::DestroyImages() {
    if (g_Device != VK_NULL_HANDLE) {
        if (m_Framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(g_Device, m_Framebuffer, g_Allocator);
            m_Framebuffer = VK_NULL_HANDLE;
        }
        if (m_View != VK_NULL_HANDLE) {
            vkDestroyImageView(g_Device, m_View, g_Allocator);
            m_View = VK_NULL_HANDLE;
        }
        if (m_DepthView != VK_NULL_HANDLE) {
            vkDestroyImageView(g_Device, m_DepthView, g_Allocator);
            m_DepthView = VK_NULL_HANDLE;
        }
        if (m_Image != VK_NULL_HANDLE) {
            vkDestroyImage(g_Device, m_Image, g_Allocator);
            m_Image = VK_NULL_HANDLE;
        }
        if (m_Memory != VK_NULL_HANDLE) {
            vkFreeMemory(g_Device, m_Memory, g_Allocator);
            m_Memory = VK_NULL_HANDLE;
        }
        if (m_DepthImage != VK_NULL_HANDLE) {
            vkDestroyImage(g_Device, m_DepthImage, g_Allocator);
            m_DepthImage = VK_NULL_HANDLE;
        }
        if (m_DepthMemory != VK_NULL_HANDLE) {
            vkFreeMemory(g_Device, m_DepthMemory, g_Allocator);
            m_DepthMemory = VK_NULL_HANDLE;
        }
    } else {
        m_Image = VK_NULL_HANDLE;
        m_Memory = VK_NULL_HANDLE;
        m_View = VK_NULL_HANDLE;
        m_DepthImage = VK_NULL_HANDLE;
        m_DepthMemory = VK_NULL_HANDLE;
        m_DepthView = VK_NULL_HANDLE;
        m_Framebuffer = VK_NULL_HANDLE;
    }
    m_Width = m_Height = 0;
    m_NeedsLayoutWarmup = false;
}

void WaterTargetRT::Cleanup() {
    if (g_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
        DestroyImages();
        if (m_Sampler != VK_NULL_HANDLE) {
            vkDestroySampler(g_Device, m_Sampler, g_Allocator);
            m_Sampler = VK_NULL_HANDLE;
        }
        if (m_RenderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(g_Device, m_RenderPass, g_Allocator);
            m_RenderPass = VK_NULL_HANDLE;
        }
    } else {
        DestroyImages();
        m_Sampler = VK_NULL_HANDLE;
        m_RenderPass = VK_NULL_HANDLE;
    }
}

void WaterTargetRT::BeginPass(VkCommandBuffer commandBuffer) {
    if (commandBuffer == VK_NULL_HANDLE || m_Framebuffer == VK_NULL_HANDLE) {
        return;
    }
    if (m_NeedsLayoutWarmup) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_Image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        m_NeedsLayoutWarmup = false;
    }

    VkClearValue clears[2]{};
    clears[0].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };   // mask=0 = 无水
    clears[1].depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = m_RenderPass;
    rpBegin.framebuffer = m_Framebuffer;
    rpBegin.renderArea.offset = { 0, 0 };
    rpBegin.renderArea.extent = { m_Width, m_Height };
    rpBegin.clearValueCount = 2;
    rpBegin.pClearValues = clears;
    vkCmdBeginRenderPass(commandBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
}

void WaterTargetRT::EndPass(VkCommandBuffer commandBuffer) const {
    if (commandBuffer == VK_NULL_HANDLE || m_Framebuffer == VK_NULL_HANDLE) {
        return;
    }
    vkCmdEndRenderPass(commandBuffer);
}
