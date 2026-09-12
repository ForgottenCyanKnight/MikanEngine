// Vulkan Manager Header
// 声明与Vulkan相关的函数

#ifndef VULKAN_MANAGER_H
#define VULKAN_MANAGER_H

#include "Platform/Export.h"
#include "Core/VulkanCompositeLifecycle.h"
#include "Core/VulkanCompositeResources.h"
#include "Core/VulkanSwapchainLifecycle.h"
#include "Core/VulkanShutdown.h"
#include <vulkan/vulkan.h>
#include "imgui_impl_vulkan.h"
#include <glm/glm.hpp>
#include <cstdint>

// 函数声明
extern void check_vk_result(VkResult err);
extern void SetupVulkan(ImVector<const char*> instance_extensions);
extern void SetVSync(bool enabled);
extern void SetTripleBuffering(bool enabled);
// 请求在下一帧已等待 fence 的安全点重建后处理链。用于游戏内实时画质设置。
extern void RequestPostProcessRebuild();
extern void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height);
extern void CleanupVulkanWindow();
extern void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data,
                        const glm::mat4& view, const glm::mat4& proj,
                        float deltaSeconds = 1.0f / 60.0f);
extern void FramePresent(ImGui_ImplVulkanH_Window* wd);
// 交换链创建时根据 surface 能力选择是否加入 transfer-src，用于最终画面截图。
extern VkImageUsageFlags GetSwapchainImageUsage();
// 启动阶段加载页：在场景尚未创建前直接绘制到 swapchain，避免长时间黑屏。
// progress 为 [0, 1] 的阶段进度，status 仅用于显示当前阶段文本。
extern void SetLoadingScreenState(bool active, float progress, const char* status);
// 启动 Logo 阶段：opacity 从 1 递减到 0 时渐隐到黑色，结束后再进入加载页。
extern void SetStartupSplashState(bool active, float opacity);
extern uint32_t GetCurrentFrameIndex();
// Monotonic recording epoch. Unlike the swapchain image index, this changes
// on every FrameRender and lets dynamic upload arenas reset only after the
// corresponding frame fence has been waited.
extern uint64_t GetCurrentFrameSerial();

// 全局 Vulkan 变量声明
extern MIKAN_API VkAllocationCallbacks* g_Allocator;
extern MIKAN_API VkInstance g_Instance;
extern MIKAN_API ImGui_ImplVulkanH_Window g_MainWindowData;

#endif // VULKAN_MANAGER_H
