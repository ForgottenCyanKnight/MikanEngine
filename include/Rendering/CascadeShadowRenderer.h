#pragma once
#include "Platform/Export.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <array>

// 方向光 CSM 阴影（2026-08-14，参考 LimitlessSquareEngine 的稳定级联实现）：
//  - 2 槽 × 4 级联 × 1024² D16 2D array（槽 0=SceneView/游戏模式，槽 1=GameView）
//  - 级联分裂：等比（base 2 × scale 3^（i+1）），每级联独立正交投影（包围球 ×1.1 + 2 guard texels）
//  - 深度：默认 NDC 深度（正交线性），采样端 clipPos.z 映射 [0,1] 直接比较（+bias）
//  - 采样：合成 pass binding 14 sampler2DArray + binding 15 级联 UBO（std140）
//  - ⭐ 世界锚点防抖（防 CSM 阴影 swimming/抖动）：
//      ① 锚点 = 相机位置对齐到网格（grid = 阴影范围）的 double 世界点，滞回更新（偏移 > grid/2 才跳变）
//      ② 视锥中心相对锚点做 texel 粒度 round（SnapDirectionalShadowCenterToTexelStable）——
//         量化参考系固定在世界锚点而非每帧移动的相机 → 静态场景阴影矩阵逐帧稳定不抖动
//  - 渲染：SceneRenderer 收集几何 → 逐级联 BeginCascade/EndCascade 提交 depth-only（双面 + depth bias）
//  - 级联数据每帧 UpdateCascades 重算并上传 UBO（级联矩阵 + split far + 参数）
class MIKAN_API CascadeShadowRenderer {
public:
    static constexpr int MAX_CASCADES = 4;
    static constexpr int MAX_SLOTS = 2;
    // ⚠️ 2026-08-15：桌面 2048²（业界标配——Unity 中档/UE 默认/Godot 默认；级联 0 texel ~2mm）；移动端保持 1024²（填充率 ×4 太重）
#ifdef __ANDROID__
    static constexpr int CASCADE_SIZE = 1024;
#else
    static constexpr int CASCADE_SIZE = 2048;
#endif
    static constexpr float NEAR_PLANE = 0.1f;
    // 级联分裂参数（照搬 LimitlessSquare 默认：base 2f / scale 3f → 2 / 8 / 26 / 80）
    static constexpr float SPLIT_BASE = 2.0f;
    static constexpr float SPLIT_SCALE = 3.0f;

    ~CascadeShadowRenderer() { Cleanup(); }

    // 创建 2 槽 × 4 层 2D array + depth-only render pass + framebuffers + UBO（幂等）
    bool Init();
    void Cleanup();

    // 每帧：给定相机 view/proj + 方向光方向，计算 4 级联正交投影矩阵（含锚点防抖），上传 UBO
    // lightDir：从场景指向光源的方向（与合成 shader pc.sunDir 同语义）
    void UpdateCascades(int slot, const glm::mat4& view, const glm::mat4& proj, const glm::vec3& lightDir);

    // 逐级联渲染：begin depth-only render pass（层 framebuffer）+ viewport/scissor（无效级联自动跳过）
    void BeginCascade(VkCommandBuffer cmd, int slot, int cascade, int width, int height);
    void EndCascade(VkCommandBuffer cmd);

    // 全部级联渲染完后：2D array → SHADER_READ_ONLY（合成 pass 采样）
    void Finalize(VkCommandBuffer cmd, int slot);

    // 渲染前：SHADER_READ_ONLY（上帧 Finalize 残留）→ DEPTH_STENCIL_ATTACHMENT_OPTIMAL + 同步先前采样读
    void PrepareRender(VkCommandBuffer cmd, int slot);

    VkRenderPass GetRenderPass() const { return m_RenderPass; }
    VkImageView GetArrayView(int slot) const { return m_Slots[slot].arrayView; }
    // ⚠️ 2026-08-14 per-frame 双缓冲 UBO（2 帧 in-flight：合成读与下一帧 UpdateCascades 写竞争 → 阴影移动偏离；
    // 点光源 UBO 矩阵固定无此问题——"为什么点光源没事"的根因）
    VkBuffer GetCascadeBuffer(int slot, int frame = 0) const { return m_Slots[slot].uboBuffers[frame % 3]; }   // 2026-08-17：3 帧槽（三重缓冲）
    void SetFrameIndex(int frame) { m_FrameIndex = frame; }
    bool IsInitialized() const { return m_Initialized; }

    // 级联元数据（剔除用）：世界空间阴影矩阵 / split far（view depth）
    const glm::mat4& GetShadowMatrix(int slot, int cascade) const { return m_Slots[slot].shadowMatrices[cascade]; }
    float GetSplitFar(int slot, int cascade) const { return m_Slots[slot].splitFars[cascade]; }
    bool IsCascadeValid(int slot, int cascade) const { return m_Slots[slot].valid[cascade]; }

private:
    struct Slot {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView arrayView = VK_NULL_HANDLE;              // 2D_ARRAY（合成采样）
        VkImageView layerViews[MAX_CASCADES] = {};           // 单层 2D view（framebuffer 附件）
        VkFramebuffer framebuffers[MAX_CASCADES] = {};
        VkBuffer uboBuffers[3] = {};   // 2026-08-17：3 帧槽（swapchain 三重缓冲——双缓冲 3 帧 in-flight 竞态 → 阴影黑闪）
        VkDeviceMemory uboMemories[3] = {};   // 2026-08-17：3 帧槽
        void* uboMapped[3] = { nullptr, nullptr, nullptr };   // 2026-08-17：3 帧槽
        // 级联元数据（CPU 侧）
        glm::mat4 shadowMatrices[MAX_CASCADES] = {};
        float splitFars[MAX_CASCADES] = { -1.0f, -1.0f, -1.0f, -1.0f };
        bool valid[MAX_CASCADES] = { false, false, false, false };
        // ⭐ 世界锚点（double，滞回更新）——2026-08-15：单例（所有级联共享，原版 Graphics.cs:175 语义）——
        // 共享 anchor → 所有级联 snap 相位一致 → 层级切换时阴影相位对齐（per-cascade 独立会各自滞回 → 切换跳变）
        glm::dvec3 stableAnchorWorld = glm::dvec3(0.0);
        bool anchorInitialized = false;
    };
    Slot m_Slots[MAX_SLOTS];

    // ⚠️ 2026-08-14：当前帧索引（SetFrameIndex 由 VulkanManager 每帧设置；UBO 双缓冲按 frame&1 写入/绑定）
    int m_FrameIndex = 0;

    VkRenderPass m_RenderPass = VK_NULL_HANDLE;
    bool m_Initialized = false;

    // 锚点（LimitlessSquare GetDirectionalShadowStableAnchorRelative 移植）：
    // 锚点初始 = 相机对齐 grid 网格；之后仅当相机偏离超过 grid/2 才跳到新网格点（滞回防边界抖动）
    // 2026-08-15：单例（所有级联共享——原版 `_directionalShadowStableAnchorWorld` 单值）；cascade 参数保留但忽略
    glm::vec3 GetStableAnchorRelative(int slot, int cascade, const glm::dvec3& cameraWorld, double gridWorldSize);
    // texel snap（LimitlessSquare SnapDirectionalShadowCenterToTexelStable 移植）：
    // 视锥中心相对锚点在光 right/up 平面内 round 到 texel；forward（深度）方向不 snap
    static glm::vec3 SnapCenterToTexelStable(const glm::vec3& centerWorld,
                                             const glm::vec3& anchorRelative,
                                             const glm::vec3& right, const glm::vec3& up, const glm::vec3& forward,
                                             double texelWorldSize);
};
