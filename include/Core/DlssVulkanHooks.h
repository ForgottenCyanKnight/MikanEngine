#pragma once
// Only Game target uses these explicit Streamline dispatch wrappers. Vendor source is untouched.
#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif
#include <vulkan/vulkan.h>
VkResult VKAPI_CALL MikanFG_CreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
VkResult VKAPI_CALL MikanFG_CreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
VkResult VKAPI_CALL MikanFG_CreateSwapchain(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
void VKAPI_CALL MikanFG_DestroySwapchain(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
VkResult VKAPI_CALL MikanFG_GetSwapchainImages(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
VkResult VKAPI_CALL MikanFG_AcquireNextImage(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
VkResult VKAPI_CALL MikanFG_Present(VkQueue, const VkPresentInfoKHR*);
VkResult VKAPI_CALL MikanFG_DeviceWaitIdle(VkDevice);
void VKAPI_CALL MikanFG_DestroySurface(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*);
#define vkCreateInstance MikanFG_CreateInstance
#define vkCreateDevice MikanFG_CreateDevice
#define vkCreateSwapchainKHR MikanFG_CreateSwapchain
#define vkDestroySwapchainKHR MikanFG_DestroySwapchain
#define vkGetSwapchainImagesKHR MikanFG_GetSwapchainImages
#define vkAcquireNextImageKHR MikanFG_AcquireNextImage
#define vkQueuePresentKHR MikanFG_Present
#define vkDeviceWaitIdle MikanFG_DeviceWaitIdle
#define vkDestroySurfaceKHR MikanFG_DestroySurface
