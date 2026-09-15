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
