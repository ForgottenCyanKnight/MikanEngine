#include "Rendering/PostProcessQuad.h"
#include "Core/EngineConfig.h"
#include "Core/EngineGlobal.h"
#include "Core/VulkanContext.h"

#include <glm/glm.hpp>
#include <iostream>
#include <fstream>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_filesystem.h>

// 内存类型查找（与 RenderTarget.cpp 同款，文件内 static）
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

// 读取 shader 二进制（与 FullscreenQuad 同款；SDL_IOFromFile 在 Android 自动 fallback APK assets，
// 不能用 std::ifstream——APK assets 不是文件系统，2026-08-23 修复链末 pass pipeline 黑屏）
static std::vector<char> readFile(const std::string& filename) {
    std::string fullPath = EngineConfig::ResolvePlatformPath(filename);

    SDL_IOStream* io = SDL_IOFromFile(fullPath.c_str(), "rb");
    if (io == nullptr) {
        fprintf(stderr, "[PostProcessQuad] Failed to open shader: %s\n", filename.c_str());
        return {};
    }

    Sint64 fileSize = SDL_GetIOSize(io);
    if (fileSize <= 0) {
        SDL_CloseIO(io);
        return {};
    }

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
    if (code.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module;
    if (vkCreateShaderModule(g_Device, &createInfo, g_Allocator, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

PostProcessQuad::~PostProcessQuad() { Cleanup(); }

void PostProcessQuad::Cleanup()
{
    if (!m_Initialized && m_Pipeline == VK_NULL_HANDLE) return;

    if (m_Pipeline != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE) {
        vkDestroyPipeline(g_Device, m_Pipeline, g_Allocator);
        m_Pipeline = VK_NULL_HANDLE;
    }
    if (m_PipelineLayout != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(g_Device, m_PipelineLayout, g_Allocator);
        m_PipelineLayout = VK_NULL_HANDLE;
    }
    if (m_DescriptorPool != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(g_Device, m_DescriptorPool, g_Allocator);
        m_DescriptorPool = VK_NULL_HANDLE;
    }
    if (m_DescriptorSetLayout != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(g_Device, m_DescriptorSetLayout, g_Allocator);
        m_DescriptorSetLayout = VK_NULL_HANDLE;
    }
    // Camera UBO 清理
    if (m_CameraUBOMapped) { vkUnmapMemory(g_Device, m_CameraUBOMem); m_CameraUBOMapped = nullptr; }
    if (m_CameraUBO != VK_NULL_HANDLE) { vkDestroyBuffer(g_Device, m_CameraUBO, g_Allocator); m_CameraUBO = VK_NULL_HANDLE; }
    if (m_CameraUBOMem != VK_NULL_HANDLE) { vkFreeMemory(g_Device, m_CameraUBOMem, g_Allocator); m_CameraUBOMem = VK_NULL_HANDLE; }
    for (auto& kv : m_SamplerCache) {
        if (kv.second != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE) {
            vkDestroySampler(g_Device, kv.second, g_Allocator);
        }
    }
    m_SamplerCache.clear();
    m_CachedInputs.clear();
    m_Initialized = false;
}

void PostProcessQuad::Init(VkRenderPass renderPass, uint32_t subpass, const char* fragShaderName, uint32_t maxInputs)
{
    if (m_Initialized) Cleanup();
    m_FragShaderName = fragShaderName ? fragShaderName : "filter.frag.spv";
    // Most passes keep the historical UBO at binding 8. A pass that needs an
    // additional texture may opt into binding 9+ and place its UBO after those
    // texture slots; the shader and this descriptor layout then stay aligned.
    m_MaxInputs = (maxInputs > 0 && maxInputs <= 16) ? maxInputs : 8;

    // 创建 Camera UBO（host visible，persistent mapped）
    {
        VkBufferCreateInfo bufInfo = {};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = sizeof(CameraUBO);
        bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(g_Device, &bufInfo, g_Allocator, &m_CameraUBO);
        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(g_Device, m_CameraUBO, &memReq);
        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_CameraUBOMem);
        vkBindBufferMemory(g_Device, m_CameraUBO, m_CameraUBOMem, 0);
        vkMapMemory(g_Device, m_CameraUBOMem, 0, sizeof(CameraUBO), 0, &m_CameraUBOMapped);
        // 写一次零初始化
        CameraUBO zero = {};
        memcpy(m_CameraUBOMapped, &zero, sizeof(CameraUBO));
    }

    CreateDescriptorSetLayout();
    CreateDescriptorPool();
    CreatePipeline(renderPass, subpass);
    CreateDescriptorSet();

    // 写 UBO descriptor（binding = m_MaxInputs）
    {
        VkDescriptorBufferInfo bufInfo = {};
        bufInfo.buffer = m_CameraUBO;
        bufInfo.offset = 0;
        bufInfo.range = sizeof(CameraUBO);
        VkWriteDescriptorSet uboWrite = {};
        uboWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        uboWrite.dstSet = m_DescriptorSet;
        uboWrite.dstBinding = m_MaxInputs;
        uboWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboWrite.descriptorCount = 1;
        uboWrite.pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(g_Device, 1, &uboWrite, 0, nullptr);
    }

    if (m_Pipeline == VK_NULL_HANDLE) {
        fprintf(stderr, "[PostProcessQuad] WARNING: pipeline creation failed (shader=%s) — pass disabled\n", m_FragShaderName.c_str());
    }
    m_Initialized = true;
}

VkSampler PostProcessQuad::GetOrCreateSampler(VkFilter magFilter, VkSamplerAddressMode wrap)
{
    SamplerKey key{ magFilter, wrap };
    for (auto& kv : m_SamplerCache) {
        if (kv.first == key) return kv.second;
    }
    VkSamplerCreateInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = magFilter;
    si.minFilter = magFilter;
    si.addressModeU = wrap;
    si.addressModeV = wrap;
    si.addressModeW = wrap;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.maxLod = 1.0f;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(g_Device, &si, g_Allocator, &s);
    m_SamplerCache.push_back({ key, s });
    return s;
}

void PostProcessQuad::CreateDescriptorSetLayout()
{
    // binding 0..m_MaxInputs-1: COMBINED_IMAGE_SAMPLER（纹理输入）
    // binding m_MaxInputs: UNIFORM_BUFFER（CameraUBO）
    std::vector<VkDescriptorSetLayoutBinding> bindings(m_MaxInputs + 1);
    for (uint32_t i = 0; i < m_MaxInputs; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[i].pImmutableSamplers = nullptr;
    }
    bindings[m_MaxInputs].binding = m_MaxInputs;
    bindings[m_MaxInputs].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[m_MaxInputs].descriptorCount = 1;
    bindings[m_MaxInputs].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[m_MaxInputs].pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = (uint32_t)bindings.size();
    layoutInfo.pBindings = bindings.data();
    VkResult err = vkCreateDescriptorSetLayout(g_Device, &layoutInfo, g_Allocator, &m_DescriptorSetLayout);
    check_vk_result(err);
}

void PostProcessQuad::CreateDescriptorPool()
{
    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = m_MaxInputs * 4;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 4;
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = 4;
    VkResult err = vkCreateDescriptorPool(g_Device, &poolInfo, g_Allocator, &m_DescriptorPool);
    check_vk_result(err);
}

void PostProcessQuad::CreatePipeline(VkRenderPass renderPass, uint32_t subpass)
{
    auto vertexShaderCode = readFile(EngineConfig::GetShaderPath("fullscreen.vert.spv"));
    auto fragmentShaderCode = readFile(EngineConfig::GetShaderPath(m_FragShaderName.c_str()));
    if (vertexShaderCode.empty() || fragmentShaderCode.empty()) return;

    VkShaderModule vertexShaderModule = createShaderModule(vertexShaderCode);
    VkShaderModule fragmentShaderModule = createShaderModule(fragmentShaderCode);
    if (vertexShaderModule == VK_NULL_HANDLE || fragmentShaderModule == VK_NULL_HANDLE) {
        if (vertexShaderModule) vkDestroyShaderModule(g_Device, vertexShaderModule, g_Allocator);
        if (fragmentShaderModule) vkDestroyShaderModule(g_Device, fragmentShaderModule, g_Allocator);
        return;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexShaderModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentShaderModule;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    // 静态 viewport/scissor 提供非 NULL 指针（Render 时动态覆盖；NVIDIA 驱动对 pViewports=NULL 有崩溃史）
    VkViewport staticViewport = {};
    staticViewport.width = (float)g_MainWindowData.Width;
    staticViewport.height = (float)g_MainWindowData.Height;
    staticViewport.maxDepth = 1.0f;
    VkRect2D staticScissor = {};
    staticScissor.extent.width = g_MainWindowData.Width;
    staticScissor.extent.height = g_MainWindowData.Height;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &staticViewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &staticScissor;

    VkDynamicState dynamicStatesArr[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStatesArr;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending = {};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_DescriptorSetLayout;
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushData);   // 16B（只传 frameInfo，相机矩阵走 UBO）
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (m_DescriptorSetLayout == VK_NULL_HANDLE) {
        vkDestroyShaderModule(g_Device, vertexShaderModule, g_Allocator);
        vkDestroyShaderModule(g_Device, fragmentShaderModule, g_Allocator);
        return;
    }
    VkResult err = vkCreatePipelineLayout(g_Device, &pipelineLayoutInfo, g_Allocator, &m_PipelineLayout);
    if (err != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, vertexShaderModule, g_Allocator);
        vkDestroyShaderModule(g_Device, fragmentShaderModule, g_Allocator);
        return;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;   // 2026-08-13 修复：漏挂动态状态 → vkCmdSetViewport 被忽略 → per-pass scale 时 viewport 恒为全窗口尺寸 → 0.5x framebuffer 只画四分之一
    pipelineInfo.layout = m_PipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = subpass;

    err = vkCreateGraphicsPipelines(g_Device, VK_NULL_HANDLE, 1, &pipelineInfo, g_Allocator, &m_Pipeline);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "[PostProcessQuad] vkCreateGraphicsPipelines FAILED err=%d rp=%p shader='%s' layout=%p\n",
            (int)err, (void*)renderPass, m_FragShaderName.c_str(), (void*)m_PipelineLayout);
    }
    vkDestroyShaderModule(g_Device, vertexShaderModule, g_Allocator);
    vkDestroyShaderModule(g_Device, fragmentShaderModule, g_Allocator);
}

void PostProcessQuad::CreateDescriptorSet()
{
    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_DescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_DescriptorSetLayout;
    VkResult err = vkAllocateDescriptorSets(g_Device, &allocInfo, &m_DescriptorSet);
    check_vk_result(err);
}

void PostProcessQuad::SetInputs(const std::vector<InputBinding>& inputs)
{
    if (m_DescriptorSet == VK_NULL_HANDLE) return;

    // 内容相同则跳过（每帧调用时省去 vkUpdateDescriptorSets）
    bool same = (inputs.size() == m_CachedInputs.size());
    if (same) {
        for (size_t i = 0; i < inputs.size(); i++) {
            if (inputs[i].view != m_CachedInputs[i].view || inputs[i].sampler != m_CachedInputs[i].sampler
                || inputs[i].layout != m_CachedInputs[i].layout) {
                same = false;
                break;
            }
        }
    }
    if (same) return;
    m_CachedInputs = inputs;

    std::vector<VkDescriptorImageInfo> infos(inputs.size());
    std::vector<VkWriteDescriptorSet> writes(inputs.size());
    for (size_t i = 0; i < inputs.size(); i++) {
        infos[i].imageLayout = inputs[i].layout;   // 与 image 实际布局一致（composite=COLOR_ATTACHMENT_OPTIMAL，其余 SHADER_READ_ONLY）
        infos[i].imageView = inputs[i].view;
        infos[i].sampler = inputs[i].sampler;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_DescriptorSet;
        writes[i].dstBinding = inputs[i].slot;   // 2026-08-12：按 JSON 声明槽位写 binding（数组下标会在输入失败跳过时错位）
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo = &infos[i];
    }
    if (!writes.empty()) {
        vkUpdateDescriptorSets(g_Device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    }
}

void PostProcessQuad::Render(VkCommandBuffer commandBuffer, int width, int height, const PushData* push)
{
    if (!m_Initialized || m_Pipeline == VK_NULL_HANDLE || m_DescriptorSet == VK_NULL_HANDLE) return;

    VkViewport viewport = {};
    viewport.width = (float)width;
    viewport.height = (float)height;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.extent = { (uint32_t)width, (uint32_t)height };
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayout, 0, 1, &m_DescriptorSet, 0, nullptr);
    if (push) {   // 2026-08-13：链 pass 相机矩阵 + frameInfo
        vkCmdPushConstants(commandBuffer, m_PipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushData), push);
    }
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}

void PostProcessQuad::UpdateCameraUBO(const CameraUBO& cam)
{
    if (m_CameraUBOMapped) {
        memcpy(m_CameraUBOMapped, &cam, sizeof(CameraUBO));
    }
}
