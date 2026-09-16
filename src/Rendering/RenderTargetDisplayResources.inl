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
            LOGSTREAM(Error) << "RenderTarget: Failed to create Hi-Z sampler, error: " << err << std::endl;
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
            LOGSTREAM(Error) << "RenderTarget: Failed to create sampler, error: " << err << std::endl;
        }
    }
    return m_Sampler;
}
