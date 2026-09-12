#include "Core/VulkanManager.h"

#include "Core/RenderGlobals.h"
#include "Core/ScreenshotCapture.h"
#include "Core/VulkanContext.h"

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
    // 一次性截图只在确有记录时等待队列，避免影响普通帧；失败/过期呈现时
    // 仍由 EngineMain 清理阶段再次 Finalize，保证不会遗留 staging 资源。
    Core::ScreenshotCapture::GetInstance().Finalize();
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
