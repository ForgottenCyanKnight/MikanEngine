#include "AtmosphereRenderer.h"
#include "Core/VulkanContext.h"
#include "Core/Log.h"
#include "Core/EngineConfig.h"
#include "Core/RenderGlobals.h"
#include "Core/VulkanRenderHelpers.h"
#include "Rendering/CloudNoise3D.h"
#include "Rendering/SceneRenderer.h"
#include "Rendering/TexturePool.h"

#include <iostream>
#include <chrono>
#include <algorithm>

namespace {

// 低分辨率 cloudRT 主要供 IBL/反射使用，不需要和主画面同频更新。
// 首次调用立即更新，之后每 2 次调用重算一次并复用上一张云图。
constexpr uint32_t kCloudUpdateInterval = 2u;

bool EnsureCloudPanoResources()
{
    CloudNoise3D& noise = GetCloudNoise3D();
    if (!noise.IsInitialized() &&
        !noise.Initialize(g_Device, g_PhysicalDevice, g_Allocator)) {
        static bool loggedNoiseFailure = false;
        if (!loggedNoiseFailure) {
            loggedNoiseFailure = true;
            LOGW("[AtmosphereRenderer] cloud pano disabled: failed to initialize Nubis textures");
        }
        return false;
    }

    if (g_TexturePool == nullptr) {
        return false;
    }
    if (g_TexturePool->GetTexture("bluenoise") == nullptr &&
        !g_TexturePool->LoadTexture2D(
            "bluenoise", EngineConfig::GetEngineTexturePath("bluenoise.png"),
            SamplerType::NearestRepeat)) {
        static bool loggedBlueNoiseFailure = false;
        if (!loggedBlueNoiseFailure) {
            loggedBlueNoiseFailure = true;
            LOGW("[AtmosphereRenderer] cloud pano disabled: failed to load bluenoise");
        }
        return false;
    }

    return noise.GetImageView() != VK_NULL_HANDLE &&
           noise.GetDetailImageView() != VK_NULL_HANDLE &&
           noise.GetCurlImageView() != VK_NULL_HANDLE &&
           noise.GetHighImageView() != VK_NULL_HANDLE &&
           noise.GetHighMapImageView() != VK_NULL_HANDLE &&
           g_TexturePool->GetImageView("bluenoise") != VK_NULL_HANDLE &&
           g_TexturePool->GetSampler("bluenoise") != VK_NULL_HANDLE;
}

} // namespace

AtmosphereRenderer::~AtmosphereRenderer()
{
    Cleanup();
}

void AtmosphereRenderer::Cleanup()
{
    m_CloudPanoQuad.Cleanup();
    m_CloudRT.Cleanup();
    m_LUT.Cleanup();
    m_CloudFrame = 0;
    m_CloudRTUpdatedThisFrame = false;
    m_Initialized = false;
}

void AtmosphereRenderer::InvalidateCachedResults()
{
    m_LUT.InvalidateCachedResults();
    m_CloudFrame = 0;
    m_CloudRTUpdatedThisFrame = false;
}

void AtmosphereRenderer::Init(uint32_t winWidth, uint32_t winHeight)
{
    if (m_Initialized) Cleanup();

    // 高海拔 t 随海拔移动放大有效区域；cubemap IBL 设施改走独立着色器 atmo_sky_cube.comp）
    uint32_t skyW = 128;   // 圆柱投影（方位 128×仰角 64）
    uint32_t skyH = 64;
    if (!m_LUT.Init(g_Device, g_PhysicalDevice, skyW, skyH)) {
        LOGE("[AtmosphereRenderer] AtmosphereLUT init failed");
        LOGI("[AtmosphereRenderer] AtmosphereLUT init FAILED");
        return;
    }
    if (!m_LUT.Generate(g_CommandPool, g_Queue)) {
        LOGE("[AtmosphereRenderer] LUT generate failed");
        LOGI("[AtmosphereRenderer] LUT generate FAILED");
        return;
    }

    // 云 RT 与背景 skyRT 使用同一方位/仰角投影，但反射用途固定为 256×128。
    // RGBA8 直接保存线性云散射和透射率；低分辨率云 RT 不再引入 LogLuv24
    // 的亮度量化，避免颜色混合时出现 banding 或色偏。
    const uint32_t cloudW = 256u;
    const uint32_t cloudH = 128u;
    m_CloudRT.Init(cloudW, cloudH, false, false, false);
    if (m_CloudRT.GetRenderPass() == VK_NULL_HANDLE ||
        m_CloudRT.GetFramebuffer() == VK_NULL_HANDLE) {
        LOGE("[AtmosphereRenderer] cloud RT init failed");
        m_CloudRT.Cleanup();
        m_LUT.Cleanup();
        return;
    }
    m_CloudPanoQuad.Init(m_CloudRT.GetRenderPass(), 0, "cloud_view.frag.spv", 10);
    if (m_CloudPanoQuad.GetPipeline() == VK_NULL_HANDLE) {
        LOGE("[AtmosphereRenderer] cloud pano pipeline init failed");
        m_CloudPanoQuad.Cleanup();
        m_CloudRT.Cleanup();
        m_LUT.Cleanup();
        return;
    }

    m_LUT.SetCloudEnvironment(
        m_CloudRT.GetColorImage(), m_CloudRT.GetColorImageView(), m_CloudRT.GetSampler());

    m_Initialized = true;
    m_CloudFrame = 0;
    m_CloudRTUpdatedThisFrame = false;
    LOGI("[AtmosphereRenderer] initialized (compute): skyRT=%ux%u cloudRT=%ux%u",
         skyW, skyH, cloudW, cloudH);
}

void AtmosphereRenderer::RenderSkyRT(VkCommandBuffer commandBuffer,
                                     const glm::vec3& sunDir,
                                     const glm::vec3& cameraPos,
                                     const glm::vec4& lightColor)
{
    if (!m_Initialized) return;

    const float altitude = glm::max(cameraPos.y + 200.0f, 0.0f);
    m_LUT.DispatchSky(commandBuffer, sunDir, altitude, lightColor);
}

void AtmosphereRenderer::RenderSkyCubeRT(VkCommandBuffer commandBuffer,
                                         const glm::vec3& sunDir,
                                         const glm::vec3& cameraPos,
                                         const glm::vec4& lightColor)
{
    if (!m_Initialized) return;

    const float altitude = glm::max(cameraPos.y + 200.0f, 0.0f);
    // 场景反射不再烘进 skyCube（改由 SceneReflectionProbe + water_composite
    // 直采），因此 skyCube 只在大气/云变化时重建：IBL 回到纯天空语义，
    // 不再被场景几何污染。
    m_LUT.DispatchPanoToCube(commandBuffer, sunDir, altitude,
                             m_CloudRTUpdatedThisFrame, lightColor);
    m_CloudRTUpdatedThisFrame = false;
}

bool AtmosphereRenderer::RenderCloudRT(VkCommandBuffer commandBuffer,
                                       const glm::vec3& sunDir,
                                       const glm::vec3& cameraPos,
                                       const RenderWorld& world)
{
    m_CloudRTUpdatedThisFrame = false;
    if (!m_Initialized) return false;

    const uint32_t cloudFrame = m_CloudFrame++;
    if ((cloudFrame % kCloudUpdateInterval) != 0u) {
        return false;
    }

    const bool cloudResourcesReady = EnsureCloudPanoResources();
    PostProcessQuad::CameraUBO cloudCamera = {};
    cloudCamera.cameraPos = glm::vec4(cameraPos, 1.0f);
    FillCloudSettings(world, cloudCamera);
    cloudCamera.cloudRenderFlags = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    if (!cloudResourcesReady) {
        // 即使云资源暂时不可用也要写出“无云”RT，保证全屏合成不会采样
        // 未初始化的附件；资源准备好后下一帧自动恢复云积分。
        cloudCamera.cloudParams0.x = 0.0f;
    }

    const VkImageView backgroundView = m_LUT.GetSkyRTView();
    const VkSampler backgroundSampler = m_LUT.GetSkyRTSampler();
    const TextureInfo* blueNoise = g_TexturePool ? g_TexturePool->GetTexture("bluenoise") : nullptr;
    const CloudNoise3D* noise = cloudResourcesReady ? &GetCloudNoise3D() : nullptr;

    // pano 模式不读取背景/历史，但所有槽位仍绑定有效只读图像，以便同一
    // cloud_view 管线在资源降级时也能安全执行。
    std::vector<PostProcessQuad::InputBinding> inputs = {
        { 0, backgroundView, backgroundSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { 1, noise ? noise->GetImageView() : backgroundView,
          noise ? noise->GetSampler() : backgroundSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { 2, blueNoise ? blueNoise->imageView : backgroundView,
          blueNoise ? g_TexturePool->GetSampler("bluenoise") : backgroundSampler,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { 3, backgroundView, backgroundSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { 4, m_LUT.GetTransmittanceView(), m_LUT.GetLUTSampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { 5, noise ? noise->GetDetailImageView() : backgroundView,
          noise ? noise->GetDetailSampler() : backgroundSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { 6, noise ? noise->GetCurlImageView() : backgroundView,
          noise ? noise->GetCurlSampler() : backgroundSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { 7, m_LUT.GetScatteringView(), m_LUT.GetLUTSampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { 8, noise ? noise->GetHighImageView() : backgroundView,
          noise ? noise->GetSampler() : backgroundSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { 9, noise ? noise->GetHighMapImageView() : backgroundView,
          noise ? noise->GetSampler() : backgroundSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }
    };
    m_CloudPanoQuad.SetInputs(inputs);
    m_CloudPanoQuad.UpdateCameraUBO(cloudCamera);

    PostProcessQuad::PushData push = {};
    push.cameraPos = glm::vec4(cameraPos, 1.0f);
    push.sunDir = glm::vec4(glm::normalize(sunDir), kSceneExposure);   // .w = 场景曝光（cloud_view 在散射 rgb 上乘）
    push.lightColor = glm::vec4(1.0f);
    push.frameInfo = glm::vec4(static_cast<float>(cloudFrame % 65536u), 0.0f, 0.0f, 1.0f);

    m_CloudRT.BeginRender(commandBuffer);
    m_CloudPanoQuad.Render(commandBuffer,
                           static_cast<int>(m_CloudRT.GetWidth()),
                           static_cast<int>(m_CloudRT.GetHeight()), &push);
    m_CloudRT.EndRender(commandBuffer);

    m_CloudRTUpdatedThisFrame = true;
    return true;
}

