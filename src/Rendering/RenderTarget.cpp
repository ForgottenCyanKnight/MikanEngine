#include "RenderTarget.h"
#include "EngineGlobal.h"

// 注意：vulkan.h 为 Vulkan 1.4 头（VK_HEADER_VERSION 346）。input attachment 引用布局用
// VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL（1.4 核心化 KHR_maintenance5，替代已移除的 INPUT_ATTACHMENT_OPTIMAL）。

#include <stdio.h>
#include <iostream>

static bool CheckFormatSupport(VkPhysicalDevice physicalDevice, VkFormat format, VkImageTiling tiling, VkFormatFeatureFlags features) {
    VkFormatProperties formatProps;
    vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &formatProps);
    
    if (tiling == VK_IMAGE_TILING_LINEAR && (formatProps.linearTilingFeatures & features) == features) {
        return true;
    } else if (tiling == VK_IMAGE_TILING_OPTIMAL && (formatProps.optimalTilingFeatures & features) == features) {
        return true;
    }
    return false;
}

static VkFormat SelectCompatibleFormat(VkPhysicalDevice physicalDevice, const std::vector<VkFormat>& preferredFormats, VkImageTiling tiling, VkFormatFeatureFlags features) {
    for (VkFormat format : preferredFormats) {
        if (CheckFormatSupport(physicalDevice, format, tiling, features)) {
            return format;
        }
    }
    return VK_FORMAT_UNDEFINED;
}

static uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    fprintf(stderr, "Failed to find suitable memory type!\n");
    return 0;
}

RenderTarget::RenderTarget()
{
}

RenderTarget::~RenderTarget()
{
    Cleanup();
}

void RenderTarget::Init(uint32_t width, uint32_t height, bool useMRT, bool outputPosition)
{
    if (m_Initialized) {
        Cleanup();
    }
    
    m_Width = width;
    m_Height = height;
    m_UseMRT = useMRT;
    m_OutputPosition = outputPosition;

    // Keep the render-pass topology identical on every MRT platform.  The
    // geometry pass owns the G-buffer (and the desktop composite placeholder),
    // while lighting/composite is always rendered by a separate pass that
    // samples the stored G-buffer images.
    m_UseSeparateComposite = m_UseMRT;
    if (m_UseSeparateComposite) {
        g_UseSeparateMrtRenderPass = true;
        printf("[RenderTarget] MRT: geometry render pass + separate composite render pass\n");
    }

#ifdef __ANDROID__
    // Adreno 对 B10G11R11_UFLOAT_PACK32 作为 composite render-pass 附件
    // 支持不稳，独立 composite pass 使用最通用的 R16G16B16A16_SFLOAT。
    m_CompositeFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
#endif
    
    CreateRenderPass();
    CreateColorResources();
    CreateDepthResources();
    CreateDisplayResource();
    CreateCompositeImageResource();
    CreateFramebuffer();
#ifndef __ANDROID__
    // 桌面透明粒子单独使用 [composite, depth] pass，避免把额外 subpass 塞进 MRT 主 pass。
    CreateParticleRenderPass();
    CreateParticleFramebuffer();
#endif
    if (m_UseSeparateComposite) {
        CreateCompositeRenderPass();
        CreateCompositeFramebuffer();
    }
    CreateFinalRenderPass();
    CreateFinalFramebuffer();
    // 注意：CreateDescriptorSet 需要在 ImGui_ImplVulkan_Init 之后调用
    // 所以这里不立即创建描述符集
    
    m_Initialized = true;
}

void RenderTarget::Cleanup()
{
    if (!m_Initialized) return;
    
    // 注意：vkDeviceWaitIdle已经在调用Cleanup的函数中执行过了，这里不需要再次调用
    // vkDeviceWaitIdle(g_Device);
    
    if (m_Sampler != VK_NULL_HANDLE) {
        vkDestroySampler(g_Device, m_Sampler, g_Allocator);
        m_Sampler = VK_NULL_HANDLE;
    }
    
    if (m_DescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(g_Device, m_DescriptorPool, g_Allocator);
        m_DescriptorPool = VK_NULL_HANDLE;
    }
    
    if (m_Framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(g_Device, m_Framebuffer, g_Allocator);
        m_Framebuffer = VK_NULL_HANDLE;
    }

    if (m_ParticleFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(g_Device, m_ParticleFramebuffer, g_Allocator);
        m_ParticleFramebuffer = VK_NULL_HANDLE;
    }

    if (m_RenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(g_Device, m_RenderPass, g_Allocator);
        m_RenderPass = VK_NULL_HANDLE;
    }

    if (m_ParticleRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(g_Device, m_ParticleRenderPass, g_Allocator);
        m_ParticleRenderPass = VK_NULL_HANDLE;
    }
    
    if (m_CompositeFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(g_Device, m_CompositeFramebuffer, g_Allocator);
        m_CompositeFramebuffer = VK_NULL_HANDLE;
    }
    if (m_CompositeRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(g_Device, m_CompositeRenderPass, g_Allocator);
        m_CompositeRenderPass = VK_NULL_HANDLE;
    }
    
    if (m_CompositeImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(g_Device, m_CompositeImageView, g_Allocator);
        m_CompositeImageView = VK_NULL_HANDLE;
    }
    if (m_CompositeImage != VK_NULL_HANDLE) {
        vkDestroyImage(g_Device, m_CompositeImage, g_Allocator);
        m_CompositeImage = VK_NULL_HANDLE;
    }
    if (m_CompositeImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_CompositeImageMemory, g_Allocator);
        m_CompositeImageMemory = VK_NULL_HANDLE;
    }
    
    if (m_FinalFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(g_Device, m_FinalFramebuffer, g_Allocator);
        m_FinalFramebuffer = VK_NULL_HANDLE;
    }
    
    if (m_FinalRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(g_Device, m_FinalRenderPass, g_Allocator);
        m_FinalRenderPass = VK_NULL_HANDLE;
    }
    if (m_DisplayUIRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(g_Device, m_DisplayUIRenderPass, g_Allocator);
        m_DisplayUIRenderPass = VK_NULL_HANDLE;
    }
    
    // 清理颜色附件
    for (size_t i = 0; i < m_ColorImageViews.size(); i++) {
        if (m_ColorImageViews[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(g_Device, m_ColorImageViews[i], g_Allocator);
        }
        if (m_ColorImages[i] != VK_NULL_HANDLE) {
            vkDestroyImage(g_Device, m_ColorImages[i], g_Allocator);
        }
        if (m_ColorImageMemories[i] != VK_NULL_HANDLE) {
            vkFreeMemory(g_Device, m_ColorImageMemories[i], g_Allocator);
        }
    }
    m_ColorImageViews.clear();
    m_ColorImages.clear();
    m_ColorImageMemories.clear();
    
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
    
    // 清理显示附件（合成 subpass 输出）
    if (m_DisplayImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(g_Device, m_DisplayImageView, g_Allocator);
        m_DisplayImageView = VK_NULL_HANDLE;
    }
    if (m_DisplayImage != VK_NULL_HANDLE) {
        vkDestroyImage(g_Device, m_DisplayImage, g_Allocator);
        m_DisplayImage = VK_NULL_HANDLE;
    }
    if (m_DisplayImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_DisplayImageMemory, g_Allocator);
        m_DisplayImageMemory = VK_NULL_HANDLE;
    }
    
    m_Initialized = false;
}

void RenderTarget::Resize(uint32_t width, uint32_t height)
{
    if (width == m_Width && height == m_Height) return;
    
    // 确保尺寸有效
    if (width == 0 || height == 0) return;
    
    // 注意：vkDeviceWaitIdle已经在调用Resize的函数中执行过了，这里不需要再次调用
    // VkResult err = vkDeviceWaitIdle(g_Device);
    // if (err != VK_SUCCESS) {
    //     fprintf(stderr, "vkDeviceWaitIdle failed in Resize: %d\n", err);
    //     return;
    // }
    
    // 清理旧的描述符集相关资源
    if (m_DescriptorSet != VK_NULL_HANDLE) {
        // ImGui描述符集会由ImGui管理，这里不需要单独销毁
        m_DescriptorSet = VK_NULL_HANDLE;
    }
    if (m_Sampler != VK_NULL_HANDLE) {
        vkDestroySampler(g_Device, m_Sampler, g_Allocator);
        m_Sampler = VK_NULL_HANDLE;
    }
    

    
    if (m_Framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(g_Device, m_Framebuffer, g_Allocator);
        m_Framebuffer = VK_NULL_HANDLE;
    }
    
    // 清理颜色附件
    for (size_t i = 0; i < m_ColorImageViews.size(); i++) {
        if (m_ColorImageViews[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(g_Device, m_ColorImageViews[i], g_Allocator);
        }
        if (m_ColorImages[i] != VK_NULL_HANDLE) {
            vkDestroyImage(g_Device, m_ColorImages[i], g_Allocator);
        }
        if (m_ColorImageMemories[i] != VK_NULL_HANDLE) {
            vkFreeMemory(g_Device, m_ColorImageMemories[i], g_Allocator);
        }
    }
    m_ColorImageViews.clear();
    m_ColorImages.clear();
    m_ColorImageMemories.clear();
    
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
    
    if (m_DisplayImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(g_Device, m_DisplayImageView, g_Allocator);
        m_DisplayImageView = VK_NULL_HANDLE;
    }
    if (m_DisplayImage != VK_NULL_HANDLE) {
        vkDestroyImage(g_Device, m_DisplayImage, g_Allocator);
        m_DisplayImage = VK_NULL_HANDLE;
    }
    if (m_DisplayImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_DisplayImageMemory, g_Allocator);
        m_DisplayImageMemory = VK_NULL_HANDLE;
    }
    m_DisplayDescriptorSet = VK_NULL_HANDLE;  // ImGui 描述符由 ImGui 管理
    
    m_Width = width;
    m_Height = height;
    
    CreateColorResources();
    CreateDepthResources();
    CreateDisplayResource();
    CreateCompositeImageResource();
    // framebuffer 重建（含合成 subpass 输出附件 composite view；render pass 与尺寸无关，保持）
    if (m_Framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(g_Device, m_Framebuffer, g_Allocator);
        m_Framebuffer = VK_NULL_HANDLE;
    }
    if (m_ParticleFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(g_Device, m_ParticleFramebuffer, g_Allocator);
        m_ParticleFramebuffer = VK_NULL_HANDLE;
    }
    CreateFramebuffer();
    CreateParticleFramebuffer();
    // 分离合成通道 framebuffer 重建（render pass 与尺寸无关，保持）
    if (m_UseSeparateComposite) {
        if (m_CompositeFramebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(g_Device, m_CompositeFramebuffer, g_Allocator);
            m_CompositeFramebuffer = VK_NULL_HANDLE;
        }
        CreateCompositeFramebuffer();
    }
    // final framebuffer 重建（render pass 与尺寸无关，保持）
    if (m_FinalFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(g_Device, m_FinalFramebuffer, g_Allocator);
        m_FinalFramebuffer = VK_NULL_HANDLE;
    }
    CreateFinalFramebuffer();
    // 注意：CreateImGuiDescriptorSet 需要在外部调用，因为ImGui可能需要在特定时机初始化
}

void RenderTarget::CreateRenderPass()
{
    // 检查支持的格式
    VkFormatFeatureFlags colorAttachmentFeatures = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    
    // 统一使用 RGBA16F 格式 (移动端兼容性好)
    VkFormat mrtFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat motionVectorFormat = VK_FORMAT_R16G16_SFLOAT;

    // Keep the established G-buffer formats.  The AMD workaround is the
    // render-pass lifetime split below, not a format conversion.
    VkFormat mainColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat normalFormat = VK_FORMAT_R16G16_SNORM;
    VkFormat materialFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat motionVectorFormatSaved = motionVectorFormat;
    
    // 保存格式到成员变量
    m_MainColorFormat = mainColorFormat;
    m_NormalFormat = normalFormat;
    m_MotionVectorFormat = motionVectorFormatSaved;
    m_MaterialFormat = materialFormat;
    
    // 颜色附件描述
    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> colorAttachmentRefs;
    
    // 主颜色附件 (用于ImGui显示)
    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = mainColorFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments.push_back(colorAttachment);
    
    VkAttachmentReference colorAttachmentRef = {};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachmentRefs.push_back(colorAttachmentRef);
    
    if (m_UseMRT) {
        // 法线附件
        VkAttachmentDescription normalAttachment = {};
        normalAttachment.format = normalFormat;
        normalAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        normalAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        normalAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        normalAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        normalAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        normalAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        normalAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachments.push_back(normalAttachment);
        
        VkAttachmentReference normalAttachmentRef = {};
        normalAttachmentRef.attachment = 1;
        normalAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachmentRefs.push_back(normalAttachmentRef);
        
        // 材质属性附件 (metallic/roughness/ao/emissive)——被合成 subpass 消费（PBR 光照 + 自发光）；全平台 STORE
        VkAttachmentDescription materialAttachment = {};
        materialAttachment.format = materialFormat;
        materialAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        materialAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        materialAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        materialAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        materialAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        materialAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        materialAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachments.push_back(materialAttachment);
        
        VkAttachmentReference materialAttachmentRef = {};
        materialAttachmentRef.attachment = 2;
        materialAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachmentRefs.push_back(materialAttachmentRef);
        
        // 运动矢量附件（屏幕空间运动矢量）——TAA 预留未读；全平台 STORE（恢复回读能力）
        VkAttachmentDescription motionVectorAttachment = {};
        motionVectorAttachment.format = motionVectorFormat;
        motionVectorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        motionVectorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        motionVectorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        motionVectorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        motionVectorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        motionVectorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        motionVectorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachments.push_back(motionVectorAttachment);
        
        VkAttachmentReference motionVectorAttachmentRef = {};
        motionVectorAttachmentRef.attachment = 3;
        motionVectorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachmentRefs.push_back(motionVectorAttachmentRef);
    }
    
    // 深度附件同时被 render pass 写入、被合成/后处理采样，因此不能只检查
    // DEPTH_STENCIL_ATTACHMENT_BIT。AMD 的 D24_UNORM_S8 在本机不支持附件用途；
    // 直接回退到无 stencil 的 D32_SFLOAT 会把“深度/模板布局 + 采样”链路交给
    // 驱动自行推断，amdvlk 在后续 graphics pipeline 创建处发生访问冲突。
    // 优先保留带 stencil 的格式，且所有候选都必须同时支持附件和采样用途。
    const VkFormat depthCandidates[] = {
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D16_UNORM_S8_UINT,
        VK_FORMAT_D16_UNORM
    };
    const VkFormatFeatureFlags depthFeatures =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    for (VkFormat candidate : depthCandidates) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(g_PhysicalDevice, candidate, &props);
        if ((props.optimalTilingFeatures & depthFeatures) == depthFeatures) {
            depthFormat = candidate;
            break;
        }
    }

    if (depthFormat == VK_FORMAT_UNDEFINED) {
        fprintf(stderr, "[RenderTarget] no depth format supports attachment + sampled usage\n");
        return;
    }

    auto depthFormatName = [](VkFormat format) {
        switch (format) {
            case VK_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8";
            case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32_SFLOAT_S8";
            case VK_FORMAT_D32_SFLOAT: return "D32_SFLOAT";
            case VK_FORMAT_D16_UNORM_S8_UINT: return "D16_UNORM_S8";
            case VK_FORMAT_D16_UNORM: return "D16_UNORM";
            default: return "UNKNOWN";
        }
    };
    printf("[RenderTarget] depth format: %s (attachment + sampled)\n", depthFormatName(depthFormat));

    // 保存深度格式；CreateDepthResources、framebuffer 和独立粒子 pass 都复用它。
    m_DepthFormat = depthFormat;
    
    VkAttachmentDescription depthAttachment = {};
    depthAttachment.format = depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    // 注意：depth push 在 MRT composite 之后（附件5 MRT / 附件1 非MRT）——composite 必须排在 depth 之前
    
    // Desktop MRT：composite 附件（附件4）先于 depth push。这个槽位仍然
    // 保留在 geometry framebuffer 中，但真正的 composite 在独立 pass 完成。
    // 合成 subpass 输出附件（后处理链输入）；先确定性清零，避免未覆盖 tile 在后续采样中泄漏。
    // finalLayout 保持 COLOR_ATTACHMENT_OPTIMAL——布局转换由 CompositeToFinalBarrier（COLOR_ATTACHMENT→SHADER_READ_ONLY）显式完成
    uint32_t compositeIndex = 0;   // 仅桌面 MRT 使用（Android 分离合成通道，composite 不在此 render pass）
#ifndef __ANDROID__
    if (m_UseMRT) {
        VkAttachmentDescription compositeAttachment = {};
        compositeAttachment.format = m_CompositeFormat;
        compositeAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        compositeAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        compositeAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        compositeAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        compositeAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        compositeAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        compositeAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments.push_back(compositeAttachment);   // 附件4
        compositeIndex = static_cast<uint32_t>(attachments.size() - 1);   // = 4

        // 几何管线的第五个颜色槽仍由写掩码关闭，但必须在同一个 geometry
        // subpass 中声明，确保附件的 CLEAR/STORE 生命周期完整。
        colorAttachmentRefs.push_back({ compositeIndex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
    }
#endif
    
    // depth 附件 push（MRT：附件5；非 MRT：附件1）
    attachments.push_back(depthAttachment);
    VkAttachmentReference depthAttachmentRef = {};
    depthAttachmentRef.attachment = static_cast<uint32_t>(attachments.size() - 1);
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    // 所有 MRT 平台统一为一个 geometry subpass；合成在该 render pass
    // 结束后通过独立的 [composite, depth] render pass 完成。这样 G-buffer
    // 一定在同一个 pass 内首次声明、CLEAR、STORE，再由普通纹理采样读取。
    // 非 MRT 目标（例如 skyRT）也保持一个几何 subpass。
    VkSubpassDescription subpass = {};
    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<uint32_t>(colorAttachmentRefs.size());
    subpass.pColorAttachments = colorAttachmentRefs.data();
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    if (m_UseMRT) {
        // geometry pass → separate composite pass.  The external scope covers
        // G-buffer shader reads, composite color writes and depth reads.
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
            | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT
            | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
            | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
            | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
            | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
            | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
            | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
            | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT
            | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
            | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    } else {
        // 非 MRT（skyRT）：subpass 0 即几何，单 subpass；0→EXT（几何写完后外部采样）
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        dependencies[1].dstAccessMask = 0;
    }

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 2u;
    renderPassInfo.pDependencies = dependencies;
    
    VkResult err = vkCreateRenderPass(g_Device, &renderPassInfo, g_Allocator, &m_RenderPass);
    check_vk_result(err);
}

void RenderTarget::CreateColorResources()
{
    // MRT 颜色附件数量
    uint32_t attachmentCount = 1; // 主颜色附件
    if (m_UseMRT) {
        attachmentCount = 4; // 主颜色 + 法线 + 位置 + 材质
    }
    
    m_ColorImages.resize(attachmentCount);
    m_ColorImageMemories.resize(attachmentCount);
    m_ColorImageViews.resize(attachmentCount);

    // The separate composite pass samples the stored G-buffer images through
    // combined image samplers; no input-attachment usage is required.
    const VkImageUsageFlags colorImageUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT;
    
    // 主颜色附件 (RGBA8_UNORM——albedo 8bit 够用，光照在合成 pass)
    {
        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = m_Width;
        imageInfo.extent.height = m_Height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = m_MainColorFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = colorImageUsage;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        VkResult err = vkCreateImage(g_Device, &imageInfo, g_Allocator, &m_ColorImages[0]);
        check_vk_result(err);
        
        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(g_Device, m_ColorImages[0], &memRequirements);
        
        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        
        err = vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_ColorImageMemories[0]);
        check_vk_result(err);
        
        vkBindImageMemory(g_Device, m_ColorImages[0], m_ColorImageMemories[0], 0);
        
        // 创建图像视图
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_ColorImages[0];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_MainColorFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        
        err = vkCreateImageView(g_Device, &viewInfo, g_Allocator, &m_ColorImageViews[0]);
        check_vk_result(err);
    }
    
    if (m_UseMRT) {
        // 法线附件 (R16G16_SNORM 4B——八面体编码完整世界法线，32bit 带宽)
        {
            VkImageCreateInfo imageInfo = {};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.extent.width = m_Width;
            imageInfo.extent.height = m_Height;
            imageInfo.extent.depth = 1;
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.format = m_NormalFormat;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imageInfo.usage = colorImageUsage;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            
            VkResult err = vkCreateImage(g_Device, &imageInfo, g_Allocator, &m_ColorImages[1]);
            check_vk_result(err);
            
            VkMemoryRequirements memRequirements;
            vkGetImageMemoryRequirements(g_Device, m_ColorImages[1], &memRequirements);
            
            VkMemoryAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memRequirements.size;
            allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            
            err = vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_ColorImageMemories[1]);
            check_vk_result(err);
            
            vkBindImageMemory(g_Device, m_ColorImages[1], m_ColorImageMemories[1], 0);
            
            // 创建图像视图
            VkImageViewCreateInfo viewInfo = {};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = m_ColorImages[1];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = m_NormalFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;
            
            err = vkCreateImageView(g_Device, &viewInfo, g_Allocator, &m_ColorImageViews[1]);
            check_vk_result(err);
        }
        
        // 运动矢量附件（附件3，RG16F）
        {
            VkImageCreateInfo imageInfo = {};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.extent.width = m_Width;
            imageInfo.extent.height = m_Height;
            imageInfo.extent.depth = 1;
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.format = m_MotionVectorFormat;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imageInfo.usage = colorImageUsage;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            
            VkResult err = vkCreateImage(g_Device, &imageInfo, g_Allocator, &m_ColorImages[3]);
            check_vk_result(err);
            
            VkMemoryRequirements memRequirements;
            vkGetImageMemoryRequirements(g_Device, m_ColorImages[3], &memRequirements);
            
            VkMemoryAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memRequirements.size;
            allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            
            err = vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_ColorImageMemories[3]);
            check_vk_result(err);
            
            vkBindImageMemory(g_Device, m_ColorImages[3], m_ColorImageMemories[3], 0);
            
            // 创建图像视图
            VkImageViewCreateInfo viewInfo = {};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = m_ColorImages[3];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = m_MotionVectorFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;
            
            err = vkCreateImageView(g_Device, &viewInfo, g_Allocator, &m_ColorImageViews[3]);
            check_vk_result(err);
        }
        
        // 材质属性附件（附件2，RGBA8_UNORM——metallic/roughness/ao [0,1]，8bit 够用）
        {
            VkImageCreateInfo imageInfo = {};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.extent.width = m_Width;
            imageInfo.extent.height = m_Height;
            imageInfo.extent.depth = 1;
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.format = m_MaterialFormat;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imageInfo.usage = colorImageUsage;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            
            VkResult err = vkCreateImage(g_Device, &imageInfo, g_Allocator, &m_ColorImages[2]);
            check_vk_result(err);
            
            VkMemoryRequirements memRequirements;
            vkGetImageMemoryRequirements(g_Device, m_ColorImages[2], &memRequirements);
            
            VkMemoryAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memRequirements.size;
            allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            
            err = vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_ColorImageMemories[2]);
            check_vk_result(err);
            
            vkBindImageMemory(g_Device, m_ColorImages[2], m_ColorImageMemories[2], 0);
            
            // 创建图像视图
            VkImageViewCreateInfo viewInfo = {};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = m_ColorImages[2];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = m_MaterialFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;
            
            err = vkCreateImageView(g_Device, &viewInfo, g_Allocator, &m_ColorImageViews[2]);
            check_vk_result(err);
        }
    }
}

void RenderTarget::CreateDepthResources()
{
    // 创建深度图像 (使用检测到的深度格式)
    // 添加SAMPLED_BIT以便计算着色器可以读取
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = m_Width;
    imageInfo.extent.height = m_Height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = m_DepthFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VkResult err = vkCreateImage(g_Device, &imageInfo, g_Allocator, &m_DepthImage);
    check_vk_result(err);
    
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(g_Device, m_DepthImage, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    err = vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_DepthImageMemory);
    check_vk_result(err);
    
    vkBindImageMemory(g_Device, m_DepthImage, m_DepthImageMemory, 0);
    
    // 创建深度图像视图
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_DepthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_DepthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    
    err = vkCreateImageView(g_Device, &viewInfo, g_Allocator, &m_DepthImageView);
    check_vk_result(err);
}

void RenderTarget::CreateFramebuffer()
{
    std::vector<VkImageView> attachments;
    
    // 添加颜色附件
    for (const auto& view : m_ColorImageViews) {
        attachments.push_back(view);
    }
    
    // 添加合成 subpass 输出附件（桌面 MRT：composite view 附件4——在深度之前，与 render pass 附件顺序对齐）
    // Android：composite 在独立合成通道，几何 framebuffer 不含它
#ifndef __ANDROID__
    if (m_UseMRT && m_CompositeImageView != VK_NULL_HANDLE) {
        attachments.push_back(m_CompositeImageView);
    }
#endif
    
    // 添加深度附件（MRT 时附件5，非 MRT 时附件1）
    if (m_DepthImageView != VK_NULL_HANDLE) {
        attachments.push_back(m_DepthImageView);
    }
    
    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_RenderPass;
    framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    framebufferInfo.pAttachments = attachments.data();
    framebufferInfo.width = m_Width;
    framebufferInfo.height = m_Height;
    framebufferInfo.layers = 1;
    
    VkResult err = vkCreateFramebuffer(g_Device, &framebufferInfo, g_Allocator, &m_Framebuffer);
    check_vk_result(err);
}

void RenderTarget::CreateParticleRenderPass()
{
#ifdef __ANDROID__
    return;
#else
    if (!m_UseMRT || m_CompositeFormat == VK_FORMAT_UNDEFINED || m_DepthFormat == VK_FORMAT_UNDEFINED) {
        return;
    }

    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = m_CompositeFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depthAttachment = {};
    depthAttachment.format = m_DepthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    // 后处理链在粒子 pass 之后仍会采样场景深度（GTAO/cloud_view/TAA）。
    // 即使本 pass 只读深度，也必须 STORE；DONT_CARE 会让 pass 结束后的深度内容变成未定义。
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depthRef = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependencies[2] = {};
    // 主场景 render pass 结束后，composite/depth 已分别处于 COLOR_ATTACHMENT_OPTIMAL /
    // DEPTH_STENCIL_READ_ONLY_OPTIMAL；粒子 pass 以 load/read-only 方式接管它们。
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
        | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
        | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

    // 粒子 pass 结束后，后处理链通过显式 CompositeToFinalBarrier 采样 composite。
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
        | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
        | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkAttachmentDescription attachments[2] = { colorAttachment, depthAttachment };
    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 2;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 2;
    renderPassInfo.pDependencies = dependencies;

    const VkResult err = vkCreateRenderPass(g_Device, &renderPassInfo, g_Allocator,
                                             &m_ParticleRenderPass);
    check_vk_result(err);
#endif
}

void RenderTarget::CreateParticleFramebuffer()
{
#ifdef __ANDROID__
    return;
#else
    if (m_ParticleRenderPass == VK_NULL_HANDLE || m_CompositeImageView == VK_NULL_HANDLE ||
        m_DepthImageView == VK_NULL_HANDLE) {
        return;
    }

    VkImageView attachments[2] = { m_CompositeImageView, m_DepthImageView };
    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_ParticleRenderPass;
    framebufferInfo.attachmentCount = 2;
    framebufferInfo.pAttachments = attachments;
    framebufferInfo.width = m_Width;
    framebufferInfo.height = m_Height;
    framebufferInfo.layers = 1;

    const VkResult err = vkCreateFramebuffer(g_Device, &framebufferInfo, g_Allocator,
                                              &m_ParticleFramebuffer);
    check_vk_result(err);
#endif
}

void RenderTarget::CreateImGuiDescriptorSet() {
#ifdef __ANDROID__
    // Android：无 ImGui 后端（编辑器未附着，g_EditorActive=false），ImGui_ImplVulkan_AddTexture 访问空 g_Context
    // （fault addr 0x188）直接崩 → 跳过，描述符置空（仅编辑器 SceneView/GameView 面板使用）
    m_DescriptorSet = VK_NULL_HANDLE;
    m_DisplayDescriptorSet = VK_NULL_HANDLE;
    return;
#endif
    // 确保采样器已创建
    GetSampler();
    
    // 使用ImGui的Vulkan后端创建描述符集（只使用主颜色附件）
    if (!m_ColorImageViews.empty() && m_Sampler != VK_NULL_HANDLE) {
        m_DescriptorSet = ImGui_ImplVulkan_AddTexture(m_Sampler, m_ColorImageViews[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    
    // 显示附件（合成后画面）的 ImGui 描述符——编辑器 SceneView/GameView 面板用它
    if (m_DisplayImageView != VK_NULL_HANDLE && m_Sampler != VK_NULL_HANDLE) {
        m_DisplayDescriptorSet = ImGui_ImplVulkan_AddTexture(m_Sampler, m_DisplayImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

// 创建显示附件（链末 tonemap pass 输出；R8G8B8A8——最终显示为 LDR 8bit，链内精度由 composite/中间附件 R16G16B16A16_SFLOAT 保证）
void RenderTarget::CreateDisplayResource()
{
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = m_Width;
    imageInfo.extent.height = m_Height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = m_DisplayFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VkResult err = vkCreateImage(g_Device, &imageInfo, g_Allocator, &m_DisplayImage);
    check_vk_result(err);
    
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(g_Device, m_DisplayImage, &memRequirements);
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    err = vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_DisplayImageMemory);
    check_vk_result(err);
    vkBindImageMemory(g_Device, m_DisplayImage, m_DisplayImageMemory, 0);
    
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_DisplayImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_DisplayFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    
    err = vkCreateImageView(g_Device, &viewInfo, g_Allocator, &m_DisplayImageView);
    check_vk_result(err);
}

VkSampler RenderTarget::GetHiZSampler() {
    if (m_Sampler == VK_NULL_HANDLE && m_Initialized) {
        GetSampler();
    }
    
    if (m_HiZSampler == VK_NULL_HANDLE && m_Initialized) {
        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.mipLodBias = 0.0f;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        
        VkResult err = vkCreateSampler(g_Device, &samplerInfo, g_Allocator, &m_HiZSampler);
        if (err != VK_SUCCESS) {
            std::cerr << "RenderTarget: Failed to create Hi-Z sampler, error: " << err << std::endl;
        }
    }
    return m_HiZSampler;
}

VkSampler RenderTarget::GetSampler() {
    if (m_Sampler == VK_NULL_HANDLE && m_Initialized) {
        // 创建采样器
        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.mipLodBias = 0.0f;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 1.0f;
        
        VkResult err = vkCreateSampler(g_Device, &samplerInfo, g_Allocator, &m_Sampler);
        if (err != VK_SUCCESS) {
            std::cerr << "RenderTarget: Failed to create sampler, error: " << err << std::endl;
        }
    }
    return m_Sampler;
}

void RenderTarget::BeginRender(VkCommandBuffer commandBuffer)
{
    m_CurrentSubpass = 0;

    VkRenderPassBeginInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_RenderPass;
    renderPassInfo.framebuffer = m_Framebuffer;
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = { m_Width, m_Height };
    
    std::vector<VkClearValue> clearValues;
    
    // 主颜色附件
    VkClearValue colorClear = {};
    colorClear.color = { 0.1f, 0.1f, 0.1f, 1.0f };  // 场景背景色
    clearValues.push_back(colorClear);
    
    if (m_UseMRT) {
        // 法线附件 (清除为 (0,0,1,1) - 朝上的法线)
        VkClearValue normalClear = {};
        normalClear.color = { 0.0f, 0.0f, 1.0f, 1.0f };
        clearValues.push_back(normalClear);
        
        VkClearValue materialClear = {};
        materialClear.color = { 0.0f, 0.5f, 1.0f, 0.0f };
        clearValues.push_back(materialClear);
        
        // 运动矢量附件 (附件3——清除为 (0,0,0,0) - 无运动)
        VkClearValue motionVectorClear = {};
        motionVectorClear.color = { 0.0f, 0.0f, 0.0f, 0.0f };
        clearValues.push_back(motionVectorClear);
        
#ifndef __ANDROID__
        // composite 附件（附件4）loadOp=CLEAR——clearValue 按附件索引对齐，避免未定义颜色内容。
        // Android：composite 在独立合成通道（m_CompositeRenderPass），几何 render pass 无此附件——不能 push（clearValueCount 必须==attachmentCount）
        VkClearValue compositeClear = {};
        compositeClear.color = { 0.0f, 0.0f, 0.0f, 1.0f };
        clearValues.push_back(compositeClear);
#endif
    }
    
    // 深度附件（MRT：附件5；非MRT：附件1）——clearValue 必须按附件索引对齐
    VkClearValue depthClear = {};
    depthClear.depthStencil = { 1.0f, 0 };
    clearValues.push_back(depthClear);
    
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();
    
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    
    // 设置视口
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)m_Width;
    viewport.height = (float)m_Height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    
    // 设置裁剪区域
    VkRect2D scissor = {};
    scissor.offset = { 0, 0 };
    scissor.extent = { m_Width, m_Height };
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

void RenderTarget::EndRender(VkCommandBuffer commandBuffer)
{
    vkCmdEndRenderPass(commandBuffer);
}

// 切换到几何/合成阶段。
void RenderTarget::NextSubpass(VkCommandBuffer commandBuffer)
{
    if (!m_UseMRT) return;

    // 统一 MRT geometry render pass 只有一个 subpass：第一次调用
    // 只标记“进入几何阶段”，第二次结束几何 pass 并开始独立 composite
    // pass。这样保留现有 RenderScene/RenderGame 的调用顺序，同时不把
    // G-buffer 延迟到第二个 subpass 才首次使用。
    if (m_CurrentSubpass == 0) {
        ++m_CurrentSubpass;
        return;
    }

    if (m_CurrentSubpass == 1) {
        vkCmdEndRenderPass(commandBuffer);
        BeginCompositeRender(commandBuffer);
        ++m_CurrentSubpass;
    }
}

void RenderTarget::BeginParticleRender(VkCommandBuffer commandBuffer)
{
    if (m_ParticleRenderPass == VK_NULL_HANDLE || m_ParticleFramebuffer == VK_NULL_HANDLE) return;

    VkRenderPassBeginInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_ParticleRenderPass;
    renderPassInfo.framebuffer = m_ParticleFramebuffer;
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = { m_Width, m_Height };
    // color/depth 都是 LOAD：保留主 pass 的 composite 和深度，不提供 clear 值。
    renderPassInfo.clearValueCount = 0;
    renderPassInfo.pClearValues = nullptr;
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {};
    viewport.width = static_cast<float>(m_Width);
    viewport.height = static_cast<float>(m_Height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.extent = { m_Width, m_Height };
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

void RenderTarget::EndParticleRender(VkCommandBuffer commandBuffer)
{
    if (m_ParticleRenderPass == VK_NULL_HANDLE || m_ParticleFramebuffer == VK_NULL_HANDLE) return;
    vkCmdEndRenderPass(commandBuffer);
}

// ===================== 独立合成通道（单 subpass，普通纹理采样 G-Buffer） =====================
// 所有 MRT 平台：几何（单 subpass，G-buffer + depth）与合成（单 subpass，1 颜色 = composite）
// 拆成两个独立 render pass，合成 pass 通过 sampler2D 从 G-Buffer 附件采样做光照。

// 合成 render pass：composite 附件（R16G16B16A16_SFLOAT HDR）+ 几何 depth 只读；单 subpass 输出；
// finalLayout=COLOR_ATTACHMENT_OPTIMAL（与后处理链首个 pass 的采样布局一致）。粒子在同一 pass
// 的透明阶段使用 depth test，写入的 HDR 结果随后继续进入 bloom/TAA/tonemap/FXAA。
void RenderTarget::CreateCompositeRenderPass()
{
    VkAttachmentDescription compositeAttachment = {};
    compositeAttachment.format = m_CompositeFormat;
    compositeAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    compositeAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    compositeAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    compositeAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    compositeAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
#ifdef __ANDROID__
    // Android geometry framebuffer 没有桌面 composite 预留槽，独立 pass 首次使用该图像。
    compositeAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
#else
    // Desktop geometry framebuffer 保留 composite 第五槽，geometry pass 结束时
    // 处于 COLOR_ATTACHMENT_OPTIMAL，独立 pass 从该布局继续。
    compositeAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
#endif
    compositeAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depthAttachment = {};
    depthAttachment.format = m_DepthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    // Android 合成 pass 之后同样进入后处理链，云/GTAO/TAA 需要继续读取深度。
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference compositeColorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depthRef = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &compositeColorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    // EXT→0：几何 pass（外部 render pass）写 G-Buffer/depth 完成 → 合成 pass FS texture 采样读，
    // 同时允许粒子在合成 pass 的 depth test 阶段读取同一深度附件。
    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
        | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
        | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
        | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
        | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

    VkAttachmentDescription attachments[2] = { compositeAttachment, depthAttachment };

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 2;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    VkResult err = vkCreateRenderPass(g_Device, &renderPassInfo, g_Allocator, &m_CompositeRenderPass);
    check_vk_result(err);
}

// 合成通道 framebuffer（[composite, depth]，几何尺寸一致）
void RenderTarget::CreateCompositeFramebuffer()
{
    if (m_CompositeRenderPass == VK_NULL_HANDLE || m_CompositeImageView == VK_NULL_HANDLE ||
        m_DepthImageView == VK_NULL_HANDLE) return;

    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_CompositeRenderPass;
    VkImageView attachments[2] = { m_CompositeImageView, m_DepthImageView };
    framebufferInfo.attachmentCount = 2;
    framebufferInfo.pAttachments = attachments;
    framebufferInfo.width = m_Width;
    framebufferInfo.height = m_Height;
    framebufferInfo.layers = 1;

    VkResult err = vkCreateFramebuffer(g_Device, &framebufferInfo, g_Allocator, &m_CompositeFramebuffer);
    check_vk_result(err);
}

void RenderTarget::BeginCompositeRender(VkCommandBuffer commandBuffer)
{
    if (m_CompositeRenderPass == VK_NULL_HANDLE || m_CompositeFramebuffer == VK_NULL_HANDLE) return;

    VkRenderPassBeginInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_CompositeRenderPass;
    renderPassInfo.framebuffer = m_CompositeFramebuffer;
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = { m_Width, m_Height };
    renderPassInfo.clearValueCount = 2;
    VkClearValue clears[2] = {};
    clears[0].color = { 0.0f, 0.0f, 0.0f, 1.0f };
    // depth attachment 使用 LOAD；该槽位只是与 framebuffer 附件数组对齐，实际值不会清除深度。
    clears[1].depthStencil = { 1.0f, 0 };
    renderPassInfo.pClearValues = clears;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)m_Width;
    viewport.height = (float)m_Height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.offset = { 0, 0 };
    scissor.extent = { m_Width, m_Height };
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

void RenderTarget::EndCompositeRender(VkCommandBuffer commandBuffer)
{
    vkCmdEndRenderPass(commandBuffer);
}

// ===================== 合成 render pass（分离 pass，普通纹理采样 G-Buffer） =====================
// 中间附件（合成 subpass 输出，供后处理链 pass 采样；R16G16B16A16_SFLOAT 线性 HDR——保留高光/暗部精度，避免色带）
void RenderTarget::CreateCompositeImageResource()
{
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = m_Width;
    imageInfo.extent.height = m_Height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = m_CompositeFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult err = vkCreateImage(g_Device, &imageInfo, g_Allocator, &m_CompositeImage);
    check_vk_result(err);

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(g_Device, m_CompositeImage, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    err = vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_CompositeImageMemory);
    check_vk_result(err);
    vkBindImageMemory(g_Device, m_CompositeImage, m_CompositeImageMemory, 0);

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_CompositeImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_CompositeFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    err = vkCreateImageView(g_Device, &viewInfo, g_Allocator, &m_CompositeImageView);
    check_vk_result(err);
}

// final render pass：后处理（黑白滤镜测试）——附件 = 显示附件（编辑器面板采样）
void RenderTarget::CreateFinalRenderPass()
{
    VkAttachmentDescription displayAttachment = {};
    displayAttachment.format = m_DisplayFormat;
    displayAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    displayAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;      // 确定性初始化，避免 AMD 暴露未定义 tile 内容
    displayAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    displayAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    displayAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    displayAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    displayAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependencies[1] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &displayAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = dependencies;

    VkResult err = vkCreateRenderPass(g_Device, &renderPassInfo, g_Allocator, &m_FinalRenderPass);
    check_vk_result(err);

    // ===== UI 叠加 pass（loadOp=LOAD）：链末 tonemap 之后画 UI，保留链结果并 alpha 混合叠加 =====
    // 与 m_FinalRenderPass 同附件/同 framebuffer（显示附件）；loadOp=LOAD 读取链输出
    VkAttachmentDescription uiAttachment = displayAttachment;
    uiAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // 保留链末 tonemap 结果，UI 混合叠加
    // loadOp=LOAD 时 initialLayout 不能 UNDEFINED（否则读未定义布局内容，驱动挂起）——链输出后布局为 SHADER_READ_ONLY，render pass 自动转换
    uiAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    uiAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkSubpassDescription uiSubpass = subpass;   // 同 subpass 0（1 颜色附件）

    VkSubpassDependency uiDeps[1] = {};
    uiDeps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    uiDeps[0].dstSubpass = 0;
    uiDeps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;   // 链末 FS 采样/输出结束
    uiDeps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    uiDeps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    uiDeps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo uiRpInfo = {};
    uiRpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    uiRpInfo.attachmentCount = 1;
    uiRpInfo.pAttachments = &uiAttachment;
    uiRpInfo.subpassCount = 1;
    uiRpInfo.pSubpasses = &uiSubpass;
    uiRpInfo.dependencyCount = 1;
    uiRpInfo.pDependencies = uiDeps;
    err = vkCreateRenderPass(g_Device, &uiRpInfo, g_Allocator, &m_DisplayUIRenderPass);
    check_vk_result(err);
}

void RenderTarget::CreateFinalFramebuffer()
{
    if (m_FinalRenderPass == VK_NULL_HANDLE || m_DisplayImageView == VK_NULL_HANDLE) return;

    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_FinalRenderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &m_DisplayImageView;
    framebufferInfo.width = m_Width;
    framebufferInfo.height = m_Height;
    framebufferInfo.layers = 1;

    VkResult err = vkCreateFramebuffer(g_Device, &framebufferInfo, g_Allocator, &m_FinalFramebuffer);
    check_vk_result(err);
}

void RenderTarget::BeginFinalRender(VkCommandBuffer commandBuffer)
{
    if (m_FinalRenderPass == VK_NULL_HANDLE || m_FinalFramebuffer == VK_NULL_HANDLE) return;

    VkRenderPassBeginInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_FinalRenderPass;
    renderPassInfo.framebuffer = m_FinalFramebuffer;
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = { m_Width, m_Height };
    renderPassInfo.clearValueCount = 1;
    VkClearValue clear = {};
    clear.color = { 0.0f, 0.0f, 0.0f, 1.0f };
    renderPassInfo.pClearValues = &clear;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {};
    viewport.width = (float)m_Width;
    viewport.height = (float)m_Height;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.extent = { m_Width, m_Height };
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

void RenderTarget::EndFinalRender(VkCommandBuffer commandBuffer)
{
    vkCmdEndRenderPass(commandBuffer);
}
