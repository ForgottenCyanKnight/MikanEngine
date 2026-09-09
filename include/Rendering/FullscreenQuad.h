#pragma once
#include "Platform/Export.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <stdint.h>
#include <string>

class RenderTarget;

// 全屏四边形：将离屏 G-Buffer 合成到 subpass 1 输出附件（composite）。
// 通过 push constant（invViewProj）按视线方向采样全景天空图（等距柱状投影）。
// 采样方式宏编译分支：桌面 texture（本机 NVIDIA 驱动 subpassLoad 返回 0）/ 移动端 subpassLoad（tile 内存省带宽）
class MIKAN_API FullscreenQuad {
public:
    FullscreenQuad();
    ~FullscreenQuad();

#ifdef __ANDROID__
#define MIKAN_COMPOSITE_SHADER "fullscreen_subpass.frag.spv"   // 移动端：subpassLoad（tile 内存省带宽）
#else
#define MIKAN_COMPOSITE_SHADER "fullscreen.frag.spv"           // 桌面：texture 采样（本机 NVIDIA 驱动 subpassLoad 返回 0）
#endif
    void Init(VkRenderPass renderPass, uint32_t subpass = 1, const char* fragShaderName = MIKAN_COMPOSITE_SHADER);
    void Cleanup();
    
    void Render(VkCommandBuffer commandBuffer, int width, int height, const glm::mat4& invViewProj, const glm::vec3& cameraPos, const glm::vec3& sunDir,
                const glm::mat4& proj = glm::mat4(1.0f), const glm::mat4& view = glm::mat4(1.0f),
                const glm::vec4& lightColor = glm::vec4(1.0f));
    VkPipeline GetPipeline() const { return m_Pipeline; }
    VkDescriptorSet GetDescriptorSet() const { return m_DescriptorSet; }
    
    // 绑定合成 subpass 的 input attachments（离屏颜色0 + 深度 + 法线 + 材质）与天空 RT 采样器
    // skyImageView/skySampler 为 null 时用颜色0/fallback sampler 占位（2D 场景深度判据恒 false，不会采样）
    // galaxyView：银河全景（end_sky.png——LogLuv32 编码）——null 时用颜色0 占位（引擎资产强制，正常必有）
    void UpdateDescriptorSet(VkImageView colorInputView, VkImageView depthInputView,
                             VkImageView skyImageView, VkSampler skySampler,
                             VkImageView normalView = VK_NULL_HANDLE,
                             VkImageView materialView = VK_NULL_HANDLE,
                             VkImageView galaxyView = VK_NULL_HANDLE,
                             VkImageView transmittanceView = VK_NULL_HANDLE,
                             VkImageView scatteringView = VK_NULL_HANDLE,
                             VkImageView skyCubeView = VK_NULL_HANDLE,
                             VkSampler skyCubeSampler = VK_NULL_HANDLE,
                             VkImageView skyIrradianceView = VK_NULL_HANDLE,
                             VkSampler skyIrradianceSampler = VK_NULL_HANDLE,
                             VkBuffer shBuffer = VK_NULL_HANDLE,
                             VkBuffer pointLightBuffer = VK_NULL_HANDLE,
                             VkBuffer clusterGridBuffer = VK_NULL_HANDLE,
                             VkImageView shadowCubeView = VK_NULL_HANDLE,
                             VkImageView csmView = VK_NULL_HANDLE,
                             VkSampler shadowSampler = VK_NULL_HANDLE,
                             VkBuffer csmBuffer = VK_NULL_HANDLE,
                             VkImageView brdfLutView = VK_NULL_HANDLE,
                             VkSampler brdfLutSampler = VK_NULL_HANDLE);

private:
    void CreatePipeline(VkRenderPass renderPass, uint32_t subpass);
    void CreateDescriptorSetLayout();
    void CreateDescriptorPool();
    void CreateDescriptorSet();

    VkPipeline m_Pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
    
    VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
    
    // 缓存上一次的输入，避免重复更新描述符集
    VkImageView m_CachedImageView = VK_NULL_HANDLE;
    VkImageView m_CachedDepthView = VK_NULL_HANDLE;
    VkImageView m_CachedSkyView = VK_NULL_HANDLE;
    VkSampler m_CachedSkySampler = VK_NULL_HANDLE;
    VkImageView m_CachedNormalView = VK_NULL_HANDLE;   // binding 3 法线（R16G16_SNORM）
VkImageView m_CachedMaterialView = VK_NULL_HANDLE; // binding 4 材质（xyz=metallic/roughness/ao, w=自发光）
    VkImageView m_CachedGalaxyView = VK_NULL_HANDLE;   // binding 5 银河（end_sky 全景——LogLuv32 编码）
    VkImageView m_CachedTransmittanceView = VK_NULL_HANDLE;
    VkImageView m_CachedScatteringView = VK_NULL_HANDLE;
    VkImageView m_CachedSkyCubeView = VK_NULL_HANDLE;
    VkSampler m_CachedSkyCubeSampler = VK_NULL_HANDLE;
    VkImageView m_CachedSkyIrradianceView = VK_NULL_HANDLE;
    VkSampler m_CachedSkyIrradianceSampler = VK_NULL_HANDLE;
    VkBuffer m_CachedShIrradianceBuffer = VK_NULL_HANDLE;
    VkBuffer m_CachedPointLightBuffer = VK_NULL_HANDLE;
    VkBuffer m_CachedClusterGridBuffer = VK_NULL_HANDLE;
    VkImageView m_CachedShadowCubeView = VK_NULL_HANDLE;
    VkImageView m_CachedCsmView = VK_NULL_HANDLE;
    VkSampler m_CachedCsmSampler = VK_NULL_HANDLE;
    VkBuffer m_CachedCsmBuffer = VK_NULL_HANDLE;
    VkImageView m_CachedBrdfLutView = VK_NULL_HANDLE;
    VkSampler m_CachedBrdfLutSampler = VK_NULL_HANDLE;
    VkSampler m_FallbackSampler = VK_NULL_HANDLE;  // 天空 RT 未初始化时的占位采样器
    
    bool m_Initialized = false;
#ifdef __ANDROID__
    std::string m_FragShaderName = "fullscreen_subpass.frag.spv";   // 移动端：subpassLoad
#else
    std::string m_FragShaderName = "fullscreen.frag.spv";           // 桌面：texture
#endif
};
