#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <cstdint>
#include "Platform/Export.h"
namespace Core::DlssFG {
bool Initialize();
MIKAN_API void SetRequested(bool requested);
MIKAN_API bool IsRequested();
MIKAN_API bool IsSupported();
MIKAN_API bool IsEnabled();
MIKAN_API bool IsInitialized();
const char* VulkanLibraryPath();
void DeviceReady(VkPhysicalDevice physical);
void BeginFrame(uint32_t serial, bool gameFrame);
MIKAN_API void SetEditorViewportRect(uint32_t x,uint32_t y,uint32_t width,uint32_t height);
void Marker(uint32_t marker);
void SubmitGuides(VkCommandBuffer cmd, VkImageView guide, uint32_t width, uint32_t height,
                  const glm::mat4& view, const glm::mat4& projection, const glm::vec2& jitter, bool reset);
void CaptureHudless(VkCommandBuffer cmd, VkImage backbuffer, VkFormat format, uint32_t width, uint32_t height);
void Shutdown();
}
