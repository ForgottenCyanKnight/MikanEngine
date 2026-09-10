#include "Rendering/GpuSphSimulation.h"

#include "Core/EngineConfig.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanManager.h"
#include "Rendering/GpuSphContainerGeometry.h"
#include "Rendering/ParticleSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr float kFixedStep = 1.0f / 120.0f;
constexpr float kSmoothingRadius = 0.14f;
constexpr float kRestDensity = 700.0f;
constexpr float kParticleMass = 0.17f;
constexpr float kPressureStiffness = 350.0f;
constexpr float kViscosity = 0.12f;
constexpr float kMaxAcceleration = 60.0f;
constexpr float kMaxVelocity = 6.0f;
constexpr float kParticleSize = 0.050f;
constexpr float kParticleSpacing = 0.0625f;
constexpr float kShortRangeRepulsion = 8.0f;
constexpr float kBoundaryRepulsion = 12.0f;
constexpr float kPi = 3.14159265358979323846f;
const glm::vec3 kContainerCenter = GpuSphContainerGeometry::GetCenter();
const glm::vec3 kContainerHalfExtents =
    GpuSphContainerGeometry::GetHalfExtents();

struct HostParticleState {
    glm::vec4 position;
    glm::vec4 velocity;
};

struct HostParticleInstance {
    glm::vec4 positionSize;
    glm::vec4 color;
    glm::vec4 rotationBlend;
};

static_assert(sizeof(HostParticleInstance) == sizeof(ParticleInstance),
              "GPU SPH render buffer must match ParticleInstance");

glm::vec3 LocalToWorld(const glm::vec3& localPosition,
                       const glm::quat& containerRotation) {
    return kContainerCenter +
           containerRotation * (localPosition - kContainerCenter);
}

void InsertBufferBarrier(VkCommandBuffer commandBuffer, VkBuffer buffer,
                         VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                         VkPipelineStageFlags srcStage,
                         VkPipelineStageFlags dstStage) {
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0,
                         0, nullptr, 1, &barrier, 0, nullptr);
}

VkDescriptorSetLayoutBinding MakeStorageBinding(uint32_t binding) {
    VkDescriptorSetLayoutBinding result{};
    result.binding = binding;
    result.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    result.descriptorCount = 1;
    result.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    return result;
}

} // namespace

GpuSphSimulation& GpuSphSimulation::GetInstance() {
    static GpuSphSimulation instance;
    return instance;
}

GpuSphSimulation::~GpuSphSimulation() {
    Cleanup();
}

VkBuffer GpuSphSimulation::GetRenderBuffer() const {
    if (!m_initialized) return VK_NULL_HANDLE;
    return m_RenderBuffers[m_currentStateIndex].GetBuffer();
}

glm::vec3 GpuSphSimulation::GetContainerCenter() const {
    return kContainerCenter;
}

glm::vec3 GpuSphSimulation::GetContainerHalfExtents() const {
    return kContainerHalfExtents;
}

void GpuSphSimulation::SetContainerRotation(const glm::quat& rotation) {
    if (glm::length(rotation) <= 0.0001f) return;
    const glm::quat normalizedRotation = glm::normalize(rotation);
    // q and -q represent the same orientation. Avoid rebuilding the render
    // buffer when the scene system only changes quaternion sign.
    if (std::abs(glm::dot(m_containerRotation, normalizedRotation)) < 0.99999f) {
        m_containerRotation = normalizedRotation;
        m_containerTransformDirty = true;
    }
}

void GpuSphSimulation::ResetContainerRotation() {
    m_containerRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    m_containerTransformDirty = true;
}

void GpuSphSimulation::Reset() {
    m_resetRequested = true;
    m_containerTransformDirty = true;
    m_timeAccumulator = 0.0f;
}

bool GpuSphSimulation::Record(VkCommandBuffer commandBuffer, float deltaSeconds) {
    if (commandBuffer == VK_NULL_HANDLE) return false;

    if (!m_initialized && !Initialize()) return false;

    if (m_resetRequested) {
        // Reset can arrive from an SDL event while the previous submission is
        // still in flight. The reset path is rare, so wait once before
        // reseeding all ring buffers instead of risking a host/GPU race.
        vkDeviceWaitIdle(g_Device);
        SeedBuffers();
        m_currentStateIndex = 0;
        m_timeAccumulator = 0.0f;
        m_resetRequested = false;
        m_containerTransformDirty = false;
    }

    // Keep the seeded first frame available to the renderer, but do not
    // advance the solver or record transform-only compute work until play
    // starts. This mirrors CPU particles, which exist in their initial state
    // before the gameplay update loop begins.
    if (!m_playbackActive) return true;

    bool transformOnly = false;
    uint32_t substeps = 0;
    if (m_paused) {
        if (!m_containerTransformDirty) return true;
        transformOnly = true;
    } else {
        m_timeAccumulator = std::min(
            m_timeAccumulator + std::clamp(deltaSeconds, 0.0f, 0.1f), 0.25f);
        substeps = std::min<uint32_t>(
            static_cast<uint32_t>(m_timeAccumulator / kFixedStep), 4u);
        if (substeps == 0) {
            if (!m_containerTransformDirty) return true;
            transformOnly = true;
        } else {
            m_timeAccumulator -= static_cast<float>(substeps) * kFixedStep;
        }
    }

    const uint32_t dispatchCount = (kParticleCount + kWorkgroupSize - 1) /
                                   kWorkgroupSize;
    const uint32_t recordedSteps = transformOnly ? 1u : substeps;
    const glm::vec3 worldGravity(0.0f, -9.8f, 0.0f);
    const glm::vec3 localGravity =
        glm::inverse(m_containerRotation) * worldGravity;

    for (uint32_t step = 0; step < recordedSteps; ++step) {
        const uint32_t inputIndex = m_currentStateIndex;
        const uint32_t outputIndex = (inputIndex + 1) % kFramesInFlight;

        SimParams params{};
        params.simulation0 = glm::vec4(
            transformOnly ? 0.0f : kFixedStep,
            static_cast<float>(kParticleCount),
            kSmoothingRadius, kRestDensity);
        params.simulation1 = glm::vec4(
            kParticleMass, kPressureStiffness, kViscosity,
            kMaxAcceleration);
        params.simulation2 = glm::vec4(
            kMaxVelocity, kParticleSize, kShortRangeRepulsion,
            kBoundaryRepulsion);
        params.gravity = glm::vec4(localGravity, 0.0f);
        params.lowerBound = glm::vec4(
            -GpuSphContainerGeometry::kChamberHalfExtent,
            GpuSphContainerGeometry::kBottomY,
            -GpuSphContainerGeometry::kChamberHalfExtent,
            GpuSphContainerGeometry::kStep1Y);
        params.upperBound = glm::vec4(
            GpuSphContainerGeometry::kChamberHalfExtent,
            GpuSphContainerGeometry::kTopY,
            GpuSphContainerGeometry::kChamberHalfExtent,
            GpuSphContainerGeometry::kStep2Y);
        params.containerRotation = glm::vec4(
            m_containerRotation.x, m_containerRotation.y,
            m_containerRotation.z, m_containerRotation.w);
        params.containerProfile = glm::vec4(
            GpuSphContainerGeometry::kStep3Y,
            GpuSphContainerGeometry::kStep1HalfExtent,
            GpuSphContainerGeometry::kStep2HalfExtent,
            GpuSphContainerGeometry::kOutletHalfExtent);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          m_DensityPipeline);
        vkCmdPushConstants(commandBuffer, m_DensityPipelineLayout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SimParams),
                           &params);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_DensityPipelineLayout, 0, 1,
                                &m_DensityDescriptorSets[inputIndex], 0,
                                nullptr);
        vkCmdDispatch(commandBuffer, dispatchCount, 1, 1);

        InsertBufferBarrier(
            commandBuffer, m_DensityBuffers[inputIndex].GetBuffer(),
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          m_IntegratePipeline);
        vkCmdPushConstants(commandBuffer, m_IntegratePipelineLayout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SimParams),
                           &params);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_IntegratePipelineLayout, 0, 1,
                                &m_IntegrateDescriptorSets[inputIndex], 0,
                                nullptr);
        vkCmdDispatch(commandBuffer, dispatchCount, 1, 1);

        VkBufferMemoryBarrier barriers[2]{};
        for (VkBufferMemoryBarrier& barrier : barriers) {
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
        }
        barriers[0].buffer = m_StateBuffers[outputIndex].GetBuffer();
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[1].buffer = m_RenderBuffers[outputIndex].GetBuffer();
        barriers[1].dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        vkCmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
            0, 0, nullptr, 2, barriers, 0, nullptr);

        m_currentStateIndex = outputIndex;
    }
    m_containerTransformDirty = false;
    return true;
}

bool GpuSphSimulation::Initialize() {
    if (m_initialized) return true;
    if (g_Device == VK_NULL_HANDLE) return false;

    if (!CreateBuffers() || !CreateDescriptorResources() ||
        !CreatePipelines() || !AllocateDescriptorSets()) {
        Cleanup();
        return false;
    }

    SeedBuffers();
    m_currentStateIndex = 0;
    m_timeAccumulator = 0.0f;
    m_resetRequested = false;
    m_containerTransformDirty = false;
    m_initialized = true;
    std::printf("[GpuSph] initialized: particles=%u, fixedStep=%.5f\n",
                kParticleCount, kFixedStep);
    return true;
}

bool GpuSphSimulation::CreateBuffers() {
    const VkMemoryPropertyFlags memory =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkDeviceSize stateSize = sizeof(HostParticleState) * kParticleCount;
    const VkDeviceSize renderSize = sizeof(HostParticleInstance) * kParticleCount;
    const VkDeviceSize densitySize = sizeof(glm::vec4) * kParticleCount;

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!m_StateBuffers[i].Create(
                stateSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory)) return false;
        if (!m_RenderBuffers[i].Create(
                renderSize,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                memory)) return false;
        if (!m_DensityBuffers[i].Create(
                densitySize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory)) return false;
    }
    return true;
}

bool GpuSphSimulation::CreateDescriptorResources() {
    std::array<VkDescriptorSetLayoutBinding, 2> densityBindings = {
        MakeStorageBinding(0), MakeStorageBinding(1)};
    VkDescriptorSetLayoutCreateInfo densityInfo{};
    densityInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    densityInfo.bindingCount = static_cast<uint32_t>(densityBindings.size());
    densityInfo.pBindings = densityBindings.data();
    if (vkCreateDescriptorSetLayout(g_Device, &densityInfo, g_Allocator,
                                    &m_DensityDescriptorLayout) != VK_SUCCESS) {
        return false;
    }

    std::array<VkDescriptorSetLayoutBinding, 4> integrateBindings = {
        MakeStorageBinding(0), MakeStorageBinding(1),
        MakeStorageBinding(2), MakeStorageBinding(3)};
    VkDescriptorSetLayoutCreateInfo integrateInfo{};
    integrateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    integrateInfo.bindingCount = static_cast<uint32_t>(integrateBindings.size());
    integrateInfo.pBindings = integrateBindings.data();
    if (vkCreateDescriptorSetLayout(g_Device, &integrateInfo, g_Allocator,
                                    &m_IntegrateDescriptorLayout) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize storagePool{};
    storagePool.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    storagePool.descriptorCount = kFramesInFlight * 6;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = kFramesInFlight * 2;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &storagePool;
    return vkCreateDescriptorPool(g_Device, &poolInfo, g_Allocator,
                                  &m_DescriptorPool) == VK_SUCCESS;
}

bool GpuSphSimulation::CreateShaderPipeline(
    const char* shaderName, VkDescriptorSetLayout descriptorLayout,
    VkPipelineLayout& pipelineLayout, VkPipeline& pipeline) {
    const std::string shaderPath = EngineConfig::GetShaderPath(shaderName);
    const std::vector<char> code = RendererUtils::ReadFile(shaderPath);
    VkShaderModule module = RendererUtils::CreateShaderModule(code, shaderName);
    if (module == VK_NULL_HANDLE) return false;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(SimParams);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(g_Device, &layoutInfo, g_Allocator,
                               &pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, module, g_Allocator);
        return false;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stage;
    pipelineInfo.layout = pipelineLayout;
    const VkResult result = vkCreateComputePipelines(
        g_Device, VK_NULL_HANDLE, 1, &pipelineInfo, g_Allocator, &pipeline);
    vkDestroyShaderModule(g_Device, module, g_Allocator);
    return result == VK_SUCCESS;
}

bool GpuSphSimulation::CreatePipelines() {
    if (!CreateShaderPipeline("sph_density.comp.spv", m_DensityDescriptorLayout,
                             m_DensityPipelineLayout, m_DensityPipeline)) {
        return false;
    }
    if (!CreateShaderPipeline("sph_integrate.comp.spv", m_IntegrateDescriptorLayout,
                             m_IntegratePipelineLayout, m_IntegratePipeline)) {
        return false;
    }
    return true;
}

bool GpuSphSimulation::AllocateDescriptorSets() {
    std::array<VkDescriptorSetLayout, kFramesInFlight> densityLayouts{};
    densityLayouts.fill(m_DensityDescriptorLayout);
    VkDescriptorSetAllocateInfo densityAlloc{};
    densityAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    densityAlloc.descriptorPool = m_DescriptorPool;
    densityAlloc.descriptorSetCount = kFramesInFlight;
    densityAlloc.pSetLayouts = densityLayouts.data();
    if (vkAllocateDescriptorSets(g_Device, &densityAlloc,
                                 m_DensityDescriptorSets.data()) != VK_SUCCESS) {
        return false;
    }

    std::array<VkDescriptorSetLayout, kFramesInFlight> integrateLayouts{};
    integrateLayouts.fill(m_IntegrateDescriptorLayout);
    VkDescriptorSetAllocateInfo integrateAlloc{};
    integrateAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    integrateAlloc.descriptorPool = m_DescriptorPool;
    integrateAlloc.descriptorSetCount = kFramesInFlight;
    integrateAlloc.pSetLayouts = integrateLayouts.data();
    if (vkAllocateDescriptorSets(g_Device, &integrateAlloc,
                                 m_IntegrateDescriptorSets.data()) != VK_SUCCESS) {
        return false;
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        const uint32_t outputIndex = (i + 1) % kFramesInFlight;
        VkDescriptorBufferInfo stateInfo{m_StateBuffers[i].GetBuffer(), 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo densityInfo{m_DensityBuffers[i].GetBuffer(), 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo outputStateInfo{m_StateBuffers[outputIndex].GetBuffer(), 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo outputRenderInfo{m_RenderBuffers[outputIndex].GetBuffer(), 0, VK_WHOLE_SIZE};

        std::array<VkWriteDescriptorSet, 2> densityWrites{};
        for (VkWriteDescriptorSet& write : densityWrites) {
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_DensityDescriptorSets[i];
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        densityWrites[0].dstBinding = 0;
        densityWrites[0].pBufferInfo = &stateInfo;
        densityWrites[1].dstBinding = 1;
        densityWrites[1].pBufferInfo = &densityInfo;
        vkUpdateDescriptorSets(g_Device,
                               static_cast<uint32_t>(densityWrites.size()),
                               densityWrites.data(), 0, nullptr);

        std::array<VkWriteDescriptorSet, 4> integrateWrites{};
        for (VkWriteDescriptorSet& write : integrateWrites) {
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_IntegrateDescriptorSets[i];
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        integrateWrites[0].dstBinding = 0;
        integrateWrites[0].pBufferInfo = &stateInfo;
        integrateWrites[1].dstBinding = 1;
        integrateWrites[1].pBufferInfo = &densityInfo;
        integrateWrites[2].dstBinding = 2;
        integrateWrites[2].pBufferInfo = &outputStateInfo;
        integrateWrites[3].dstBinding = 3;
        integrateWrites[3].pBufferInfo = &outputRenderInfo;
        vkUpdateDescriptorSets(g_Device,
                               static_cast<uint32_t>(integrateWrites.size()),
                               integrateWrites.data(), 0, nullptr);
    }
    return true;
}

void GpuSphSimulation::SeedBuffers() {
    std::vector<HostParticleState> states(kParticleCount);
    std::vector<HostParticleInstance> instances(kParticleCount);
    // Use a regular 16 x 32 x 16 block so the larger particle count starts as
    // a coherent volume inside the chamber and has room to fall through the
    // stepped observation section.
    constexpr uint32_t countX = 16;
    constexpr uint32_t countY = 32;
    constexpr uint32_t countZ = 16;
    const glm::vec3 start(-0.46875f, 0.25f, -0.46875f);
    const glm::vec3 spacing(kParticleSpacing);

    uint32_t index = 0;
    for (uint32_t z = 0; z < countZ; ++z) {
        for (uint32_t y = 0; y < countY; ++y) {
            for (uint32_t x = 0; x < countX; ++x) {
                const glm::vec3 position = start + glm::vec3(
                    static_cast<float>(x), static_cast<float>(y),
                    static_cast<float>(z)) * spacing;
                states[index].position = glm::vec4(position, 1.0f);
                states[index].velocity = glm::vec4(0.0f);
                instances[index].positionSize = glm::vec4(
                    LocalToWorld(position, m_containerRotation), kParticleSize);
                instances[index].color = glm::vec4(0.10f, 0.48f, 0.95f, 0.88f);
                instances[index].rotationBlend = glm::vec4(0.0f);
                ++index;
            }
        }
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        m_StateBuffers[i].Write(states.data(),
                                sizeof(HostParticleState) * states.size());
        m_RenderBuffers[i].Write(instances.data(),
                                 sizeof(HostParticleInstance) * instances.size());
    }
}

void GpuSphSimulation::DestroyPipelines() {
    if (g_Device == VK_NULL_HANDLE) return;
    if (m_DensityPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(g_Device, m_DensityPipeline, g_Allocator);
        m_DensityPipeline = VK_NULL_HANDLE;
    }
    if (m_IntegratePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(g_Device, m_IntegratePipeline, g_Allocator);
        m_IntegratePipeline = VK_NULL_HANDLE;
    }
    if (m_DensityPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(g_Device, m_DensityPipelineLayout, g_Allocator);
        m_DensityPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_IntegratePipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(g_Device, m_IntegratePipelineLayout, g_Allocator);
        m_IntegratePipelineLayout = VK_NULL_HANDLE;
    }
}

void GpuSphSimulation::DestroyDescriptorResources() {
    if (g_Device == VK_NULL_HANDLE) return;
    if (m_DescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(g_Device, m_DescriptorPool, g_Allocator);
        m_DescriptorPool = VK_NULL_HANDLE;
    }
    if (m_DensityDescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(g_Device, m_DensityDescriptorLayout, g_Allocator);
        m_DensityDescriptorLayout = VK_NULL_HANDLE;
    }
    if (m_IntegrateDescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(g_Device, m_IntegrateDescriptorLayout, g_Allocator);
        m_IntegrateDescriptorLayout = VK_NULL_HANDLE;
    }
    m_DensityDescriptorSets = {};
    m_IntegrateDescriptorSets = {};
}

void GpuSphSimulation::Cleanup() {
    if (g_Device == VK_NULL_HANDLE) return;
    bool hasBuffers = false;
    for (const VulkanBuffer& buffer : m_StateBuffers) {
        hasBuffers = hasBuffers || buffer.GetBuffer() != VK_NULL_HANDLE;
    }
    for (const VulkanBuffer& buffer : m_RenderBuffers) {
        hasBuffers = hasBuffers || buffer.GetBuffer() != VK_NULL_HANDLE;
    }
    for (const VulkanBuffer& buffer : m_DensityBuffers) {
        hasBuffers = hasBuffers || buffer.GetBuffer() != VK_NULL_HANDLE;
    }
    if (!m_initialized && m_DensityDescriptorLayout == VK_NULL_HANDLE &&
        m_IntegrateDescriptorLayout == VK_NULL_HANDLE &&
        m_DescriptorPool == VK_NULL_HANDLE && !hasBuffers) {
        return;
    }

    vkDeviceWaitIdle(g_Device);
    DestroyPipelines();
    DestroyDescriptorResources();
    for (VulkanBuffer& buffer : m_StateBuffers) buffer.Cleanup();
    for (VulkanBuffer& buffer : m_RenderBuffers) buffer.Cleanup();
    for (VulkanBuffer& buffer : m_DensityBuffers) buffer.Cleanup();
    m_initialized = false;
    m_playbackActive = false;
    m_resetRequested = true;
    m_currentStateIndex = 0;
    m_timeAccumulator = 0.0f;
}
