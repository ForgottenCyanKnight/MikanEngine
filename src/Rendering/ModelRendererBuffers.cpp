#include "ModelRenderer.h"
#include "Core/VulkanContext.h"
#include "VulkanManager.h"

void ModelRenderer::CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size)
{
    if (g_CommandPool == VK_NULL_HANDLE || g_Queue == VK_NULL_HANDLE) {
        return;
    }
    
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = g_CommandPool;
    allocInfo.commandBufferCount = 1;
    
    VkCommandBuffer commandBuffer;
    if (vkAllocateCommandBuffers(g_Device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
        return;
    }
    
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        return;
    }
    
    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);
    
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        return;
    }
    
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    
    if (vkQueueSubmit(g_Queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        return;
    }
    
    if (vkQueueWaitIdle(g_Queue) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
        return;
    }
    
    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
}

void ModelRenderer::CreateUniformBuffer()
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    // 骨骼蒙皮矩阵 buffer（UBO 固定 64，vertex 动态索引；usage 含 UNIFORM_BUFFER 主路径 + TEXEL/STORAGE 兼容旧路径）
    bufferInfo.size = MAX_BONES * sizeof(glm::mat4) * ModelRenderData::MAX_FRAMES_IN_FLIGHT;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &m_ModelData.uniformBuffer) != VK_SUCCESS) {
        return;
    }
    
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(g_Device, m_ModelData.uniformBuffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &m_ModelData.uniformBufferMemory) != VK_SUCCESS) {
        return;
    }
    
    vkBindBufferMemory(g_Device, m_ModelData.uniformBuffer, m_ModelData.uniformBufferMemory, 0);

    // 持久映射（骨骼矩阵每帧更新；3 帧槽全量映射，每帧写 frameIndex 偏移槽）
    vkMapMemory(g_Device, m_ModelData.uniformBufferMemory, 0,
        MAX_BONES * sizeof(glm::mat4) * ModelRenderData::MAX_FRAMES_IN_FLIGHT, 0, &m_ModelData.boneBufferMapped);

    // 骨骼矩阵 texel buffer 视图（uniform texel buffer，每骨骼 4 个 vec4 列）
    // 必须在 SetupDescriptorSets 之前创建（descriptor 写入时视图必须有效，否则 GPU 读空视图不渲染）
    if (m_ModelData.boneBufferView == VK_NULL_HANDLE) {
        VkBufferViewCreateInfo bvi{};
        bvi.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        bvi.buffer = m_ModelData.uniformBuffer;
        bvi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        bvi.offset = 0;
        bvi.range = MAX_BONES * sizeof(glm::mat4);
        vkCreateBufferView(g_Device, &bvi, g_Allocator, &m_ModelData.boneBufferView);
    }
}

