// Vulkan device and instance shutdown coordinator.

#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "Core/VulkanShutdown.h"

#include "Core/VulkanContext.h"
#include "Core/VulkanFrameLoop.h"
#include "Core/VulkanRuntimeResources.h"
#include "DescriptorSetCache.h"
#include "Rendering/CloudNoise3D.h"

void CleanupVulkan()
{
    // EngineMain 已在这里之前等待设备空闲；先释放粒子叠加管线和 buffer，
    // 避免静态对象在 g_Device 销毁后再调用 Vulkan 销毁函数。
    ResetFrameLoopSynchronizationState();
    CleanupParticleResources();
    GetCloudNoise3D().Cleanup();
    DescriptorSetCache::GetInstance().Cleanup();
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);
    if (g_CommandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(g_Device, g_CommandPool, g_Allocator);
    }
    vkDestroyDevice(g_Device, g_Allocator);
    // 其成员（PointShadow/CascadeShadow renderer）Cleanup 用 g_Device 守卫；悬空句柄（非 NULL 已销毁）
    // 会让守卫失效 → vulkan-1.dll 内 0xC0000005（静态析构期崩溃）。
    // Vulkan 规范：device 销毁后子对象句柄全部失效并隐式释放，后续跳过逐个 vkDestroy 是安全正确的。
    g_Device = VK_NULL_HANDLE;
    vkDestroyInstance(g_Instance, g_Allocator);
    g_Instance = VK_NULL_HANDLE;
}
