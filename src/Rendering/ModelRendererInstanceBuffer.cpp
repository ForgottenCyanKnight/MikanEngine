#include "ModelRenderer.h"
#include "ModelRendererInternals.h"
#include "VulkanManager.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace {

struct InstanceUploadSlot {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    size_t capacity = 0;
};

struct InstanceUploadState {
    std::vector<InstanceUploadSlot> slots[ModelRenderData::MAX_FRAMES_IN_FLIGHT];
    uint32_t cursor[ModelRenderData::MAX_FRAMES_IN_FLIGHT] = {};
    uint64_t frameSerial = UINT64_MAX;
};

std::unordered_map<const ModelRenderer*, InstanceUploadState> g_InstanceUploadStates;

void DestroyInstanceUploadSlot(InstanceUploadSlot& slot)
{
    if (slot.mapped != nullptr && slot.memory != VK_NULL_HANDLE) {
        vkUnmapMemory(g_Device, slot.memory);
    }
    if (slot.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, slot.buffer, g_Allocator);
    }
    if (slot.memory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, slot.memory, g_Allocator);
    }
    slot = {};
}

void DestroyInstanceUploadState(InstanceUploadState& state)
{
    for (auto& bucket : state.slots) {
        for (auto& slot : bucket) {
            DestroyInstanceUploadSlot(slot);
        }
        bucket.clear();
    }
    for (uint32_t& cursor : state.cursor) {
        cursor = 0;
    }
    state.frameSerial = UINT64_MAX;
}

bool CreateInstanceUploadSlot(size_t capacity, InstanceUploadSlot& slot)
{
    const VkDeviceSize bufferSize = sizeof(ModelInstanceData) * capacity;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &slot.buffer) != VK_SUCCESS) {
        slot = {};
        return false;
    }

    VkMemoryRequirements memRequirements{};
    vkGetBufferMemoryRequirements(g_Device, slot.buffer, &memRequirements);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &slot.memory) != VK_SUCCESS) {
        DestroyInstanceUploadSlot(slot);
        return false;
    }
    if (vkBindBufferMemory(g_Device, slot.buffer, slot.memory, 0) != VK_SUCCESS) {
        DestroyInstanceUploadSlot(slot);
        return false;
    }
    if (vkMapMemory(g_Device, slot.memory, 0, bufferSize, 0, &slot.mapped) != VK_SUCCESS) {
        DestroyInstanceUploadSlot(slot);
        return false;
    }
    slot.capacity = capacity;
    return true;
}

} // namespace

namespace ModelRendererDetail {

void ReleaseInstanceUploads(const ModelRenderer* renderer)
{
    if (const auto it = g_InstanceUploadStates.find(renderer); it != g_InstanceUploadStates.end()) {
        DestroyInstanceUploadState(it->second);
        g_InstanceUploadStates.erase(it);
    }
}

} // namespace ModelRendererDetail

void ModelRenderer::CreateInstanceBuffer(size_t maxInstances)
{
    auto [stateIt, inserted] = g_InstanceUploadStates.try_emplace(this);
    (void)inserted;
    DestroyInstanceUploadState(stateIt->second);

    for (size_t frame = 0; frame < ModelRenderData::MAX_FRAMES_IN_FLIGHT; ++frame) {
        m_ModelData.instanceBuffers[frame] = VK_NULL_HANDLE;
        m_ModelData.instanceBufferMemories[frame] = VK_NULL_HANDLE;
        m_ModelData.instanceBufferMapped[frame] = nullptr;
    }
    m_ModelData.instanceBufferSize = maxInstances;
    m_ModelData.currentInstanceBufferSize = maxInstances;

    // 槽按需创建；这里预留第一个槽只为了让 Init 后的句柄行为与旧实现
    // 一致，但不把 Vulkan 资源写入 ModelRenderData 的可变容器。
    auto& firstBucket = stateIt->second.slots[0];
    firstBucket.emplace_back();
    if (!CreateInstanceUploadSlot(maxInstances, firstBucket.back())) {
        firstBucket.clear();
        return;
    }
    m_ModelData.instanceBuffer = firstBucket.back().buffer;
    m_ModelData.instanceBufferMemory = firstBucket.back().memory;
}

void ModelRenderer::UpdateInstanceBuffer(const std::vector<ModelInstanceData>& instanceData)
{
    if (instanceData.empty()) {
        return;
    }

    auto stateIt = g_InstanceUploadStates.find(this);
    if (stateIt == g_InstanceUploadStates.end()) {
        CreateInstanceBuffer(std::max<size_t>(instanceData.size() * 2, 4096));
        stateIt = g_InstanceUploadStates.find(this);
        if (stateIt == g_InstanceUploadStates.end()) return;
    }
    auto& state = stateIt->second;
    const uint32_t frameIndex = GetCurrentFrameIndex() % ModelRenderData::MAX_FRAMES_IN_FLIGHT;
    const uint64_t frameSerial = GetCurrentFrameSerial();
    if (state.frameSerial != frameSerial) {
        // FrameRender 已等待当前 swapchain image 的 fence；这个 bucket 的
        // 所有旧槽现在可复用，但本帧内 cursor 不能回退。
        state.frameSerial = frameSerial;
        state.cursor[frameIndex] = 0;
    }

    auto& uploads = state.slots[frameIndex];
    const size_t uploadIndex = state.cursor[frameIndex]++;
    if (uploads.size() <= uploadIndex) {
        uploads.resize(uploadIndex + 1);
    }
    auto& upload = uploads[uploadIndex];
    const size_t requiredCapacity = instanceData.size();

    if (upload.buffer == VK_NULL_HANDLE || upload.mapped == nullptr || upload.capacity < requiredCapacity) {
        DestroyInstanceUploadSlot(upload);
        const size_t newCapacity = std::max(requiredCapacity * 2,
            std::max<size_t>(m_ModelData.currentInstanceBufferSize, 4096));
        if (!CreateInstanceUploadSlot(newCapacity, upload)) {
            return;
        }
        m_ModelData.currentInstanceBufferSize = std::max(m_ModelData.currentInstanceBufferSize, newCapacity);
        m_ModelData.instanceBufferSize = m_ModelData.currentInstanceBufferSize;
    }

    memcpy(upload.mapped, instanceData.data(), sizeof(ModelInstanceData) * instanceData.size());
    m_ModelData.instanceBuffer = upload.buffer;
    m_ModelData.instanceBufferMemory = upload.memory;
}
