#pragma once
#include "Platform/Export.h"
#include "RendererBase.h"
#include "AtmosphereLUT.h"
#include "RenderTarget.h"
#include "PostProcessQuad.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

struct RenderWorld;

// 物理天空（Bruneton 2017 预计算大气散射，compute shader 驱动）：
//  - AtmosphereLUT：transmittance LUT（256×64）+ scattering LUT（3D 256×128×32）启动时生成一次
//  - skyRT：固定低分辨率天空全景图（RGBA8 LogLuv32），每帧 compute dispatch 生成，
//    合成 render pass 采样上采样（等距柱状 + pow4 球面映射，与相机无关）
//  - 太阳圆盘由 fullscreen.frag 全分辨率绘制（低分辨率全景图 <1px 无意义）
class MIKAN_API AtmosphereRenderer {
public:
    AtmosphereRenderer() = default;
    ~AtmosphereRenderer();

    void Init(uint32_t winWidth, uint32_t winHeight);
    void Cleanup();
    void InvalidateCachedResults();

    // 渲染物理天空背景 skyRT；天空 RT 不包含体积云。
    void RenderSkyRT(VkCommandBuffer commandBuffer, const glm::vec3& sunDir,
                     const glm::vec3& cameraPos = glm::vec3(0.0f),
                     const glm::vec4& lightColor = glm::vec4(1.0f, 0.96f, 0.89f, 1.0f));

    // 在 cloudRT 完成后，把纯 skyRT 与 cloudRT 重投影合成到 skyCube/SH，
    // 使材质 IBL 的 diffuse/specular 环境本身包含体积云。
    void RenderSkyCubeRT(VkCommandBuffer commandBuffer, const glm::vec3& sunDir,
                         const glm::vec3& cameraPos = glm::vec3(0.0f),
                         const glm::vec4& lightColor = glm::vec4(1.0f, 0.96f, 0.89f, 1.0f));

    // 渲染独立的低分辨率全景云 RT：只输出 rgb=云散射、a=云透射率，
    // 使用与 skyRT 相同的 skylutdir 投影；必须在合成 pass 开始前调用。
    bool RenderCloudRT(VkCommandBuffer commandBuffer, const glm::vec3& sunDir,
                       const glm::vec3& cameraPos, const RenderWorld& world);

    VkImageView GetSkyImageView() const { return m_LUT.GetSkyRTView(); }
    VkSampler GetSkySampler() { return m_LUT.GetSkyRTSampler(); }
    VkImageView GetCloudImageView() const { return m_CloudRT.GetColorImageView(); }
    VkSampler GetCloudSampler() { return m_CloudRT.GetSampler(); }
    VkImageView GetTransmittanceView() const { return m_LUT.GetTransmittanceView(); }
    VkSampler GetTransmittanceSampler() const { return m_LUT.GetLUTSampler(); }   // 太阳/云光照共用 LUT sampler
    VkImageView GetScatteringView() const { return m_LUT.GetScatteringView(); }
    VkImageView GetSkyCubeView() const { return m_LUT.GetSkyCubeView(); }
    VkSampler GetSkyCubeSampler() const { return m_LUT.GetSkyCubeSampler(); }
    VkImageView GetBRDFLutView() const { return m_LUT.GetBRDFLutView(); }
    VkSampler GetBRDFLutSampler() const { return m_LUT.GetBRDFLutSampler(); }
    VkBuffer GetSkyCubeSHBuffer() const { return m_LUT.GetSkyCubeSHBuffer(); }
    bool DumpSHCoefs(const char* tag, VkCommandPool commandPool, VkQueue queue) { return m_LUT.DumpSHCoefs(tag, commandPool, queue); }
    uint32_t GetSkyWidth() const { return m_LUT.GetSkyWidth(); }
    uint32_t GetSkyHeight() const { return m_LUT.GetSkyHeight(); }

    void SetSunDirection(const glm::vec3& dir) { m_SunDir = glm::normalize(dir); }
    const glm::vec3& GetSunDirection() const { return m_SunDir; }
    bool IsInitialized() const { return m_Initialized; }

private:
    AtmosphereLUT m_LUT;               // compute LUT 生成 + 天空全景图 dispatch
    RenderTarget m_CloudRT;            // 独立云全景 RT：RGBA8，RGB=线性云散射，A=透射率
    PostProcessQuad m_CloudPanoQuad;   // 复用 cloud_view.frag 的云积分逻辑
    glm::vec3 m_SunDir = glm::normalize(glm::vec3(0.5f, 0.7f, -0.4f));
    uint32_t m_CloudFrame = 0;
    bool m_CloudRTUpdatedThisFrame = false;
    bool m_Initialized = false;
};
