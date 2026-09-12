#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "Core/VulkanManager.h"
#include "Core/VulkanContext.h"
#include "DescriptorSetCache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
bool IsExtensionAvailable(const ImVector<VkExtensionProperties>& properties,
                          const char* extension)
{
    for (const VkExtensionProperties& property : properties) {
        if (std::strcmp(property.extensionName, extension) == 0) {
            return true;
        }
    }
    return false;
}
}

// 初始化 Vulkan 实例、物理设备、逻辑设备以及全局基础命令资源。
// 该模块只负责设备级启动；窗口 surface/swapchain 生命周期由窗口模块负责。
void SetupVulkan(ImVector<const char*> instance_extensions)
{
    VkResult err;

    // 创建Vulkan实例
    VkInstanceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

    // 枚举实例扩展
    uint32_t properties_count;
    ImVector<VkExtensionProperties> properties;
    vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr);
    properties.resize(properties_count);
    err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);
    check_vk_result(err);

    // 添加必要的扩展
    if (IsExtensionAvailable(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
        instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

    // 添加 RenderDoc 支持扩展
    if (IsExtensionAvailable(properties, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    if (IsExtensionAvailable(properties, VK_EXT_DEBUG_REPORT_EXTENSION_NAME))
        instance_extensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);

    // 检查是否有 RenderDoc 层注入
    // RenderDoc 会通过环境变量或其他方式注入其层
    // 我们需要让 create_info.enabledLayerCount 为 0，让系统使用环境变量指定的层
    create_info.enabledLayerCount = 0;
    create_info.ppEnabledLayerNames = nullptr;

    // 尝试获取环境变量，检查是否有 RenderDoc 层
    const char* renderDocLayer = std::getenv("VK_INSTANCE_LAYERS");
    if (renderDocLayer) {
        std::printf("[VulkanManager] VK_INSTANCE_LAYERS: %s", renderDocLayer);
    }

    create_info.enabledExtensionCount = (uint32_t)instance_extensions.Size;
    create_info.ppEnabledExtensionNames = instance_extensions.Data;

    // 尝试创建实例，如果失败，尝试不使用任何层
    err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
    if (err != VK_SUCCESS) {
        std::printf("[VulkanManager] Failed to create instance: %d", err);

        // 尝试不使用任何层
        create_info.enabledLayerCount = 0;
        create_info.ppEnabledLayerNames = nullptr;
        err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
        if (err != VK_SUCCESS) {
            std::printf("[VulkanManager] Failed to create instance without layers: %d", err);
            check_vk_result(err);
        } else {
            std::printf("[VulkanManager] Created instance without layers");
        }
    } else {
        std::printf("[VulkanManager] Created instance successfully");
    }

    // 选择物理设备
    g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(g_Instance);
    if (g_PhysicalDevice == VK_NULL_HANDLE) {
        std::fprintf(stderr, "Failed to select physical device!\n");
        std::exit(-1);
    }

    // 打印物理设备信息
    VkPhysicalDeviceProperties deviceProps;
    vkGetPhysicalDeviceProperties(g_PhysicalDevice, &deviceProps);
    std::fprintf(stderr, "Selected physical device: %s\n", deviceProps.deviceName);
    std::fprintf(stderr, "Vulkan API version: %d.%d.%d\n",
                 VK_API_VERSION_MAJOR(deviceProps.apiVersion),
                 VK_API_VERSION_MINOR(deviceProps.apiVersion),
                 VK_API_VERSION_PATCH(deviceProps.apiVersion));

    // 枚举队列族信息
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &queueFamilyCount, queueFamilies.data());

    // 选择队列族
    g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);
    if (g_QueueFamily == (uint32_t)-1) {
        std::fprintf(stderr, "Failed to select queue family!\n");
        std::exit(-1);
    }

    // 创建逻辑设备
    {
        ImVector<const char*> device_extensions;
        device_extensions.push_back("VK_KHR_swapchain");

        // 枚举设备扩展
        uint32_t props_count;
        ImVector<VkExtensionProperties> props;
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &props_count, nullptr);
        props.resize(props_count);
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &props_count, props.Data);

        // 添加 RenderDoc 支持的设备扩展
        if (IsExtensionAvailable(props, VK_EXT_DEBUG_MARKER_EXTENSION_NAME))
            device_extensions.push_back(VK_EXT_DEBUG_MARKER_EXTENSION_NAME);

        // 添加描述符索引扩展（减少 descriptor set 切换）
        bool hasDescriptorIndexing = IsExtensionAvailable(props, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        if (hasDescriptorIndexing) {
            device_extensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        } else {
            std::cout << "[VulkanManager] Extension NOT available: " << VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME << std::endl;
        }

        // 但此前未启用该扩展+特性 → vkBindBufferMemory 驱动崩（RenderDoc 下必现；fix 老项目有）
        bool hasBufferDeviceAddress = IsExtensionAvailable(props, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
        if (hasBufferDeviceAddress) {
            device_extensions.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
        } else {
            std::cout << "[VulkanManager] Extension NOT available: " << VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME << std::endl;
        }

        // 基本的 subgroup 功能是 Vulkan 1.1 的核心特性，不需要额外扩展
        // 以下扩展是可选的，提供额外功能但不是必需的
        // VK_KHR_shader_subgroup_extended_types - 提供更多类型的 subgroup 操作
        // VK_EXT_subgroup_size_control - 允许控制 subgroup 大小
        // 这些扩展在某些设备上可能不可用，但不影响基本的 subgroup 功能

        const float queue_priority[] = { 1.0f };
        VkDeviceQueueCreateInfo queue_info[2] = {};
        queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info[0].queueFamilyIndex = g_QueueFamily;
        queue_info[0].queueCount = 1;
        queue_info[0].pQueuePriorities = queue_priority;

        // 查询独立 transfer 队列族（TRANSFER && !GRAPHICS；Vulkan 1.1 起 transfer-only 族必须显式 TRANSFER 标志）
        uint32_t transferFamily = (uint32_t)-1;
        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &qfCount, nullptr);
        if (qfCount > 0) {
            std::vector<VkQueueFamilyProperties> qf(qfCount);
            vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &qfCount, qf.data());
            for (uint32_t i = 0; i < qfCount; ++i) {
                if ((qf[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
                    !(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                    i != g_QueueFamily) {
                    transferFamily = i;
                    break;
                }
            }
        }
        uint32_t queueInfoCount = 1;
        if (transferFamily != (uint32_t)-1) {
            queue_info[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_info[1].queueFamilyIndex = transferFamily;
            queue_info[1].queueCount = 1;
            queue_info[1].pQueuePriorities = queue_priority;
            queueInfoCount = 2;
            std::cout << "[VulkanManager] Requesting dedicated transfer queue family " << transferFamily << std::endl;
        } else {
            std::cout << "[VulkanManager] No dedicated transfer queue family, uploads stay on graphics queue" << std::endl;
        }

        // 构建特性链 - 只添加设备支持的特性
        void* pNextChain = nullptr;

        // 描述符索引特性
        VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexingFeatures = {};
        if (hasDescriptorIndexing) {
            descriptorIndexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
            descriptorIndexingFeatures.shaderInputAttachmentArrayDynamicIndexing = VK_TRUE;
            descriptorIndexingFeatures.shaderUniformTexelBufferArrayDynamicIndexing = VK_TRUE;
            descriptorIndexingFeatures.shaderStorageTexelBufferArrayDynamicIndexing = VK_TRUE;
            descriptorIndexingFeatures.shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
            descriptorIndexingFeatures.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            descriptorIndexingFeatures.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
            descriptorIndexingFeatures.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
            descriptorIndexingFeatures.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
            descriptorIndexingFeatures.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            descriptorIndexingFeatures.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
            descriptorIndexingFeatures.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
            descriptorIndexingFeatures.descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
            descriptorIndexingFeatures.descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
            descriptorIndexingFeatures.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
            descriptorIndexingFeatures.descriptorBindingPartiallyBound = VK_TRUE;
            descriptorIndexingFeatures.descriptorBindingVariableDescriptorCount = VK_TRUE;
            descriptorIndexingFeatures.runtimeDescriptorArray = VK_TRUE;
            if (pNextChain) {
                descriptorIndexingFeatures.pNext = pNextChain;
            }
            pNextChain = &descriptorIndexingFeatures;
        }

        // Subgroup 扩展类型特性 - 只有在扩展可用时才添加
        // 注意：这个特性是可选的，不影响基本的 subgroup 功能
        /*
        VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures subgroupExtendedTypesFeatures = {};
        if (hasSubgroupExtendedTypes) {
            subgroupExtendedTypesFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES;
            subgroupExtendedTypesFeatures.shaderSubgroupExtendedTypes = VK_TRUE;
            if (pNextChain) {
                subgroupExtendedTypesFeatures.pNext = pNextChain;
            }
            pNextChain = &subgroupExtendedTypesFeatures;
            std::cout << "[VulkanManager] Subgroup extended types feature enabled" << std::endl;
        } else {
            std::cout << "[VulkanManager] Subgroup extended types feature NOT available - using fallback" << std::endl;
        }
        */

        VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddressFeatures = {};
        if (hasBufferDeviceAddress) {
            bufferDeviceAddressFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
            bufferDeviceAddressFeatures.bufferDeviceAddress = VK_TRUE;
            if (pNextChain) {
                bufferDeviceAddressFeatures.pNext = pNextChain;
            }
            pNextChain = &bufferDeviceAddressFeatures;
        }

        // 核心特性：fillModeNonSolid（VK_POLYGON_MODE_LINE 线框渲染，地形线框模式需要）
        // 最后入链成为链头；先查询物理设备支持情况，避免在低端/移动端设备上
        // 因请求不支持的 core 特性导致 vkCreateDevice 返回 VK_ERROR_FEATURE_NOT_PRESENT。
        // 其余 core 特性保持默认关闭，与既有行为一致。
        VkPhysicalDeviceFeatures physicalDeviceFeatures{};
        vkGetPhysicalDeviceFeatures(g_PhysicalDevice, &physicalDeviceFeatures);
        VkPhysicalDeviceFeatures2 physicalDeviceFeatures2 = {};
        if (physicalDeviceFeatures.fillModeNonSolid) {
            physicalDeviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            physicalDeviceFeatures2.features.fillModeNonSolid = VK_TRUE;
            if (pNextChain) {
                physicalDeviceFeatures2.pNext = pNextChain;
            }
            pNextChain = &physicalDeviceFeatures2;
        } else {
            std::cout << "[VulkanManager] fillModeNonSolid NOT supported - terrain wireframe mode unavailable" << std::endl;
        }

        // 创建设备
        VkDeviceCreateInfo device_create_info = {};
        device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_create_info.queueCreateInfoCount = queueInfoCount;
        device_create_info.pQueueCreateInfos = queue_info;
        device_create_info.enabledExtensionCount = (uint32_t)device_extensions.Size;
        device_create_info.ppEnabledExtensionNames = device_extensions.Data;
        device_create_info.pNext = pNextChain;
        err = vkCreateDevice(g_PhysicalDevice, &device_create_info, g_Allocator, &g_Device);
        check_vk_result(err);

        // 检查设备是否创建成功
        if (g_Device == VK_NULL_HANDLE) {
            std::fprintf(stderr, "Error: Device creation returned null handle\n");
            std::exit(-1);
        }
        std::fprintf(stderr, "Device created successfully\n");

        // 检查队列族索引是否有效
        if (g_QueueFamily == (uint32_t)-1) {
            std::fprintf(stderr, "Error: Invalid queue family index\n");
            std::exit(-1);
        }
        std::fprintf(stderr, "Queue family index: %d\n", g_QueueFamily);

        // 检查队列族是否存在
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &queueFamilyCount, nullptr);
        if (g_QueueFamily >= queueFamilyCount) {
            std::fprintf(stderr, "Error: Queue family index %d out of range (max: %d)\n",
                         g_QueueFamily, queueFamilyCount - 1);
            std::exit(-1);
        }

        // 获取队列
        vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
        if (g_Queue == VK_NULL_HANDLE) {
            std::fprintf(stderr, "Error: Failed to get device queue\n");
            std::exit(-1);
        }
        std::fprintf(stderr, "Queue obtained successfully\n");
    }

    // 创建描述符池
    {
        VkDescriptorPoolSize pool_sizes[] =
        {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE * 16 },
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 0;
        for (VkDescriptorPoolSize& pool_size : pool_sizes)
            pool_info.maxSets += pool_size.descriptorCount;
        pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        err = vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator, &g_DescriptorPool);
        check_vk_result(err);
    }

    // 创建命令池
    {
        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = g_QueueFamily;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        err = vkCreateCommandPool(g_Device, &poolInfo, g_Allocator, &g_CommandPool);
        check_vk_result(err);
    }

    // 初始化描述符集缓存
    DescriptorSetCache::GetInstance().Init();
    DescriptorSetCache::GetInstance().CreateDescriptorSetLayout();
    DescriptorSetCache::GetInstance().CreateDescriptorPool(1000);
}
