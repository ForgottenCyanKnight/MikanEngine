// CMAA2 引擎接入（2026-08-16/17）——Intel CMAA2 compute 实现移植（官方语义：延迟混合链表）
// 流水线：cmaa_edges.comp（全屏：边缘检测 + 候选列表）→ cmaa_process.comp（候选：Simple/Z 形状 → 混合颜色 → 链表）
//   → cmaa_apply.comp（全 quad：遍历链表加权平均 → 写 result 图）
//   → cmaa_apply.frag（后处理链 pass：仅采样 result，见 postprocess_chain.json "cmaa_result" 源）
#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

class CMAA2 {
public:
    struct ComputePipeline {
        VkShaderModule module = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    bool Init(VkDevice device, uint32_t w, uint32_t h);
    void Cleanup();

    // 每帧：清候选/链表头/item 计数 → edges（全屏 16×16）→ process（候选 /128）→ apply（quad×4）
    // colorView/colorImage = tonemap 输出（hook 已 barrier 为 GENERAL）——edges/process 读、apply 原地写（官方语义：无独立 result 图）
    void Dispatch(VkCommandBuffer cmd, VkImageView colorView, VkSampler colorSampler, VkImage colorImage, uint32_t w, uint32_t h);

    // 2026-08-17 回退：独立 result 图（官方 3 compute 语义——apply 写 result，cmaa_apply.frag 采样透传）
    VkImageView GetWeightView() const { return m_ResultView; }
    VkSampler   GetWeightSampler() const { return m_WeightSampler; }

private:
    bool CreateImages(uint32_t w, uint32_t h);
    bool CreateBuffers(uint32_t w, uint32_t h);
    bool CreatePipelines();
    void DestroyPipeline(ComputePipeline& pipe);
    bool CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                     VkImage& image, VkDeviceMemory& memory, VkImageView& view);
    void ImageBarrier(VkCommandBuffer cmd, VkImage image,
                      VkImageLayout oldLayout, VkImageLayout newLayout,
                      VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                      VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);

    VkDevice m_Device = VK_NULL_HANDLE;
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;

    // 资源
    VkImage m_EdgesImage = VK_NULL_HANDLE;
    VkDeviceMemory m_EdgesMemory = VK_NULL_HANDLE;
    VkImageView m_EdgesView = VK_NULL_HANDLE;
    VkImage m_ResultImage = VK_NULL_HANDLE;      // AA 结果图（rgba16f：apply 写，cmaa_apply.frag 采样）
    VkDeviceMemory m_ResultMemory = VK_NULL_HANDLE;
    VkImageView m_ResultView = VK_NULL_HANDLE;
    VkSampler m_WeightSampler = VK_NULL_HANDLE;   // Linear+Clamp（cmaa_apply 采样 result）
    VkSampler m_EdgesSampler = VK_NULL_HANDLE;    // Nearest（process 读 edges）
    VkBuffer m_CandidateBuffer = VK_NULL_HANDLE;  // 候选：uint count + uint data[]（edges 原子累加）
    VkDeviceMemory m_CandidateMemory = VK_NULL_HANDLE;
    VkBuffer m_ArgsBuffer = VK_NULL_HANDLE;       // 独立 DispatchIndirect 参数（12B {x,y,z}——绝不复用 candidate，见 cmaa_args.comp）
    VkDeviceMemory m_ArgsMemory = VK_NULL_HANDLE;
    ComputePipeline m_ArgsPipe;                   // cmaa_args.comp：candidate[0] → argsBuffer（组数 = ceil(count/128)）
    uint32_t m_CandidateCapacity = 0;

    // 管线
    ComputePipeline m_EdgesPipe;
    ComputePipeline m_ProcessPipe;
};
