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
