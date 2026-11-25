// WorldRenderer.cpp - Vulkan 体素世界渲染器
#include "World/WorldRenderer.h"
#include "EngineGlobal.h"
#include "Core/EngineConfig.h"
#include "Core/VulkanManager.h"
#include "Core/CpuStageTrace.h"
#include "Core/VulkanGpuProfiler.h"

#include <SDL3_image/SDL_image.h>
#include <SDL3/SDL.h>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <algorithm>
#include "Rendering/RenderStats.h"
#include "Core/Log.h"
#include "Core/LogStream.h"

namespace {
// 上传数据到 DEVICE_LOCAL 缓冲（staging + 一次性命令）
void CopyBuffer(VkBuffer dstBuffer, VkBuffer srcBuffer, VkDeviceSize size) {
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = g_CommandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(g_Device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkBufferCopy copyRegion = {};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(g_Queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_Queue);

    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
}
}


namespace {
constexpr int kRegionSize = 4;
constexpr size_t kMaterialCount = 3;
constexpr size_t kOpaqueMaterial = 0;
constexpr size_t kWaterMaterial = 2;
constexpr uint32_t kAtlasTilesPerAxis = 16;
constexpr uint32_t kAtlasTexelBytes = 4;
constexpr uint32_t kInstancesPerPage = 256 * 1024;
constexpr VkMemoryPropertyFlags kMappedMemory =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

using ChunkCoord = std::pair<int, int>;

int RegionCoordinate(int coordinate)
{
    return coordinate / kRegionSize - (coordinate % kRegionSize < 0 ? 1 : 0);
}

ChunkCoord RegionOf(ChunkCoord coordinate)
{
    return {RegionCoordinate(coordinate.first), RegionCoordinate(coordinate.second)};
}

size_t SlotInRegion(ChunkCoord coordinate, ChunkCoord region)
{
    return (coordinate.first - region.first * kRegionSize) * kRegionSize +
           (coordinate.second - region.second * kRegionSize);
}

enum class RegionVisibility { Outside, Partial, Inside };

RegionVisibility TestRegion(const AABB& bounds, const std::array<Plane, 6>& frustum)
{
    bool inside = true;
    for (const auto& plane : frustum) {
        const glm::vec3 positive(
            plane.normal.x >= 0 ? bounds.max.x : bounds.min.x,
            plane.normal.y >= 0 ? bounds.max.y : bounds.min.y,
            plane.normal.z >= 0 ? bounds.max.z : bounds.min.z);
        if (glm::dot(plane.normal, positive) + plane.distance < 0) {
            return RegionVisibility::Outside;
        }
        const glm::vec3 negative = bounds.min + bounds.max - positive;
        inside &= glm::dot(plane.normal, negative) + plane.distance >= 0;
    }
    return inside ? RegionVisibility::Inside : RegionVisibility::Partial;
}

std::unique_ptr<VulkanBuffer> CreateMappedBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage)
{
    auto buffer = std::make_unique<VulkanBuffer>();
    if (!buffer->Create(bytes, usage, kMappedMemory)) return {};
    buffer->Map();
    if (!buffer->GetMappedPtr()) return {};
    return buffer;
}
}

struct WorldRenderer::ResidentMeshes {
    struct FreeRange {
        uint32_t first;
        uint32_t count;
    };

    struct Page {
        std::unique_ptr<VulkanBuffer> buffer;
        std::vector<FreeRange> free;
        uint32_t used = 0;
    };

    struct Range {
        size_t page = 0;
        uint32_t first = 0;
        uint32_t count = 0;
    };

    struct Chunk {
        AABB bounds;
        std::array<Range, kMaterialCount> ranges;
    };

    struct Region {
        std::array<std::optional<Chunk>, kRegionSize * kRegionSize> chunks;
        AABB bounds;

        void RefreshBounds()
        {
            bounds = {};
            for (const auto& chunk : chunks) {
                if (chunk) bounds = bounds.UnionWith(chunk->bounds);
            }
        }
    };

    struct Draw {
        Range range;
        float depth;
    };

    struct FrameStats {
        uint64_t visibleChunks = 0;
        uint64_t visibleFaces = 0;
        uint64_t indirectBytes = 0;
        uint64_t apiCalls = 0;
        uint64_t subDraws = 0;
    };

    std::vector<std::unique_ptr<Page>> pages;
    std::map<ChunkCoord, Region> regions;
    std::map<ChunkCoord, World::RasterChunkUpdate> pending;
    std::array<std::vector<Draw>, kMaterialCount> draws;
    std::vector<std::unique_ptr<VulkanBuffer>> commandBuffers;
    size_t nextCommandBuffer = 0;
    uint64_t worldId = 0;
    uint64_t revision = 0;
    uint64_t serial = UINT64_MAX;
    size_t chunkCount = 0;
    size_t uploadBytes = 0;
    uint32_t maxIndirectCount = 0;
    bool allocationFailed = false;
    std::ofstream stats;

    ResidentMeshes()
    {
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(g_PhysicalDevice, &features);
        if (features.multiDrawIndirect && features.drawIndirectFirstInstance) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(g_PhysicalDevice, &properties);
            maxIndirectCount = properties.limits.maxDrawIndirectCount;
        }
        if (const char* path = std::getenv("MIKAN_VOXEL_RASTER_STATS"); path && *path) {
            stats.open(path, std::ios::app | std::ios::ate);
            if (stats.tellp() == 0) {
                stats << "frame,view_slot,probe_face,resident_chunks,visible_chunks,visible_faces,"
                         "geometry_upload_bytes,indirect_bytes,allocated_instance_bytes,api_draw_calls,sub_draws\n";
            }
        }
    }

    void ClearChunks()
    {
        pages.clear();
        regions.clear();
        pending.clear();
        chunkCount = 0;
        revision = 0;
        allocationFailed = false;
    }

    std::optional<Range> Allocate(uint32_t count)
    {
        size_t index = 0;
        for (; index < pages.size(); ++index) {
            if (!pages[index]) continue;
            Page& page = *pages[index];
            auto gap = std::find_if(page.free.begin(), page.free.end(), [count](const FreeRange& range) {
                return range.count >= count;
            });
            if (gap == page.free.end()) continue;
            Range result{index, gap->first, count};
            gap->first += count;
            gap->count -= count;
            if (gap->count == 0) page.free.erase(gap);
            page.used += count;
            return result;
        }

        auto page = std::make_unique<Page>();
        const uint32_t capacity = std::max(kInstancesPerPage, count);
        page->buffer = CreateMappedBuffer(
            VkDeviceSize(capacity) * sizeof(::Chunk::FaceInstance), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (!page->buffer) return std::nullopt;

        page->used = count;
        if (capacity > count) page->free.push_back({count, capacity - count});

        const auto vacant = std::find_if(pages.begin(), pages.end(), [](const auto& entry) { return !entry; });
        index = static_cast<size_t>(vacant - pages.begin());
        if (vacant == pages.end()) pages.push_back(std::move(page));
        else *vacant = std::move(page);
        return Range{index, 0, count};
    }

    void Release(Range range)
    {
        if (!range.count) return;
        Page& page = *pages[range.page];
        page.used -= range.count;
        auto next = std::lower_bound(page.free.begin(), page.free.end(), range.first,
            [](const FreeRange& gap, uint32_t first) { return gap.first < first; });

        if (next != page.free.begin() && (next - 1)->first + (next - 1)->count == range.first) {
            --next;
            next->count += range.count;
        } else {
            next = page.free.insert(next, {range.first, range.count});
        }
        if (next + 1 != page.free.end() && next->first + next->count == (next + 1)->first) {
            next->count += (next + 1)->count;
            page.free.erase(next + 1);
        }
    }

    void Remove(ChunkCoord coordinate)
    {
        const ChunkCoord key = RegionOf(coordinate);
        const auto group = regions.find(key);
        if (group == regions.end()) return;
        auto& chunk = group->second.chunks[SlotInRegion(coordinate, key)];
        if (!chunk) return;
        for (const Range range : chunk->ranges) Release(range);
        chunk.reset();
        --chunkCount;
        group->second.RefreshBounds();
        if (!group->second.bounds.IsValid()) regions.erase(group);
    }

    bool Upload(const World::RasterChunkUpdate& update)
    {
        const ChunkCoord key = RegionOf(update.coordinate);
        const size_t slot = SlotInRegion(update.coordinate, key);
        const auto group = regions.find(key);
        std::array<Range, kMaterialCount> previous{};
        if (group != regions.end() && group->second.chunks[slot]) {
            previous = group->second.chunks[slot]->ranges;
        }

        std::array<Range, kMaterialCount> ranges{};
        std::array<bool, kMaterialCount> reuse{};
        for (size_t material = 0; material < kMaterialCount; ++material) {
            const auto count = static_cast<uint32_t>(update.faces[material].size());
            if (!count) continue;
            if (count <= previous[material].count) {
                reuse[material] = true;
                ranges[material] = previous[material];
                ranges[material].count = count;
            } else if (auto range = Allocate(count)) {
                ranges[material] = *range;
            } else {
                for (size_t allocated = 0; allocated < material; ++allocated) {
                    if (!reuse[allocated]) Release(ranges[allocated]);
                }
                return false;
            }
        }

        for (size_t material = 0; material < kMaterialCount; ++material) {
            const Range range = ranges[material];
            if (range.count) {
                Page& page = *pages[range.page];
                auto* data = static_cast<std::byte*>(page.buffer->GetMappedPtr());
                const size_t bytes = range.count * sizeof(::Chunk::FaceInstance);
                std::memcpy(data + range.first * sizeof(::Chunk::FaceInstance),
                            update.faces[material].data(), bytes);
                uploadBytes += bytes;
            }
            const Range old = previous[material];
            if (reuse[material]) Release({old.page, old.first + range.count, old.count - range.count});
            else Release(old);
        }

        auto& region = regions[key];
        if (!region.chunks[slot]) ++chunkCount;
        region.chunks[slot] = Chunk{update.bounds, ranges};
        region.RefreshBounds();
        return true;
    }

    void BeginFrame(const World& world, uint64_t frame)
    {
        if (frame == serial) return;
        Core::CpuStageTrace timing("raster.voxel_sync");
        // FrameRender waits for the previous offscreen submission before any view
        // is recorded. No earlier view in this frame has bound these ranges yet.
        serial = frame;
        nextCommandBuffer = 0;
        uploadBytes = 0;
        if (world.GetId() != worldId) {
            ClearChunks();
            worldId = world.GetId();
        }
        auto updates = world.CaptureRasterMeshUpdates(revision);
        if (!updates && pending.empty()) return;
        if (updates) {
            if (updates->full) ClearChunks();
            revision = updates->revision;
            for (auto& update : updates->chunks) {
                const auto coordinate = update.coordinate;
                const bool hasFaces = std::any_of(update.faces.begin(), update.faces.end(), [](const auto& faces) {
                    return !faces.empty();
                });
                if (update.revision == 0 || !hasFaces) {
                    Remove(coordinate);
                    pending.erase(coordinate);
                } else {
                    pending.insert_or_assign(coordinate, std::move(update));
                }
            }
        }
        for (auto it = pending.begin(); it != pending.end();) {
            if (Upload(it->second)) it = pending.erase(it);
            else ++it;
        }
        if (!pending.empty() && !allocationFailed) {
            LOGW("[WorldRenderer] Instance page allocation failed; pending chunks will retry");
        }
        allocationFailed = !pending.empty();

        // Retain one empty standard page to handle fluctuating chunk residency.
        bool keptEmptyPage = false;
        for (auto& page : pages) {
            if (!page || page->used) continue;
            if (!keptEmptyPage && page->buffer->GetSize() ==
                                      VkDeviceSize(kInstancesPerPage) * sizeof(::Chunk::FaceInstance)) {
                keptEmptyPage = true;
            } else {
                page.reset();
            }
        }
        while (!pages.empty() && !pages.back()) pages.pop_back();
    }

    void Collect(const glm::mat4& view, const std::array<Plane, 6>& frustum, FrameStats& frameStats)
    {
        Core::CpuStageTrace timing("raster.voxel_cull");
        for (auto& list : draws) list.clear();

        for (const auto& [coordinate, region] : regions) {
            const RegionVisibility visibility = TestRegion(region.bounds, frustum);
            if (visibility == RegionVisibility::Outside) continue;
            for (const auto& chunk : region.chunks) {
                if (!chunk) continue;
                if (visibility == RegionVisibility::Partial && !chunk->bounds.IsInsideFrustum(frustum)) continue;

                ++frameStats.visibleChunks;
                for (size_t material = 0; material < kMaterialCount; ++material) {
                    const Range range = chunk->ranges[material];
                    if (!range.count) continue;
                    const float depth = material == kWaterMaterial
                        ? -(view * glm::vec4(chunk->bounds.GetCenter(), 1.0f)).z : 0.0f;
                    draws[material].push_back({range, depth});
                    frameStats.visibleFaces += range.count;
                }
            }
        }

        for (size_t material = 0; material < kWaterMaterial; ++material) {
            auto& list = draws[material];
            std::sort(list.begin(), list.end(), [](const Draw& a, const Draw& b) {
                return a.range.page < b.range.page;
            });
        }
        // Keep transparent chunks in back-to-front order across buffer pages.
        auto& water = draws[kWaterMaterial];
        std::stable_sort(water.begin(), water.end(), [](const Draw& a, const Draw& b) {
            return a.depth > b.depth;
        });
    }

    VkBuffer WriteIndirectCommands(FrameStats& frameStats)
    {
        if (!maxIndirectCount) return VK_NULL_HANDLE;
        size_t count = 0;
        for (const auto& list : draws) count += list.size();
        if (!count) return VK_NULL_HANDLE;

        // Each view recorded in this command buffer keeps its own arguments.
        if (nextCommandBuffer == commandBuffers.size()) commandBuffers.emplace_back();
        auto& buffer = commandBuffers[nextCommandBuffer++];
        const VkDeviceSize bytes = count * sizeof(VkDrawIndirectCommand);
        if (!buffer || buffer->GetSize() < bytes) {
            const VkDeviceSize previous = buffer ? buffer->GetSize() : 0;
            const auto capacity = std::max({VkDeviceSize(4096), previous * 2, bytes});
            auto replacement = CreateMappedBuffer(capacity, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
            if (!replacement) return VK_NULL_HANDLE;
            buffer = std::move(replacement);
        }

        auto* command = static_cast<VkDrawIndirectCommand*>(buffer->GetMappedPtr());
        for (const auto& list : draws) {
            for (const auto& draw : list) *command++ = {4, draw.range.count, 0, draw.range.first};
        }
        frameStats.indirectBytes = bytes;
        return buffer->GetBuffer();
    }

    void WriteStats(int viewSlot, int probeFace, const FrameStats& frameStats)
    {
        if (!stats.is_open()) return;
        VkDeviceSize allocatedBytes = 0;
        for (const auto& page : pages) {
            if (page) allocatedBytes += page->buffer->GetSize();
        }
        const size_t frameUploadBytes = std::exchange(uploadBytes, 0);
        stats << serial << ',' << viewSlot << ',' << probeFace << ',' << chunkCount << ','
              << frameStats.visibleChunks << ',' << frameStats.visibleFaces << ',' << frameUploadBytes << ','
              << frameStats.indirectBytes << ',' << allocatedBytes << ','
              << frameStats.apiCalls << ',' << frameStats.subDraws << '\n';
    }
};

WorldRenderer::WorldRenderer() = default;
WorldRenderer::~WorldRenderer() { Cleanup(); }

void WorldRenderer::Init(VkRenderPass renderPass)
{
    if (m_Initialized) return;
    if (!CreateQuadBuffer()) {
        LOGSTREAM(Error) << "[WorldRenderer] FAILED: CreateQuadBuffer" << std::endl;
        LOGE("[WorldRenderer] Failed to create quad buffer");
        return;
    }
    if (!LoadAtlasTexture()) {
        LOGSTREAM(Error) << "[WorldRenderer] FAILED: LoadAtlasTexture" << std::endl;
        LOGE("[WorldRenderer] Failed to load atlas texture");
        Cleanup();
        return;
    }
    LOGSTREAM(Info) << "[WorldRenderer] Atlas loaded (" << m_AtlasWidth << "x" << m_AtlasHeight << ")" << std::endl;
    if (!CreateDescriptorSet() || !CreatePipeline(renderPass)) {
        Cleanup();
        return;
    }
    m_ResidentMeshes = std::make_unique<ResidentMeshes>();
    m_Initialized = true;
    LOGI("[WorldRenderer] Initialized");
}

void WorldRenderer::Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj)
{
    // IRenderer 接口入口：从 view/proj 提取视锥，传入渲染相机位置
    if (!m_World || !m_Initialized) return;
    std::array<Plane, 6> frustum = AABBUtils::ExtractFrustumPlanes(proj * view);
    glm::vec3 cameraPos = glm::vec3(-view[3][0], -view[3][1], -view[3][2]);
    RenderWorld(commandBuffer, 0, 0, view, proj, cameraPos, frustum, 0, 0);
}

void WorldRenderer::RenderWorld(VkCommandBuffer commandBuffer, int width, int height,
                                const glm::mat4& view, const glm::mat4& proj,
                                const glm::vec3&, const std::array<Plane, 6>& frustumPlanes,
                                int viewSlot, int probeFace)
{
    if (!m_World || !m_Initialized || commandBuffer == VK_NULL_HANDLE) return;

    auto& resident = *m_ResidentMeshes;
    resident.BeginFrame(*m_World, GetCurrentFrameSerial());
    ResidentMeshes::FrameStats frameStats;
    resident.Collect(view, frustumPlanes, frameStats);
    if (!frameStats.visibleFaces) {
        resident.WriteStats(viewSlot, probeFace, frameStats);
        return;
    }

    Core::CpuStageTrace timing("raster.voxel_record");
    const VkBuffer indirect = resident.WriteIndirectCommands(frameStats);

    if (width > 0 && height > 0) {
        VkViewport viewport{};
        viewport.width = static_cast<float>(width);
        viewport.height = static_cast<float>(height);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        const VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}};
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    }

    const WorldPushConstants pushConstants{proj * view};
    const std::array pipelines{&m_Pipeline, &m_AlphaPipeline, &m_TransparentPipeline};

    {
        Core::VulkanGpuScope gpuTiming(commandBuffer, "raster.voxel");
        VkDeviceSize commandOffset = 0;
        for (size_t material = 0; material < pipelines.size(); ++material) {
            const auto& list = resident.draws[material];
            if (list.empty()) continue;

            const auto& pipeline = *pipelines[material];
            const VkDescriptorSet atlas = material == kOpaqueMaterial ? m_OpaqueDescriptorSet : m_DescriptorSet;
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.GetPipeline());
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.GetLayout(),
                                    0, 1, &atlas, 0, nullptr);
            vkCmdPushConstants(commandBuffer, pipeline.GetLayout(),
                               VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(pushConstants), &pushConstants);

            size_t first = 0;
            while (first < list.size()) {
                const size_t page = list[first].range.page;
                size_t end = first + 1;
                while (end < list.size() && list[end].range.page == page) ++end;

                const VkBuffer buffers[]{m_QuadVertexBuffer, resident.pages[page]->buffer->GetBuffer()};
                constexpr VkDeviceSize offsets[]{0, 0};
                vkCmdBindVertexBuffers(commandBuffer, 0, 2, buffers, offsets);

                if (indirect) {
                    for (size_t cursor = first; cursor < end;) {
                        const auto count = static_cast<uint32_t>(std::min<size_t>(end - cursor, resident.maxIndirectCount));
                        vkCmdDrawIndirect(commandBuffer, indirect, commandOffset, count, sizeof(VkDrawIndirectCommand));
                        commandOffset += VkDeviceSize(count) * sizeof(VkDrawIndirectCommand);
                        cursor += count;
                        ++frameStats.apiCalls;
                    }
                } else {
                    for (size_t i = first; i < end; ++i) {
                        const auto range = list[i].range;
                        vkCmdDraw(commandBuffer, 4, range.count, 0, range.first);
                        ++frameStats.apiCalls;
                    }
                }

                frameStats.subDraws += end - first;
                first = end;
            }
        }
    }

    if (indirect) {
        Rendering::RenderStats::Get().OnDrawIndirect(frameStats.subDraws, frameStats.visibleFaces * 2,
                                                     frameStats.visibleFaces);
    }
    resident.WriteStats(viewSlot, probeFace, frameStats);
}

void WorldRenderer::ReleaseDetachedMeshes()
{
    if (m_ResidentMeshes && m_ResidentMeshes->worldId &&
        (!m_World || m_World->GetId() != m_ResidentMeshes->worldId)) {
        m_ResidentMeshes->ClearChunks();
        m_ResidentMeshes->worldId = 0;
        m_ResidentMeshes->commandBuffers.clear();
    }
}

void WorldRenderer::Cleanup()
{
    if (m_QuadVertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, m_QuadVertexBuffer, g_Allocator);
        m_QuadVertexBuffer = VK_NULL_HANDLE;
    }
    if (m_QuadVertexBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_QuadVertexBufferMemory, g_Allocator);
        m_QuadVertexBufferMemory = VK_NULL_HANDLE;
    }

    m_ResidentMeshes.reset();

    m_Pipeline.Cleanup();
    m_AlphaPipeline.Cleanup();
    m_TransparentPipeline.Cleanup();
    m_Descriptor.Cleanup();
    m_DescriptorSet = VK_NULL_HANDLE;
    m_OpaqueDescriptorSet = VK_NULL_HANDLE;

    if (m_AtlasSampler != VK_NULL_HANDLE) { vkDestroySampler(g_Device, m_AtlasSampler, g_Allocator); m_AtlasSampler = VK_NULL_HANDLE; }
    if (m_OpaqueAtlasSampler != VK_NULL_HANDLE) { vkDestroySampler(g_Device, m_OpaqueAtlasSampler, g_Allocator); m_OpaqueAtlasSampler = VK_NULL_HANDLE; }
    if (m_AtlasImageView != VK_NULL_HANDLE) { vkDestroyImageView(g_Device, m_AtlasImageView, g_Allocator); m_AtlasImageView = VK_NULL_HANDLE; }
    if (m_AtlasImage != VK_NULL_HANDLE) { vkDestroyImage(g_Device, m_AtlasImage, g_Allocator); m_AtlasImage = VK_NULL_HANDLE; }
    if (m_AtlasImageMemory != VK_NULL_HANDLE) { vkFreeMemory(g_Device, m_AtlasImageMemory, g_Allocator); m_AtlasImageMemory = VK_NULL_HANDLE; }

    m_Initialized = false;
}

// ---------------- 内部实现 ----------------

bool WorldRenderer::CreateQuadBuffer()
{
    // 单元 quad：TRIANGLE_STRIP 4 顶点（与旧版一致）
    const float quadVertices[] = {
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
        -0.5f,  0.5f, 0.0f,
         0.5f,  0.5f, 0.0f,
    };
    VkDeviceSize bufferSize = sizeof(quadVertices);

    // staging
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &stagingBuffer) != VK_SUCCESS) return false;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(g_Device, stagingBuffer, &memReq);
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &stagingMemory) != VK_SUCCESS) {
        vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
        return false;
    }
    vkBindBufferMemory(g_Device, stagingBuffer, stagingMemory, 0);
    void* data = nullptr;
    vkMapMemory(g_Device, stagingMemory, 0, bufferSize, 0, &data);
    memcpy(data, quadVertices, bufferSize);
    vkUnmapMemory(g_Device, stagingMemory);

    // DEVICE_LOCAL 目标
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &m_QuadVertexBuffer) != VK_SUCCESS) {
        vkFreeMemory(g_Device, stagingMemory, g_Allocator);
        vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
        return false;
    }
    vkGetBufferMemoryRequirements(g_Device, m_QuadVertexBuffer, &memReq);
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_QuadVertexBufferMemory) != VK_SUCCESS) {
        vkDestroyBuffer(g_Device, m_QuadVertexBuffer, g_Allocator);
        vkFreeMemory(g_Device, stagingMemory, g_Allocator);
        vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
        return false;
    }
    vkBindBufferMemory(g_Device, m_QuadVertexBuffer, m_QuadVertexBufferMemory, 0);

    CopyBuffer(m_QuadVertexBuffer, stagingBuffer, bufferSize);

    vkFreeMemory(g_Device, stagingMemory, g_Allocator);
    vkDestroyBuffer(g_Device, stagingBuffer, g_Allocator);
    return true;
}

bool WorldRenderer::LoadAtlasTexture()
{
    std::string texPath = EngineConfig::GetEngineTexturePath("Blocks.png");
    SDL_Surface* surface = IMG_Load(texPath.c_str());
    if (surface == nullptr) {
        LOGE("[WorldRenderer] Failed to load atlas: %s (SDL: %s)",
            texPath.c_str(), SDL_GetError());
        return false;
    }

    SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA8888);
    SDL_DestroySurface(surface);
    if (converted == nullptr) {
        LOGE("[WorldRenderer] Failed to convert atlas surface");
        return false;
    }

    const int width = converted->w;
    const int height = converted->h;
    if (width <= 0 || height <= 0) {
        SDL_DestroySurface(converted);
        return false;
    }
    const int pitch = converted->pitch;
    const auto* pixels = static_cast<const unsigned char*>(converted->pixels);
    m_AtlasWidth = (uint32_t)width;
    m_AtlasHeight = (uint32_t)height;

    // 通道重排 (SDL RGBA8888 小端内存为 ABGR)
    // 注意：不翻转 Y —— 保持 SDL 行序（图像顶部在内存顶部），
    // 配合 world.vert 中 baseUV.y 翻转，与旧版 OpenGL 语义一致（t=0=图像顶部）
    std::vector<unsigned char> imageData(size_t(width) * height * kAtlasTexelBytes);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            unsigned char a = pixels[y * pitch + x * 4 + 0];
            unsigned char b = pixels[y * pitch + x * 4 + 1];
            unsigned char g = pixels[y * pitch + x * 4 + 2];
            unsigned char r = pixels[y * pitch + x * 4 + 3];
            imageData[(y * width + x) * 4 + 0] = r;
            imageData[(y * width + x) * 4 + 1] = g;
            imageData[(y * width + x) * 4 + 2] = b;
            imageData[(y * width + x) * 4 + 3] = a;
        }
    }
    SDL_DestroySurface(converted);

    std::vector<VkBufferImageCopy> mipRegions;
    VkExtent3D mipExtent{m_AtlasWidth, m_AtlasHeight, 1};
    size_t mipOffset = 0;
    for (;;) {
        VkBufferImageCopy& region = mipRegions.emplace_back();
        region.bufferOffset = mipOffset;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = static_cast<uint32_t>(mipRegions.size() - 1);
        region.imageSubresource.layerCount = 1;
        region.imageExtent = mipExtent;

        // Stop when a tile can no longer be halved. Each 2x2 footprint stays within its tile.
        if (mipExtent.width % (2 * kAtlasTilesPerAxis) || mipExtent.height % (2 * kAtlasTilesPerAxis)) break;
        const uint32_t nextWidth = mipExtent.width / 2;
        const uint32_t nextHeight = mipExtent.height / 2;
        const size_t nextOffset = imageData.size();
        const size_t sourceStride = size_t(mipExtent.width) * kAtlasTexelBytes;
        imageData.resize(nextOffset + size_t(nextWidth) * nextHeight * kAtlasTexelBytes);
        for (uint32_t y = 0; y < nextHeight; ++y) {
            for (uint32_t x = 0; x < nextWidth; ++x) {
                const size_t source = mipOffset + 2 * y * sourceStride + 2 * x * kAtlasTexelBytes;
                const size_t destination = nextOffset + (size_t(y) * nextWidth + x) * kAtlasTexelBytes;
                for (uint32_t channel = 0; channel < kAtlasTexelBytes; ++channel) {
                    const size_t a = source + channel;
                    const size_t b = a + sourceStride;
                    imageData[destination + channel] = static_cast<unsigned char>(
                        (imageData[a] + imageData[a + kAtlasTexelBytes] +
                         imageData[b] + imageData[b + kAtlasTexelBytes] + 2) / 4);
                }
            }
        }
        mipOffset = nextOffset;
        mipExtent = {nextWidth, nextHeight, 1};
    }
    const uint32_t mipLevels = static_cast<uint32_t>(mipRegions.size());

    // 创建 Vulkan 图像
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = static_cast<uint32_t>(width);
    imageInfo.extent.height = static_cast<uint32_t>(height);
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(g_Device, &imageInfo, g_Allocator, &m_AtlasImage) != VK_SUCCESS) return false;

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(g_Device, m_AtlasImage, &memReq);
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_AtlasImageMemory) != VK_SUCCESS) {
        vkDestroyImage(g_Device, m_AtlasImage, g_Allocator);
        m_AtlasImage = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindImageMemory(g_Device, m_AtlasImage, m_AtlasImageMemory, 0) != VK_SUCCESS) return false;

    // image view
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_AtlasImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(g_Device, &viewInfo, g_Allocator, &m_AtlasImageView) != VK_SUCCESS) return false;

    // sampler（像素风：NEAREST + CLAMP）
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxLod = 0.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    if (vkCreateSampler(g_Device, &samplerInfo, g_Allocator, &m_AtlasSampler) != VK_SUCCESS) return false;
    // Nearest texels preserve block pixels and prevent filtering across tile edges.
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.maxLod = static_cast<float>(mipLevels - 1);
    if (vkCreateSampler(g_Device, &samplerInfo, g_Allocator, &m_OpaqueAtlasSampler) != VK_SUCCESS) return false;

    auto staging = CreateMappedBuffer(imageData.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!staging) return false;
    std::memcpy(staging->GetMappedPtr(), imageData.data(), imageData.size());

    // 上传：UNDEFINED -> TRANSFER_DST -> 拷贝 -> SHADER_READ_ONLY
    VkCommandBufferAllocateInfo cmdAlloc = {};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandPool = g_CommandPool;
    cmdAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(g_Device, &cmdAlloc, &cmd) != VK_SUCCESS) return false;

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &cmd);
        return false;
    }

    VkImageMemoryBarrier toDst = {};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = m_AtlasImage;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toDst.subresourceRange.baseMipLevel = 0;
    toDst.subresourceRange.levelCount = mipLevels;
    toDst.subresourceRange.baseArrayLayer = 0;
    toDst.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toDst);

    vkCmdCopyBufferToImage(cmd, staging->GetBuffer(), m_AtlasImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        mipLevels, mipRegions.data());

    VkImageMemoryBarrier toShader = toDst;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toShader);

    VkResult result = vkEndCommandBuffer(cmd);
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (result == VK_SUCCESS) result = vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE);
    if (result == VK_SUCCESS) result = vkQueueWaitIdle(g_Queue);

    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &cmd);
    if (result == VK_SUCCESS) LOGI("[WorldRenderer] Opaque atlas mip levels: %u", mipLevels);
    return result == VK_SUCCESS;
}

bool WorldRenderer::CreateDescriptorSet()
{
    m_Descriptor.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
    if (!m_Descriptor.CreateLayout()) return false;
    if (!m_Descriptor.CreatePool(2)) return false;
    if (!m_Descriptor.AllocateSet(m_DescriptorSet)) return false;
    if (!m_Descriptor.AllocateSet(m_OpaqueDescriptorSet)) return false;

    for (const auto& [descriptor, sampler] : {std::pair{m_DescriptorSet, m_AtlasSampler},
                                             std::pair{m_OpaqueDescriptorSet, m_OpaqueAtlasSampler}}) {
        VkDescriptorImageInfo imageInfo = {};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = m_AtlasImageView;
        imageInfo.sampler = sampler;

        VkWriteDescriptorSet write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptor;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(g_Device, 1, &write, 0, nullptr);
    }
    return true;
}

bool WorldRenderer::CreatePipeline(VkRenderPass renderPass)
{
    PipelineConfig config;
    config.vertShader = "world.vert.spv";
    config.fragShader = "world.frag.spv";
    config.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    // 与旧版一致：opaque 单面剔除（OpenGL CULL_FACE 开启）
    config.cullMode = VK_CULL_MODE_BACK_BIT;
    config.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    config.depthTest = true;
    config.depthWrite = true;
    config.colorAttachmentCount = kMainMrtGeometryColorAttachmentCount;
    config.colorWriteMasks = {
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        0,
        0
    };
    config.subpass = 1;              // MRT 几何 subpass（0=z-prepass）
    config.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;   // z-prepass 后必须 <=

    VkVertexInputBindingDescription bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].stride = 3 * sizeof(float);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(Chunk::FaceInstance); // 16 字节: pos(12) + packedData(4)
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attributes[3] = {};
    attributes[0].binding = 0;
    attributes[0].location = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = 0;
    attributes[1].binding = 1;
    attributes[1].location = 1;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(Chunk::FaceInstance, posX);
    attributes[2].binding = 1;
    attributes[2].location = 2;
    attributes[2].format = VK_FORMAT_R32_UINT;
    attributes[2].offset = offsetof(Chunk::FaceInstance, packedData);

    config.vertexBindings.assign(bindings, bindings + 2);
    config.vertexAttributes.assign(attributes, attributes + 3);

    config.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    config.pushConstantRange.offset = 0;
    config.pushConstantRange.size = sizeof(WorldPushConstants);
    config.usePushConstants = true;

    PipelineConfig opaqueConfig = config;
    opaqueConfig.fragmentSpecialization = FragmentSpecializationConstant{0, VK_FALSE};
    if (!m_Pipeline.Create(renderPass, m_Descriptor.GetLayout(), opaqueConfig)) {
        LOGE("[WorldRenderer] Failed to create opaque pipeline");
        return false;
    }

    // 植物/树叶管线（alpha 剔除，双面：与旧版 alphatest pass 关 cull 一致）
    PipelineConfig alphaConfig = config;
    alphaConfig.cullMode = VK_CULL_MODE_NONE;
    if (!m_AlphaPipeline.Create(renderPass, m_Descriptor.GetLayout(), alphaConfig)) {
        LOGE("[WorldRenderer] Failed to create alpha pipeline");
        return false;
    }

    // 透明管线（水）：alpha 混合，不写深度（旧版 transparent pass 开 cull + blend）
    PipelineConfig transparentConfig = config;
    transparentConfig.blending = true;
    transparentConfig.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    transparentConfig.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    transparentConfig.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    transparentConfig.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    transparentConfig.depthWrite = false;
    if (!m_TransparentPipeline.Create(renderPass, m_Descriptor.GetLayout(), transparentConfig)) {
        LOGE("[WorldRenderer] Failed to create transparent pipeline");
        return false;
    }
    return true;
}
