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

    // 每帧：天空全景图 compute dispatch（LogLuv32 → RGBA8）
    // 内部处理 skyRT 布局转换（SHADER_READ_ONLY → GENERAL → SHADER_READ_ONLY）与写读 barrier
    void DispatchSky(VkCommandBuffer commandBuffer, const glm::vec3& sunDir, float altitudeMeters = 200.0f);   // 2026-08-11：海拔输入（用户约定 max(0, 相机y+200)）
    // 2026-08-12：skyRT 全景 → cube 重投影（复用 skyRT 散射结果——比散射版 cube 快一个量级；含 blit mip + SH 投影）
    void DispatchPanoToCube(VkCommandBuffer commandBuffer, const glm::vec3& sunDir, float altitudeMeters = 200.0f);
    // 2026-08-12：SH 辐照度投影（从 atmo cubemap 读——3 阶 9 系数 RGB 写 SSBO；太阳/海拔不变时跳过）
    void DispatchSHProj(VkCommandBuffer commandBuffer, const glm::vec3& sunDir, float altitudeMeters = 200.0f);
    VkBuffer GetSkyCubeSHBuffer() const { return m_SkyCubeSHBuffer; }   // 合成 binding 10 读（UBO 类型绑 STORAGE|UNIFORM buffer）
    void DumpSHCoefs(const char* tag);   // 2026-08-12 临时调试：回读 SSBO 打印系数

    VkImageView GetSkyRTView() const { return m_SkyRTView; }
    VkImageView GetTransmittanceView() const { return m_TransmittanceView; }   // 2026-08-11：合成 pass 物理太阳透射
    VkSampler GetLUTSampler() const { return m_LUTSampler; }   // transmittance/scattering 共享的线性 clamp 采样器
    VkImageView GetScatteringView() const { return m_ScatteringView; }   // 2026-08-11 per-pixel：散射 LUT（GetSkyRadiance）
    VkSampler GetSkyRTSampler();
    uint32_t GetSkyWidth() const { return m_SkyW; }
    uint32_t GetSkyHeight() const { return m_SkyH; }

    // 2026-08-12：天空环境 cubemap（IBL/反射专用——各向同性，无柱面极区聚集；独立于柱面 skyRT）
    void DispatchSkyCube(VkCommandBuffer commandBuffer, const glm::vec3& sunDir, float altitudeMeters = 200.0f);
    VkImageView GetSkyCubeView() const { return m_SkyCubeView; }
    VkImageView GetSkyCubeArrayView() const { return m_SkyCubeArrayView; }
    VkSampler GetSkyCubeSampler() const { return m_SkyCubeSampler; }
    uint32_t GetSkyCubeSize() const { return m_SkyCubeW; }
    VkImageView GetBRDFLutView() const { return m_BRDFLutView; }   // 2026-08-15：split-sum BRDF LUT（合成 binding 16）
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
                     uint32_t mipLevels = 1);   // 2026-08-11：skyRT 硬件 cubemap=6 层 + mip 链
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
                      uint32_t baseLayer = 0, uint32_t layerCount = 1);   // 2026-08-11：per-mip/per-layer（cubemap mip 链 blit 需要）

    VkDevice m_Device = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    uint32_t m_SkyW = 0;
    uint32_t m_SkyH = 0;
    uint32_t m_SkyMips = 1;   // 2026-08-11：skyRT cubemap mip 级数（128²→8）

    // 纹理
    VkImage m_TransmittanceImage = VK_NULL_HANDLE;
    VkDeviceMemory m_TransmittanceMemory = VK_NULL_HANDLE;
    VkImageView m_TransmittanceView = VK_NULL_HANDLE;
    VkImage m_ScatteringImage = VK_NULL_HANDLE;
    VkDeviceMemory m_ScatteringMemory = VK_NULL_HANDLE;
    VkImageView m_ScatteringView = VK_NULL_HANDLE;
#if 1   // 2026-08-11: 多重散射已恢复（Bruneton 4 阶）
    VkImage m_DensityImage = VK_NULL_HANDLE;       // iteration intermediate (scattering density, 3D)
    VkDeviceMemory m_DensityMemory = VK_NULL_HANDLE;
    VkImageView m_DensityView = VK_NULL_HANDLE;
    VkImage m_IrradianceImage = VK_NULL_HANDLE;    // ground indirect irradiance (2D 64x16, 累计——渲染用)
    VkDeviceMemory m_IrradianceMemory = VK_NULL_HANDLE;
    VkImageView m_IrradianceView = VK_NULL_HANDLE;
    // 2026-08-11 严格 n-1 阶：deltaMulti=上一阶纯多次散射（3D，含相函数，下一阶 density 入射场）、
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

    // 2026-08-12：天空环境 cubemap（IBL——独立于柱面 skyRT；64×64×6 + LogLuv32）
    VkImage m_SkyCubeImage = VK_NULL_HANDLE;
    VkDeviceMemory m_SkyCubeMemory = VK_NULL_HANDLE;
    VkImageView m_SkyCubeView = VK_NULL_HANDLE;       // CUBE（合成端 IBL 采样）
    VkImageView m_SkyCubeArrayView = VK_NULL_HANDLE;  // 2D_ARRAY（compute 写 6 层）
    VkImageLayout m_SkyCubeLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkSampler m_SkyCubeSampler = VK_NULL_HANDLE;
    uint32_t m_SkyCubeW = 128;   // 每面分辨率（2026-08-15：64→128——反射太阳像需要更高分辨率（64² 太阳仅 1-2 texel 不可见）；散射开销低频可接受）
    uint32_t m_SkyCubeMips = 8;   // 2026-08-15：128² → 1，8 级（GGX 预滤波——粗糙度模糊；合成端 lod = roughness × 7）

    glm::vec3 m_LastSunDir = glm::vec3(0.0f, 0.0f, 0.0f);   // 2026-08-11: frame cache（sky 只依赖 sun dir）——⚠️ 初始必须零向量：(0,-1,0) 会在 sunDir 恰为 (0,-1,0) 时首帧跳过 dispatch → skyRT 从未生成 → 黑
    float m_LastAltitude = -1.0f;   // 2026-08-11: cache 键加海拔（海拔变了必须重算 skyRT——否则停留在旧海拔）
    glm::vec3 m_CubeLastSunDir = glm::vec3(0.0f, 0.0f, 0.0f);   // 2026-08-12: skyCube 独立 frame cache
    float m_CubeLastAltitude = -1.0f;
    uint32_t m_CubeFramesSinceUpdate = 0;   // 2026-08-25：IBL/SH 自适应刷新节流（按渲染调用计数）

    ComputePipeline m_TransmittancePipe;
    ComputePipeline m_ScatteringPipe;
#if 1   // 2026-08-11: 多重散射已恢复（Bruneton 4 阶）
    ComputePipeline m_DensityPipe;
    ComputePipeline m_MultiscatterPipe;
    ComputePipeline m_IrradiancePipe;
#endif
    ComputePipeline m_SkyPipe;
    ComputePipeline m_SkyCubePipe;
    ComputePipeline m_PanoToCubePipe;   // 2026-08-12：skyRT 全景 → cube 重投影（省 cube 自身散射计算）
    ComputePipeline m_ShProjPipe;   // 2026-08-12：SH 辐照度投影（读 atmo cube → 写系数 SSBO）
    ComputePipeline m_CubePrefilterPipe;   // 2026-08-12：真 GGX 预滤波（替代 blit box 平均）
    VkDescriptorSet m_PrefilterSets[8] = {};   // 2026-08-15：每 mip 独立 set（录制中更新同一 set → GPU 全读最后一次 view → mip1-5 黑）
    ComputePipeline m_BRDFLutPipe;   // 2026-08-15：split-sum BRDF LUT（一次性生成）
    VkImageView m_SkyCubeMipViews[8] = {};   // 每 mip 的 2DArray view（预滤波 dst 写）
    VkBuffer m_SkyCubeSHBuffer = VK_NULL_HANDLE;   // 2026-08-12：SH 系数（STORAGE 写 + UNIFORM 读，144B）
    VkDeviceMemory m_SkyCubeSHMemory = VK_NULL_HANDLE;   // 2026-08-12：IBL cubemap
    // 2026-08-15：BRDF LUT（128×128 R16G16B16A16_SFLOAT——合成端 split-sum specular）
    VkImage m_BRDFLutImage = VK_NULL_HANDLE;
    VkDeviceMemory m_BRDFLutMemory = VK_NULL_HANDLE;
    VkImageView m_BRDFLutView = VK_NULL_HANDLE;
    VkSampler m_BRDFLutSampler = VK_NULL_HANDLE;
    bool m_BRDFLutReady = false;   // 一次性 dispatch 已提交
    void DispatchBRDFLut(VkCommandBuffer cmd);   // 2026-08-15：BRDF LUT 生成（首帧 cube 路径内）
    void DispatchCubePrefilter(VkCommandBuffer cmd);   // 2026-08-15：真 GGX 预滤波（两处 cube 生成路径共用）

    bool m_Initialized = false;
};
