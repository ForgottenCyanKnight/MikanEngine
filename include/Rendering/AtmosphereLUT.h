#pragma once
#include "Platform/Export.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

// ============================================================================
// AtmosphereLUT — Bruneton 2017 预计算大气散射（compute shader 驱动）
//  - transmittance LUT：256×64 RGBA16F（2D storage image，500 步光学厚度积分）
//  - scattering LUT：  256×128×32 RGBA16F（3D storage image，combined 布局：
//                      RGB=单次瑞利+Σ多重（除瑞利相函数）、A=单次 Mie.R）
//  - density LUT：     256×128×32 RGBA16F（3D，每阶散射密度，迭代中间量）
//  - irradiance LUT：  64×16 RGBA16F（2D，地面间接辐照度，逐阶累加）
//  - skyRT：低分辨率天空全景图 RGBA8（LogLuv32 编码，UNORM 可双线性过滤）
// 多次散射：4 阶迭代（density → irradiance → multiscatter 累加进 scattering）
// LUT 生成一次（Generate，启动时）；天空全景图每帧 DispatchSky
// ============================================================================

// ============================================================================
// 场景曝光 kSceneExposure —— 「物理辐射度 → 显示参考 HDR」的唯一定标因子
// ----------------------------------------------------------------------------
// 单一事实来源：本常量被写入每个 pass 的 push constant sunDir.w（各 shader 侧
// 用 pc.sunDir.w 读取），由 shader 在**每一个辐射度生产点**上乘且只乘一次。
//
//   施加点（全部）：
//     ① atmo_sky.comp            → skyRT（含月光项）
//     ② atmo_pano_to_cube.comp   → skyCube 的太阳盘（skyRT/cloudRT 已在①②③里带过）
//     ③ cloud_view.frag          → cloudRT 的散射 rgb（alpha=透射率不缩放！）
//     ④ fullscreen.frag          → 直射日光 sunDiskColor / 月光常数项 / 自发光 /
//                                  银河 / 银河 IBL / 点光源（两个分支）
//     ⑤ gtao_apply.frag          → 太阳盘 / 月亮盘
//     ⑥ tonemap.frag             → 反向归一化（÷kSceneExposure，三种 tonemap 共用）
//     ⑦ bloom_down2x_th.frag     → BLOOM_THRESHOLD ×kSceneExposure
//
// ⚠️ 只让部分生产点乘 ⇒ 那部分光源会比其它亮/暗 10×（不是"更准"，是失衡）。
//    新增任何辐射度来源（新光源类型、新的 LUT、新的 emissive 通道）都必须同样
//    乘 pc.sunDir.w。
// ⚠️ shader 侧没有共享头，tonemap.frag / bloom_down2x_th.frag 内有同值 GLSL 常量
//    （拼作 SCENE_EXPOSURE）。改这里必须同步改那两处——A/B 截图逐像素对照是
//    唯一验收判据（能量守恒 ⇒ 前后图必须一致）。
// ============================================================================
inline constexpr float kSceneExposure = 10.0f;

class MIKAN_API AtmosphereLUT {
public:
    AtmosphereLUT() = default;
    ~AtmosphereLUT();

    bool Init(VkDevice device, VkPhysicalDevice physicalDevice,
              uint32_t skyWidth, uint32_t skyHeight);
    void Cleanup();
    bool IsInitialized() const { return m_Initialized; }

    // LUT 生成（transmittance + scattering；单次提交并等待完成）
    bool Generate(VkCommandPool commandPool, VkQueue queue);

    // Invalidate only frame-dependent caches. Existing images/layouts remain
    // valid; the next dispatch regenerates skyRT, skyCube and SH as needed.
    void InvalidateCachedResults();

    // 每帧：天空全景图 compute dispatch（LogLuv32 → RGBA8）
    // 内部处理 skyRT 布局转换（SHADER_READ_ONLY → GENERAL → SHADER_READ_ONLY）与写读 barrier
    void DispatchSky(VkCommandBuffer commandBuffer, const glm::vec3& sunDir, float altitudeMeters = 200.0f,
                     const glm::vec4& lightColor = glm::vec4(1.0f, 0.96f, 0.89f, 1.0f));
    // 生成包含 cloudRT 的 skyCube；cloudRTUpdated 用于让云层变化主动失效
    // skyCube 的太阳/海拔缓存。
    void DispatchPanoToCube(VkCommandBuffer commandBuffer, const glm::vec3& sunDir,
                            float altitudeMeters = 200.0f,
                            bool cloudRTUpdated = false,
                            const glm::vec4& lightColor = glm::vec4(1.0f, 0.96f, 0.89f, 1.0f));
    void DispatchSHProj(VkCommandBuffer commandBuffer, const glm::vec3& sunDir, float altitudeMeters = 200.0f);
    VkBuffer GetSkyCubeSHBuffer() const { return m_SkyCubeSHBuffer; }   // 合成 binding 10 读（UBO 类型绑 STORAGE|UNIFORM buffer）
    // 调试回读 SH 系数（仅前 144B，最多 5 次）。
    // 该 buffer 位于 DEVICE_LOCAL，CPU 无法直接映射，故内部临时创建 host-visible
    // staging 并做一次 one-shot copy 回读 —— 因此需要调用方提供可用的 command pool / queue。
    // 返回是否真的读到了数据（缺 pool/queue 或复制失败时为 false）。
    bool DumpSHCoefs(const char* tag, VkCommandPool commandPool, VkQueue queue);

    VkImageView GetSkyRTView() const { return m_SkyRTView; }
    // 绑定外部 cloudRT 供 pano→cube 计算采样（RGB=线性云散射，A=透射率）；
    // image 仅用于录制写后读同步，所有权仍由调用方持有。
    void SetCloudEnvironment(VkImage cloudImage, VkImageView cloudView, VkSampler cloudSampler);
    VkImageView GetTransmittanceView() const { return m_TransmittanceView; }
    VkSampler GetLUTSampler() const { return m_LUTSampler; }   // transmittance/scattering 共享的线性 clamp 采样器
    VkImageView GetScatteringView() const { return m_ScatteringView; }
    VkSampler GetSkyRTSampler();
    uint32_t GetSkyWidth() const { return m_SkyW; }
    uint32_t GetSkyHeight() const { return m_SkyH; }

    void DispatchSkyCube(VkCommandBuffer commandBuffer, const glm::vec3& sunDir, float altitudeMeters = 200.0f);
    VkImageView GetSkyCubeView() const { return m_SkyCubeView; }
    VkImageView GetSkyCubeArrayView() const { return m_SkyCubeArrayView; }
    VkSampler GetSkyCubeSampler() const { return m_SkyCubeSampler; }
    uint32_t GetSkyCubeSize() const { return m_SkyCubeW; }
    VkImageView GetBRDFLutView() const { return m_BRDFLutView; }
    VkSampler GetBRDFLutSampler() const { return m_BRDFLutSampler; }

private:
    struct ComputePipeline {
        VkShaderModule module = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    bool CreateImage(uint32_t width, uint32_t height, uint32_t depth, VkFormat format,
                     VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& memory,
                     uint32_t arrayLayers = 1, VkImageCreateFlags flags = 0,
                     uint32_t mipLevels = 1);
    bool CreateSamplers();
    bool CreatePipelines();
    void CreateDescriptors();
    void DestroyPipeline(ComputePipeline& pipe);
    bool ShouldRefreshSkyCube(const glm::vec3& sunDirN, float altitudeMeters);

    void ImageBarrier(VkCommandBuffer cmd, VkImage image,
                      VkImageLayout oldLayout, VkImageLayout newLayout,
                      VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                      VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                      uint32_t baseMip = 0, uint32_t mipCount = 1,
                       uint32_t baseLayer = 0, uint32_t layerCount = 1,
                       VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT);

    VkDevice m_Device = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    uint32_t m_SkyW = 0;
    uint32_t m_SkyH = 0;
    uint32_t m_SkyMips = 1;

    // 纹理
    VkImage m_TransmittanceImage = VK_NULL_HANDLE;
    VkDeviceMemory m_TransmittanceMemory = VK_NULL_HANDLE;
    VkImageView m_TransmittanceView = VK_NULL_HANDLE;
    VkImage m_ScatteringImage = VK_NULL_HANDLE;
    VkDeviceMemory m_ScatteringMemory = VK_NULL_HANDLE;
    VkImageView m_ScatteringView = VK_NULL_HANDLE;
#if 1
    VkImage m_DensityImage = VK_NULL_HANDLE;       // iteration intermediate (scattering density, 3D)
    VkDeviceMemory m_DensityMemory = VK_NULL_HANDLE;
    VkImageView m_DensityView = VK_NULL_HANDLE;
    VkImage m_IrradianceImage = VK_NULL_HANDLE;    // ground indirect irradiance (2D 64x16, 累计——渲染用)
    VkDeviceMemory m_IrradianceMemory = VK_NULL_HANDLE;
    VkImageView m_IrradianceView = VK_NULL_HANDLE;
    // deltaIrr=上一阶纯间接辐照度（2D，下一阶 density 地面反弹场）——官方 delta_multiple/delta_irradiance 语义
    VkImage m_DeltaMultipleImage = VK_NULL_HANDLE;
    VkDeviceMemory m_DeltaMultipleMemory = VK_NULL_HANDLE;
    VkImageView m_DeltaMultipleView = VK_NULL_HANDLE;
    VkImage m_DeltaIrradianceImage = VK_NULL_HANDLE;
    VkDeviceMemory m_DeltaIrradianceMemory = VK_NULL_HANDLE;
    VkImageView m_DeltaIrradianceView = VK_NULL_HANDLE;
#endif
    VkImage m_SkyRTImage = VK_NULL_HANDLE;
    VkDeviceMemory m_SkyRTMemory = VK_NULL_HANDLE;
    VkImageView m_SkyRTView = VK_NULL_HANDLE;   // CUBE（合成采样——硬件 cubemap）
    VkImageView m_SkyRTArrayView = VK_NULL_HANDLE;   // 2D_ARRAY（compute 写 skyRT——6 层）
    VkImageLayout m_SkyRTLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkSampler m_LUTSampler = VK_NULL_HANDLE;   // 线性 + clamp（transmittance/scattering 查询）
    VkSampler m_SkyRTSampler = VK_NULL_HANDLE; // 线性 + clamp（合成端采样全景图）

    VkImage m_CloudRTImage = VK_NULL_HANDLE;   // 外部 cloudRT，仅用于 pano→cube 写后读 barrier

    VkImage m_SkyCubeImage = VK_NULL_HANDLE;
    VkDeviceMemory m_SkyCubeMemory = VK_NULL_HANDLE;
    VkImageView m_SkyCubeView = VK_NULL_HANDLE;       // CUBE（合成端 IBL 采样）
    VkImageView m_SkyCubeArrayView = VK_NULL_HANDLE;  // 2D_ARRAY（compute 写 6 层）
    VkImageLayout m_SkyCubeLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkSampler m_SkyCubeSampler = VK_NULL_HANDLE;
    uint32_t m_SkyCubeW = 128;
    uint32_t m_SkyCubeMips = 8;

    glm::vec3 m_LastSunDir = glm::vec3(0.0f, 0.0f, 0.0f);
    float m_LastAltitude = -1.0f;
    glm::vec4 m_LastLightColor = glm::vec4(0.0f);   // 天空帧缓存含平行光：滑条变化必须触发重生成
    glm::vec3 m_CubeLastSunDir = glm::vec3(0.0f, 0.0f, 0.0f);
    float m_CubeLastAltitude = -1.0f;
    uint32_t m_CubeFramesSinceUpdate = 0;

    ComputePipeline m_TransmittancePipe;
    ComputePipeline m_ScatteringPipe;
#if 1
    ComputePipeline m_DensityPipe;
    ComputePipeline m_MultiscatterPipe;
    ComputePipeline m_IrradiancePipe;
#endif
    ComputePipeline m_SkyPipe;
    ComputePipeline m_SkyCubePipe;
    ComputePipeline m_PanoToCubePipe;
    ComputePipeline m_ShProjPipe;
    ComputePipeline m_CubePrefilterPipe;
    VkDescriptorSet m_PrefilterSets[8] = {};
    ComputePipeline m_BRDFLutPipe;
    VkImageView m_SkyCubeMipViews[8] = {};   // 每 mip 的 2DArray view（预滤波 dst 写）
    VkBuffer m_SkyCubeSHBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_SkyCubeSHMemory = VK_NULL_HANDLE;
    VkImage m_BRDFLutImage = VK_NULL_HANDLE;
    VkDeviceMemory m_BRDFLutMemory = VK_NULL_HANDLE;
    VkImageView m_BRDFLutView = VK_NULL_HANDLE;
    VkSampler m_BRDFLutSampler = VK_NULL_HANDLE;
    bool m_BRDFLutReady = false;   // 一次性 dispatch 已提交
    void DispatchBRDFLut(VkCommandBuffer cmd);
    void DispatchCubePrefilter(VkCommandBuffer cmd);

    bool m_Initialized = false;
};
