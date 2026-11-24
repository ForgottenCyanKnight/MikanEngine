#include "Core/Log.h"
#include "Core/VulkanGpuProfiler.h"
#include "Core/VulkanContext.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iomanip>
#include <locale>
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
    uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, queues.data());
    const uint32_t validBits = g_QueueFamily < queueCount ? queues[g_QueueFamily].timestampValidBits : 0;
    if (properties.limits.timestampComputeAndGraphics == VK_FALSE ||
        properties.limits.timestampPeriod <= 0.0f || validBits == 0) {
        LOGW(
                     "[VulkanManager][GPU] timestamp queries unavailable; profiler disabled");
        m_Disabled = true;
        return false;
    }

    m_Device = device;
    m_Allocator = allocator;
    m_TimestampPeriodNs = static_cast<double>(properties.limits.timestampPeriod);
    m_TimestampMask = validBits >= 64 ? UINT64_MAX : (uint64_t{1} << validBits) - 1;
    const char* warmup = std::getenv("MIKAN_GPU_PROFILE_WARMUP");
    m_WarmupFrames = warmup ? std::strtoull(warmup, nullptr, 10) : 0;
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

    const char* csvPath = std::getenv("MIKAN_GPU_PROFILE_CSV");
    if (!m_Csv.is_open() && csvPath != nullptr && csvPath[0] != '\0') {
        m_Csv.open(csvPath, std::ios::binary | std::ios::trunc);
        if (m_Csv.is_open()) {
            m_Csv.imbue(std::locale::classic());
            m_Csv << "frame,scope,gpu_ms\n" << std::fixed << std::setprecision(6);
            LOGI("[VulkanManager][GPU] writing per-frame timings to %s", csvPath);
        } else {
            LOGW("[VulkanManager][GPU] could not open timing CSV at %s", csvPath);
        }
    }

    m_Initialized = true;
    LOGI("[VulkanManager][GPU] enabled timestamp_period_ns=%.3f frame_slots=%u valid_bits=%u warmup_frames=%llu",
                m_TimestampPeriodNs, frameCount, validBits,
                static_cast<unsigned long long>(m_WarmupFrames));
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
        if (m_Csv.is_open()) {
            // Keep this stream across query pool recreation as frame serials continue.
            m_Csv.flush();
        }
    }

    DestroyQueryPools();
    m_Device = VK_NULL_HANDLE;
    m_Allocator = nullptr;
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

    std::array<uint64_t, kMaxQueriesPerFrame> timestamps{};
    const VkDeviceSize dataSize = sizeof(uint64_t) * frame.queryCount;
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

    if (m_CompletedFrames >= m_WarmupFrames) for (const ScopeRecord& scope : frame.scopes) {
        if (scope.beginQuery >= frame.queryCount ||
            scope.endQuery >= frame.queryCount) {
            continue;
        }
        const uint64_t ticks = (timestamps[scope.endQuery] - timestamps[scope.beginQuery]) & m_TimestampMask;
        const double milliseconds =
            (static_cast<double>(ticks) * m_TimestampPeriodNs) / 1'000'000.0;
        if (m_Csv.is_open()) {
            m_Csv << frame.serial << ',' << scope.label << ',' << milliseconds << '\n';
        }
        Aggregate& aggregate = m_Aggregates[scope.label];
        aggregate.totalMs += milliseconds;
        aggregate.maxMs = std::max(aggregate.maxMs, milliseconds);
        ++aggregate.samples;
    }

    frame.hasSubmission = false;
    frame.queryCount = 0;
    frame.scopes.clear();
    ++m_CompletedFrames;
    // CSV capture keeps periodic console and log file output off the frame path.
    if (!m_Csv.is_open() && (m_CompletedFrames % 60u) == 0u) {
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
    if (frame.hasSubmission) {
        return;
    }
    vkCmdResetQueryPool(commandBuffer, frame.queryPool, 0, kMaxQueriesPerFrame);

    frame.serial = frameSerial;
    frame.queryCount = 0;
    frame.scopes.clear();
    m_ActiveFrame = &frame;
    m_ActiveScopes.clear();
    m_NextQuery = 0;
    m_FrameScope = BeginScope(commandBuffer, "frame_total", VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

VulkanGpuProfiler::ScopeId VulkanGpuProfiler::BeginScope(VkCommandBuffer commandBuffer,
                                                         const char* label,
                                                         VkPipelineStageFlagBits beginStage)
{
    if (!m_Initialized || m_ActiveFrame == nullptr ||
        commandBuffer == VK_NULL_HANDLE ||
        m_NextQuery + m_ActiveScopes.size() + 2 > kMaxQueriesPerFrame) {
        return InvalidScope;
    }

    const ScopeId scope = static_cast<ScopeId>(m_ActiveFrame->scopes.size());
    ScopeRecord record;
    record.label = label != nullptr ? label : "unnamed";
    record.beginQuery = m_NextQuery++;
    record.endQuery = InvalidScope;
    m_ActiveFrame->scopes.push_back(std::move(record));
    vkCmdWriteTimestamp(commandBuffer, beginStage,
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
