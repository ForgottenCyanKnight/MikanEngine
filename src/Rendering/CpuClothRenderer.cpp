#include "Rendering/CpuClothRenderer.h"

#include "Core/VulkanContext.h"
#include "Core/VulkanManager.h"

#include <algorithm>
#include <cstddef>

namespace {

VkVertexInputBindingDescription MakeVertexBinding() {
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(ClothRenderVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return binding;
}

VkVertexInputAttributeDescription MakeVertexAttribute(uint32_t location,
                                                       uint32_t offset) {
    VkVertexInputAttributeDescription attribute{};
    attribute.location = location;
    attribute.binding = 0;
    attribute.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attribute.offset = offset;
    return attribute;
}

} // namespace

CpuClothRenderer::~CpuClothRenderer() {
    Cleanup();
}

bool CpuClothRenderer::EnsureInitialized(VkRenderPass renderPass,
                                          uint32_t subpass) {
    if (renderPass == VK_NULL_HANDLE || g_Device == VK_NULL_HANDLE) return false;

    for (auto& pipelineSet : m_PipelineSets) {
        if (pipelineSet && pipelineSet->renderPass == renderPass &&
            pipelineSet->subpass == subpass &&
            pipelineSet->surface.GetPipeline() != VK_NULL_HANDLE &&
            pipelineSet->springs.GetPipeline() != VK_NULL_HANDLE) {
            m_ActivePipelineSet = pipelineSet.get();
            return true;
        }
    }

    auto pipelineSet = std::make_unique<PipelineSet>();
    pipelineSet->renderPass = renderPass;
    pipelineSet->subpass = subpass;
    if (!CreatePipelines(*pipelineSet, subpass)) return false;
    m_ActivePipelineSet = pipelineSet.get();
    m_PipelineSets.push_back(std::move(pipelineSet));
    return true;
}

bool CpuClothRenderer::CreatePipelines(PipelineSet& pipelineSet,
                                       uint32_t subpass) {
    std::array<VkVertexInputBindingDescription, 1> bindings = {
        MakeVertexBinding()};
    std::array<VkVertexInputAttributeDescription, 2> attributes = {
        MakeVertexAttribute(0, offsetof(ClothRenderVertex, position)),
        MakeVertexAttribute(1, offsetof(ClothRenderVertex, color))};

    PipelineConfig surfaceConfig{};
    surfaceConfig.vertShader = "cloth.vert.spv";
    surfaceConfig.fragShader = "cloth.frag.spv";
    surfaceConfig.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    surfaceConfig.cullMode = VK_CULL_MODE_NONE;
    surfaceConfig.depthTest = true;
    surfaceConfig.depthWrite = false;
    surfaceConfig.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    surfaceConfig.blending = true;
    surfaceConfig.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    surfaceConfig.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    surfaceConfig.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    surfaceConfig.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    surfaceConfig.colorAttachmentCount = 1;
    surfaceConfig.subpass = subpass;
    surfaceConfig.vertexBindings.assign(bindings.begin(), bindings.end());
    surfaceConfig.vertexAttributes.assign(attributes.begin(), attributes.end());
    surfaceConfig.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    surfaceConfig.pushConstantRange.offset = 0;
    surfaceConfig.pushConstantRange.size = sizeof(PushConstants);
    surfaceConfig.usePushConstants = true;

    if (!pipelineSet.surface.Create(pipelineSet.renderPass, VK_NULL_HANDLE,
                                    surfaceConfig)) {
        return false;
    }

    PipelineConfig springConfig = surfaceConfig;
    springConfig.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    springConfig.fragShader = "cloth_spring.frag.spv";
    if (!pipelineSet.springs.Create(pipelineSet.renderPass, VK_NULL_HANDLE,
                                    springConfig)) {
        pipelineSet.surface.Cleanup();
        return false;
    }
    return true;
}

bool CpuClothRenderer::EnsureBufferCapacity(VulkanBuffer& buffer,
                                             size_t& capacity,
                                             size_t required,
                                             size_t initialCapacity) {
    if (required == 0) return true;
    if (buffer.GetBuffer() != VK_NULL_HANDLE && required <= capacity) {
        return true;
    }
    if (g_Device == VK_NULL_HANDLE) return false;

    if (buffer.GetBuffer() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
        buffer.Cleanup();
    }
    const size_t newCapacity = std::max(
        required, std::max(initialCapacity, capacity * 2));
    const VkDeviceSize size = static_cast<VkDeviceSize>(
        newCapacity * sizeof(ClothRenderVertex));
    const VkMemoryPropertyFlags memory =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!buffer.Create(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory)) {
        capacity = 0;
        return false;
    }
    capacity = newCapacity;
    return true;
}

bool CpuClothRenderer::UploadFrame(const CpuClothSimulation& simulation,
                                   uint32_t frameSlot,
                                   uint64_t frameSerial) {
    if (frameSlot >= kFramesInFlight) return false;
    if (m_LastUploadFrameSerial == frameSerial) return true;

    simulation.BuildRenderVertices(m_SurfaceUpload, m_SpringUpload);
    if (m_SurfaceUpload.empty() || m_SpringUpload.empty()) return false;

    if (!EnsureBufferCapacity(m_SurfaceBuffers[frameSlot],
                              m_SurfaceCapacities[frameSlot],
                              m_SurfaceUpload.size(),
                              kInitialSurfaceCapacity) ||
        !EnsureBufferCapacity(m_SpringBuffers[frameSlot],
                              m_SpringCapacities[frameSlot],
                              m_SpringUpload.size(),
                              kInitialSpringCapacity)) {
        return false;
    }

    m_SurfaceBuffers[frameSlot].Write(
        m_SurfaceUpload.data(), static_cast<VkDeviceSize>(
            m_SurfaceUpload.size() * sizeof(ClothRenderVertex)));
    m_SpringBuffers[frameSlot].Write(
        m_SpringUpload.data(), static_cast<VkDeviceSize>(
            m_SpringUpload.size() * sizeof(ClothRenderVertex)));
    m_SurfaceCounts[frameSlot] =
        static_cast<uint32_t>(m_SurfaceUpload.size());
    m_SpringCounts[frameSlot] =
        static_cast<uint32_t>(m_SpringUpload.size());
    m_LastUploadFrameSerial = frameSerial;
    return true;
}

void CpuClothRenderer::Render(
    VkCommandBuffer commandBuffer, uint32_t width, uint32_t height,
    VkRenderPass renderPass, uint32_t subpass, const glm::mat4& view,
    const glm::mat4& proj, const CpuClothSimulation& simulation) {
    if (commandBuffer == VK_NULL_HANDLE || width == 0 || height == 0 ||
        !simulation.IsInitialized() ||
        !EnsureInitialized(renderPass, subpass) ||
        m_ActivePipelineSet == nullptr) {
        return;
    }

    const uint64_t frameSerial = GetCurrentFrameSerial();
    const uint32_t frameSlot = static_cast<uint32_t>(
        frameSerial % kFramesInFlight);
    if (!UploadFrame(simulation, frameSlot, frameSerial)) return;
    if (m_SurfaceCounts[frameSlot] == 0 || m_SpringCounts[frameSlot] == 0) {
        return;
    }

    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {width, height};

    const glm::mat4 model = glm::translate(
        glm::mat4(1.0f), simulation.GetCenter()) *
        glm::mat4_cast(simulation.GetRotation());
    PushConstants pushConstants{};
    pushConstants.viewProj = proj * view;
    pushConstants.model = model;

    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    const VkDeviceSize offset = 0;
    const VkBuffer surfaceBuffer = m_SurfaceBuffers[frameSlot].GetBuffer();
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_ActivePipelineSet->surface.GetPipeline());
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &surfaceBuffer, &offset);
    vkCmdPushConstants(commandBuffer, m_ActivePipelineSet->surface.GetLayout(),
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pushConstants),
                       &pushConstants);
    vkCmdDraw(commandBuffer, m_SurfaceCounts[frameSlot], 1, 0, 0);

    const VkBuffer springBuffer = m_SpringBuffers[frameSlot].GetBuffer();
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_ActivePipelineSet->springs.GetPipeline());
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &springBuffer, &offset);
    vkCmdPushConstants(commandBuffer, m_ActivePipelineSet->springs.GetLayout(),
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pushConstants),
                       &pushConstants);
    vkCmdDraw(commandBuffer, m_SpringCounts[frameSlot], 1, 0, 0);
}

void CpuClothRenderer::Cleanup() {
    if (g_Device == VK_NULL_HANDLE) {
        m_ActivePipelineSet = nullptr;
        m_SurfaceCounts = {};
        m_SpringCounts = {};
        m_SurfaceCapacities = {};
        m_SpringCapacities = {};
        m_SurfaceUpload.clear();
        m_SpringUpload.clear();
        return;
    }

    if (!m_PipelineSets.empty() || m_SurfaceBuffers[0].GetBuffer() != VK_NULL_HANDLE ||
        m_SpringBuffers[0].GetBuffer() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
    }
    for (VulkanBuffer& buffer : m_SurfaceBuffers) buffer.Cleanup();
    for (VulkanBuffer& buffer : m_SpringBuffers) buffer.Cleanup();
    for (auto& pipelineSet : m_PipelineSets) {
        if (pipelineSet) {
            pipelineSet->surface.Cleanup();
            pipelineSet->springs.Cleanup();
        }
    }
    m_PipelineSets.clear();
    m_ActivePipelineSet = nullptr;
    m_SurfaceCapacities = {};
    m_SpringCapacities = {};
    m_SurfaceCounts = {};
    m_SpringCounts = {};
    m_SurfaceUpload.clear();
    m_SpringUpload.clear();
    m_LastUploadFrameSerial = UINT64_MAX;
}
