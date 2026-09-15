#include "Core/Log.h"
#include "Core/VulkanGpuProfiler.h"

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

namespace Core {

VulkanGpuProfiler g_VulkanGpuProfiler;

bool VulkanGpuProfiler::IsRequested()
{
    static const bool requested = [] {
        const char* value = std::getenv("MIKAN_GPU_PROFILE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return requested;
}

bool VulkanGpuProfiler::EnsureInitialized(VkDevice device,
                                          VkPhysicalDevice physicalDevice,
                                          VkAllocationCallbacks* allocator,
                                          uint32_t frameCount)
{
    if (!IsRequested() || m_Disabled || device == VK_NULL_HANDLE ||
        physicalDevice == VK_NULL_HANDLE || frameCount == 0) {
        return false;
    }

    if (m_Initialized && m_Device == device && m_Frames.size() == frameCount) {
        return true;
    }

    if (m_Initialized) {
        // Swapchain recreation waits for the device before changing the frame
        // resource count. Do not silently carry query pools across devices.
        Shutdown();
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    if (properties.limits.timestampComputeAndGraphics == VK_FALSE ||
        properties.limits.timestampPeriod <= 0.0f) {
        LOGW(
                     "[VulkanManager][GPU] timestamp queries unavailable; profiler disabled");
        m_Disabled = true;
        return false;
    }

    m_HostResetQueryPool = reinterpret_cast<PFN_vkResetQueryPool>(
        vkGetDeviceProcAddr(device, "vkResetQueryPool"));
    m_CommandResetQueryPool = reinterpret_cast<PFN_vkCmdResetQueryPool>(
        vkGetDeviceProcAddr(device, "vkCmdResetQueryPool"));
    if (m_HostResetQueryPool == nullptr && m_CommandResetQueryPool == nullptr) {
        LOGW(
                     "[VulkanManager][GPU] query pool reset command unavailable; profiler disabled");
        m_Disabled = true;
        return false;
    }

    m_Device = device;
    m_Allocator = allocator;
    m_TimestampPeriodNs = static_cast<double>(properties.limits.timestampPeriod);
    m_Frames.resize(frameCount);

    VkQueryPoolCreateInfo queryPoolInfo{};
    queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryPoolInfo.queryCount = kMaxQueriesPerFrame;

    for (FrameData& frame : m_Frames) {
        if (vkCreateQueryPool(m_Device, &queryPoolInfo, m_Allocator,
                              &frame.queryPool) != VK_SUCCESS) {
            LOGE(
                         "[VulkanManager][GPU] vkCreateQueryPool failed; profiler disabled");
            DestroyQueryPools();
            m_Device = VK_NULL_HANDLE;
            m_Allocator = nullptr;
            m_Disabled = true;
            return false;
        }
    }

    m_Initialized = true;
    LOGI("[VulkanManager][GPU] enabled timestamp_period_ns=%.3f frame_slots=%u",
                m_TimestampPeriodNs, frameCount);
    return true;
}

void VulkanGpuProfiler::DestroyQueryPools()
{
    if (m_Device != VK_NULL_HANDLE) {
        for (FrameData& frame : m_Frames) {
            if (frame.queryPool != VK_NULL_HANDLE) {
                vkDestroyQueryPool(m_Device, frame.queryPool, m_Allocator);
                frame.queryPool = VK_NULL_HANDLE;
            }
        }
    }
    m_Frames.clear();
}

void VulkanGpuProfiler::Shutdown()
{
    if (m_Initialized && m_Device != VK_NULL_HANDLE) {
        // Engine shutdown waits for the device before reaching this method.
        // Read every still-owned frame so short profiling runs are useful too.
        for (FrameData& frame : m_Frames) {
            ReadFrame(frame);
        }
        if (m_CompletedFrames > m_LastReportedFrames) {
            Report();
        }
    }

    DestroyQueryPools();
    m_Device = VK_NULL_HANDLE;
    m_Allocator = nullptr;
    m_HostResetQueryPool = nullptr;
    m_CommandResetQueryPool = nullptr;
    m_TimestampPeriodNs = 0.0;
    m_ActiveFrame = nullptr;
    m_ActiveScopes.clear();
    m_FrameScope = InvalidScope;
    m_NextQuery = 0;
    m_Aggregates.clear();
    m_CompletedFrames = 0;
    m_LastReportedFrames = 0;
    m_Initialized = false;
}

bool VulkanGpuProfiler::ReadFrame(FrameData& frame)
{
    if (!frame.hasSubmission || m_Device == VK_NULL_HANDLE ||
        frame.queryPool == VK_NULL_HANDLE || frame.queryCount == 0) {
        return false;
    }

    std::vector<uint64_t> timestamps(frame.queryCount, 0);
    const VkDeviceSize dataSize = sizeof(uint64_t) * timestamps.size();
    const VkResult result = vkGetQueryPoolResults(
        m_Device, frame.queryPool, 0, frame.queryCount, dataSize,
        timestamps.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result == VK_NOT_READY) {
        // This should not happen because the frame fence is waited before the
        // slot is reused, but keeping the record allows a later retry.
        return false;
    }
    if (result != VK_SUCCESS) {
        LOGE(
                     "[VulkanManager][GPU] vkGetQueryPoolResults failed: %d",
                     static_cast<int>(result));
        frame.hasSubmission = false;
        frame.scopes.clear();
        frame.queryCount = 0;
        return false;
    }

    for (const ScopeRecord& scope : frame.scopes) {
        if (scope.beginQuery >= frame.queryCount ||
            scope.endQuery >= frame.queryCount ||
            timestamps[scope.endQuery] < timestamps[scope.beginQuery]) {
            continue;
        }
        const uint64_t ticks = timestamps[scope.endQuery] - timestamps[scope.beginQuery];
        const double milliseconds =
            (static_cast<double>(ticks) * m_TimestampPeriodNs) / 1'000'000.0;
        Aggregate& aggregate = m_Aggregates[scope.label];
        aggregate.totalMs += milliseconds;
        aggregate.maxMs = std::max(aggregate.maxMs, milliseconds);
        ++aggregate.samples;
    }

    frame.hasSubmission = false;
    frame.queryCount = 0;
    frame.scopes.clear();
    ++m_CompletedFrames;
    if ((m_CompletedFrames % 60u) == 0u) {
        Report();
    }
    return true;
}

void VulkanGpuProfiler::BeginFrame(VkCommandBuffer commandBuffer,
                                   uint32_t frameIndex,
                                   uint64_t frameSerial)
{
    if (!m_Initialized || commandBuffer == VK_NULL_HANDLE ||
        frameIndex >= m_Frames.size() || m_ActiveFrame != nullptr) {
        return;
    }

    FrameData& frame = m_Frames[frameIndex];
    ReadFrame(frame);
    if (m_HostResetQueryPool != nullptr) {
        m_HostResetQueryPool(m_Device, frame.queryPool, 0, kMaxQueriesPerFrame);
    } else if (m_CommandResetQueryPool != nullptr) {
        m_CommandResetQueryPool(commandBuffer, frame.queryPool, 0,
                                kMaxQueriesPerFrame);
    }

    frame.serial = frameSerial;
    frame.queryCount = 0;
    frame.scopes.clear();
    m_ActiveFrame = &frame;
    m_ActiveScopes.clear();
    m_NextQuery = 0;
    m_FrameScope = BeginScope(commandBuffer, "frame_total");
}

VulkanGpuProfiler::ScopeId VulkanGpuProfiler::BeginScope(VkCommandBuffer commandBuffer,
                                                         const char* label)
{
    if (!m_Initialized || m_ActiveFrame == nullptr ||
        commandBuffer == VK_NULL_HANDLE || m_NextQuery + 2 > kMaxQueriesPerFrame) {
        return InvalidScope;
    }

    const ScopeId scope = static_cast<ScopeId>(m_ActiveFrame->scopes.size());
    ScopeRecord record;
    record.label = label != nullptr ? label : "unnamed";
    record.beginQuery = m_NextQuery++;
    record.endQuery = InvalidScope;
    m_ActiveFrame->scopes.push_back(std::move(record));
    vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        m_ActiveFrame->queryPool,
                        m_ActiveFrame->scopes[scope].beginQuery);
    m_ActiveScopes.push_back(scope);
    return scope;
}

void VulkanGpuProfiler::EndScope(VkCommandBuffer commandBuffer, ScopeId scope)
{
    if (!m_Initialized || m_ActiveFrame == nullptr ||
        commandBuffer == VK_NULL_HANDLE || scope == InvalidScope ||
        m_ActiveScopes.empty() || m_ActiveScopes.back() != scope) {
        return;
    }

    ScopeRecord& record = m_ActiveFrame->scopes[scope];
    record.endQuery = m_NextQuery++;
    vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        m_ActiveFrame->queryPool, record.endQuery);
    m_ActiveScopes.pop_back();
}

void VulkanGpuProfiler::EndFrame(VkCommandBuffer commandBuffer)
{
    if (!m_Initialized || m_ActiveFrame == nullptr ||
        commandBuffer == VK_NULL_HANDLE) {
        return;
    }

    // Close an accidentally unbalanced nested scope at the frame boundary so
    // a diagnostic marker can never invalidate the rest of the query pool.
    while (m_ActiveScopes.size() > 1) {
        EndScope(commandBuffer, m_ActiveScopes.back());
    }
    if (!m_ActiveScopes.empty()) {
        EndScope(commandBuffer, m_ActiveScopes.back());
    }

    m_ActiveFrame->queryCount = m_NextQuery;
    m_ActiveFrame->hasSubmission = m_NextQuery > 0;
    m_ActiveFrame = nullptr;
    m_ActiveScopes.clear();
    m_FrameScope = InvalidScope;
    m_NextQuery = 0;
}

void VulkanGpuProfiler::Report()
{
    if (m_CompletedFrames == 0) {
        return;
    }

    LOGI("[VulkanManager][GPU] frames=%llu",
                static_cast<unsigned long long>(m_CompletedFrames));
    for (const auto& [label, aggregate] : m_Aggregates) {
        if (aggregate.samples == 0) {
            continue;
        }
        LOGI("[VulkanManager][GPU] frames=%llu label=%s samples=%llu ""avg_ms=%.3f max_ms=%.3f",
                    static_cast<unsigned long long>(m_CompletedFrames),
                    label.c_str(),
                    static_cast<unsigned long long>(aggregate.samples),
                    aggregate.totalMs / static_cast<double>(aggregate.samples),
                    aggregate.maxMs);
    }
    m_LastReportedFrames = m_CompletedFrames;
}

} // namespace Core
