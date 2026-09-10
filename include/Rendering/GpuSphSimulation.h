#pragma once

#include "Platform/Export.h"
#include "Rendering/RendererBase.h"

#include <array>
#include <cstdint>

#include <glm/gtc/quaternion.hpp>

// GPU SPH prototype used by the sphfluidgpu sample.  The simulation owns its
// ping-pong state and render buffers; ParticleRenderer only consumes the
// resulting ParticleInstance buffer as a vertex stream.
class MIKAN_API GpuSphSimulation {
public:
    static GpuSphSimulation& GetInstance();

    GpuSphSimulation(const GpuSphSimulation&) = delete;
    GpuSphSimulation& operator=(const GpuSphSimulation&) = delete;

    bool Record(VkCommandBuffer commandBuffer, float deltaSeconds);
    void Cleanup();

    void Reset();
    void TogglePaused() { m_paused = !m_paused; }
    void SetPaused(bool paused) { m_paused = paused; }
    bool IsPaused() const { return m_paused; }

    // The engine owns the play state. While inactive, Record() still creates
    // the initial particle buffer, but returns before any simulation command
    // is recorded so the first frame remains visible and frozen.
    void SetPlaybackActive(bool active) { m_playbackActive = active; }
    bool IsPlaybackActive() const { return m_playbackActive; }

    // The solver stays in container-local coordinates. The scene Transform
    // supplies the local-to-world frame; Record() transforms world gravity
    // into that frame without changing the local stepped boundary profile.
    void SetContainerRotation(const glm::quat& rotation);
    void ResetContainerRotation();
    glm::quat GetContainerRotation() const { return m_containerRotation; }
    glm::vec3 GetContainerCenter() const;
    glm::vec3 GetContainerHalfExtents() const;

    bool IsInitialized() const { return m_initialized; }
    uint32_t GetParticleCount() const { return kParticleCount; }
    VkBuffer GetRenderBuffer() const;

private:
    GpuSphSimulation() = default;
    ~GpuSphSimulation();

    static constexpr uint32_t kFramesInFlight = 3;
    static constexpr uint32_t kParticleCount = 8192;
    static constexpr uint32_t kWorkgroupSize = 64;

    struct SimParams {
        glm::vec4 simulation0{}; // dt, particleCount, smoothingRadius, restDensity
        glm::vec4 simulation1{}; // particleMass, pressureStiffness, viscosity, maxAcceleration
        glm::vec4 simulation2{}; // maxVelocity, particleSize, shortRangeRepulsion, boundaryRepulsion
        glm::vec4 gravity{};
        glm::vec4 lowerBound{}; // minX, bottomY, minZ, step1Y
        glm::vec4 upperBound{}; // maxX, topY, maxZ, step2Y
        glm::vec4 containerRotation{}; // xyzw quaternion, local -> world
        glm::vec4 containerProfile{}; // step3Y, step1Half, step2Half, outletHalf
    };
    static_assert(sizeof(SimParams) % 16 == 0,
                  "GPU SPH push constants must remain vec4 aligned");

    bool Initialize();
    bool CreateBuffers();
    bool CreateDescriptorResources();
    bool CreatePipelines();
    bool CreateShaderPipeline(const char* shaderName,
                              VkDescriptorSetLayout descriptorLayout,
                              VkPipelineLayout& pipelineLayout,
                              VkPipeline& pipeline);
    bool AllocateDescriptorSets();
    void SeedBuffers();
    void DestroyPipelines();
    void DestroyDescriptorResources();

    std::array<VulkanBuffer, kFramesInFlight> m_StateBuffers;
    std::array<VulkanBuffer, kFramesInFlight> m_RenderBuffers;
    std::array<VulkanBuffer, kFramesInFlight> m_DensityBuffers;

    VkDescriptorSetLayout m_DensityDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_IntegrateDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> m_DensityDescriptorSets{};
    std::array<VkDescriptorSet, kFramesInFlight> m_IntegrateDescriptorSets{};

    VkPipelineLayout m_DensityPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_IntegratePipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_DensityPipeline = VK_NULL_HANDLE;
    VkPipeline m_IntegratePipeline = VK_NULL_HANDLE;

    bool m_initialized = false;
    bool m_playbackActive = false;
    bool m_paused = false;
    bool m_resetRequested = true;
    bool m_containerTransformDirty = true;
    uint32_t m_currentStateIndex = 0;
    float m_timeAccumulator = 0.0f;
    glm::quat m_containerRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
};
