#pragma once

#include "Platform/Export.h"

#include <vulkan/vulkan.h>

// ============================================================================
// WaterTargetRT — 水面目标 RT（deferred water compositing 的数据源）
//
// 水面（地形涂刷水 + WaterComponent 实体水）不再写入 G-buffer/主深度，而是
// 全部渲染进这张独立 float RT，供后处理 water_composite 用双深度比较
// （waterZ vs opaqueZ）判覆盖并做吸收/反射合成：
//   R  = mask（1.0 = 有水；clear 0 = 无水，合成时直通场景色）
//   G  = gl_FragCoord.z（与主深度缓冲同一 NDC [0,1] 空间，同一投影下可直接比较）
//   BA = 八面体编码的世界法线 xy
// 附带 D32 深度附件，仅用于水面自身（多水体/多地形重叠）的近者胜出。
//
// 生命周期：EnsureRenderPass 在 SceneRenderer::Init 时创建一次（管线依赖）；
// EnsureSize 随目标视图尺寸惰性重建图像与 framebuffer。所有水面绘制共用
// 同一 render pass 实例（BeginPass/EndPass 之间可依次录多个渲染器）。
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
