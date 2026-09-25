#pragma once
#include "Platform/Export.h"
#include "Rendering/RenderTarget.h"
#include "Rendering/FullscreenQuad.h"
#include "Rendering/RendererBase.h"   // VulkanPipeline（解析 pass + 着色器热更新登记）

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>

// ============================================================================
// SceneReflectionProbe — 场景反射探针（cubemap）
//
// 架构：**探针是一个真正的独立相机视图**，与编辑器的「场景视图 / 游戏视图」
// 同级，走同一条视图管线：
//
//   视图       相机            离屏目标                  光照剔除槽位
//   场景视图   编辑器相机       g_SceneRenderTarget       viewSlot 0
//   游戏视图   主相机           g_GameRenderTarget        viewSlot 1
//   反射探针   6 个 90° 面朝向   m_Target（本类持有）      viewSlot 2
//
// 每个面都是一次完整的「切相机 → 跑视图管线」：cluster 光照剔除（探针相机）→
// MRT 几何（地形/草/模型/体素，真身渲染器 + 真身 G-buffer）→ 独立合成 pass
// （fullscreen.frag：方向光 + CSM + IBL/SH + 点光 + 大气/云/雾，全光照）。
// 因此倒影带纹理颜色与光照，不存在早期等距柱状全景「只输出 albedo、只有轮廓」
// 的问题，也不需要任何探针专用的顶点/片元着色器。
//
// 解析：探针视图的合成附件是 B10G11R11（无 alpha），cubemap 面是 RGBA16F 且需要
// alpha 作为「该方向有内容」的掩码，格式不兼容无法拷贝/blit，于是走一次
// probe_resolve.frag（采样合成附件 → 写面 + alpha=1）。顶点复用 fullscreen.vert。
//
// 时序（"上一帧的 cubemap 反射探针"）：只有 **一张** cubemap，不做 ring。捕获录在
// 帧尾，水面合成在本帧命令缓冲更早的位置 ⇒ 采样端读到的永远是上一帧的解析结果。
// 与 AO / SSGI / Cloud / TAA history 的「单张、读上一帧」约定一致。
//
// 降频（时间切片）：6 个面每帧全重渲太贵（实测净成本 11~13 ms/帧），改为每帧只捕
// 1 个面、按面号轮转 —— 一轮 kFaceCount 帧覆盖全部方向。采样端语义随之变成「最近
// 一次该面被解析的结果」，最坏 kFaceCount 帧前。cubemap 是持久的、每层独立，所以
// 跳过某面时那一层保留上一轮内容，天然正确。每帧面数见 AcquireFrameFaces。
//
// 刻意不参与探针的东西（见 RenderSceneProbeCapture 的注释）：
//   * CSM 阴影：桌面 CascadeShadowRenderer 只有 2 个 slot 且级联 UBO 是常驻映射的
//     host 写入，帧尾往 slot 0 重渲会改坏主视图本帧更早录制的合成。探针直接复用
//     主视图本帧算好的 CSM（slot 0，只读绑定）。
//   * Hi-Z / 叶片级草剔除：探针不生成也不消费 Hi-Z（viewSlot 2 在
//     GetGrassHiZShader/IsGrassHiZCullingEnabled 里天然返回空 → 走回退路径）。
//   * 水面与后处理链：水面目标 RT 是单例、water_composite 属于 PostProcessChain，
//     跑链会把 TAA 抖动/历史污染进探针。
// ============================================================================
class MIKAN_API SceneReflectionProbe {
public:
    static constexpr uint32_t kFaceCount = 6;
    // 探针面尺寸（像素）。2026-09-23 由 128 提升到 256（反射清晰度）。
    // 角分辨率账：一个面覆盖 90°，128² ⇒ 1.42 px/°；屏幕（1920 宽 / ~90° FOV）
    // 约 21 px/°。差 15× —— 256² 把它缩到 7.5×，即纯分辨率能买到的全部收益，
    // 代价是捕获填充 ×4。运行时可用 env MIKAN_SCENE_PROBE_FACE_SIZE 覆盖。
    static constexpr uint32_t kDefaultFaceSize = 256;
    // 探针近平面：地形/模型都在米级尺度上，0.1 足够且不至于把精度全吃掉。
    static constexpr float kNearPlane = 0.1f;
    // 探针远平面（米）。反射只保留捕获点周围 100 米内的几何，避免六面
    // 视锥在远处遍历大量无关场景；该值同时用于 GPU 深度裁剪和 CPU 视锥剔除。
    static constexpr float kFarPlane = 100.0f;

    SceneReflectionProbe() = default;
    ~SceneReflectionProbe();

    SceneReflectionProbe(const SceneReflectionProbe&) = delete;
    SceneReflectionProbe& operator=(const SceneReflectionProbe&) = delete;
    SceneReflectionProbe(SceneReflectionProbe&&) = delete;
    SceneReflectionProbe& operator=(SceneReflectionProbe&&) = delete;

    bool Init(uint32_t faceSize);
    void Cleanup();
    bool IsInitialized() const { return m_Initialized; }
    uint32_t GetFaceSize() const { return m_FaceSize; }

    // 探针视图的离屏目标。规格与 g_SceneRenderTarget / g_GameRenderTarget 完全一致
    // （MRT + 独立合成 pass），因此共享的几何管线按其 render pass 兼容即可直接使用。
    RenderTarget& GetTarget() { return m_Target; }
    // 探针视图的合成 quad：管线按 m_Target 自己的 composite render pass 创建
    // （每个 target 的 render pass 是不同句柄，quad 不能跨 target 共用）。
    FullscreenQuad& GetCompositeQuad() { return m_CompositeQuad; }

    // 采样端：CUBE 视图 + 线性 clamp 采样器（与 skyCube 同规格）。
    VkImageView GetCubeView() const { return m_CubeView; }
    VkSampler GetSampler() const { return m_Sampler; }
    VkImage GetImage() const { return m_ColorImage; }
    VkFormat GetColorFormat() const { return kColorFormat; }

    // 6 个面的 90° 透视 view / proj（-Z 朝前，1:1 纵横比）。
    // 面序与 Vulkan cubemap 约定一致：+X -X +Y -Y +Z -Z，up 向量按各面选，
    // 保证相邻面共享边上的方向完全一致（与 PointShadowRenderer 用同一组）。
    static void ComputeFaceViewProjs(const glm::vec3& capturePosition, float farPlane,
                                     std::array<glm::mat4, kFaceCount>& outViews,
                                     std::array<glm::mat4, kFaceCount>& outProjs);

    // 解析第 face 面：把 source（探针视图的 RenderTarget）的合成附件搬进该面。
    // 内部完成「合成附件 COLOR_ATTACHMENT → SHADER_READ_ONLY」的屏障，再起止探针
    // 自己的 render pass 画一次全屏三角。必须在任何 render pass 之外调用，且每面
    // 调用一次（合成附件每面都被重写，所以屏障也必须每面一次）。
    void ResolveFace(VkCommandBuffer commandBuffer, uint32_t face, RenderTarget& source);

    // 第 face 面解析完立即调用：该层 颜色附件 → SHADER_READ_ONLY，供水面合成采样。
    // **必须按层**（layerCount=1 + baseArrayLayer=face）：降频轮转下每帧只解析
    // 1 个面，若按整图 6 层发 barrier，未写过的 5 层会被声明成「刚从
    // COLOR_ATTACHMENT 出来」——它们的真实布局是 SHADER_READ_ONLY，oldLayout
    // 不匹配（校验层报错，部分驱动会真的破坏这两层的内容）。
    void RecordShaderReadBarrier(VkCommandBuffer commandBuffer, uint32_t face);

    // ---- 时间切片（降频更新）----
    // 探针捕获点变化时必须同一帧捕获全部面；否则 cubemap 会混合不同相机位置的
    // 六组结果，表现为几何缺面/接缝。捕获点不变时才允许按面号轮转。
    //
    // 每帧面数由 env MIKAN_SCENE_PROBE_FACES_PER_FRAME 决定（1..6，默认 1）；
    // 设 6 即回到「每帧全量」，用于 A/B 对照。
    //
    // 首次调用**强制**返回全部 6 面：PrimeColorLayout 把 6 层清成 alpha=0，若首帧
    // 只渲 1 面，其余 5 个方向会在整整一轮内一直回退天空——那不是「稍微旧一点」，
    // 而是「根本没有探针内容」，水面会出现沿面边界的扇形分界。
    uint32_t AcquireFrameFaces(const glm::vec3& capturePosition, uint32_t* outFaces);

private:
    static constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    bool CreateImages();
    bool CreateRenderPass();
    bool CreateSampler();
    bool CreateFaceViewsAndFramebuffers();
    bool CreateResolvePipeline();
    // 首帧初始布局：Init 时把 6 层从 UNDEFINED 清成 (0,0,0,0) 再推到
    // SHADER_READ_ONLY。水面 pass 在帧内比探针捕获更早，若首帧直接采样一张
    // UNDEFINED 布局的图，既违反 Vulkan 规定，alpha 也会是垃圾值（把垃圾当
    // 覆盖掩码 ⇒ 首帧水面反射错乱）。清成 alpha=0 等价于「本帧全部回退天空」。
    bool PrimeColorLayout();

    uint32_t m_FaceSize = kDefaultFaceSize;

    VkImage m_ColorImage = VK_NULL_HANDLE;
    VkDeviceMemory m_ColorMemory = VK_NULL_HANDLE;
    VkImageView m_CubeView = VK_NULL_HANDLE;
    std::array<VkImageView, kFaceCount> m_FaceColorViews{};

    VkRenderPass m_RenderPass = VK_NULL_HANDLE;
    std::array<VkFramebuffer, kFaceCount> m_FaceFramebuffers{};
    VkSampler m_Sampler = VK_NULL_HANDLE;

    // 解析 pass（极简：2 个 combined image sampler，无 push constant）：
    //   binding 0 = 探针视图的合成附件（颜色）
    //   binding 1 = 探针视图的**深度附件** —— 用来判定"该像素是不是天空"，从而写出
    //               正确的覆盖掩码 alpha。天空方向必须写 alpha=0：探针的合成 shader
    //               （fullscreen.frag）绑定列表里没有云纹理，天空分支只采纯大气 skyRT；
    //               若无条件写 1.0，水面端 mix(skyRefl, probe.rgb, probe.a) 会让
    //               "无云的探针天空"完全盖掉 skyCube 的云（skyCube 才是生成阶段
    //               合入了 cloudRT 的那份），表现为水面倒影丢云。
    // 管线走 VulkanPipeline 以便自动登记到着色器热更新注册表。
    VkDescriptorSetLayout m_ResolveSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_ResolvePool = VK_NULL_HANDLE;
    VkDescriptorSet m_ResolveSet = VK_NULL_HANDLE;
    VulkanPipeline m_ResolvePipeline;
    // 解析描述符的缓存：合成/深度附件的句柄在探针生命周期内不变，只在变化时重写
    VkImageView m_CachedCompositeView = VK_NULL_HANDLE;
    VkSampler m_CachedCompositeSampler = VK_NULL_HANDLE;
    VkImageView m_CachedResolveDepthView = VK_NULL_HANDLE;

    // 探针视图（独立相机）自己的离屏目标与合成 quad
    RenderTarget m_Target;
    FullscreenQuad m_CompositeQuad;

    // 颜色图的「per-layer 布局已就绪」位掩码（bit i = 第 i 层已在
    // SHADER_READ_ONLY）。PrimeColorLayout 成功后置满；它失败时保持 0，此时
    // ResolveFace 用 UNDEFINED 当前驱布局兜底。
    // 必须是位掩码而不是单个 bool：降频轮转下每帧只解析 1 层，各层的布局状态
    // 天然是「按层独立」的。
    uint32_t m_PrimedLayers = 0;
    bool m_Initialized = false;

    // 降频轮转状态
    uint32_t m_NextFace = 0;            // 下一帧从哪个面开始
    bool m_FaceWarmupPending = true;    // 首帧还欠一次全量捕获
    bool m_HasCapturePosition = false;
    glm::vec3 m_LastCapturePosition = glm::vec3(0.0f);

    static uint32_t ResolveFacesPerFrame();
};
