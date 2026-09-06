#include "RendererBase.h"
#include "EngineGlobal.h"
#include "EngineConfig.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_filesystem.h>
#include <cstring>
#include <algorithm>

// ---- Shader 热更新全局注册表 ----
// VulkanPipeline::Create 成功后自动登记，Cleanup 时自动注销。仅主线程访问。
namespace {
struct PipelineReloadEntry {
    VulkanPipeline* pipeline;
};
std::vector<PipelineReloadEntry>& GetPipelineReloadRegistry() {
    static std::vector<PipelineReloadEntry> s_registry;
    return s_registry;
}
} // namespace

namespace RendererUtils {

uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    fprintf(stderr, "Failed to find suitable memory type!\n");
    return 0;
}

std::vector<char> ReadFile(const std::string& filename) {
    // 平台路径解析收编到 EngineConfig（RendererBase 的 Android 分支会移除 assets/ 前缀）
    std::string fullPath = EngineConfig::StripAssetPrefix(EngineConfig::ResolvePlatformPath(filename));

    SDL_IOStream* io = SDL_IOFromFile(fullPath.c_str(), "rb");
    if (io == nullptr) {
        fprintf(stderr, "Failed to open file: %s (SDL Error: %s)\n", fullPath.c_str(), SDL_GetError());
        return {};
    }

    Sint64 fileSize = SDL_GetIOSize(io);
    if (fileSize <= 0) {
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

VkShaderModule CreateShaderModule(const std::vector<char>& code, const char* shaderName) {
    if (code.empty()) {
        printf("[Shader] Failed to create shader module %s: empty code", shaderName);
        return VK_NULL_HANDLE;
    }

    if (code.size() % 4 != 0) {
        printf("[Shader] Failed to create shader module %s: code size %zu is not aligned to 4 bytes", shaderName, code.size());
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    VkResult err = vkCreateShaderModule(g_Device, &createInfo, g_Allocator, &shaderModule);
    if (err != VK_SUCCESS) {
        printf("[Shader] Failed to create shader module %s: VkResult = %d", shaderName, err);
        return VK_NULL_HANDLE;
    }
    return shaderModule;
}

}

bool VulkanBuffer::Create(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult err = vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &m_Buffer);
    if (err != VK_SUCCESS) return false;

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(g_Device, m_Buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits, properties);

    err = vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_Memory);
    if (err != VK_SUCCESS) {
        vkDestroyBuffer(g_Device, m_Buffer, g_Allocator);
        m_Buffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(g_Device, m_Buffer, m_Memory, 0);
    m_Size = size;
    return true;
}

void VulkanBuffer::Cleanup() {
    if (m_Mapped) {
        vkUnmapMemory(g_Device, m_Memory);
        m_Mapped = nullptr;
    }
    if (m_Buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, m_Buffer, g_Allocator);
        m_Buffer = VK_NULL_HANDLE;
    }
    if (m_Memory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_Memory, g_Allocator);
        m_Memory = VK_NULL_HANDLE;
    }
    m_Size = 0;
}

void VulkanBuffer::Map() {
    if (m_Memory != VK_NULL_HANDLE && m_Mapped == nullptr) {
        vkMapMemory(g_Device, m_Memory, 0, m_Size, 0, &m_Mapped);
    }
}

void VulkanBuffer::Unmap() {
    if (m_Mapped) {
        vkUnmapMemory(g_Device, m_Memory);
        m_Mapped = nullptr;
    }
}

void VulkanBuffer::Write(const void* data, VkDeviceSize size, VkDeviceSize offset) {
    if (m_Mapped == nullptr) Map();
    if (m_Mapped) {
        memcpy((char*)m_Mapped + offset, data, size);
    }
}

bool VulkanImage::Create(uint32_t width, uint32_t height, VkFormat format,
    VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties) {

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult err = vkCreateImage(g_Device, &imageInfo, g_Allocator, &m_Image);
    if (err != VK_SUCCESS) return false;

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(g_Device, m_Image, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits, properties);

    err = vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_Memory);
    if (err != VK_SUCCESS) {
        vkDestroyImage(g_Device, m_Image, g_Allocator);
        m_Image = VK_NULL_HANDLE;
        return false;
    }

    vkBindImageMemory(g_Device, m_Image, m_Memory, 0);
    return true;
}

bool VulkanImage::CreateView(VkFormat format, VkImageAspectFlags aspectFlags) {
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_Image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkResult err = vkCreateImageView(g_Device, &viewInfo, g_Allocator, &m_View);
    return err == VK_SUCCESS;
}

void VulkanImage::Cleanup() {
    if (m_View != VK_NULL_HANDLE) {
        vkDestroyImageView(g_Device, m_View, g_Allocator);
        m_View = VK_NULL_HANDLE;
    }
    if (m_Image != VK_NULL_HANDLE) {
        vkDestroyImage(g_Device, m_Image, g_Allocator);
        m_Image = VK_NULL_HANDLE;
    }
    if (m_Memory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_Memory, g_Allocator);
        m_Memory = VK_NULL_HANDLE;
    }
}

void VulkanDescriptor::AddBinding(uint32_t binding, VkDescriptorType type, VkShaderStageFlags stageFlags, uint32_t count) {
    VkDescriptorSetLayoutBinding layoutBinding = {};
    layoutBinding.binding = binding;
    layoutBinding.descriptorType = type;
    layoutBinding.descriptorCount = count;
    layoutBinding.stageFlags = stageFlags;
    layoutBinding.pImmutableSamplers = nullptr;
    m_Bindings.push_back(layoutBinding);
}

bool VulkanDescriptor::CreateLayout() {
    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(m_Bindings.size());
    layoutInfo.pBindings = m_Bindings.data();

    VkResult err = vkCreateDescriptorSetLayout(g_Device, &layoutInfo, g_Allocator, &m_Layout);
    return err == VK_SUCCESS;
}

bool VulkanDescriptor::CreatePool(uint32_t maxSets) {
    std::vector<VkDescriptorPoolSize> poolSizes;
    for (const auto& binding : m_Bindings) {
        VkDescriptorPoolSize poolSize = {};
        poolSize.type = binding.descriptorType;
        poolSize.descriptorCount = binding.descriptorCount * maxSets;
        poolSizes.push_back(poolSize);
    }

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = maxSets;

    VkResult err = vkCreateDescriptorPool(g_Device, &poolInfo, g_Allocator, &m_Pool);
    return err == VK_SUCCESS;
}

bool VulkanDescriptor::AllocateSet(VkDescriptorSet& set) {
    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_Pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_Layout;

    VkResult err = vkAllocateDescriptorSets(g_Device, &allocInfo, &set);
    return err == VK_SUCCESS;
}

void VulkanDescriptor::Cleanup() {
    if (m_Pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(g_Device, m_Pool, g_Allocator);
        m_Pool = VK_NULL_HANDLE;
    }
    if (m_Layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(g_Device, m_Layout, g_Allocator);
        m_Layout = VK_NULL_HANDLE;
    }
    m_Bindings.clear();
}

bool VulkanPipeline::Create(VkRenderPass renderPass, VkDescriptorSetLayout descriptorLayout, PipelineConfig config) {
    // All MRT geometry render passes contain one subpass.  Keep the existing
    // logical geometry subpass=1 at call sites, but bind those pipelines to
    // the only actual subpass; the composite pipeline is created separately.
    if (g_UseSeparateMrtRenderPass && config.subpass == 1) {
        config.subpass = 0;
    }
    // 用局部变量组装，全部成功后才写入成员（Reload 时先建新后毁旧，失败可回滚）
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    std::string vertPath = EngineConfig::GetShaderPath(config.vertShader.c_str());
    std::string fragPath = EngineConfig::GetShaderPath(config.fragShader.c_str());
    auto vertCode = RendererUtils::ReadFile(vertPath);
    auto fragCode = RendererUtils::ReadFile(fragPath);

    if (vertCode.empty()) {
        printf("[Pipeline] Failed to read vertex shader: %s", vertPath.c_str());
        return false;
    }
    if (fragCode.empty()) {
        printf("[Pipeline] Failed to read fragment shader: %s", fragPath.c_str());
        return false;
    }

    VkShaderModule vertModule = RendererUtils::CreateShaderModule(vertCode, config.vertShader.c_str());
    VkShaderModule fragModule = RendererUtils::CreateShaderModule(fragCode, config.fragShader.c_str());

    if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) {
        printf("[Pipeline] Failed to create shader modules");
        if (vertModule) vkDestroyShaderModule(g_Device, vertModule, g_Allocator);
        if (fragModule) vkDestroyShaderModule(g_Device, fragModule, g_Allocator);
        return false;
    }

    VkPipelineShaderStageCreateInfo vertStageInfo = {};
    vertStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStageInfo.module = vertModule;
    vertStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragStageInfo = {};
    fragStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStageInfo.module = fragModule;
    fragStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = { vertStageInfo, fragStageInfo };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(config.vertexBindings.size());
    vertexInputInfo.pVertexBindingDescriptions = config.vertexBindings.empty() ? nullptr : config.vertexBindings.data();
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(config.vertexAttributes.size());
    vertexInputInfo.pVertexAttributeDescriptions = config.vertexAttributes.empty() ? nullptr : config.vertexAttributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = config.topology;
    inputAssembly.primitiveRestartEnable = config.primitiveRestartEnable ? VK_TRUE : VK_FALSE;

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
    rasterizer.polygonMode = config.polygonMode;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = config.cullMode;
    rasterizer.frontFace = config.frontFace;
    // 2026-08-14：depth bias（CSM 方向光阴影防 acne；默认关闭不影响现有管线）
    rasterizer.depthBiasEnable = config.depthBiasEnable ? VK_TRUE : VK_FALSE;
    rasterizer.depthBiasConstantFactor = config.depthBiasConstantFactor;
    rasterizer.depthBiasClamp = config.depthBiasClamp;
    rasterizer.depthBiasSlopeFactor = config.depthBiasSlopeFactor;

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = config.depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = config.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = config.depthCompareOp;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = config.blending ? VK_TRUE : VK_FALSE;
    colorBlendAttachment.srcColorBlendFactor = config.srcColorBlendFactor;
    colorBlendAttachment.dstColorBlendFactor = config.dstColorBlendFactor;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = config.srcAlphaBlendFactor;
    colorBlendAttachment.dstAlphaBlendFactor = config.dstAlphaBlendFactor;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments(
        config.colorAttachmentCount, colorBlendAttachment);
    for (size_t i = 0; i < colorBlendAttachments.size() && i < config.colorWriteMasks.size(); ++i) {
        colorBlendAttachments[i].colorWriteMask = config.colorWriteMasks[i];
    }

    VkPipelineColorBlendStateCreateInfo colorBlending = {};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = config.colorAttachmentCount;
    colorBlending.pAttachments = colorBlendAttachments.data();

    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(config.dynamicStates.size());
    dynamicState.pDynamicStates = config.dynamicStates.data();

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

    if (descriptorLayout != VK_NULL_HANDLE) {
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &descriptorLayout;
    } else {
        pipelineLayoutInfo.setLayoutCount = 0;
        pipelineLayoutInfo.pSetLayouts = nullptr;
    }

    if (config.usePushConstants) {
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &config.pushConstantRange;
    }

    VkResult err = vkCreatePipelineLayout(g_Device, &pipelineLayoutInfo, g_Allocator, &layout);
    if (err != VK_SUCCESS) {
        printf("[Pipeline] Failed to create pipeline layout: VkResult = %d", err);
        vkDestroyShaderModule(g_Device, vertModule, g_Allocator);
        vkDestroyShaderModule(g_Device, fragModule, g_Allocator);
        return false;
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
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = config.subpass;

    err = vkCreateGraphicsPipelines(g_Device, VK_NULL_HANDLE, 1, &pipelineInfo, g_Allocator, &pipeline);

    vkDestroyShaderModule(g_Device, vertModule, g_Allocator);
    vkDestroyShaderModule(g_Device, fragModule, g_Allocator);

    if (err != VK_SUCCESS) {
        printf("[Pipeline] Failed to create graphics pipeline: VkResult = %d", err);
        if (layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(g_Device, layout, g_Allocator);
        return false;
    }

    m_Pipeline = pipeline;
    m_Layout = layout;
    RegisterForReload(renderPass, descriptorLayout, config);
    return true;
}

void VulkanPipeline::Cleanup() {
    if (m_ReloadRegistered) {
        auto& reg = GetPipelineReloadRegistry();
        reg.erase(std::remove_if(reg.begin(), reg.end(),
            [this](const PipelineReloadEntry& e) { return e.pipeline == this; }), reg.end());
        m_ReloadRegistered = false;
    }
    if (m_Pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(g_Device, m_Pipeline, g_Allocator);
        m_Pipeline = VK_NULL_HANDLE;
    }
    if (m_Layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(g_Device, m_Layout, g_Allocator);
        m_Layout = VK_NULL_HANDLE;
    }
}

void VulkanPipeline::RegisterForReload(VkRenderPass renderPass, VkDescriptorSetLayout descriptorLayout, const PipelineConfig& config) {
    m_ReloadConfig = config;
    m_ReloadRenderPass = renderPass;
    m_ReloadDescriptorLayout = descriptorLayout;
    if (!m_ReloadRegistered) {
        GetPipelineReloadRegistry().push_back({this});
        m_ReloadRegistered = true;
    }
}

bool VulkanPipeline::Reload() {
    if (!m_ReloadRegistered || m_ReloadRenderPass == VK_NULL_HANDLE) {
        return false;
    }
    // 先建新管线（写局部变量，失败不影响成员），成功后才销毁旧管线 —— 保证失败回滚、画面不黑
    VkPipeline oldPipeline = m_Pipeline;
    VkPipelineLayout oldLayout = m_Layout;
    m_Pipeline = VK_NULL_HANDLE;
    m_Layout = VK_NULL_HANDLE;
    if (!Create(m_ReloadRenderPass, m_ReloadDescriptorLayout, m_ReloadConfig)) {
        m_Pipeline = oldPipeline;
        m_Layout = oldLayout;
        return false;
    }
    if (oldPipeline != VK_NULL_HANDLE) vkDestroyPipeline(g_Device, oldPipeline, g_Allocator);
    if (oldLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(g_Device, oldLayout, g_Allocator);
    return true;
}

uint32_t VulkanPipeline::ReloadAllPipelines() {
    uint32_t reloaded = 0;
    auto& reg = GetPipelineReloadRegistry();
    for (auto& entry : reg) {
        if (entry.pipeline->Reload()) reloaded++;
    }
    return reloaded;
}

void BaseRenderer::Cleanup() {
    m_Pipeline.Cleanup();
    m_Descriptor.Cleanup();
    m_UniformBuffer.Cleanup();
    m_DescriptorSet = VK_NULL_HANDLE;
}

bool BaseRenderer::CreateUniformBuffer(VkDeviceSize size) {
    return m_UniformBuffer.Create(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

bool BaseRenderer::CreateDescriptorSet() {
    return m_Descriptor.AllocateSet(m_DescriptorSet);
}

void BaseRenderer::UpdateUniformBuffer(const void* data, VkDeviceSize size) {
    m_UniformBuffer.Write(data, size);
}
