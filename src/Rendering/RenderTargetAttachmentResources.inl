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

void RenderTarget::CreateHiZOccluderResources()
{
    if (!m_UseMRT || m_HiZOccluderFormat == VK_FORMAT_UNDEFINED) {
        return;
    }

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = m_Width;
    imageInfo.extent.height = m_Height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = m_HiZOccluderFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult err = vkCreateImage(g_Device, &imageInfo, g_Allocator, &m_HiZOccluderImage);
    check_vk_result(err);

    VkMemoryRequirements memRequirements{};
    vkGetImageMemoryRequirements(g_Device, m_HiZOccluderImage, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    err = vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_HiZOccluderImageMemory);
    check_vk_result(err);
    vkBindImageMemory(g_Device, m_HiZOccluderImage, m_HiZOccluderImageMemory, 0);

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_HiZOccluderImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_HiZOccluderFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    err = vkCreateImageView(g_Device, &viewInfo, g_Allocator, &m_HiZOccluderImageView);
    check_vk_result(err);
}

void RenderTarget::DestroyHiZOccluderResources()
{
    if (m_HiZOccluderImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(g_Device, m_HiZOccluderImageView, g_Allocator);
        m_HiZOccluderImageView = VK_NULL_HANDLE;
    }
    if (m_HiZOccluderImage != VK_NULL_HANDLE) {
        vkDestroyImage(g_Device, m_HiZOccluderImage, g_Allocator);
        m_HiZOccluderImage = VK_NULL_HANDLE;
    }
    if (m_HiZOccluderImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_HiZOccluderImageMemory, g_Allocator);
        m_HiZOccluderImageMemory = VK_NULL_HANDLE;
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
    
    // 添加独立 Hi-Z 遮挡源（附件4，位于 G-buffer 之后）。
    // 它与主深度分离，草管线通过写掩码不写入该附件。
    if (m_UseMRT && m_HiZOccluderImageView != VK_NULL_HANDLE) {
        attachments.push_back(m_HiZOccluderImageView);
    }

    // 添加合成 subpass 输出附件（桌面 MRT：composite view 位于 Hi-Z 源之后、深度之前）
    // Android：composite 在独立合成通道，几何 framebuffer 不含它
#ifndef __ANDROID__
    if (m_UseMRT && m_CompositeImageView != VK_NULL_HANDLE) {
        attachments.push_back(m_CompositeImageView);
    }
#endif
    
    // 添加深度附件（MRT：Android 附件5，桌面附件6；非 MRT 时附件1）
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
