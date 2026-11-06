#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "Core/VulkanManager.h"
#include "Core/VulkanPresentation.h"
#include "Core/VulkanContext.h"
#include "Core/Log.h"
#include "Core/ScreenshotCapture.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL.h>

namespace {
const char* PresentModeName(VkPresentModeKHR mode){
    switch(mode){case VK_PRESENT_MODE_IMMEDIATE_KHR:return "immediate";case VK_PRESENT_MODE_MAILBOX_KHR:return "mailbox";
        case VK_PRESENT_MODE_FIFO_KHR:return "fifo";case VK_PRESENT_MODE_FIFO_RELAXED_KHR:return "fifo_relaxed";default:return "other";}
}
VkPresentModeKHR SelectWindowPresentMode(VkSurfaceKHR surface,bool vsync){
    VkPresentModeKHR preferred=vsync?VK_PRESENT_MODE_FIFO_KHR:
#ifdef __ANDROID__
        VK_PRESENT_MODE_MAILBOX_KHR;
#else
        VK_PRESENT_MODE_IMMEDIATE_KHR;
#endif
    if(const char* option=std::getenv("MIKAN_VK_PRESENT_MODE");option&&*option){
        if(std::strcmp(option,"immediate")==0)preferred=VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if(std::strcmp(option,"mailbox")==0)preferred=VK_PRESENT_MODE_MAILBOX_KHR;
        else if(std::strcmp(option,"fifo")==0)preferred=VK_PRESENT_MODE_FIFO_KHR;
        else if(std::strcmp(option,"fifo_relaxed")==0)preferred=VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        else LOGW("[Presentation] unknown mode override: %s",option);
    }
    const VkPresentModeKHR choices[]={preferred,VK_PRESENT_MODE_FIFO_KHR};
    const auto selected=ImGui_ImplVulkanH_SelectPresentMode(g_PhysicalDevice,surface,choices,2);
    LOGI("[Presentation] requested=%s selected=%s vsync=%u",PresentModeName(preferred),PresentModeName(selected),uint32_t(vsync));
    return selected;
}
uint32_t SelectWindowImageCount(VkSurfaceKHR surface){
    VkSurfaceCapabilitiesKHR cap{};check_vk_result(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_PhysicalDevice,surface,&cap));
    uint32_t count=g_TripleBufferingEnabled?3u:2u;
#ifdef __ANDROID__
    count=std::max(count,3u);
#endif
    if(const char* option=std::getenv("MIKAN_VK_IMAGE_COUNT");option&&*option){const int requested=std::atoi(option);if(requested>=2&&requested<=8)count=uint32_t(requested);}
    count=std::max(count,cap.minImageCount);if(cap.maxImageCount)count=std::min(count,cap.maxImageCount);return count;
}
}

// Swapchain screenshots use a copy recorded into the same frame command buffer.
// Keep the usage decision in one place so initial creation and resize follow the
// same surface capability rule.
VkImageUsageFlags GetSwapchainImageUsage()
{
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (g_PhysicalDevice == VK_NULL_HANDLE || g_MainWindowData.Surface == VK_NULL_HANDLE) {
        Core::ScreenshotCapture::GetInstance().SetSwapchainTransferSupported(false);
        return usage;
    }

    VkSurfaceCapabilitiesKHR capabilities{};
    const VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        g_PhysicalDevice, g_MainWindowData.Surface, &capabilities);
    if (result != VK_SUCCESS) {
        LOGW("[Screenshot] cannot query surface usage flags: %d", static_cast<int>(result));
        Core::ScreenshotCapture::GetInstance().SetSwapchainTransferSupported(false);
        return usage;
    }

    const bool supported = (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    Core::ScreenshotCapture::GetInstance().SetSwapchainTransferSupported(supported);
    if (supported) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    } else {
        LOGW("[Screenshot] surface does not support VK_IMAGE_USAGE_TRANSFER_SRC_BIT; PNG capture disabled");
    }
    return usage;
}

// 清理Vulkan窗口
void CleanupVulkanWindow()
{
    Core::ShutdownPresentationQueue();
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
}

// 设置垂直同步
void SetVSync(bool enabled)
{
    if (g_VSyncEnabled == enabled)
        return;

    g_VSyncEnabled = enabled;

    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    wd->PresentMode = SelectWindowPresentMode(wd->Surface,enabled);
    g_MinImageCount=SelectWindowImageCount(wd->Surface);
    // 设置交换链重建标志，让主循环在适当的时候重建交换链
    g_SwapChainRebuild = true;
}

// 设置三重缓冲
void SetTripleBuffering(bool enabled)
{
    if (g_TripleBufferingEnabled == enabled)
        return;

    g_TripleBufferingEnabled = enabled;

    g_MinImageCount=SelectWindowImageCount(g_MainWindowData.Surface);
    // 设置交换链重建标志，让主循环在适当的时候重建交换链
    g_SwapChainRebuild = true;
}

// 独占全屏绕开 DWM 合成（窗口模式 present 平台税 0.6-0.9ms → ~0.05ms）
// SDL3：SetWindowFullscreenMode(mode) —— NULL=桌面无边框；非 NULL=独占；SetWindowFullscreen(bool) 应用
extern SDL_Window* window;   // EngineGlobals.cpp 全局窗口

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
        if (!pick) {
            LOGW("[Fullscreen] WARN: 未获取到独占全屏 mode（count=%d），回滚到模式 %d", count, prevMode);
            std::fflush(stdout);
            g_FullscreenMode = prevMode;
            return;
        }
        SDL_SetWindowFullscreenMode(window, pick);
        SDL_SetWindowFullscreen(window, true);
        // 验证独占生效：窗口 fullscreen mode 应非 NULL（NULL=无边框）
        const SDL_DisplayMode* applied = SDL_GetWindowFullscreenMode(window);
        LOGI("[Fullscreen] exclusive applied: %dx%d@%dHz (mode=%p)",
            applied ? applied->w : 0, applied ? applied->h : 0,
            applied ? applied->refresh_rate : 0, (void*)applied);
        std::fflush(stdout);
    }
    // 不主动重建 swapchain：全屏切换后系统发 SDL_EVENT_WINDOW_RESIZED → 现有 resize 路径重建
}

// 设置 Vulkan 窗口
void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height)
{
    wd->Surface = surface;

    // 检查物理设备是否支持窗口表面
    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, wd->Surface, &res);
    if (res != VK_TRUE) {
        LOGE("Error no WSI support on physical device");
        std::exit(-1);
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
    const VkFormat requestSurfaceImageFormat[] = {
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM
    };
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat,
        (size_t)IM_ARRAYSIZE(requestSurfaceImageFormat), requestSurfaceColorSpace);

    wd->PresentMode=SelectWindowPresentMode(wd->Surface,g_VSyncEnabled);
    g_MinImageCount=SelectWindowImageCount(wd->Surface);
    // 创建或调整窗口大小
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator,
        width, height, g_MinImageCount, GetSwapchainImageUsage());
    LOGI("[Presentation] mode=%s requestedImages=%u actualImages=%u extent=%dx%d",PresentModeName(wd->PresentMode),g_MinImageCount,wd->ImageCount,wd->Width,wd->Height);
}
