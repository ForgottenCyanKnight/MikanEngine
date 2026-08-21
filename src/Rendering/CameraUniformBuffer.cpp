// CameraUniformBuffer.cpp - camera data UBO management
#include "Rendering/CameraUniformBuffer.h"

#include <iostream>
#include <cstring>

extern VkDevice g_Device;
extern VkPhysicalDevice g_PhysicalDevice;
extern VkAllocationCallbacks* g_Allocator;

static uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return 0;
}

bool CameraUniformBuffer::Create()
{
    Cleanup();

    struct CameraData {
        glm::mat4 view;
        glm::mat4 proj;
        glm::mat4 viewInverse;
        glm::mat4 projInverse;
        glm::vec4 cameraPos;
    };
    m_BufferSize = sizeof(CameraData);

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = m_BufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &bufferInfo, nullptr, &m_Buffer) != VK_SUCCESS) {
        std::cout << "[CameraUniformBuffer] Failed to create buffer" << std::endl;
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(g_Device, m_Buffer, &memRequirements);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_Device, &allocInfo, nullptr, &m_Memory) != VK_SUCCESS) {
        std::cout << "[CameraUniformBuffer] Failed to allocate memory" << std::endl;
        vkDestroyBuffer(g_Device, m_Buffer, nullptr);
        m_Buffer = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(g_Device, m_Buffer, m_Memory, 0);
    vkMapMemory(g_Device, m_Memory, 0, m_BufferSize, 0, &m_Mapped);
    return true;
}

void CameraUniformBuffer::Cleanup()
{
    if (m_Mapped != nullptr && m_Memory != VK_NULL_HANDLE) {
        vkUnmapMemory(g_Device, m_Memory);
        m_Mapped = nullptr;
    }
    if (m_Buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_Device, m_Buffer, g_Allocator);
        m_Buffer = VK_NULL_HANDLE;
    }
    if (m_Memory != VK_NULL_HANDLE) {
        vkFreeMemory(g_Device, m_Memory, g_Allocator);
        m_Memory = VK_NULL_HANDLE;
    }
}

void CameraUniformBuffer::Update(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPos)
{
    if (m_Mapped == nullptr) return;

    struct CameraData {
        glm::mat4 view;
        glm::mat4 proj;
        glm::mat4 viewInverse;
        glm::mat4 projInverse;
        glm::vec4 cameraPos;
    };
    CameraData cameraData{};
    cameraData.view = view;
    cameraData.proj = proj;
    cameraData.viewInverse = glm::inverse(view);
    cameraData.projInverse = glm::inverse(proj);
    cameraData.cameraPos = glm::vec4(cameraPos, 1.0f);
    std::memcpy(m_Mapped, &cameraData, sizeof(CameraData));
}