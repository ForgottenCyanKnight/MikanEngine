#pragma once
#include "Platform/Export.h"
#include "Rendering/PostProcessQuad.h"

#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <functional>   // 2026-08-17：pass 后 hook（CMAA2 compute 在 tonemap 后执行）

// 配置驱动的后处理链（参考 FMDS Shadercfg.json 的 pipline_sets / material 组合思想）：
//  - 每个 pass 0-8 个纹理槽（binding 0..7），槽位可引用：
//      "composite"（合成中间附件）/ "pass:<name>"（前方任意 pass 输出）/ "gbuffer0"/"depth"/"skyrt"
//  - 每输入槽独立 sampler 过滤配置（filter + wrap，类似 material samplerStates）
//  - 链末 pass 输出到调用方提供的 final target（显示附件 / swapchain）
//  - 数据流（跨 pass 引用）由 JSON 声明，代码不写死——前后 pass 任意指向
class MIKAN_API PostProcessChain {
public:
    struct PassInput {
        int slot = 0;
        std::string source;             // "composite" | "pass:<name>" | "gbuffer0" | "depth" | "skyrt"
        std::string filter = "Linear";  // "Linear" | "Nearest"
        std::string wrap = "Clamp";     // "Clamp" | "Repeat" | "Mirrored"
    };
    struct PassDef {
        std::string name;
        std::string shader;             // frag shader 名（如 "filter.frag.spv"）
        // 输出格式（仅中间 pass 生效；末 pass 输出到显示附件，格式由 m_FinalRenderPass 决定）。
        // JSON "format": "rgba16f"(默认) | "rgba8" | "r8" | "r16" | "r11g11b10" | "rg16f"
        VkFormat outputFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        float scale = 1.0f;   // 2026-08-12：per-pass 分辨率（JSON "scale"）——金字塔降/升采样依赖
        bool enabled = true;  // 2026-08-15：JSON "enable"（默认 true）——false 跳过该 pass（下游引用自动回退 composite）
        std::string before;   // 2026-08-15：JSON "before": "<pass名>"——该 pass 排序到目标之后执行；inputs 为空时 slot0 自动链接目标输出
        std::vector<PassInput> inputs;
    };

    // 从 JSON 加载链定义（如 assets/postprocess_chain.json）
    bool LoadFromJson(const std::string& path);
    ~PostProcessChain();

    // 构建：为每个 pass（除最后一个）创建中间附件 + render pass + framebuffer + quad
    // finalRenderPass：链末 pass 的 render pass（显示附件 pass 或 swapchain pass，固定；framebuffer 由 Execute 每帧传）
    bool Build(uint32_t w, uint32_t h, VkRenderPass finalRenderPass);

    void Cleanup();
    void Resize(uint32_t w, uint32_t h);

    struct ExternalInputs {
        VkImageView compositeView = VK_NULL_HANDLE;  // 合成中间附件（链入口）
        VkImageView gbufferView = VK_NULL_HANDLE;
        VkImageView gbuffer1View = VK_NULL_HANDLE;   // 2026-08-13 GTAO：法线附件（八面体 R16G16_SNORM）
        VkImageView gbuffer2View = VK_NULL_HANDLE;   // 2026-08-13：材质附件（w = emissive 强度——gtao_apply 重建自发光）
        VkImageView depthView = VK_NULL_HANDLE;
        VkImageView skyView = VK_NULL_HANDLE;
        VkSampler skySampler = VK_NULL_HANDLE;
        VkImageView areaTexView = VK_NULL_HANDLE;    // 2026-08-16 SMAA：areaTex（160x560 RGBA，Linear+Clamp）
        VkSampler areaTexSampler = VK_NULL_HANDLE;
        VkImageView searchTexView = VK_NULL_HANDLE;  // 2026-08-16 SMAA：searchTex（64x16 灰度，Nearest+Clamp）
        VkSampler searchTexSampler = VK_NULL_HANDLE;
        VkImageView cmaaWeightView = VK_NULL_HANDLE;   // 2026-08-16 CMAA2：compute 输出的每像素 4 方向混合权重图（RGBA8）
        VkSampler cmaaWeightSampler = VK_NULL_HANDLE;
        PostProcessQuad::PushData pushData = {};
        PostProcessQuad::CameraUBO cameraUBO = {};  // 完整相机 UBO（所有后处理 shader 共享）
        VkImageView historyView = VK_NULL_HANDLE;  // 时序 GTAO 历史（半分辨率 RGBA8：AO/体积光/Godray）
        VkSampler historySampler = VK_NULL_HANDLE;
        VkImageView taaHistoryView = VK_NULL_HANDLE;     // 2026-08-17 TAA：上帧输出历史（全分辨率 HDR）
        VkSampler taaHistorySampler = VK_NULL_HANDLE;
        VkImageView gbufferMotionView = VK_NULL_HANDLE;  // 2026-08-17 TAA depth-guided：G-Buffer 附件3 运动向量（Nearest）
        VkImageView csmShadowView = VK_NULL_HANDLE;       // 2026-：CSM 阴影 2D array（gtao 半分辨率体积光采样）
        VkSampler csmShadowSampler = VK_NULL_HANDLE;     // 硬件 shadow sampler（compareOp=LESS）
        VkImageView ssgiHistoryView = VK_NULL_HANDLE;     // 2026-：SSGI 历史（半分辨率 RGBA8，跨帧累积降噪）
        VkSampler ssgiHistorySampler = VK_NULL_HANDLE;
    };

    // 每帧执行链：逐 pass（输入 barrier → render pass → quad 绘制）
    // finalFB：链末 pass 的输出 framebuffer（显示附件 或 swapchain per-image framebuffer）
    void Execute(VkCommandBuffer cmd, int w, int h, ExternalInputs& ext, VkFramebuffer finalFB);   // 2026-08-13 非 const：Execute 内填 frameInfo

    // 2026-08-17：pass 后 hook——指定 pass 执行完后调用（该 pass 输出已 barrier 为 GENERAL（compute 读写），
    // 传入其 view + image）。CMAA2 compute 用它"在 tonemap 之后"原地修改 tonemap 输出（官方语义，无 result 图）
    using PassHookFn = std::function<void(VkCommandBuffer, VkImageView, VkImage)>;
    void SetPassHook(const std::string& afterPass, PassHookFn fn) { m_HookAfterPass = afterPass; m_PassHook = std::move(fn); }

    // 2026-08-17：查询 pass 是否启用（hook 消费方据此跳过——cmaa_apply 禁用时 compute 不应白跑）
    bool IsPassEnabled(const std::string& name) const;

    bool IsBuilt() const { return m_Built; }
    const std::vector<PassDef>& GetPasses() const { return m_Passes; }
    int GetPassCount() const { return (int)m_Runtime.size(); }
    // 2026-08-13：暴露指定 pass 的输出 image（时序 GTAO 历史拷贝用）
    VkImage GetPassOutputImage(const std::string& passName) const;

private:
    struct PassRuntime {
        size_t passIndex = 0;            // 指向 m_Passes[passIndex]（不复制 PassDef——本环境 string/vector 拷贝入 PassRuntime 崩溃，已定位）
        VkRenderPass renderPass = VK_NULL_HANDLE;   // 中间 pass 输出（末 pass 用调用方 final render pass）
        VkFramebuffer framebuffer = VK_NULL_HANDLE; // 中间附件 framebuffer（末 pass 用调用方 final framebuffer）
        VkImage image = VK_NULL_HANDLE;             // 中间附件（末 pass 无）
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;   // 2026-08-12：per-pass 输出尺寸（末 pass = 主尺寸）
        PostProcessQuad quad;
    };

    bool ResolveSource(const PassInput& in, const ExternalInputs& ext, PostProcessQuad::InputBinding& out,
                       size_t currentPassIndex = 0);   // 2026-08-15：currentPassIndex 供 "pass:before"（前一个 pass 输出）解析
    VkSampler SamplerFor(const PassInput& in);

    std::vector<PassDef> m_Passes;
    std::vector<PassRuntime> m_Runtime;
    VkRenderPass m_FinalRenderPass = VK_NULL_HANDLE;
    uint32_t m_Width = 0, m_Height = 0;
    bool m_Built = false;
    // 2026-08-17：pass 后 hook（SetPassHook 注册；Execute 在指定 pass 结束后触发）
    std::string m_HookAfterPass;
    PassHookFn m_PassHook;
};
