#include "WaterRenderer.h"

#include "Core/EngineConfig.h"
#include "Core/RenderGlobals.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanManager.h"
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <glm/gtc/matrix_transform.hpp>

namespace {

constexpr size_t kMaxDescriptorSets = 64;

VkVertexInputBindingDescription MakeVertexBinding(uint32_t binding, uint32_t stride,
                                                   VkVertexInputRate inputRate) {
    VkVertexInputBindingDescription desc{};
    desc.binding = binding;
    desc.stride = stride;
    desc.inputRate = inputRate;
    return desc;
}

VkVertexInputAttributeDescription MakeVertexAttribute(uint32_t location, uint32_t binding,
                                                       VkFormat format, uint32_t offset) {
    VkVertexInputAttributeDescription desc{};
    desc.location = location;
    desc.binding = binding;
    desc.format = format;
    desc.offset = offset;
    return desc;
}

glm::mat4 MakeWaterModel(const glm::mat4& worldMatrix, const ECS::WaterComponent& settings) {
    const glm::vec2 size = glm::max(glm::abs(settings.size), glm::vec2(0.001f));
    glm::mat4 model = worldMatrix;
    model = model * glm::translate(glm::mat4(1.0f),
                                   glm::vec3(0.0f, settings.surfaceOffset, 0.0f));
    model = model * glm::scale(glm::mat4(1.0f), glm::vec3(size.x, 1.0f, size.y));
    return model;
}

} // namespace

WaterRenderer::~WaterRenderer() {
    Cleanup();
}

void WaterRenderer::Init(VkRenderPass renderPass) {
    m_RenderPass = renderPass;

    // 交换链重建只需要重建依赖 render pass 的管线，网格、UBO 和 descriptor
    // 可以复用，避免每次窗口变化都重新上传水面数据。
    m_Pipeline.Cleanup();
    m_DepthPipeline.Cleanup();

    if (!CreateDescriptorResources() || !BuildMesh() ||
        !CreateUniformBuffers() || !CreateDescriptorSets() || !CreatePipelines()) {
        std::printf("[WaterRenderer] initialization failed\n");
        return;
    }
    std::printf("[WaterRenderer] initialized mesh=%ux%u\n",
                kMeshResolution, kMeshResolution);
}

void WaterRenderer::Cleanup() {
    if (g_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
    }

    m_PreparedInstances.clear();
    m_PreparedEntities.clear();
    m_PreviousModels.clear();
    m_InstanceCapacity = 0;

    for (auto& buffer : m_InstanceBuffers) {
        buffer.Cleanup();
    }
    for (auto& uniform : m_UniformBuffers) {
        uniform.reset();
    }
    m_DescriptorSets.fill(VK_NULL_HANDLE);

    m_VertexBuffer.Cleanup();
    m_IndexBuffer.Cleanup();
    m_IndexCount = 0;

    m_Pipeline.Cleanup();
    m_DepthPipeline.Cleanup();
    DestroyDescriptorResources();
    m_RenderPass = VK_NULL_HANDLE;
}

void WaterRenderer::Prepare(const std::vector<ECS::Entity>& rootEntities,
                            const glm::vec3& cameraPosition,
                            const std::array<Plane, 6>& frustumPlanes,
                            bool useFrustumCulling) {
    m_PreparedInstances.clear();
    m_PreparedEntities.clear();

    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& scene = ECS::SceneECS::GetInstance();
    std::vector<ECS::Entity> waterEntities;
    for (ECS::Entity root : rootEntities) {
        CollectWaterEntities(root, waterEntities);
    }

    m_PreparedInstances.reserve(waterEntities.size());
    m_PreparedEntities.reserve(waterEntities.size());
    for (ECS::Entity entity : waterEntities) {
        if (!coordinator.HasComponent<ECS::WaterComponent>(entity) ||
            !coordinator.HasComponent<ECS::TransformComponent>(entity)) {
            continue;
        }

        const auto& settings = coordinator.GetComponent<ECS::WaterComponent>(entity);
        if (!settings.enabled) continue;
        if (coordinator.HasComponent<ECS::RenderComponent>(entity) &&
            !coordinator.GetComponent<ECS::RenderComponent>(entity).visible) {
            continue;
        }

        const glm::mat4 model = MakeWaterModel(scene.GetWorldMatrix(entity), settings);
        const AABB localBounds(glm::vec3(-0.5f, -0.02f, -0.5f),
                               glm::vec3(0.5f, 0.02f, 0.5f));
        const AABB worldBounds = localBounds.Transform(model);
        if (useFrustumCulling && !worldBounds.IsInsideFrustum(frustumPlanes)) {
            continue;
        }

        // cameraPosition is intentionally consumed here even though culling is
        // currently frustum based; keeping the distance available makes adding
        // water streaming/range culling later ABI-neutral.
        (void)cameraPosition;

        WaterInstance instance;
        instance.model = model;
        auto previous = m_PreviousModels.find(entity);
        instance.previousModel = previous != m_PreviousModels.end()
            ? previous->second : model;
        instance.color = glm::vec4(glm::clamp(settings.color,
                                              glm::vec3(0.0f), glm::vec3(1.0f)), 1.0f);
        instance.material = glm::vec4(
            0.0f,
            std::clamp(settings.roughness, 0.02f, 1.0f),
            1.0f,
            0.0f);
        m_PreparedInstances.push_back(instance);
        m_PreparedEntities.push_back(entity);
    }
}

void WaterRenderer::PrepareFromScene(const glm::vec3& cameraPosition,
                                     const std::array<Plane, 6>& frustumPlanes,
                                     bool useFrustumCulling) {
    Prepare(ECS::SceneECS::GetInstance().GetRootEntities(), cameraPosition,
            frustumPlanes, useFrustumCulling);
}

void WaterRenderer::CollectWaterEntities(ECS::Entity entity,
                                         std::vector<ECS::Entity>& entities) const {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& scene = ECS::SceneECS::GetInstance();
    if (coordinator.HasComponent<ECS::WaterComponent>(entity)) {
        entities.push_back(entity);
    }
    for (ECS::Entity child : scene.GetChildren(entity)) {
        CollectWaterEntities(child, entities);
    }
}

bool WaterRenderer::BuildMesh() {
    if (m_VertexBuffer.GetBuffer() != VK_NULL_HANDLE &&
        m_IndexBuffer.GetBuffer() != VK_NULL_HANDLE && m_IndexCount > 0) {
        return true;
    }

    std::vector<WaterVertex> vertices;
    std::vector<uint32_t> indices;
    vertices.resize(static_cast<size_t>(kMeshResolution) * kMeshResolution);

    for (uint32_t z = 0; z < kMeshResolution; ++z) {
        for (uint32_t x = 0; x < kMeshResolution; ++x) {
            const size_t index = static_cast<size_t>(z) * kMeshResolution + x;
            const glm::vec2 uv(
                static_cast<float>(x) / static_cast<float>(kMeshResolution - 1),
                static_cast<float>(z) / static_cast<float>(kMeshResolution - 1));
            vertices[index].uv = uv;
            vertices[index].position = uv - glm::vec2(0.5f);
        }
    }

    constexpr uint32_t kRestart = std::numeric_limits<uint32_t>::max();
    indices.reserve(static_cast<size_t>(kMeshResolution - 1) *
                    (static_cast<size_t>(kMeshResolution) * 2u + 1u));
    for (uint32_t z = 0; z + 1 < kMeshResolution; ++z) {
        if (z > 0) indices.push_back(kRestart);
        for (uint32_t x = 0; x < kMeshResolution; ++x) {
            // [top-left, bottom-left, top-right, bottom-right]；与 terrain
            // patch 一致，三角形正面朝局部 +Y。
            indices.push_back(z * kMeshResolution + x);
            indices.push_back((z + 1) * kMeshResolution + x);
        }
    }

    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    m_VertexBuffer.Cleanup();
    m_IndexBuffer.Cleanup();
    if (!m_VertexBuffer.Create(static_cast<VkDeviceSize>(vertices.size() * sizeof(WaterVertex)),
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory)) {
        return false;
    }
    if (!m_IndexBuffer.Create(static_cast<VkDeviceSize>(indices.size() * sizeof(uint32_t)),
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT, memory)) {
        m_VertexBuffer.Cleanup();
        return false;
    }
    m_VertexBuffer.Write(vertices.data(),
                         static_cast<VkDeviceSize>(vertices.size() * sizeof(WaterVertex)));
    m_IndexBuffer.Write(indices.data(),
                        static_cast<VkDeviceSize>(indices.size() * sizeof(uint32_t)));
    m_IndexCount = static_cast<uint32_t>(indices.size());
    return true;
}

bool WaterRenderer::CreateDescriptorResources() {
    if (m_DescriptorLayout != VK_NULL_HANDLE && m_DescriptorPool != VK_NULL_HANDLE) {
        return true;
    }
    if (g_Device == VK_NULL_HANDLE) return false;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(g_Device, &layoutInfo, g_Allocator,
                                    &m_DescriptorLayout) != VK_SUCCESS) {
        m_DescriptorLayout = VK_NULL_HANDLE;
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = static_cast<uint32_t>(kMaxDescriptorSets);
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = static_cast<uint32_t>(kMaxDescriptorSets);
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(g_Device, &poolInfo, g_Allocator, &m_DescriptorPool) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(g_Device, m_DescriptorLayout, g_Allocator);
        m_DescriptorLayout = VK_NULL_HANDLE;
        m_DescriptorPool = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void WaterRenderer::DestroyDescriptorResources() {
    if (g_Device == VK_NULL_HANDLE) {
        m_DescriptorPool = VK_NULL_HANDLE;
        m_DescriptorLayout = VK_NULL_HANDLE;
        return;
    }
    if (m_DescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(g_Device, m_DescriptorPool, g_Allocator);
        m_DescriptorPool = VK_NULL_HANDLE;
    }
    if (m_DescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(g_Device, m_DescriptorLayout, g_Allocator);
        m_DescriptorLayout = VK_NULL_HANDLE;
    }
}

bool WaterRenderer::CreateUniformBuffers() {
    if (g_Device == VK_NULL_HANDLE) return false;
    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (auto& uniform : m_UniformBuffers) {
        if (!uniform) uniform = std::make_unique<VulkanBuffer>();
        if (uniform->GetBuffer() == VK_NULL_HANDLE &&
            !uniform->Create(sizeof(WaterUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             memory)) {
            return false;
        }
    }
    return true;
}

bool WaterRenderer::CreateDescriptorSets() {
    if (g_Device == VK_NULL_HANDLE || m_DescriptorLayout == VK_NULL_HANDLE ||
        m_DescriptorPool == VK_NULL_HANDLE) {
        return false;
    }
    if (m_DescriptorSets[0] != VK_NULL_HANDLE) return true;

    std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{};
    layouts.fill(m_DescriptorLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_DescriptorPool;
    allocInfo.descriptorSetCount = kFramesInFlight;
    allocInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(g_Device, &allocInfo, m_DescriptorSets.data()) != VK_SUCCESS) {
        m_DescriptorSets.fill(VK_NULL_HANDLE);
        return false;
    }

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_UniformBuffers[frame]->GetBuffer();
        bufferInfo.range = sizeof(WaterUniformData);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_DescriptorSets[frame];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(g_Device, 1, &write, 0, nullptr);
    }
    return true;
}

bool WaterRenderer::CreatePipelines() {
    if (m_RenderPass == VK_NULL_HANDLE || m_DescriptorLayout == VK_NULL_HANDLE) {
        return false;
    }

    std::array<VkVertexInputBindingDescription, 2> bindings = {
        MakeVertexBinding(0, sizeof(WaterVertex), VK_VERTEX_INPUT_RATE_VERTEX),
        MakeVertexBinding(1, sizeof(WaterInstance), VK_VERTEX_INPUT_RATE_INSTANCE)
    };
    std::array<VkVertexInputAttributeDescription, 12> attributes{};
    attributes[0] = MakeVertexAttribute(0, 0, VK_FORMAT_R32G32_SFLOAT,
                                        offsetof(WaterVertex, position));
    attributes[1] = MakeVertexAttribute(1, 0, VK_FORMAT_R32G32_SFLOAT,
                                        offsetof(WaterVertex, uv));
    for (uint32_t i = 0; i < 4; ++i) {
        attributes[2 + i] = MakeVertexAttribute(
            2 + i, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
            static_cast<uint32_t>(offsetof(WaterInstance, model) + sizeof(glm::vec4) * i));
        attributes[6 + i] = MakeVertexAttribute(
            6 + i, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
            static_cast<uint32_t>(offsetof(WaterInstance, previousModel) + sizeof(glm::vec4) * i));
    }
    attributes[10] = MakeVertexAttribute(10, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                         offsetof(WaterInstance, color));
    attributes[11] = MakeVertexAttribute(11, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                         offsetof(WaterInstance, material));

    PipelineConfig geometry{};
    geometry.vertShader = "water.vert.spv";
    geometry.fragShader = "water.frag.spv";
    geometry.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    geometry.primitiveRestartEnable = m_PrimitiveRestartSupported;
    geometry.cullMode = VK_CULL_MODE_NONE; // 水面原型支持从上下两侧观察
    geometry.depthTest = true;
    geometry.depthWrite = !g_EnableZPrepass;
    geometry.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    geometry.blending = false; // 第一阶段明确不接入透明混合
    geometry.colorAttachmentCount = 4;
    geometry.subpass = 1;
    geometry.vertexBindings.assign(bindings.begin(), bindings.end());
    geometry.vertexAttributes.assign(attributes.begin(), attributes.end());
    if (!m_Pipeline.Create(m_RenderPass, m_DescriptorLayout, geometry)) {
        return false;
    }

    PipelineConfig depth = geometry;
    depth.vertShader = "water.vert.spv";
    depth.fragShader = "water_depth.frag.spv";
    depth.depthWrite = true;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    depth.colorAttachmentCount = 0;
    depth.subpass = 0;
    if (!m_DepthPipeline.Create(m_RenderPass, m_DescriptorLayout, depth)) {
        m_Pipeline.Cleanup();
        return false;
    }
    return true;
}

bool WaterRenderer::EnsureInstanceCapacity(size_t instanceCount) {
    if (instanceCount <= m_InstanceCapacity) return true;
    if (g_Device != VK_NULL_HANDLE) vkDeviceWaitIdle(g_Device);

    const size_t newCapacity = std::max(instanceCount,
                                        std::max<size_t>(1, m_InstanceCapacity * 2));
    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (auto& buffer : m_InstanceBuffers) {
        buffer.Cleanup();
        if (!buffer.Create(static_cast<VkDeviceSize>(newCapacity * sizeof(WaterInstance)),
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory)) {
            for (auto& cleanup : m_InstanceBuffers) cleanup.Cleanup();
            m_InstanceCapacity = 0;
            return false;
        }
    }
    m_InstanceCapacity = newCapacity;
    return true;
}

void WaterRenderer::Render(VkCommandBuffer commandBuffer, int width, int height,
                           const glm::mat4& projView,
                           const glm::mat4& prevProjView,
                           const glm::vec3& cameraPosition) {
    RenderInternal(commandBuffer, width, height, projView, prevProjView,
                   cameraPosition, false);
}

void WaterRenderer::RenderDepthPrepass(VkCommandBuffer commandBuffer, int width, int height,
                                       const glm::mat4& projView,
                                       const glm::vec3& cameraPosition) {
    RenderInternal(commandBuffer, width, height, projView, projView,
                   cameraPosition, true);
}

void WaterRenderer::RenderInternal(VkCommandBuffer commandBuffer, int width, int height,
                                   const glm::mat4& projView,
                                   const glm::mat4& prevProjView,
                                   const glm::vec3& cameraPosition,
                                   bool depthOnly) {
    if (commandBuffer == VK_NULL_HANDLE || width <= 0 || height <= 0 ||
        m_PreparedInstances.empty() || m_IndexCount == 0) {
        return;
    }

    const VkPipeline pipeline = depthOnly ? m_DepthPipeline.GetPipeline()
                                          : m_Pipeline.GetPipeline();
    const VkPipelineLayout pipelineLayout = depthOnly ? m_DepthPipeline.GetLayout()
                                                      : m_Pipeline.GetLayout();
    if (pipeline == VK_NULL_HANDLE || pipelineLayout == VK_NULL_HANDLE ||
        !EnsureInstanceCapacity(m_PreparedInstances.size())) {
        return;
    }

    const uint32_t frame = GetCurrentFrameIndex() % kFramesInFlight;
    m_InstanceBuffers[frame].Write(
        m_PreparedInstances.data(),
        static_cast<VkDeviceSize>(m_PreparedInstances.size() * sizeof(WaterInstance)));

    WaterUniformData uniform;
    uniform.projView = projView;
    uniform.prevProjView = prevProjView;
    uniform.cameraPosition = glm::vec4(cameraPosition, 1.0f);
    uniform.taaJitter = glm::vec4(g_CurrentTAAJitter, 0.0f, 0.0f);
    m_UniformBuffers[frame]->Write(&uniform, sizeof(uniform));

    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, 0, 1, &m_DescriptorSets[frame], 0, nullptr);

    VkBuffer vertexBuffers[2] = {
        m_VertexBuffer.GetBuffer(), m_InstanceBuffers[frame].GetBuffer()
    };
    VkDeviceSize offsets[2] = {0, 0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, m_IndexBuffer.GetBuffer(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(commandBuffer, m_IndexCount,
                     static_cast<uint32_t>(m_PreparedInstances.size()), 0, 0, 0);

    if (!depthOnly) {
        for (size_t i = 0; i < m_PreparedEntities.size(); ++i) {
            m_PreviousModels[m_PreparedEntities[i]] = m_PreparedInstances[i].model;
        }
    }
}
