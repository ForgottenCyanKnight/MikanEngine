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

    if (m_UseMRT) {
        // Hi-Z 的遮挡源是颜色附件中的归一化 [0,1] 深度值。优先使用
        // 单通道浮点格式；如果设备没有单通道颜色附件能力，则回退到
        // 引擎已经用于 G-buffer/Hi-Z 中的常见浮点格式。
        m_HiZOccluderFormat = SelectCompatibleFormat(
            g_PhysicalDevice,
            { VK_FORMAT_R32_SFLOAT,
              VK_FORMAT_R16_SFLOAT,
              VK_FORMAT_R32G32B32A32_SFLOAT,
              VK_FORMAT_R16G16B16A16_SFLOAT,
              VK_FORMAT_R8_UNORM,
              mainColorFormat },
            VK_IMAGE_TILING_OPTIMAL,
            colorAttachmentFeatures);
        if (m_HiZOccluderFormat == VK_FORMAT_UNDEFINED) {
            // 主颜色附件本身已经是该 render pass 的有效颜色格式；这里只
            // 作为极端设备上的最后回退，保证 geometry pass 拓扑仍完整。
            m_HiZOccluderFormat = mainColorFormat;
        }
        LOGI("[RenderTarget] Hi-Z occluder format: %d", static_cast<int>(m_HiZOccluderFormat));
    } else {
        m_HiZOccluderFormat = VK_FORMAT_UNDEFINED;
    }
    
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

        // 独立 Hi-Z 遮挡源：清为最远深度 1.0；只有地形和静态模型
        // 管线打开附件4的颜色写掩码，草/体素/世界层保持关闭。
        VkAttachmentDescription hizOccluderAttachment = {};
        hizOccluderAttachment.format = m_HiZOccluderFormat;
        hizOccluderAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        hizOccluderAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        hizOccluderAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        hizOccluderAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        hizOccluderAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        hizOccluderAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        hizOccluderAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        const uint32_t hizOccluderIndex = static_cast<uint32_t>(attachments.size());
        attachments.push_back(hizOccluderAttachment);
        colorAttachmentRefs.push_back({ hizOccluderIndex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
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
        LOGW("[RenderTarget] no depth format supports attachment + sampled usage");
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
    LOGI("[RenderTarget] depth format: %s (attachment + sampled)", depthFormatName(depthFormat));

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
    // 注意：depth push 在 MRT 的 Hi-Z 源和 desktop composite 之后（附件6 桌面、附件5 Android / 附件1 非MRT）。
    // composite 必须排在 depth 之前。
    
    // Desktop MRT：composite 附件先于 depth push。这个槽位仍然保留在
    // geometry framebuffer 中，但真正的 composite 在独立 pass 完成。
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
        attachments.push_back(compositeAttachment);
        compositeIndex = static_cast<uint32_t>(attachments.size() - 1);

        // 几何管线的 composite 颜色槽仍由写掩码关闭，但必须在同一个 geometry
        // subpass 中声明，确保附件的 CLEAR/STORE 生命周期完整。
        colorAttachmentRefs.push_back({ compositeIndex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
    }
#endif
    
    // depth 附件 push（MRT：桌面附件6 / Android 附件5；非 MRT：附件1）
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
