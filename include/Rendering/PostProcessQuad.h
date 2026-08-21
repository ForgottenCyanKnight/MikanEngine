#pragma once
#include "Platform/Export.h"

#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <glm/glm.hpp>

// 后处理全屏四边形：0-8 个纹理输入槽（binding 0..N-1，COMBINED_IMAGE_SAMPLER），每槽独立 sampler
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
    };
    // push constant：光照 + 帧状态
    struct PushData {
        glm::vec4 cameraPos;       // 与 UBO 同步（fullscreen.frag 仍在用）
        glm::vec4 sunDir;
        glm::vec4 lightColor;
        glm::vec4 frameInfo;
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
