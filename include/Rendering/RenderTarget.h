#pragma once
#include "Platform/Export.h"

#include <vulkan/vulkan.h>
#include <stdint.h>
#include <vector>

// 离屏渲染目标类
// 用于将场景渲染到纹理，然后在ImGui中显示
class MIKAN_API RenderTarget {
public:
    RenderTarget();
    ~RenderTarget();

    // 初始化渲染目标
    void Init(uint32_t width, uint32_t height, bool useMRT = false, bool outputPosition = true);
    
    // 清理资源
    void Cleanup();
    
    // 调整大小
    void Resize(uint32_t width, uint32_t height);
    
    // 开始渲染到该目标
    void BeginRender(VkCommandBuffer commandBuffer);
    
    // 兼容现有调用顺序：桌面 MRT 的第二次调用结束几何 pass 并开始合成 pass。
    void NextSubpass(VkCommandBuffer commandBuffer);
    
    // 结束渲染
    void EndRender(VkCommandBuffer commandBuffer);
    
    // 获取颜色纹理 (用于ImGui显示)
    VkImageView GetColorImageView() const { return m_ColorImageViews[0]; }
    VkImage GetColorImage() const { return m_ColorImages[0]; }
    
    // 获取MRT颜色纹理
    VkImageView GetColorImageView(uint32_t index) const { return index < m_ColorImageViews.size() ? m_ColorImageViews[index] : VK_NULL_HANDLE; }
    VkImage GetColorImage(uint32_t index) const { return index < m_ColorImages.size() ? m_ColorImages[index] : VK_NULL_HANDLE; }
    
    // 获取深度纹理
    VkImageView GetDepthImageView() const { return m_DepthImageView; }
    VkImage GetDepthImage() const { return m_DepthImage; }
    
    // 获取描述符集 (用于ImGui渲染，绑定 G-Buffer 颜色0 —— MRT 调试用)
    VkDescriptorSet GetDescriptorSet() const { return m_DescriptorSet; }
    
    // 显示附件（合成 subpass 输出）：所有视口统一的合成后画面，编辑器面板采样它
    VkImageView GetDisplayImageView() const { return m_DisplayImageView; }
    VkImage GetDisplayImage() const { return m_DisplayImage; }
    VkDescriptorSet GetDisplayDescriptorSet() const { return m_DisplayDescriptorSet; }
    VkFormat GetDisplayFormat() const { return m_DisplayFormat; }
    
    // 获取采样器
    VkSampler GetSampler();
    
    // 获取用于 Hi-Z 纹理采样的采样器（支持多层 mip）
    VkSampler GetHiZSampler();
    
    // 获取渲染通道
    VkRenderPass GetRenderPass() const { return m_RenderPass; }
    
    // 中间附件（合成 subpass 输出，final 后处理 pass 采样）
    VkImageView GetCompositeImageView() const { return m_CompositeImageView; }
    VkImage GetCompositeImage() const { return m_CompositeImage; }
    
    // 独立合成通道（单 subpass，普通纹理采样 G-Buffer）
    VkRenderPass GetCompositeRenderPass() const { return m_CompositeRenderPass; }
    // 所有 MRT 平台都使用独立合成 pass；桌面仍保留 geometry render pass
    // 的 composite 附件槽，保证主几何管线接口不变。
    bool UsesSeparateComposite() const { return m_UseSeparateComposite; }
    void BeginCompositeRender(VkCommandBuffer commandBuffer);
    void EndCompositeRender(VkCommandBuffer commandBuffer);

    // 透明前向粒子 pass：加载已合成的 HDR composite，并只读复用几何深度。
    // 桌面端用于避开复杂 MRT render pass 的额外 subpass；Android 复用上面的独立合成 pass。
    VkRenderPass GetParticleRenderPass() const { return m_ParticleRenderPass; }
    void BeginParticleRender(VkCommandBuffer commandBuffer);
    void EndParticleRender(VkCommandBuffer commandBuffer);
    
    // final render pass（黑白滤镜等后处理：读中间附件 → 显示附件；多 pass 链路验证）
    VkRenderPass GetFinalRenderPass() const { return m_FinalRenderPass; }
    VkFramebuffer GetFinalFramebuffer() const { return m_FinalFramebuffer; }
    // UI 叠加 pass（loadOp=LOAD）：链末 tonemap 之后画 UI，保留链结果并 alpha 混合叠加
    VkRenderPass GetDisplayUIRenderPass() const { return m_DisplayUIRenderPass; }
    void BeginFinalRender(VkCommandBuffer commandBuffer);
    void EndFinalRender(VkCommandBuffer commandBuffer);
    
    // 获取帧缓冲
    VkFramebuffer GetFramebuffer() const { return m_Framebuffer; }
    
    // 获取尺寸
    uint32_t GetWidth() const { return m_Width; }
    uint32_t GetHeight() const { return m_Height; }
    
    // 附件格式（供合并 render pass 创建使用）
    VkFormat GetMainColorFormat() const { return m_MainColorFormat; }
    VkFormat GetNormalFormat() const { return m_NormalFormat; }
    VkFormat GetMotionVectorFormat() const { return m_MotionVectorFormat; }
    VkFormat GetMaterialFormat() const { return m_MaterialFormat; }
    VkFormat GetDepthFormat() const { return m_DepthFormat; }
    bool GetUseMRT() const { return m_UseMRT; }
    
    // 创建ImGui可用的纹理描述符
    void CreateImGuiDescriptorSet();

private:
    void CreateRenderPass();
    void CreateFramebuffer();
    void CreateParticleRenderPass();
    void CreateParticleFramebuffer();
    void CreateCompositeImageResource();
    void CreateCompositeRenderPass();       // 分离合成通道（单 subpass）
    void CreateCompositeFramebuffer();      // 分离合成通道 framebuffer（[composite, depth]）
    void CreateFinalRenderPass();
    void CreateFinalFramebuffer();
    void CreateColorResources();
    void CreateDepthResources();
    void CreateDisplayResource();
    void CreateDescriptorSet();

    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    bool m_UseMRT = false;
    bool m_UseSeparateComposite = false;
    // 兼容桌面调用序列的阶段状态：BeginRender=0，首次 NextSubpass=1（几何），
    // 再次 NextSubpass=2（已切换到独立合成 pass）。
    uint32_t m_CurrentSubpass = 0;
    bool m_OutputPosition = true;
    
    // MRT 格式选择 (Adreno GPU 兼容)
    VkFormat m_MainColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat m_NormalFormat = VK_FORMAT_R16G16_SNORM;   // 法线附件：R16G16_SNORM（4B，[-1,1] 直接存法线 x,y，z 光照重建）
    VkFormat m_MotionVectorFormat = VK_FORMAT_R16G16_SFLOAT;  // 运动矢量格式（TAA 预留）
    VkFormat m_MaterialFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat m_DepthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
    
    VkRenderPass m_RenderPass = VK_NULL_HANDLE;
    VkFramebuffer m_Framebuffer = VK_NULL_HANDLE;
    // 桌面透明前向粒子 pass（[composite, depth]；仅 MRT 目标创建）
    VkRenderPass m_ParticleRenderPass = VK_NULL_HANDLE;
    VkFramebuffer m_ParticleFramebuffer = VK_NULL_HANDLE;
    // MRT 独立合成通道（单 subpass，composite + depth）
    VkRenderPass m_CompositeRenderPass = VK_NULL_HANDLE;
    VkFramebuffer m_CompositeFramebuffer = VK_NULL_HANDLE;
    
    // 中间附件（合成 subpass 输出，供 final 后处理 pass 采样；R8G8B8A8_UNORM，sRGB 编码）
    VkImage m_CompositeImage = VK_NULL_HANDLE;
    VkDeviceMemory m_CompositeImageMemory = VK_NULL_HANDLE;
    VkImageView m_CompositeImageView = VK_NULL_HANDLE;
    // composite（合成 subpass 输出）——B10G11R11_UFLOAT_PACK32（32bpp 浮点，无 alpha）：合成是预处理 pass（后续还有最终光照/tonemap 阶段），
    // r11g11b10 保留 HDR 动态范围（>1.0 高光不 clamp）同时比 16F 省一半带宽；alpha 恒 1 不使用正好匹配
    // 2026-08-11 用户拍板（最终）：合成直出线性 HDR（不 LogLuv32 编码解码——8bit 编码+tonemap 解码精度损失导致颜色问题）
    VkFormat m_CompositeFormat = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    
    // final render pass（后处理：读中间附件 → 显示附件）
    VkRenderPass m_FinalRenderPass = VK_NULL_HANDLE;
    VkFramebuffer m_FinalFramebuffer = VK_NULL_HANDLE;
    VkRenderPass m_DisplayUIRenderPass = VK_NULL_HANDLE;   // UI 叠加 pass（loadOp=LOAD）
    
    // 颜色附件
    std::vector<VkImage> m_ColorImages;
    std::vector<VkDeviceMemory> m_ColorImageMemories;
    std::vector<VkImageView> m_ColorImageViews;
    
    // 深度附件
    VkImage m_DepthImage = VK_NULL_HANDLE;
    VkDeviceMemory m_DepthImageMemory = VK_NULL_HANDLE;
    VkImageView m_DepthImageView = VK_NULL_HANDLE;
    
    // 显示附件（合成 subpass 输出；R8G8B8A8_UNORM，sRGB 编码的合成后画面）
    VkImage m_DisplayImage = VK_NULL_HANDLE;
    VkDeviceMemory m_DisplayImageMemory = VK_NULL_HANDLE;
    VkImageView m_DisplayImageView = VK_NULL_HANDLE;
    VkFormat m_DisplayFormat = VK_FORMAT_R8G8B8A8_UNORM;   // 2026-08-11：LogLuv32 全链解决色带后显示附件回 rgba8（8bit 通道比 r11g11b10 5-6-5 更细 + 有 alpha；Bayer 抖动匹配 8bit）
    
    // ImGui描述符集
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSet m_DisplayDescriptorSet = VK_NULL_HANDLE;  // 显示附件（合成后）的 ImGui 描述符
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
    VkSampler m_Sampler = VK_NULL_HANDLE;
    VkSampler m_HiZSampler = VK_NULL_HANDLE; // 用于 Hi-Z 纹理采样的采样器
    
    bool m_Initialized = false;
};
