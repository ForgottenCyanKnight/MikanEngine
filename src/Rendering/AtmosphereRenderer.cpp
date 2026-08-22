#include "AtmosphereRenderer.h"
#include "Core/VulkanContext.h"
#include "Core/Log.h"

#include <iostream>
#include <chrono>

AtmosphereRenderer::~AtmosphereRenderer()
{
    Cleanup();
}

void AtmosphereRenderer::Cleanup()
{
    m_LUT.Cleanup();
    m_Initialized = false;
}

void AtmosphereRenderer::Init(uint32_t winWidth, uint32_t winHeight)
{
    if (m_Initialized) Cleanup();

    // 全景天空 RT：128x64 圆柱投影（2026-08-11 用户拍板恢复——pow4 动态映射海拔参数化：低海拔地平线细腻、
    // 高海拔 t 随海拔移动放大有效区域；cubemap IBL 设施改走独立着色器 atmo_sky_cube.comp）
    uint32_t skyW = 128;   // 圆柱投影（方位 128×仰角 64）
    uint32_t skyH = 64;
    if (!m_LUT.Init(g_Device, g_PhysicalDevice, skyW, skyH)) {
        fprintf(stderr, "[AtmosphereRenderer] AtmosphereLUT init failed\n");
        LOGI("[AtmosphereRenderer] AtmosphereLUT init FAILED");
        return;
    }
    if (!m_LUT.Generate(g_CommandPool, g_Queue)) {
        fprintf(stderr, "[AtmosphereRenderer] LUT generate failed\n");
        LOGI("[AtmosphereRenderer] LUT generate FAILED");
        return;
    }

    m_Initialized = true;
    printf("[AtmosphereRenderer] initialized (compute): skyRT=%ux%u\n", skyW, skyH);
    LOGI("[AtmosphereRenderer] initialized (compute): skyRT=%ux%u", skyW, skyH);
}
void AtmosphereRenderer::RenderSkyRT(VkCommandBuffer commandBuffer, const glm::vec3& sunDir, const glm::vec3& cameraPos)
{
    if (!m_Initialized) return;
    // 2026-08-11 用户约定：海拔 = max(0, 相机y + 200) 米（相机地面=0 → 200m；负数兜底）
    const float altitude = glm::max(cameraPos.y + 200.0f, 0.0f);
    m_LUT.DispatchSky(commandBuffer, sunDir, altitude);
    // 2026-08-12：IBL 切回物理大气（用户拍板：环境光 = 大气散射 + 太阳光源）——atmo cubemap 生成 + SH 投影（SH 在 DispatchPanoToCube 生成路径内）
    m_LUT.DispatchPanoToCube(commandBuffer, sunDir, altitude);
}
