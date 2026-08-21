#pragma once
// CameraUniformBuffer.h - camera data UBO management (view/proj/inverses/position)
#include "Platform/Export.h"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

class MIKAN_API CameraUniformBuffer {
public:
    bool Create();
    void Cleanup();
    void Update(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPos);

    VkBuffer GetBuffer() const { return m_Buffer; }
    void* GetMapped() const { return m_Mapped; }
    VkDeviceSize GetSize() const { return m_BufferSize; }

private:
    VkBuffer m_Buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_Memory = VK_NULL_HANDLE;
    void* m_Mapped = nullptr;
    VkDeviceSize m_BufferSize = 0;
};