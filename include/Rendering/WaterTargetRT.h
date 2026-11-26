#pragma once

#include "Platform/Export.h"

#include <vulkan/vulkan.h>

// ============================================================================
// WaterTargetRT — 水面目标 RT（deferred water compositing 的数据源）
//
// 水面（地形涂刷水、WaterComponent 实体水、体素水）不再写入 G-buffer/主深度，而是
// 全部渲染进这张独立 float RT，供后处理 water_composite 用双深度比较
// （waterZ vs opaqueZ）判覆盖并做吸收/反射合成：
//   R  = mask（1.0 = 有水；clear 0 = 无水，合成时直通场景色）
//   G  = 相机到水面的线性视距（主深度在后处理重建到同一空间）
//   BA = 八面体编码的世界法线 xy
// 附带 D32 深度附件，仅用于水面自身（多水体/多地形重叠）的近者胜出。
//
// EnsureSize 按视图尺寸惰性创建图像和 framebuffer。不同视图共享兼容的管线。
// ============================================================================
class MIKAN_API WaterTargetRT {
public:
    WaterTargetRT() = default;
    ~WaterTargetRT();
    WaterTargetRT(const WaterTargetRT&) = delete;
    WaterTargetRT& operator=(const WaterTargetRT&) = delete;

    bool EnsureRenderPass();
    bool EnsureSize(uint32_t width, uint32_t height);
    void Cleanup();

    // 帧内录制：BeginPass 内部处理新建 RT 的首帧布局 warmup 屏障。
    void BeginPass(VkCommandBuffer commandBuffer);
    void EndPass(VkCommandBuffer commandBuffer) const;

    VkRenderPass GetRenderPass() const { return m_RenderPass; }
    VkImageView GetView() const { return m_View; }
    VkSampler GetSampler() const { return m_Sampler; }
    VkImage GetImage() const { return m_Image; }   // TEMP-PROBE: GPU 回读诊断用
    uint32_t GetWidth() const { return m_Width; }
    uint32_t GetHeight() const { return m_Height; }
    bool IsValid() const { return m_Framebuffer != VK_NULL_HANDLE; }

private:
    bool CreateImages(uint32_t width, uint32_t height);
    void DestroyImages();

    VkImage m_Image = VK_NULL_HANDLE;
    VkDeviceMemory m_Memory = VK_NULL_HANDLE;
    VkImageView m_View = VK_NULL_HANDLE;
    VkImage m_DepthImage = VK_NULL_HANDLE;
    VkDeviceMemory m_DepthMemory = VK_NULL_HANDLE;
    VkImageView m_DepthView = VK_NULL_HANDLE;
    VkRenderPass m_RenderPass = VK_NULL_HANDLE;
    VkFramebuffer m_Framebuffer = VK_NULL_HANDLE;
    VkSampler m_Sampler = VK_NULL_HANDLE;
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    bool m_NeedsLayoutWarmup = false;   // 新建后首帧：UNDEFINED→SHADER_READ_ONLY 丢弃屏障
};
