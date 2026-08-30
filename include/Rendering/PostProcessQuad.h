#pragma once
#include "Platform/Export.h"

#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <glm/glm.hpp>

// 后处理全屏四边形：0-16 个纹理输入槽（binding 0..N-1，COMBINED_IMAGE_SAMPLER），每槽独立 sampler
// 用于 PostProcessChain 的每个 pass（顶点 fullscreen.vert，fragment 为各 pass 自定义 shader）
class MIKAN_API PostProcessQuad {
public:
    struct InputBinding {
        uint32_t slot = 0;   // 2026-08-12：JSON 声明的输入槽位（SetInputs 按 slot 写 binding；跳过失败输入不再错位）
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;   // 采样时 image 的实际布局（composite 输入为 COLOR_ATTACHMENT_OPTIMAL）
    };
    // Camera UBO：纯相机矩阵（binding = m_MaxInputs）
    struct CameraUBO {
        glm::vec4 cameraPos;       // vec4（w = padding）
        glm::mat4 proj;            // 正向投影矩阵
        glm::mat4 view;            // 正向视图矩阵
        glm::mat4 prevViewProj;    // 上一帧 view * proj（SSR/GTAO 重投影）
        glm::mat4 invProj;         // 逆投影矩阵
        glm::mat4 invView;         // 逆视图矩阵
        // 2026-：CSM 级联数据（gtao 半分辨率体积光在链内采样阴影）——std140 对齐
        glm::mat4 csmMatrices[4];  // 世界 → 光 NDC（每级联）
        glm::vec4 csmSplitFars;    // 每级联 far（view depth；无效 = -1）
        glm::vec4 csmParams;       // x = 有效级联数, y = 启用(>0)
        // 体积云参数：追加在 UBO 尾部，保持前面所有相机/CSM 字段偏移不变。
        // cloudParams0 = enabled, coverage, density, baseAltitudeKm
        glm::vec4 cloudParams0;
        // cloudParams1 = thicknessKm, noiseScale, detailErosion, single-scatter albedo
        glm::vec4 cloudParams1;
        // xyz = 世界空间噪声偏移（km）；w = cloud_view 历史是否有效（1 = 可重投影）
        glm::vec4 cloudNoiseOffsetKm;
        // x = 多阶散射强度, y = 建立系数, z = 光路边界混合, w = 阶次衰减
        glm::vec4 cloudLightingParams;
        // 体积云独立的上一帧 VP；SceneView/GameView 各自维护，避免双视口历史串扰。
        glm::mat4 cloudPrevViewProj;
        // x = 高频 detail 纹理相对基础形状的采样倍率；
        // yz = 低层云归一化 XZ 风向（不改变 UBO 尺寸）。
        glm::vec4 cloudShapeParams;
        // xyz = 当前基于时间的云层平移（地心公里坐标）；w 保留。
        glm::vec4 cloudWindOffsetKm;
        // xyz = 上一帧云层平移，用于历史重投影跟随云移动。
        glm::vec4 cloudPrevWindOffsetKm;
        // 高层 2D 云：enabled, coverage, extinction density (1/km), base altitude (km)。
        glm::vec4 cloudHighParams0;
        // 高层 2D 云：thickness (km), horizontal feature scale (cycles/km), detail, brightness。
        glm::vec4 cloudHighParams1;
        // 高层 2D 云当前/上一帧平移，用于纹理滚动和时序重投影。
        glm::vec4 cloudHighWindOffsetKm;
        glm::vec4 cloudHighPrevWindOffsetKm;
        // 高层卷云的水平拉伸方向（xy = 归一化 XZ 风向）。
        glm::vec4 cloudHighWindDirectionXZ;
    };
    // push constant：光照 + 帧状态
    struct PushData {
        glm::vec4 cameraPos;       // 与 UBO 同步（fullscreen.frag 仍在用）
        glm::vec4 sunDir;
        glm::vec4 lightColor;
        glm::vec4 frameInfo;
        // 云专用月光辐照度：不能复用 lightColor，因为原型昼夜系统会在夜间
        // 降低太阳方向光强度，而月光需要保持独立的夜间照明。
        glm::vec4 moonLightColor = glm::vec4(0.18f, 0.25f, 0.45f, 1.0f);
    };

    void Init(VkRenderPass renderPass, uint32_t subpass, const char* fragShaderName, uint32_t maxInputs = 8);
    ~PostProcessQuad();
    void Cleanup();

    // 更新输入槽（binding 0..inputs.size()-1；内容变化才重建描述符）
    void SetInputs(const std::vector<InputBinding>& inputs);

    void Render(VkCommandBuffer commandBuffer, int width, int height, const PushData* push = nullptr);
    void UpdateCameraUBO(const CameraUBO& cam);

    // 获取/创建采样器（filter/wrap 组合缓存复用）
    VkSampler GetOrCreateSampler(VkFilter magFilter, VkSamplerAddressMode wrap);
    VkPipeline GetPipeline() const { return m_Pipeline; }
    VkDescriptorSet GetDescriptorSet() const { return m_DescriptorSet; }

private:
    void CreateDescriptorSetLayout();
    void CreateDescriptorPool();
    void CreatePipeline(VkRenderPass renderPass, uint32_t subpass);
    void CreateDescriptorSet();

    VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_Pipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;

    uint32_t m_MaxInputs = 8;
    std::string m_FragShaderName = "filter.frag.spv";
    std::vector<InputBinding> m_CachedInputs;   // 与上次比较，避免重复更新

    struct SamplerKey {
        VkFilter f;
        VkSamplerAddressMode w;
        bool operator==(const SamplerKey& o) const { return f == o.f && w == o.w; }
    };
    std::vector<std::pair<SamplerKey, VkSampler>> m_SamplerCache;

    bool m_Initialized = false;

    // Camera UBO（binding = m_MaxInputs，所有纹理 slot 之后）
    VkBuffer m_CameraUBO = VK_NULL_HANDLE;
    VkDeviceMemory m_CameraUBOMem = VK_NULL_HANDLE;
    void* m_CameraUBOMapped = nullptr;
};
