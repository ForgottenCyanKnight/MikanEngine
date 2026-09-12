// Swapchain recreation coordinator.

#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "Core/VulkanSwapchainLifecycle.h"

#include "Core/EngineGlobal.h"
#include "Core/Log.h"
#include "Core/RenderGlobals.h"
#include "Core/VulkanCompositeLifecycle.h"
#include "Core/VulkanCompositeResources.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanFrameLoop.h"
#include "Core/VulkanPostProcessHistory.h"
#include "Core/VulkanRuntimeResources.h"
#include "AtmosphereRenderer.h"
#include "FullscreenQuad.h"
#include "Rendering/CMAA2.h"
#include "Rendering/PostProcessChain.h"
#include "Rendering/RenderTarget.h"
#include "Rendering/Renderer2D.h"
#include "Rendering/SceneRenderer.h"
#include "Rendering/SkyboxRenderer.h"

extern VkImageUsageFlags GetSwapchainImageUsage();
extern FullscreenQuad g_SceneCompositeQuad;
extern FullscreenQuad g_GameCompositeQuad;
extern CMAA2 g_SceneCMAA2;
extern CMAA2 g_GameCMAA2;
extern CMAA2 g_SwapCMAA2;
extern AtmosphereRenderer g_AtmosphereRenderer;

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
    // The old fence handle belongs to the swapchain frame array that is about
    // to be destroyed/recreated. The device is idle here, so it is safe to
    // forget the handle before those frame objects are replaced.
    ResetFrameLoopSynchronizationState();
    // 粒子渲染器可能同时持有 SceneView/GameView/swapchain 的多套 UI 管线；
    // 交换链与离屏 render pass 销毁前，在 GPU 空闲点统一释放它们。
    CleanupParticleRenderers();

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
    g_SwapCMAA2.Cleanup();
    CleanupAOAndSSGIHistoryTextures();
    if (g_SceneTAAHistory) {
        vkDestroyImageView(g_Device, g_SceneTAAHistoryView, g_Allocator);
        vkDestroyImage(g_Device, g_SceneTAAHistory, g_Allocator);
        vkFreeMemory(g_Device, g_SceneTAAHistoryMem, g_Allocator);
        vkDestroyImageView(g_Device, g_GameTAAHistoryView, g_Allocator);
        vkDestroyImage(g_Device, g_GameTAAHistory, g_Allocator);
        vkFreeMemory(g_Device, g_GameTAAHistoryMem, g_Allocator);
        g_SceneTAAHistory = g_GameTAAHistory = VK_NULL_HANDLE;
    }
    if (g_TAAHistorySampler) {
        vkDestroySampler(g_Device, g_TAAHistorySampler, g_Allocator);
        g_TAAHistorySampler = VK_NULL_HANDLE;
    }
    g_GameCompositeQuad.Cleanup();
    g_SceneChain.Cleanup();
    g_GameChain.Cleanup();
    g_SwapChain.Cleanup();
#ifndef __ANDROID__
    g_AtmosphereRenderer.Cleanup();
#else
    // Android：重建保留大气渲染器（skyRT 固定 128x64，不依赖窗口尺寸）——二次 Init 的 LUT Generate 在 Adreno 上
#endif
    DestroyCompositeResources();

    // 创建或调整窗口大小
    LOGI("Calling ImGui_ImplVulkanH_CreateOrResizeWindow with new surface...");
    ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd,
        g_QueueFamily, g_Allocator, width, height, g_MinImageCount,
        GetSwapchainImageUsage());
    LOGI("SwapChain recreated successfully");

    // 初始化离屏渲染目标 (使用窗口大小)
    g_SceneRenderTarget.Init(width, height, true); // 启用MRT
    g_GameRenderTarget.Init(width, height, true); // 启用MRT

    // 创建ImGui描述符集 (必须在ImGui_ImplVulkan_Init之后)
    g_SceneRenderTarget.CreateImGuiDescriptorSet();
    g_GameRenderTarget.CreateImGuiDescriptorSet();

    // 重新初始化场景渲染器 (使用离屏渲染目标的RenderPass)
    g_SceneRenderer.Init(g_SceneRenderTarget.GetRenderPass());
    g_SceneRenderer.EnsurePointShadows();
    g_SkyboxRenderer.Init(g_SceneRenderTarget.GetRenderPass());

    // Rebuild model GPU resources right away: Cleanup() above cleared m_ModelRenderers
    // and Init() does not reload models. Without this, the lazy load inside the render
    // frame can fail silently and models stay invisible until the next swapchain rebuild.
    g_SceneRenderer.PreloadModels();

    // MRT geometry render pass + separate composite render pass；合成 quad 通过纹理采样读取 G-Buffer。
    InitCompositeResources();
    // 物理天空随窗口重建（天空 RT 为 1/4 分辨率）
    // 二次 Init 的 LUT Generate 在 Adreno 上 vkQueueSubmit 返回 DEVICE_LOST（首次已成功生成，重建无需重算）
    if (!g_AtmosphereRenderer.IsInitialized()) {
        g_AtmosphereRenderer.Init(width, height);
    }
    // 更新合成描述符：绑定真实天空 RT
    UpdateFullscreenQuadDescriptors();
    // 2D 渲染核心（离屏世界层 + 主窗口 UI 层；重建时自动重建双管线）
    Renderer2D::GetInstance().Init(g_GameRenderTarget.GetRenderPass(), wd->RenderPass,
        g_GameRenderTarget.GetDisplayUIRenderPass(), g_CompositeUIPass);

    // 编辑器模式需要更新UI管理器中的描述符集
    if (g_RunMode == RunMode::Editor) {
        // Scene/Game view descriptors are forwarded to the editor at attach time (Editor.dll)
    }
}
