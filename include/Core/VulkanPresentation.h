#pragma once
#include "Platform/Export.h"
#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif
#include <vulkan/vulkan.h>
struct ImGui_ImplVulkanH_Window;

namespace Core {
// A second queue in the graphics/present family is exclusively owned by the
// bounded presentation worker. The graphics queue stays on the render thread.
MIKAN_API void ConfigurePresentationQueue(VkQueue queue);
MIKAN_API VkResult DrainPresentationQueue();
MIKAN_API void ShutdownPresentationQueue();
MIKAN_API VkResult PresentSwapchain(ImGui_ImplVulkanH_Window* window,const VkPresentInfoKHR& info);
MIKAN_API VkResult AcquirePresentationImage(VkSwapchainKHR swapchain,VkSemaphore available,uint32_t* image);
MIKAN_API void ResetPresentationImage();
// Device-wide waits must also synchronize host access to the present queue.
MIKAN_API VkResult WaitForDeviceIdle(VkDevice device);
}
