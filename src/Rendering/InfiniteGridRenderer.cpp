#include "Rendering/InfiniteGridRenderer.h"

#include "Core/EngineGlobal.h"
#include "Core/Log.h"

#include <algorithm>

void InfiniteGridRenderer::Init(VkRenderPass renderPass)
{
    Cleanup();

    if (g_Device == VK_NULL_HANDLE || renderPass == VK_NULL_HANDLE) {
        return;
    }

    if (!m_UniformBuffer.Create(sizeof(UniformData),
                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        Cleanup();
        return;
    }
    m_UniformBuffer.Map();
    if (m_UniformBuffer.GetMappedPtr() == nullptr) {
        Cleanup();
        return;
    }

    m_Descriptor.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    if (!m_Descriptor.CreateLayout() ||
        !m_Descriptor.CreatePool(1) ||
        !m_Descriptor.AllocateSet(m_DescriptorSet)) {
        Cleanup();
        return;
    }

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = m_UniformBuffer.GetBuffer();
    bufferInfo.offset = 0;
    bufferInfo.range = sizeof(UniformData);

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_DescriptorSet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(g_Device, 1, &write, 0, nullptr);

    PipelineConfig pipelineConfig;
    pipelineConfig.vertShader = "fullscreen.vert.spv";
    pipelineConfig.fragShader = "infinite_grid.frag.spv";
    pipelineConfig.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    pipelineConfig.cullMode = VK_CULL_MODE_NONE;
    pipelineConfig.depthTest = true;
    pipelineConfig.depthWrite = false;
    pipelineConfig.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    pipelineConfig.blending = true;
    pipelineConfig.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    pipelineConfig.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    pipelineConfig.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    pipelineConfig.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    pipelineConfig.colorAttachmentCount = 1;
    pipelineConfig.subpass = 0;

    if (!m_Pipeline.Create(renderPass, m_Descriptor.GetLayout(), pipelineConfig)) {
        Cleanup();
        return;
    }

    m_RenderPass = renderPass;
    m_Initialized = true;
    LOGI("[InfiniteGrid] GPU renderer initialized");
}

void InfiniteGridRenderer::Cleanup()
{
    m_Initialized = false;
    m_RenderPass = VK_NULL_HANDLE;
    // The pipeline layout references the descriptor layout, so destroy the
    // pipeline before releasing the descriptor set layout.
    m_Pipeline.Cleanup();
    m_DescriptorSet = VK_NULL_HANDLE;
    m_Descriptor.Cleanup();
    m_UniformBuffer.Cleanup();
}

void InfiniteGridRenderer::Render(VkCommandBuffer commandBuffer,
                                  uint32_t width,
                                  uint32_t height,
                                  const glm::mat4& view,
                                  const glm::mat4& projection,
                                  const Settings& settings)
{
    if (!m_Initialized || commandBuffer == VK_NULL_HANDLE ||
        m_Pipeline.GetPipeline() == VK_NULL_HANDLE ||
        m_DescriptorSet == VK_NULL_HANDLE || width <= 1 || height <= 1 ||
        !settings.enabled) {
        return;
    }

    const glm::mat4 viewProjection = projection * view;
    const glm::mat4 inverseViewProjection = glm::inverse(viewProjection);
    const glm::vec3 cameraPosition = glm::vec3(glm::inverse(view)[3]);

    UniformData data;
    data.viewProjection = viewProjection;
    data.inverseViewProjection = inverseViewProjection;
    data.cameraPosition = glm::vec4(cameraPosition, 1.0f);
    data.gridParams = glm::vec4(
        std::max(settings.gridScale, 0.001f),
        std::max(settings.fadeDistance, 1.0f),
        std::max(settings.axisLength, 0.001f),
        0.0f);   // w：未使用（保留槽位，保持 std140 布局稳定）
    data.viewport = glm::vec4(static_cast<float>(width),
                              static_cast<float>(height), 0.0f, 0.0f);
    data.gridColorThin = settings.gridColorThin;
    data.gridColorThick = settings.gridColorThick;
    data.axisColorX = settings.axisColorX;
    data.axisColorZ = settings.axisColorZ;
    data.axisColorY = settings.axisColorY;
    m_UniformBuffer.Write(&data, sizeof(data));

    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = { width, height };
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_Pipeline.GetPipeline());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_Pipeline.GetLayout(), 0, 1, &m_DescriptorSet,
                            0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}

