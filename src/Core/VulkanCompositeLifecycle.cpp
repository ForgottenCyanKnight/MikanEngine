// High-level composite setup shared by startup and swapchain recreation.

#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "Core/VulkanCompositeLifecycle.h"

#include "Core/EngineConfig.h"
#include "Core/EngineGlobal.h"
#include "Core/ProjectManager.h"
#include "Core/RenderGlobals.h"
#include "Core/VulkanCompositeResources.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanFramePipeline.h"
#include "Core/VulkanLightingCulling.h"
#include "Core/VulkanManager.h"
#include "Core/VulkanPostProcessChains.h"
#include "Core/VulkanRuntimeResources.h"
#include "AtmosphereRenderer.h"
#include "FullscreenQuad.h"
#include "Rendering/CMAA2.h"
#include "Rendering/PostProcessChain.h"
#include "Rendering/RenderTarget.h"
#include "Rendering/SceneRenderer.h"
#include "Rendering/TexturePool.h"

#include <cstdio>

extern AtmosphereRenderer g_AtmosphereRenderer;
extern bool g_AtmosphereEnabled;
extern FullscreenQuad g_SceneCompositeQuad;
extern FullscreenQuad g_GameCompositeQuad;
extern CMAA2 g_SceneCMAA2;
extern CMAA2 g_GameCMAA2;
extern CMAA2 g_SwapCMAA2;

void InitCompositeResources()
{
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    VulkanComposite_CreateRenderPass(wd);
    VulkanComposite_CreateFramebuffers(wd);

    const std::string fallbackChain = DefaultPostProcessChainPath();
    s_RequestedScenePostProcessChainPath = ResolveEditorPostProcessChainPath();
    const ECS::Entity mainCamera = g_SceneRenderer.GetMainCameraEntity();
    s_RequestedGamePostProcessChainPath = ResolveCameraPostProcessChainPath(
        g_SceneRenderer.GetRenderWorld(), mainCamera);
    s_RequestedSwapPostProcessChainPath = s_RequestedGamePostProcessChainPath;
#ifdef __ANDROID__
    // 几何与合成分成两个独立 render pass（Adreno 多 subpass + input attachment 的 vkCreateRenderPass 即崩）。
    // Android 无编辑器/SceneView，不初始化 g_SceneCompositeQuad/Scene 链/CMAA2；Swap 链（mobile 配置）与桌面共用 Execute 路径。
    // ⚠️ 独立合成 pass 无 G-Buffer input attachments——必须显式用 texture 采样版 fullscreen.frag.spv
    //    （默认宏 MIKAN_COMPOSITE_SHADER 是 fullscreen_subpass.frag.spv 的 subpassLoad 版，仅适用于合并 render pass 的合成 subpass；
    g_GameCompositeQuad.Init(g_GameRenderTarget.GetCompositeRenderPass(), 0, "fullscreen.frag.spv");
    {
        // 移动端后处理链（默认配置可被主相机的 profile 覆盖）。
        BuildPostProcessChainWithFallback(
            g_SwapChain, s_ActiveSwapPostProcessChainPath,
            s_RequestedSwapPostProcessChainPath, fallbackChain,
            wd->Width, wd->Height, g_CompositeRenderPass, "swapchain", true);
    }
    return;
#endif
    // All desktop MRT targets use the same separate composite pass as Android:
    // the fullscreen shader samples the stored G-buffer images explicitly.
    g_SceneCompositeQuad.Init(g_SceneRenderTarget.GetCompositeRenderPass(), 0, "fullscreen.frag.spv");
    g_GameCompositeQuad.Init(g_GameRenderTarget.GetCompositeRenderPass(), 0, "fullscreen.frag.spv");
    // SceneView 使用项目为编辑器自由相机指定的链；GameView 与游戏模式使用主相机链。
    BuildPostProcessChainWithFallback(
        g_SceneChain, s_ActiveScenePostProcessChainPath,
        s_RequestedScenePostProcessChainPath, fallbackChain,
        g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight(),
        g_SceneRenderTarget.GetFinalRenderPass(), "scene", true);
    BuildPostProcessChainWithFallback(
        g_GameChain, s_ActiveGamePostProcessChainPath,
        s_RequestedGamePostProcessChainPath, fallbackChain,
        g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
        g_GameRenderTarget.GetFinalRenderPass(), "game", true);
    BuildPostProcessChainWithFallback(
        g_SwapChain, s_ActiveSwapPostProcessChainPath,
        s_RequestedSwapPostProcessChainPath, fallbackChain,
        wd->Width, wd->Height, g_CompositeRenderPass, "swapchain", true);
    // 输入 = tonemap 输出（LDR gamma 空间，官方 CMAA2 语义），而非 HDR 线性 composite（未合成 AO、暗部对比度低）
    g_SceneChain.SetPassHook("tonemap", [](VkCommandBuffer cmd, VkImageView view, VkImage image) {
        if (!g_SceneChain.IsPassEnabled("cmaa_apply")) return;
        VkSampler s = g_TexturePool->GetSamplerByType(SamplerType::LinearClamp);
        g_SceneCMAA2.Dispatch(cmd, view, s, image, g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight());
    });
    g_GameChain.SetPassHook("tonemap", [](VkCommandBuffer cmd, VkImageView view, VkImage image) {
        if (!g_GameChain.IsPassEnabled("cmaa_apply")) return;
        VkSampler s = g_TexturePool->GetSamplerByType(SamplerType::LinearClamp);
        g_GameCMAA2.Dispatch(cmd, view, s, image, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight());
    });
    g_SwapChain.SetPassHook("tonemap", [](VkCommandBuffer cmd, VkImageView view, VkImage image) {
        if (!g_SwapChain.IsPassEnabled("cmaa_apply")) return;
        VkSampler s = g_TexturePool->GetSamplerByType(SamplerType::LinearClamp);
        g_SwapCMAA2.Dispatch(cmd, view, s, image, (uint32_t)g_MainWindowData.Width, (uint32_t)g_MainWindowData.Height);
    });
    if (!g_SceneCMAA2.Init(g_Device, g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight()) ||
        !g_GameCMAA2.Init(g_Device, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight()) ||
        !g_SwapCMAA2.Init(g_Device, g_MainWindowData.Width, g_MainWindowData.Height)) {
        fprintf(stderr, "[VulkanCompositeLifecycle] CMAA2 Init failed\n");
    }
    // 天空 RT 占位（AtmosphereRenderer 初始化完成后由调用方更新为真实天空 RT）
    UpdateFullscreenQuadDescriptors();
}

void UpdateFullscreenQuadDescriptors()
{
    // 银河全景（end_sky.png——LogLuv32 编码——4096x2048；合成 pass sky 分支采样叠加；幂等加载）
    if (!g_TexturePool->GetTexture("end_sky")) {
        g_TexturePool->LoadTexture2D("end_sky",
            ProjectManager::GetInstance().GetEngineAssetPath("textures/end_sky.png"));
    }
    const TextureInfo* galaxy = g_TexturePool->GetTexture("end_sky");
    VkImageView galaxyView = galaxy ? galaxy->imageView : VK_NULL_HANDLE;
    if (!g_TexturePool->GetTexture("sky_hdr")) {
        g_TexturePool->LoadHDRCubemap("sky_hdr", EngineConfig::GetEngineTexturePath("skybox") + "/EnvironmentMap/snow_field_puresky_1k.hdr");
    }
    const TextureInfo* skyHDR = g_TexturePool->GetTexture("sky_hdr");
    VkImageView skyCubeView = g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyCubeView() : (skyHDR ? skyHDR->imageView : VK_NULL_HANDLE);
    VkSampler skyCubeSampler = g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyCubeSampler() : g_TexturePool->GetSamplerByType(SamplerType::Linear);
    const TextureInfo* skyIrr = g_TexturePool->GetTexture("sky_hdr_irr");
    VkImageView skyIrrView = skyIrr ? skyIrr->imageView : (skyHDR ? skyHDR->imageView : VK_NULL_HANDLE);
    VkSampler skyIrrSampler = g_TexturePool->GetSamplerByType(SamplerType::Linear);
    // 合成 quad：G-Buffer 颜色0/深度 + skyRT（独立 descriptor set，勿与其他视口共用）
    CascadeShadowRenderer* csmSceneInit = g_SceneRenderer.EnsureCascadeShadows();
    CascadeShadowRenderer* csmGameInit = g_SceneRenderer.EnsureCascadeShadows();
#ifndef __ANDROID__
    // Android 无 SceneCompositeQuad（InitCompositeResources 的 __ANDROID__ 分支只初始化 Game），跳过 Scene quad 更新
    g_SceneCompositeQuad.UpdateDescriptorSet(g_SceneRenderTarget.GetColorImageView(), g_SceneRenderTarget.GetDepthImageView(), g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyImageView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkySampler() : VK_NULL_HANDLE, g_SceneRenderTarget.GetColorImageView(1), g_SceneRenderTarget.GetColorImageView(2), galaxyView, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetTransmittanceView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetScatteringView() : VK_NULL_HANDLE, skyCubeView, skyCubeSampler, skyIrrView, skyIrrSampler, GetShIrradianceBuffer(), UpdatePointLightBuffer(), GetSceneClusterGridBuffer(),
        (g_SceneRenderer.EnsurePointShadows() && g_SceneRenderer.EnsurePointShadows()->IsInitialized()) ? g_SceneRenderer.EnsurePointShadows()->GetCubeArrayView() : VK_NULL_HANDLE,
        (csmSceneInit && csmSceneInit->IsInitialized()) ? csmSceneInit->GetArrayView(0) : VK_NULL_HANDLE,
        g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare),
        (csmSceneInit && csmSceneInit->IsInitialized()) ? csmSceneInit->GetCascadeBuffer(0, (int)g_MainWindowData.FrameIndex) : VK_NULL_HANDLE,
        g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutView() : VK_NULL_HANDLE,
        g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutSampler() : VK_NULL_HANDLE);
#endif
    g_GameCompositeQuad.UpdateDescriptorSet(g_GameRenderTarget.GetColorImageView(), g_GameRenderTarget.GetDepthImageView(), g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyImageView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkySampler() : VK_NULL_HANDLE, g_GameRenderTarget.GetColorImageView(1), g_GameRenderTarget.GetColorImageView(2), galaxyView, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetTransmittanceView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetScatteringView() : VK_NULL_HANDLE, skyCubeView, skyCubeSampler, skyIrrView, skyIrrSampler, GetShIrradianceBuffer(), UpdatePointLightBuffer(), GetGameClusterGridBuffer(),
        (g_SceneRenderer.EnsurePointShadows() && g_SceneRenderer.EnsurePointShadows()->IsInitialized()) ? g_SceneRenderer.EnsurePointShadows()->GetCubeArrayView() : VK_NULL_HANDLE,
        (csmGameInit && csmGameInit->IsInitialized()) ? csmGameInit->GetArrayView(kGameCsmSlot) : VK_NULL_HANDLE,
        g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare),
        (csmGameInit && csmGameInit->IsInitialized()) ? csmGameInit->GetCascadeBuffer(kGameCsmSlot, (int)g_MainWindowData.FrameIndex) : VK_NULL_HANDLE,
        g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutView() : VK_NULL_HANDLE,
        g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutSampler() : VK_NULL_HANDLE);
}
