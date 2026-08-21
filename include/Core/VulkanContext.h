#pragma once
// VulkanContext.h - Vulkan core objects and swapchain state (P0 refactor: split from EngineGlobal.h)
#include <vulkan/vulkan.h>
#include "imgui_impl_vulkan.h"
#include "Platform/Export.h"
#include <glm/glm.hpp>   // 2026-08-17：g_CurrentTAAJitter（vec2）

extern MIKAN_API VkAllocationCallbacks*   g_Allocator;
extern MIKAN_API VkInstance               g_Instance;
extern MIKAN_API VkPhysicalDevice         g_PhysicalDevice;
extern MIKAN_API VkDevice                 g_Device;
extern MIKAN_API uint32_t                 g_QueueFamily;
extern MIKAN_API VkQueue                  g_Queue;
extern MIKAN_API VkDescriptorPool         g_DescriptorPool;
extern MIKAN_API VkCommandPool            g_CommandPool;

// Window / swapchain
extern MIKAN_API ImGui_ImplVulkanH_Window g_MainWindowData;
extern MIKAN_API uint32_t                 g_MinImageCount;
extern MIKAN_API bool                     g_SwapChainRebuild;
extern MIKAN_API bool                     g_VSyncEnabled;
extern MIKAN_API bool                     g_TripleBufferingEnabled;
extern MIKAN_API int                      g_FullscreenMode;   // 2026-08-17：0=窗口 1=桌面全屏 2=独占全屏
extern MIKAN_API glm::vec2                g_CurrentTAAJitter;  // 2026-08-17：当前渲染视图 TAA 亚像素抖动（NDC 偏移；ModelRenderer 用）

// Vulkan helpers (declared here so dependents do not need VulkanManager.h)
extern void check_vk_result(VkResult err);
extern void SetVSync(bool enabled);
extern void SetTripleBuffering(bool enabled);
extern void SetFullscreenMode(int mode);
extern void RecreateSwapChain(int width, int height);