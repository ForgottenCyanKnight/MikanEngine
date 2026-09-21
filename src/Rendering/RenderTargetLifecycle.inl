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
        LOGI("[RenderTarget] MRT: geometry render pass + separate composite render pass");
    }

#ifdef __ANDROID__
    // Adreno 对 B10G11R11_UFLOAT_PACK32 作为 composite render-pass 附件
    // 支持不稳，独立 composite pass 使用最通用的 R16G16B16A16_SFLOAT。
    m_CompositeFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
#endif
    
    CreateRenderPass();
    CreateColorResources();
    CreateHiZOccluderResources();
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

    DestroyHiZOccluderResources();
    
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

    DestroyHiZOccluderResources();
    
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
    CreateHiZOccluderResources();
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
