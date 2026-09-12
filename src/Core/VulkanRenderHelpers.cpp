#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "Core/VulkanRenderHelpers.h"

#include "EngineGlobal.h"
#include "Core/Log.h"
#include "Core/ProjectManager.h"
#include "Rendering/PostProcessChain.h"
#include "Rendering/RenderWorld.h"
#include "Rendering/Renderer2D.h"
#include "SceneRenderer.h"
#include "AtmosphereRenderer.h"
#include "Game/GameManager.h"
#include "UI/Canvas2D.h"
#include "UI/RuntimeSettingsOverlay.h"

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <filesystem>
#include <vector>

extern AtmosphereRenderer g_AtmosphereRenderer;

// 合成 → final barrier：composite 附件从合成 subpass 的写入布局显式转换到采样布局，
// 合成 → final barrier：composite 保持 COLOR_ATTACHMENT_OPTIMAL（合成 subpass 写入布局），
// 链对 composite 也以 COLOR_ATTACHMENT_OPTIMAL 采样（布局全程一致，无需转换）——此 barrier 仅做 access 同步
void CompositeToFinalBarrier(VkCommandBuffer commandBuffer, VkImage compositeImage)
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
void RenderUIOverlay(VkCommandBuffer, uint32_t, uint32_t, VkRenderPass, VkFramebuffer, bool,
                     const glm::mat4* gridView, const glm::mat4* gridProj,
                     const glm::mat4* renderView, const glm::mat4* renderProj);
// 场景方向光收集
bool GetSceneDirectionalLight(const RenderWorld& world,
                                     glm::vec3& dir, glm::vec3& color, float& intensity);
// 填充 CSM 级联数据到 cameraUBO（gtao 半分辨率体积光采样阴影）——通用（场景/游戏视口各自 slot）
void FillCsmIntoUExt(PostProcessChain::ExternalInputs& ext, CascadeShadowRenderer* csm, int slot, VkSampler shadowSampler)
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

// 云的太阳/月光颜色必须与天空太阳盘共享同一张物理透射率 LUT。
// 集中填写，避免 Scene/Game/Android 三条后处理路径出现颜色不一致。
void FillAtmosphereTransmittanceIntoExt(PostProcessChain::ExternalInputs& ext)
{
    const bool initialized = g_AtmosphereRenderer.IsInitialized();
    ext.atmoTransmittanceView = initialized
        ? g_AtmosphereRenderer.GetTransmittanceView()
        : VK_NULL_HANDLE;
    ext.atmoTransmittanceSampler = initialized
        ? g_AtmosphereRenderer.GetTransmittanceSampler()
        : VK_NULL_HANDLE;
    ext.atmoScatteringView = initialized
        ? g_AtmosphereRenderer.GetScatteringView()
        : VK_NULL_HANDLE;
    ext.atmoScatteringSampler = initialized
        ? g_AtmosphereRenderer.GetTransmittanceSampler()
        : VK_NULL_HANDLE;
}

// 云层在 HSPE 的地心公里坐标中移动。时间来自应用时钟，避免把
// 后处理链的 pass/frame 计数误当成真实时间（同一帧可能执行多个视口）。
glm::vec3 CloudWindOffsetKm(float speedKmPerSecond, glm::vec2 direction)
{
    const float directionLength = glm::length(direction);
    if (directionLength <= 1e-5f) {
        direction = glm::vec2(1.0f, 0.0f);
    } else {
        direction /= directionLength;
    }

    const float timeSeconds = static_cast<float>(SDL_GetTicks()) * 0.001f;
    const float distanceKm = glm::max(speedKmPerSecond, 0.0f) * timeSeconds;
    return glm::vec3(direction.x * distanceKm, 0.0f, direction.y * distanceKm);
}

glm::vec3 CloudWindOffsetKm(const RenderCloudData& cloud)
{
    return CloudWindOffsetKm(cloud.windSpeedKmPerSecond, cloud.windDirectionXZ);
}

glm::vec3 CloudHighWindOffsetKm(const RenderCloudData& cloud)
{
    return CloudWindOffsetKm(cloud.highCloudWindSpeedKmPerSecond,
                             cloud.highCloudWindDirectionXZ);
}

static const RenderCloudData* FindCloudInSnapshotSubtree(
    const RenderWorld& world, ECS::Entity root)
{
    std::vector<ECS::Entity> pending = { root };
    while (!pending.empty()) {
        const ECS::Entity entity = pending.back();
        pending.pop_back();
        const RenderWorldEntity* data = world.Find(entity);
        if (data == nullptr) continue;
        if (data->hasCloud) return &data->cloud;
        pending.insert(pending.end(), data->children.begin(), data->children.end());
    }
    return nullptr;
}

void FillCloudSettingsFromSnapshot(const RenderCloudData& cloud,
                                          PostProcessQuad::CameraUBO& ubo)
{
    ubo.cloudParams0 = glm::vec4(
        cloud.enabled ? 1.0f : 0.0f,
        glm::clamp(cloud.coverage, 0.0f, 0.98f),
        glm::max(cloud.density, 0.0f),
        glm::max(cloud.baseAltitudeKm, 0.0f));
    ubo.cloudParams1 = glm::vec4(
        glm::max(cloud.thicknessKm, 0.01f),
        glm::max(cloud.noiseScale, 0.00001f),
        glm::clamp(cloud.detailErosion, 0.0f, 1.0f),
        glm::max(cloud.lightAbsorption, 0.0f));
    ubo.cloudNoiseOffsetKm = glm::vec4(cloud.noiseOffsetKm, 0.0f);
    ubo.cloudLightingParams = glm::vec4(
        glm::clamp(cloud.multipleScattering, 0.0f, 1.0f),
        glm::clamp(cloud.multipleScatteringBuild, 0.0f, 1.0f),
        glm::clamp(cloud.multipleScatteringBoundary, 0.0f, 1.0f),
        glm::clamp(cloud.multipleScatteringCompress, 0.0f, 2.0f));

    const float windDirectionLength = glm::length(cloud.windDirectionXZ);
    const glm::vec2 normalizedWindDirection =
        windDirectionLength > 0.0001f
            ? (cloud.windDirectionXZ / windDirectionLength)
            : glm::vec2(1.0f, 0.0f);
    ubo.cloudShapeParams = glm::vec4(
        glm::max(cloud.detailScale, 0.05f),
        normalizedWindDirection.x, normalizedWindDirection.y, 0.0f);
    ubo.cloudWindOffsetKm = glm::vec4(CloudWindOffsetKm(cloud), 0.0f);
    ubo.cloudHighParams0 = glm::vec4(
        cloud.highCloudEnabled ? 1.0f : 0.0f,
        glm::clamp(cloud.highCloudCoverage, 0.0f, 0.98f),
        glm::max(cloud.highCloudDensity, 0.0f),
        glm::max(cloud.highCloudAltitudeKm, 0.0f));
    ubo.cloudHighParams1 = glm::vec4(
        glm::max(cloud.highCloudThicknessKm, 0.01f),
        glm::max(cloud.highCloudScale, 0.00001f),
        glm::clamp(cloud.highCloudDetail, 0.0f, 1.0f),
        glm::max(cloud.highCloudBrightness, 0.0f));
    ubo.cloudHighWindOffsetKm = glm::vec4(CloudHighWindOffsetKm(cloud), 0.0f);
    const float highWindDirectionLength = glm::length(cloud.highCloudWindDirectionXZ);
    const glm::vec2 normalizedHighWindDirection =
        highWindDirectionLength > 0.0001f
            ? (cloud.highCloudWindDirectionXZ / highWindDirectionLength)
            : glm::vec2(1.0f, 0.0f);
    ubo.cloudHighWindDirectionXZ = glm::vec4(
        normalizedHighWindDirection.x, normalizedHighWindDirection.y, 0.0f, 0.0f);
}

// 将当前选中的体积云实体（若有）或场景中的第一个组件写入后处理 UBO。
void FillCloudSettings(const RenderWorld& world, PostProcessQuad::CameraUBO& ubo)
{
    // 默认关闭；仅当场景树挂载 CloudVolumeComponent 时打开。
    ubo.cloudParams0 = glm::vec4(0.0f, 0.45f, 0.55f, 7.5f);
    ubo.cloudParams1 = glm::vec4(12.5f, 0.01f, 0.24f, 1.0f);
    ubo.cloudNoiseOffsetKm = glm::vec4(37.0f, 13.0f, -61.0f, 0.0f);
    ubo.cloudLightingParams = glm::vec4(0.22f, 0.15f, 0.65f, 0.35f);
    ubo.cloudShapeParams = glm::vec4(4.0f, 1.0f, 0.0f, 0.0f);
    ubo.cloudWindOffsetKm = glm::vec4(0.0f);
    ubo.cloudPrevWindOffsetKm = glm::vec4(0.0f);
    ubo.cloudHighParams0 = glm::vec4(1.0f, 0.28f, 0.35f, 18.0f);
    ubo.cloudHighParams1 = glm::vec4(1.0f, 0.0045f, 0.45f, 0.32f);
    ubo.cloudHighWindOffsetKm = glm::vec4(0.0f);
    ubo.cloudHighPrevWindOffsetKm = glm::vec4(0.0f);
    ubo.cloudHighWindDirectionXZ = glm::vec4(0.35f, 1.0f, 0.0f, 0.0f);

    const RenderCloudData* active = nullptr;
    bool selectedSource = false;
    if (world.selectedEntity != ECS::INVALID_ENTITY) {
        active = FindCloudInSnapshotSubtree(world, world.selectedEntity);
        selectedSource = active != nullptr;
    }
    if (active == nullptr && !world.clouds.empty()) {
        active = &world.clouds.front();
    }
    if (active != nullptr) {
        FillCloudSettingsFromSnapshot(*active, ubo);
        static bool s_loggedCloudSettings = false;
        if (!s_loggedCloudSettings) {
            s_loggedCloudSettings = true;
            LOGI("[CloudSettings] source=%s entity=%u enabled=%.0f coverage=%.3f density=%.3f baseKm=%.3f thicknessKm=%.3f noiseScale=%.5f detailErosion=%.3f",
                 selectedSource ? "selected" : "scene", active->entity,
                 ubo.cloudParams0.x, ubo.cloudParams0.y, ubo.cloudParams0.z,
                 ubo.cloudParams0.w, ubo.cloudParams1.x, ubo.cloudParams1.y,
                 ubo.cloudParams1.z);
        }
        return;
    }

    static bool s_loggedCloudMissing = false;
    if (!s_loggedCloudMissing) {
        s_loggedCloudMissing = true;
        LOGW("[CloudSettings] no CloudVolumeComponent found; cloud_view UBO remains disabled");
    }
}


// UI 叠加 pass：链末 tonemap 之后，UI alpha 混合叠加在结果之上（编辑器=显示附件；游戏模式=swapchain）
// swapchainMode=false → Renderer2D SetDisplayUI（显示附件 loadOp=LOAD pass）；true → SetSwapchainUI（swapchain loadOp=LOAD pass）
// gridView/gridProj 非空时绘制无限刻度网格（仅 SceneView 链末；GameView 传 nullptr）
void RenderUIOverlay(VkCommandBuffer commandBuffer, uint32_t width, uint32_t height,
                            VkRenderPass uiPass, VkFramebuffer fb, bool swapchainMode,
                            const glm::mat4* gridView, const glm::mat4* gridProj,
                            const glm::mat4* renderView, const glm::mat4* renderProj)
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
    UI::Canvas2D::GetInstance().RenderWorld(
        r2d, commandBuffer, g_SceneRenderer.GetRenderWorld());
    // UI 层（画布 UI + 游戏 UI）
    UI::Canvas2D::GetInstance().RenderUI(
        r2d, commandBuffer, g_SceneRenderer.GetRenderWorld());
    r2d.BeginFrame(commandBuffer, UI::Canvas2D::GetInstance().GetUIViewProj(), width, height, false);
    r2d.SetRenderViewContext(renderView, renderProj, gridView != nullptr);
    if (auto* gm = Game::GameManager::GetInstance().GetCurrent()) {
        gm->OnRenderUI(r2d, width, height);
    }
    // 运行时设置页不是引擎默认 UI，只由项目清单显式开启时绘制；
    // SceneView 网格视口也不显示它，避免编辑器同一帧的多个视口重复绘制入口。
    auto* currentGame = Game::GameManager::GetInstance().GetCurrent();
    const bool runtimeSettingsEnabled =
        currentGame != nullptr &&
        ProjectManager::GetInstance().GetManifest().runtimeSettingsOverlay;
    if (gridView == nullptr && (swapchainMode || g_RunMode == RunMode::Game) &&
        runtimeSettingsEnabled) {
        UI::RuntimeSettingsOverlay::GetInstance().Render(r2d, static_cast<int>(width), static_cast<int>(height));
    } else if (!runtimeSettingsEnabled && UI::RuntimeSettingsOverlay::GetInstance().IsOpen()) {
        // 切换到不提供该能力的项目时，清除旧游戏留下的打开状态。
        UI::RuntimeSettingsOverlay::GetInstance().SetOpen(false);
    }
    r2d.Flush();
    if (swapchainMode) r2d.SetSwapchainUI(false); else r2d.SetDisplayUI(false);
    r2d.UseSecondaryBuffer(false);
    vkCmdEndRenderPass(commandBuffer);
}

// 方向 = Transform rotation * forward(0,0,-1)；颜色/强度来自 LightComponent。
// 返回 false = 场景无方向光（调用方保持原 sunDir + 白光/1 强度，即"硬编码固定位置"回退）
bool GetSceneDirectionalLight(const RenderWorld& world,
                                     glm::vec3& dir, glm::vec3& color, float& intensity) {
    for (const RenderLightData& light : world.lights) {
        if (light.type != RenderLightType::Directional) continue;
        dir = glm::normalize(light.rotation * glm::vec3(0.0f, 0.0f, -1.0f));
        color = light.color;
        intensity = light.intensity;
        return true;
    }
    return false;
}

