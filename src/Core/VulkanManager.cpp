// Vulkan Manager
// 负责 Vulkan 相关的初始化、配置和管理

// 启用Vulkan光线追踪扩展（必须在包含vulkan.h之前定义）
#define VK_ENABLE_BETA_EXTENSIONS

#include "EngineGlobal.h"
#include "EngineConfig.h"
#include "RenderGlobals.h"
#include "Core/Log.h"


#include "imgui_impl_vulkan.h"
#include "SceneRenderer.h"
#include "SkyboxRenderer.h"
#include "AtmosphereRenderer.h"
#include "Game/GameManager.h"
#include "TextRenderer.h"
#include "RenderTarget.h"
#include "FullscreenQuad.h"
#include "Rendering/PostProcessChain.h"
#include "Rendering/CMAA2.h"
#include "TexturePool.h"
#include "DescriptorSetCache.h"
#include "Rendering/Renderer2D.h"
#include "UI/Canvas2D.h"
#include "Rendering/ShaderHotReload.h"
#include "ECS/SceneECS.h"
#include "ECS/Components.h"
#include "Core/ProjectManager.h"
#include <functional>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vector>
#include <array>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <glm/glm.hpp>
#include <glm/ext/matrix_transform.hpp>

// Android 平台使用 logcat 输出日志
#ifdef __ANDROID__
#include <android/log.h>
#endif

// 全局帧计数器，用于蓝噪声时序抖动
static uint32_t g_frameCounter = 0;

// check_vk_result 函数实现
void check_vk_result(VkResult err)
{
    if (err == 0) {
        return;
    }
    
    const char* error_text = "";
    const char* error_description = "";
    switch (err) {
        case VK_NOT_READY: 
            error_text = "VK_NOT_READY";
            error_description = "Operation is not ready to complete";
            break;
        case VK_TIMEOUT: 
            error_text = "VK_TIMEOUT";
            error_description = "Operation timed out";
            break;
        case VK_EVENT_SET: 
            error_text = "VK_EVENT_SET";
            error_description = "Event is set";
            break;
        case VK_EVENT_RESET: 
            error_text = "VK_EVENT_RESET";
            error_description = "Event is reset";
            break;
        case VK_INCOMPLETE: 
            error_text = "VK_INCOMPLETE";
            error_description = "Operation completed but not all data was returned";
            break;
        case VK_ERROR_OUT_OF_HOST_MEMORY: 
            error_text = "VK_ERROR_OUT_OF_HOST_MEMORY";
            error_description = "Out of host memory";
            break;
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: 
            error_text = "VK_ERROR_OUT_OF_DEVICE_MEMORY";
            error_description = "Out of device memory";
            break;
        case VK_ERROR_INITIALIZATION_FAILED: 
            error_text = "VK_ERROR_INITIALIZATION_FAILED";
            error_description = "Initialization failed";
            break;
        case VK_ERROR_DEVICE_LOST: 
            error_text = "VK_ERROR_DEVICE_LOST";
            error_description = "Device lost - likely due to invalid operation, memory access, or driver crash";
            break;
        case VK_ERROR_MEMORY_MAP_FAILED: 
            error_text = "VK_ERROR_MEMORY_MAP_FAILED";
            error_description = "Memory map failed";
            break;
        case VK_ERROR_LAYER_NOT_PRESENT: 
            error_text = "VK_ERROR_LAYER_NOT_PRESENT";
            error_description = "Requested layer not present";
            break;
        case VK_ERROR_EXTENSION_NOT_PRESENT: 
            error_text = "VK_ERROR_EXTENSION_NOT_PRESENT";
            error_description = "Requested extension not present";
            break;
        case VK_ERROR_FEATURE_NOT_PRESENT: 
            error_text = "VK_ERROR_FEATURE_NOT_PRESENT";
            error_description = "Requested feature not present";
            break;
        case VK_ERROR_INCOMPATIBLE_DRIVER: 
            error_text = "VK_ERROR_INCOMPATIBLE_DRIVER";
            error_description = "Incompatible driver";
            break;
        case VK_ERROR_TOO_MANY_OBJECTS: 
            error_text = "VK_ERROR_TOO_MANY_OBJECTS";
            error_description = "Too many objects";
            break;
        case VK_ERROR_FORMAT_NOT_SUPPORTED: 
            error_text = "VK_ERROR_FORMAT_NOT_SUPPORTED";
            error_description = "Format not supported";
            break;
        case VK_ERROR_FRAGMENTED_POOL: 
            error_text = "VK_ERROR_FRAGMENTED_POOL";
            error_description = "Pool is fragmented";
            break;
        case VK_ERROR_UNKNOWN: 
            error_text = "VK_ERROR_UNKNOWN";
            error_description = "Unknown error";
            break;
        case VK_ERROR_OUT_OF_POOL_MEMORY: 
            error_text = "VK_ERROR_OUT_OF_POOL_MEMORY";
            error_description = "Out of pool memory";
            break;
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: 
            error_text = "VK_ERROR_INVALID_EXTERNAL_HANDLE";
            error_description = "Invalid external handle";
            break;
        case VK_ERROR_FRAGMENTATION: 
            error_text = "VK_ERROR_FRAGMENTATION";
            error_description = "Fragmentation";
            break;
        case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: 
            error_text = "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
            error_description = "Invalid opaque capture address";
            break;
        case VK_ERROR_SURFACE_LOST_KHR: 
            error_text = "VK_ERROR_SURFACE_LOST_KHR";
            error_description = "Surface lost";
            break;
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: 
            error_text = "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
            error_description = "Native window in use";
            break;
        case VK_SUBOPTIMAL_KHR: 
            error_text = "VK_SUBOPTIMAL_KHR";
            error_description = "Suboptimal";
            break;
        case VK_ERROR_OUT_OF_DATE_KHR: 
            error_text = "VK_ERROR_OUT_OF_DATE_KHR";
            error_description = "Out of date";
            break;
        case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: 
            error_text = "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
            error_description = "Incompatible display";
            break;
        case VK_ERROR_VALIDATION_FAILED_EXT: 
            error_text = "VK_ERROR_VALIDATION_FAILED_EXT";
            error_description = "Validation failed";
            break;
        case VK_ERROR_INVALID_SHADER_NV: 
            error_text = "VK_ERROR_INVALID_SHADER_NV";
            error_description = "Invalid shader";
            break;
        case VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR: 
            error_text = "VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR";
            error_description = "Image usage not supported";
            break;
        case VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR: 
            error_text = "VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR";
            error_description = "Video picture layout not supported";
            break;
        case VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR: 
            error_text = "VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR";
            error_description = "Video profile operation not supported";
            break;
        case VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR: 
            error_text = "VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR";
            error_description = "Video profile format not supported";
            break;
        case VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR: 
            error_text = "VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR";
            error_description = "Video profile codec not supported";
            break;
        case VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR: 
            error_text = "VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR";
            error_description = "Video std version not supported";
            break;
        case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT: 
            error_text = "VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT";
            error_description = "Invalid DRM format modifier plane layout";
            break;
        case VK_ERROR_NOT_PERMITTED_KHR: 
            error_text = "VK_ERROR_NOT_PERMITTED_KHR";
            error_description = "Not permitted";
            break;
        case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: 
            error_text = "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
            error_description = "Full screen exclusive mode lost";
            break;
        default: 
            error_text = "UNKNOWN_ERROR";
            error_description = "Unknown error";
            break;
    }
    
    fprintf(stderr, "[vulkan] Error: VkResult = %d (%s) - %s\n", static_cast<int>(err), error_text, error_description);
    if (err == VK_ERROR_VALIDATION_FAILED_EXT || err == VK_ERROR_UNKNOWN) {
        // 断言失败，可以在此处设置断点
        #ifdef _DEBUG
        __debugbreak();
        #endif
    }
}
// 全局变量定义
// Vulkan核心对象
VkAllocationCallbacks*   g_Allocator = nullptr;  // 内存分配回调
VkInstance               g_Instance = VK_NULL_HANDLE;  // Vulkan实例
VkPhysicalDevice         g_PhysicalDevice = VK_NULL_HANDLE;  // 物理设备
VkDevice                 g_Device = VK_NULL_HANDLE;  // 逻辑设备
uint32_t                 g_QueueFamily = (uint32_t)-1;  // 队列族索引
VkQueue                  g_Queue = VK_NULL_HANDLE;  // 队列
VkDescriptorPool         g_DescriptorPool = VK_NULL_HANDLE;  // 描述符池
VkCommandPool            g_CommandPool = VK_NULL_HANDLE;  // 命令池

// 窗口和渲染相关
ImGui_ImplVulkanH_Window g_MainWindowData;  // 窗口数据
uint32_t                 g_MinImageCount = 2;  // 最小图像数量

// ===== GPU 时间戳查询（2026-08-10：waitGPU 细分各 pass GPU 耗时）=====
bool                     g_SwapChainRebuild = false;  // 交换链重建标志
#ifdef __ANDROID__
bool                     g_VSyncEnabled = true;   // Android 默认开启 FIFO 垂直同步，避免移动设备撕裂和无意义的超高帧率
#else
bool                     g_VSyncEnabled = false;  // 桌面默认保持非 VSync + 三重缓冲配置
#endif
bool                     g_TripleBufferingEnabled = true;  // 桌面默认三重缓冲；Android 也保留至少 3 张交换链图像
bool                     g_IsPaused = false;  // 应用暂停标志

// 相机和输入控制器
Camera                   g_Camera(glm::vec3(0.0f, 8.0f, 12.0f)); // 编辑器相机:世界原点附近,默认朝向 -Z 看向原点
InputController          g_InputController;
RunMode                  g_RunMode = RunMode::Editor;  // 默认编辑器模式
bool                     g_EditorActive = false;       // 编辑器 DLL 附着状态（EngineMain attach/detach 置位）
GizmoMode                g_GizmoMode = GizmoMode::Translate;
bool                     g_ShowAxis = true;

// 场景渲染器
extern SceneRenderer g_SceneRenderer;
extern SkyboxRenderer g_SkyboxRenderer;
extern RenderTarget g_SceneRenderTarget;
extern RenderTarget g_GameRenderTarget;
extern FullscreenQuad g_FullscreenQuad;
extern FullscreenQuad g_SceneCompositeQuad;
extern FullscreenQuad g_GameCompositeQuad;
extern AtmosphereRenderer g_AtmosphereRenderer;
extern bool g_AtmosphereEnabled;

CMAA2 g_SceneCMAA2;   // 2026-08-17：三链独立实例（Scene/Game/Swap 尺寸不同——共用单实例会尺寸不匹配静默失效）
CMAA2 g_GameCMAA2;
CMAA2 g_SwapCMAA2;

// 合并 render pass（游戏模式）：subpass 0 = 几何/2D/UI 写离屏 G-Buffer，
// subpass 1 = 全屏四边形经 input attachment 从 tile 内读颜色0 合成到 swapchain（G-Buffer 不写回主存）
VkRenderPass g_CompositeRenderPass = VK_NULL_HANDLE;
VkRenderPass g_CompositeUIPass = VK_NULL_HANDLE;   // swapchain UI 叠加 pass（loadOp=LOAD，链后画 UI）
std::vector<VkFramebuffer> g_CompositeFramebuffers;  // per swapchain image（离屏附件 + swapchain 附件）

// ===== TAA 历史纹理（2026-08-17 重做；单历史 + 每帧 copy，Scene/Game 各一份）=====
// 声明放全局区（Cleanup 在文件前部使用）；Ensure/Prepare/Copy 函数定义在 CopyAOHistory 之后
static VkImage g_SceneTAAHistory = VK_NULL_HANDLE;
static VkDeviceMemory g_SceneTAAHistoryMem = VK_NULL_HANDLE;
static VkImageView g_SceneTAAHistoryView = VK_NULL_HANDLE;
static VkImage g_GameTAAHistory = VK_NULL_HANDLE;
static VkDeviceMemory g_GameTAAHistoryMem = VK_NULL_HANDLE;
static VkImageView g_GameTAAHistoryView = VK_NULL_HANDLE;
static VkSampler g_TAAHistorySampler = VK_NULL_HANDLE;
static uint32_t g_TAAHistoryW = 0, g_TAAHistoryH = 0;
static bool g_TAAHistoryNeedsClear = true;   // 创建后首帧 clear（UNDEFINED 内容 → 0，防 NaN 传染）
static glm::mat4 s_PrevView = glm::mat4(1.0f);
static glm::mat4 s_PrevProj = glm::mat4(1.0f);
static glm::mat4 s_PrevViewProj = glm::mat4(1.0f);
static void CleanupAOAndSSGIHistoryTextures();

// 检查扩展是否可用
static bool IsExtensionAvailable(const ImVector<VkExtensionProperties>& properties, const char* extension)
{
    for (const VkExtensionProperties& p : properties)
        if (strcmp(p.extensionName, extension) == 0)
            return true;
    return false;
}



// 初始化Vulkan
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
    const char* renderDocLayer = getenv("VK_INSTANCE_LAYERS");
    if (renderDocLayer) {
        printf("[VulkanManager] VK_INSTANCE_LAYERS: %s", renderDocLayer);
    }

    create_info.enabledExtensionCount = (uint32_t)instance_extensions.Size;
    create_info.ppEnabledExtensionNames = instance_extensions.Data;
    
    // 尝试创建实例，如果失败，尝试不使用任何层
    err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
    if (err != VK_SUCCESS) {
        printf("[VulkanManager] Failed to create instance: %d", err);
        
        // 尝试不使用任何层
        create_info.enabledLayerCount = 0;
        create_info.ppEnabledLayerNames = nullptr;
        err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
        if (err != VK_SUCCESS) {
            printf("[VulkanManager] Failed to create instance without layers: %d", err);
            check_vk_result(err);
        } else {
            printf("[VulkanManager] Created instance without layers");
        }
    } else {
        printf("[VulkanManager] Created instance successfully");
    }

    // 选择物理设备
    g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(g_Instance);
    if (g_PhysicalDevice == VK_NULL_HANDLE)
    {
        fprintf(stderr, "Failed to select physical device!\n");
        exit(-1);
    }

    // 打印物理设备信息
    VkPhysicalDeviceProperties deviceProps;
    vkGetPhysicalDeviceProperties(g_PhysicalDevice, &deviceProps);
    fprintf(stderr, "Selected physical device: %s\n", deviceProps.deviceName);
    fprintf(stderr, "Vulkan API version: %d.%d.%d\n", 
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
    if (g_QueueFamily == (uint32_t)-1)
    {
        fprintf(stderr, "Failed to select queue family!\n");
        exit(-1);
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
        
        // 缓冲设备地址扩展（2026-08-06 修复：模型顶点/索引缓冲用 SHADER_DEVICE_ADDRESS_BIT
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

        // 创建队列（2026-08-10：额外请求独立 transfer 队列族，供纹理上传离屏使用——不请求则 vkGetDeviceQueue 返回 NULL）
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
                if ((qf[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && i != g_QueueFamily) {
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
        

        // buffer device address 特性（2026-08-06 修复：与扩展配套；模型缓冲用 SHADER_DEVICE_ADDRESS_BIT 必须启用）
        VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddressFeatures = {};
        if (hasBufferDeviceAddress) {
            bufferDeviceAddressFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
            bufferDeviceAddressFeatures.bufferDeviceAddress = VK_TRUE;
            if (pNextChain) {
                bufferDeviceAddressFeatures.pNext = pNextChain;
            }
            pNextChain = &bufferDeviceAddressFeatures;
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
            fprintf(stderr, "Error: Device creation returned null handle\n");
            exit(-1);
        }
        fprintf(stderr, "Device created successfully\n");
        
        // 检查队列族索引是否有效
        if (g_QueueFamily == (uint32_t)-1) {
            fprintf(stderr, "Error: Invalid queue family index\n");
            exit(-1);
        }
        fprintf(stderr, "Queue family index: %d\n", g_QueueFamily);
        
        // 检查队列族是否存在
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &queueFamilyCount, nullptr);
        if (g_QueueFamily >= queueFamilyCount) {
            fprintf(stderr, "Error: Queue family index %d out of range (max: %d)\n", g_QueueFamily, queueFamilyCount - 1);
            exit(-1);
        }
        
        // 获取队列
        vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
        if (g_Queue == VK_NULL_HANDLE) {
            fprintf(stderr, "Error: Failed to get device queue\n");
            exit(-1);
        }
        fprintf(stderr, "Queue obtained successfully\n");
    }

    // 创建描述符池
    {
        VkDescriptorPoolSize pool_sizes[] = 
        {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE * 16 }, // 增加描述符池大小以支持 MRT
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

// 清理Vulkan
void CleanupVulkan()
{
    DescriptorSetCache::GetInstance().Cleanup();
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);
    if (g_CommandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(g_Device, g_CommandPool, g_Allocator);
    }
    vkDestroyDevice(g_Device, g_Allocator);
    // ⚠️ 2026-08-14：销毁后必须置 NULL——全局静态对象（g_SceneRenderer 等）在 main 返回后析构，
    // 其成员（PointShadow/CascadeShadow renderer）Cleanup 用 g_Device 守卫；悬空句柄（非 NULL 已销毁）
    // 会让守卫失效 → vulkan-1.dll 内 0xC0000005（静态析构期崩溃）。
    // Vulkan 规范：device 销毁后子对象句柄全部失效并隐式释放，后续跳过逐个 vkDestroy 是安全正确的。
    g_Device = VK_NULL_HANDLE;
    vkDestroyInstance(g_Instance, g_Allocator);
    g_Instance = VK_NULL_HANDLE;
}

// 清理Vulkan窗口
void CleanupVulkanWindow()
{
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
}

// 创建游戏模式合成 render pass：
//   attachment 0: swapchain 颜色（DONT_CARE 装载——全屏四边形覆盖，最终 PRESENT）
//   单 subpass: 全屏四边形经普通纹理采样（descriptor）读 GameRT G-Buffer 颜色0/深度 → 写 swapchain
//   （几何已由 GameRT render pass 单独渲染；本机驱动不支持 subpass input attachment，合成走独立 pass + barrier）
static void CreateCompositeRenderPass(ImGui_ImplVulkanH_Window* wd)
{
    VkAttachmentDescription swap = {};
    swap.format = wd->SurfaceFormat.format;
    swap.samples = VK_SAMPLE_COUNT_1_BIT;
    swap.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // 合成覆盖全屏
    swap.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    swap.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    swap.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    swap.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    swap.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency deps[1] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp = {};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments = &swap;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 1;
    rp.pDependencies = deps;
    check_vk_result(vkCreateRenderPass(g_Device, &rp, g_Allocator, &g_CompositeRenderPass));

    // ===== swapchain UI 叠加 pass（loadOp=LOAD）：链末输出后画 UI，保留链结果并 alpha 混合 =====
    // 游戏模式（RenderGameComposite）链（g_SwapChain）输出 swapchain 后，UI 叠加于此
    VkAttachmentDescription swapUI = swap;
    swapUI.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // 保留链末 tonemap 结果
    swapUI.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   // loadOp=LOAD 不能 UNDEFINED（链后布局 PRESENT_SRC，自动转换）
    swapUI.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkSubpassDependency uiDeps[1] = {};
    uiDeps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    uiDeps[0].dstSubpass = 0;
    uiDeps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    uiDeps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    uiDeps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    uiDeps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo uiRp = {};
    uiRp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    uiRp.attachmentCount = 1;
    uiRp.pAttachments = &swapUI;
    uiRp.subpassCount = 1;
    uiRp.pSubpasses = &subpass;
    uiRp.dependencyCount = 1;
    uiRp.pDependencies = uiDeps;
    check_vk_result(vkCreateRenderPass(g_Device, &uiRp, g_Allocator, &g_CompositeUIPass));
}

// 合成 render pass 的 framebuffer（per swapchain image）：只含 swapchain view（GameRT G-Buffer 经 descriptor 采样）
static void CreateCompositeFramebuffers(ImGui_ImplVulkanH_Window* wd)
{
    g_CompositeFramebuffers.resize(wd->ImageCount);
    for (uint32_t i = 0; i < wd->ImageCount; i++) {
        VkFramebufferCreateInfo fb = {};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = g_CompositeRenderPass;
        fb.attachmentCount = 1;
        fb.pAttachments = &wd->Frames[i].BackbufferView;
        fb.width = wd->Width;
        fb.height = wd->Height;
        fb.layers = 1;
        check_vk_result(vkCreateFramebuffer(g_Device, &fb, g_Allocator, &g_CompositeFramebuffers[i]));
    }
}

// 合并 render pass（游戏模式合成）资源：首次初始化与 swapchain 重建共用
// 依赖 g_GameRenderTarget 已初始化（附件格式/视图）与 wd（swapchain 格式/BackbufferView）
void InitCompositeResources()
{
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    CreateCompositeRenderPass(wd);
    CreateCompositeFramebuffers(wd);
#ifdef __ANDROID__
    // 2026-08-22 合并后处理链：游戏合成 quad 绑定到"分离合成通道"（单 subpass，普通纹理采样 G-Buffer）；
    // 几何与合成分成两个独立 render pass（Adreno 多 subpass + input attachment 的 vkCreateRenderPass 即崩）。
    // Android 无编辑器/SceneView，不初始化 g_SceneCompositeQuad/Scene 链/CMAA2；Swap 链（mobile 配置）与桌面共用 Execute 路径。
    // ⚠️ 独立合成 pass 无 G-Buffer input attachments——必须显式用 texture 采样版 fullscreen.frag.spv
    //    （默认宏 MIKAN_COMPOSITE_SHADER 是 fullscreen_subpass.frag.spv 的 subpassLoad 版，仅适用于合并 render pass 的合成 subpass；
    //     subpassLoad 在无 input attachment 的独立 pass 里读不到 G-Buffer → 黑屏，2026-08-22 定位）
    g_GameCompositeQuad.Init(g_GameRenderTarget.GetCompositeRenderPass(), 0, "fullscreen.frag.spv");
    {
        // 移动端后处理链（mobile 配置：gtao + bloom + tonemap；与桌面同用 PostProcessChain，final render pass = 合成输出 render pass）
        const std::string chainCfg = ProjectManager::GetInstance().GetEngineAssetPath("postprocess_chain_mobile.json");
        g_SwapChain.LoadFromJson(chainCfg);
        g_SwapChain.Build(wd->Width, wd->Height, g_CompositeRenderPass);
    }
    return;
#endif
    // 合成 subpass（各自几何 render pass 的 subpass 1）：input attachment 读 G-Buffer → 中间附件
    g_SceneCompositeQuad.Init(g_SceneRenderTarget.GetRenderPass(), 2);   // 合成 subpass（3 subpass 结构：0=zpre、1=几何、2=合成）
    g_GameCompositeQuad.Init(g_GameRenderTarget.GetRenderPass(), 2);
    // 配置驱动的后处理链（FMDS 式自由组合：跨 pass 引用 + 每槽采样器）：Scene/Game/swapchain 各一条（final render pass 不同）
    // engine/postprocess_chain.json = 引擎系统配置（职责分离：引擎资产在 engine/，游戏内容在 assets/）
    const std::string chainCfg = ProjectManager::GetInstance().GetEngineAssetPath("postprocess_chain.json");
    g_SceneChain.LoadFromJson(chainCfg);
    g_SceneChain.Build(g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight(), g_SceneRenderTarget.GetFinalRenderPass());
    g_GameChain.LoadFromJson(chainCfg);
    g_GameChain.Build(g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(), g_GameRenderTarget.GetFinalRenderPass());
    g_SwapChain.LoadFromJson(chainCfg);
    g_SwapChain.Build(wd->Width, wd->Height, g_CompositeRenderPass);
    // 2026-08-17：CMAA2 compute 改为"链内 tonemap pass 后"执行（pass 后 hook）——
    // 输入 = tonemap 输出（LDR gamma 空间，官方 CMAA2 语义），而非 HDR 线性 composite（未合成 AO、暗部对比度低）
    g_SceneChain.SetPassHook("tonemap", [](VkCommandBuffer cmd, VkImageView view, VkImage image) {
        if (!g_SceneChain.IsPassEnabled("cmaa_apply")) return;   // 2026-08-17：AA 切换到 FXAA/SMAA/关时 compute 不白跑
        VkSampler s = g_TexturePool->GetSamplerByType(SamplerType::LinearClamp);
        g_SceneCMAA2.Dispatch(cmd, view, s, image, g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight());
    });
    g_GameChain.SetPassHook("tonemap", [](VkCommandBuffer cmd, VkImageView view, VkImage image) {
        if (!g_GameChain.IsPassEnabled("cmaa_apply")) return;
        VkSampler s = g_TexturePool->GetSamplerByType(SamplerType::LinearClamp);
        g_GameCMAA2.Dispatch(cmd, view, s, image, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight());
    });
    g_SwapChain.SetPassHook("tonemap", [](VkCommandBuffer cmd, VkImageView view, VkImage image) {
        if (!g_SwapChain.IsPassEnabled("cmaa_apply")) return;
        VkSampler s = g_TexturePool->GetSamplerByType(SamplerType::LinearClamp);
        g_SwapCMAA2.Dispatch(cmd, view, s, image, (uint32_t)g_MainWindowData.Width, (uint32_t)g_MainWindowData.Height);
    });
    // 2026-08-16：CMAA2 compute（edges/process）——按 SceneView 分辨率初始化（cmaa_apply 在链内）
    if (!g_SceneCMAA2.Init(g_Device, g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight()) ||
        !g_GameCMAA2.Init(g_Device, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight()) ||
        !g_SwapCMAA2.Init(g_Device, g_MainWindowData.Width, g_MainWindowData.Height)) {
        fprintf(stderr, "[VulkanManager] CMAA2 Init failed\n");
    }
    // 天空 RT 占位（AtmosphereRenderer 初始化完成后由调用方更新为真实天空 RT）
    UpdateFullscreenQuadDescriptors();
}

// 更新合成描述符：G-Buffer 颜色0/深度 + 天空 RT（后处理链内部自行解析输入）
// 天空 RT 未初始化时传 null（占位；2D 场景深度判据恒 false 不会采样）
// 2026-08-12：SH 辐照度系数 UBO（binding 10——sky_hdr 的 CPU 投影 27 系数）
// 2026-08-13：点光源 UBO（binding 11）与 cluster grid SSBO（binding 12）——实现见文件下方
static VkBuffer UpdatePointLightBuffer();
static VkBuffer GetSceneClusterGridBuffer();
static VkBuffer GetGameClusterGridBuffer();
static VkBuffer GetShIrradianceBuffer() {
    // 2026-08-12：物理大气为主——atmo SH 投影 SSBO 优先（compute 每太阳变更新）；sky_hdr 静态系数作 fallback
    if (g_AtmosphereRenderer.IsInitialized()) {
        VkBuffer atmoSh = g_AtmosphereRenderer.GetSkyCubeSHBuffer();
        if (atmoSh) return atmoSh;
    }
    static VkBuffer buf = VK_NULL_HANDLE;
    static VkDeviceMemory mem = VK_NULL_HANDLE;
    if (buf != VK_NULL_HANDLE) return buf;
    float sh[27] = {0};
    if (g_TexturePool) g_TexturePool->ProjectSHIrradiance("sky_hdr", sh);
    VkBufferCreateInfo binfo = {};
    binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    binfo.size = 144;
    binfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &binfo, g_Allocator, &buf) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_Device, buf, &req);
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &props);
    uint32_t mt = VK_MAX_MEMORY_TYPES;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            mt = i; break;
        }
    }
    if (mt == VK_MAX_MEMORY_TYPES) return VK_NULL_HANDLE;
    VkMemoryAllocateInfo ainfo = {};
    ainfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ainfo.allocationSize = req.size;
    ainfo.memoryTypeIndex = mt;
    if (vkAllocateMemory(g_Device, &ainfo, g_Allocator, &mem) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkBindBufferMemory(g_Device, buf, mem, 0);
    void* data = nullptr;
    if (vkMapMemory(g_Device, mem, 0, 144, 0, &data) == VK_SUCCESS) {
        // shader 侧是 vec4 shCoefs[9]（std140 每元素 16B）——CPU 侧必须按组打包（RGB + 0 填充），
        // 直接 memcpy 连续 27 float 会错位（vec4[1].rgb 取到 c1_g/c1_b/c2_r——SH 显示不正常根因）
        float shPacked[36] = {0};
        for (int i = 0; i < 9; i++) {
            shPacked[i * 4 + 0] = sh[i * 3 + 0];
            shPacked[i * 4 + 1] = sh[i * 3 + 1];
            shPacked[i * 4 + 2] = sh[i * 3 + 2];
        }
        memcpy(data, shPacked, 144);
        vkUnmapMemory(g_Device, mem);
    }
    return buf;
}

void UpdateFullscreenQuadDescriptors()
{
    // 银河全景（end_sky.png——LogLuv32 编码——4096x2048；合成 pass sky 分支采样叠加；幂等加载）
    if (!g_TexturePool->GetTexture("end_sky")) {
        g_TexturePool->LoadTexture2D("end_sky",
            ProjectManager::GetInstance().GetEngineAssetPath("textures/end_sky.png"));
    }
    const TextureInfo* galaxy = g_TexturePool->GetTexture("end_sky");
    VkImageView galaxyView = galaxy ? galaxy->imageView : VK_NULL_HANDLE;
    // 2026-08-12：静态 HDR 天空盒（IBL——IDKEngine snow_field_puresky_1k.hdr——幂等加载；线性 SFLOAT + mip 预滤波）
    if (!g_TexturePool->GetTexture("sky_hdr")) {
        g_TexturePool->LoadHDRCubemap("sky_hdr", EngineConfig::GetEngineTexturePath("skybox") + "/EnvironmentMap/snow_field_puresky_1k.hdr");
    }
    const TextureInfo* skyHDR = g_TexturePool->GetTexture("sky_hdr");
    // 2026-08-12：IBL 切回物理大气 cubemap（用户拍板——环境光 = 大气散射 + 太阳光源）——atmo cube 优先，静态 sky_hdr 作 fallback
    VkImageView skyCubeView = g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyCubeView() : (skyHDR ? skyHDR->imageView : VK_NULL_HANDLE);
    VkSampler skyCubeSampler = g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyCubeSampler() : g_TexturePool->GetSamplerByType(SamplerType::Linear);
    // 2026-08-12：辐照度图（diffuse IBL——LoadHDRCubemap 自动生成）
    const TextureInfo* skyIrr = g_TexturePool->GetTexture("sky_hdr_irr");
    VkImageView skyIrrView = skyIrr ? skyIrr->imageView : (skyHDR ? skyHDR->imageView : VK_NULL_HANDLE);
    VkSampler skyIrrSampler = g_TexturePool->GetSamplerByType(SamplerType::Linear);
    // 合成 quad：G-Buffer 颜色0/深度 + skyRT（独立 descriptor set，勿与其他视口共用）
    // 2026-08-14：CSM 阴影（Scene 槽 0 / Game 槽 1）——初始化期 renderer 未就绪则 null（RenderSceneToTarget 每帧再刷新）
    CascadeShadowRenderer* csmSceneInit = g_SceneRenderer.EnsureCascadeShadows();
    CascadeShadowRenderer* csmGameInit = g_SceneRenderer.EnsureCascadeShadows();
#ifndef __ANDROID__
    // Android 无 SceneCompositeQuad（InitCompositeResources 的 __ANDROID__ 分支只初始化 Game），跳过 Scene quad 更新
    g_SceneCompositeQuad.UpdateDescriptorSet(g_SceneRenderTarget.GetColorImageView(), g_SceneRenderTarget.GetDepthImageView(), g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyImageView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkySampler() : VK_NULL_HANDLE, g_SceneRenderTarget.GetColorImageView(1), g_SceneRenderTarget.GetColorImageView(2), galaxyView, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetTransmittanceView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetScatteringView() : VK_NULL_HANDLE, skyCubeView, skyCubeSampler, skyIrrView, skyIrrSampler, GetShIrradianceBuffer(), UpdatePointLightBuffer(), GetSceneClusterGridBuffer(),
        (g_SceneRenderer.EnsurePointShadows() && g_SceneRenderer.EnsurePointShadows()->IsInitialized()) ? g_SceneRenderer.EnsurePointShadows()->GetCubeArrayView() : VK_NULL_HANDLE,
        (csmSceneInit && csmSceneInit->IsInitialized()) ? csmSceneInit->GetArrayView(0) : VK_NULL_HANDLE,
        g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare),   // 2026-08-15：阴影比较采样器（硬件 PCF，HSPE 同款）
        (csmSceneInit && csmSceneInit->IsInitialized()) ? csmSceneInit->GetCascadeBuffer(0, (int)g_MainWindowData.FrameIndex) : VK_NULL_HANDLE,
        g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutView() : VK_NULL_HANDLE,   // 2026-08-15：split-sum BRDF LUT
        g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutSampler() : VK_NULL_HANDLE);
#endif
    g_GameCompositeQuad.UpdateDescriptorSet(g_GameRenderTarget.GetColorImageView(), g_GameRenderTarget.GetDepthImageView(), g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyImageView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkySampler() : VK_NULL_HANDLE, g_GameRenderTarget.GetColorImageView(1), g_GameRenderTarget.GetColorImageView(2), galaxyView, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetTransmittanceView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetScatteringView() : VK_NULL_HANDLE, skyCubeView, skyCubeSampler, skyIrrView, skyIrrSampler, GetShIrradianceBuffer(), UpdatePointLightBuffer(), GetGameClusterGridBuffer(),
        (g_SceneRenderer.EnsurePointShadows() && g_SceneRenderer.EnsurePointShadows()->IsInitialized()) ? g_SceneRenderer.EnsurePointShadows()->GetCubeArrayView() : VK_NULL_HANDLE,
        (csmGameInit && csmGameInit->IsInitialized()) ? csmGameInit->GetArrayView(1) : VK_NULL_HANDLE,
        g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare),   // 2026-08-15：阴影比较采样器（硬件 PCF，HSPE 同款）
        (csmGameInit && csmGameInit->IsInitialized()) ? csmGameInit->GetCascadeBuffer(1, (int)g_MainWindowData.FrameIndex) : VK_NULL_HANDLE,
        g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutView() : VK_NULL_HANDLE,   // 2026-08-15：split-sum BRDF LUT
        g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutSampler() : VK_NULL_HANDLE);
}

void DestroyCompositeResources()
{
    for (VkFramebuffer fb : g_CompositeFramebuffers) {
        if (fb != VK_NULL_HANDLE)
            vkDestroyFramebuffer(g_Device, fb, g_Allocator);
    }
    g_CompositeFramebuffers.clear();
    if (g_CompositeRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(g_Device, g_CompositeRenderPass, g_Allocator);
        g_CompositeRenderPass = VK_NULL_HANDLE;
    }
    if (g_CompositeUIPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(g_Device, g_CompositeUIPass, g_Allocator);
        g_CompositeUIPass = VK_NULL_HANDLE;
    }
}

// 重建交换链
void RecreateSwapChain(int width, int height)
{
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    
    LOGI("RecreateSwapChain called: %dx%d", width, height);
    LOGI("Surface handle: %p", (void*)wd->Surface);
    
    // 检查 surface 是否有效
    if (wd->Surface == VK_NULL_HANDLE) {
        LOGE("ERROR: Surface is VK_NULL_HANDLE! Cannot recreate swapchain.");
        return;
    }
    
    // 等待设备空闲
    VkResult err = vkDeviceWaitIdle(g_Device);
    check_vk_result(err);
    
    // 清理旧的交换链和帧缓冲区
    LOGI("Cleaning up old swapchain and framebuffers...");
    for (uint32_t i = 0; i < wd->ImageCount; i++) {
        vkDestroyImageView(g_Device, wd->Frames[i].BackbufferView, g_Allocator);
        wd->Frames[i].BackbufferView = VK_NULL_HANDLE;
    }
    if (wd->Swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(g_Device, wd->Swapchain, g_Allocator);
        wd->Swapchain = VK_NULL_HANDLE;
    }
    if (wd->RenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(g_Device, wd->RenderPass, g_Allocator);
        wd->RenderPass = VK_NULL_HANDLE;
    }
    
    // 清理场景渲染器资源
    g_SceneRenderer.Cleanup();
    g_SkyboxRenderer.Cleanup();
    g_SceneRenderTarget.Cleanup();
    g_GameRenderTarget.Cleanup();
    g_FullscreenQuad.Cleanup();
    g_SceneCompositeQuad.Cleanup();
    g_SceneCMAA2.Cleanup();
    g_GameCMAA2.Cleanup();
    g_SwapCMAA2.Cleanup();   // 2026-08-16：CMAA2 compute 资源（三链独立实例）
    CleanupAOAndSSGIHistoryTextures();
    // 2026-08-17 TAA：历史纹理（Scene/Game 各一）
    if (g_SceneTAAHistory) {
        vkDestroyImageView(g_Device, g_SceneTAAHistoryView, g_Allocator);
        vkDestroyImage(g_Device, g_SceneTAAHistory, g_Allocator);
        vkFreeMemory(g_Device, g_SceneTAAHistoryMem, g_Allocator);
        vkDestroyImageView(g_Device, g_GameTAAHistoryView, g_Allocator);
        vkDestroyImage(g_Device, g_GameTAAHistory, g_Allocator);
        vkFreeMemory(g_Device, g_GameTAAHistoryMem, g_Allocator);
        g_SceneTAAHistory = g_GameTAAHistory = VK_NULL_HANDLE;
    }
    if (g_TAAHistorySampler) { vkDestroySampler(g_Device, g_TAAHistorySampler, g_Allocator); g_TAAHistorySampler = VK_NULL_HANDLE; }
    g_GameCompositeQuad.Cleanup();
    g_SceneChain.Cleanup();
    g_GameChain.Cleanup();
    g_SwapChain.Cleanup();
#ifndef __ANDROID__
    g_AtmosphereRenderer.Cleanup();
#else
    // Android：重建保留大气渲染器（skyRT 固定 128x64，不依赖窗口尺寸）——二次 Init 的 LUT Generate 在 Adreno 上
    // vkQueueSubmit 返回 DEVICE_LOST，且 DEVICE_LOST 会让后续所有提交失效 → 黑屏（2026-08-22 定位）
#endif
    DestroyCompositeResources();
    
    // 创建或调整窗口大小
    LOGI("Calling ImGui_ImplVulkanH_CreateOrResizeWindow with new surface...");
    ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, width, height, g_MinImageCount, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    LOGI("SwapChain recreated successfully");
    
    // 初始化离屏渲染目标 (使用窗口大小)
    g_SceneRenderTarget.Init(width, height, true); // 启用MRT
    g_GameRenderTarget.Init(width, height, true); // 启用MRT
    
    // 创建ImGui描述符集 (必须在ImGui_ImplVulkan_Init之后)
    g_SceneRenderTarget.CreateImGuiDescriptorSet();
    g_GameRenderTarget.CreateImGuiDescriptorSet();
    
    // 重新初始化场景渲染器 (使用离屏渲染目标的RenderPass)
    g_SceneRenderer.Init(g_SceneRenderTarget.GetRenderPass());
    g_SceneRenderer.EnsurePointShadows();   // 2026-08-13：阴影渲染器提前创建（合成 descriptor 首次绑定需要有效 cube view）
    g_SkyboxRenderer.Init(g_SceneRenderTarget.GetRenderPass());

    // Rebuild model GPU resources right away: Cleanup() above cleared m_ModelRenderers
    // and Init() does not reload models. Without this, the lazy load inside the render
    // frame can fail silently and models stay invisible until the next swapchain rebuild.
    g_SceneRenderer.PreloadModels();
    
    // 合并 render pass（游戏模式合成）：几何 subpass 0 + 全屏四边形 subpass 1（input attachment 读 G-Buffer）
    InitCompositeResources();
    // 物理天空随窗口重建（天空 RT 为 1/4 分辨率）
    // 2026-08-22：Android 重建时若大气渲染器已初始化则跳过重新 Init——
    // 二次 Init 的 LUT Generate 在 Adreno 上 vkQueueSubmit 返回 DEVICE_LOST（首次已成功生成，重建无需重算）
    if (!g_AtmosphereRenderer.IsInitialized()) {
        g_AtmosphereRenderer.Init(width, height);   // 2026-08-11：compute 版（AtmosphereLUT，内部生成 LUT）
    }
    // 更新合成描述符：绑定真实天空 RT
    UpdateFullscreenQuadDescriptors();
    // 2D 渲染核心（离屏世界层 + 主窗口 UI 层；重建时自动重建双管线）
    Renderer2D::GetInstance().Init(g_GameRenderTarget.GetRenderPass(), wd->RenderPass,
        g_GameRenderTarget.GetDisplayUIRenderPass(), g_CompositeUIPass);
    
    // 编辑器模式需要更新UI管理器中的描述符集
    if (g_RunMode == RunMode::Editor)
    {
        // Scene/Game view descriptors are forwarded to the editor at attach time (Editor.dll)
    }
}

// 设置垂直同步
void SetVSync(bool enabled)
{
    if (g_VSyncEnabled == enabled)
        return;
    
    g_VSyncEnabled = enabled;
    
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    VkPresentModeKHR new_present_mode;
    
    if (enabled) {
        // 启用垂直同步：使用 FIFO 模式（标准的垂直同步）
        new_present_mode = VK_PRESENT_MODE_FIFO_KHR;
    } else {
        // 禁用垂直同步：优先使用 MAILBOX 模式（无撕裂的低延迟），回退到 FIFO_RELAXED
        // 注意：Android 设备通常不支持 IMMEDIATE 模式
        #ifdef __ANDROID__
        new_present_mode = VK_PRESENT_MODE_MAILBOX_KHR;  // Android 首选
        #else
        new_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        #endif
    }
    
    wd->PresentMode = new_present_mode;
    
    // 根据三重缓冲设置更新最小图像数量
    if (g_TripleBufferingEnabled) {
        g_MinImageCount = 3;
    } else {
        g_MinImageCount = 2;
    }
    
    // 设置交换链重建标志，让主循环在适当的时候重建交换链
    g_SwapChainRebuild = true;
}

// 设置三重缓冲
void SetTripleBuffering(bool enabled)
{
    if (g_TripleBufferingEnabled == enabled)
        return;
    
    g_TripleBufferingEnabled = enabled;
    
    // 更新最小图像数量
    if (enabled) {
        g_MinImageCount = 3;
    } else {
        g_MinImageCount = 2;
    }
    
    // 设置交换链重建标志，让主循环在适当的时候重建交换链
    g_SwapChainRebuild = true;
}

// ===== 全屏模式（2026-08-17）：0=窗口 1=桌面全屏（无边框） 2=独占全屏 =====
// 独占全屏绕开 DWM 合成（窗口模式 present 平台税 0.6-0.9ms → ~0.05ms）
// SDL3：SetWindowFullscreenMode(mode) —— NULL=桌面无边框；非 NULL=独占；SetWindowFullscreen(bool) 应用
extern SDL_Window* window;   // EngineGlobals.cpp 全局窗口
int g_FullscreenMode = 0;    // 当前模式（控制面板读取/设置）

void SetFullscreenMode(int mode)
{
    if (mode < 0 || mode > 2) return;
    if (g_FullscreenMode == mode) return;
    const int prevMode = g_FullscreenMode;
    g_FullscreenMode = mode;
    if (!window) return;

    if (mode == 0) {
        SDL_SetWindowFullscreen(window, false);   // 退出全屏（恢复窗口）
    } else if (mode == 1) {
        // 桌面全屏（无边框，borderless fullscreen desktop）——不绕 DWM，但尺寸铺满
        SDL_SetWindowFullscreenMode(window, NULL);
        SDL_SetWindowFullscreen(window, true);
    } else {
        // 独占全屏：取与当前桌面分辨率匹配的全屏 mode
        SDL_DisplayID disp = SDL_GetDisplayForWindow(window);
        const SDL_DisplayMode* desktop = SDL_GetCurrentDisplayMode(disp);
        const SDL_DisplayMode* pick = NULL;
        int count = 0;
        SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(disp, &count);
        if (desktop && modes) {
            for (int i = 0; i < count; ++i) {
                if (modes[i] && modes[i]->w == desktop->w && modes[i]->h == desktop->h) { pick = modes[i]; break; }
            }
            if (!pick && count > 0) pick = modes[0];
        }
        // 2026-08-17：picker 失败绝不静默降级无边框（SetWindowFullscreenMode(NULL)=无边框）——独占要求明确
        if (!pick) {
            printf("[Fullscreen] WARN: 未获取到独占全屏 mode（count=%d），回滚到模式 %d\n", count, prevMode);
            fflush(stdout);
            g_FullscreenMode = prevMode;
            return;
        }
        SDL_SetWindowFullscreenMode(window, pick);
        SDL_SetWindowFullscreen(window, true);
        // 验证独占生效：窗口 fullscreen mode 应非 NULL（NULL=无边框）
        const SDL_DisplayMode* applied = SDL_GetWindowFullscreenMode(window);
        printf("[Fullscreen] exclusive applied: %dx%d@%dHz (mode=%p)\n",
            applied ? applied->w : 0, applied ? applied->h : 0,
            applied ? applied->refresh_rate : 0, (void*)applied);
        fflush(stdout);
    }
    // 不主动重建 swapchain：全屏切换后系统发 SDL_EVENT_WINDOW_RESIZED → 现有 resize 路径重建
}

// 渲染场景到离屏目标（统一管线：物理天空 + 合成 subpass → 显示附件，SceneView 面板采样）

// 合成 → final barrier：composite 附件从合成 subpass 的写入布局显式转换到采样布局，
// 合成 → final barrier：composite 保持 COLOR_ATTACHMENT_OPTIMAL（合成 subpass 写入布局），
// 链对 composite 也以 COLOR_ATTACHMENT_OPTIMAL 采样（布局全程一致，无需转换）——此 barrier 仅做 access 同步
static void CompositeToFinalBarrier(VkCommandBuffer commandBuffer, VkImage compositeImage)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   // 与 finalLayout 一致（无布局转换）
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = compositeImage;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// UI 叠加 helper（链末 tonemap 后，UI alpha 混合叠加在结果之上）——前向声明（RenderSceneToTarget 等先于定义使用）
static void RenderUIOverlay(VkCommandBuffer, uint32_t, uint32_t, VkRenderPass, VkFramebuffer, bool,
                            const glm::mat4* gridView = nullptr, const glm::mat4* gridProj = nullptr,
                            const glm::vec3* gridCamPos = nullptr);
// 场景方向光收集
static bool GetSceneDirectionalLight(glm::vec3& dir, glm::vec3& color, float& intensity);
// 填充 CSM 级联数据到 cameraUBO（gtao 半分辨率体积光采样阴影）——通用（场景/游戏视口各自 slot）
static void FillCsmIntoUExt(PostProcessChain::ExternalInputs& ext, CascadeShadowRenderer* csm, int slot, VkSampler shadowSampler)
{
    for (int c = 0; c < CascadeShadowRenderer::MAX_CASCADES; c++) {
        ext.cameraUBO.csmMatrices[c] = (csm && csm->IsInitialized()) ? csm->GetShadowMatrix(slot, c) : glm::mat4(0.0f);
        ext.cameraUBO.csmSplitFars[c] = (csm && csm->IsInitialized()) ? csm->GetSplitFar(slot, c) : -1.0f;
    }
    ext.cameraUBO.csmParams.x = (csm && csm->IsInitialized()) ? float(CascadeShadowRenderer::MAX_CASCADES) : 0.0f;
    ext.cameraUBO.csmParams.y = (csm && csm->IsInitialized()) ? 1.0f : 0.0f;
    ext.csmShadowView = (csm && csm->IsInitialized()) ? csm->GetArrayView(slot) : VK_NULL_HANDLE;
    ext.csmShadowSampler = shadowSampler;
}

// ===== 时序 GTAO 历史纹理：Scene/Game 各自尺寸的半分辨率 RGBA8 =====
static VkImage g_SceneAOHistory = VK_NULL_HANDLE;
static VkDeviceMemory g_SceneAOHistoryMem = VK_NULL_HANDLE;
static VkImageView g_SceneAOHistoryView = VK_NULL_HANDLE;
static VkImage g_GameAOHistory = VK_NULL_HANDLE;
static VkDeviceMemory g_GameAOHistoryMem = VK_NULL_HANDLE;
static VkImageView g_GameAOHistoryView = VK_NULL_HANDLE;
static VkSampler g_AOHistorySampler = VK_NULL_HANDLE;
static uint32_t g_SceneAOHistoryW = 0, g_SceneAOHistoryH = 0;
static uint32_t g_GameAOHistoryW = 0, g_GameAOHistoryH = 0;
static bool g_SceneAOHistoryNeedsClear = true;
static bool g_GameAOHistoryNeedsClear = true;
// SSGI 时间 reblur 历史（RGBA16F 半分辨率，跨帧累积——参考 gtao history 机制）
static VkImage g_SceneSSGIHistory = VK_NULL_HANDLE;
static VkDeviceMemory g_SceneSSGIHistoryMem = VK_NULL_HANDLE;
static VkImageView g_SceneSSGIHistoryView = VK_NULL_HANDLE;
static VkImage g_GameSSGIHistory = VK_NULL_HANDLE;
static VkDeviceMemory g_GameSSGIHistoryMem = VK_NULL_HANDLE;
static VkImageView g_GameSSGIHistoryView = VK_NULL_HANDLE;
static VkSampler g_SSGIHistorySampler = VK_NULL_HANDLE;
static uint32_t g_SceneSSGIHistoryW = 0, g_SceneSSGIHistoryH = 0;
static uint32_t g_GameSSGIHistoryW = 0, g_GameSSGIHistoryH = 0;
static bool g_SceneSSGIHistoryNeedsClear = true;
static bool g_GameSSGIHistoryNeedsClear = true;

static void DestroyHistoryImage(VkImage& image, VkDeviceMemory& memory, VkImageView& view)
{
    if (view != VK_NULL_HANDLE) vkDestroyImageView(g_Device, view, g_Allocator);
    if (image != VK_NULL_HANDLE) vkDestroyImage(g_Device, image, g_Allocator);
    if (memory != VK_NULL_HANDLE) vkFreeMemory(g_Device, memory, g_Allocator);
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    view = VK_NULL_HANDLE;
}

static void CleanupAOAndSSGIHistoryTextures()
{
    if (g_Device == VK_NULL_HANDLE) return;
    DestroyHistoryImage(g_SceneAOHistory, g_SceneAOHistoryMem, g_SceneAOHistoryView);
    DestroyHistoryImage(g_GameAOHistory, g_GameAOHistoryMem, g_GameAOHistoryView);
    DestroyHistoryImage(g_SceneSSGIHistory, g_SceneSSGIHistoryMem, g_SceneSSGIHistoryView);
    DestroyHistoryImage(g_GameSSGIHistory, g_GameSSGIHistoryMem, g_GameSSGIHistoryView);
    if (g_AOHistorySampler != VK_NULL_HANDLE) vkDestroySampler(g_Device, g_AOHistorySampler, g_Allocator);
    if (g_SSGIHistorySampler != VK_NULL_HANDLE) vkDestroySampler(g_Device, g_SSGIHistorySampler, g_Allocator);
    g_AOHistorySampler = VK_NULL_HANDLE;
    g_SSGIHistorySampler = VK_NULL_HANDLE;
    g_SceneAOHistoryW = g_SceneAOHistoryH = 0;
    g_GameAOHistoryW = g_GameAOHistoryH = 0;
    g_SceneSSGIHistoryW = g_SceneSSGIHistoryH = 0;
    g_GameSSGIHistoryW = g_GameSSGIHistoryH = 0;
    g_SceneAOHistoryNeedsClear = g_GameAOHistoryNeedsClear = true;
    g_SceneSSGIHistoryNeedsClear = g_GameSSGIHistoryNeedsClear = true;
}

static void EnsureAOHistoryTexture(bool sceneHistory, uint32_t w, uint32_t h)
{
    VkImage& image = sceneHistory ? g_SceneAOHistory : g_GameAOHistory;
    VkDeviceMemory& memory = sceneHistory ? g_SceneAOHistoryMem : g_GameAOHistoryMem;
    VkImageView& view = sceneHistory ? g_SceneAOHistoryView : g_GameAOHistoryView;
    uint32_t& currentW = sceneHistory ? g_SceneAOHistoryW : g_GameAOHistoryW;
    uint32_t& currentH = sceneHistory ? g_SceneAOHistoryH : g_GameAOHistoryH;
    bool& needsClear = sceneHistory ? g_SceneAOHistoryNeedsClear : g_GameAOHistoryNeedsClear;
    if (image != VK_NULL_HANDLE && currentW == w && currentH == h) return;
    DestroyHistoryImage(image, memory, view);
    currentW = w;
    currentH = h;
    VkImageCreateInfo ii = {};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;   // 2026-：随 gtao 链输出改 RGBA8（R=AO G=体积光 B=Godray），历史拷贝格式匹配
    ii.extent = { w, h, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(g_Device, &ii, g_Allocator, &image);
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_Device, image, &mr);
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = 0;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mp);
    for (uint32_t m = 0; m < mp.memoryTypeCount; m++) {
        if ((mr.memoryTypeBits & (1u << m)) && (mp.memoryTypes[m].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { ai.memoryTypeIndex = m; break; }
    }
    vkAllocateMemory(g_Device, &ai, g_Allocator, &memory);
    vkBindImageMemory(g_Device, image, memory, 0);
    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(g_Device, &vi, g_Allocator, &view);
    needsClear = true;
    if (g_AOHistorySampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(g_Device, &si, g_Allocator, &g_AOHistorySampler);
    }
}

// 帧首：新历史先从 UNDEFINED 清为 AO=1/体积光=0；已有历史从上帧 TRANSFER_DST 转为采样。
static void PrepareAOHistoryForRead(VkCommandBuffer cmd, VkImage history, bool& needsClear)
{
    if (!history) return;
    if (needsClear) {
        VkImageMemoryBarrier toTransfer = {};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = history;
        toTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toTransfer);
        const VkClearColorValue clear = { { 1.0f, 0.0f, 0.0f, 0.0f } };
        vkCmdClearColorImage(cmd, history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear,
            1, &toTransfer.subresourceRange);
        needsClear = false;
    }
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = history;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// 链后：gtao pass 输出 → 历史（下帧累积用）
static void CopyAOHistory(VkCommandBuffer cmd, VkImage gtaoImg, VkImage history, uint32_t width, uint32_t height)
{
    static int s_gtaoDiag = 0;
    if (s_gtaoDiag < 3) {
        s_gtaoDiag++;
        LOGI("[GTAO-DIAG] CopyAOHistory gtaoImg=%p history=%p (w=%u h=%u)",
            (void*)gtaoImg, (void*)history, width, height);
    }
    if (!gtaoImg || !history) {
        printf("[AOHistory] SKIP gtaoImg=%p history=%p\n", (void*)gtaoImg, (void*)history);
        return;
    }
    static int dbgCount = 0;
    if ((++dbgCount % 120) == 1) printf("[AOHistory] copy %ux%u\n", width, height);
    VkImageMemoryBarrier bs[2] = {};
    bs[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bs[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bs[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bs[0].srcQueueFamilyIndex = bs[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bs[0].image = gtaoImg;
    bs[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bs[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bs[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bs[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bs[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bs[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bs[1].srcQueueFamilyIndex = bs[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bs[1].image = history;
    bs[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bs[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bs[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, bs);
    VkImageCopy region = {};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { width, height, 1 };
    vkCmdCopyImage(cmd, gtaoImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // 历史图保持 TRANSFER_DST，下一帧 PrepareAOHistoryForRead 会从该布局转为采样布局；
    // GTAO pass 输出则必须恢复为 SHADER_READ_ONLY，否则下一帧链会以错误的旧布局开始。
    VkImageMemoryBarrier tail = {};
    tail.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    tail.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    tail.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    tail.srcQueueFamilyIndex = tail.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    tail.image = gtaoImg;
    tail.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    tail.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    tail.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &tail);
}

// ===== SSGI 时间 reblur 历史（2026：RGBA16F 半分辨率，参考 gtao history）=====
static void EnsureSSGIHistoryTexture(bool sceneHistory, uint32_t w, uint32_t h)
{
    VkImage& image = sceneHistory ? g_SceneSSGIHistory : g_GameSSGIHistory;
    VkDeviceMemory& memory = sceneHistory ? g_SceneSSGIHistoryMem : g_GameSSGIHistoryMem;
    VkImageView& view = sceneHistory ? g_SceneSSGIHistoryView : g_GameSSGIHistoryView;
    uint32_t& currentW = sceneHistory ? g_SceneSSGIHistoryW : g_GameSSGIHistoryW;
    uint32_t& currentH = sceneHistory ? g_SceneSSGIHistoryH : g_GameSSGIHistoryH;
    bool& needsClear = sceneHistory ? g_SceneSSGIHistoryNeedsClear : g_GameSSGIHistoryNeedsClear;
    if (image != VK_NULL_HANDLE && currentW == w && currentH == h) return;
    DestroyHistoryImage(image, memory, view);
    currentW = w;
    currentH = h;
    VkImageCreateInfo ii = {};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;   // SSGI 历史：间接光 HDR 线性，保精度防溢色
    ii.extent = { w, h, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(g_Device, &ii, g_Allocator, &image);
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_Device, image, &mr);
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = 0;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mp);
    for (uint32_t m = 0; m < mp.memoryTypeCount; m++) {
        if ((mr.memoryTypeBits & (1u << m)) && (mp.memoryTypes[m].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { ai.memoryTypeIndex = m; break; }
    }
    vkAllocateMemory(g_Device, &ai, g_Allocator, &memory);
    vkBindImageMemory(g_Device, image, memory, 0);
    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(g_Device, &vi, g_Allocator, &view);
    needsClear = true;
    if (g_SSGIHistorySampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(g_Device, &si, g_Allocator, &g_SSGIHistorySampler);
    }
}

static void PrepareSSGIHistoryForRead(VkCommandBuffer cmd, VkImage history, bool& needsClear)
{
    if (!history) return;
    if (needsClear) {
        VkImageMemoryBarrier toTransfer = {};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = history;
        toTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toTransfer);
        const VkClearColorValue clear = {};
        vkCmdClearColorImage(cmd, history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear,
            1, &toTransfer.subresourceRange);
        needsClear = false;
    }
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = history;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// 链后：ssgi pass 输出 → 历史（下帧累积用）
static void CopySSGIHistory(VkCommandBuffer cmd, VkImage ssgiImg, VkImage history, uint32_t width, uint32_t height)
{
    if (!ssgiImg || !history) return;
    VkImageMemoryBarrier bs[2] = {};
    bs[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bs[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bs[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bs[0].srcQueueFamilyIndex = bs[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bs[0].image = ssgiImg;
    bs[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bs[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bs[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bs[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bs[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bs[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bs[1].srcQueueFamilyIndex = bs[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bs[1].image = history;
    bs[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bs[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bs[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, bs);
    VkImageCopy region = {};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { width, height, 1 };
    vkCmdCopyImage(cmd, ssgiImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

// ===== TAA 历史纹理管理函数（2026-08-17；变量声明在文件前部 AOHistory 旁）=====
static void CopyTAAHistory(VkCommandBuffer cmd, VkImage taaImg, VkImage history);   // 前向声明（PrepareTAAHistoryForRead 先使用）
static void EnsureTAAHistoryTexture(uint32_t w, uint32_t h)
{
    if (g_SceneTAAHistory != VK_NULL_HANDLE && g_TAAHistoryW == w && g_TAAHistoryH == h) return;
    if (g_SceneTAAHistory != VK_NULL_HANDLE) {
        vkDestroyImageView(g_Device, g_SceneTAAHistoryView, g_Allocator);
        vkDestroyImage(g_Device, g_SceneTAAHistory, g_Allocator);
        vkFreeMemory(g_Device, g_SceneTAAHistoryMem, g_Allocator);
        vkDestroyImageView(g_Device, g_GameTAAHistoryView, g_Allocator);
        vkDestroyImage(g_Device, g_GameTAAHistory, g_Allocator);
        vkFreeMemory(g_Device, g_GameTAAHistoryMem, g_Allocator);
        g_SceneTAAHistory = g_GameTAAHistory = VK_NULL_HANDLE;
    }
    g_TAAHistoryW = w; g_TAAHistoryH = h;
    g_TAAHistoryNeedsClear = true;
    VkImageCreateInfo ii = {};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent = { w, h, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    for (int i = 0; i < 2; i++) {
        VkImage* img = i == 0 ? &g_SceneTAAHistory : &g_GameTAAHistory;
        VkDeviceMemory* mem = i == 0 ? &g_SceneTAAHistoryMem : &g_GameTAAHistoryMem;
        VkImageView* view = i == 0 ? &g_SceneTAAHistoryView : &g_GameTAAHistoryView;
        vkCreateImage(g_Device, &ii, g_Allocator, img);
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(g_Device, *img, &mr);
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = 0;
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mp);
        for (uint32_t m = 0; m < mp.memoryTypeCount; m++) {
            if ((mr.memoryTypeBits & (1u << m)) && (mp.memoryTypes[m].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { ai.memoryTypeIndex = m; break; }
        }
        vkAllocateMemory(g_Device, &ai, g_Allocator, mem);
        vkBindImageMemory(g_Device, *img, *mem, 0);
        VkImageViewCreateInfo vi = {};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = *img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(g_Device, &vi, g_Allocator, view);
    }
    if (g_TAAHistorySampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(g_Device, &si, g_Allocator, &g_TAAHistorySampler);
    }
}

// 帧首：上帧 taa 输出 → 历史（同 command buffer 串行——3 帧 in-flight 下帧 N 的 barrier
// 会等待帧 N-1 的写入完成（同 queue 提交有序），消除帧末 copy 的跨帧竞态）；创建后首帧额外 clear
static void PrepareTAAHistoryForRead(VkCommandBuffer cmd, VkImage history, VkImage prevTaaOutput)
{
    if (!history) return;
    if (g_TAAHistoryNeedsClear) {
        // 首帧：UNDEFINED → clear → SHADER_READ_ONLY（无上帧输出）
        VkImageMemoryBarrier b = {};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        // vkCmdClearColorImage 要求目标已经处于 TRANSFER_DST_OPTIMAL；
        // 这里若直接标成 SHADER_READ_ONLY，会让清除操作与实际 layout 不一致，
        // 首帧历史内容在移动 GPU 上可能变成未定义数据。
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = history;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkClearColorValue cc = {};
        VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cmd, history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rng);
        g_TAAHistoryNeedsClear = false;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    } else {
        // 常规：上帧 taa 输出 → 历史（CopyTAAHistory 自带 barrier + 尾部转回 SHADER_READ_ONLY）
        CopyTAAHistory(cmd, prevTaaOutput, history);
    }
}

// 链后：taa pass 输出 → 历史（下帧累积用）
static void CopyTAAHistory(VkCommandBuffer cmd, VkImage taaImg, VkImage history)
{
    // [TAA-DIAG] 首 3 次打印链路状态；使用 LOGI 以便 Android logcat 能看到句柄是否有效。
    static int s_taaDiag = 0;
    if (s_taaDiag < 3) {
        s_taaDiag++;
        LOGI("[TAA-DIAG] CopyTAAHistory taaImg=%p history=%p (w=%u h=%u needsClear=%d)",
            (void*)taaImg, (void*)history, g_TAAHistoryW, g_TAAHistoryH, g_TAAHistoryNeedsClear ? 1 : 0);
    }
    if (!taaImg || !history) return;
    VkImageMemoryBarrier bs[2] = {};
    bs[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bs[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bs[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bs[0].srcQueueFamilyIndex = bs[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bs[0].image = taaImg;
    bs[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bs[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bs[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bs[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bs[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bs[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bs[1].srcQueueFamilyIndex = bs[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bs[1].image = history;
    bs[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bs[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bs[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, bs);
    VkImageCopy region = {};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { g_TAAHistoryW, g_TAAHistoryH, 1 };
    vkCmdCopyImage(cmd, taaImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    // 两张图都恢复为 SHADER_READ_ONLY：history 供下一帧 TAA 采样，taaImg
    // 作为链中的中间附件在下一帧仍会被同一个 pass 复用，不能残留 TRANSFER_SRC 布局。
    VkImageMemoryBarrier tail[2] = {};
    tail[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    tail[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    tail[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    tail[0].srcQueueFamilyIndex = tail[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    tail[0].image = taaImg;
    tail[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    tail[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    tail[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    tail[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    tail[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    tail[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    tail[1].srcQueueFamilyIndex = tail[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    tail[1].image = history;
    tail[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    tail[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    tail[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 2, tail);
}

// ===== TAA 相机抖动（2026-08-17 v2）：Halton(2,3) 亚像素序列，NDC 偏移 =====
// v2：投影矩阵保持无 jitter（projView/prevProjView/invViewProj 全部几何真实——motion 纯运动量），
// jitter 由 model.vert 在顶点内 `clipPos.xy += taaJitter·w` 实现（IDKEngine 语义，深度不变）；
// 每视图独立计数器（⚠️ 共享计数器会让 GUI 每帧 +2/+3 → Halton 跳步失真 → 抖动）
static float TAAHalton(int index, int base)
{
    float f = 1.0f, r = 0.0f;
    while (index > 0) { f /= (float)base; r += f * (float)(index % base); index /= base; }
    return r;
}
static glm::vec2 ComputeTAAJitter(uint32_t& frameCounter, float w, float h)
{
    uint32_t idx = frameCounter++;
    float jx = (TAAHalton((int)idx, 2) - 0.5f) * 2.0f / w;
    float jy = (TAAHalton((int)idx, 3) - 0.5f) * 2.0f / h;
    return glm::vec2(jx, jy);
}
// 当前渲染视图的 TAA jitter（NDC 偏移；ModelRenderer 填充 push constant 用；TAA 禁用时 = 0）
glm::vec2 g_CurrentTAAJitter = glm::vec2(0.0f);
// 每视图独立计数器：Scene（编辑器 SceneView）/ GameView（编辑器 GameView 面板）/ Game（游戏模式主输出）各一条 Halton 序列
// ⚠️ 编辑器模式三路径同帧执行——必须各持计数器，否则序列跳步失真
static uint32_t g_TAAJitterFrameScene = 0;
static uint32_t g_TAAJitterFrameGameView = 0;
static uint32_t g_TAAJitterFrameGame = 0;
// TAA fragment pass 需要上一帧与当前帧 jitter 的差值，把排除了 jitter 的运动矢量
// 重新对齐到两帧实际写入的屏幕位置。Android 游戏路径每帧只渲染一次，单独保存即可。
static glm::vec2 g_PreviousTAAJitterGame = glm::vec2(0.0f);
// 上一帧的 view*proj（GTAO 相机重投影 UBO）

static void RenderSceneToTarget(const glm::mat4& view, const glm::mat4& proj, uint32_t frameIndex)   // 2026-08-17：加 frameIndex（GPU 时间戳）
{
    // 诊断（临时）：首帧确认编辑器 SceneView 渲染执行
    static bool s_loggedScene = false;
    if (!s_loggedScene) {
        printf("[VulkanManager] RenderSceneToTarget executed (g_ShowSceneView=%d g_SceneIs2D=%d)\n", (int)g_ShowSceneView, (int)g_SceneIs2D);
        s_loggedScene = true;
    }

    // 使用主命令缓冲区进行场景渲染
    VkCommandBuffer commandBuffer = g_MainWindowData.Frames[g_MainWindowData.FrameIndex].CommandBuffer;
    // 2026-08-17 TAA：相机亚像素抖动（Halton 2,3）——几何/合成/pushData 全用 jittered 投影；UI 叠加保持原 proj
    g_CurrentTAAJitter = g_SceneChain.IsPassEnabled("taa") ? ComputeTAAJitter(g_TAAJitterFrameScene, (float)g_SceneRenderTarget.GetWidth(), (float)g_SceneRenderTarget.GetHeight()) : glm::vec2(0.0f);
    (void)proj;   // 后续渲染调用换 proj

    
    // 物理天空全景图已由 FrameRender 统一渲染（相机无关）——此处只判断 usePhysicalSky
    g_SkyboxRenderer.SyncFromScene();
    bool usePhysicalSky = false;
    if (g_SkyboxRenderer.IsEnabled() && g_AtmosphereEnabled && g_AtmosphereRenderer.IsInitialized()) {
        usePhysicalSky = true;
    }
    
    // 场景方向光收集（函数级：CSM 阴影 + 合成共用；无光源回退太阳方向/白光/1 强度，2026-08-10）
    glm::vec3 lightDir, lightColor(1.0f, 0.96f, 0.89f); float lightIntensity = 1.0f;   // 2026-08-12：回退色改近似太阳（~5778K 暖白）；场景有方向光时被覆盖
    glm::vec3 sunDir = g_AtmosphereRenderer.GetSunDirection();
    if (GetSceneDirectionalLight(lightDir, lightColor, lightIntensity)) sunDir = lightDir;

    // 合成描述符：SceneRT 颜色0/深度 + 天空 RT（独立 descriptor set，勿与 GameView 共用）
    if (!g_TexturePool->GetTexture("sky_hdr")) g_TexturePool->LoadHDRCubemap("sky_hdr", EngineConfig::GetEngineTexturePath("skybox") + "/EnvironmentMap/snow_field_puresky_1k.hdr");
    const TextureInfo* skyHDR2 = g_TexturePool->GetTexture("sky_hdr");
    const TextureInfo* skyIrr2 = g_TexturePool->GetTexture("sky_hdr_irr");
    // 2026-08-12：IBL 物理大气优先（与 UpdateFullscreenQuadDescriptors 一致）——sky_hdr 静态作 fallback
    VkImageView sceneSkyCube = g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyCubeView() : (skyHDR2 ? skyHDR2->imageView : VK_NULL_HANDLE);
    VkSampler sceneSkyCubeSamp = g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyCubeSampler() : g_TexturePool->GetSamplerByType(SamplerType::Linear);
    // 2026-08-14：CSM 方向光阴影（slot 0 = SceneView）——合成 descriptor 绑定前确保渲染器就绪
    CascadeShadowRenderer* csmScene = g_SceneRenderer.EnsureCascadeShadows();
    g_SceneCompositeQuad.UpdateDescriptorSet(g_SceneRenderTarget.GetColorImageView(), g_SceneRenderTarget.GetDepthImageView(), g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyImageView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkySampler() : VK_NULL_HANDLE, g_SceneRenderTarget.GetColorImageView(1), g_SceneRenderTarget.GetColorImageView(2), (g_TexturePool->GetTexture("end_sky")) ? g_TexturePool->GetTexture("end_sky")->imageView : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetTransmittanceView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetScatteringView() : VK_NULL_HANDLE, sceneSkyCube, sceneSkyCubeSamp, skyIrr2 ? skyIrr2->imageView : (skyHDR2 ? skyHDR2->imageView : VK_NULL_HANDLE), g_TexturePool->GetSamplerByType(SamplerType::Linear), GetShIrradianceBuffer(), UpdatePointLightBuffer(), GetSceneClusterGridBuffer(),
        (g_SceneRenderer.EnsurePointShadows() && g_SceneRenderer.EnsurePointShadows()->IsInitialized()) ? g_SceneRenderer.EnsurePointShadows()->GetCubeArrayView() : VK_NULL_HANDLE,
        (csmScene && csmScene->IsInitialized()) ? csmScene->GetArrayView(0) : VK_NULL_HANDLE,
        g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare),   // 2026-08-15：阴影比较采样器（硬件 PCF，HSPE 同款）
        (csmScene && csmScene->IsInitialized()) ? csmScene->GetCascadeBuffer(0, (int)g_MainWindowData.FrameIndex) : VK_NULL_HANDLE,
        g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutView() : VK_NULL_HANDLE,   // 2026-08-15：split-sum BRDF LUT
        g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutSampler() : VK_NULL_HANDLE);

    // 2026-08-14：CSM 方向光阴影（每视口相机各一套——SceneView 槽 0）——主 render pass 前渲染 + barrier
    csmScene->SetFrameIndex((int)g_MainWindowData.FrameIndex);   // per-frame UBO 双缓冲（帧竞争修复）
    g_SceneRenderer.RenderCascadeShadowMaps(commandBuffer, 0, view, proj, sunDir);
    
    // 场景渲染阶段(2D 游戏:渲染 2D 画布内容供编辑器查看——与游戏视图同世界层/相机)
    g_SceneRenderTarget.BeginRender(commandBuffer);
    // z-prepass（subpass 0，depth-only）：提前写 3D 深度，MRT 几何阶段被遮挡片元在 fragment shader 前剔除
    // 2026-08-09：场景视图传 useMainCameraFrustum=true（z-prepass 与几何一致用主相机视锥剔除，防灰色清屏）
    // z-prepass（subpass 0，depth-only）——g_EnableZPrepass 开关（2026-08-10 GPU 对比验证）
    if (g_EnableZPrepass) {
        g_SceneRenderer.RenderDepthPrepass(commandBuffer, g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight(), view, proj, true);
    }
    g_SceneRenderTarget.NextSubpass(commandBuffer);   // → subpass 1（几何/2D/UI）
    // 统一 3D 管线（2D/3D 场景共用，取消 g_SceneIs2D 分支）：
    // 3D 场景 + 2D 世界层（玩法层/精灵）→ G-Buffer（subpass 1）→ 合成 → 链 → UI 链后叠加
    if (g_SkyboxRenderer.IsEnabled() && !usePhysicalSky) {
        g_SkyboxRenderer.Render(commandBuffer, view, proj);   // 物理天空时天空由合成 subpass 还原
    }
    // 使用 ECS 渲染系统渲染模型（场景视图，启用可视化；2D 场景无 3D 实体 → 空提交）
    g_SceneRenderer.RenderSceneView(commandBuffer, g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight(), view, proj);
    // 2D 玩法层（普通精灵/Canvas 世界层实体）与 UI：已移到链后 RenderUIOverlay（后处理之外，玩法层在 UI 之前）
    // 合成 subpass（subpass 2）：几何/2D/UI 写 G-Buffer 后切换 subpass，
    // 全屏四边形经 input attachment 从 tile 内读颜色0/深度/法线合成 → 中间附件（不写回主存）
    g_SceneRenderTarget.NextSubpass(commandBuffer);
    // ⚠️ 2026-08-14 一次性诊断：确认 proj/invViewProj 深度约定（直传 vs *2-1 之谜）
    {
        static bool s_pcLogged = false;
        if (!s_pcLogged) {
            s_pcLogged = true;
            glm::mat4 ivp = glm::inverse(proj * view);
            LOGI("[PC] proj: p00=%.4f p11=%.4f p22=%.4f p23=%.4f p32=%.4f p33=%.4f",
                 proj[0][0], proj[1][1], proj[2][2], proj[2][3], proj[3][2], proj[3][3]);
            LOGI("[PC] invViewProj: m00=%.4f m11=%.4f m22=%.4f m23=%.4f m32=%.4f m33=%.4f",
                 ivp[0][0], ivp[1][1], ivp[2][2], ivp[2][3], ivp[3][2], ivp[3][3]);
        }
    }
    g_SceneCompositeQuad.Render(commandBuffer, g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight(),
        glm::inverse(proj * view), glm::vec3(glm::inverse(view)[3]), sunDir, proj, view);
    g_SceneRenderTarget.EndRender(commandBuffer);
    // 后处理链（配置驱动）：SceneRT composite → 链逐 pass → 显示附件
    CompositeToFinalBarrier(commandBuffer, g_SceneRenderTarget.GetCompositeImage());
    const uint32_t sceneHistoryW = std::max(1u, g_SceneRenderTarget.GetWidth() / 2);
    const uint32_t sceneHistoryH = std::max(1u, g_SceneRenderTarget.GetHeight() / 2);
    const bool sceneGtaoEnabled = g_SceneChain.IsPassEnabled("gtao");
    const bool sceneSsgiEnabled = g_SceneChain.IsPassEnabled("ssgi");
    if (sceneGtaoEnabled) {
        EnsureAOHistoryTexture(true, sceneHistoryW, sceneHistoryH);
        PrepareAOHistoryForRead(commandBuffer, g_SceneAOHistory, g_SceneAOHistoryNeedsClear);
    }
    if (sceneSsgiEnabled) {
        EnsureSSGIHistoryTexture(true, sceneHistoryW, sceneHistoryH);
        PrepareSSGIHistoryForRead(commandBuffer, g_SceneSSGIHistory, g_SceneSSGIHistoryNeedsClear);
    }
    EnsureTAAHistoryTexture(g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight());
    PrepareTAAHistoryForRead(commandBuffer, g_SceneTAAHistory, g_SceneChain.GetPassOutputImage("taa"));   // TAA：上帧输出→历史（帧首串行，防 3 帧 in-flight 竞态）
    PostProcessChain::ExternalInputs ext;
    ext.compositeView = g_SceneRenderTarget.GetCompositeImageView();
    ext.depthView = g_SceneRenderTarget.GetDepthImageView();
    ext.gbuffer1View = g_SceneRenderTarget.GetColorImageView(1);   // 2026-08-13 GTAO 法线附件
    ext.gbuffer2View = g_SceneRenderTarget.GetColorImageView(2);   // 2026-08-13 材质附件（emissive 强度）
    ext.gbufferView = g_SceneRenderTarget.GetColorImageView(0);   // gbuffer0（gtao_apply 重建 emissive 用 albedo）
    ext.skyView = g_AtmosphereRenderer.GetSkyImageView();   // skyrt（gtao_apply 雾色）
    ext.skySampler = g_AtmosphereRenderer.GetSkySampler();
    ext.historyView = g_SceneAOHistoryView;   // 时序 GTAO 历史
    ext.historySampler = g_AOHistorySampler;
    ext.ssgiHistoryView = g_SceneSSGIHistoryView;   // 2026：时序 SSGI 历史
    ext.ssgiHistorySampler = g_SSGIHistorySampler;
    ext.taaHistoryView = g_SceneTAAHistoryView;   // TAA：上帧输出历史
    ext.gbufferMotionView = g_SceneRenderTarget.GetColorImageView(3);   // TAA depth-guided：运动向量附件
    ext.taaHistorySampler = g_TAAHistorySampler;
    // 2026-08-17：CMAA2 compute 已移到链内 tonemap pass 后（pass 后 hook，输入 LDR）——此处只取权重图
    {
        ext.cmaaWeightView = g_SceneCMAA2.GetWeightView();
        ext.cmaaWeightSampler = g_SceneCMAA2.GetWeightSampler();
    }
    ext.cameraUBO.cameraPos = glm::vec4(glm::vec3(glm::inverse(view)[3]), 1.0f);
    ext.cameraUBO.proj = proj;
    ext.cameraUBO.view = view;
    ext.cameraUBO.prevViewProj = s_PrevViewProj;
    ext.cameraUBO.invProj = glm::inverse(proj);
    ext.cameraUBO.invView = glm::inverse(view);
    // CSM 级联数据（gtao 半分辨率体积光采样阴影）——场景 slot 0
    FillCsmIntoUExt(ext, csmScene, 0, g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare));
    ext.pushData.cameraPos = ext.cameraUBO.cameraPos;
    ext.pushData.sunDir = glm::vec4(sunDir, 0.0f);
    ext.pushData.lightColor = glm::vec4(lightColor * lightIntensity, 1.0f);
    ext.pushData.frameInfo = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    g_SceneChain.Execute(commandBuffer, g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight(),
        ext, g_SceneRenderTarget.GetFinalFramebuffer());
    if (sceneGtaoEnabled) {
        CopyAOHistory(commandBuffer, g_SceneChain.GetPassOutputImage("gtao"), g_SceneAOHistory,
            sceneHistoryW, sceneHistoryH);
    }
    if (sceneSsgiEnabled) {
        CopySSGIHistory(commandBuffer, g_SceneChain.GetPassOutputImage("ssgi"), g_SceneSSGIHistory,
            sceneHistoryW, sceneHistoryH);
    }
    s_PrevViewProj = proj * view;
    // UI 叠加（链末 tonemap 后）：UI alpha 混合叠加在离屏结果之上，不受后处理/光照影响
    // 无限刻度网格也在此叠加（SceneView 专属：用户拍板画到后处理之后，不进 G-Buffer/合成）
    glm::vec3 sceneCamPos = glm::vec3(glm::inverse(view)[3]);
    RenderUIOverlay(commandBuffer, g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight(),
        g_SceneRenderTarget.GetDisplayUIRenderPass(), g_SceneRenderTarget.GetFinalFramebuffer(), false,
        &view, &proj, &sceneCamPos);
}

// 绘制游戏视图内容（3D 几何 + 2D 世界层 + UI 层 + 游戏 UI）
// 在"已开始的 render pass 的 subpass 0"内调用；编辑器离屏路径与游戏合并路径共用
// usePhysicalSky=true 时：skybox 位置画背景 quad 采样低分辨率天空 RT（物理天空）
static void RenderGameContent(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj, bool usePhysicalSky)
{
    // 每帧 2D 渲染开始：重置顶点写入位置（本帧内 RenderWorld/RenderUI/文本多次 Flush 接续写入，
    // 避免后写覆盖先前 draw 引用的顶点数据 —— Vulkan 命令缓冲统一提交后所有 draw 读缓冲最终状态）
    Renderer2D::GetInstance().ResetFrame();

    g_SkyboxRenderer.SyncFromScene();   // 从场景树读取 SkyboxComponent 状态
    bool skyboxVisible = g_SkyboxRenderer.IsEnabled();
    // 统一 3D 管线（2D/3D 场景共用）：3D 内容（skybox/模型/体素）2D 场景为空提交，2D 世界层总是绘制
    if (skyboxVisible && !usePhysicalSky) {
        g_SkyboxRenderer.Render(commandBuffer, view, proj);   // 物理天空时天空由合成 subpass 还原
    }
    g_SceneRenderer.RenderGameView(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(), view, proj);
    // 2D 玩法层（世界层）与 UI：已移到链后 RenderUIOverlay（后处理之外，玩法层在 UI 之前）——不经过 G-Buffer/合成
}

// UI 叠加 pass：链末 tonemap 之后，UI alpha 混合叠加在结果之上（编辑器=显示附件；游戏模式=swapchain）
// swapchainMode=false → Renderer2D SetDisplayUI（显示附件 loadOp=LOAD pass）；true → SetSwapchainUI（swapchain loadOp=LOAD pass）
// gridView/gridProj/gridCamPos 非空时绘制无限刻度网格（仅 SceneView 链末；GameView 传 nullptr）
static void RenderUIOverlay(VkCommandBuffer commandBuffer, uint32_t width, uint32_t height,
                            VkRenderPass uiPass, VkFramebuffer fb, bool swapchainMode,
                            const glm::mat4* gridView, const glm::mat4* gridProj, const glm::vec3* gridCamPos)
{
    if (uiPass == VK_NULL_HANDLE || fb == VK_NULL_HANDLE) return;
    VkRenderPassBeginInfo rp = {};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = uiPass;
    rp.framebuffer = fb;
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {width, height};
    rp.clearValueCount = 0;   // loadOp=LOAD 无需 clear（保留链末 tonemap 结果）
    vkCmdBeginRenderPass(commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {};
    viewport.x = 0.0f; viewport.y = 0.0f;
    viewport.width = static_cast<float>(width); viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {width, height};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    // 无限刻度网格：未移植（蓝本无 InfiniteGridRenderer——后续步骤④再加）
    // 调试线框叠加（视锥/AABB/OBB/BVH 可视化，SceneView 链末；2026-08-10 移出 G-Buffer）
    if (gridView != nullptr) {
        g_SceneRenderer.RenderOverlayLinework(commandBuffer, width, height, uiPass, *gridView, *gridProj);
    }

    auto& r2d = Renderer2D::GetInstance();
    // SceneView/GameView 在编辑器同一帧可能各自 ResetFrame；SceneView 使用
    // 独立的 secondary 帧槽，避免后一个视口覆盖前一个视口已经录入命令的顶点。
    r2d.UseSecondaryBuffer(gridView != nullptr);
    if (swapchainMode) r2d.SetSwapchainUI(true); else r2d.SetDisplayUI(true);
    r2d.ResetFrame();
    // 2D 玩法层（世界层/普通精灵）：链后、UI 之前——不经过后处理（tonemap 结果之上直接画），
    // 绕开 G-Buffer/合成路径（该路径曾导致画布不在视口中央）
    UI::Canvas2D::GetInstance().RenderWorld(r2d, commandBuffer);
    // UI 层（画布 UI + 游戏 UI）
    UI::Canvas2D::GetInstance().RenderUI(r2d, commandBuffer);
    r2d.BeginFrame(commandBuffer, UI::Canvas2D::GetInstance().GetUIViewProj(), width, height, false);
    if (auto* gm = Game::GameManager::GetInstance().GetCurrent()) {
        gm->OnRenderUI(r2d, width, height);
    }
    r2d.Flush();
    if (swapchainMode) r2d.SetSwapchainUI(false); else r2d.SetDisplayUI(false);
    r2d.UseSecondaryBuffer(false);
    vkCmdEndRenderPass(commandBuffer);
}

// GameRT 显示附件是否已渲染过（强制唤醒标志）：
// 启动后若直接进游戏模式且从未渲染过 GameView，显示附件保持未定义（loadOp DONT_CARE）——
// 游戏模式首次执行时补跑一次 Game 链填充，保证任何消费点（编辑器面板/后续切回）读到有效内容
static bool s_GameDisplayRendered = false;


// ===== 场景方向光（2026-08-10）：合成 pass 光照用场景 Directional Light；无光源回退调用方默认 =====
// 方向 = Transform rotation * forward(0,0,-1)；颜色/强度来自 LightComponent。
// 返回 false = 场景无方向光（调用方保持原 sunDir + 白光/1 强度，即"硬编码固定位置"回退）
static bool GetSceneDirectionalLight(glm::vec3& dir, glm::vec3& color, float& intensity) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();
    bool found = false;
    std::function<void(ECS::Entity)> visit = [&](ECS::Entity e) {
        if (found) return;
        if (coordinator.HasComponent<ECS::LightComponent>(e) && coordinator.HasComponent<ECS::TransformComponent>(e)) {
            const auto& light = coordinator.GetComponent<ECS::LightComponent>(e);
            if (light.type == ECS::LightComponent::Type::Directional) {
                const auto& tf = coordinator.GetComponent<ECS::TransformComponent>(e);
                dir = glm::normalize(tf.rotation * glm::vec3(0.0f, 0.0f, -1.0f));
                color = light.color;
                intensity = light.intensity;
                found = true;
            }
        }
        for (const auto child : sceneECS.GetChildren(e)) visit(child);
    };
    for (const auto root : sceneECS.GetRootEntities()) visit(root);
    return found;
}

// ===== 点光源（2026-08-13 第一步：常规遍历，无阴影）=====
// 收集场景所有 Point 型 LightComponent → GPU 布局（vec4 position_range / vec4 color_intensity）
// 与 fullscreen.frag binding 11 UBO（std140，32×32B + int count + padding = 1040B）对齐
static constexpr int MAX_POINT_LIGHTS = 32;
struct GpuPointLight {
    glm::vec4 position_range;    // xyz = 世界位置（Transform），w = range
    glm::vec4 color_intensity;   // rgb = 颜色，w = intensity
    glm::vec4 shadow_info;       // 2026-08-13：x = 阴影槽（-1 无阴影；0..MAX_SHADOW_LIGHTS-1 有），yz 保留
};
// 每帧点光源阴影列表（2026-08-13）：UpdatePointLightBuffer 收集时同步填充（UBO shadow_info 槽号 = 此列表下标），
// FrameRender 阴影渲染直接消费——两处必须同源，否则 UBO 槽号与渲染列表错位
static SceneRenderer::ShadowLight g_shadowLightList[PointShadowRenderer::MAX_SHADOW_LIGHTS];
static int g_shadowLightCount = 0;
static int CollectPointLights(GpuPointLight* out, int maxCount,
                              SceneRenderer::ShadowLight* shadowOut = nullptr, int maxShadow = 0) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();
    int n = 0;
    int shadowSlot = 0;
    std::function<void(ECS::Entity)> visit = [&](ECS::Entity e) {
        if (n >= maxCount) return;
        if (coordinator.HasComponent<ECS::LightComponent>(e) && coordinator.HasComponent<ECS::TransformComponent>(e)) {
            const auto& light = coordinator.GetComponent<ECS::LightComponent>(e);
            if (light.type == ECS::LightComponent::Type::Point) {
                const auto& tf = coordinator.GetComponent<ECS::TransformComponent>(e);
                out[n].position_range = glm::vec4(tf.position, light.range > 0.0f ? light.range : 1.0f);
                out[n].color_intensity = glm::vec4(light.color, light.intensity);
                // 2026-08-13 castShadow 开关：勾选的光源按序分配阴影槽（≤MAX_SHADOW_LIGHTS），其余 -1
                out[n].shadow_info = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
                if (light.castShadow && shadowOut && shadowSlot < maxShadow) {
                    out[n].shadow_info.x = (float)shadowSlot;
                    shadowOut[shadowSlot].position = glm::vec3(tf.position);
                    shadowOut[shadowSlot].range = light.range > 0.0f ? light.range : 1.0f;
                    shadowSlot++;
                }
                n++;
            }
        }
        for (const auto child : sceneECS.GetChildren(e)) visit(child);
    };
    for (const auto root : sceneECS.GetRootEntities()) visit(root);
    return n;
}
// 点光源 UBO（惰性创建 + 常驻映射；每帧写满 32 个槽 + count，未使用槽清零）
static VkBuffer GetPointLightBuffer(void** mappedPtr) {
    static VkBuffer buf = VK_NULL_HANDLE;
    static VkDeviceMemory mem = VK_NULL_HANDLE;
    static void* mapped = nullptr;
    if (buf != VK_NULL_HANDLE) {
        if (mappedPtr) *mappedPtr = mapped;
        return buf;
    }
    VkBufferCreateInfo binfo = {};
    binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    binfo.size = MAX_POINT_LIGHTS * (uint32_t)sizeof(GpuPointLight) + 16;   // 32×32 + count/padding
    binfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &binfo, g_Allocator, &buf) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_Device, buf, &req);
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &props);
    uint32_t mt = VK_MAX_MEMORY_TYPES;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            mt = i; break;
        }
    }
    if (mt == VK_MAX_MEMORY_TYPES) return VK_NULL_HANDLE;
    VkMemoryAllocateInfo ainfo = {};
    ainfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ainfo.allocationSize = req.size;
    ainfo.memoryTypeIndex = mt;
    if (vkAllocateMemory(g_Device, &ainfo, g_Allocator, &mem) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkBindBufferMemory(g_Device, buf, mem, 0);
    if (vkMapMemory(g_Device, mem, 0, binfo.size, 0, &mapped) == VK_SUCCESS) {
        memset(mapped, 0, binfo.size);   // 初始清零（未用槽）
    }
    if (mappedPtr) *mappedPtr = mapped;
    return buf;
}
// 每帧：收集场景点光源 → 写 UBO（未用槽保持 0），返回 buffer 供 descriptor 绑定
// 2026-08-13：同步填阴影列表（g_shadowLightList）——UBO 的 shadow_info.x 槽号与该列表下标一致
static VkBuffer UpdatePointLightBuffer() {
    void* mapped = nullptr;
    VkBuffer buf = GetPointLightBuffer(&mapped);
    if (!buf || !mapped) return buf;
    GpuPointLight pls[MAX_POINT_LIGHTS] = {};
    int n = CollectPointLights(pls, MAX_POINT_LIGHTS, g_shadowLightList, PointShadowRenderer::MAX_SHADOW_LIGHTS);
    g_shadowLightCount = 0;
    for (int i = 0; i < n; i++) {
        if (pls[i].shadow_info.x >= 0.0f) g_shadowLightCount++;
    }
    memcpy(mapped, pls, sizeof(pls));
    int* countPtr = (int*)((char*)mapped + MAX_POINT_LIGHTS * (int)sizeof(GpuPointLight));
    *countPtr = n;
    return buf;
}

// ===== Cluster 光源剔除（2026-08-13 第二步：分块 + GPU cull）=====
// 12×12 屏幕 tile × 24 深度切片（指数分割），view 空间 AABB——与 cluster_cull.comp / fullscreen.frag 严格一致
// CPU 每帧算 AABB（2.7 万次求交 <0.1ms）→ GPU compute 球-AABB cull → grid SSBO 供合成 pass 查询
static constexpr int CLUSTER_X = 12, CLUSTER_Y = 12, CLUSTER_Z = 24;
static constexpr int CLUSTER_COUNT = CLUSTER_X * CLUSTER_Y * CLUSTER_Z;   // 3456
static constexpr uint32_t CLUSTER_GRID_SIZE = 16 + 3456 * 168;            // params vec4 + Cluster[3456]（std430）

struct GpuCluster {
    glm::vec4 minPoint;   // view 空间 AABB
    glm::vec4 maxPoint;
    uint32_t count;
    uint32_t pad;
    uint32_t lightIndices[MAX_POINT_LIGHTS];
};

// scene/game 两套（各自相机的 cluster 独立；pipeline/ds layout 共用）
static bool CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem, void** mapped);   // 定义见下方
static VkDescriptorSet EnsureClusterDescriptorSet(VkBuffer aabbBuf, VkBuffer gridBuf);   // 定义见下方
struct ClusterCulling {
    VkBuffer aabbBuf = VK_NULL_HANDLE, gridBuf = VK_NULL_HANDLE;
    VkDeviceMemory aabbMem = VK_NULL_HANDLE, gridMem = VK_NULL_HANDLE;
    void* aabbMapped = nullptr;
    void* gridMapped = nullptr;
    VkDescriptorSet ds = VK_NULL_HANDLE;

    bool Ensure() {
        if (aabbBuf && gridBuf && ds) return true;
        // AABB SSBO：3456 × 2 × vec4
        if (!aabbBuf) {
            if (!CreateHostBuffer(3456 * 2 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, aabbBuf, aabbMem, &aabbMapped)) return false;
        }
        // grid SSBO：头部 params（CPU 写）+ Cluster[3456]（GPU cull 写）
        if (!gridBuf) {
            if (!CreateHostBuffer(CLUSTER_GRID_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gridBuf, gridMem, &gridMapped)) return false;
            memset(gridMapped, 0, CLUSTER_GRID_SIZE);
        }
        if (!ds) ds = EnsureClusterDescriptorSet(aabbBuf, gridBuf);
        return ds != VK_NULL_HANDLE;
    }
};

static ClusterCulling g_SceneCluster, g_GameCluster;
static VkPipelineLayout g_ClusterPipeLayout = VK_NULL_HANDLE;
static VkPipeline g_ClusterPipeline = VK_NULL_HANDLE;
static VkDescriptorSetLayout g_ClusterDSLayout = VK_NULL_HANDLE;

// 通用 host-visible buffer 创建（与 GetPointLightBuffer 同款）
static bool CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem, void** mapped) {
    VkBufferCreateInfo binfo = {};
    binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    binfo.size = size;
    binfo.usage = usage;
    binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &binfo, g_Allocator, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_Device, buf, &req);
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &props);
    uint32_t mt = VK_MAX_MEMORY_TYPES;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            mt = i; break;
        }
    }
    if (mt == VK_MAX_MEMORY_TYPES) return false;
    VkMemoryAllocateInfo ainfo = {};
    ainfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ainfo.allocationSize = req.size;
    ainfo.memoryTypeIndex = mt;
    if (vkAllocateMemory(g_Device, &ainfo, g_Allocator, &mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(g_Device, buf, mem, 0);
    return vkMapMemory(g_Device, mem, 0, size, 0, mapped) == VK_SUCCESS;
}

// compute pipeline + descriptor（惰性，一次；binding 0=光源 UBO、1=AABB SSBO、2=grid SSBO）
static bool EnsureClusterPipeline() {
    if (g_ClusterPipeline) return true;
    // 跨平台读取 spv：Android 上 APK assets 不能走 std::ifstream，同 VoxelMeshGPUCulling 用 SDL IO
    const std::string spvPath = EngineConfig::GetShaderPath("cluster_cull.comp.spv");
    std::vector<char> code;
    if (SDL_IOStream* io = SDL_IOFromFile(spvPath.c_str(), "rb")) {
        Sint64 sz = SDL_GetIOSize(io);
        if (sz > 0) {
            code.resize((size_t)sz);
            if (SDL_ReadIO(io, code.data(), (size_t)sz) != (size_t)sz) code.clear();
        }
        SDL_CloseIO(io);
    }
    if (code.empty()) { LOGE("[ClusterCulling] shader not found: cluster_cull.comp.spv"); return false; }
    VkShaderModule sm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sci.codeSize = code.size();
    sci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    if (vkCreateShaderModule(g_Device, &sci, g_Allocator, &sm) != VK_SUCCESS) return false;

    // descriptor set layout：0 UBO 光源、1 SSBO AABB（readonly）、2 SSBO grid（writeonly）
    VkDescriptorSetLayoutBinding cb[3] = {};
    cb[0].binding = 0; cb[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; cb[0].descriptorCount = 1; cb[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    cb[1].binding = 1; cb[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; cb[1].descriptorCount = 1; cb[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    cb[2].binding = 2; cb[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; cb[2].descriptorCount = 1; cb[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dli = {};
    dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dli.bindingCount = 3;
    dli.pBindings = cb;
    if (vkCreateDescriptorSetLayout(g_Device, &dli, g_Allocator, &g_ClusterDSLayout) != VK_SUCCESS) return false;

    VkPipelineLayoutCreateInfo pli = {};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &g_ClusterDSLayout;
    // 2026-08-13：view 矩阵（世界→view，cull 光源坐标变换用）
    VkPushConstantRange pcr = {};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = 64;   // mat4
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(g_Device, &pli, g_Allocator, &g_ClusterPipeLayout) != VK_SUCCESS) return false;

    VkComputePipelineCreateInfo cpi = {};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = sm;
    cpi.stage.pName = "main";
    cpi.layout = g_ClusterPipeLayout;
    if (vkCreateComputePipelines(g_Device, VK_NULL_HANDLE, 1, &cpi, g_Allocator, &g_ClusterPipeline) != VK_SUCCESS) return false;
    vkDestroyShaderModule(g_Device, sm, g_Allocator);
    return true;
}

// 每实例 descriptor set（光源 buffer 共用；AABB/grid 各自）
static VkDescriptorSet EnsureClusterDescriptorSet(VkBuffer aabbBuf, VkBuffer gridBuf) {
    if (!EnsureClusterPipeline()) return VK_NULL_HANDLE;
    VkDescriptorPoolSize ps[3] = {};
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; ps[0].descriptorCount = 1;
    ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ps[1].descriptorCount = 2;
    VkDescriptorPoolCreateInfo pci = {};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 1;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(g_Device, &pci, g_Allocator, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorSet ds = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_ClusterDSLayout;
    if (vkAllocateDescriptorSets(g_Device, &ai, &ds) != VK_SUCCESS) return VK_NULL_HANDLE;

    VkDescriptorBufferInfo infoLight = {};
    infoLight.buffer = GetPointLightBuffer(nullptr);
    infoLight.offset = 0;
    infoLight.range = 1552;   // 32×(16×3) + 16（GpuPointLight 48B×3 vec4 + count）
    VkDescriptorBufferInfo infoAABB = {};
    infoAABB.buffer = aabbBuf;
    infoAABB.offset = 0;
    infoAABB.range = 3456 * 2 * 16;
    VkDescriptorBufferInfo infoGrid = {};
    infoGrid.buffer = gridBuf;
    infoGrid.offset = 0;
    infoGrid.range = CLUSTER_GRID_SIZE;
    VkWriteDescriptorSet wr[3] = {};
    wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[0].dstSet = ds; wr[0].dstBinding = 0;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[0].descriptorCount = 1; wr[0].pBufferInfo = &infoLight;
    wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[1].dstSet = ds; wr[1].dstBinding = 1;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[1].descriptorCount = 1; wr[1].pBufferInfo = &infoAABB;
    wr[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[2].dstSet = ds; wr[2].dstBinding = 2;
    wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[2].descriptorCount = 1; wr[2].pBufferInfo = &infoGrid;
    vkUpdateDescriptorSets(g_Device, 3, wr, 0, nullptr);
    return ds;
}

// CPU 算 cluster AABB（view 空间）→ 写 AABB SSBO + grid 头部 params（near/far/screenW/H）
static void ComputeClusterAABB(ClusterCulling& cc, const glm::mat4& proj, float screenW, float screenH) {
    if (!cc.aabbMapped || !cc.gridMapped) return;
    // ⚠️ 2026-08-13：项目 proj 是 glm::perspective 默认 **-1..1 深度约定**（无 GLM_FORCE_DEPTH_ZERO_TO_ONE）
    //   m22=(f+n)/(n-f)、m32=2fn/(n-f) → near = m32/(m22-1)、far = m32/(m22+1)（勿用 0-1 约定公式！）
    const float m22 = proj[2][2], m32 = proj[3][2];
    const float nearP = m32 / (m22 - 1.0f);
    const float farP = m32 / (m22 + 1.0f);
    const float tanH = 1.0f / proj[1][1];
    const float tanW = tanH * (proj[1][1] / proj[0][0]);
    // ⚠️ shader 布局是分离数组（vec4 minPts[3456]; vec4 maxPts[3456];）——必须分离写！
    // 曾交错写（min0,max0,min1,max1...）→ 每个 cluster 读到别的 cluster 的 AABB → 剔除错乱（网格遮罩）
    glm::vec4* aabbMin = (glm::vec4*)cc.aabbMapped;
    glm::vec4* aabbMax = (glm::vec4*)cc.aabbMapped + CLUSTER_COUNT;
    for (int tz = 0; tz < CLUSTER_Z; tz++) {
        const float zNear = nearP * powf(farP / nearP, tz / (float)CLUSTER_Z);
        const float zFar = nearP * powf(farP / nearP, (tz + 1) / (float)CLUSTER_Z);
        for (int ty = 0; ty < CLUSTER_Y; ty++) {
            for (int tx = 0; tx < CLUSTER_X; tx++) {
                glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
                for (int cy = 0; cy < 2; cy++) {
                    const float ndcY = (ty + cy) * 2.0f / CLUSTER_Y - 1.0f;
                    for (int cx = 0; cx < 2; cx++) {
                        const float ndcX = (tx + cx) * 2.0f / CLUSTER_X - 1.0f;
                        for (int cz = 0; cz < 2; cz++) {
                            const float z = cz ? -zFar : -zNear;   // view z（负朝前）
                            glm::vec3 p(ndcX * tanW * -z, ndcY * tanH * -z, z);
                            mn = glm::min(mn, p); mx = glm::max(mx, p);
                        }
                    }
                }
                const int idx = tz * (CLUSTER_X * CLUSTER_Y) + ty * CLUSTER_X + tx;
                aabbMin[idx] = glm::vec4(mn, 1.0f);
                aabbMax[idx] = glm::vec4(mx, 1.0f);
            }
        }
    }
    ((glm::vec4*)cc.gridMapped)[0] = glm::vec4(nearP, farP, screenW, screenH);   // params 头部（cull 不碰）
}

// 每帧：算 AABB + dispatch cull（必须在合成 render pass 前、光源 UBO 更新后）
// ⚠️ 2026-08-13：点光源 ≤16 时跳过 dispatch（fragment 走全遍历分支，不读 grid）——少量光源 cluster 是净负收益
static constexpr int CLUSTER_MIN_LIGHTS = 16;
static void DispatchClusterCull(VkCommandBuffer cmd, ClusterCulling& cc, const glm::mat4& view, const glm::mat4& proj, float screenW, float screenH) {
    void* plMapped = nullptr;
    GetPointLightBuffer(&plMapped);
    if (plMapped) {
        const int cnt = *(int*)((char*)plMapped + MAX_POINT_LIGHTS * (int)sizeof(GpuPointLight));
        if (cnt <= CLUSTER_MIN_LIGHTS) return;   // 少量光源：不 dispatch（grid 残留旧数据，fragment 不读）
    }
    if (!EnsureClusterPipeline()) return;
    if (!cc.Ensure()) return;
    ComputeClusterAABB(cc, proj, screenW, screenH);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_ClusterPipeline);
    vkCmdPushConstants(cmd, g_ClusterPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 64, &view);   // 世界→view（cull 光源变换）
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_ClusterPipeLayout, 0, 1, &cc.ds, 0, nullptr);
    vkCmdDispatch(cmd, (CLUSTER_COUNT + 127) / 128, 1, 1);   // 3456/128 = 27 groups
    // compute 写 grid → 合成 pass fragment 读：必须 buffer barrier（否则读到旧 count=0）
    VkBufferMemoryBarrier bmb = {};
    bmb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bmb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bmb.buffer = cc.gridBuf;
    bmb.offset = 0;
    bmb.size = CLUSTER_GRID_SIZE;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 1, &bmb, 0, nullptr);
}

// 合成 quad 绑定用的 grid buffer（惰性 Ensure；合成 pass 读 cull 结果）
static VkBuffer GetSceneClusterGridBuffer() { return g_SceneCluster.Ensure() ? g_SceneCluster.gridBuf : VK_NULL_HANDLE; }
static VkBuffer GetGameClusterGridBuffer() { return g_GameCluster.Ensure() ? g_GameCluster.gridBuf : VK_NULL_HANDLE; }

// 渲染游戏视图到离屏目标并生成 Hi-ZB（编辑器模式：GameView 面板采样显示附件——统一合成管线）
static void RenderGameToTarget(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPos,
                               const glm::vec3& cameraFront, const glm::vec3& cameraRight, const glm::vec3& cameraUp,
                               uint32_t frameIndex)   // 2026-08-10：GPU 时间戳槽位
{
    (void)cameraPos; (void)cameraFront; (void)cameraRight; (void)cameraUp;
    VkCommandBuffer commandBuffer = g_MainWindowData.Frames[g_MainWindowData.FrameIndex].CommandBuffer;
    // 2026-08-17 TAA：相机亚像素抖动（Halton 2,3）——几何/合成/pushData 全用 jittered 投影；UI 叠加保持原 proj
    g_CurrentTAAJitter = g_GameChain.IsPassEnabled("taa") ? ComputeTAAJitter(g_TAAJitterFrameGameView, (float)g_GameRenderTarget.GetWidth(), (float)g_GameRenderTarget.GetHeight()) : glm::vec2(0.0f);
    (void)proj;


    // 物理天空全景图已由 FrameRender 统一渲染（相机无关）——此处只判断 usePhysicalSky
    g_SkyboxRenderer.SyncFromScene();
    bool usePhysicalSky = false;
    if (g_SkyboxRenderer.IsEnabled() && g_AtmosphereEnabled && g_AtmosphereRenderer.IsInitialized()) {
        usePhysicalSky = true;
    }

    // 场景方向光收集（函数级：CSM 阴影 + 合成共用；无光源回退太阳方向/白光/1 强度）
    glm::vec3 lightDir, lightColor(1.0f, 0.96f, 0.89f); float lightIntensity = 1.0f;
    glm::vec3 sunDir = g_AtmosphereRenderer.GetSunDirection();
    if (GetSceneDirectionalLight(lightDir, lightColor, lightIntensity)) sunDir = lightDir;

    // 合成描述符：GameRT 颜色0/深度 + 天空 RT（独立 descriptor set，勿与 SceneView 共用）
    if (!g_TexturePool->GetTexture("sky_hdr")) g_TexturePool->LoadHDRCubemap("sky_hdr", EngineConfig::GetEngineTexturePath("skybox") + "/EnvironmentMap/snow_field_puresky_1k.hdr");
    const TextureInfo* skyHDR3 = g_TexturePool->GetTexture("sky_hdr");
    const TextureInfo* skyIrr3 = g_TexturePool->GetTexture("sky_hdr_irr");
    // 2026-08-12：IBL 物理大气优先（与 UpdateFullscreenQuadDescriptors 一致）——sky_hdr 静态作 fallback
    VkImageView gameSkyCube = g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyCubeView() : (skyHDR3 ? skyHDR3->imageView : VK_NULL_HANDLE);
    VkSampler gameSkyCubeSamp = g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyCubeSampler() : g_TexturePool->GetSamplerByType(SamplerType::Linear);
    // 2026-08-14：CSM 方向光阴影（slot 1 = GameView）——合成 descriptor 绑定前确保渲染器就绪
    CascadeShadowRenderer* csmGame = g_SceneRenderer.EnsureCascadeShadows();
    g_GameCompositeQuad.UpdateDescriptorSet(g_GameRenderTarget.GetColorImageView(), g_GameRenderTarget.GetDepthImageView(), g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyImageView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkySampler() : VK_NULL_HANDLE, g_GameRenderTarget.GetColorImageView(1), g_GameRenderTarget.GetColorImageView(2), (g_TexturePool->GetTexture("end_sky")) ? g_TexturePool->GetTexture("end_sky")->imageView : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetTransmittanceView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetScatteringView() : VK_NULL_HANDLE, gameSkyCube, gameSkyCubeSamp, skyIrr3 ? skyIrr3->imageView : (skyHDR3 ? skyHDR3->imageView : VK_NULL_HANDLE), g_TexturePool->GetSamplerByType(SamplerType::Linear), GetShIrradianceBuffer(), UpdatePointLightBuffer(), GetGameClusterGridBuffer(),
        (g_SceneRenderer.EnsurePointShadows() && g_SceneRenderer.EnsurePointShadows()->IsInitialized()) ? g_SceneRenderer.EnsurePointShadows()->GetCubeArrayView() : VK_NULL_HANDLE,
        (csmGame && csmGame->IsInitialized()) ? csmGame->GetArrayView(1) : VK_NULL_HANDLE,
        g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare),   // 2026-08-15：阴影比较采样器（硬件 PCF，HSPE 同款）
        (csmGame && csmGame->IsInitialized()) ? csmGame->GetCascadeBuffer(1, (int)g_MainWindowData.FrameIndex) : VK_NULL_HANDLE,
        g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutView() : VK_NULL_HANDLE,   // 2026-08-15：split-sum BRDF LUT
        g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutSampler() : VK_NULL_HANDLE);

    // 2026-08-14：CSM 方向光阴影（每视口相机各一套——GameView 槽 1）——主 render pass 前渲染 + barrier
    csmGame->SetFrameIndex((int)g_MainWindowData.FrameIndex);   // per-frame UBO 双缓冲（帧竞争修复）
    g_SceneRenderer.RenderCascadeShadowMaps(commandBuffer, 1, view, proj, sunDir);

    g_GameRenderTarget.BeginRender(commandBuffer);
    // z-prepass（subpass 0，depth-only）——g_EnableZPrepass 开关（2026-08-10 GPU 对比验证）
    if (g_EnableZPrepass) {
        g_SceneRenderer.RenderDepthPrepass(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(), view, proj);
    }
    g_GameRenderTarget.NextSubpass(commandBuffer);   // → subpass 1（几何/2D/UI）
    RenderGameContent(commandBuffer, view, proj, usePhysicalSky);

    // 合成 subpass（subpass 2）：几何写 G-Buffer → 切换 subpass → input attachment 合成 → 中间附件
    g_GameRenderTarget.NextSubpass(commandBuffer);
    // ⚠️ 2026-08-14 一次性诊断：确认 proj/invViewProj 深度约定（直传 vs *2-1 之谜）
    {
        static bool s_pcLogged = false;
        if (!s_pcLogged) {
            s_pcLogged = true;
            glm::mat4 ivp = glm::inverse(proj * view);
            LOGI("[PC] proj: p00=%.4f p11=%.4f p22=%.4f p23=%.4f p32=%.4f p33=%.4f",
                 proj[0][0], proj[1][1], proj[2][2], proj[2][3], proj[3][2], proj[3][3]);
            LOGI("[PC] invViewProj: m00=%.4f m11=%.4f m22=%.4f m23=%.4f m32=%.4f m33=%.4f",
                 ivp[0][0], ivp[1][1], ivp[2][2], ivp[2][3], ivp[3][2], ivp[3][3]);
        }
    }
    g_GameCompositeQuad.Render(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
        glm::inverse(proj * view), glm::vec3(glm::inverse(view)[3]), sunDir, proj, view);
    g_GameRenderTarget.EndRender(commandBuffer);
    // 后处理链（配置驱动）：GameRT composite → 链逐 pass → 显示附件
    CompositeToFinalBarrier(commandBuffer, g_GameRenderTarget.GetCompositeImage());
    const uint32_t gameHistoryW = std::max(1u, g_GameRenderTarget.GetWidth() / 2);
    const uint32_t gameHistoryH = std::max(1u, g_GameRenderTarget.GetHeight() / 2);
    const bool gameGtaoEnabled = g_GameChain.IsPassEnabled("gtao");
    const bool gameSsgiEnabled = g_GameChain.IsPassEnabled("ssgi");
    if (gameGtaoEnabled) {
        EnsureAOHistoryTexture(false, gameHistoryW, gameHistoryH);
        PrepareAOHistoryForRead(commandBuffer, g_GameAOHistory, g_GameAOHistoryNeedsClear);
    }
    if (gameSsgiEnabled) {
        EnsureSSGIHistoryTexture(false, gameHistoryW, gameHistoryH);
        PrepareSSGIHistoryForRead(commandBuffer, g_GameSSGIHistory, g_GameSSGIHistoryNeedsClear);
    }
    EnsureTAAHistoryTexture(g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight());
    PrepareTAAHistoryForRead(commandBuffer, g_GameTAAHistory, g_GameChain.GetPassOutputImage("taa"));   // TAA：上帧输出→历史（帧首串行，防 3 帧 in-flight 竞态）
    PostProcessChain::ExternalInputs ext;
    ext.compositeView = g_GameRenderTarget.GetCompositeImageView();
    ext.depthView = g_GameRenderTarget.GetDepthImageView();
    ext.gbuffer1View = g_GameRenderTarget.GetColorImageView(1);   // 2026-08-13 GTAO 法线附件
    ext.gbuffer2View = g_GameRenderTarget.GetColorImageView(2);   // 2026-08-13 材质附件（emissive 强度）
    ext.gbufferView = g_GameRenderTarget.GetColorImageView(0);   // gbuffer0（gtao_apply 重建 emissive 用 albedo）
    ext.skyView = g_AtmosphereRenderer.GetSkyImageView();   // skyrt（gtao_apply 雾色）
    ext.skySampler = g_AtmosphereRenderer.GetSkySampler();
    ext.historyView = g_GameAOHistoryView;   // 时序 GTAO 历史
    ext.historySampler = g_AOHistorySampler;
    ext.ssgiHistoryView = g_GameSSGIHistoryView;   // 2026：时序 SSGI 历史
    ext.ssgiHistorySampler = g_SSGIHistorySampler;
    ext.taaHistoryView = g_GameTAAHistoryView;   // TAA：上帧输出历史
    ext.gbufferMotionView = g_GameRenderTarget.GetColorImageView(3);   // TAA depth-guided：运动向量附件
    ext.taaHistorySampler = g_TAAHistorySampler;
    // 2026-08-17：CMAA2 compute 已移到链内 tonemap pass 后（pass 后 hook，输入 LDR）——此处只取权重图
    {
        ext.cmaaWeightView = g_GameCMAA2.GetWeightView();
        ext.cmaaWeightSampler = g_GameCMAA2.GetWeightSampler();
    }
    ext.cameraUBO.cameraPos = glm::vec4(glm::vec3(glm::inverse(view)[3]), 1.0f);
    ext.cameraUBO.proj = proj;
    ext.cameraUBO.view = view;
    ext.cameraUBO.prevViewProj = s_PrevViewProj;
    ext.cameraUBO.invProj = glm::inverse(proj);
    ext.cameraUBO.invView = glm::inverse(view);
    FillCsmIntoUExt(ext, csmGame, 1, g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare));
    ext.pushData.cameraPos = ext.cameraUBO.cameraPos;
    ext.pushData.sunDir = glm::vec4(sunDir, 0.0f);
    ext.pushData.lightColor = glm::vec4(lightColor * lightIntensity, 1.0f);
    ext.pushData.frameInfo = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    g_GameChain.Execute(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
        ext, g_GameRenderTarget.GetFinalFramebuffer());
    if (gameGtaoEnabled) {
        CopyAOHistory(commandBuffer, g_GameChain.GetPassOutputImage("gtao"), g_GameAOHistory,
            gameHistoryW, gameHistoryH);
    }
    if (gameSsgiEnabled) {
        CopySSGIHistory(commandBuffer, g_GameChain.GetPassOutputImage("ssgi"), g_GameSSGIHistory,
            gameHistoryW, gameHistoryH);
    }
    s_GameDisplayRendered = true;
    s_PrevViewProj = proj * view;
    // 保存当前帧 VP 供下一帧 GTAO 重投影
    // UI 叠加（链末 tonemap 后）：UI alpha 混合叠加在 GameRT 显示附件（编辑器 GameView 面板）之上
    RenderUIOverlay(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
        g_GameRenderTarget.GetDisplayUIRenderPass(), g_GameRenderTarget.GetFinalFramebuffer(), false);

    // 生成 Hi-ZB（深度金字塔）——仅 3D 且存在体素时（无体素消费者则跳过，2026-08-10 省 GPU 34%）
    if (false) {   // 2026-08-12 用户：先跳过 Hi-Z 生成（暂时不需要）
        VkImage depthImage = g_GameRenderTarget.GetDepthImage();
        uint32_t mipLevels = g_SceneRenderer.GetHiZShader().GetMipLevels();
        if (depthImage != VK_NULL_HANDLE && mipLevels > 0)
            g_SceneRenderer.GetHiZShader().GenerateMipLevels(commandBuffer, depthImage, mipLevels);
    }
}

// 游戏模式：几何写 GameRT G-Buffer（与编辑器 GameView 同一路径）→ 合成 subpass（subpass 1）
// 全屏四边形经 input attachment 读 GameRT 颜色0/深度合成到中间附件（深度 STORE 供 Hi-Z）→ 后处理链 → swapchain
static void RenderGameComposite(const glm::mat4& view, const glm::mat4& proj, uint32_t frameIndex)   // 2026-08-10：GPU 时间戳槽位
{
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    VkCommandBuffer commandBuffer = wd->Frames[wd->FrameIndex].CommandBuffer;

#ifdef __ANDROID__
    // ===== 2026-08-21 逐步恢复 MRT：几何 subpass 0 + 合成 subpass 2，composite 直连 blit 到 swapchain（暂跳过后处理链）=====
    // 三 subpass 结构（0=z-prepass、1=几何、2=合成）已在 RenderTarget.cpp 恢复；合成 quad 由 InitCompositeResources 初始化。
    {
        VkViewport viewport = {};
        viewport.x = 0.0f; viewport.y = 0.0f;
        viewport.width = (float)wd->Width; viewport.height = (float)wd->Height;
        viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        VkRect2D scissor = {};
        scissor.offset = { 0, 0 };
        scissor.extent = { (uint32_t)wd->Width, (uint32_t)wd->Height };
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

        // 场景方向光（合成光照用；无光源回退默认太阳方向/白光 1 强度）
        glm::vec3 lightDir, lightColor(1.0f, 0.96f, 0.89f); float lightIntensity = 1.0f;
        glm::vec3 sunDir = g_AtmosphereRenderer.GetSunDirection();
        if (GetSceneDirectionalLight(lightDir, lightColor, lightIntensity)) sunDir = lightDir;

        // 合成 quad descriptor（G-Buffer input attachments + 天空/IBL/光源）。
        // 阴影/CSM 暂不渲染（后续逐步恢复）——但必须绑定有效句柄防空解引用（shader 读取 csmParams/阴影槽时）。
        CascadeShadowRenderer* csmGame0 = g_SceneRenderer.EnsureCascadeShadows();
        g_GameCompositeQuad.UpdateDescriptorSet(
            g_GameRenderTarget.GetColorImageView(), g_GameRenderTarget.GetDepthImageView(),
            g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyImageView() : VK_NULL_HANDLE,
            g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkySampler() : VK_NULL_HANDLE,
            g_GameRenderTarget.GetColorImageView(1), g_GameRenderTarget.GetColorImageView(2),
            (g_TexturePool->GetTexture("end_sky")) ? g_TexturePool->GetTexture("end_sky")->imageView : VK_NULL_HANDLE,
            g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetTransmittanceView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetScatteringView() : VK_NULL_HANDLE,
            (g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyCubeView() : VK_NULL_HANDLE),
            g_TexturePool->GetSamplerByType(SamplerType::Linear),
            (g_TexturePool->GetTexture("sky_hdr_irr")) ? g_TexturePool->GetTexture("sky_hdr_irr")->imageView : VK_NULL_HANDLE,
            g_TexturePool->GetSamplerByType(SamplerType::Linear),
            GetShIrradianceBuffer(), UpdatePointLightBuffer(), GetGameClusterGridBuffer(),
            (g_SceneRenderer.EnsurePointShadows() && g_SceneRenderer.EnsurePointShadows()->IsInitialized()) ? g_SceneRenderer.EnsurePointShadows()->GetCubeArrayView() : VK_NULL_HANDLE,
            (csmGame0 && csmGame0->IsInitialized()) ? csmGame0->GetArrayView(0) : VK_NULL_HANDLE,
            g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare),
            (csmGame0 && csmGame0->IsInitialized()) ? csmGame0->GetCascadeBuffer(0, (int)g_MainWindowData.FrameIndex) : VK_NULL_HANDLE,
            g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutView() : VK_NULL_HANDLE,
            g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutSampler() : VK_NULL_HANDLE);

        // 物理天空（大气渲染）：合成 pass 前生成 skyRT（compute dispatch LUT + pano→cube IBL）
        // 2026-08-22：移动端与桌面一致启用完整大气渲染
        if (g_SkyboxRenderer.IsEnabled() && g_AtmosphereEnabled && g_AtmosphereRenderer.IsInitialized()) {
            g_AtmosphereRenderer.RenderSkyRT(commandBuffer, sunDir, glm::vec3(glm::inverse(view)[3]));
        }

        // TAA 开启时，在几何阶段加入 Halton 亚像素抖动；TAA 关闭时保持零偏移。
        // 该值由 model.vert 只作用于光栅化 xy，运动矢量仍使用无抖动的真实位置。
        g_CurrentTAAJitter = g_SwapChain.IsPassEnabled("taa")
            ? ComputeTAAJitter(g_TAAJitterFrameGame, (float)g_GameRenderTarget.GetWidth(),
                               (float)g_GameRenderTarget.GetHeight())
            : glm::vec2(0.0f);

        // 几何 render pass（单 subpass）：GameRT 写 G-Buffer（4 颜色附件 + depth）
        // 跳过 z-prepass——几何 subpass 自身做深度测试，正确性不受影响（Adreno 多 subpass 的 vkCreateRenderPass 即崩）
        g_GameRenderTarget.BeginRender(commandBuffer);
        RenderGameContent(commandBuffer, view, proj, false);
        g_GameRenderTarget.EndRender(commandBuffer);

        // 分离合成通道（独立单 subpass render pass）：全屏四边形 texture 采样 G-Buffer → 光照 → composite
        g_GameRenderTarget.BeginCompositeRender(commandBuffer);
        g_GameCompositeQuad.Render(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
            glm::inverse(proj * view), glm::vec3(glm::inverse(view)[3]), sunDir, proj, view, glm::vec4(lightColor * lightIntensity, 1.0f));
        g_GameRenderTarget.EndCompositeRender(commandBuffer);

        // 完整移动端后处理链：composite → TAA → bloom → tonemap → FXAA → swapchain。
        // composite 仍保持 COLOR_ATTACHMENT_OPTIMAL，由链首采样；不要用直出 blit 绕过链。
        CompositeToFinalBarrier(commandBuffer, g_GameRenderTarget.GetCompositeImage());
        const bool mobileGtaoEnabled = g_SwapChain.IsPassEnabled("gtao");
        const bool mobileTaaEnabled = g_SwapChain.IsPassEnabled("taa");
        const uint32_t mobileAOHistoryW = std::max(1u, static_cast<uint32_t>(wd->Width) / 2);
        const uint32_t mobileAOHistoryH = std::max(1u, static_cast<uint32_t>(wd->Height) / 2);
        if (mobileGtaoEnabled) {
            // gtao 是移动链中的半分辨率 pass；历史尺寸必须与其输出附件一致。
            EnsureAOHistoryTexture(false, mobileAOHistoryW, mobileAOHistoryH);
            PrepareAOHistoryForRead(commandBuffer, g_GameAOHistory, g_GameAOHistoryNeedsClear);
        }
        bool mobileTaaHistoryValid = true;
        const glm::vec2 previousMobileTaaJitter = g_PreviousTAAJitterGame;
        if (mobileTaaEnabled) {
            EnsureTAAHistoryTexture(g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight());
            mobileTaaHistoryValid = !g_TAAHistoryNeedsClear;
            // 当前链执行前，把上一帧 TAA 输出复制到常驻历史纹理；首帧自动 clear。
            PrepareTAAHistoryForRead(commandBuffer, g_GameTAAHistory,
                                     g_SwapChain.GetPassOutputImage("taa"));
        }
        {
            PostProcessChain::ExternalInputs ext;
            ext.compositeView = g_GameRenderTarget.GetCompositeImageView();
            ext.depthView = g_GameRenderTarget.GetDepthImageView();
            ext.gbufferView = g_GameRenderTarget.GetColorImageView(0);
            ext.gbuffer1View = g_GameRenderTarget.GetColorImageView(1);
            ext.gbuffer2View = g_GameRenderTarget.GetColorImageView(2);
            ext.gbufferMotionView = g_GameRenderTarget.GetColorImageView(3);
            ext.skyView = g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyImageView() : VK_NULL_HANDLE;
            ext.skySampler = g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkySampler() : VK_NULL_HANDLE;
            // Android 的 GTAO/TAA 历史与运动矢量由当前 GameRT/常驻纹理提供；其他未启用的时序 pass 保持空句柄。
            ext.historyView = mobileGtaoEnabled ? g_GameAOHistoryView : VK_NULL_HANDLE;
            ext.historySampler = mobileGtaoEnabled ? g_AOHistorySampler : VK_NULL_HANDLE;
            ext.ssgiHistoryView = VK_NULL_HANDLE; ext.ssgiHistorySampler = VK_NULL_HANDLE;
            ext.taaHistoryView = mobileTaaEnabled ? g_GameTAAHistoryView : VK_NULL_HANDLE;
            ext.taaHistorySampler = mobileTaaEnabled ? g_TAAHistorySampler : VK_NULL_HANDLE;
            ext.cmaaWeightView = VK_NULL_HANDLE; ext.cmaaWeightSampler = VK_NULL_HANDLE;
            ext.cameraUBO.cameraPos = glm::vec4(glm::vec3(glm::inverse(view)[3]), 1.0f);
            ext.cameraUBO.proj = proj;
            ext.cameraUBO.view = view;
            ext.cameraUBO.prevViewProj = s_PrevViewProj;
            ext.cameraUBO.invProj = glm::inverse(proj);
            ext.cameraUBO.invView = glm::inverse(view);
            // GTAO 的 CSM descriptor 必须有效；Android 当前只恢复 GTAO，不恢复 CSM 阴影图逐帧渲染，
            // 因此保留视图用于满足 binding 5，但关闭 CSM 依赖的体积光/遮挡分支。
            FillCsmIntoUExt(ext, csmGame0, 0, g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare));
            ext.cameraUBO.csmParams = glm::vec4(0.0f);
            static int s_mobileGtaoDiag = 0;
            if (mobileGtaoEnabled && s_mobileGtaoDiag < 3) {
                s_mobileGtaoDiag++;
                LOGI("[GTAO-DIAG] enabled=%d historyView=%p historySampler=%p csmView=%p csmDisabled=1",
                    mobileGtaoEnabled ? 1 : 0, (void*)ext.historyView, (void*)ext.historySampler,
                    (void*)ext.csmShadowView);
            }
            ext.pushData.cameraPos = ext.cameraUBO.cameraPos;
            ext.pushData.sunDir = glm::vec4(sunDir, 0.0f);
            ext.pushData.lightColor = glm::vec4(lightColor * lightIntensity, 1.0f);
            // frameInfo.yz = 上一帧 jitter - 当前帧 jitter（NDC），供 TAA
            // 把“无 jitter 运动矢量”映射回上一帧实际的采样位置；w=0 表示首帧历史无效。
            const glm::vec2 jitterDelta = previousMobileTaaJitter - g_CurrentTAAJitter;
            ext.pushData.frameInfo = glm::vec4(
                0.0f, jitterDelta.x, jitterDelta.y, mobileTaaHistoryValid ? 1.0f : 0.0f);
            g_SwapChain.Execute(commandBuffer, wd->Width, wd->Height, ext, g_CompositeFramebuffers[wd->FrameIndex]);
            if (mobileTaaEnabled) {
                // 当前帧 TAA 输出供下一帧重投影使用；TAA 位于 bloom 之前，因此取中间 pass 输出。
                CopyTAAHistory(commandBuffer, g_SwapChain.GetPassOutputImage("taa"), g_GameTAAHistory);
                g_PreviousTAAJitterGame = g_CurrentTAAJitter;
            }
            if (mobileGtaoEnabled) {
                CopyAOHistory(commandBuffer, g_SwapChain.GetPassOutputImage("gtao"), g_GameAOHistory,
                    mobileAOHistoryW, mobileAOHistoryH);
            }
        }
        // 链末 tonemap/FXAA 后叠加移动端 UI，不改变后处理结果。
        RenderUIOverlay(commandBuffer, wd->Width, wd->Height, g_CompositeUIPass, g_CompositeFramebuffers[wd->FrameIndex], true);
        s_PrevViewProj = proj * view;
        return;
    }
#endif
    // 2026-08-17 TAA：相机亚像素抖动（Halton 2,3）——几何/合成/pushData 全用 jittered 投影；UI 叠加保持原 proj
    g_CurrentTAAJitter = g_SwapChain.IsPassEnabled("taa") ? ComputeTAAJitter(g_TAAJitterFrameGame, (float)wd->Width, (float)wd->Height) : glm::vec2(0.0f);
    (void)proj;


    VkViewport viewport = {};
    viewport.x = 0.0f; viewport.y = 0.0f;
    viewport.width = (float)wd->Width; viewport.height = (float)wd->Height;
    viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {(uint32_t)wd->Width, (uint32_t)wd->Height};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    // 物理天空：全景天空图（独立 render pass，在合并 render pass 前；天空图可被 SSR fallback 采样）
    g_SkyboxRenderer.SyncFromScene();
    bool usePhysicalSky = false;
    // 场景方向光收集（函数级：天空 + 合成共用；无光源回退太阳方向/白光/1 强度，2026-08-10）
    glm::vec3 lightDir, lightColor(1.0f, 0.96f, 0.89f); float lightIntensity = 1.0f;   // 2026-08-12：回退色改近似太阳（~5778K 暖白）；场景有方向光时被覆盖
    glm::vec3 sunDir = g_AtmosphereRenderer.GetSunDirection();
    if (GetSceneDirectionalLight(lightDir, lightColor, lightIntensity)) sunDir = lightDir;
    if (g_SkyboxRenderer.IsEnabled() && g_AtmosphereEnabled && g_AtmosphereRenderer.IsInitialized()) {
        usePhysicalSky = true;
        g_AtmosphereRenderer.RenderSkyRT(commandBuffer, sunDir, glm::vec3(glm::inverse(view)[3]));   // 海拔=max(0, 相机y+200)（skyRT 随相机高度实时变化）
    }
    // 2026-08-13：cluster 光源剔除（游戏相机）——合成 pass 前 dispatch（CPU AABB + GPU cull）
    DispatchClusterCull(commandBuffer, g_GameCluster, view, proj,
                        (float)g_GameRenderTarget.GetWidth(), (float)g_GameRenderTarget.GetHeight());

    // 2026-08-14：CSM 方向光阴影（游戏模式槽 0）——主 render pass 前渲染 + barrier；descriptor 每帧刷新（slot 0）
    CascadeShadowRenderer* csmGame0 = g_SceneRenderer.EnsureCascadeShadows();
    g_GameCompositeQuad.UpdateDescriptorSet(g_GameRenderTarget.GetColorImageView(), g_GameRenderTarget.GetDepthImageView(), g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyImageView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkySampler() : VK_NULL_HANDLE, g_GameRenderTarget.GetColorImageView(1), g_GameRenderTarget.GetColorImageView(2), (g_TexturePool->GetTexture("end_sky")) ? g_TexturePool->GetTexture("end_sky")->imageView : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetTransmittanceView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetScatteringView() : VK_NULL_HANDLE, (g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyCubeView() : VK_NULL_HANDLE), g_TexturePool->GetSamplerByType(SamplerType::Linear), (g_TexturePool->GetTexture("sky_hdr_irr")) ? g_TexturePool->GetTexture("sky_hdr_irr")->imageView : VK_NULL_HANDLE, g_TexturePool->GetSamplerByType(SamplerType::Linear), GetShIrradianceBuffer(), UpdatePointLightBuffer(), GetGameClusterGridBuffer(),
        (g_SceneRenderer.EnsurePointShadows() && g_SceneRenderer.EnsurePointShadows()->IsInitialized()) ? g_SceneRenderer.EnsurePointShadows()->GetCubeArrayView() : VK_NULL_HANDLE,
        (csmGame0 && csmGame0->IsInitialized()) ? csmGame0->GetArrayView(0) : VK_NULL_HANDLE,
        g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare),   // 2026-08-15：阴影比较采样器（硬件 PCF，HSPE 同款）
        (csmGame0 && csmGame0->IsInitialized()) ? csmGame0->GetCascadeBuffer(0, (int)g_MainWindowData.FrameIndex) : VK_NULL_HANDLE,
        g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutView() : VK_NULL_HANDLE,   // 2026-08-15：split-sum BRDF LUT
        g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutSampler() : VK_NULL_HANDLE);
    csmGame0->SetFrameIndex((int)g_MainWindowData.FrameIndex);   // per-frame UBO 双缓冲（帧竞争修复）
    g_SceneRenderer.RenderCascadeShadowMaps(commandBuffer, 0, view, proj, sunDir);

    // 几何 subpass 0：GameRT（与编辑器 GameView 同一路径）——几何/2D/UI → G-Buffer
    g_GameRenderTarget.BeginRender(commandBuffer);
    // z-prepass（subpass 0，depth-only）——g_EnableZPrepass 开关（2026-08-10 GPU 对比验证）
    if (g_EnableZPrepass) {
        g_SceneRenderer.RenderDepthPrepass(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(), view, proj);
    }
    g_GameRenderTarget.NextSubpass(commandBuffer);   // → subpass 1（几何/2D/UI）
    RenderGameContent(commandBuffer, view, proj, usePhysicalSky);

    // 合成 subpass 2：几何写 G-Buffer → 切换 subpass → input attachment 合成 → 中间附件
    g_GameRenderTarget.NextSubpass(commandBuffer);
    g_GameCompositeQuad.Render(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
        glm::inverse(proj * view), glm::vec3(glm::inverse(view)[3]), sunDir, proj, view);
    g_GameRenderTarget.EndRender(commandBuffer);

    // 后处理链（配置驱动）：GameRT composite → 链逐 pass → swapchain（游戏模式主输出）
    // 注：游戏模式无 GameView 面板，不执行 Game 链（GameRT 显示附件无人消费）；
    //     编辑器 GameView 面板由 RenderGameToTarget 的 Game 链保证（EditorDllApi/EngineMain 的 GetDisplayDescriptorSet）
    CompositeToFinalBarrier(commandBuffer, g_GameRenderTarget.GetCompositeImage());
    // ⚠️ 2026-08-17：历史 copy 源/尺寸必须匹配"实际执行的链"——
    // 编辑器（g_EditorActive）Composite 分支执行 Game 链（输出 GameRT 尺寸）→ 源 = Game 链、尺寸 = GameRT；
    // 无编辑器（headless/纯游戏）执行 Swap 链（输出窗口尺寸）→ 源 = Swap 链、尺寸 = wd。
    // 曾固定用 Swap 链源 + GameRT 尺寸 → 编辑器模式 copy 自从不执行的 Swap 链 taa 输出（垃圾历史 → 抖动）
    const uint32_t activeWidth = g_EditorActive
        ? g_GameRenderTarget.GetWidth()
        : ((wd->Width > 0) ? static_cast<uint32_t>(wd->Width) : g_GameRenderTarget.GetWidth());
    const uint32_t activeHeight = g_EditorActive
        ? g_GameRenderTarget.GetHeight()
        : ((wd->Height > 0) ? static_cast<uint32_t>(wd->Height) : g_GameRenderTarget.GetHeight());
    const uint32_t activeHistoryW = std::max(1u, activeWidth / 2);
    const uint32_t activeHistoryH = std::max(1u, activeHeight / 2);
    const bool activeGtaoEnabled = g_EditorActive
        ? g_GameChain.IsPassEnabled("gtao")
        : g_SwapChain.IsPassEnabled("gtao");
    const bool activeSsgiEnabled = g_EditorActive
        ? g_GameChain.IsPassEnabled("ssgi")
        : g_SwapChain.IsPassEnabled("ssgi");
    if (activeGtaoEnabled) {
        EnsureAOHistoryTexture(false, activeHistoryW, activeHistoryH);
        PrepareAOHistoryForRead(commandBuffer, g_GameAOHistory, g_GameAOHistoryNeedsClear);
    }
    if (activeSsgiEnabled) {
        EnsureSSGIHistoryTexture(false, activeHistoryW, activeHistoryH);
        PrepareSSGIHistoryForRead(commandBuffer, g_GameSSGIHistory, g_GameSSGIHistoryNeedsClear);
    }

    if (g_EditorActive) {
        EnsureTAAHistoryTexture(g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight());
        PrepareTAAHistoryForRead(commandBuffer, g_GameTAAHistory, g_GameChain.GetPassOutputImage("taa"));   // Game 链（编辑器游戏模式）
    } else {
        const uint32_t histW = (wd->Width > 0) ? (uint32_t)wd->Width : g_GameRenderTarget.GetWidth();
        const uint32_t histH = (wd->Height > 0) ? (uint32_t)wd->Height : g_GameRenderTarget.GetHeight();
        EnsureTAAHistoryTexture(histW, histH);
        PrepareTAAHistoryForRead(commandBuffer, g_GameTAAHistory, g_SwapChain.GetPassOutputImage("taa"));   // Swap 链（headless/纯游戏）
    }
    PostProcessChain::ExternalInputs ext;
    ext.compositeView = g_GameRenderTarget.GetCompositeImageView();
    ext.depthView = g_GameRenderTarget.GetDepthImageView();
    ext.gbuffer1View = g_GameRenderTarget.GetColorImageView(1);   // 2026-08-13 GTAO 法线附件
    ext.gbuffer2View = g_GameRenderTarget.GetColorImageView(2);   // 2026-08-13 材质附件（emissive 强度）
    ext.gbufferView = g_GameRenderTarget.GetColorImageView(0);   // gbuffer0（gtao_apply 重建 emissive 用 albedo）
    ext.skyView = g_AtmosphereRenderer.GetSkyImageView();   // skyrt（gtao_apply 雾色）
    ext.skySampler = g_AtmosphereRenderer.GetSkySampler();
    ext.historyView = g_GameAOHistoryView;   // 时序 GTAO 历史
    ext.historySampler = g_AOHistorySampler;
    ext.ssgiHistoryView = g_GameSSGIHistoryView;   // 2026：时序 SSGI 历史
    ext.ssgiHistorySampler = g_SSGIHistorySampler;
    ext.taaHistoryView = g_GameTAAHistoryView;   // TAA：上帧输出历史
    ext.gbufferMotionView = g_GameRenderTarget.GetColorImageView(3);   // TAA depth-guided：运动向量附件
    ext.taaHistorySampler = g_TAAHistorySampler;
    // 2026-08-17：CMAA2 compute 已移到链内 tonemap pass 后（pass 后 hook，输入 LDR）——此处只取权重图
    {
        ext.cmaaWeightView = g_GameCMAA2.GetWeightView();
        ext.cmaaWeightSampler = g_GameCMAA2.GetWeightSampler();
    }
    ext.cameraUBO.cameraPos = glm::vec4(glm::vec3(glm::inverse(view)[3]), 1.0f);
    ext.cameraUBO.proj = proj;
    ext.cameraUBO.view = view;
    ext.cameraUBO.prevViewProj = s_PrevViewProj;
    ext.cameraUBO.invProj = glm::inverse(proj);
    ext.cameraUBO.invView = glm::inverse(view);
    FillCsmIntoUExt(ext, csmGame0, 0, g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare));
    ext.pushData.cameraPos = ext.cameraUBO.cameraPos;
    ext.pushData.sunDir = glm::vec4(sunDir, 0.0f);
    ext.pushData.lightColor = glm::vec4(lightColor * lightIntensity, 1.0f);
    ext.pushData.frameInfo = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    // ⚠️ 2026-08-17：仅当 GameView 面板未激活（g_ShowGameView=false）时才在此执行 Game 链；
    // 面板激活时 RenderGameToTarget 已渲染显示附件（含 UI 叠加）——重复执行会让同一帧跑两次 Game 链
    // （两个 jitter 序列 + 共享历史交叉污染 → 游戏模式 TAA 不稳定）
    if (g_EditorActive && !g_ShowGameView) {
        g_GameChain.Execute(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
            ext, g_GameRenderTarget.GetFinalFramebuffer());
        if (activeGtaoEnabled) {
            CopyAOHistory(commandBuffer, g_GameChain.GetPassOutputImage("gtao"), g_GameAOHistory,
                activeHistoryW, activeHistoryH);
        }
        if (activeSsgiEnabled) {
            CopySSGIHistory(commandBuffer, g_GameChain.GetPassOutputImage("ssgi"), g_GameSSGIHistory,
                activeHistoryW, activeHistoryH);
        }
        s_GameDisplayRendered = true;
        // 编辑器全屏游戏视图（EditorDllApi::RenderGameViewFullscreen 显示显示附件）：UI 叠加在链末 tonemap 结果之上
        RenderUIOverlay(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
            g_GameRenderTarget.GetDisplayUIRenderPass(), g_GameRenderTarget.GetFinalFramebuffer(), false);
    }
    // 强制唤醒：启动后直接进游戏模式且从未渲染过 GameView 时，GameRT 显示附件未初始化——
    // 补跑一次 Game 链填充它，再输出 swapchain
    if (g_EditorActive && !s_GameDisplayRendered) {
        g_GameChain.Execute(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
            ext, g_GameRenderTarget.GetFinalFramebuffer());
        if (activeGtaoEnabled) {
            CopyAOHistory(commandBuffer, g_GameChain.GetPassOutputImage("gtao"), g_GameAOHistory,
                activeHistoryW, activeHistoryH);
        }
        if (activeSsgiEnabled) {
            CopySSGIHistory(commandBuffer, g_GameChain.GetPassOutputImage("ssgi"), g_GameSSGIHistory,
                activeHistoryW, activeHistoryH);
        }
        s_GameDisplayRendered = true;
    }
    // 2026-08-17：编辑器内游戏模式——Swap 链后处理输出被 ImGui 清屏覆盖（画面来自 GameRT 显示附件的全屏游戏视图）——
    // 跳过 Swap 链省第二套 bloom+CMAA2+tonemap（trace 实测 ~1ms）；纯游戏/headless（无编辑器）Swap 链是唯一输出，必须执行
    if (!g_EditorActive) {
        g_SwapChain.Execute(commandBuffer, wd->Width, wd->Height, ext, g_CompositeFramebuffers[wd->FrameIndex]);
        if (activeGtaoEnabled) {
            CopyAOHistory(commandBuffer, g_SwapChain.GetPassOutputImage("gtao"), g_GameAOHistory,
                activeHistoryW, activeHistoryH);
        }
        if (activeSsgiEnabled) {
            CopySSGIHistory(commandBuffer, g_SwapChain.GetPassOutputImage("ssgi"), g_GameSSGIHistory,
                activeHistoryW, activeHistoryH);
        }
    }
    // UI 叠加（游戏模式 swapchain）：链末 tonemap 输出后，UI alpha 混合叠加在 swapchain 之上；
    // 仅无编辑器时（编辑器模式 swapchain 是 ImGui 界面，叠加游戏 UI 会错乱）
    if (!g_EditorActive) {
        RenderUIOverlay(commandBuffer, wd->Width, wd->Height, g_CompositeUIPass, g_CompositeFramebuffers[wd->FrameIndex], true);
    }

    // 生成 Hi-ZB（深度金字塔）——仅 3D 且存在体素时（无体素消费者则跳过，2026-08-10 省 GPU 34%）
    if (false) {   // 2026-08-12 用户：先跳过 Hi-Z 生成（暂时不需要）
        VkImage depthImage = g_GameRenderTarget.GetDepthImage();
        uint32_t mipLevels = g_SceneRenderer.GetHiZShader().GetMipLevels();
        if (depthImage != VK_NULL_HANDLE && mipLevels > 0)
            g_SceneRenderer.GetHiZShader().GenerateMipLevels(commandBuffer, depthImage, mipLevels);
    }
}

// 渲染场景到离屏目标（编辑器 SceneView）
// 注意：不在场景视图生成 Hi-ZB，因为 Voxel 剔除使用的是游戏相机视角
    // Hi-ZB 只在游戏视图渲染后生成（见 RenderGameToTarget）
    
    // 场景视图也使用完整的计算着色器处理
void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data, const glm::mat4& view, const glm::mat4& proj)
{
    // ===== 场景模式判定（每帧无条件，渲染分支判断之前；修复 2D→3D 切换后 g_SceneIs2D 卡 true）=====
    g_SceneRenderer.UpdateSceneMode();

    // ===== FrameRender 分阶段计时 =====
    auto t0 = std::chrono::high_resolution_clock::now();

    // 帧级标志：本帧是否真的执行了 GenerateMipLevels 写入 m_WriteBufferIndex（供尾部 SwapBuffers 判断）
    bool hiZGenerated = false;

    // Acquire next image
    VkSemaphore image_acquired_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;

    VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR) { g_SwapChainRebuild = true; return; }
    if (err == VK_SUBOPTIMAL_KHR)   { g_SwapChainRebuild = true; /* 继续使用：SUBOPTIMAL 的 image/semaphore 有效，完整走完 acquire→submit→present，避免生命周期断裂导致 semaphore/fence 状态错乱 */ }
    check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    check_vk_result(vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX));
    check_vk_result(vkResetFences(g_Device, 1, &fd->Fence));

    // ===== Shader 热更新 =====
    // 上一帧 fence 已等待（GPU 空闲），命令缓冲尚未开始录制，此处重建管线最安全。
    // 内部限频 0.5s：检测 glsl/spv 变化 -> 自动重编（可选）-> 重建全部已登记管线；失败保留旧管线。
    ShaderHotReload::GetInstance().Poll();

    check_vk_result(vkResetCommandPool(g_Device, fd->CommandPool, 0));

    VkCommandBufferBeginInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
    check_vk_result(err);

    
    // 设置视口和裁剪区域（提前设置，避免重复）
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)wd->Width;
    viewport.height = (float)wd->Height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    
    VkRect2D scissor = {};
    scissor.offset = { 0, 0 };
    scissor.extent = { (uint32_t)wd->Width, (uint32_t)wd->Height };
    
    // 帧计时（统计块引用；编辑器/游戏分支内赋值）
    
    // ===== 点光源阴影（2026-08-13）：每帧一次（光源为中心，与相机无关；Scene/Game 两个合成视口共用）=====
    // 列表由 UpdatePointLightBuffer 收集时同步填充（g_shadowLightList/g_shadowLightCount）——与 UBO 槽号同源
    if (g_shadowLightCount > 0) {
        g_SceneRenderer.RenderPointShadowMaps(fd->CommandBuffer, g_shadowLightList, g_shadowLightCount,
                                              PointShadowRenderer::SHADOW_MAP_SIZE);
    }

    // 根据模式选择渲染方式
    if (g_RunMode == RunMode::Editor)
    {
        // 编辑器模式：物理天空全景图（相机无关，每帧渲染一次，SceneView/GameView 共用）
        // 天空图写入后已插入 barrier，所有视口合成可采样
        g_SkyboxRenderer.SyncFromScene();
        if (g_SkyboxRenderer.IsEnabled() && g_AtmosphereEnabled && g_AtmosphereRenderer.IsInitialized()) {
            // 场景方向光（skyRT 只接受方向；无光源回退太阳方向）——2026-08-10
            glm::vec3 lightDir, lightColor(1.0f, 0.96f, 0.89f); float lightIntensity = 1.0f;   // 2026-08-12：回退色改近似太阳（~5778K 暖白）；场景有方向光时被覆盖
            glm::vec3 sunDir = g_AtmosphereRenderer.GetSunDirection();
            if (GetSceneDirectionalLight(lightDir, lightColor, lightIntensity)) sunDir = lightDir;
            g_AtmosphereRenderer.RenderSkyRT(fd->CommandBuffer, sunDir, glm::vec3(glm::inverse(view)[3]));   // 海拔=max(0, 相机y+200)
        }
        // 2026-08-13：cluster 光源剔除（编辑器 SceneView 相机）——SceneView 合成前 dispatch
        DispatchClusterCull(fd->CommandBuffer, g_SceneCluster, view, proj,
                            (float)g_SceneRenderTarget.GetWidth(), (float)g_SceneRenderTarget.GetHeight());

        // 只有场景视图窗口可见且真正可见时才渲染Scene View
        if (g_ShowSceneView) {
            RenderSceneToTarget(view, proj, wd->FrameIndex);
        }
        
        // 编辑器模式：始终生成游戏相机视角的 Hi-ZB（用于 Voxel 剔除）
        // 不管游戏视图是否可见，都需要 Hi-ZB 数据
        {
            glm::mat4 gameView, gameProj;
            glm::vec3 gameCameraPos;
            float aspectRatio = (float)g_GameRenderTarget.GetWidth() / (float)g_GameRenderTarget.GetHeight();
            bool hasGameCamera = g_SceneRenderer.GetMainCameraMatrices(aspectRatio, gameView, gameProj, gameCameraPos);
            
            // 无主 3D 相机(纯 2D 场景如 snake.json): 用编辑器相机矩阵占位,仍渲染游戏视图
            // (2D 世界层/UI 渲染不依赖 3D view/proj;3D 场景下无相机则渲染空场景)
            if (!hasGameCamera) {
                gameView = view;
                gameProj = proj;
                gameCameraPos = g_Camera.Position;
                hasGameCamera = true;
            }
            
            if (hasGameCamera) {
                glm::vec3 cameraFront = -glm::vec3(gameView[0][2], gameView[1][2], gameView[2][2]);
                glm::vec3 cameraRight = glm::vec3(gameView[0][0], gameView[1][0], gameView[2][0]);
                glm::vec3 cameraUp = -glm::vec3(gameView[0][1], gameView[1][1], gameView[2][1]);
                
                // 仅当游戏视图为激活标签页时才渲染（含 Hi-Z）；后台/未激活标签零渲染
                if (g_ShowGameView) {
                    RenderGameToTarget(gameView, gameProj, gameCameraPos, cameraFront, cameraRight, cameraUp, wd->FrameIndex);
                    hiZGenerated = true;
                }
                // 注：未激活时不再生成 Hi-ZB（避免后台渲染消耗 GPU）；
                //     世界剔除使用 mainCameraFrustumPlanes（CPU 视锥），不依赖 Hi-Z
            }
        }
        
        // 开始主渲染通道（只切换一次）
        VkRenderPassBeginInfo renderPassInfo = {};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = wd->RenderPass;
        renderPassInfo.framebuffer = fd->Framebuffer;
        renderPassInfo.renderArea.extent.width = wd->Width;
        renderPassInfo.renderArea.extent.height = wd->Height;
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        
        vkCmdSetViewport(fd->CommandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(fd->CommandBuffer, 0, 1, &scissor);
        
        // 渲染ImGui（draw_data 为 null 时跳过：非编辑器模式无 ImGui）
        if (draw_data && !(draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f))
        {
            ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);
        }
        
        vkCmdEndRenderPass(fd->CommandBuffer);
    }
    else
    {
        // 游戏模式：获取游戏摄像机矩阵
        glm::mat4 gameView, gameProj;
        glm::vec3 gameCameraPos;
        float aspectRatio = (float)g_SceneRenderTarget.GetWidth() / (float)g_SceneRenderTarget.GetHeight();
        bool hasGameCamera = g_SceneRenderer.GetMainCameraMatrices(aspectRatio, gameView, gameProj, gameCameraPos);

        // 无主 3D 相机(纯 2D 场景): 用编辑器相机矩阵占位,仍渲染游戏画面
        if (!hasGameCamera) {
            gameView = view;
            gameProj = proj;
            gameCameraPos = g_Camera.Position;
            hasGameCamera = true;
        }

        // 合并 render pass：subpass 0 几何/2D/UI → G-Buffer，subpass 1 全屏四边形（input attachment）→ swapchain
        RenderGameComposite(gameView, gameProj, wd->FrameIndex);
        hiZGenerated = true;

        // 若仍存在 ImGui draw data（理论上游戏模式无编辑器，防御处理）：用独立 pass 叠加到合成结果之上
        if (draw_data && !(draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f))
        {
            VkRenderPassBeginInfo imguiPass = {};
            imguiPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            imguiPass.renderPass = wd->RenderPass;
            imguiPass.framebuffer = fd->Framebuffer;
            imguiPass.renderArea.extent.width = wd->Width;
            imguiPass.renderArea.extent.height = wd->Height;
            imguiPass.clearValueCount = 1;
            imguiPass.pClearValues = &wd->ClearValue;
            vkCmdBeginRenderPass(fd->CommandBuffer, &imguiPass, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(fd->CommandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(fd->CommandBuffer, 0, 1, &scissor);
            ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);
            vkCmdEndRenderPass(fd->CommandBuffer);
        }
    }
    
    // 每帧结束时交换 Hi-Z 双缓冲区：仅当本帧确实写入了 m_WriteBufferIndex（即执行了 GenerateMipLevels）才切换。
    // 编辑器模式若 !g_ShowGameView（不渲染游戏视图/不生成 Hi-Z），SwapBuffers 空切会把读槽在两个陈旧 buffer 间来回切换，
    // 导致 Voxel GPU 剔除读到的 Hi-Z 在"上一次有效数据 / 上上次有效数据"间抖动；不切则读槽固定指向"最后一次写入的 buffer"，语义更稳定。
    if (hiZGenerated && g_SceneRenderer.IsHiZCullingEnabled() && g_SceneRenderer.GetHiZShader().IsInitialized()) {
        g_SceneRenderer.GetHiZShader().SwapBuffers();
    }
    
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &image_acquired_semaphore;
    submitInfo.pWaitDstStageMask = &wait_stage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &fd->CommandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &render_complete_semaphore;

    err = vkEndCommandBuffer(fd->CommandBuffer);
    check_vk_result(err);
    err = vkQueueSubmit(g_Queue, 1, &submitInfo, fd->Fence);
    check_vk_result(err);
}

// 呈现一帧
void FramePresent(ImGui_ImplVulkanH_Window* wd)
{
    // 暂停状态下不执行呈现
    if (g_IsPaused)
        return;
    
    // 交换链重建中不执行呈现
    if (g_SwapChainRebuild)
        return;
    
    // 获取渲染完成信号量
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    
    // 呈现信息
    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
     
    // 呈现到屏幕
    VkResult err = vkQueuePresentKHR(g_Queue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        g_SwapChainRebuild = true;  // 全平台：OUT_OF_DATE 必须重建（Android 原被排除会导致死循环）
    // 注：桌面平台 VK_SUBOPTIMAL_KHR 不再单独触发 rebuild。
    //   resize 导致的 suboptimal 会由 SDL_EVENT_WINDOW_RESIZED 事件提前设置 g_SwapChainRebuild（EngineMain.cpp:L441），
    //   两者时序重合且连续多帧 suboptimal 叠加会触发重复重建抖动；仅保留 OUT_OF_DATE 硬错误 + RESIZED 事件触发，减少 resize 期间多次重建。
    //   HDR toggle 等非尺寸触发的 SUBOPTIMAL 仍可容忍使用（直到下一帧尺寸变化或下一次 OUT_OF_DATE）。
    #ifndef __ANDROID__
    // 旧: if (err == VK_SUBOPTIMAL_KHR) g_SwapChainRebuild = true;
    #endif
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err == VK_ERROR_DEVICE_LOST) {
        // 设备丢失，需要重建交换链
        g_SwapChainRebuild = true;
        return;
    }
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);
    
    // 更新信号量索引
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

uint32_t GetCurrentFrameIndex()
{
    return g_MainWindowData.FrameIndex;
}

// 设置 Vulkan 窗口
void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height)
{
    wd->Surface = surface;

    // 检查物理设备是否支持窗口表面
    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, wd->Surface, &res);
    if (res != VK_TRUE)
    {
        fprintf(stderr, "Error no WSI support on physical device\n");
        exit(-1);
    }

    // 获取表面能力，确保 minImageCount 符合要求
    VkSurfaceCapabilitiesKHR cap;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_PhysicalDevice, wd->Surface, &cap);
    
    // Android 平台需要至少 3 个图像，桌面平台可以是 2 个
    #ifdef __ANDROID__
    g_MinImageCount = std::max(cap.minImageCount, (uint32_t)3);  // Android 至少需要 3 个
    #else
    g_MinImageCount = std::max(cap.minImageCount, (uint32_t)2);  // 桌面至少需要 2 个
    #endif
    
    // 如果启用了三重缓冲，需要更多图像
    if (g_TripleBufferingEnabled) {
        g_MinImageCount = std::max(g_MinImageCount, (uint32_t)3);
    }

    // 选择表面格式
    const VkFormat requestSurfaceImageFormat[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat, (size_t)IM_ARRAYSIZE(requestSurfaceImageFormat), requestSurfaceColorSpace);

    // 选择呈现模式：根据g_VSyncEnabled状态选择
    if (g_VSyncEnabled) {
        // 启用垂直同步：使用FIFO模式
        wd->PresentMode = VK_PRESENT_MODE_FIFO_KHR;
    } else {
        // 禁用垂直同步：Android 使用 MAILBOX，桌面使用 IMMEDIATE
        #ifdef __ANDROID__
        wd->PresentMode = VK_PRESENT_MODE_MAILBOX_KHR;  // Android 首选低延迟模式
        #else
        wd->PresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        #endif
    }
    
    // 创建或调整窗口大小
    ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, width, height, g_MinImageCount, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
}
