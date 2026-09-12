#pragma once
// VulkanContext.h - Vulkan core objects and swapchain state (P0 refactor: split from EngineGlobal.h)
#include <vulkan/vulkan.h>
#include "imgui_impl_vulkan.h"
#include "Platform/Export.h"
#include <glm/glm.hpp>
#include <vector>

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
extern MIKAN_API int                      g_FullscreenMode;
extern MIKAN_API glm::vec2                g_CurrentTAAJitter;

// Game-mode composite render passes and their per-swapchain framebuffers.
extern MIKAN_API VkRenderPass             g_CompositeRenderPass;
extern MIKAN_API VkRenderPass             g_CompositeUIPass;
extern MIKAN_API std::vector<VkFramebuffer> g_CompositeFramebuffers;

// Vulkan helpers (declared here so dependents do not need VulkanManager.h)
extern void check_vk_result(VkResult err);
extern void SetVSync(bool enabled);
extern void SetTripleBuffering(bool enabled);
extern void SetFullscreenMode(int mode);
extern void RecreateSwapChain(int width, int height);
