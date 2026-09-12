#include "Core/VulkanStartupPasses.h"

#include "Core/VulkanManager.h"
#include "Rendering/Renderer2D.h"
#include "TextRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <glm/gtc/matrix_transform.hpp>

namespace
{
// 启动加载页状态。该页面直接绘制到 swapchain，不依赖 ECS 场景或后处理链，
// 因此可以在 SceneSerializer::LoadScene() 之前显示，覆盖模型/纹理预加载期间的等待。
bool s_loadingScreenActive = false;
float s_loadingScreenProgress = 0.0f;
char s_loadingScreenStatus[128] = "Preparing...";
bool s_startupSplashActive = false;
float s_startupSplashOpacity = 1.0f;
}

void SetLoadingScreenState(bool active, float progress, const char* status)
{
    s_loadingScreenActive = active;
    s_loadingScreenProgress = std::clamp(progress, 0.0f, 1.0f);
    const char* text = (status != nullptr && status[0] != '\0') ? status : "Loading...";
    std::snprintf(s_loadingScreenStatus, sizeof(s_loadingScreenStatus), "%s", text);
}

void SetStartupSplashState(bool active, float opacity)
{
    s_startupSplashActive = active;
    s_startupSplashOpacity = std::clamp(opacity, 0.0f, 1.0f);
}

bool IsLoadingScreenActive()
{
    return s_loadingScreenActive;
}

// 启动加载页的 swapchain pass。这里不调用场景、天空、CSM 或后处理链，
// 所以即使 ECS 尚未加载任何实体，也能稳定提交一帧可见内容。
void RenderLoadingScreenPass(VkCommandBuffer commandBuffer,
                             ImGui_ImplVulkanH_Window* wd,
                             ImGui_ImplVulkanH_Frame* frame)
{
    const uint32_t width = wd ? wd->Width : 0;
    const uint32_t height = wd ? wd->Height : 0;
    if (commandBuffer == VK_NULL_HANDLE || wd == nullptr || frame == nullptr ||
        width == 0 || height == 0) {
        return;
    }

    VkClearValue clearValue{};
    clearValue.color.float32[0] = 0.025f;
    clearValue.color.float32[1] = 0.032f;
    clearValue.color.float32[2] = 0.045f;
    clearValue.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = wd->RenderPass;
    renderPassInfo.framebuffer = frame->Framebuffer;
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = { width, height };
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = { width, height };
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    auto& r2d = Renderer2D::GetInstance();
    r2d.UseSecondaryBuffer(false);
    r2d.SetDisplayUI(false);
    r2d.SetSwapchainUI(false);
    r2d.ResetFrame();
    const glm::mat4 uiProj = glm::ortho(0.0f, static_cast<float>(width),
                                        0.0f, static_cast<float>(height));
    r2d.BeginFrame(commandBuffer, uiProj, width, height, true);

    const float screenW = static_cast<float>(width);
    const float screenH = static_cast<float>(height);
    const VkDescriptorSet logo = r2d.GetTexture("mikan_engine_splash");
    const bool hasLogo = logo != VK_NULL_HANDLE && logo != r2d.GetWhiteTexture();

    // Unity 风格启动 Splash：Logo 独占整屏，保持后按 opacity 渐隐到黑色。
    // 进入该分支时不绘制进度条，渐隐完成后才切换到下方 Loading 卡片。
    if (s_startupSplashActive) {
        r2d.DrawRect({ 0.0f, 0.0f }, { screenW, screenH },
                     { 0.99f, 0.985f, 0.94f, 1.0f }, -100);
        if (hasLogo) {
            constexpr float kLogoAspect = 1.778f;
            // 横屏设备通常比 Logo 的 16:9 画布更宽；使用 cover 而不是 contain，
            // 让图片本身覆盖左右边缘，避免图片米白渐变与纯色边带产生色差。
            // Android Activity 锁定 sensorLandscape，因此这里仅裁剪上下留白区域。
            const float coverScale = std::max(screenW / kLogoAspect, screenH);
            const float logoW = coverScale * kLogoAspect;
            const float logoH = coverScale;
            r2d.DrawSprite({ (screenW - logoW) * 0.5f, (screenH - logoH) * 0.5f },
                            { logoW, logoH }, logo,
                            glm::vec2(0.0f), glm::vec2(1.0f),
                            { 1.0f, 1.0f, 1.0f, s_startupSplashOpacity }, -90);
        }
        if (s_startupSplashOpacity < 1.0f) {
            r2d.DrawRect({ 0.0f, 0.0f }, { screenW, screenH },
                          { 0.0f, 0.0f, 0.0f, 1.0f - s_startupSplashOpacity }, 100);
        }
        r2d.Flush();
        r2d.SetDisplayUI(false);
        r2d.SetSwapchainUI(false);
        r2d.UseSecondaryBuffer(false);
        vkCmdEndRenderPass(commandBuffer);
        return;
    }

    // Loading 页面保持纯信息布局：不再重复显示开屏 Logo，只保留状态文字、百分比和进度条。
    // 放大卡片与进度条，避免在高分辨率移动屏上内容过小。
    const float panelW = std::min(screenW * 0.78f, 900.0f);
    const float panelH = std::min(std::max(screenH * 0.28f, 260.0f), screenH * 0.45f);
    const float panelX = (screenW - panelW) * 0.5f;
    const float panelY = (screenH - panelH) * 0.5f;
    const float inset = std::min(72.0f, panelW * 0.10f);

    r2d.DrawRect({ 0.0f, 0.0f }, { screenW, screenH },
                 { 0.025f, 0.032f, 0.045f, 1.0f }, -100);
    r2d.DrawRect({ panelX, panelY }, { panelW, panelH },
                 { 0.10f, 0.12f, 0.16f, 0.97f }, -90);
    r2d.DrawRect({ panelX, panelY }, { 5.0f, panelH },
                 { 0.31f, 0.67f, 1.0f, 1.0f }, -80);

    const float barX = panelX + inset;
    const float barW = std::max(80.0f, panelW - inset * 2.0f);
    const float barY = panelY + panelH * 0.60f;
    const float barH = std::clamp(screenH * 0.026f, 22.0f, 32.0f);
    r2d.DrawRect({ barX, barY }, { barW, barH },
                 { 0.035f, 0.045f, 0.065f, 1.0f }, 0);
    const float progressW = barW * s_loadingScreenProgress;
    if (progressW > 0.0f) {
        r2d.DrawRect({ barX, barY }, { progressW, barH },
                     { 0.31f, 0.67f, 1.0f, 1.0f }, 1);
    }

    TextRenderer& text = TextRenderer::GetInstance();
    if (text.IsReady()) {
        const float statusSize = std::clamp(std::min(screenW, screenH) * 0.052f, 28.0f, 48.0f);
        const float statusY = panelY + panelH * 0.25f;
        text.DrawStringSdf(s_loadingScreenStatus, barX, statusY,
                           statusSize, { 0.82f, 0.87f, 0.95f, 1.0f }, 2);

        char percent[16] = {};
        std::snprintf(percent, sizeof(percent), "%d%%",
                      static_cast<int>(std::round(s_loadingScreenProgress * 100.0f)));
        const float percentWidth = text.MeasureString(percent, statusSize);
        text.DrawStringSdf(percent, panelX + panelW - inset - percentWidth,
                           statusY, statusSize,
                           { 0.68f, 0.82f, 1.0f, 1.0f }, 2);
    }

    r2d.Flush();
    r2d.SetDisplayUI(false);
    r2d.SetSwapchainUI(false);
    r2d.UseSecondaryBuffer(false);
    vkCmdEndRenderPass(commandBuffer);
}

// 项目管理器阶段只提交交换链清屏和管理器 UI；不触碰旧项目的离屏目标、
// 后处理、光源或场景渲染器，避免切换项目时旧画面在后台继续运行。
void RenderProjectManagerPass(VkCommandBuffer commandBuffer,
                              ImGui_ImplVulkanH_Window* wd,
                              ImGui_ImplVulkanH_Frame* frame,
                              ImDrawData* drawData)
{
    const uint32_t width = wd ? wd->Width : 0;
    const uint32_t height = wd ? wd->Height : 0;
    if (commandBuffer == VK_NULL_HANDLE || wd == nullptr || frame == nullptr ||
        width == 0 || height == 0) {
        return;
    }

    VkClearValue clearValue{};
    clearValue.color.float32[0] = 0.025f;
    clearValue.color.float32[1] = 0.032f;
    clearValue.color.float32[2] = 0.045f;
    clearValue.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = wd->RenderPass;
    renderPassInfo.framebuffer = frame->Framebuffer;
    renderPassInfo.renderArea.extent = { width, height };
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = { width, height };
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    if (drawData && drawData->DisplaySize.x > 0.0f && drawData->DisplaySize.y > 0.0f) {
        ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
    }
    vkCmdEndRenderPass(commandBuffer);
}
