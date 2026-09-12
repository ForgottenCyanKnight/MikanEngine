#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "Core/VulkanFrameLoop.h"

#include "Core/EngineGlobal.h"
#include "Core/Log.h"
#include "Core/ProjectManager.h"
#include "Core/ScreenshotCapture.h"
#include "Core/VulkanFramePipeline.h"
#include "Core/VulkanLightingCulling.h"
#include "Core/VulkanManager.h"
#include "Core/VulkanPostProcessChains.h"
#include "Core/VulkanPostProcessHistory.h"
#include "Core/VulkanRenderHelpers.h"
#include "Core/VulkanStartupPasses.h"
#include "AtmosphereRenderer.h"
#include "Game/GameManager.h"
#include "Rendering/RenderTarget.h"
#include "Rendering/SceneRenderer.h"
#include "Rendering/SkyboxRenderer.h"
#include "Rendering/ShaderHotReload.h"
#include "Rendering/GpuSphSimulation.h"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>

extern AtmosphereRenderer g_AtmosphereRenderer;
extern bool g_AtmosphereEnabled;

// 全局帧计数器，用于蓝噪声时序抖动
static uint32_t g_frameCounter = 0;
// 每次 FrameRender 的命令录制 epoch。swapchain image index 可能连续重复，
// 所以不能拿 FrameIndex 判断“是否开始了新一帧”的动态上传。
static uint64_t g_renderFrameSerial = 0;

// SceneRT/GameRT and their post-process images are single-image resources,
// shared by all swapchain frames. Waiting only the fence belonging to the
// newly acquired swapchain image does not prove that the previous submission
// has stopped using those offscreen images. Keep the last submission fence and
// wait for it before recording the next frame.
static VkFence g_LastOffscreenFrameFence = VK_NULL_HANDLE;

// 临时 CPU/同步诊断：仅在 MIKAN_CPU_PROFILE 非空且不为 0 时启用。
// frame_wall_ms 包含 Acquire/Fence 等待；command_record_ms 只覆盖等待结束后的
// 命令录制阶段，用于区分“CPU 录制慢”和“CPU 在等 GPU”。
static bool IsVulkanCpuProfileEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MIKAN_CPU_PROFILE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

static uint64_t g_cpuProfileFrameCount = 0;
static double g_cpuProfileFrameWallMs = 0.0;
static double g_cpuProfileFenceWaitMs = 0.0;
static double g_cpuProfileCommandRecordMs = 0.0;


bool IsGpuSphProjectActive()
{
    auto* game = Game::GameManager::GetInstance().GetCurrent();
    return game != nullptr && game->GetName() != nullptr &&
           std::strcmp(game->GetName(), "sphfluidgpu") == 0;
}


void ResetFrameLoopSynchronizationState()
{
    g_LastOffscreenFrameFence = VK_NULL_HANDLE;
}

static void RebuildPostProcessChainsIfRequested()
{
    if (!g_PostProcessRebuildRequested || g_Device == VK_NULL_HANDLE) return;
    if (g_MainWindowData.Width == 0 || g_MainWindowData.Height == 0) return;

    g_PostProcessRebuildRequested = false;
    LOGI("[Graphics] rebuilding post-process chains for camera/settings");
    check_vk_result(vkDeviceWaitIdle(g_Device));

#ifdef __ANDROID__
    RebuildSelectedPostProcessChain(
        g_SwapChain, s_ActiveSwapPostProcessChainPath,
        s_RequestedSwapPostProcessChainPath, DefaultPostProcessChainPath(),
        g_MainWindowData.Width, g_MainWindowData.Height, g_CompositeRenderPass, "swapchain");
#else
    const std::string fallbackChain = DefaultPostProcessChainPath();
    RebuildSelectedPostProcessChain(
        g_SceneChain, s_ActiveScenePostProcessChainPath,
        s_RequestedScenePostProcessChainPath, fallbackChain,
        g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight(),
        g_SceneRenderTarget.GetFinalRenderPass(), "scene");
    RebuildSelectedPostProcessChain(
        g_GameChain, s_ActiveGamePostProcessChainPath,
        s_RequestedGamePostProcessChainPath, fallbackChain,
        g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
        g_GameRenderTarget.GetFinalRenderPass(), "game");
    RebuildSelectedPostProcessChain(
        g_SwapChain, s_ActiveSwapPostProcessChainPath,
        s_RequestedSwapPostProcessChainPath, fallbackChain,
        g_MainWindowData.Width, g_MainWindowData.Height, g_CompositeRenderPass, "swapchain");
#endif

    // AA/时序 pass 切换后丢弃旧历史，避免关闭后重新开启时把不同链路的结果混合。
    g_TAAHistoryNeedsClear = true;
    g_PreviousTAAJitterGame = glm::vec2(0.0f);
    g_SceneAOHistoryNeedsClear = true;
    g_GameAOHistoryNeedsClear = true;
    g_SceneSSGIHistoryNeedsClear = true;
    g_GameSSGIHistoryNeedsClear = true;
    g_SceneCloudHistoryNeedsClear = true;
    g_GameCloudHistoryNeedsClear = true;
    s_PrevCloudViewProjScene = glm::mat4(1.0f);
    s_PrevCloudViewProjGame = glm::mat4(1.0f);
    s_PrevCloudWindOffsetScene = glm::vec3(0.0f);
    s_PrevCloudWindOffsetGame = glm::vec3(0.0f);
    s_PrevCloudHighWindOffsetScene = glm::vec3(0.0f);
    s_PrevCloudHighWindOffsetGame = glm::vec3(0.0f);
}


// 渲染场景到离屏目标（编辑器 SceneView）
// 注意：不在场景视图生成 Hi-ZB，因为 Voxel 剔除使用的是游戏相机视角
    // Hi-ZB 只在游戏视图渲染后生成（见 RenderGameToTarget）
    
    // 场景视图也使用完整的计算着色器处理
void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data,
                 const glm::mat4& view, const glm::mat4& proj,
                 float deltaSeconds)
{
    // ===== FrameRender 分阶段计时 =====
    const bool cpuProfileEnabled = IsVulkanCpuProfileEnabled();
    const auto t0 = std::chrono::high_resolution_clock::now();

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
    if (g_LastOffscreenFrameFence != VK_NULL_HANDLE &&
        g_LastOffscreenFrameFence != fd->Fence) {
        // The offscreen render targets are not swapchain-image indexed. Do
        // not begin a new clear/write sequence while the prior frame can
        // still be reading or writing the same SceneRT/GameRT attachments.
        check_vk_result(vkWaitForFences(g_Device, 1, &g_LastOffscreenFrameFence,
                                        VK_TRUE, UINT64_MAX));
    }
    check_vk_result(vkResetFences(g_Device, 1, &fd->Fence));
    ++g_renderFrameSerial;
    const auto afterFenceWait = std::chrono::high_resolution_clock::now();

    // 启动阶段只提交加载页，不触碰未加载的 ECS 场景、模型、阴影或后处理资源。
    // FramePresent 仍由调用方负责，因此该分支与普通帧共享同一 acquire/submit/present 节奏。
    if (IsLoadingScreenActive()) {
        check_vk_result(vkResetCommandPool(g_Device, fd->CommandPool, 0));

        VkCommandBufferBeginInfo loadingBeginInfo{};
        loadingBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        loadingBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk_result(vkBeginCommandBuffer(fd->CommandBuffer, &loadingBeginInfo));
        RenderLoadingScreenPass(fd->CommandBuffer, wd, fd);
        // 加载页也纳入最终画面检查：AI/自动化可以验证启动阶段是否真的可见。
        Core::ScreenshotCapture::GetInstance().RecordSwapchainImage(
            fd->CommandBuffer, fd->Backbuffer, wd->SurfaceFormat.format,
            static_cast<uint32_t>(wd->Width), static_cast<uint32_t>(wd->Height),
            g_renderFrameSerial);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo loadingSubmit{};
        loadingSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        loadingSubmit.waitSemaphoreCount = 1;
        loadingSubmit.pWaitSemaphores = &image_acquired_semaphore;
        loadingSubmit.pWaitDstStageMask = &waitStage;
        loadingSubmit.commandBufferCount = 1;
        loadingSubmit.pCommandBuffers = &fd->CommandBuffer;
        loadingSubmit.signalSemaphoreCount = 1;
        loadingSubmit.pSignalSemaphores = &render_complete_semaphore;

        check_vk_result(vkEndCommandBuffer(fd->CommandBuffer));
        check_vk_result(vkQueueSubmit(g_Queue, 1, &loadingSubmit, fd->Fence));
        g_LastOffscreenFrameFence = fd->Fence;
        return;
    }

    if (g_ProjectSelectionPending) {
        check_vk_result(vkResetCommandPool(g_Device, fd->CommandPool, 0));

        VkCommandBufferBeginInfo managerBeginInfo{};
        managerBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        managerBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk_result(vkBeginCommandBuffer(fd->CommandBuffer, &managerBeginInfo));
        RenderProjectManagerPass(fd->CommandBuffer, wd, fd, draw_data);
        Core::ScreenshotCapture::GetInstance().RecordSwapchainImage(
            fd->CommandBuffer, fd->Backbuffer, wd->SurfaceFormat.format,
            static_cast<uint32_t>(wd->Width), static_cast<uint32_t>(wd->Height),
            g_renderFrameSerial);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo managerSubmit{};
        managerSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        managerSubmit.waitSemaphoreCount = 1;
        managerSubmit.pWaitSemaphores = &image_acquired_semaphore;
        managerSubmit.pWaitDstStageMask = &waitStage;
        managerSubmit.commandBufferCount = 1;
        managerSubmit.pCommandBuffers = &fd->CommandBuffer;
        managerSubmit.signalSemaphoreCount = 1;
        managerSubmit.pSignalSemaphores = &render_complete_semaphore;

        check_vk_result(vkEndCommandBuffer(fd->CommandBuffer));
        check_vk_result(vkQueueSubmit(g_Queue, 1, &managerSubmit, fd->Fence));
        g_LastOffscreenFrameFence = fd->Fence;
        return;
    }

    // Establish one immutable ECS snapshot after the fence and before any
    // shadow/view pass. SceneView, GameView, UI and particles share it.
    g_SceneRenderer.BeginRenderFrame();

    // 主相机的后处理链可能在属性面板中被修改；先刷新选择，再在 fence 已等待且
    // 尚未开始录制新命令的安全点重建链中间附件和管线。
    RefreshPostProcessChainSelection();
    RebuildPostProcessChainsIfRequested();

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

    // Compute-driven samples record simulation before any render pass begins.
    // The output render buffer is then consumed by the particle vertex stage
    // through an explicit compute->vertex barrier in GpuSphSimulation.
    if (IsGpuSphProjectActive()) {
        GpuSphSimulation::GetInstance().Record(fd->CommandBuffer, deltaSeconds);
    }

    
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
    
    // 点光源阴影列表由 UpdatePointLightBuffer 同步维护，与 UBO 槽号同源。
    RenderPointShadowMaps(fd->CommandBuffer);

    // 根据模式选择渲染方式
    if (g_RunMode == RunMode::Editor)
    {
        // 编辑器模式：物理天空全景图（相机无关，每帧渲染一次，SceneView/GameView 共用）
        // 天空图写入后已插入 barrier，所有视口合成可采样
        g_SkyboxRenderer.SyncFromRenderWorld(g_SceneRenderer.GetRenderWorld());
        if (g_SkyboxRenderer.IsEnabled() && g_AtmosphereEnabled && g_AtmosphereRenderer.IsInitialized()) {
            glm::vec3 lightDir, lightColor(1.0f, 0.96f, 0.89f); float lightIntensity = 1.0f;
            glm::vec3 sunDir = g_AtmosphereRenderer.GetSunDirection();
            if (GetSceneDirectionalLight(g_SceneRenderer.GetRenderWorld(), lightDir, lightColor, lightIntensity)) sunDir = lightDir;
            g_AtmosphereRenderer.RenderSkyRT(fd->CommandBuffer, sunDir, glm::vec3(glm::inverse(view)[3]));   // 海拔=max(0, 相机y+200)
        }
        DispatchSceneClusterCull(fd->CommandBuffer, view, proj,
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
                    // SceneView 和 GameView 使用独立的聚簇网格。编辑器路径之前
                    // 只更新了 g_SceneCluster，Game composite 读取的 g_GameCluster
                    // 会保留上一帧/空数据，导致大量点光源在游戏视图中剔除错误。
                    DispatchGameClusterCull(fd->CommandBuffer, gameView, gameProj,
                                        (float)g_GameRenderTarget.GetWidth(),
                                        (float)g_GameRenderTarget.GetHeight());
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

        // geometry RenderPass → separate composite RenderPass → 后处理链 → swapchain
        RenderGameComposite(gameView, gameProj, wd->FrameIndex);
        hiZGenerated = true;

        // 编辑器托管的游戏模式直接输出到 swapchain；ImGui 必须使用 LOAD pass，
        // 不能再用默认 CLEAR 的窗口 pass，否则会清掉刚完成的游戏画面。
        if (draw_data && !(draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f))
        {
            VkRenderPassBeginInfo imguiPass = {};
            imguiPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            const bool useSwapchainUiPass =
                g_EditorActive && g_RunMode == RunMode::Game &&
                g_CompositeUIPass != VK_NULL_HANDLE &&
                wd->FrameIndex < g_CompositeFramebuffers.size();
            imguiPass.renderPass = useSwapchainUiPass ? g_CompositeUIPass : wd->RenderPass;
            imguiPass.framebuffer = useSwapchainUiPass
                ? g_CompositeFramebuffers[wd->FrameIndex]
                : fd->Framebuffer;
            imguiPass.renderArea.extent.width = wd->Width;
            imguiPass.renderArea.extent.height = wd->Height;
            imguiPass.clearValueCount = useSwapchainUiPass ? 0 : 1;
            imguiPass.pClearValues = useSwapchainUiPass ? nullptr : &wd->ClearValue;
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

    // 所有游戏/编辑器 UI、调试叠加和 ImGui 都已经完成录制；截图必须位于这里，
    // 才能代表用户实际看到的最终 Swapchain 内容。
    Core::ScreenshotCapture::GetInstance().RecordSwapchainImage(
        fd->CommandBuffer, fd->Backbuffer, wd->SurfaceFormat.format,
        static_cast<uint32_t>(wd->Width), static_cast<uint32_t>(wd->Height),
        g_renderFrameSerial);
    
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
    g_LastOffscreenFrameFence = fd->Fence;
    g_SceneRenderer.EndRenderFrame();

    if (cpuProfileEnabled) {
        const auto afterSubmit = std::chrono::high_resolution_clock::now();
        ++g_cpuProfileFrameCount;
        g_cpuProfileFrameWallMs +=
            std::chrono::duration<double, std::milli>(afterSubmit - t0).count();
        g_cpuProfileFenceWaitMs +=
            std::chrono::duration<double, std::milli>(afterFenceWait - t0).count();
        g_cpuProfileCommandRecordMs +=
            std::chrono::duration<double, std::milli>(afterSubmit - afterFenceWait).count();

        if ((g_cpuProfileFrameCount % 60u) == 0u) {
            const double invFrames = 1.0 / static_cast<double>(g_cpuProfileFrameCount);
            printf("[VulkanManager][CPU] frames=%llu avg_frame_wall_ms=%.3f "
                   "avg_fence_wait_ms=%.3f avg_command_record_ms=%.3f\n",
                   static_cast<unsigned long long>(g_cpuProfileFrameCount),
                   g_cpuProfileFrameWallMs * invFrames,
                   g_cpuProfileFenceWaitMs * invFrames,
                   g_cpuProfileCommandRecordMs * invFrames);
        }
    }
}

uint64_t GetCurrentFrameSerial()
{
    return g_renderFrameSerial;
}
