#include "FullscreenQuad.h"
#include "EngineGlobal.h"
#include "EngineConfig.h"
#include "Log.h"

#include <vector>
#include <string>
#include <cstring>
#include <fstream>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_filesystem.h>
#include <iostream>

// G-Buffer 附件引用布局（descriptor imageLayout 必须与附件实际布局一致）：
// Android（分离合成通道，单 subpass 普通纹理采样 G-Buffer）：合成 pass 通过 sampler2D 读 G-Buffer，
//   几何 pass finalLayout=SHADER_READ_ONLY_OPTIMAL（颜色/法线/材质）/ DEPTH_STENCIL_READ_ONLY_OPTIMAL（深度）。
//   注意：Adreno 驱动对"多 subpass + input attachment"的 vkCreateRenderPass 直接 SIGSEGV，故不在此 pass 用 input attachment。
// 桌面（三 subpass input attachment）：READ_ONLY_OPTIMAL（与 RenderTarget.cpp CreateRenderPass 一致）。
#ifdef __ANDROID__
constexpr VkImageLayout kGBufferImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
#else
constexpr VkImageLayout kGBufferImageLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
#endif

static std::vector<char> readFile(const std::string& filename) {
    std::string fullPath = EngineConfig::ResolvePlatformPath(filename);

    SDL_IOStream* io = SDL_IOFromFile(fullPath.c_str(), "rb");
    if (io == nullptr) {
        return {};
    }

    Sint64 fileSize = SDL_GetIOSize(io);
    
    if (fileSize <= 0) {
        SDL_CloseIO(io);
        return {};
    }

    // 检查文件大小是否合理
    if (fileSize > 1024 * 1024) { // 1MB
        SDL_CloseIO(io);
        return {};
    }

    std::vector<char> buffer((size_t)fileSize);
    
    if (SDL_ReadIO(io, buffer.data(), (size_t)fileSize) != (size_t)fileSize) {
        SDL_CloseIO(io);
        return {};
    }

    SDL_CloseIO(io);
    return buffer;
}

static VkShaderModule createShaderModule(const std::vector<char>& code) {
    if (code.empty()) {
        return VK_NULL_HANDLE;
    }
    
    if (code.size() % 4 != 0) {
        return VK_NULL_HANDLE;
    }
    
    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
    
    VkShaderModule shaderModule;
    VkResult err = vkCreateShaderModule(g_Device, &createInfo, g_Allocator, &shaderModule);
    if (err != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return shaderModule;
}

FullscreenQuad::FullscreenQuad() {
}

FullscreenQuad::~FullscreenQuad() {
    Cleanup();
}

void FullscreenQuad::Init(VkRenderPass renderPass, uint32_t subpass, const char* fragShaderName) {
    if (m_Initialized) {
        Cleanup();
    }
    m_FragShaderName = fragShaderName ? fragShaderName : "fullscreen.frag.spv";
    
    CreateDescriptorSetLayout();
    CreateDescriptorPool();
    CreatePipeline(renderPass, subpass);
    CreateDescriptorSet();
    
    // 诊断：管线创建失败时合成会静默失效（画面显示未合成的 G-Buffer）
    if (m_Pipeline == VK_NULL_HANDLE) {
        fprintf(stderr, "[FullscreenQuad] WARNING: pipeline creation failed (renderPass=%p subpass=%u) — composite disabled\n",
            (void*)renderPass, subpass);
    }
    
    // 占位采样器（天空 RT 未初始化时使用，线性 + clamp）
    if (m_FallbackSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.maxLod = 1.0f;
        vkCreateSampler(g_Device, &samplerInfo, g_Allocator, &m_FallbackSampler);
    }
    
    m_Initialized = true;
}

void FullscreenQuad::Cleanup() {
    if (!m_Initialized) {
        return;
    }
    
    extern VkDevice g_Device;
    extern VkAllocationCallbacks* g_Allocator;
    bool deviceValid = (g_Device != VK_NULL_HANDLE);
    
    if (deviceValid) {
        vkDeviceWaitIdle(g_Device);
    }
    
    if (m_Pipeline != VK_NULL_HANDLE && deviceValid) {
        vkDestroyPipeline(g_Device, m_Pipeline, g_Allocator);
        m_Pipeline = VK_NULL_HANDLE;
    }
    
    if (m_PipelineLayout != VK_NULL_HANDLE && deviceValid) {
        vkDestroyPipelineLayout(g_Device, m_PipelineLayout, g_Allocator);
        m_PipelineLayout = VK_NULL_HANDLE;
    }
    
    if (m_DescriptorPool != VK_NULL_HANDLE && deviceValid) {
        vkDestroyDescriptorPool(g_Device, m_DescriptorPool, g_Allocator);
        m_DescriptorPool = VK_NULL_HANDLE;
    }
    
    if (m_FallbackSampler != VK_NULL_HANDLE && deviceValid) {
        vkDestroySampler(g_Device, m_FallbackSampler, g_Allocator);
        m_FallbackSampler = VK_NULL_HANDLE;
    }
    
    if (m_DescriptorSetLayout != VK_NULL_HANDLE && deviceValid) {
        vkDestroyDescriptorSetLayout(g_Device, m_DescriptorSetLayout, g_Allocator);
        m_DescriptorSetLayout = VK_NULL_HANDLE;
    }
    
    m_Initialized = false;
}

void FullscreenQuad::CreateDescriptorSetLayout() {
    // 采样方式：binding 0/1/3/4 = G-Buffer（合成 pass 采样）
    //   Android（分离合成通道）：sampler2D texture 采样（composite pass 是独立 render pass，无 input attachment）
    //   桌面：COMBINED_IMAGE_SAMPLER（texture 采样；本机 NVIDIA 驱动 subpassLoad 返回 0）
    // binding 2：天空 RT 采样（外部图像，普通纹理上采样）
    // binding 4：2026-08 材质附件（xyz=metallic/roughness/ao，w=自发光强度）
    // binding 5：2026-08-11 银河全景（end_sky——LogLuv32 编码——普通纹理采样）
    // binding 6：2026-08-11 透射率 LUT（合成 pass 官方物理太阳——transmittance 查询）
    // binding 7：2026-08-11 per-pixel 散射 LUT（GetSkyRadiance）
    // binding 8：2026-08-12 IBL 天空环境 cubemap（samplerCube——各向同性）
    // binding 9：2026-08-12 辐照度图（diffuse IBL——半球积分）
    // binding 10：2026-08-12 SH 辐照度系数 UBO
    // binding 11：2026-08-13 点光源数组 UBO（position_range/color_intensity × 32 + count）
    // binding 12：2026-08-13 cluster grid SSBO（params + Cluster[3456]）
    // binding 13：2026-08-13 点光源阴影 cubemap 数组（samplerCubeArray，PCF 手动比较）
    // binding 14：2026-08-14 CSM 方向光阴影 2D array（sampler2DArray，级联 PCF 手动比较）
    // binding 15：2026-08-14 CSM 级联 UBO（shadowMatrices[4] + splitDepths + params）
    VkDescriptorSetLayoutBinding bindings[17] = {};
    for (int i = 0; i < 10; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;   // 全平台 texture 采样（Android 分离合成通道 / 桌面 IMR）
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[i].pImmutableSamplers = nullptr;
    }
    
    bindings[10].binding = 10;
    bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[10].descriptorCount = 1;
    bindings[10].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    bindings[11].binding = 11;   // 2026-08-13 点光源数组 UBO
    bindings[11].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[11].descriptorCount = 1;
    bindings[11].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    bindings[12].binding = 12;   // 2026-08-13 cluster grid SSBO
    bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[12].descriptorCount = 1;
    bindings[12].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    bindings[13].binding = 13;   // 2026-08-13 阴影 cubemap 数组
    bindings[13].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[13].descriptorCount = 1;
    bindings[13].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[14].binding = 14;   // 2026-08-14 CSM 阴影 2D array
    bindings[14].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[14].descriptorCount = 1;
    bindings[14].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[15].binding = 15;   // 2026-08-14 CSM 级联 UBO
    bindings[15].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[15].descriptorCount = 1;
    bindings[15].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[16].binding = 16;   // 2026-08-15 split-sum BRDF LUT
    bindings[16].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[16].descriptorCount = 1;
    bindings[16].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 17;
    layoutInfo.pBindings = bindings;
    
    VkResult err = vkCreateDescriptorSetLayout(g_Device, &layoutInfo, g_Allocator, &m_DescriptorSetLayout);
    check_vk_result(err);
}

void FullscreenQuad::CreateDescriptorPool() {
    VkDescriptorPoolSize poolSizes[5] = {};   // 2026-08-13：+SSBO（3）+阴影 cubemap（4）
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    poolSizes[0].descriptorCount = 16;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 80;   // 2026-08-12 +skyIrradiance（9）——16 sets×11 bindings；2026-08-14 +CSM array（14）
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[2].descriptorCount = 48;   // 2026-08-13 +点光源数组 UBO（binding 11）；2026-08-14 +CSM 级联 UBO（binding 15）
    poolSizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[3].descriptorCount = 16;   // 2026-08-13 +cluster grid SSBO（binding 12）
    poolSizes[4].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[4].descriptorCount = 32;   // 2026-08-13 +阴影 cubemap 数组（binding 13）

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 16;
    poolInfo.poolSizeCount = 5;
    poolInfo.pPoolSizes = poolSizes;

    VkResult err = vkCreateDescriptorPool(g_Device, &poolInfo, g_Allocator, &m_DescriptorPool);
    check_vk_result(err);
}

void FullscreenQuad::CreatePipeline(VkRenderPass renderPass, uint32_t subpass) {
    auto vertexShaderCode = readFile(EngineConfig::GetShaderPath("fullscreen.vert.spv"));
    auto fragmentShaderCode = readFile(EngineConfig::GetShaderPath(m_FragShaderName.c_str()));
    
    if (vertexShaderCode.empty()) {
        return;
    }
    if (fragmentShaderCode.empty()) {
        return;
    }
    
    VkShaderModule vertexShaderModule = createShaderModule(vertexShaderCode);
    VkShaderModule fragmentShaderModule = createShaderModule(fragmentShaderCode);
    
    if (vertexShaderModule == VK_NULL_HANDLE) {
        return;
    }
    if (fragmentShaderModule == VK_NULL_HANDLE) {
        if (vertexShaderModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(g_Device, vertexShaderModule, g_Allocator);
        }
        return;
    }
    
    VkPipelineShaderStageCreateInfo vertexShaderStageInfo = {};
    vertexShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertexShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertexShaderStageInfo.module = vertexShaderModule;
    vertexShaderStageInfo.pName = "main";
    
    VkPipelineShaderStageCreateInfo fragmentShaderStageInfo = {};
    fragmentShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragmentShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragmentShaderStageInfo.module = fragmentShaderModule;
    fragmentShaderStageInfo.pName = "main";
    
    VkPipelineShaderStageCreateInfo shaderStages[] = {vertexShaderStageInfo, fragmentShaderStageInfo};
    
    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 0;
    vertexInputInfo.vertexAttributeDescriptionCount = 0;
    
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    inputAssembly.primitiveRestartEnable = VK_FALSE;
    
    // 6. 视口和裁剪
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(g_MainWindowData.Width);
    viewport.height = static_cast<float>(g_MainWindowData.Height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    
    VkRect2D scissor = {};
    scissor.offset = { 0, 0 };
    scissor.extent.width = static_cast<uint32_t>(g_MainWindowData.Width);
    scissor.extent.height = static_cast<uint32_t>(g_MainWindowData.Height);
    
    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    
    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = dynamicStates.size();
    dynamicState.pDynamicStates = dynamicStates.data();
    
    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    
    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    // 9. 深度和模板
    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;
    
    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;
    
    VkPipelineColorBlendStateCreateInfo colorBlending = {};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    
    if (m_DescriptorSetLayout == VK_NULL_HANDLE) {
        vkDestroyShaderModule(g_Device, vertexShaderModule, g_Allocator);
        vkDestroyShaderModule(g_Device, fragmentShaderModule, g_Allocator);
        return;
    }
    
    VkPushConstantRange pushConstantRange = {};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(glm::mat4) * 3 + 4 * sizeof(glm::vec4);   // invViewProj+cameraPos+sunDir+lightColor+invProj+invView（256B——2026-08-11 高海拔精度全链路；⚠️ 超 Vulkan 128B 最低保证，桌面/主流 GPU OK）
    
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_DescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    
    VkResult err = vkCreatePipelineLayout(g_Device, &pipelineLayoutInfo, g_Allocator, &m_PipelineLayout);
    if (err != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, vertexShaderModule, g_Allocator);
        vkDestroyShaderModule(g_Device, fragmentShaderModule, g_Allocator);
        return;
    }
    
    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_PipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = subpass;
    
    err = vkCreateGraphicsPipelines(g_Device, VK_NULL_HANDLE, 1, &pipelineInfo, g_Allocator, &m_Pipeline);
    if (err != VK_SUCCESS) {
        vkDestroyPipelineLayout(g_Device, m_PipelineLayout, g_Allocator);
        m_PipelineLayout = VK_NULL_HANDLE;
        vkDestroyShaderModule(g_Device, vertexShaderModule, g_Allocator);
        vkDestroyShaderModule(g_Device, fragmentShaderModule, g_Allocator);
        return;
    }
    
    // 清理
    vkDestroyShaderModule(g_Device, vertexShaderModule, g_Allocator);
    vkDestroyShaderModule(g_Device, fragmentShaderModule, g_Allocator);
}

void FullscreenQuad::CreateDescriptorSet() {
    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_DescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_DescriptorSetLayout;
    
    VkResult err = vkAllocateDescriptorSets(g_Device, &allocInfo, &m_DescriptorSet);
    check_vk_result(err);
}

void FullscreenQuad::UpdateDescriptorSet(VkImageView colorInputView, VkImageView depthInputView, VkImageView skyImageView, VkSampler skySampler, VkImageView normalView, VkImageView materialView, VkImageView galaxyView, VkImageView transmittanceView, VkImageView scatteringView, VkImageView skyCubeView, VkSampler skyCubeSampler, VkImageView skyIrradianceView, VkSampler skyIrradianceSampler, VkBuffer shBuffer, VkBuffer pointLightBuffer, VkBuffer clusterGridBuffer, VkImageView shadowCubeView, VkImageView csmView, VkSampler shadowSampler, VkBuffer csmBuffer, VkImageView brdfLutView, VkSampler brdfLutSampler) {
    // 如果输入都没有变化，跳过更新
    if (m_CachedImageView == colorInputView && m_CachedDepthView == depthInputView
        && m_CachedSkyView == skyImageView && m_CachedSkySampler == skySampler
        && m_CachedNormalView == normalView && m_CachedMaterialView == materialView
        && m_CachedGalaxyView == galaxyView && m_CachedTransmittanceView == transmittanceView
        && m_CachedScatteringView == scatteringView
        && m_CachedSkyCubeView == skyCubeView && m_CachedSkyCubeSampler == skyCubeSampler   // 2026-08-12
        && m_CachedSkyIrradianceView == skyIrradianceView && m_CachedSkyIrradianceSampler == skyIrradianceSampler
        && m_CachedShIrradianceBuffer == shBuffer
        && m_CachedPointLightBuffer == pointLightBuffer   // 2026-08-13
        && m_CachedClusterGridBuffer == clusterGridBuffer   // 2026-08-13
        && m_CachedShadowCubeView == shadowCubeView   // 2026-08-13
        && m_CachedCsmView == csmView && m_CachedCsmSampler == shadowSampler && m_CachedCsmBuffer == csmBuffer   // 2026-08-14
        && m_CachedBrdfLutView == brdfLutView && m_CachedBrdfLutSampler == brdfLutSampler   // 2026-08-15
        && m_DescriptorSet != VK_NULL_HANDLE) {
        return;
    }
    m_CachedImageView = colorInputView;
    m_CachedDepthView = depthInputView;
    m_CachedSkyView = skyImageView;
    m_CachedSkySampler = skySampler;
    m_CachedNormalView = normalView;
    m_CachedMaterialView = materialView;
    m_CachedGalaxyView = galaxyView;
    m_CachedTransmittanceView = transmittanceView;
    m_CachedScatteringView = scatteringView;
    m_CachedSkyCubeView = skyCubeView;   // 2026-08-12
    m_CachedSkyCubeSampler = skyCubeSampler;
    m_CachedSkyIrradianceView = skyIrradianceView;   // 2026-08-12
    m_CachedSkyIrradianceSampler = skyIrradianceSampler;
    m_CachedShIrradianceBuffer = shBuffer;   // 2026-08-12
    m_CachedPointLightBuffer = pointLightBuffer;   // 2026-08-13
    m_CachedClusterGridBuffer = clusterGridBuffer;   // 2026-08-13
    m_CachedShadowCubeView = shadowCubeView;   // 2026-08-13
    m_CachedCsmView = csmView;   // 2026-08-14
    m_CachedCsmSampler = shadowSampler;
    m_CachedBrdfLutView = brdfLutView;   // 2026-08-15
    m_CachedBrdfLutSampler = brdfLutSampler;
    m_CachedCsmBuffer = csmBuffer;
    
    // 天空 RT 未初始化时用颜色0 占位（2D 场景深度判据恒 false，不会实际采样）；
    // 颜色0/深度/法线/材质为 G-Buffer（COMBINED_IMAGE_SAMPLER texture 采样）——descriptor 带 sampler，
    // imageLayout 与附件实际布局一致（颜色/法线/材质 SHADER_READ_ONLY、深度 DEPTH_STENCIL_READ_ONLY）
    VkImageView effSkyView = (skyImageView != VK_NULL_HANDLE) ? skyImageView : colorInputView;
    VkSampler effSkySampler = (skySampler != VK_NULL_HANDLE) ? skySampler : m_FallbackSampler;
    VkImageView effNormalView = (normalView != VK_NULL_HANDLE) ? normalView : colorInputView;   // 法线未绑定时占位
    VkImageView effMaterialView = (materialView != VK_NULL_HANDLE) ? materialView : colorInputView;   // 材质未绑定时占位（同色）
    VkImageView effGalaxyView = (galaxyView != VK_NULL_HANDLE) ? galaxyView : colorInputView;   // 银河未绑定时占位（引擎资产强制，正常必有）
    VkImageView effScatteringView = (scatteringView != VK_NULL_HANDLE) ? scatteringView : colorInputView;   // per-pixel 散射 LUT（大气未初始化时占位）
    
    VkDescriptorImageInfo infoColor = {};
    infoColor.imageLayout = kGBufferImageLayout;   // G-Buffer 颜色0 实际布局（Android=SHADER_READ_ONLY；桌面=READ_ONLY_OPTIMAL）
    infoColor.imageView = colorInputView;
    VkDescriptorImageInfo infoDepth = {};
    infoDepth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;   // 深度实际布局（采样读）
    infoDepth.imageView = depthInputView;
    VkDescriptorImageInfo infoSky = {};
    infoSky.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infoSky.imageView = effSkyView;
    infoSky.sampler = effSkySampler;
    VkDescriptorImageInfo infoNormal = {};
    infoNormal.imageLayout = kGBufferImageLayout;
    infoNormal.imageView = effNormalView;
    VkDescriptorImageInfo infoMaterial = {};
    infoMaterial.imageLayout = kGBufferImageLayout;
    infoMaterial.imageView = effMaterialView;
    VkDescriptorImageInfo infoGalaxy = {};
    infoGalaxy.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infoGalaxy.imageView = effGalaxyView;
    infoGalaxy.sampler = m_FallbackSampler;   // 银河普通纹理采样（CLAMP/LINEAR——全屏 1:1 LOD0）
    VkDescriptorImageInfo infoTransmittance = {};
    infoTransmittance.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infoTransmittance.imageView = (transmittanceView != VK_NULL_HANDLE) ? transmittanceView : effGalaxyView;
    infoTransmittance.sampler = m_FallbackSampler;   // 透射率 LUT（普通纹理采样；未绑定时占位）
    VkDescriptorImageInfo infoScattering = {};
    infoScattering.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infoScattering.imageView = effScatteringView;
    infoScattering.sampler = m_FallbackSampler;   // 散射 LUT（3D——per-pixel GetSkyRadiance；未绑定时占位）
    VkDescriptorImageInfo infoSkyCube = {};   // 2026-08-12：IBL cubemap（samplerCube——未绑定时占位）
    infoSkyCube.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infoSkyCube.imageView = (skyCubeView != VK_NULL_HANDLE) ? skyCubeView : effGalaxyView;
    infoSkyCube.sampler = (skyCubeSampler != VK_NULL_HANDLE) ? skyCubeSampler : m_FallbackSampler;
    VkDescriptorImageInfo infoSkyIrradiance = {};   // 2026-08-12：辐照度图（未绑定时占位）
    infoSkyIrradiance.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infoSkyIrradiance.imageView = (skyIrradianceView != VK_NULL_HANDLE) ? skyIrradianceView : effGalaxyView;
    infoSkyIrradiance.sampler = (skyIrradianceSampler != VK_NULL_HANDLE) ? skyIrradianceSampler : m_FallbackSampler;
    // G-Buffer 为 COMBINED_IMAGE_SAMPLER（texture 采样）——必须绑定 sampler（全平台一致）
    infoColor.sampler = m_FallbackSampler;
    // 深度采样必须 NEAREST（等价 subpassLoad 逐 texel 读）：Adreno 对 depth 格式 + LINEAR filter 采样返回垃圾 → 天空判定 depth>=0.9999 永假 → 黑屏
    // （桌面 subpassLoad 无 filter 从未暴露；Android 分离合成通道 texture 采样首次触发，2026-08-22 定位）
    infoDepth.sampler = g_TexturePool ? g_TexturePool->GetSamplerByType(SamplerType::NearestClamp) : m_FallbackSampler;
    infoNormal.sampler = m_FallbackSampler;
    infoMaterial.sampler = m_FallbackSampler;
    
    VkWriteDescriptorSet writes[17] = {};   // 2026-08-15：+BRDF LUT（16）
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_DescriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;   // 全平台 texture 采样
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &infoColor;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_DescriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &infoDepth;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_DescriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &infoSky;
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = m_DescriptorSet;
    writes[3].dstBinding = 3;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].descriptorCount = 1;
    writes[3].pImageInfo = &infoNormal;
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = m_DescriptorSet;
    writes[4].dstBinding = 4;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[4].descriptorCount = 1;
    writes[4].pImageInfo = &infoMaterial;
    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = m_DescriptorSet;
    writes[5].dstBinding = 5;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[5].descriptorCount = 1;
    writes[5].pImageInfo = &infoGalaxy;
    writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = m_DescriptorSet;
    writes[6].dstBinding = 6;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[6].descriptorCount = 1;
    writes[6].pImageInfo = &infoTransmittance;
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = m_DescriptorSet;
    writes[7].dstBinding = 7;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[7].descriptorCount = 1;
    writes[7].pImageInfo = &infoScattering;
    writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;   // 2026-08-12：IBL cubemap
    writes[8].dstSet = m_DescriptorSet;
    writes[8].dstBinding = 8;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[8].descriptorCount = 1;
    writes[8].pImageInfo = &infoSkyCube;
    writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;   // 2026-08-12：辐照度图
    writes[9].dstSet = m_DescriptorSet;
    writes[9].dstBinding = 9;
    writes[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[9].descriptorCount = 1;
    writes[9].pImageInfo = &infoSkyIrradiance;
    
    // 2026-08-12：binding 10——SH 辐照度系数 UBO（替代/对比 irradiance 卷积）
    VkDescriptorBufferInfo infoShBuffer = {};
    infoShBuffer.buffer = shBuffer;
    infoShBuffer.offset = 0;
    infoShBuffer.range = 144;   // vec4[9]（RGB×9 系数 + 3 填充）
    writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[10].dstSet = m_DescriptorSet;
    writes[10].dstBinding = 10;
    writes[10].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[10].descriptorCount = 1;
    writes[10].pBufferInfo = &infoShBuffer;
    
    // 2026-08-13：binding 11——点光源数组 UBO（32×32B + count/padding = 1040B）
    VkDescriptorBufferInfo infoPointLight = {};
    infoPointLight.buffer = pointLightBuffer;
    infoPointLight.offset = 0;
    infoPointLight.range = 1552;   // 32×(16×3) + 16（GpuPointLight 48B×3 vec4 + count）
    writes[11].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[11].dstSet = m_DescriptorSet;
    writes[11].dstBinding = 11;
    writes[11].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[11].descriptorCount = 1;
    writes[11].pBufferInfo = &infoPointLight;
    
    // 2026-08-13：binding 12——cluster grid SSBO（params 16B + Cluster[3456]×168B）
    VkDescriptorBufferInfo infoClusterGrid = {};
    infoClusterGrid.buffer = clusterGridBuffer;
    infoClusterGrid.offset = 0;
    infoClusterGrid.range = 16 + 3456 * 168;
    writes[12].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[12].dstSet = m_DescriptorSet;
    writes[12].dstBinding = 12;
    writes[12].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[12].descriptorCount = 1;
    writes[12].pBufferInfo = &infoClusterGrid;
    
    // 2026-08-13：binding 13——阴影 cubemap 数组；2026-08-15 改用阴影比较采样器（shader 已改 samplerCubeArrayShadow，
    // ⚠️ sampler 必须是 compare 采样器（普通 sampler 绑定 shadow sampler 类型 = validation error）；NULL 时 fallback ShadowCompare
    VkDescriptorImageInfo infoShadowCube = {};
    infoShadowCube.sampler = shadowSampler != VK_NULL_HANDLE ? shadowSampler : g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare);
    infoShadowCube.imageView = shadowCubeView;
    infoShadowCube.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    writes[13].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[13].dstSet = m_DescriptorSet;
    writes[13].dstBinding = 13;
    writes[13].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[13].descriptorCount = 1;
    writes[13].pImageInfo = &infoShadowCube;

    // 2026-08-14：binding 14——CSM 阴影 2D array（sampler 独立传入；null 时占位同 shadowCubeView）
    VkDescriptorImageInfo infoCsm = {};
    infoCsm.sampler = shadowSampler != VK_NULL_HANDLE ? shadowSampler : g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare);
    infoCsm.imageView = csmView != VK_NULL_HANDLE ? csmView : (shadowCubeView != VK_NULL_HANDLE ? shadowCubeView : effGalaxyView);
    infoCsm.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    writes[14].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[14].dstSet = m_DescriptorSet;
    writes[14].dstBinding = 14;
    writes[14].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[14].descriptorCount = 1;
    writes[14].pImageInfo = &infoCsm;

    // 2026-08-14：binding 15——CSM 级联 UBO（shadowMatrices[4] + splitDepths + params）
    VkDescriptorBufferInfo infoCsmUbo = {};
    infoCsmUbo.buffer = csmBuffer;
    infoCsmUbo.offset = 0;
    infoCsmUbo.range = 288;   // 4×64B + 2×16B（std140）
    writes[15].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[15].dstSet = m_DescriptorSet;
    writes[15].dstBinding = 15;
    writes[15].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[15].descriptorCount = 1;
    writes[15].pBufferInfo = &infoCsmUbo;

    // 2026-08-15：binding 16——split-sum BRDF LUT（Fermion 移植；view NULL 时 fallback galaxyView 占位——shader 只在 IBL 采样）
    VkDescriptorImageInfo infoBrdf = {};
    infoBrdf.sampler = brdfLutSampler != VK_NULL_HANDLE ? brdfLutSampler : g_TexturePool->GetSamplerByType(SamplerType::Linear);
    infoBrdf.imageView = brdfLutView != VK_NULL_HANDLE ? brdfLutView : (galaxyView != VK_NULL_HANDLE ? galaxyView : effGalaxyView);
    infoBrdf.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    writes[16].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[16].dstSet = m_DescriptorSet;
    writes[16].dstBinding = 16;
    writes[16].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[16].descriptorCount = 1;
    writes[16].pImageInfo = &infoBrdf;

    vkUpdateDescriptorSets(g_Device, 17, writes, 0, nullptr);
}

void FullscreenQuad::Render(VkCommandBuffer commandBuffer, int width, int height, const glm::mat4& invViewProj, const glm::vec3& cameraPos, const glm::vec3& sunDir, const glm::mat4& proj, const glm::mat4& view, const glm::vec4& lightColor) {
    // ⚠️ 2026-08-14 一次性诊断：确认 proj/invViewProj 深度约定（直传 vs *2-1 之谜）
    {
        static bool s_pcLogged = false;
        if (!s_pcLogged) {
            s_pcLogged = true;
            LOGI("[PC] proj: p22=%.4f p23=%.4f p32=%.4f p33=%.4f", proj[2][2], proj[2][3], proj[3][2], proj[3][3]);
            LOGI("[PC] invViewProj: m22=%.4f m23=%.4f m32=%.4f m33=%.4f", invViewProj[2][2], invViewProj[2][3], invViewProj[3][2], invViewProj[3][3]);
            glm::mat4 ip = glm::inverse(proj);
            LOGI("[PC] invProj: i22=%.4f i23=%.4f i32=%.4f i33=%.4f", ip[2][2], ip[2][3], ip[3][2], ip[3][3]);
            glm::mat4 prod = proj * view * invViewProj;   // 应 ≈ 单位阵（验证矩阵链数学一致）
            LOGI("[PC] verify proj*view*invViewProj: diag=(%.4f, %.4f, %.4f, %.4f)", prod[0][0], prod[1][1], prod[2][2], prod[3][3]);
            LOGI("[PC] view: row2=(%.4f,%.4f,%.4f,%.4f) row3=(%.4f,%.4f,%.4f,%.4f)",
                 view[0][2], view[1][2], view[2][2], view[3][2],
                 view[0][3], view[1][3], view[2][3], view[3][3]);
        }
    }
    if (!m_Initialized) {
        return;
    }
    if (m_Pipeline == VK_NULL_HANDLE) {   // pipeline 创建失败时禁止绑定 NULL（合成静默跳过，避免 VUID/崩溃）
        return;
    }
    
    // 重建视线方向所需的逆投影视图矩阵 + 相机位置（视线 = 远平面点 - 相机位置；各自视口相机）+ 太阳方向（全分辨率太阳圆盘/云光照）
    // 2026-08-11 高海拔精度（全链路）：dir = mat3(invView) × (invProj × ndc)——相机空间重建 + 旋转，无大数相减。
    // 世界 dir = invViewProj远平面点 - cameraPos 在相机 1e5 量级时 float32 灾难性抵消 → 方向误差 ~0.2°
    // → 太阳边缘锯齿 + 银河/skyRT 采样错位（断断续续）——现全部走相机空间（投影逆无平移，精度恒好）
    struct PC {
        glm::mat4 invViewProj; glm::vec4 cameraPos; glm::vec4 sunDir; glm::vec4 lightColor;
        glm::mat4 invProj; glm::mat4 invView;
    } pc;
    pc.invViewProj = invViewProj;
    pc.cameraPos = glm::vec4(cameraPos, 1.0f);
    pc.sunDir = glm::vec4(sunDir, 0.0f);
    // ⚠️ 2026-08-11 修复：pc.lightColor 从未赋值 = 栈垃圾 → sunLight 垃圾有色 → PBR 溢色红绿/灰白（用户定位"完全出在 sunLight"）
    pc.lightColor = lightColor;   // 默认 vec4(1,0.96,0.89,1) 近似太阳（调用方未传时）
    pc.invProj = glm::inverse(proj);
    pc.invView = glm::inverse(view);
    vkCmdPushConstants(commandBuffer, m_PipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PC), &pc);
    
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    
    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayout, 0, 1, &m_DescriptorSet, 0, nullptr);
    
    // 使用单三角形绘制全屏（3 个顶点而不是 4 个）
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}
