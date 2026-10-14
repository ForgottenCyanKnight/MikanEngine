#include "Core/VulkanRayTracingDevice.h"
#include "Core/Log.h"
#include <cstring>
#include <cstdlib>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
namespace {
    RayTracingDeviceCapabilities capabilities;
    RayTracingFunctions functions;
}
const RayTracingDeviceCapabilities& GetRayTracingDeviceCapabilities(){return capabilities;}
const RayTracingFunctions& GetRayTracingFunctions(){return functions;}
void ResetRayTracingDevice(){capabilities={};functions={};}
void RayTracingDeviceRequest::Probe(VkPhysicalDevice physical,std::span<const VkExtensionProperties> available,uint32_t api) {
    extensions.clear();address.bufferDeviceAddress=VK_FALSE;acceleration.accelerationStructure=VK_FALSE;query.rayQuery=VK_FALSE;
    if(const char* mode=std::getenv("MIKAN_HWRT_DEVICE");mode&&mode[0]=='0'){LOGI("[HardwareRT] device features disabled by MIKAN_HWRT_DEVICE=0");return;}
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);
    // Core 1.2 supplies BDA, descriptor indexing, SPIR-V 1.4 and shader float controls.
    if(api<VK_API_VERSION_1_2 || properties.apiVersion<VK_API_VERSION_1_2)return;
    const auto has=[&](const char* name){for(const auto& p:available)if(std::strcmp(p.extensionName,name)==0)return true;return false;};
    const char* diagnostic=std::getenv("MIKAN_HWRT_DEVICE");
#ifdef _WIN32
    // RenderDoc capture on non-NVIDIA drivers kills the device on the first scene-frame
    // submit when VK_KHR_acceleration_structure sits in the device extension chain
    // (bisected 2026-10-14: AMD 780M + RenderDoc 1.46 loses the device even in scenes
    // with zero RT entities; RayQuery is not required, BDA alone is fine; NVIDIA GPUs
    // are unaffected). Drop AS/RayQuery under capture and keep BDA.
    // MIKAN_RD_KEEP_HWRT=1 forces them back on for testing future RenderDoc builds.
    if(const char* keep=std::getenv("MIKAN_RD_KEEP_HWRT");!(keep&&keep[0]=='1')
       && GetModuleHandleA("renderdoc.dll") && properties.vendorID!=0x10DE){
        if(!diagnostic){
            diagnostic="bda";
            LOGW("[HardwareRT] RenderDoc capture on non-NVIDIA GPU (%s): AS/RayQuery extensions disabled to avoid DEVICE_LOST (MIKAN_RD_KEEP_HWRT=1 overrides)",properties.deviceName);
        }
    }
#endif
    const bool hasAs=!(diagnostic&&std::strcmp(diagnostic,"bda")==0)&&has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    const bool hasQuery=!(diagnostic&&std::strcmp(diagnostic,"as")==0)&&hasAs && has(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    supported.pNext=&address;address.pNext=hasAs?&acceleration:nullptr;acceleration.pNext=hasQuery?&query:nullptr;
    vkGetPhysicalDeviceFeatures2(physical,&supported);
    address.pNext=nullptr;acceleration.pNext=nullptr;query.pNext=nullptr;
    // Enable only the fields used by this foundation, never indirect/host builds or capture replay.
    const bool bda=address.bufferDeviceAddress;const bool as=hasAs && bda && acceleration.accelerationStructure;
    const bool rq=hasQuery && as && query.rayQuery;
    address={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};address.bufferDeviceAddress=bda;
    acceleration={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};acceleration.accelerationStructure=as;
    query={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};query.rayQuery=rq;
    if(as){extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);}
    if(rq)extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
}
void RayTracingDeviceRequest::PrependFeatures(void*& chain) {
    if(address.bufferDeviceAddress){address.pNext=chain;chain=&address;}
    if(acceleration.accelerationStructure){acceleration.pNext=chain;chain=&acceleration;}
    if(query.rayQuery){query.pNext=chain;chain=&query;}
}
void InitializeRayTracingDevice(VkDevice device,VkPhysicalDevice physical,const RayTracingDeviceRequest& request) {
    ResetRayTracingDevice();
    functions.getBufferAddress=reinterpret_cast<PFN_vkGetBufferDeviceAddress>(vkGetDeviceProcAddr(device,"vkGetBufferDeviceAddress"));
    capabilities.bufferDeviceAddress=request.address.bufferDeviceAddress && functions.getBufferAddress;
    if(request.acceleration.accelerationStructure) {
#define LOAD(member,name) functions.member=reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device,#name))
        LOAD(create,vkCreateAccelerationStructureKHR);LOAD(destroy,vkDestroyAccelerationStructureKHR);
        LOAD(getBuildSizes,vkGetAccelerationStructureBuildSizesKHR);LOAD(build,vkCmdBuildAccelerationStructuresKHR);
        LOAD(getAddress,vkGetAccelerationStructureDeviceAddressKHR);
        LOAD(writeProperties,vkCmdWriteAccelerationStructuresPropertiesKHR);
        LOAD(copy,vkCmdCopyAccelerationStructureKHR);
#undef LOAD
        capabilities.accelerationStructure=capabilities.bufferDeviceAddress && functions.create && functions.destroy && functions.getBuildSizes && functions.build && functions.getAddress;
        if(capabilities.accelerationStructure){VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p.pNext=&capabilities.limits;vkGetPhysicalDeviceProperties2(physical,&p);}
    }
    capabilities.rayQuery=capabilities.accelerationStructure && request.query.rayQuery;
    LOGI("[HardwareRT] enabled: BDA=%d AS=%d RayQuery=%d",capabilities.bufferDeviceAddress,capabilities.accelerationStructure,capabilities.rayQuery);
}
