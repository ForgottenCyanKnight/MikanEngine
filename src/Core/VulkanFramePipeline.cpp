#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "Core/VulkanFramePipeline.h"

#include "EngineGlobal.h"
#include "EngineConfig.h"
#include "Core/Log.h"
#include "Core/ProjectManager.h"
#include "Core/VulkanLightingCulling.h"
#include "Core/VulkanPostProcessHistory.h"
#include "Core/VulkanPostProcessChains.h"
#include "Core/VulkanRenderHelpers.h"
#include "AtmosphereRenderer.h"
#include "Game/GameManager.h"
#include "Rendering/CMAA2.h"
#include "Rendering/PostProcessChain.h"
#include "Rendering/RenderTarget.h"
#include "Rendering/Renderer2D.h"
#include "SceneRenderer.h"
#include "SkyboxRenderer.h"
#include "UI/Canvas2D.h"
#include "UI/RuntimeSettingsOverlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

extern AtmosphereRenderer g_AtmosphereRenderer;
extern bool g_AtmosphereEnabled;
extern FullscreenQuad g_SceneCompositeQuad;
extern FullscreenQuad g_GameCompositeQuad;
extern CMAA2 g_SceneCMAA2;
extern CMAA2 g_GameCMAA2;
extern CMAA2 g_SwapCMAA2;

// 声明放全局区（Cleanup 在文件前部使用）；Ensure/Prepare/Copy 函数定义在 CopyAOHistory 之后
glm::mat4 s_PrevView = glm::mat4(1.0f);
glm::mat4 s_PrevProj = glm::mat4(1.0f);
glm::mat4 s_PrevViewProj = glm::mat4(1.0f);
glm::mat4 s_PrevCloudViewProjScene = glm::mat4(1.0f);
glm::mat4 s_PrevCloudViewProjGame = glm::mat4(1.0f);
glm::vec3 s_PrevCloudWindOffsetScene = glm::vec3(0.0f);
glm::vec3 s_PrevCloudWindOffsetGame = glm::vec3(0.0f);
glm::vec3 s_PrevCloudHighWindOffsetScene = glm::vec3(0.0f);
glm::vec3 s_PrevCloudHighWindOffsetGame = glm::vec3(0.0f);
// Android 游戏路径的 TAA 上一帧 jitter；运行时切换链后由安全重建点清零。
glm::vec2 g_PreviousTAAJitterGame = glm::vec2(0.0f);

// v2：投影矩阵保持无 jitter（projView/prevProjView/invViewProj 全部几何真实——motion 纯运动量），
// jitter 由 model.vert 在顶点内 `clipPos.xy += taaJitter·w` 实现（IDKEngine 语义，深度不变）；
// 每视图独立计数器（⚠️ 共享计数器会让 GUI 每帧 +2/+3 → Halton 跳步失真 → 抖动）
static float TAAHalton(int index, int base)
{
    float f = 1.0f, r = 0.0f;
    while (index > 0) { f /= (float)base; r += f * (float)(index % base); index /= base; }
    return r;
}
static glm::vec2 ComputeTAAJitter(uint32_t& frameCounter, float w, float h)
{
    uint32_t idx = frameCounter++;
    float jx = (TAAHalton((int)idx, 2) - 0.5f) * 2.0f / w;
    float jy = (TAAHalton((int)idx, 3) - 0.5f) * 2.0f / h;
    return glm::vec2(jx, jy);
}
// 当前渲染视图的 TAA jitter（NDC 偏移；ModelRenderer 填充 push constant 用；TAA 禁用时 = 0）
glm::vec2 g_CurrentTAAJitter = glm::vec2(0.0f);
// 每视图独立计数器：Scene（编辑器 SceneView）/ GameView（编辑器 GameView 面板）/ Game（游戏模式主输出）各一条 Halton 序列
// ⚠️ 编辑器模式三路径同帧执行——必须各持计数器，否则序列跳步失真
static uint32_t g_TAAJitterFrameScene = 0;
static uint32_t g_TAAJitterFrameGameView = 0;
static uint32_t g_TAAJitterFrameGame = 0;
// TAA fragment pass 需要上一帧与当前帧 jitter 的差值，把排除了 jitter 的运动矢量
// 重新对齐到两帧实际写入的屏幕位置。Android 游戏路径每帧只渲染一次，单独保存即可。
// 上一帧的 view*proj（GTAO 相机重投影 UBO）

void RenderSceneToTarget(const glm::mat4& view, const glm::mat4& proj, uint32_t frameIndex)
{
    // 诊断（临时）：首帧确认编辑器 SceneView 渲染执行
    static bool s_loggedScene = false;
    if (!s_loggedScene) {
        printf("[VulkanManager] RenderSceneToTarget executed (g_ShowSceneView=%d g_SceneIs2D=%d)\n", (int)g_ShowSceneView, (int)g_SceneIs2D);
        s_loggedScene = true;
    }

    // 使用主命令缓冲区进行场景渲染
    VkCommandBuffer commandBuffer = g_MainWindowData.Frames[g_MainWindowData.FrameIndex].CommandBuffer;
    g_CurrentTAAJitter = g_SceneChain.IsPassEnabled("taa") ? ComputeTAAJitter(g_TAAJitterFrameScene, (float)g_SceneRenderTarget.GetWidth(), (float)g_SceneRenderTarget.GetHeight()) : glm::vec2(0.0f);
    (void)proj;   // 后续渲染调用换 proj

    
    // 物理天空全景图已由 FrameRender 统一渲染（相机无关）——此处只判断 usePhysicalSky
    g_SkyboxRenderer.SyncFromRenderWorld(g_SceneRenderer.GetRenderWorld());
    bool usePhysicalSky = false;
    if (g_SkyboxRenderer.IsEnabled() && g_AtmosphereEnabled && g_AtmosphereRenderer.IsInitialized()) {
        usePhysicalSky = true;
    }
    
    glm::vec3 lightDir, lightColor(1.0f, 0.96f, 0.89f); float lightIntensity = 1.0f;
    glm::vec3 sunDir = g_AtmosphereRenderer.GetSunDirection();
    if (GetSceneDirectionalLight(g_SceneRenderer.GetRenderWorld(), lightDir, lightColor, lightIntensity)) sunDir = lightDir;

    // 合成描述符：SceneRT 颜色0/深度 + 天空 RT（独立 descriptor set，勿与 GameView 共用）
    if (!g_TexturePool->GetTexture("sky_hdr")) g_TexturePool->LoadHDRCubemap("sky_hdr", EngineConfig::GetEngineTexturePath("skybox") + "/EnvironmentMap/snow_field_puresky_1k.hdr");
    const TextureInfo* skyHDR2 = g_TexturePool->GetTexture("sky_hdr");
    const TextureInfo* skyIrr2 = g_TexturePool->GetTexture("sky_hdr_irr");
    VkImageView sceneSkyCube = g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyCubeView() : (skyHDR2 ? skyHDR2->imageView : VK_NULL_HANDLE);
    VkSampler sceneSkyCubeSamp = g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyCubeSampler() : g_TexturePool->GetSamplerByType(SamplerType::Linear);
    CascadeShadowRenderer* csmScene = g_SceneRenderer.EnsureCascadeShadows();
    g_SceneCompositeQuad.UpdateDescriptorSet(g_SceneRenderTarget.GetColorImageView(), g_SceneRenderTarget.GetDepthImageView(), g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyImageView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkySampler() : VK_NULL_HANDLE, g_SceneRenderTarget.GetColorImageView(1), g_SceneRenderTarget.GetColorImageView(2), (g_TexturePool->GetTexture("end_sky")) ? g_TexturePool->GetTexture("end_sky")->imageView : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetTransmittanceView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetScatteringView() : VK_NULL_HANDLE, sceneSkyCube, sceneSkyCubeSamp, skyIrr2 ? skyIrr2->imageView : (skyHDR2 ? skyHDR2->imageView : VK_NULL_HANDLE), g_TexturePool->GetSamplerByType(SamplerType::Linear), GetShIrradianceBuffer(), UpdatePointLightBuffer(), GetSceneClusterGridBuffer(),
        (g_SceneRenderer.EnsurePointShadows() && g_SceneRenderer.EnsurePointShadows()->IsInitialized()) ? g_SceneRenderer.EnsurePointShadows()->GetCubeArrayView() : VK_NULL_HANDLE,
         (csmScene && csmScene->IsInitialized()) ? csmScene->GetArrayView(0) : VK_NULL_HANDLE,
         g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare),
         (csmScene && csmScene->IsInitialized()) ? csmScene->GetCascadeBuffer(0, (int)g_MainWindowData.FrameIndex) : VK_NULL_HANDLE,
         g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutView() : VK_NULL_HANDLE,
         g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutSampler() : VK_NULL_HANDLE);

    csmScene->SetFrameIndex((int)g_MainWindowData.FrameIndex);   // per-frame UBO 双缓冲（帧竞争修复）
    g_SceneRenderer.RenderCascadeShadowMaps(commandBuffer, 0, view, proj, sunDir);
    
    // 场景渲染阶段(2D 游戏:渲染 2D 画布内容供编辑器查看——与游戏视图同世界层/相机)
    g_SceneRenderTarget.BeginRender(commandBuffer);
    // z-prepass（subpass 0，depth-only）：提前写 3D 深度，MRT 几何阶段被遮挡片元在 fragment shader 前剔除
    if (g_EnableZPrepass && !g_SceneRenderTarget.UsesSeparateComposite()) {
        g_SceneRenderer.RenderDepthPrepass(commandBuffer, g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight(), view, proj, true);
    }
    g_SceneRenderTarget.NextSubpass(commandBuffer);   // 兼容调用序列：进入 geometry pass
    // 统一 3D 管线（2D/3D 场景共用，取消 g_SceneIs2D 分支）：
    // 3D 场景 + 2D 世界层（玩法层/精灵）→ G-Buffer → 独立合成 pass → 链 → UI 链后叠加
    if (g_SkyboxRenderer.IsEnabled() && !usePhysicalSky) {
        g_SkyboxRenderer.Render(commandBuffer, view, proj);   // 物理天空时天空由合成 subpass 还原
    }
    // 使用 ECS 渲染系统渲染模型（场景视图，启用可视化；2D 场景无 3D 实体 → 空提交）
    g_SceneRenderer.RenderSceneView(commandBuffer, g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight(), view, proj);
    // 2D 玩法层（普通精灵/Canvas 世界层实体）与 UI：已移到链后 RenderUIOverlay（后处理之外，玩法层在 UI 之前）
    // 结束 geometry pass，开始独立 composite pass；全屏四边形通过普通纹理
    // 采样读取颜色0/深度/法线/材质并写入中间附件。
    g_SceneRenderTarget.NextSubpass(commandBuffer);
    {
        static bool s_pcLogged = false;
        if (!s_pcLogged) {
            s_pcLogged = true;
            glm::mat4 ivp = glm::inverse(proj * view);
            LOGI("[PC] proj: p00=%.4f p11=%.4f p22=%.4f p23=%.4f p32=%.4f p33=%.4f",
                 proj[0][0], proj[1][1], proj[2][2], proj[2][3], proj[3][2], proj[3][3]);
            LOGI("[PC] invViewProj: m00=%.4f m11=%.4f m22=%.4f m23=%.4f m32=%.4f m33=%.4f",
                 ivp[0][0], ivp[1][1], ivp[2][2], ivp[2][3], ivp[3][2], ivp[3][3]);
        }
    }
    g_SceneCompositeQuad.Render(commandBuffer, g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight(),
        glm::inverse(proj * view), glm::vec3(glm::inverse(view)[3]), sunDir, proj, view);
    g_SceneRenderTarget.EndRender(commandBuffer);
    // 独立透明前向粒子 pass：加载 HDR composite，读取几何深度，随后统一进入 bloom/TAA/tonemap/FXAA。
    g_SceneRenderTarget.BeginParticleRender(commandBuffer);
    RenderParticlePass(commandBuffer, g_SceneRenderTarget.GetWidth(),
        g_SceneRenderTarget.GetHeight(), g_SceneRenderTarget.GetParticleRenderPass(),
        0, view, proj, glm::vec3(glm::inverse(view)[3]), g_CurrentTAAJitter);
    g_SceneRenderTarget.EndParticleRender(commandBuffer);
    // 后处理链（配置驱动）：SceneRT composite → 链逐 pass → 显示附件
    CompositeToFinalBarrier(commandBuffer, g_SceneRenderTarget.GetCompositeImage());
    const uint32_t sceneHistoryW = std::max(1u, g_SceneRenderTarget.GetWidth() / 2);
    const uint32_t sceneHistoryH = std::max(1u, g_SceneRenderTarget.GetHeight() / 2);
    const bool sceneGtaoEnabled = g_SceneChain.IsPassEnabled("gtao");
    const bool sceneSsgiEnabled = g_SceneChain.IsPassEnabled("ssgi");
    const bool sceneCloudEnabled = g_SceneChain.IsPassEnabled("cloud_view");
    const bool sceneTaaEnabled = g_SceneChain.IsPassEnabled("taa");
    bool sceneCloudHistoryValid = false;
    if (sceneGtaoEnabled) {
        EnsureAOHistoryTexture(true, sceneHistoryW, sceneHistoryH);
        PrepareAOHistoryForRead(commandBuffer, g_SceneAOHistory, g_SceneAOHistoryNeedsClear);
    }
    if (sceneSsgiEnabled) {
        EnsureSSGIHistoryTexture(true, sceneHistoryW, sceneHistoryH);
        PrepareSSGIHistoryForRead(commandBuffer, g_SceneSSGIHistory, g_SceneSSGIHistoryNeedsClear);
    }
    if (sceneCloudEnabled) {
        EnsureCloudHistoryTexture(true, sceneHistoryW, sceneHistoryH);
        // EnsureCloudHistoryTexture may recreate the image on resize. Capture
        // validity after that check but before Prepare clears a new image.
        sceneCloudHistoryValid = !g_SceneCloudHistoryNeedsClear;
        PrepareCloudHistoryForRead(commandBuffer, g_SceneCloudHistory, g_SceneCloudHistoryNeedsClear);
    }
    if (sceneTaaEnabled) {
        EnsureTAAHistoryTexture(g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight());
        PrepareTAAHistoryForRead(commandBuffer, g_SceneTAAHistory, g_SceneChain.GetPassOutputImage("taa"));   // TAA：上帧输出→历史（帧首串行，防 3 帧 in-flight 竞态）
    }
    PostProcessChain::ExternalInputs ext;
    ext.compositeView = g_SceneRenderTarget.GetCompositeImageView();
    ext.depthView = g_SceneRenderTarget.GetDepthImageView();
    ext.gbuffer1View = g_SceneRenderTarget.GetColorImageView(1);
    ext.gbuffer2View = g_SceneRenderTarget.GetColorImageView(2);
    ext.gbufferView = g_SceneRenderTarget.GetColorImageView(0);   // gbuffer0（gtao_apply 重建 emissive 用 albedo）
    ext.skyView = g_AtmosphereRenderer.GetSkyImageView();   // skyrt（gtao_apply 雾色）
    ext.skySampler = g_AtmosphereRenderer.GetSkySampler();
    FillAtmosphereTransmittanceIntoExt(ext);
    ext.historyView = g_SceneAOHistoryView;   // 时序 GTAO 历史
    ext.historySampler = g_AOHistorySampler;
    ext.ssgiHistoryView = g_SceneSSGIHistoryView;
    ext.ssgiHistorySampler = g_SSGIHistorySampler;
    ext.cloudHistoryView = g_SceneCloudHistoryView;
    ext.cloudHistorySampler = g_CloudHistorySampler;
    ext.taaHistoryView = g_SceneTAAHistoryView;   // TAA：上帧输出历史
    ext.gbufferMotionView = g_SceneRenderTarget.GetColorImageView(3);   // TAA depth-guided：运动向量附件
    ext.taaHistorySampler = g_TAAHistorySampler;
    {
        ext.cmaaWeightView = g_SceneCMAA2.GetWeightView();
        ext.cmaaWeightSampler = g_SceneCMAA2.GetWeightSampler();
    }
    ext.cameraUBO.cameraPos = glm::vec4(glm::vec3(glm::inverse(view)[3]), 1.0f);
    ext.cameraUBO.proj = proj;
    ext.cameraUBO.view = view;
    ext.cameraUBO.prevViewProj = s_PrevViewProj;
    ext.cameraUBO.invProj = glm::inverse(proj);
    ext.cameraUBO.invView = glm::inverse(view);
    ext.cameraUBO.cloudPrevViewProj = s_PrevCloudViewProjScene;
    // CSM 级联数据（gtao 半分辨率体积光采样阴影）——场景 slot 0
    FillCsmIntoUExt(ext, csmScene, 0, g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare));
    FillCloudSettings(g_SceneRenderer.GetRenderWorld(), ext.cameraUBO);
    ext.cameraUBO.cloudPrevWindOffsetKm = glm::vec4(s_PrevCloudWindOffsetScene, 0.0f);
    ext.cameraUBO.cloudHighPrevWindOffsetKm = glm::vec4(s_PrevCloudHighWindOffsetScene, 0.0f);
    ext.cameraUBO.cloudNoiseOffsetKm.w = sceneCloudHistoryValid ? 1.0f : 0.0f;
    ext.pushData.cameraPos = ext.cameraUBO.cameraPos;
    ext.pushData.sunDir = glm::vec4(sunDir, 0.0f);
    ext.pushData.lightColor = glm::vec4(lightColor * lightIntensity, 1.0f);
    ext.pushData.frameInfo = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    g_SceneChain.Execute(commandBuffer, g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight(),
        ext, g_SceneRenderTarget.GetFinalFramebuffer());
    if (sceneGtaoEnabled) {
        CopyAOHistory(commandBuffer, g_SceneChain.GetPassOutputImage("gtao"), g_SceneAOHistory,
            sceneHistoryW, sceneHistoryH);
    }
    if (sceneSsgiEnabled) {
        CopySSGIHistory(commandBuffer, g_SceneChain.GetPassOutputImage("ssgi"), g_SceneSSGIHistory,
            sceneHistoryW, sceneHistoryH);
    }
    if (sceneCloudEnabled) {
        CopyCloudHistory(commandBuffer, g_SceneChain.GetPassOutputImage("cloud_view"),
                         g_SceneCloudHistory, sceneHistoryW, sceneHistoryH);
    }
    s_PrevViewProj = proj * view;
    s_PrevCloudViewProjScene = proj * view;
    s_PrevCloudWindOffsetScene = glm::vec3(ext.cameraUBO.cloudWindOffsetKm);
    s_PrevCloudHighWindOffsetScene = glm::vec3(ext.cameraUBO.cloudHighWindOffsetKm);
    // UI 叠加（链末 tonemap 后）：UI alpha 混合叠加在离屏结果之上，不受后处理/光照影响
    // 无限刻度网格也在此叠加（SceneView 专属：用户拍板画到后处理之后，不进 G-Buffer/合成）
    RenderUIOverlay(commandBuffer, g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight(),
        g_SceneRenderTarget.GetDisplayUIRenderPass(), g_SceneRenderTarget.GetFinalFramebuffer(), false,
        &view, &proj, &view, &proj);
}

// 绘制游戏视图内容（3D 几何 + 2D 世界层 + UI 层 + 游戏 UI）
// 在"已开始的 render pass 的 subpass 0"内调用；编辑器离屏路径与游戏合并路径共用
// usePhysicalSky=true 时：skybox 位置画背景 quad 采样低分辨率天空 RT（物理天空）
void RenderGameContent(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj, bool usePhysicalSky)
{
    // 每帧 2D 渲染开始：重置顶点写入位置（本帧内 RenderWorld/RenderUI/文本多次 Flush 接续写入，
    // 避免后写覆盖先前 draw 引用的顶点数据 —— Vulkan 命令缓冲统一提交后所有 draw 读缓冲最终状态）
    Renderer2D::GetInstance().ResetFrame();

    g_SkyboxRenderer.SyncFromRenderWorld(g_SceneRenderer.GetRenderWorld());
    bool skyboxVisible = g_SkyboxRenderer.IsEnabled();
    // 统一 3D 管线（2D/3D 场景共用）：3D 内容（skybox/模型/体素）2D 场景为空提交，2D 世界层总是绘制
    if (skyboxVisible && !usePhysicalSky) {
        g_SkyboxRenderer.Render(commandBuffer, view, proj);   // 物理天空时天空由合成 subpass 还原
    }
    g_SceneRenderer.RenderGameView(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(), view, proj);
    // 2D 玩法层（世界层）与 UI：已移到链后 RenderUIOverlay（后处理之外，玩法层在 UI 之前）——不经过 G-Buffer/合成
}

// 渲染游戏视图到离屏目标并生成 Hi-ZB（编辑器模式：GameView 面板采样显示附件——统一合成管线）
void RenderGameToTarget(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPos,
                               const glm::vec3& cameraFront, const glm::vec3& cameraRight, const glm::vec3& cameraUp,
                               uint32_t frameIndex)
{
    (void)cameraPos; (void)cameraFront; (void)cameraRight; (void)cameraUp;
    VkCommandBuffer commandBuffer = g_MainWindowData.Frames[g_MainWindowData.FrameIndex].CommandBuffer;
    g_CurrentTAAJitter = g_GameChain.IsPassEnabled("taa") ? ComputeTAAJitter(g_TAAJitterFrameGameView, (float)g_GameRenderTarget.GetWidth(), (float)g_GameRenderTarget.GetHeight()) : glm::vec2(0.0f);
    (void)proj;


    // 物理天空全景图已由 FrameRender 统一渲染（相机无关）——此处只判断 usePhysicalSky
    g_SkyboxRenderer.SyncFromRenderWorld(g_SceneRenderer.GetRenderWorld());
    bool usePhysicalSky = false;
    if (g_SkyboxRenderer.IsEnabled() && g_AtmosphereEnabled && g_AtmosphereRenderer.IsInitialized()) {
        usePhysicalSky = true;
    }

    // 场景方向光收集（函数级：CSM 阴影 + 合成共用；无光源回退太阳方向/白光/1 强度）
    glm::vec3 lightDir, lightColor(1.0f, 0.96f, 0.89f); float lightIntensity = 1.0f;
    glm::vec3 sunDir = g_AtmosphereRenderer.GetSunDirection();
    if (GetSceneDirectionalLight(g_SceneRenderer.GetRenderWorld(), lightDir, lightColor, lightIntensity)) sunDir = lightDir;

    // 合成描述符：GameRT 颜色0/深度 + 天空 RT（独立 descriptor set，勿与 SceneView 共用）
    if (!g_TexturePool->GetTexture("sky_hdr")) g_TexturePool->LoadHDRCubemap("sky_hdr", EngineConfig::GetEngineTexturePath("skybox") + "/EnvironmentMap/snow_field_puresky_1k.hdr");
    const TextureInfo* skyHDR3 = g_TexturePool->GetTexture("sky_hdr");
    const TextureInfo* skyIrr3 = g_TexturePool->GetTexture("sky_hdr_irr");
    VkImageView gameSkyCube = g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyCubeView() : (skyHDR3 ? skyHDR3->imageView : VK_NULL_HANDLE);
    VkSampler gameSkyCubeSamp = g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyCubeSampler() : g_TexturePool->GetSamplerByType(SamplerType::Linear);
    CascadeShadowRenderer* csmGame = g_SceneRenderer.EnsureCascadeShadows();
    g_GameCompositeQuad.UpdateDescriptorSet(g_GameRenderTarget.GetColorImageView(), g_GameRenderTarget.GetDepthImageView(), g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyImageView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkySampler() : VK_NULL_HANDLE, g_GameRenderTarget.GetColorImageView(1), g_GameRenderTarget.GetColorImageView(2), (g_TexturePool->GetTexture("end_sky")) ? g_TexturePool->GetTexture("end_sky")->imageView : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetTransmittanceView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetScatteringView() : VK_NULL_HANDLE, gameSkyCube, gameSkyCubeSamp, skyIrr3 ? skyIrr3->imageView : (skyHDR3 ? skyHDR3->imageView : VK_NULL_HANDLE), g_TexturePool->GetSamplerByType(SamplerType::Linear), GetShIrradianceBuffer(), UpdatePointLightBuffer(), GetGameClusterGridBuffer(),
        (g_SceneRenderer.EnsurePointShadows() && g_SceneRenderer.EnsurePointShadows()->IsInitialized()) ? g_SceneRenderer.EnsurePointShadows()->GetCubeArrayView() : VK_NULL_HANDLE,
         (csmGame && csmGame->IsInitialized()) ? csmGame->GetArrayView(kGameCsmSlot) : VK_NULL_HANDLE,
         g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare),
         (csmGame && csmGame->IsInitialized()) ? csmGame->GetCascadeBuffer(kGameCsmSlot, (int)g_MainWindowData.FrameIndex) : VK_NULL_HANDLE,
         g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutView() : VK_NULL_HANDLE,
         g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutSampler() : VK_NULL_HANDLE);

    csmGame->SetFrameIndex((int)g_MainWindowData.FrameIndex);   // per-frame UBO 双缓冲（帧竞争修复）
    g_SceneRenderer.RenderCascadeShadowMaps(commandBuffer, kGameCsmSlot, view, proj, sunDir);

    g_GameRenderTarget.BeginRender(commandBuffer);
    if (g_EnableZPrepass && !g_GameRenderTarget.UsesSeparateComposite()) {
        g_SceneRenderer.RenderDepthPrepass(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(), view, proj);
    }
    g_GameRenderTarget.NextSubpass(commandBuffer);   // 兼容调用序列：进入 geometry pass
    RenderGameContent(commandBuffer, view, proj, usePhysicalSky);

    // 结束 geometry pass，开始独立 composite pass，写入中间附件。
    g_GameRenderTarget.NextSubpass(commandBuffer);
    {
        static bool s_pcLogged = false;
        if (!s_pcLogged) {
            s_pcLogged = true;
            glm::mat4 ivp = glm::inverse(proj * view);
            LOGI("[PC] proj: p00=%.4f p11=%.4f p22=%.4f p23=%.4f p32=%.4f p33=%.4f",
                 proj[0][0], proj[1][1], proj[2][2], proj[2][3], proj[3][2], proj[3][3]);
            LOGI("[PC] invViewProj: m00=%.4f m11=%.4f m22=%.4f m23=%.4f m32=%.4f m33=%.4f",
                 ivp[0][0], ivp[1][1], ivp[2][2], ivp[2][3], ivp[3][2], ivp[3][3]);
        }
    }
    g_GameCompositeQuad.Render(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
        glm::inverse(proj * view), glm::vec3(glm::inverse(view)[3]), sunDir, proj, view);
    g_GameRenderTarget.EndRender(commandBuffer);
    // 独立透明前向粒子 pass：粒子读几何深度、写入 HDR composite，后续由 Game 后处理链统一处理。
    g_GameRenderTarget.BeginParticleRender(commandBuffer);
    RenderParticlePass(commandBuffer, g_GameRenderTarget.GetWidth(),
        g_GameRenderTarget.GetHeight(), g_GameRenderTarget.GetParticleRenderPass(),
        0, view, proj, glm::vec3(glm::inverse(view)[3]), g_CurrentTAAJitter);
    g_GameRenderTarget.EndParticleRender(commandBuffer);
    // 后处理链（配置驱动）：GameRT composite → 链逐 pass → 显示附件
    CompositeToFinalBarrier(commandBuffer, g_GameRenderTarget.GetCompositeImage());
    const uint32_t gameHistoryW = std::max(1u, g_GameRenderTarget.GetWidth() / 2);
    const uint32_t gameHistoryH = std::max(1u, g_GameRenderTarget.GetHeight() / 2);
    const bool gameGtaoEnabled = g_GameChain.IsPassEnabled("gtao");
    const bool gameSsgiEnabled = g_GameChain.IsPassEnabled("ssgi");
    const bool gameCloudEnabled = g_GameChain.IsPassEnabled("cloud_view");
    const bool gameTaaEnabled = g_GameChain.IsPassEnabled("taa");
    bool gameCloudHistoryValid = false;
    if (gameGtaoEnabled) {
        EnsureAOHistoryTexture(false, gameHistoryW, gameHistoryH);
        PrepareAOHistoryForRead(commandBuffer, g_GameAOHistory, g_GameAOHistoryNeedsClear);
    }
    if (gameSsgiEnabled) {
        EnsureSSGIHistoryTexture(false, gameHistoryW, gameHistoryH);
        PrepareSSGIHistoryForRead(commandBuffer, g_GameSSGIHistory, g_GameSSGIHistoryNeedsClear);
    }
    if (gameCloudEnabled) {
        EnsureCloudHistoryTexture(false, gameHistoryW, gameHistoryH);
        gameCloudHistoryValid = !g_GameCloudHistoryNeedsClear;
        PrepareCloudHistoryForRead(commandBuffer, g_GameCloudHistory, g_GameCloudHistoryNeedsClear);
    }
    if (gameTaaEnabled) {
        EnsureTAAHistoryTexture(g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight());
        PrepareTAAHistoryForRead(commandBuffer, g_GameTAAHistory, g_GameChain.GetPassOutputImage("taa"));   // TAA：上帧输出→历史（帧首串行，防 3 帧 in-flight 竞态）
    }
    PostProcessChain::ExternalInputs ext;
    ext.compositeView = g_GameRenderTarget.GetCompositeImageView();
    ext.depthView = g_GameRenderTarget.GetDepthImageView();
    ext.gbuffer1View = g_GameRenderTarget.GetColorImageView(1);
    ext.gbuffer2View = g_GameRenderTarget.GetColorImageView(2);
    ext.gbufferView = g_GameRenderTarget.GetColorImageView(0);   // gbuffer0（gtao_apply 重建 emissive 用 albedo）
    ext.skyView = g_AtmosphereRenderer.GetSkyImageView();   // skyrt（gtao_apply 雾色）
    ext.skySampler = g_AtmosphereRenderer.GetSkySampler();
    FillAtmosphereTransmittanceIntoExt(ext);
    ext.historyView = g_GameAOHistoryView;   // 时序 GTAO 历史
    ext.historySampler = g_AOHistorySampler;
    ext.ssgiHistoryView = g_GameSSGIHistoryView;
    ext.ssgiHistorySampler = g_SSGIHistorySampler;
    ext.cloudHistoryView = g_GameCloudHistoryView;
    ext.cloudHistorySampler = g_CloudHistorySampler;
    ext.taaHistoryView = g_GameTAAHistoryView;   // TAA：上帧输出历史
    ext.gbufferMotionView = g_GameRenderTarget.GetColorImageView(3);   // TAA depth-guided：运动向量附件
    ext.taaHistorySampler = g_TAAHistorySampler;
    {
        ext.cmaaWeightView = g_GameCMAA2.GetWeightView();
        ext.cmaaWeightSampler = g_GameCMAA2.GetWeightSampler();
    }
    ext.cameraUBO.cameraPos = glm::vec4(glm::vec3(glm::inverse(view)[3]), 1.0f);
    ext.cameraUBO.proj = proj;
    ext.cameraUBO.view = view;
    ext.cameraUBO.prevViewProj = s_PrevViewProj;
    ext.cameraUBO.invProj = glm::inverse(proj);
    ext.cameraUBO.invView = glm::inverse(view);
    ext.cameraUBO.cloudPrevViewProj = s_PrevCloudViewProjGame;
    FillCsmIntoUExt(ext, csmGame, kGameCsmSlot, g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare));
    FillCloudSettings(g_SceneRenderer.GetRenderWorld(), ext.cameraUBO);
    ext.cameraUBO.cloudPrevWindOffsetKm = glm::vec4(s_PrevCloudWindOffsetGame, 0.0f);
    ext.cameraUBO.cloudHighPrevWindOffsetKm = glm::vec4(s_PrevCloudHighWindOffsetGame, 0.0f);
    ext.cameraUBO.cloudNoiseOffsetKm.w = gameCloudHistoryValid ? 1.0f : 0.0f;
    ext.pushData.cameraPos = ext.cameraUBO.cameraPos;
    ext.pushData.sunDir = glm::vec4(sunDir, 0.0f);
    ext.pushData.lightColor = glm::vec4(lightColor * lightIntensity, 1.0f);
    ext.pushData.frameInfo = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    g_GameChain.Execute(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
        ext, g_GameRenderTarget.GetFinalFramebuffer());
    if (gameGtaoEnabled) {
        CopyAOHistory(commandBuffer, g_GameChain.GetPassOutputImage("gtao"), g_GameAOHistory,
            gameHistoryW, gameHistoryH);
    }
    if (gameSsgiEnabled) {
        CopySSGIHistory(commandBuffer, g_GameChain.GetPassOutputImage("ssgi"), g_GameSSGIHistory,
            gameHistoryW, gameHistoryH);
    }
    if (gameCloudEnabled) {
        CopyCloudHistory(commandBuffer, g_GameChain.GetPassOutputImage("cloud_view"),
                         g_GameCloudHistory, gameHistoryW, gameHistoryH);
    }
    s_PrevViewProj = proj * view;
    s_PrevCloudViewProjGame = proj * view;
    s_PrevCloudWindOffsetGame = glm::vec3(ext.cameraUBO.cloudWindOffsetKm);
    s_PrevCloudHighWindOffsetGame = glm::vec3(ext.cameraUBO.cloudHighWindOffsetKm);
    // 保存当前帧 VP 供下一帧 GTAO 重投影
    // UI 叠加（链末 tonemap 后）：UI alpha 混合叠加在 GameRT 显示附件（编辑器 GameView 面板）之上
    RenderUIOverlay(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
        g_GameRenderTarget.GetDisplayUIRenderPass(), g_GameRenderTarget.GetFinalFramebuffer(), false,
        nullptr, nullptr, &view, &proj);

    if (false) {
        VkImage depthImage = g_GameRenderTarget.GetDepthImage();
        uint32_t mipLevels = g_SceneRenderer.GetHiZShader().GetMipLevels();
        if (depthImage != VK_NULL_HANDLE && mipLevels > 0)
            g_SceneRenderer.GetHiZShader().GenerateMipLevels(commandBuffer, depthImage, mipLevels);
    }
}

// 游戏模式：几何写 GameRT G-Buffer（与编辑器 GameView 同一路径）→ 独立合成 pass
// 通过普通纹理采样读 GameRT 颜色0/深度/法线/材质并写入中间附件 → 后处理链 → swapchain
void RenderGameComposite(const glm::mat4& view, const glm::mat4& proj, uint32_t frameIndex)
{
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    VkCommandBuffer commandBuffer = wd->Frames[wd->FrameIndex].CommandBuffer;

#ifdef __ANDROID__
    {
        VkViewport viewport = {};
        viewport.x = 0.0f; viewport.y = 0.0f;
        viewport.width = (float)wd->Width; viewport.height = (float)wd->Height;
        viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        VkRect2D scissor = {};
        scissor.offset = { 0, 0 };
        scissor.extent = { (uint32_t)wd->Width, (uint32_t)wd->Height };
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

        // 场景方向光（合成光照用；无光源回退默认太阳方向/白光 1 强度）
        glm::vec3 lightDir, lightColor(1.0f, 0.96f, 0.89f); float lightIntensity = 1.0f;
        glm::vec3 sunDir = g_AtmosphereRenderer.GetSunDirection();
        if (GetSceneDirectionalLight(g_SceneRenderer.GetRenderWorld(), lightDir, lightColor, lightIntensity)) sunDir = lightDir;

        // 合成 quad descriptor（G-Buffer textures + 天空/IBL/光源/CSM）。
        // CSM 先绑定当前帧对应的 UBO 槽；阴影图在主场景 render pass 前准备并转为可采样布局。
        CascadeShadowRenderer* csmGame0 = g_SceneRenderer.EnsureCascadeShadows();
        g_GameCompositeQuad.UpdateDescriptorSet(
            g_GameRenderTarget.GetColorImageView(), g_GameRenderTarget.GetDepthImageView(),
            g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyImageView() : VK_NULL_HANDLE,
            g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkySampler() : VK_NULL_HANDLE,
            g_GameRenderTarget.GetColorImageView(1), g_GameRenderTarget.GetColorImageView(2),
            (g_TexturePool->GetTexture("end_sky")) ? g_TexturePool->GetTexture("end_sky")->imageView : VK_NULL_HANDLE,
            g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetTransmittanceView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetScatteringView() : VK_NULL_HANDLE,
            (g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyCubeView() : VK_NULL_HANDLE),
            g_TexturePool->GetSamplerByType(SamplerType::Linear),
            (g_TexturePool->GetTexture("sky_hdr_irr")) ? g_TexturePool->GetTexture("sky_hdr_irr")->imageView : VK_NULL_HANDLE,
            g_TexturePool->GetSamplerByType(SamplerType::Linear),
            GetShIrradianceBuffer(), UpdatePointLightBuffer(), GetGameClusterGridBuffer(),
            (g_SceneRenderer.EnsurePointShadows() && g_SceneRenderer.EnsurePointShadows()->IsInitialized()) ? g_SceneRenderer.EnsurePointShadows()->GetCubeArrayView() : VK_NULL_HANDLE,
            (csmGame0 && csmGame0->IsInitialized()) ? csmGame0->GetArrayView(0) : VK_NULL_HANDLE,
            g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare),
            (csmGame0 && csmGame0->IsInitialized()) ? csmGame0->GetCascadeBuffer(0, (int)g_MainWindowData.FrameIndex) : VK_NULL_HANDLE,
            g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutView() : VK_NULL_HANDLE,
            g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutSampler() : VK_NULL_HANDLE);

        // 物理天空（大气渲染）：合成 pass 前生成 skyRT（compute dispatch LUT + pano→cube IBL）
        if (g_SkyboxRenderer.IsEnabled() && g_AtmosphereEnabled && g_AtmosphereRenderer.IsInitialized()) {
            g_AtmosphereRenderer.RenderSkyRT(commandBuffer, sunDir, glm::vec3(glm::inverse(view)[3]));
        }

        // 必须先设置同一帧的 CSM UBO 槽，再由 RenderCascadeShadowMaps 更新级联矩阵并完成 depth→shader-read barrier。
        if (csmGame0 && csmGame0->IsInitialized()) {
            csmGame0->SetFrameIndex((int)g_MainWindowData.FrameIndex);
            g_SceneRenderer.RenderCascadeShadowMaps(commandBuffer, 0, view, proj, sunDir);
            static int s_mobileCsmDiag = 0;
            if (s_mobileCsmDiag < 3) {
                s_mobileCsmDiag++;
                LOGI("[CSM-DIAG] android render=1 slot=0 frame=%u arrayView=%p ubo=%p",
                    g_MainWindowData.FrameIndex, (void*)csmGame0->GetArrayView(0),
                    (void*)csmGame0->GetCascadeBuffer(0, (int)g_MainWindowData.FrameIndex));
            }
        } else {
            static bool s_mobileCsmUnavailableLogged = false;
            if (!s_mobileCsmUnavailableLogged) {
                s_mobileCsmUnavailableLogged = true;
                LOGE("[CSM-DIAG] android renderer unavailable; CSM sampling remains inactive");
            }
        }

        // TAA 开启时，在几何阶段加入 Halton 亚像素抖动；TAA 关闭时保持零偏移。
        // 该值由 model.vert 只作用于光栅化 xy，运动矢量仍使用无抖动的真实位置。
        g_CurrentTAAJitter = g_SwapChain.IsPassEnabled("taa")
            ? ComputeTAAJitter(g_TAAJitterFrameGame, (float)g_GameRenderTarget.GetWidth(),
                               (float)g_GameRenderTarget.GetHeight())
            : glm::vec2(0.0f);

        // 几何 render pass（单 subpass）：GameRT 写 G-Buffer（4 颜色附件 + depth）
        // 跳过 z-prepass——几何 subpass 自身做深度测试，正确性不受影响（Adreno 多 subpass 的 vkCreateRenderPass 即崩）
        g_GameRenderTarget.BeginRender(commandBuffer);
        RenderGameContent(commandBuffer, view, proj, false);
        g_GameRenderTarget.EndRender(commandBuffer);

        // 分离合成通道（独立单 subpass render pass）：全屏四边形 texture 采样 G-Buffer → 光照 → composite
        g_GameRenderTarget.BeginCompositeRender(commandBuffer);
        g_GameCompositeQuad.Render(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
            glm::inverse(proj * view), glm::vec3(glm::inverse(view)[3]), sunDir, proj, view, glm::vec4(lightColor * lightIntensity, 1.0f));
        // 安卓独立合成 pass 仍复用同一份深度附件；粒子在合成 pass 内做只读深度测试。
        RenderParticlePass(commandBuffer, g_GameRenderTarget.GetWidth(),
            g_GameRenderTarget.GetHeight(), g_GameRenderTarget.GetCompositeRenderPass(),
            0, view, proj, glm::vec3(glm::inverse(view)[3]), g_CurrentTAAJitter);
        g_GameRenderTarget.EndCompositeRender(commandBuffer);

        // 完整移动端后处理链：composite → TAA → bloom → tonemap → FXAA → swapchain。
        // composite 仍保持 COLOR_ATTACHMENT_OPTIMAL，由链首采样；不要用直出 blit 绕过链。
        CompositeToFinalBarrier(commandBuffer, g_GameRenderTarget.GetCompositeImage());
        const bool mobileGtaoEnabled = g_SwapChain.IsPassEnabled("gtao");
        const bool mobileTaaEnabled = g_SwapChain.IsPassEnabled("taa");
        const bool mobileCloudEnabled = g_SwapChain.IsPassEnabled("cloud_view");
        const uint32_t mobileAOHistoryW = std::max(1u, static_cast<uint32_t>(wd->Width) / 2);
        const uint32_t mobileAOHistoryH = std::max(1u, static_cast<uint32_t>(wd->Height) / 2);
        bool mobileCloudHistoryValid = false;
        glm::vec3 mobileCloudWindOffset(0.0f);
        glm::vec3 mobileCloudHighWindOffset(0.0f);
        if (mobileGtaoEnabled) {
            // gtao 是移动链中的半分辨率 pass；历史尺寸必须与其输出附件一致。
            EnsureAOHistoryTexture(false, mobileAOHistoryW, mobileAOHistoryH);
            PrepareAOHistoryForRead(commandBuffer, g_GameAOHistory, g_GameAOHistoryNeedsClear);
        }
        if (mobileCloudEnabled) {
            EnsureCloudHistoryTexture(false, mobileAOHistoryW, mobileAOHistoryH);
            mobileCloudHistoryValid = !g_GameCloudHistoryNeedsClear;
            PrepareCloudHistoryForRead(commandBuffer, g_GameCloudHistory, g_GameCloudHistoryNeedsClear);
        }
        bool mobileTaaHistoryValid = true;
        const glm::vec2 previousMobileTaaJitter = g_PreviousTAAJitterGame;
        if (mobileTaaEnabled) {
            EnsureTAAHistoryTexture(g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight());
            mobileTaaHistoryValid = !g_TAAHistoryNeedsClear;
            // 当前链执行前，把上一帧 TAA 输出复制到常驻历史纹理；首帧自动 clear。
            PrepareTAAHistoryForRead(commandBuffer, g_GameTAAHistory,
                                     g_SwapChain.GetPassOutputImage("taa"));
        }
        {
            PostProcessChain::ExternalInputs ext;
            ext.compositeView = g_GameRenderTarget.GetCompositeImageView();
            ext.depthView = g_GameRenderTarget.GetDepthImageView();
            ext.gbufferView = g_GameRenderTarget.GetColorImageView(0);
            ext.gbuffer1View = g_GameRenderTarget.GetColorImageView(1);
            ext.gbuffer2View = g_GameRenderTarget.GetColorImageView(2);
            ext.gbufferMotionView = g_GameRenderTarget.GetColorImageView(3);
            ext.skyView = g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyImageView() : VK_NULL_HANDLE;
            ext.skySampler = g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkySampler() : VK_NULL_HANDLE;
            FillAtmosphereTransmittanceIntoExt(ext);
            // Android 的 GTAO/TAA 历史与运动矢量由当前 GameRT/常驻纹理提供；其他未启用的时序 pass 保持空句柄。
            ext.historyView = mobileGtaoEnabled ? g_GameAOHistoryView : VK_NULL_HANDLE;
            ext.historySampler = mobileGtaoEnabled ? g_AOHistorySampler : VK_NULL_HANDLE;
            ext.ssgiHistoryView = VK_NULL_HANDLE; ext.ssgiHistorySampler = VK_NULL_HANDLE;
            ext.cloudHistoryView = mobileCloudEnabled ? g_GameCloudHistoryView : VK_NULL_HANDLE;
            ext.cloudHistorySampler = mobileCloudEnabled ? g_CloudHistorySampler : VK_NULL_HANDLE;
            ext.taaHistoryView = mobileTaaEnabled ? g_GameTAAHistoryView : VK_NULL_HANDLE;
            ext.taaHistorySampler = mobileTaaEnabled ? g_TAAHistorySampler : VK_NULL_HANDLE;
            ext.cmaaWeightView = VK_NULL_HANDLE; ext.cmaaWeightSampler = VK_NULL_HANDLE;
            ext.cameraUBO.cameraPos = glm::vec4(glm::vec3(glm::inverse(view)[3]), 1.0f);
            ext.cameraUBO.proj = proj;
            ext.cameraUBO.view = view;
            ext.cameraUBO.prevViewProj = s_PrevViewProj;
            ext.cameraUBO.invProj = glm::inverse(proj);
            ext.cameraUBO.invView = glm::inverse(view);
            ext.cameraUBO.cloudPrevViewProj = s_PrevCloudViewProjGame;
            // GTAO 的 CSM descriptor 与主合成共用本帧已完成 barrier 的阴影图；恢复体积光的 CSM 遮挡采样。
            FillCsmIntoUExt(ext, csmGame0, 0, g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare));
            FillCloudSettings(g_SceneRenderer.GetRenderWorld(), ext.cameraUBO);
            ext.cameraUBO.cloudPrevWindOffsetKm = glm::vec4(s_PrevCloudWindOffsetGame, 0.0f);
            ext.cameraUBO.cloudHighPrevWindOffsetKm = glm::vec4(s_PrevCloudHighWindOffsetGame, 0.0f);
            ext.cameraUBO.cloudNoiseOffsetKm.w = mobileCloudHistoryValid ? 1.0f : 0.0f;
            mobileCloudWindOffset = glm::vec3(ext.cameraUBO.cloudWindOffsetKm);
            mobileCloudHighWindOffset = glm::vec3(ext.cameraUBO.cloudHighWindOffsetKm);
            static int s_mobileGtaoDiag = 0;
            if (mobileGtaoEnabled && s_mobileGtaoDiag < 3) {
                s_mobileGtaoDiag++;
                LOGI("[GTAO-DIAG] enabled=%d historyView=%p historySampler=%p csmView=%p csmEnabled=1",
                    mobileGtaoEnabled ? 1 : 0, (void*)ext.historyView, (void*)ext.historySampler,
                    (void*)ext.csmShadowView);
            }
            ext.pushData.cameraPos = ext.cameraUBO.cameraPos;
            ext.pushData.sunDir = glm::vec4(sunDir, 0.0f);
            ext.pushData.lightColor = glm::vec4(lightColor * lightIntensity, 1.0f);
            // frameInfo.yz = 上一帧 jitter - 当前帧 jitter（NDC），供 TAA
            // 把“无 jitter 运动矢量”映射回上一帧实际的采样位置；w=0 表示首帧历史无效。
            const glm::vec2 jitterDelta = previousMobileTaaJitter - g_CurrentTAAJitter;
            ext.pushData.frameInfo = glm::vec4(
                0.0f, jitterDelta.x, jitterDelta.y, mobileTaaHistoryValid ? 1.0f : 0.0f);
            g_SwapChain.Execute(commandBuffer, wd->Width, wd->Height, ext, g_CompositeFramebuffers[wd->FrameIndex]);
            if (mobileTaaEnabled) {
                // 当前帧 TAA 输出供下一帧重投影使用；TAA 位于 bloom 之前，因此取中间 pass 输出。
                CopyTAAHistory(commandBuffer, g_SwapChain.GetPassOutputImage("taa"), g_GameTAAHistory);
                g_PreviousTAAJitterGame = g_CurrentTAAJitter;
            }
            if (mobileGtaoEnabled) {
                CopyAOHistory(commandBuffer, g_SwapChain.GetPassOutputImage("gtao"), g_GameAOHistory,
                    mobileAOHistoryW, mobileAOHistoryH);
            }
            if (mobileCloudEnabled) {
                CopyCloudHistory(commandBuffer, g_SwapChain.GetPassOutputImage("cloud_view"),
                                 g_GameCloudHistory, mobileAOHistoryW, mobileAOHistoryH);
            }
        }
        // 链末 tonemap/FXAA 后叠加移动端 UI，不改变后处理结果。
        RenderUIOverlay(commandBuffer, wd->Width, wd->Height, g_CompositeUIPass, g_CompositeFramebuffers[wd->FrameIndex], true,
            nullptr, nullptr, &view, &proj);
        s_PrevViewProj = proj * view;
        s_PrevCloudViewProjGame = proj * view;
        s_PrevCloudWindOffsetGame = mobileCloudWindOffset;
        s_PrevCloudHighWindOffsetGame = mobileCloudHighWindOffset;
        return;
    }
#endif
    g_CurrentTAAJitter = g_SwapChain.IsPassEnabled("taa") ? ComputeTAAJitter(g_TAAJitterFrameGame, (float)wd->Width, (float)wd->Height) : glm::vec2(0.0f);
    (void)proj;


    VkViewport viewport = {};
    viewport.x = 0.0f; viewport.y = 0.0f;
    viewport.width = (float)wd->Width; viewport.height = (float)wd->Height;
    viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {(uint32_t)wd->Width, (uint32_t)wd->Height};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    // 物理天空：全景天空图（独立 render pass，在合并 render pass 前；天空图可被 SSR fallback 采样）
    g_SkyboxRenderer.SyncFromRenderWorld(g_SceneRenderer.GetRenderWorld());
    bool usePhysicalSky = false;
    glm::vec3 lightDir, lightColor(1.0f, 0.96f, 0.89f); float lightIntensity = 1.0f;
    glm::vec3 sunDir = g_AtmosphereRenderer.GetSunDirection();
    if (GetSceneDirectionalLight(g_SceneRenderer.GetRenderWorld(), lightDir, lightColor, lightIntensity)) sunDir = lightDir;
    if (g_SkyboxRenderer.IsEnabled() && g_AtmosphereEnabled && g_AtmosphereRenderer.IsInitialized()) {
        usePhysicalSky = true;
        g_AtmosphereRenderer.RenderSkyRT(commandBuffer, sunDir, glm::vec3(glm::inverse(view)[3]));   // 海拔=max(0, 相机y+200)（skyRT 随相机高度实时变化）
    }
    DispatchGameClusterCull(commandBuffer, view, proj,
                        (float)g_GameRenderTarget.GetWidth(), (float)g_GameRenderTarget.GetHeight());

    CascadeShadowRenderer* csmGame0 = g_SceneRenderer.EnsureCascadeShadows();
    g_GameCompositeQuad.UpdateDescriptorSet(g_GameRenderTarget.GetColorImageView(), g_GameRenderTarget.GetDepthImageView(), g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyImageView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkySampler() : VK_NULL_HANDLE, g_GameRenderTarget.GetColorImageView(1), g_GameRenderTarget.GetColorImageView(2), (g_TexturePool->GetTexture("end_sky")) ? g_TexturePool->GetTexture("end_sky")->imageView : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetTransmittanceView() : VK_NULL_HANDLE, g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetScatteringView() : VK_NULL_HANDLE, (g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetSkyCubeView() : VK_NULL_HANDLE), g_TexturePool->GetSamplerByType(SamplerType::Linear), (g_TexturePool->GetTexture("sky_hdr_irr")) ? g_TexturePool->GetTexture("sky_hdr_irr")->imageView : VK_NULL_HANDLE, g_TexturePool->GetSamplerByType(SamplerType::Linear), GetShIrradianceBuffer(), UpdatePointLightBuffer(), GetGameClusterGridBuffer(),
        (g_SceneRenderer.EnsurePointShadows() && g_SceneRenderer.EnsurePointShadows()->IsInitialized()) ? g_SceneRenderer.EnsurePointShadows()->GetCubeArrayView() : VK_NULL_HANDLE,
        (csmGame0 && csmGame0->IsInitialized()) ? csmGame0->GetArrayView(0) : VK_NULL_HANDLE,
        g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare),
        (csmGame0 && csmGame0->IsInitialized()) ? csmGame0->GetCascadeBuffer(0, (int)g_MainWindowData.FrameIndex) : VK_NULL_HANDLE,
        g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutView() : VK_NULL_HANDLE,
        g_AtmosphereRenderer.IsInitialized() ? g_AtmosphereRenderer.GetBRDFLutSampler() : VK_NULL_HANDLE);
    csmGame0->SetFrameIndex((int)g_MainWindowData.FrameIndex);   // per-frame UBO 双缓冲（帧竞争修复）
    g_SceneRenderer.RenderCascadeShadowMaps(commandBuffer, 0, view, proj, sunDir);

    // GameRT geometry RenderPass（与编辑器 GameView 同一路径）——几何/2D/UI → G-Buffer
    g_GameRenderTarget.BeginRender(commandBuffer);
    if (g_EnableZPrepass && !g_GameRenderTarget.UsesSeparateComposite()) {
        g_SceneRenderer.RenderDepthPrepass(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(), view, proj);
    }
    g_GameRenderTarget.NextSubpass(commandBuffer);   // 兼容调用序列：进入 geometry pass
    RenderGameContent(commandBuffer, view, proj, usePhysicalSky);

    // 结束 geometry pass，开始独立 composite pass，写入中间附件。
    g_GameRenderTarget.NextSubpass(commandBuffer);
    g_GameCompositeQuad.Render(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
        glm::inverse(proj * view), glm::vec3(glm::inverse(view)[3]), sunDir, proj, view);
    g_GameRenderTarget.EndRender(commandBuffer);
    // 独立透明前向粒子 pass：在后处理前读取深度并写入 HDR composite。
    g_GameRenderTarget.BeginParticleRender(commandBuffer);
    RenderParticlePass(commandBuffer, g_GameRenderTarget.GetWidth(),
        g_GameRenderTarget.GetHeight(), g_GameRenderTarget.GetParticleRenderPass(),
        0, view, proj, glm::vec3(glm::inverse(view)[3]), g_CurrentTAAJitter);
    g_GameRenderTarget.EndParticleRender(commandBuffer);

    // 后处理链（配置驱动）：GameRT composite → 链逐 pass → swapchain（游戏模式主输出）。
    // GameRT 仍然承载 G-Buffer/合成附件；游戏模式直接执行 SwapChain，避免
    // 再写一套 GameRT 最终附件后由 ImGui 全屏采样，控制面板只作为 UI 叠加。
    CompositeToFinalBarrier(commandBuffer, g_GameRenderTarget.GetCompositeImage());
    // RenderGameComposite 当前只从 RunMode::Game 进入。保留 g_EditorActive
    // 的兼容分支，避免将来被其他调用方复用时错误选择输出链。
    const bool useSwapChainOutput = (g_RunMode == RunMode::Game) || !g_EditorActive;
    const uint32_t activeWidth = useSwapChainOutput
        ? ((wd->Width > 0) ? static_cast<uint32_t>(wd->Width) : g_GameRenderTarget.GetWidth())
        : g_GameRenderTarget.GetWidth();
    const uint32_t activeHeight = useSwapChainOutput
        ? ((wd->Height > 0) ? static_cast<uint32_t>(wd->Height) : g_GameRenderTarget.GetHeight())
        : g_GameRenderTarget.GetHeight();
    const uint32_t activeHistoryW = std::max(1u, activeWidth / 2);
    const uint32_t activeHistoryH = std::max(1u, activeHeight / 2);
    const bool activeGtaoEnabled = useSwapChainOutput
        ? g_SwapChain.IsPassEnabled("gtao")
        : g_GameChain.IsPassEnabled("gtao");
    const bool activeSsgiEnabled = useSwapChainOutput
        ? g_SwapChain.IsPassEnabled("ssgi")
        : g_GameChain.IsPassEnabled("ssgi");
    const bool activeCloudEnabled = useSwapChainOutput
        ? g_SwapChain.IsPassEnabled("cloud_view")
        : g_GameChain.IsPassEnabled("cloud_view");
    const bool activeTaaEnabled = useSwapChainOutput
        ? g_SwapChain.IsPassEnabled("taa")
        : g_GameChain.IsPassEnabled("taa");
    bool activeCloudHistoryValid = false;
    if (activeGtaoEnabled) {
        EnsureAOHistoryTexture(false, activeHistoryW, activeHistoryH);
        PrepareAOHistoryForRead(commandBuffer, g_GameAOHistory, g_GameAOHistoryNeedsClear);
    }
    if (activeSsgiEnabled) {
        EnsureSSGIHistoryTexture(false, activeHistoryW, activeHistoryH);
        PrepareSSGIHistoryForRead(commandBuffer, g_GameSSGIHistory, g_GameSSGIHistoryNeedsClear);
    }
    if (activeCloudEnabled) {
        EnsureCloudHistoryTexture(false, activeHistoryW, activeHistoryH);
        activeCloudHistoryValid = !g_GameCloudHistoryNeedsClear;
        PrepareCloudHistoryForRead(commandBuffer, g_GameCloudHistory, g_GameCloudHistoryNeedsClear);
    }

    if (activeTaaEnabled) {
        if (useSwapChainOutput) {
            const uint32_t histW = (wd->Width > 0) ? (uint32_t)wd->Width : g_GameRenderTarget.GetWidth();
            const uint32_t histH = (wd->Height > 0) ? (uint32_t)wd->Height : g_GameRenderTarget.GetHeight();
            EnsureTAAHistoryTexture(histW, histH);
            PrepareTAAHistoryForRead(commandBuffer, g_GameTAAHistory, g_SwapChain.GetPassOutputImage("taa"));
        } else {
            EnsureTAAHistoryTexture(g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight());
            PrepareTAAHistoryForRead(commandBuffer, g_GameTAAHistory, g_GameChain.GetPassOutputImage("taa"));
        }
    }
    PostProcessChain::ExternalInputs ext;
    ext.compositeView = g_GameRenderTarget.GetCompositeImageView();
    ext.depthView = g_GameRenderTarget.GetDepthImageView();
    ext.gbuffer1View = g_GameRenderTarget.GetColorImageView(1);
    ext.gbuffer2View = g_GameRenderTarget.GetColorImageView(2);
    ext.gbufferView = g_GameRenderTarget.GetColorImageView(0);   // gbuffer0（gtao_apply 重建 emissive 用 albedo）
    ext.skyView = g_AtmosphereRenderer.GetSkyImageView();   // skyrt（gtao_apply 雾色）
    ext.skySampler = g_AtmosphereRenderer.GetSkySampler();
    FillAtmosphereTransmittanceIntoExt(ext);
    ext.historyView = g_GameAOHistoryView;   // 时序 GTAO 历史
    ext.historySampler = g_AOHistorySampler;
    ext.ssgiHistoryView = g_GameSSGIHistoryView;
    ext.ssgiHistorySampler = g_SSGIHistorySampler;
    ext.cloudHistoryView = g_GameCloudHistoryView;
    ext.cloudHistorySampler = g_CloudHistorySampler;
    ext.taaHistoryView = g_GameTAAHistoryView;   // TAA：上帧输出历史
    ext.gbufferMotionView = g_GameRenderTarget.GetColorImageView(3);   // TAA depth-guided：运动向量附件
    ext.taaHistorySampler = g_TAAHistorySampler;
    {
        ext.cmaaWeightView = g_GameCMAA2.GetWeightView();
        ext.cmaaWeightSampler = g_GameCMAA2.GetWeightSampler();
    }
    ext.cameraUBO.cameraPos = glm::vec4(glm::vec3(glm::inverse(view)[3]), 1.0f);
    ext.cameraUBO.proj = proj;
    ext.cameraUBO.view = view;
    ext.cameraUBO.prevViewProj = s_PrevViewProj;
    ext.cameraUBO.invProj = glm::inverse(proj);
    ext.cameraUBO.invView = glm::inverse(view);
    ext.cameraUBO.cloudPrevViewProj = s_PrevCloudViewProjGame;
    FillCsmIntoUExt(ext, csmGame0, 0, g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare));
    FillCloudSettings(g_SceneRenderer.GetRenderWorld(), ext.cameraUBO);
    ext.cameraUBO.cloudPrevWindOffsetKm = glm::vec4(s_PrevCloudWindOffsetGame, 0.0f);
    ext.cameraUBO.cloudHighPrevWindOffsetKm = glm::vec4(s_PrevCloudHighWindOffsetGame, 0.0f);
    ext.cameraUBO.cloudNoiseOffsetKm.w = activeCloudHistoryValid ? 1.0f : 0.0f;
    ext.pushData.cameraPos = ext.cameraUBO.cameraPos;
    ext.pushData.sunDir = glm::vec4(sunDir, 0.0f);
    ext.pushData.lightColor = glm::vec4(lightColor * lightIntensity, 1.0f);
    ext.pushData.frameInfo = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    if (!useSwapChainOutput) {
        g_GameChain.Execute(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
            ext, g_GameRenderTarget.GetFinalFramebuffer());
        if (activeGtaoEnabled) {
            CopyAOHistory(commandBuffer, g_GameChain.GetPassOutputImage("gtao"), g_GameAOHistory,
                activeHistoryW, activeHistoryH);
        }
        if (activeSsgiEnabled) {
            CopySSGIHistory(commandBuffer, g_GameChain.GetPassOutputImage("ssgi"), g_GameSSGIHistory,
                activeHistoryW, activeHistoryH);
        }
        if (activeCloudEnabled) {
            CopyCloudHistory(commandBuffer, g_GameChain.GetPassOutputImage("cloud_view"),
                             g_GameCloudHistory, activeHistoryW, activeHistoryH);
        }
        // 兼容非游戏模式复用：GameRT 最终附件上的游戏 UI。
        RenderUIOverlay(commandBuffer, g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight(),
            g_GameRenderTarget.GetDisplayUIRenderPass(), g_GameRenderTarget.GetFinalFramebuffer(), false,
            nullptr, nullptr, &view, &proj);
    }
    if (useSwapChainOutput) {
        g_SwapChain.Execute(commandBuffer, wd->Width, wd->Height, ext, g_CompositeFramebuffers[wd->FrameIndex]);
        if (activeGtaoEnabled) {
            CopyAOHistory(commandBuffer, g_SwapChain.GetPassOutputImage("gtao"), g_GameAOHistory,
                activeHistoryW, activeHistoryH);
        }
        if (activeSsgiEnabled) {
            CopySSGIHistory(commandBuffer, g_SwapChain.GetPassOutputImage("ssgi"), g_GameSSGIHistory,
                activeHistoryW, activeHistoryH);
        }
        if (activeCloudEnabled) {
            CopyCloudHistory(commandBuffer, g_SwapChain.GetPassOutputImage("cloud_view"),
                             g_GameCloudHistory, activeHistoryW, activeHistoryH);
        }
    }
    // 游戏模式的 UI 直接叠加到 swapchain；控制面板的 ImGui pass 随后以 LOAD 方式继续叠加。
    if (useSwapChainOutput) {
        RenderUIOverlay(commandBuffer, wd->Width, wd->Height, g_CompositeUIPass, g_CompositeFramebuffers[wd->FrameIndex], true,
            nullptr, nullptr, &view, &proj);
    }

    // GameChain/SwapChain 都在上面完成了本帧的唯一游戏输出。提交同一帧
    // 的 VP 与云风偏移，下一帧的 TAA/GTAO/cloud reprojection 才不会继续
    // 使用编辑器帧或初始化时的旧矩阵。
    const glm::mat4 currentGameViewProj = proj * view;
    s_PrevViewProj = currentGameViewProj;
    s_PrevCloudViewProjGame = currentGameViewProj;
    // 记录本次实际送入云 pass 的位移；下一帧历史重投影会用它补回云的运动。
    s_PrevCloudWindOffsetGame = glm::vec3(ext.cameraUBO.cloudWindOffsetKm);
    s_PrevCloudHighWindOffsetGame = glm::vec3(ext.cameraUBO.cloudHighWindOffsetKm);

    if (false) {
        VkImage depthImage = g_GameRenderTarget.GetDepthImage();
        uint32_t mipLevels = g_SceneRenderer.GetHiZShader().GetMipLevels();
        if (depthImage != VK_NULL_HANDLE && mipLevels > 0)
            g_SceneRenderer.GetHiZShader().GenerateMipLevels(commandBuffer, depthImage, mipLevels);
    }
}

