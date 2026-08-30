#pragma once
#include "Platform/Export.h"
#include "RendererBase.h"
#include "AtmosphereLUT.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

// 物理天空（Bruneton 2017 预计算大气散射，compute shader 驱动）：
//  - AtmosphereLUT：transmittance LUT（256×64）+ scattering LUT（3D 256×128×32）启动时生成一次
//  - skyRT：低分辨率天空全景图（1/4 窗口，RGBA8 LogLuv32），每帧 compute dispatch 生成，
//    合成 render pass 采样上采样（等距柱状 + pow4 球面映射，与相机无关）
//  - 太阳圆盘由 fullscreen.frag 全分辨率绘制（低分辨率全景图 <1px 无意义）
class MIKAN_API AtmosphereRenderer {
public:
    AtmosphereRenderer() = default;
    ~AtmosphereRenderer();

    void Init(uint32_t winWidth, uint32_t winHeight);
    void Cleanup();

    // 渲染物理天空到低分辨率全景 RT（compute dispatch；必须在合成 render pass 开始前调用）
    void RenderSkyRT(VkCommandBuffer commandBuffer, const glm::vec3& sunDir, const glm::vec3& cameraPos = glm::vec3(0.0f));   // 2026-08-11：海拔=max(0, cameraY+200) 实时输入

    VkImageView GetSkyImageView() const { return m_LUT.GetSkyRTView(); }
    VkSampler GetSkySampler() { return m_LUT.GetSkyRTSampler(); }
    VkImageView GetTransmittanceView() const { return m_LUT.GetTransmittanceView(); }   // 2026-08-11：合成 pass 物理太阳透射
    VkSampler GetTransmittanceSampler() const { return m_LUT.GetLUTSampler(); }   // 太阳/云光照共用 LUT sampler
    VkImageView GetScatteringView() const { return m_LUT.GetScatteringView(); }   // 2026-08-11 per-pixel：散射 LUT（GetSkyRadiance）
    VkImageView GetSkyCubeView() const { return m_LUT.GetSkyCubeView(); }   // 2026-08-12：IBL cubemap
    VkSampler GetSkyCubeSampler() const { return m_LUT.GetSkyCubeSampler(); }
    VkImageView GetBRDFLutView() const { return m_LUT.GetBRDFLutView(); }   // 2026-08-15：split-sum BRDF LUT
    VkSampler GetBRDFLutSampler() const { return m_LUT.GetBRDFLutSampler(); }
    VkBuffer GetSkyCubeSHBuffer() const { return m_LUT.GetSkyCubeSHBuffer(); }   // 2026-08-12：SH 辐照度系数 SSBO
    void DumpSHCoefs(const char* tag) { m_LUT.DumpSHCoefs(tag); }
    uint32_t GetSkyWidth() const { return m_LUT.GetSkyWidth(); }
    uint32_t GetSkyHeight() const { return m_LUT.GetSkyHeight(); }

    void SetSunDirection(const glm::vec3& dir) { m_SunDir = glm::normalize(dir); }
    const glm::vec3& GetSunDirection() const { return m_SunDir; }
    bool IsInitialized() const { return m_Initialized; }

private:
    AtmosphereLUT m_LUT;               // compute LUT 生成 + 天空全景图 dispatch
    glm::vec3 m_SunDir = glm::normalize(glm::vec3(0.5f, 0.7f, -0.4f));
    bool m_Initialized = false;
};
