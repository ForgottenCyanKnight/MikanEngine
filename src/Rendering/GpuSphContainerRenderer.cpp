#include "Rendering/GpuSphContainerRenderer.h"

#include "Core/VulkanContext.h"
#include "Rendering/GpuSphContainerGeometry.h"

#include <array>
#include <vector>

#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace {

struct ContainerRing {
    float y = 0.0f;
    float halfExtent = 0.0f;
};

glm::vec3 RingCorner(const ContainerRing& ring, int corner) {
    const float x = (corner == 0 || corner == 3)
        ? -ring.halfExtent : ring.halfExtent;
    const float z = (corner == 0 || corner == 1)
        ? -ring.halfExtent : ring.halfExtent;
    return GpuSphContainerGeometry::NormalizeLocalPoint(
        glm::vec3(x, ring.y, z));
}

void AddQuad(std::vector<glm::vec3>& vertices,
             const glm::vec3& a, const glm::vec3& b,
             const glm::vec3& c, const glm::vec3& d) {
    vertices.push_back(a);
    vertices.push_back(b);
    vertices.push_back(c);
    vertices.push_back(c);
    vertices.push_back(d);
    vertices.push_back(a);
}

void AddSegment(std::vector<glm::vec3>& vertices,
                const glm::vec3& a, const glm::vec3& b) {
    vertices.push_back(a);
    vertices.push_back(b);
}

void AddRingEdges(std::vector<glm::vec3>& edges,
                  const ContainerRing& ring) {
    for (int side = 0; side < 4; ++side) {
        AddSegment(edges, RingCorner(ring, side),
                   RingCorner(ring, (side + 1) % 4));
    }
}

} // namespace

GpuSphContainerRenderer::~GpuSphContainerRenderer() {
    Cleanup();
}

bool GpuSphContainerRenderer::EnsureInitialized(VkRenderPass renderPass,
                                                 uint32_t subpass) {
    if (renderPass == VK_NULL_HANDLE || g_Device == VK_NULL_HANDLE) return false;

    for (auto& pipelineSet : m_PipelineSets) {
        if (pipelineSet && pipelineSet->renderPass == renderPass &&
            pipelineSet->subpass == subpass &&
            pipelineSet->faces.GetPipeline() != VK_NULL_HANDLE &&
            pipelineSet->edges.GetPipeline() != VK_NULL_HANDLE) {
            m_ActivePipelineSet = pipelineSet.get();
            return true;
        }
    }

    if (m_VertexBuffer.GetBuffer() == VK_NULL_HANDLE && !CreateVertexBuffer()) {
        return false;
    }

    auto pipelineSet = std::make_unique<PipelineSet>();
    pipelineSet->renderPass = renderPass;
    pipelineSet->subpass = subpass;
    if (!CreatePipelines(*pipelineSet, subpass)) {
        return false;
    }
    m_ActivePipelineSet = pipelineSet.get();
    m_PipelineSets.push_back(std::move(pipelineSet));
    return true;
}

bool GpuSphContainerRenderer::CreateVertexBuffer() {
    // Rings are ordered from top to bottom. Each vertical segment keeps the
    // upper ring width, then a horizontal ledge narrows to the next ring. This
    // produces a real stair profile instead of a continuously sloped funnel.
    const std::array<ContainerRing, 6> rings = {{
        {GpuSphContainerGeometry::kTopY,
         GpuSphContainerGeometry::kChamberHalfExtent},
        {GpuSphContainerGeometry::kChamberBottomY,
         GpuSphContainerGeometry::kChamberHalfExtent},
        {GpuSphContainerGeometry::kStep1Y,
         GpuSphContainerGeometry::kStep1HalfExtent},
        {GpuSphContainerGeometry::kStep2Y,
         GpuSphContainerGeometry::kStep2HalfExtent},
        {GpuSphContainerGeometry::kStep3Y,
         GpuSphContainerGeometry::kOutletHalfExtent},
        {GpuSphContainerGeometry::kBottomY,
         GpuSphContainerGeometry::kOutletHalfExtent},
    }};

    std::vector<glm::vec3> vertices;
    vertices.reserve(204);
    for (size_t segment = 0; segment + 1 < rings.size(); ++segment) {
        const ContainerRing& upper = rings[segment];
        const ContainerRing& lower = rings[segment + 1];
        const ContainerRing lowerWall{lower.y, upper.halfExtent};
        for (int side = 0; side < 4; ++side) {
            const int nextSide = (side + 1) % 4;
            AddQuad(vertices, RingCorner(upper, side),
                    RingCorner(upper, nextSide),
                    RingCorner(lowerWall, nextSide),
                    RingCorner(lowerWall, side));

            // Leave a central square opening in each horizontal ledge so the
            // particles can fall through while the step remains visible.
            if (lower.halfExtent < upper.halfExtent) {
                AddQuad(vertices, RingCorner(lowerWall, side),
                        RingCorner(lowerWall, nextSide),
                        RingCorner(lower, nextSide),
                        RingCorner(lower, side));
            }
        }
    }
    AddQuad(vertices, RingCorner(rings.front(), 0),
            RingCorner(rings.front(), 1), RingCorner(rings.front(), 2),
            RingCorner(rings.front(), 3));
    AddQuad(vertices, RingCorner(rings.back(), 3),
            RingCorner(rings.back(), 2), RingCorner(rings.back(), 1),
            RingCorner(rings.back(), 0));

    std::vector<glm::vec3> edges;
    edges.reserve(112);
    for (size_t segment = 0; segment + 1 < rings.size(); ++segment) {
        const ContainerRing& upper = rings[segment];
        const ContainerRing& lower = rings[segment + 1];
        const ContainerRing lowerWall{lower.y, upper.halfExtent};
        AddRingEdges(edges, upper);
        AddRingEdges(edges, lowerWall);
        for (int corner = 0; corner < 4; ++corner) {
            AddSegment(edges, RingCorner(upper, corner),
                       RingCorner(lowerWall, corner));
        }
        if (lower.halfExtent < upper.halfExtent) AddRingEdges(edges, lower);
    }

    const VkDeviceSize size =
        sizeof(glm::vec3) * static_cast<VkDeviceSize>(vertices.size());
    if (!m_VertexBuffer.Create(
            size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        return false;
    }
    m_VertexBuffer.Write(vertices.data(), size);
    m_VertexCount = static_cast<uint32_t>(vertices.size());

    const VkDeviceSize edgeSize =
        sizeof(glm::vec3) * static_cast<VkDeviceSize>(edges.size());
    if (!m_EdgeVertexBuffer.Create(
            edgeSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        m_VertexBuffer.Cleanup();
        m_VertexCount = 0;
        return false;
    }
    m_EdgeVertexBuffer.Write(edges.data(), edgeSize);
    m_EdgeVertexCount = static_cast<uint32_t>(edges.size());
    return true;
}

bool GpuSphContainerRenderer::CreatePipelines(PipelineSet& pipelineSet,
                                               uint32_t subpass) {
    PipelineConfig config{};
    config.vertShader = "sph_container.vert.spv";
    config.fragShader = "sph_container.frag.spv";
    config.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    config.cullMode = VK_CULL_MODE_NONE;
    config.depthTest = true;
    config.depthWrite = false;
    config.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    config.blending = true;
    config.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    config.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    config.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    config.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    config.subpass = subpass;

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(glm::vec3);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    config.vertexBindings.push_back(binding);

    VkVertexInputAttributeDescription attribute{};
    attribute.location = 0;
    attribute.binding = 0;
    attribute.format = VK_FORMAT_R32G32B32_SFLOAT;
    attribute.offset = 0;
    config.vertexAttributes.push_back(attribute);

    config.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    config.pushConstantRange.offset = 0;
    config.pushConstantRange.size = sizeof(PushConstants);
    config.usePushConstants = true;

    if (!pipelineSet.faces.Create(pipelineSet.renderPass, VK_NULL_HANDLE,
                                  config)) {
        return false;
    }

    PipelineConfig edgeConfig = config;
    edgeConfig.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    edgeConfig.fragShader = "sph_container_edge.frag.spv";
    if (!pipelineSet.edges.Create(pipelineSet.renderPass, VK_NULL_HANDLE,
                                  edgeConfig)) {
        pipelineSet.faces.Cleanup();
        return false;
    }
    return true;
}

void GpuSphContainerRenderer::Render(
    VkCommandBuffer commandBuffer, uint32_t width, uint32_t height,
    VkRenderPass renderPass, uint32_t subpass, const glm::mat4& view,
    const glm::mat4& proj, const glm::vec3& center,
    const glm::vec3& halfExtents, const glm::quat& rotation) {
    if (commandBuffer == VK_NULL_HANDLE || width == 0 || height == 0 ||
        !EnsureInitialized(renderPass, subpass) || m_ActivePipelineSet == nullptr ||
        m_VertexBuffer.GetBuffer() == VK_NULL_HANDLE || m_VertexCount == 0 ||
        m_EdgeVertexBuffer.GetBuffer() == VK_NULL_HANDLE || m_EdgeVertexCount == 0) {
        return;
    }

    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {width, height};

    const glm::mat4 model = glm::translate(glm::mat4(1.0f), center) *
                            glm::mat4_cast(rotation) *
                            glm::scale(glm::mat4(1.0f), halfExtents);
    PushConstants pushConstants{};
    pushConstants.viewProj = proj * view;
    pushConstants.model = model;

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_ActivePipelineSet->faces.GetPipeline());
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    const VkBuffer vertexBuffer = m_VertexBuffer.GetBuffer();
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &offset);
    vkCmdPushConstants(commandBuffer, m_ActivePipelineSet->faces.GetLayout(),
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pushConstants),
                       &pushConstants);
    vkCmdDraw(commandBuffer, m_VertexCount, 1, 0, 0);

    const VkBuffer edgeVertexBuffer = m_EdgeVertexBuffer.GetBuffer();
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_ActivePipelineSet->edges.GetPipeline());
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &edgeVertexBuffer, &offset);
    vkCmdPushConstants(commandBuffer, m_ActivePipelineSet->edges.GetLayout(),
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pushConstants),
                       &pushConstants);
    vkCmdDraw(commandBuffer, m_EdgeVertexCount, 1, 0, 0);
}

void GpuSphContainerRenderer::Cleanup() {
    if (g_Device == VK_NULL_HANDLE) {
        m_PipelineSets.clear();
        m_ActivePipelineSet = nullptr;
        m_VertexCount = 0;
        m_EdgeVertexCount = 0;
        return;
    }

    if (!m_PipelineSets.empty() || m_VertexBuffer.GetBuffer() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
    }
    m_PipelineSets.clear();
    m_ActivePipelineSet = nullptr;
    m_VertexBuffer.Cleanup();
    m_EdgeVertexBuffer.Cleanup();
    m_VertexCount = 0;
    m_EdgeVertexCount = 0;
}
