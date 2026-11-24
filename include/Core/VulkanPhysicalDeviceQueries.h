#pragma once

#include <vulkan/vulkan.h>

// Android's API 26 linker stubs do not export the Vulkan 1.1 queries.
inline PFN_vkGetPhysicalDeviceFeatures2 LoadPhysicalDeviceFeatures2(VkInstance instance)
{
    return reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
}
