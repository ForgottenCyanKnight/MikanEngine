#pragma once
#include "Platform/Export.h"
#include <vulkan/vulkan.h>
#include <span>
#include <vector>

// Negotiation is local to vkCreateDevice; no extension feature is assumed from its name.
struct RayTracingDeviceRequest {
    VkPhysicalDeviceBufferDeviceAddressFeatures address{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    std::vector<const char*> extensions;
    void Probe(VkPhysicalDevice physical,std::span<const VkExtensionProperties> available,uint32_t applicationApi);
    void PrependFeatures(void*& chain);
};
struct MIKAN_API RayTracingDeviceCapabilities {
    bool bufferDeviceAddress=false;
    bool accelerationStructure=false;
    bool rayQuery=false;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
};
struct RayTracingFunctions {
    PFN_vkGetBufferDeviceAddress getBufferAddress=nullptr;
    PFN_vkCreateAccelerationStructureKHR create=nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroy=nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR getBuildSizes=nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR build=nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getAddress=nullptr;
    PFN_vkCmdWriteAccelerationStructuresPropertiesKHR writeProperties=nullptr;
    PFN_vkCmdCopyAccelerationStructureKHR copy=nullptr;
};
MIKAN_API const RayTracingDeviceCapabilities& GetRayTracingDeviceCapabilities();
const RayTracingFunctions& GetRayTracingFunctions();
void InitializeRayTracingDevice(VkDevice device,VkPhysicalDevice physical,const RayTracingDeviceRequest& request);
void ResetRayTracingDevice();
