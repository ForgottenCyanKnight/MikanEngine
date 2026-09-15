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
    ScopeId BeginScope(VkCommandBuffer commandBuffer, const char* label);
    void EndScope(VkCommandBuffer commandBuffer, ScopeId scope);
    void EndFrame(VkCommandBuffer commandBuffer);

    bool IsInitialized() const { return m_Initialized; }

private:
    static constexpr uint32_t kMaxQueriesPerFrame = 64;

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

} // namespace Core
