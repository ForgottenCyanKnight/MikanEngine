#include "RayTracingValidationDevice.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanRayTracingDevice.h"
#include <stdexcept>
#include <vector>
#include <iostream>
#include <cstring>
namespace VoxRTValidation {
namespace {
VKAPI_ATTR VkBool32 VKAPI_CALL Debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* message,void* user){
    auto& device=*static_cast<Device*>(user);
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT){++device.validationErrors;std::cerr<<"VULKAN ERROR: "<<message->pMessage<<'\n';}
    else if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT){++device.validationWarnings;std::cerr<<"VULKAN WARNING: "<<message->pMessage<<'\n';}
    return VK_FALSE;
}
}
void Require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void Check(VkResult result,const char* message){if(result!=VK_SUCCESS)throw std::runtime_error(std::string(message)+": VkResult="+std::to_string(result));}
Device::~Device(){Shutdown();}
void Device::Shutdown(){
    if(g_Device){vkDeviceWaitIdle(g_Device);if(fence)vkDestroyFence(g_Device,fence,nullptr);if(g_CommandPool)vkDestroyCommandPool(g_Device,g_CommandPool,nullptr);ResetRayTracingDevice();vkDestroyDevice(g_Device,nullptr);}
    g_Device=VK_NULL_HANDLE;g_CommandPool=VK_NULL_HANDLE;g_Queue=VK_NULL_HANDLE;fence=VK_NULL_HANDLE;
    if(messenger&&g_Instance){auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(g_Instance,"vkDestroyDebugUtilsMessengerEXT"));if(destroy)destroy(g_Instance,messenger,nullptr);messenger=VK_NULL_HANDLE;}
    if(g_Instance)vkDestroyInstance(g_Instance,nullptr);g_Instance=VK_NULL_HANDLE;g_PhysicalDevice=VK_NULL_HANDLE;
}
void Device::Initialize(bool requireCpu,bool enableValidation,bool fullShaders){
    Require(!g_Device && !g_Instance,"Validation requires an isolated process");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Mikan Vox Hardware RT Validation";app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};create.pApplicationInfo=&app;
    const char* layer="VK_LAYER_KHRONOS_validation";const char* extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback=Debug;debug.pUserData=this;
    if(enableValidation){create.enabledLayerCount=1;create.ppEnabledLayerNames=&layer;create.enabledExtensionCount=1;create.ppEnabledExtensionNames=&extension;create.pNext=&debug;}
    Check(vkCreateInstance(&create,nullptr,&g_Instance),"Create instance");
    if(enableValidation){auto make=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(g_Instance,"vkCreateDebugUtilsMessengerEXT"));Require(make,"Debug utils missing");Check(make(g_Instance,&debug,nullptr,&messenger),"Create debug messenger");}
    uint32_t count=0;Check(vkEnumeratePhysicalDevices(g_Instance,&count,nullptr),"Enumerate devices");
    std::vector<VkPhysicalDevice> devices(count);Check(vkEnumeratePhysicalDevices(g_Instance,&count,devices.data()),"Enumerate devices");
    RayTracingDeviceRequest request;
    for(auto physical:devices){
        VkPhysicalDeviceProperties candidateProperties{};vkGetPhysicalDeviceProperties(physical,&candidateProperties);
        if(requireCpu&&candidateProperties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU)continue;
        uint32_t extensionsCount=0;Check(vkEnumerateDeviceExtensionProperties(physical,nullptr,&extensionsCount,nullptr),"Enumerate extensions");
        std::vector<VkExtensionProperties> extensions(extensionsCount);Check(vkEnumerateDeviceExtensionProperties(physical,nullptr,&extensionsCount,extensions.data()),"Enumerate extensions");
        request.Probe(physical,extensions,VK_API_VERSION_1_2);if(!request.query.rayQuery)continue;
        uint32_t queuesCount=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&queuesCount,nullptr);
        std::vector<VkQueueFamilyProperties> queues(queuesCount);vkGetPhysicalDeviceQueueFamilyProperties(physical,&queuesCount,queues.data());
        // The foundation's query barrier includes fragment consumers; use a queue supporting both stages.
        constexpr auto required=VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT;
        for(uint32_t q=0;q<queuesCount;++q)if(queues[q].queueCount && (queues[q].queueFlags&required)==required){g_PhysicalDevice=physical;g_QueueFamily=q;break;}
        if(g_PhysicalDevice)break;
    }
    Require(g_PhysicalDevice!=VK_NULL_HANDLE,"No Vulkan 1.2 device supports hardware acceleration structures and Ray Query");
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&properties);name=properties.deviceName;
    const float priority=1;VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queue.queueFamilyIndex=g_QueueFamily;queue.queueCount=1;queue.pQueuePriorities=&priority;
    void* features=nullptr;request.PrependFeatures(features);
    VkPhysicalDeviceDescriptorIndexingFeatures indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};supported.pNext=&indexing;
    vkGetPhysicalDeviceFeatures2(g_PhysicalDevice,&supported);
    if(fullShaders){Require(indexing.runtimeDescriptorArray&&indexing.shaderSampledImageArrayNonUniformIndexing,"full shaders need descriptor indexing");
        indexing.pNext=features;features=&indexing;}
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};device.pNext=features;device.pEnabledFeatures=fullShaders?&supported.features:nullptr;device.queueCreateInfoCount=1;device.pQueueCreateInfos=&queue;
    device.enabledExtensionCount=uint32_t(request.extensions.size());device.ppEnabledExtensionNames=request.extensions.data();
    Check(vkCreateDevice(g_PhysicalDevice,&device,nullptr,&g_Device),"Create device");vkGetDeviceQueue(g_Device,g_QueueFamily,0,&g_Queue);
    InitializeRayTracingDevice(g_Device,g_PhysicalDevice,request);Require(GetRayTracingDeviceCapabilities().rayQuery,"Ray Query dispatch unavailable");
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.queueFamilyIndex=g_QueueFamily;
    Check(vkCreateCommandPool(g_Device,&pool,nullptr,&g_CommandPool),"Create command pool");
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};Check(vkCreateFence(g_Device,&fenceInfo,nullptr,&fence),"Create fence");
    std::cout<<(properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU?"CPU Vulkan: ":"GPU: ")<<name<<"; headless compute + Vulkan Ray Query\n";
}
VkCommandBuffer Device::Begin(){
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocate.commandPool=g_CommandPool;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
    VkCommandBuffer cmd=VK_NULL_HANDLE;Check(vkAllocateCommandBuffers(g_Device,&allocate,&cmd),"Allocate command buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;Check(vkBeginCommandBuffer(cmd,&begin),"Begin command buffer");return cmd;
}
void Device::SubmitAndWait(VkCommandBuffer cmd){
    Check(vkEndCommandBuffer(cmd),"End command buffer");Check(vkResetFences(g_Device,1,&fence),"Reset fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;
    Check(vkQueueSubmit(g_Queue,1,&submit,fence),"Submit compute");Check(vkWaitForFences(g_Device,1,&fence,VK_TRUE,UINT64_MAX),"Wait compute");
    vkFreeCommandBuffers(g_Device,g_CommandPool,1,&cmd);
}
}
