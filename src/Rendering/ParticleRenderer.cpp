#include "ParticleRenderer.h"

#include "Core/RenderGlobals.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanManager.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>

#include <glm/gtc/matrix_inverse.hpp>

namespace {

constexpr uint32_t kFramesInFlight = 3;
constexpr uint32_t kMaxOverlayDrawsPerFrame = 4;
constexpr size_t kMaxDescriptorSets =
    static_cast<size_t>(kFramesInFlight) *
    static_cast<size_t>(kMaxOverlayDrawsPerFrame);

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

} // namespace

ParticleRenderer::~ParticleRenderer() {
    Cleanup();
}

bool ParticleRenderer::EnsureInitialized(VkRenderPass renderPass, uint32_t subpass) {
    if (renderPass == VK_NULL_HANDLE || g_Device == VK_NULL_HANDLE) return false;

    for (auto& pipelineSet : m_PipelineSets) {
        if (pipelineSet && pipelineSet->renderPass == renderPass &&
            pipelineSet->subpass == subpass &&
            pipelineSet->alpha.GetPipeline() != VK_NULL_HANDLE &&
            pipelineSet->additive.GetPipeline() != VK_NULL_HANDLE) {
            m_ActivePipelineSet = pipelineSet.get();
            return true;
        }
    }

    if (!CreateDescriptorResources() || !CreateUniformBuffers() ||
        !CreateDescriptorSets() || !CreateInstanceBuffers()) {
        std::printf("[ParticleRenderer] initialization failed\n");
        return false;
    }

    auto pipelineSet = std::make_unique<PipelineSet>();
    pipelineSet->renderPass = renderPass;
    pipelineSet->subpass = subpass;
    if (!CreatePipelines(*pipelineSet, subpass)) {
        std::printf("[ParticleRenderer] pipeline initialization failed\n");
        return false;
    }
    m_ActivePipelineSet = pipelineSet.get();
    m_PipelineSets.push_back(std::move(pipelineSet));
    return true;
}

bool ParticleRenderer::IsInitialized() const {
    return m_ActivePipelineSet != nullptr &&
           m_ActivePipelineSet->alpha.GetPipeline() != VK_NULL_HANDLE;
}

void ParticleRenderer::Cleanup() {
    // VulkanManager calls this before destroying g_Device.  The guard also
    // keeps the static object's late destructor harmless after device teardown.
    if (g_Device == VK_NULL_HANDLE) {
        m_DescriptorSets = {};
        m_InstanceCapacities = {};
        m_AlphaInstances.clear();
        m_AdditiveInstances.clear();
        m_UploadInstances.clear();
        return;
    }

    vkDeviceWaitIdle(g_Device);
    for (auto& frameBuffers : m_InstanceBuffers) {
        for (VulkanBuffer& buffer : frameBuffers) buffer.Cleanup();
    }
    for (auto& frameUniforms : m_UniformBuffers) {
        for (auto& uniform : frameUniforms) uniform.reset();
    }
    m_DescriptorSets = {};
    m_InstanceCapacities = {};

    for (auto& pipelineSet : m_PipelineSets) {
        if (!pipelineSet) continue;
        pipelineSet->alpha.Cleanup();
        pipelineSet->additive.Cleanup();
    }
    m_PipelineSets.clear();
    m_ActivePipelineSet = nullptr;
    DestroyDescriptorResources();
    m_LastFrameSlot = UINT32_MAX;
    m_DrawIndex = 0;
    m_AlphaInstances.clear();
    m_AdditiveInstances.clear();
    m_UploadInstances.clear();
}

bool ParticleRenderer::CreateDescriptorResources() {
    if (m_DescriptorLayout != VK_NULL_HANDLE && m_DescriptorPool != VK_NULL_HANDLE) {
        return true;
    }
    if (g_Device == VK_NULL_HANDLE) return false;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

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

void ParticleRenderer::DestroyDescriptorResources() {
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

bool ParticleRenderer::CreateUniformBuffers() {
    if (g_Device == VK_NULL_HANDLE) return false;
    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (auto& frameUniforms : m_UniformBuffers) {
        for (auto& uniform : frameUniforms) {
            if (!uniform) uniform = std::make_unique<VulkanBuffer>();
            if (uniform->GetBuffer() == VK_NULL_HANDLE &&
                !uniform->Create(sizeof(UniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                 memory)) {
                return false;
            }
        }
    }
    return true;
}

bool ParticleRenderer::CreateDescriptorSets() {
    if (g_Device == VK_NULL_HANDLE || m_DescriptorLayout == VK_NULL_HANDLE ||
        m_DescriptorPool == VK_NULL_HANDLE) {
        return false;
    }
    if (m_DescriptorSets[0][0] != VK_NULL_HANDLE) return true;

    std::array<VkDescriptorSetLayout, kMaxDescriptorSets> layouts{};
    layouts.fill(m_DescriptorLayout);
    std::array<VkDescriptorSet, kMaxDescriptorSets> sets{};
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_DescriptorPool;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(kMaxDescriptorSets);
    allocInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(g_Device, &allocInfo, sets.data()) != VK_SUCCESS) {
        m_DescriptorSets = {};
        return false;
    }

    size_t index = 0;
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        for (uint32_t draw = 0; draw < kMaxOverlayDrawsPerFrame; ++draw) {
            m_DescriptorSets[frame][draw] = sets[index++];
            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = m_UniformBuffers[frame][draw]->GetBuffer();
            bufferInfo.range = sizeof(UniformData);

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_DescriptorSets[frame][draw];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.pBufferInfo = &bufferInfo;
            vkUpdateDescriptorSets(g_Device, 1, &write, 0, nullptr);
        }
    }
    return true;
}

bool ParticleRenderer::CreateInstanceBuffers() {
    if (g_Device == VK_NULL_HANDLE) return false;
    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        for (uint32_t draw = 0; draw < kMaxOverlayDrawsPerFrame; ++draw) {
            if (m_InstanceBuffers[frame][draw].GetBuffer() != VK_NULL_HANDLE) continue;
            if (!m_InstanceBuffers[frame][draw].Create(
                    static_cast<VkDeviceSize>(kInitialInstanceCapacity * sizeof(ParticleInstance)),
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory)) {
                return false;
            }
            m_InstanceCapacities[frame][draw] = kInitialInstanceCapacity;
        }
    }
    return true;
}

bool ParticleRenderer::CreatePipelines(PipelineSet& pipelineSet, uint32_t subpass) {
    if (pipelineSet.renderPass == VK_NULL_HANDLE || m_DescriptorLayout == VK_NULL_HANDLE) {
        return false;
    }

    std::array<VkVertexInputBindingDescription, 1> bindings = {
        MakeVertexBinding(0, sizeof(ParticleInstance), VK_VERTEX_INPUT_RATE_INSTANCE)
    };
    std::array<VkVertexInputAttributeDescription, 3> attributes = {
        MakeVertexAttribute(0, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                            offsetof(ParticleInstance, positionSize)),
        MakeVertexAttribute(1, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                            offsetof(ParticleInstance, color)),
        MakeVertexAttribute(2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                            offsetof(ParticleInstance, rotationBlend)),
    };

    PipelineConfig alpha{};
    alpha.vertShader = "particle.vert.spv";
    alpha.fragShader = "particle.frag.spv";
    // particle.vert emits six vertices for two explicit triangles.  Keeping
    // this as a list avoids any shared-diagonal interpolation ambiguity.
    alpha.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    alpha.cullMode = VK_CULL_MODE_NONE;
    alpha.depthTest = true;
    alpha.depthWrite = false;
    alpha.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    alpha.blending = true;
    alpha.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    alpha.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    alpha.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    alpha.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    alpha.colorAttachmentCount = 1;
    alpha.subpass = subpass;
    alpha.vertexBindings.assign(bindings.begin(), bindings.end());
    alpha.vertexAttributes.assign(attributes.begin(), attributes.end());
    if (!pipelineSet.alpha.Create(pipelineSet.renderPass, m_DescriptorLayout, alpha)) return false;

    PipelineConfig additive = alpha;
    additive.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    additive.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    if (!pipelineSet.additive.Create(pipelineSet.renderPass, m_DescriptorLayout, additive)) {
        pipelineSet.alpha.Cleanup();
        return false;
    }
    return true;
}

bool ParticleRenderer::EnsureInstanceCapacity(uint32_t frameSlot, uint32_t drawSlot,
                                               size_t instanceCount) {
    if (frameSlot >= kFramesInFlight || drawSlot >= kMaxOverlayDrawsPerFrame) return false;
    size_t& capacity = m_InstanceCapacities[frameSlot][drawSlot];
    if (instanceCount <= capacity) return true;

    // This path is exceptional because normal emitters fit the initial capacity.
    // Wait before replacing a host-visible buffer that may still be referenced by
    // an older frame; the initial allocation never enters this branch.
    if (g_Device != VK_NULL_HANDLE) vkDeviceWaitIdle(g_Device);
    const size_t newCapacity = std::max(instanceCount, std::max<size_t>(capacity * 2, 1));
    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    m_InstanceBuffers[frameSlot][drawSlot].Cleanup();
    if (!m_InstanceBuffers[frameSlot][drawSlot].Create(
            static_cast<VkDeviceSize>(newCapacity * sizeof(ParticleInstance)),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory)) {
        capacity = 0;
        return false;
    }
    capacity = newCapacity;
    return true;
}

void ParticleRenderer::Render(VkCommandBuffer commandBuffer, uint32_t width, uint32_t height,
                              VkRenderPass renderPass, uint32_t subpass,
                              const glm::mat4& view, const glm::mat4& proj,
                              const glm::vec3& cameraPosition, const glm::vec2& taaJitter) {
    if (commandBuffer == VK_NULL_HANDLE || width == 0 || height == 0) return;

    const auto& allInstances = ParticleSystem::GetInstance().GetRenderInstances();
    if (allInstances.empty() || !EnsureInitialized(renderPass, subpass) ||
        m_ActivePipelineSet == nullptr) return;

    const uint32_t frameSlot = GetCurrentFrameIndex() % kFramesInFlight;
    if (m_LastFrameSlot != frameSlot) {
        m_LastFrameSlot = frameSlot;
        m_DrawIndex = 0;
    }
    if (m_DrawIndex >= kMaxOverlayDrawsPerFrame) {
        return;
    }
    const uint32_t drawSlot = m_DrawIndex++;

    m_AlphaInstances.clear();
    m_AdditiveInstances.clear();
    m_AlphaInstances.reserve(allInstances.size());
    m_AdditiveInstances.reserve(allInstances.size());
    for (const ParticleInstance& instance : allInstances) {
        if (instance.rotationBlend.y >= 0.5f) {
            m_AdditiveInstances.push_back(instance);
        } else {
            m_AlphaInstances.push_back(instance);
        }
    }

    const auto backToFront = [&cameraPosition](const ParticleInstance& lhs,
                                                 const ParticleInstance& rhs) {
        const glm::vec3 lhsPosition(lhs.positionSize);
        const glm::vec3 rhsPosition(rhs.positionSize);
        return glm::dot(lhsPosition - cameraPosition, lhsPosition - cameraPosition) >
               glm::dot(rhsPosition - cameraPosition, rhsPosition - cameraPosition);
    };
    std::sort(m_AlphaInstances.begin(), m_AlphaInstances.end(), backToFront);

    m_UploadInstances.clear();
    m_UploadInstances.reserve(m_AlphaInstances.size() + m_AdditiveInstances.size());
    m_UploadInstances.insert(m_UploadInstances.end(), m_AlphaInstances.begin(),
                             m_AlphaInstances.end());
    const size_t additiveOffset = m_UploadInstances.size();
    m_UploadInstances.insert(m_UploadInstances.end(), m_AdditiveInstances.begin(),
                             m_AdditiveInstances.end());
    if (m_UploadInstances.empty() ||
        !EnsureInstanceCapacity(frameSlot, drawSlot, m_UploadInstances.size())) {
        return;
    }

    UniformData uniform;
    uniform.viewProj = proj * view;
    const glm::mat4 inverseView = glm::inverse(view);
    uniform.cameraRight = glm::vec4(glm::normalize(glm::vec3(inverseView[0])), 0.0f);
    uniform.cameraUp = glm::vec4(glm::normalize(glm::vec3(inverseView[1])), 0.0f);
    uniform.taaJitter = glm::vec4(taaJitter, 0.0f, 0.0f);

    m_InstanceBuffers[frameSlot][drawSlot].Write(
        m_UploadInstances.data(),
        static_cast<VkDeviceSize>(m_UploadInstances.size() * sizeof(ParticleInstance)));
    m_UniformBuffers[frameSlot][drawSlot]->Write(&uniform, sizeof(uniform));

    if (!m_AlphaInstances.empty()) {
        RenderBatch(commandBuffer, width, height, frameSlot, drawSlot, 0,
                    m_AlphaInstances.size(), m_ActivePipelineSet->alpha);
    }
    if (!m_AdditiveInstances.empty()) {
        RenderBatch(commandBuffer, width, height, frameSlot, drawSlot, additiveOffset,
                    m_AdditiveInstances.size(), m_ActivePipelineSet->additive);
    }
}

void ParticleRenderer::RenderBatch(VkCommandBuffer commandBuffer, uint32_t width,
                                   uint32_t height, uint32_t frameSlot, uint32_t drawSlot,
                                   size_t instanceOffset, size_t instanceCount,
                                   VulkanPipeline& pipeline) {
    if (instanceCount == 0 || pipeline.GetPipeline() == VK_NULL_HANDLE ||
        pipeline.GetLayout() == VK_NULL_HANDLE) {
        return;
    }

    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {width, height};

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.GetPipeline());
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    const VkDescriptorSet descriptorSet = m_DescriptorSets[frameSlot][drawSlot];
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline.GetLayout(), 0, 1, &descriptorSet, 0, nullptr);

    const VkBuffer instanceBuffer = m_InstanceBuffers[frameSlot][drawSlot].GetBuffer();
    const VkDeviceSize offset = static_cast<VkDeviceSize>(instanceOffset * sizeof(ParticleInstance));
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &instanceBuffer, &offset);
    vkCmdDraw(commandBuffer, 6, static_cast<uint32_t>(instanceCount), 0, 0);
}
