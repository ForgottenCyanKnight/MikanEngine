// Vulkan Manager Header
// 声明与Vulkan相关的函数

#ifndef VULKAN_MANAGER_H
#define VULKAN_MANAGER_H

#include <vulkan/vulkan.h>
#include "imgui_impl_vulkan.h"
#include <glm/glm.hpp>

// 函数声明
extern void check_vk_result(VkResult err);
extern void SetupVulkan(ImVector<const char*> instance_extensions);
extern void RecreateSwapChain(int width, int height);
extern void SetVSync(bool enabled);
extern void SetTripleBuffering(bool enabled);
extern void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height);
extern void CleanupVulkan();
extern void CleanupVulkanWindow();
// 合并 render pass（游戏模式 subpass 合成）资源：首次初始化与 swapchain 重建共用
// 必须在 RenderTarget 初始化之后调用（依赖其附件格式/视图）
extern void InitCompositeResources();
extern void DestroyCompositeResources();
// 更新合成描述符（颜色0/深度 input + 天空 RT sampler）；AtmosphereRenderer 初始化后调用
extern void UpdateFullscreenQuadDescriptors();
extern void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data, const glm::mat4& view, const glm::mat4& proj);
extern void FramePresent(ImGui_ImplVulkanH_Window* wd);
extern uint32_t GetCurrentFrameIndex();

// 全局 Vulkan 变量声明
extern MIKAN_API VkAllocationCallbacks* g_Allocator;
extern MIKAN_API VkInstance g_Instance;
extern MIKAN_API ImGui_ImplVulkanH_Window g_MainWindowData;

#endif // VULKAN_MANAGER_H