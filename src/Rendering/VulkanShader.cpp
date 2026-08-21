#include "VulkanShader.h"
#include "EngineGlobal.h"
#include "Core/EngineConfig.h"
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <fstream>
#include <iostream>

VulkanShader::VulkanShader(VkDevice device)
    : m_Device(device)
{
}

VulkanShader::~VulkanShader()
{
    Cleanup();
}

void VulkanShader::Cleanup()
{
    if (m_Pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_Device, m_Pipeline, g_Allocator);
        m_Pipeline = VK_NULL_HANDLE;
    }
    if (m_PipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_Device, m_PipelineLayout, g_Allocator);
        m_PipelineLayout = VK_NULL_HANDLE;
    }
    if (m_DescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_Device, m_DescriptorPool, g_Allocator);
        m_DescriptorPool = VK_NULL_HANDLE;
    }
    if (m_DescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_Device, m_DescriptorSetLayout, g_Allocator);
        m_DescriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_VertShaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_Device, m_VertShaderModule, g_Allocator);
        m_VertShaderModule = VK_NULL_HANDLE;
    }
    if (m_FragShaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_Device, m_FragShaderModule, g_Allocator);
        m_FragShaderModule = VK_NULL_HANDLE;
    }
}

std::vector<char> VulkanShader::ReadFile(const std::string& filename)
{
    std::string fullPath = EngineConfig::ResolvePlatformPath(filename);

    SDL_IOStream* io = SDL_IOFromFile(fullPath.c_str(), "rb");
    if (io == nullptr) {
        fprintf(stderr, "Failed to open shader file: %s (SDL Error: %s)\n", fullPath.c_str(), SDL_GetError());
        return {};
    }
    
    Sint64 fileSize = SDL_GetIOSize(io);
    if (fileSize <= 0) {
        SDL_CloseIO(io);
        fprintf(stderr, "Failed to get shader file size: %s\n", fullPath.c_str());
        return {};
    }
    
    std::vector<char> buffer((size_t)fileSize);
    if (SDL_ReadIO(io, buffer.data(), (size_t)fileSize) != (size_t)fileSize) {
        SDL_CloseIO(io);
        fprintf(stderr, "Failed to read shader file: %s\n", fullPath.c_str());
        return {};
    }
    
    SDL_CloseIO(io);
    return buffer;
}

VkShaderModule VulkanShader::CreateShaderModule(const std::vector<char>& code)
{
    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    VkResult err = vkCreateShaderModule(m_Device, &createInfo, g_Allocator, &shaderModule);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "Failed to create shader module\n");
        return VK_NULL_HANDLE;
    }
    return shaderModule;
}

bool VulkanShader::LoadFromSPIRV(const std::string& vertPath, const std::string& fragPath)
{
    auto vertCode = ReadFile(vertPath);
    auto fragCode = ReadFile(fragPath);

    if (vertCode.empty() || fragCode.empty()) {
        fprintf(stderr, "Failed to read shader files: %s, %s\n", vertPath.c_str(), fragPath.c_str());
        return false;
    }

    m_VertShaderModule = CreateShaderModule(vertCode);
    m_FragShaderModule = CreateShaderModule(fragCode);

    if (m_VertShaderModule == VK_NULL_HANDLE || m_FragShaderModule == VK_NULL_HANDLE) {
        return false;
    }

    return true;
}

bool VulkanShader::CreateDescriptorSetLayout()
{
    if (m_LayoutBindings.empty()) {
        return true;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(m_LayoutBindings.size());
    layoutInfo.pBindings = m_LayoutBindings.data();

    VkResult err = vkCreateDescriptorSetLayout(m_Device, &layoutInfo, g_Allocator, &m_DescriptorSetLayout);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "Failed to create descriptor set layout\n");
        return false;
    }
    return true;
}

bool VulkanShader::CreatePipelineLayout()
{
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_DescriptorSetLayout;

    VkResult err = vkCreatePipelineLayout(m_Device, &pipelineLayoutInfo, g_Allocator, &m_PipelineLayout);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "Failed to create pipeline layout\n");
        return false;
    }
    return true;
}

bool VulkanShader::CreatePipeline(VkRenderPass renderPass,
                                   VkVertexInputBindingDescription bindingDesc,
                                   const std::vector<VkVertexInputAttributeDescription>& attributeDescs,
                                   VkPrimitiveTopology topology,
                                   bool depthTest,
                                   bool depthWrite)
{
    if (!CreateDescriptorSetLayout()) {
        return false;
    }

    if (!CreatePipelineLayout()) {
        return false;
    }

    VkPipelineShaderStageCreateInfo vertShaderStageInfo = {};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = m_VertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo = {};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = m_FragShaderModule;
    fragShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo, fragShaderStageInfo };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDesc;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = topology;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    // 静态 viewport/scissor 提供非 NULL 指针（Render 时动态覆盖；NVIDIA 驱动 nvoglv64.dll 对 pViewports=NULL 崩溃读 0x0）
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

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | 
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending = {};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

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
    pipelineInfo.subpass = 0;

    VkResult err = vkCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE, 1, &pipelineInfo, g_Allocator, &m_Pipeline);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "Failed to create graphics pipeline\n");
        return false;
    }

    return true;
}

void VulkanShader::SetUniformBuffer(uint32_t binding, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range)
{
    while (m_LayoutBindings.size() <= binding) {
        VkDescriptorSetLayoutBinding layoutBinding = {};
        layoutBinding.binding = static_cast<uint32_t>(m_LayoutBindings.size());
        layoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        layoutBinding.descriptorCount = 1;
        layoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        m_LayoutBindings.push_back(layoutBinding);
    }

    m_LayoutBindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    m_LayoutBindings[binding].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorBufferInfo bufferInfo = {};
    bufferInfo.buffer = buffer;
    bufferInfo.offset = offset;
    bufferInfo.range = range;
    m_BufferInfos.push_back(bufferInfo);

    VkWriteDescriptorSet descriptorWrite = {};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstBinding = binding;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pBufferInfo = &m_BufferInfos.back();
    m_DescriptorWrites.push_back(descriptorWrite);
}

void VulkanShader::SetCombinedImageSampler(uint32_t binding, VkImageView imageView, VkSampler sampler, VkImageLayout layout)
{
    while (m_LayoutBindings.size() <= binding) {
        VkDescriptorSetLayoutBinding layoutBinding = {};
        layoutBinding.binding = static_cast<uint32_t>(m_LayoutBindings.size());
        layoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        layoutBinding.descriptorCount = 1;
        layoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        m_LayoutBindings.push_back(layoutBinding);
    }

    m_LayoutBindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    m_LayoutBindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = layout;
    imageInfo.imageView = imageView;
    imageInfo.sampler = sampler;
    m_ImageInfos.push_back(imageInfo);

    VkWriteDescriptorSet descriptorWrite = {};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstBinding = binding;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &m_ImageInfos.back();
    m_DescriptorWrites.push_back(descriptorWrite);
}

void VulkanShader::Bind(VkCommandBuffer cmd) const
{
    if (m_Pipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
    }
}

void VulkanShader::BindDescriptorSet(VkCommandBuffer cmd, VkDescriptorSet descriptorSet) const
{
    if (m_PipelineLayout != VK_NULL_HANDLE && descriptorSet != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    }
}
