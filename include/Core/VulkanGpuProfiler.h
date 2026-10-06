#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Core {

// Optional GPU timestamp profiler for diagnosing frame cost on real hardware.
// Enable it with MIKAN_GPU_PROFILE=1. Query results are read only after the
// corresponding frame fence has completed, so this never stalls the render
// queue just to collect timing data.
class VulkanGpuProfiler {
public:
    using ScopeId = uint32_t;
    static constexpr ScopeId InvalidScope = UINT32_MAX;

    static bool IsRequested();

    // The caller must ensure that the device is idle when changing frameCount
    // or replacing the device (swapchain recreation satisfies that contract).
    bool EnsureInitialized(VkDevice device,
                           VkPhysicalDevice physicalDevice,
                           VkAllocationCallbacks* allocator,
                           uint32_t frameCount);

    // Must be called before the device is destroyed. It also flushes query
    // results that belong to frames which have not yet been recycled.
    void Shutdown();

    void BeginFrame(VkCommandBuffer commandBuffer,
                    uint32_t frameIndex,
                    uint64_t frameSerial);
    ScopeId BeginScope(VkCommandBuffer commandBuffer, const char* label,
                       VkPipelineStageFlagBits beginStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    void EndScope(VkCommandBuffer commandBuffer, ScopeId scope);
    void EndFrame(VkCommandBuffer commandBuffer);

    bool IsInitialized() const { return m_Initialized; }

private:
    static constexpr uint32_t kMaxQueriesPerFrame = 128;

    struct ScopeRecord {
        std::string label;
        uint32_t beginQuery = InvalidScope;
        uint32_t endQuery = InvalidScope;
    };

    struct FrameData {
        VkQueryPool queryPool = VK_NULL_HANDLE;
        bool hasSubmission = false;
        uint32_t queryCount = 0;
        uint64_t serial = 0;
        std::vector<ScopeRecord> scopes;
    };

    struct Aggregate {
        double totalMs = 0.0;
        double maxMs = 0.0;
        uint64_t samples = 0;
    };

    bool ReadFrame(FrameData& frame);
    void Report();
    void DestroyQueryPools();

    VkDevice m_Device = VK_NULL_HANDLE;
    VkAllocationCallbacks* m_Allocator = nullptr;
    PFN_vkResetQueryPool m_HostResetQueryPool = nullptr;
    PFN_vkCmdResetQueryPool m_CommandResetQueryPool = nullptr;
    double m_TimestampPeriodNs = 0.0;
    uint64_t m_TimestampMask = UINT64_MAX;
    uint64_t m_WarmupFrames = 0;
    std::vector<FrameData> m_Frames;

    FrameData* m_ActiveFrame = nullptr;
    std::vector<ScopeId> m_ActiveScopes;
    ScopeId m_FrameScope = InvalidScope;
    uint32_t m_NextQuery = 0;

    std::map<std::string, Aggregate> m_Aggregates;
    uint64_t m_CompletedFrames = 0;
    uint64_t m_LastReportedFrames = 0;
    bool m_Initialized = false;
    bool m_Disabled = false;
};

extern VulkanGpuProfiler g_VulkanGpuProfiler;

// Bottom-to-bottom intervals exclude earlier work still retiring on this queue.
// They include dependencies/barriers inside the interval, rather than CPU time.
class VulkanGpuScope {
public:
    VulkanGpuScope(VkCommandBuffer cmd, const std::string& label)
        : m_CommandBuffer(cmd), m_Scope(g_VulkanGpuProfiler.BeginScope(
              cmd, label.c_str(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT)) {}
    ~VulkanGpuScope() { g_VulkanGpuProfiler.EndScope(m_CommandBuffer, m_Scope); }
    VulkanGpuScope(const VulkanGpuScope&) = delete;
    VulkanGpuScope& operator=(const VulkanGpuScope&) = delete;
private:
    VkCommandBuffer m_CommandBuffer;
    VulkanGpuProfiler::ScopeId m_Scope;
};

} // namespace Core
